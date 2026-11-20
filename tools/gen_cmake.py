#!/usr/bin/env python3
"""Generates CMake for the andropkg runtime from AOSP Android.bp files (see bp.py).

usage: gen_cmake.py <aosp-root> <out.cmake> <config.py>

<config.py> defines TARGETS (list of dicts, see runtime/modules.py) and VARIANT kwargs.
For every target an OBJECT library is emitted; the top-level CMakeLists.txt links
those objects into dylibs. Generated sources (operator_out, asm_defines.h) become
add_custom_command rules.
"""
import os
import runpy
import sys

sys.path.insert(0, os.path.dirname(__file__))
from bp import Tree, Variant  # noqa: E402

# Flags that are meaningless or harmful for Apple clang / Mach-O.
DROP_FLAGS = {
    "-Werror", "-fvisibility=protected", "-ggdb3", "-Wl,--exclude-libs=libziparchive.a",
    "-fsanitize-address-use-after-return=never", "-Wframe-larger-than=1736",
}
DROP_PREFIXES = ("-Werror=", "-Wl,--", "-fsanitize", "-Wframe-larger-than", "-Xclang")


def keep_flag(f):
    if not isinstance(f, str):
        raise SystemExit(f"non-string flag {f!r} (unsupported Blueprint construct)")
    return f not in DROP_FLAGS and not f.startswith(DROP_PREFIXES)


