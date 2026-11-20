#!/usr/bin/env python3
"""Mini-Soong: parses Android.bp (Blueprint) files and resolves cc modules for one variant.

We do not run Soong; we only need, per module: sources, flags, include dirs and the
libraries it depends on — resolved for our variant (arm64, a "host-like" Darwin target).

    from bp import Tree
    t = Tree(aosp_root, ["art/runtime/Android.bp", ...])
    m = t.resolve("libart", variant)   # -> dict of merged properties, paths absolute
"""
import glob
import os
import re

# ---------------------------------------------------------------------------------------
# Parser


class ParseError(Exception):
    pass


_TOKEN = re.compile(r"""
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<str>"(?:[^"\\]|\\.)*"|`[^`]*`)
  | (?P<num>-?\d+)
  | (?P<ident>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<op>\+=|[{}\[\]():,=+])
""", re.S | re.X)


def tokenize(src, path):
    pos, out = 0, []
    while pos < len(src):
        m = _TOKEN.match(src, pos)
        if not m:
            line = src.count("\n", 0, pos) + 1
            raise ParseError(f"{path}:{line}: unexpected {src[pos:pos + 20]!r}")
        pos = m.end()
        kind = m.lastgroup
        if kind == "ws":
            continue
        text = m.group(kind)
        if kind == "str":
            text = bytes(text[1:-1], "utf-8").decode("unicode_escape") if text[0] == '"' else text[1:-1]
        out.append((kind, text))
    out.append(("eof", ""))
    return out


class Parser:
    def __init__(self, src, path, variables):
        self.toks, self.i, self.path, self.vars = tokenize(src, path), 0, path, variables

    def peek(self, k=0):
        return self.toks[self.i + k]

    def take(self, kind=None, text=None):
        t = self.toks[self.i]
        if (kind and t[0] != kind) or (text and t[1] != text):
            raise ParseError(f"{self.path}: expected {text or kind}, got {t}")
        self.i += 1
        return t

    def parse(self):
        modules = []
        while self.peek()[0] != "eof":
            kind, name = self.take("ident")
            nxt = self.peek()[1]
            if nxt in ("=", "+="):
                self.take()
                val = self.expr()
                self.vars[name] = add(self.vars[name], val) if nxt == "+=" else val
            elif nxt == "{":
                modules.append((name, self.map()))
            elif nxt == "(":   # e.g. soong_namespace() or other call-ish syntax: skip
                depth = 0
                while True:
                    t = self.take()[1]
                    depth += t == "("
                    depth -= t == ")"
                    if depth == 0:
                        break
            else:
                raise ParseError(f"{self.path}: unexpected {self.peek()} after {name}")
        return modules

    def map(self):
        self.take("op", "{")
        out = {}
        while self.peek()[1] != "}":
            key = self.take()[1]
            self.take("op", ":") if self.peek()[1] == ":" else self.take("op", "=")
            out[key] = self.expr()
            if self.peek()[1] == ",":
                self.take()
        self.take("op", "}")
        return out

    def expr(self):
        v = self.atom()
        while self.peek()[1] == "+":
            self.take()
            v = add(v, self.atom())
        return v

    def atom(self):
        kind, text = self.peek()
        if kind == "str":
            self.take(); return text
        if kind == "num":
            self.take(); return int(text)
        if text == "[":
            self.take()
            out = []
            while self.peek()[1] != "]":
                out.append(self.expr())
                if self.peek()[1] == ",":
                    self.take()
            self.take()
            return out
        if text == "{":
            return self.map()
        if kind == "ident":
            self.take()
            if text in ("true", "false"):
                return text == "true"
            if text not in self.vars:
                raise ParseError(f"{self.path}: unknown variable {text}")
            return self.vars[text]
        raise ParseError(f"{self.path}: unexpected token {kind} {text!r}")


def add(a, b):
    if isinstance(a, list):
        return a + b
    if isinstance(a, str):
        return a + b
    if isinstance(a, dict):
        return merge(a, b)
    return a + b


def merge(base, over):
    """Soong-style property merge: lists append, maps merge recursively, scalars override."""
    out = dict(base)
    for k, v in over.items():
        if k in out and isinstance(out[k], list) and isinstance(v, list):
            out[k] = out[k] + v
        elif k in out and isinstance(out[k], dict) and isinstance(v, dict):
            out[k] = merge(out[k], v)
        else:
            out[k] = v
    return out