class Gen:
    def __init__(self, root, cfg):
        self.root = root
        self.cfg = cfg
        bps = []
        for d, _, files in os.walk(root):
            if "/.git" in d or "/test" in d.replace(root, ""):
                continue
            bps += [os.path.relpath(os.path.join(d, f), root) for f in files if f.endswith(".bp")]
        self.tree = Tree(root, sorted(bps))
        self.variant = Variant(**cfg.get("VARIANT", {}))
        self.cache = {}
        self.out = []

    def mod(self, name):
        if name not in self.cache:
            try:
                self.cache[name] = self.tree.resolve(name, self.variant)
            except KeyError:
                self.cache[name] = None
        return self.cache[name]

    def exported_includes(self, name, seen):
        """Include dirs a dependency exports to its users (recursively through re-exports)."""
        if name in seen:
            return []
        seen.add(name)
        ext = self.cfg.get("EXTERNAL_INCLUDES", {})
        if name in ext:
            return list(ext[name])
        m = self.mod(name)
        if m is None:
            return []
        dirs = list(m.get("export_include_dirs", [])) + list(m.get("export_system_include_dirs", []))
        for key in ("export_header_lib_headers", "export_shared_lib_headers",
                    "export_static_lib_headers"):
            for dep in m.get(key, []):
                dirs += self.exported_includes(dep, seen)
        # whole_static_libs re-export implicitly in practice for our purposes
        for dep in m.get("whole_static_libs", []):
            dirs += self.exported_includes(dep, seen)
        return dirs

    def emit_target(self, t):
        name, m = t["name"], self.mod(t["bp"])
        if m is None:
            raise SystemExit(f"bp module {t['bp']} not found")
        srcs, gen_srcs = [], []
        bp_srcs = m.get("srcs", []) + [":" + g for g in m.get("generated_sources", [])]
        if "srcs" in t:  # andropkg picks the sources itself (module used for flags/includes only)
            bp_srcs = [os.path.join(self.root, x) for x in t["srcs"]]
        for s in bp_srcs:
            if s.startswith(":"):
                gen_srcs.append(s[1:])
            else:
                srcs.append(s)
        excl = {os.path.join(self.root, x) for x in t.get("exclude_srcs", [])}
        srcs = [s for s in srcs if s not in excl and not any(s.endswith(x) for x in t.get("exclude_suffixes", []))]
        srcs += [os.path.join(self.root, x) for x in t.get("extra_srcs", [])]
        asm = []
        if t.get("asm_port"):
            asm = [x for x in srcs if x.endswith(".S")]
            srcs = [x for x in srcs if not x.endswith(".S")]
        srcs += [os.path.abspath(x) for x in t.get("andropkg_srcs", [])]

        gen_files, gen_inc = [], []
        for g in gen_srcs + m.get("generated_headers", []):
            if g in t.get("skip_generated", []):
                continue
            files, inc = self.emit_generated(g, m, t)
            gen_files += files
            gen_inc += inc

        incs = list(t.get("andropkg_includes", []))
        incs += m.get("local_include_dirs", []) + m.get("export_include_dirs", [])
        if m.get("include_build_directory", True):
            incs.append(m["_dir"])  # Soong adds the module directory implicitly
        incs += [os.path.join(self.root, d) for d in m.get("include_dirs", [])]
        seen = set([t["bp"]])
        for key in ("header_libs", "shared_libs", "static_libs", "whole_static_libs"):
            for dep in m.get(key, []):
                incs += self.exported_includes(dep, seen)
        incs += [os.path.join(self.root, d) for d in t.get("extra_includes", [])]
        incs += gen_inc
        incs = list(dict.fromkeys(os.path.normpath(i) for i in incs))

        cflags = [f for f in m.get("cflags", []) + self.cfg.get("GLOBAL_CFLAGS", []) + t.get("cflags", []) if keep_flag(f)]
        cppflags = [f for f in self.cfg.get("GLOBAL_CPPFLAGS", []) + m.get("cppflags", []) if keep_flag(f)]
        std = {"c++2a": "gnu++20", "c++20": "gnu++20", "gnu++20": "gnu++20", "c++17": "gnu++17",
               "gnu++17": "gnu++17", "experimental": "gnu++2b"}.get(m.get("cpp_std", ""), None)
        if std:
            cppflags = [f for f in cppflags if not f.startswith("-std=")] + [f"-std={std}"]
        conly = [f for f in m.get("conlyflags", []) + self.cfg.get("GLOBAL_CONLYFLAGS", []) if keep_flag(f)]
        asflags = [f for f in m.get("asflags", []) + t.get("asflags", []) if keep_flag(f)]
        if t.get("art"):
            cflags += self.cfg.get("ART_CFLAGS", [])
            asflags += self.cfg.get("ART_ASFLAGS", [])
        remove = set(t.get("remove_cflags", []))
        cflags = [f for f in cflags if f not in remove]
        if m.get("rtti"):
            cppflags = [f for f in cppflags if f != "-fno-rtti"] + ["-frtti"]
        # Soong passes flags through a shell: "-include stdio.h" is two arguments. CMake would
        # de-duplicate a bare "-include", so keep such groups together with SHELL:.
        split = lambda fl: [f"SHELL:{f}" if " " in f.strip() else f for f in fl]
        cflags, cppflags, conly, asflags = split(cflags), split(cppflags), split(conly), split(asflags)

        o = self.out
        o.append(f"# ---- {name} (from {os.path.relpath(m['_dir'], self.root)}:{t['bp']})")
        o.append(f"add_library({name} OBJECT")
        o += [f"  {q(s)}" for s in srcs + gen_files]
        o.append(")")
        o.append(f"target_include_directories({name} PRIVATE")
        o += [f"  {q(i)}" for i in incs]
        o.append(")")
        o.append(f"target_compile_options({name} PRIVATE")
        o += [f"  {q(f)}" for f in cflags]
        o += [f"  $<$<COMPILE_LANGUAGE:CXX>:{q(f)}>" for f in cppflags]
        o += [f"  $<$<COMPILE_LANGUAGE:C>:{q(f)}>" for f in conly]
        o += [f"  $<$<COMPILE_LANGUAGE:ASM>:{q(f)}>" for f in asflags]
        o.append(")")
        o.append(f"set_target_properties({name} PROPERTIES POSITION_INDEPENDENT_CODE ON)")
        o.append(f"set(ANDROPKG_INCLUDES_{name} {' '.join(q(i) for i in incs)})")
        o.append(f"set(ANDROPKG_CXXFLAGS_{name} {' '.join(q(f) for f in cflags + cppflags)})")
        if asm:
            o.append(f"set(ANDROPKG_ASM_SRCS_{name} {' '.join(q(os.path.relpath(a, self.root)) for a in asm)})")
            o.append(f"set(ANDROPKG_ASM_PORT_DIRS_{name} {' '.join(t['asm_port'])})")
            o.append(f"set(ANDROPKG_ASMFLAGS_{name} {' '.join(q(f) for f in asflags + [x for x in cflags if x.startswith('-D')])})")
        o.append("")

    def emit_generated(self, g, owner, t):
        gm = self.mod(g)
        if gm is None:
            raise SystemExit(f"generated module {g} not found")
        gen_dir = "${ANDROPKG_GEN_DIR}/" + g
        if gm["_type"] == "gensrcs" and gm.get("tools") == ["generate_operator_out"]:
            tool = os.path.join(self.root, "art/tools/generate_operator_out.py")
            prefix = gm["cmd"].split(")", 1)[1].split()[0]  # "$(location tool) art/runtime ..."
            outs = []
            for h in gm["srcs"]:
                rel = os.path.relpath(h, os.path.join(self.root, prefix))
                out = f"{gen_dir}/{rel}.operator_out.cc"
                outs.append(out)
                self.out.append(
                    f"add_custom_command(OUTPUT {q(out)}"
                    f" COMMAND ${{Python3_EXECUTABLE}} ${{ANDROPKG_TOOLS}}/redirect.py {q(out)} {q(tool)} {prefix} {q(os.path.relpath(h, self.root))}"
                    f" DEPENDS {q(h)} {q(tool)} WORKING_DIRECTORY {q(self.root)} VERBATIM)")
            return outs, []
        if g == "cpp-define-generator-asm-support":
            # Compile asm_defines.cc to assembly with libart's flags, scrape constants.
            src = os.path.join(self.root, "art/tools/cpp-define-generator/asm_defines.cc")
            tool = os.path.join(self.root, "art/tools/cpp-define-generator/make_header.py")
            self.out.append(f"set(ANDROPKG_ASM_DEFINES_SRC {q(src)})")
            self.out.append(f"set(ANDROPKG_ASM_DEFINES_TOOL {q(tool)})")
            self.out.append(f"set(ANDROPKG_ASM_DEFINES_DIR {q(gen_dir)})")
            self.out.append(f"set(ANDROPKG_ASM_DEFINES_OWNER {t['name']})")
            return [f"{gen_dir}/asm_defines.h"], [gen_dir]
        raise SystemExit(f"don't know how to generate {g} ({gm['_type']}) for {t['name']}")

    def run(self, out_path):
        self.out.append("# Generated by tools/gen_cmake.py — do not edit.")
        for t in self.cfg["TARGETS"]:
            try:
                self.emit_target(t)
            except SystemExit as e:
                raise SystemExit(f"{t['name']}: {e}")
        with open(out_path, "w") as f:
            f.write("\n".join(self.out) + "\n")


def q(s):
    s = str(s)
    if s.startswith("SHELL:"):
        return '"' + s + '"'
    return '"' + s.replace('"', '\\"') + '"' if any(c in s for c in ' ;"()') else s


if __name__ == "__main__":
    root, out, cfg_path = sys.argv[1:4]
    cfg = runpy.run_path(cfg_path, init_globals={"AOSP": os.path.abspath(root),
                                                 "PLATFORM": os.environ.get("ANDROPKG_PLATFORM", "macos")})
    Gen(os.path.abspath(root), cfg).run(out)