# ---------------------------------------------------------------------------------------
# Variant resolution

# Properties that are paths relative to the module directory.
PATH_LIST_PROPS = ("srcs", "exclude_srcs", "local_include_dirs", "export_include_dirs",
                   "export_system_include_dirs")


class Variant:
    """Which arch/target branches apply. Our runtime is a Darwin "host-like" build:
    no bionic, no glibc, no Android platform services."""

    def __init__(self, arch="arm64", targets=("host", "darwin", "not_windows"), codegen=()):
        self.arch, self.targets, self.codegen = arch, set(targets), set(codegen)


class Tree:
    def __init__(self, root, bp_files):
        self.root = root
        self.modules = {}   # name -> (module_type, props, dir)
        for rel in bp_files:
            path = os.path.join(root, rel)
            variables = {}
            for mtype, props in Parser(open(path).read(), path, variables).parse():
                if "name" in props:
                    self.modules.setdefault(props["name"], (mtype, props, os.path.dirname(path)))

    def raw(self, name):
        if name not in self.modules:
            raise KeyError(f"module {name!r} not found in parsed Android.bp files")
        return self.modules[name]

    def _with_defaults(self, name, seen=()):
        mtype, props, d = self.raw(name)
        props = self._absolutize(props, d)
        merged = {}
        for dflt in props.get("defaults", []):
            if dflt in seen:
                continue
            try:
                merged = merge(merged, self._with_defaults(dflt, seen + (name,)))
            except KeyError:
                merged.setdefault("_missing_defaults", []).append(dflt)
        own = {k: v for k, v in props.items() if k != "defaults"}
        return merge(merged, own)

    def _absolutize(self, props, d):
        out = {}
        for k, v in props.items():
            if k in PATH_LIST_PROPS and isinstance(v, list):
                out[k] = [x if x.startswith(":") else os.path.normpath(os.path.join(d, x)) for x in v]
            elif isinstance(v, dict):
                out[k] = self._absolutize(v, d)
            else:
                out[k] = v
        return out

    def resolve(self, name, variant):
        p = self._with_defaults(name)
        p = self._flatten(p, variant)
        p["srcs"] = self._expand_globs(self._expand_filegroups(p.get("srcs", []), variant),
                                       p.get("exclude_srcs", []))
        p["_type"] = self.raw(name)[0]
        p["_dir"] = self.raw(name)[2]
        return p

    def _flatten(self, p, v):
        """Folds arch/target/codegen sub-blocks that apply to the variant into top level."""
        out = {k: x for k, x in p.items() if k not in ("arch", "target", "codegen", "multilib")}
        branches = []
        branches += [p.get("arch", {}).get(v.arch, {})]
        for t, b in p.get("target", {}).items():
            if t in v.targets or any(t == f"{x}_{v.arch}" for x in v.targets):
                branches.append(b)
        for c in v.codegen:
            branches.append(p.get("codegen", {}).get(c, {}))
        branches.append(p.get("multilib", {}).get("lib64", {}))
        for b in branches:
            if b:
                out = merge(out, self._flatten(b, v))
        return out

    def _expand_filegroups(self, srcs, variant):
        """Replaces ":name" references to filegroups with their sources. References to
        genrules/gensrcs stay as ":name" for the build generator to handle."""
        out = []
        for s in srcs:
            name = s[1:].split("{")[0] if s.startswith(":") else None
            if name and name in self.modules and self.modules[name][0] == "filegroup":
                fg = self._flatten(self._with_defaults(name), variant)
                out += self._expand_filegroups(
                    self._expand_globs(fg.get("srcs", []), fg.get("exclude_srcs", [])), variant)
            else:
                out.append(s)
        return out

    @staticmethod
    def _expand_globs(srcs, excludes):
        def expand(items):
            res = []
            for s in items:
                if not s.startswith(":") and any(c in s for c in "*?["):
                    res += sorted(glob.glob(s, recursive=True))
                else:
                    res.append(s)
            return res
        ex = set(expand(excludes))
        return [s for s in expand(srcs) if s not in ex]


if __name__ == "__main__":
    import json, sys
    root, bp, mod = sys.argv[1:4]
    t = Tree(root, bp.split(","))
    print(json.dumps(t.resolve(mod, Variant()), indent=1))
