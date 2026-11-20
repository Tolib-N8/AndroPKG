#!/usr/bin/env python3
"""Ports ART's GNU-as/ELF AArch64 assembly sources to Apple's Mach-O assembler dialect.

    elf2macho_asm.py <aosp-root> <out-root> <rel-dir-or-file>...

Every .S/.h under the given paths is rewritten into <out-root>/<same relative path>, so
quoted #includes resolve to the ported copies. Symbol naming (Mach-O's leading '_') is NOT
handled here: asm_symbols.py derives a `#define sym _sym` header from the assembled objects.

Rewrites:
  .type/.size                      -> dropped (no Mach-O equivalent; not needed)
  .hidden sym                      -> .private_extern sym
  .extern sym                      -> dropped (undefined symbols are external on Mach-O)
  .Llabel                          -> Llabel (Mach-O assembler-local prefix)
  :lo12:sym / #:lo12:sym           -> sym@PAGEOFF
  :pg_hi21_nc:sym, adrp xN, sym    -> sym@PAGE
  .section .text/.rodata/...       -> Mach-O sections
  global labels inside a function  -> preceded by .alt_entry (Mach-O alternate entry point)
"""
import os
import re
import sys

SECTION_MAP = {
    ".text": ".text",
    ".rodata": ".section __TEXT,__const",
    ".data": ".data",
    ".bss": ".bss",
    ".data.rel.ro": ".section __DATA,__const",
}

RE_LOCAL = re.compile(r"(?<![\w.$\\])\.L(?=[\w\\$])")
RE_LO12 = re.compile(r"#?:lo12:([\w\\$@()]+)")
RE_PGHI = re.compile(r":pg_hi21(?:_nc)?:([\w\\$()]+)")
RE_ADRP = re.compile(r"^(\s*adrp\s+[\w\\]+\s*,\s*)([A-Za-z_\\][\w\\$()]*)\s*$")
RE_LABEL = re.compile(r"^\s*([A-Za-z_][\w$]*):")


def port_line(line, state):
    code, sep, comment = line.partition("//")
    s = code.strip()
    if re.match(r"\.(type|size)\b", s):
        return ""
    if re.match(r"\.extern\b", s):
        return ""
    m = re.match(r"(\s*)\.hidden\s+(\S+)", code)
    if m:
        return f"{m.group(1)}.private_extern {m.group(2)}\n"
    m = re.match(r"(\s*)\.section\s+([\w.]+)", code)
    if m:
        name = m.group(2)
        base = next((v for k, v in SECTION_MAP.items() if name == k or name.startswith(k + ".")), None)
        if base is None:
            raise SystemExit(f"unmapped section {name!r}: {line.strip()}")
        return f"{m.group(1)}{base}\n"

    code = RE_LOCAL.sub("L", code)
    code = RE_PGHI.sub(r"\1@PAGE", code)
    code = RE_LO12.sub(r"\1@PAGEOFF", code)
    m = RE_ADRP.match(code.rstrip("\n"))
    if m and "@PAGE" not in code:
        code = f"{m.group(1)}{m.group(2)}@PAGE\n" if code.endswith("\n") else f"{m.group(1)}{m.group(2)}@PAGE"

    # Track .cfi_startproc/.cfi_endproc to flag global labels inside a function body.
    if ".cfi_startproc" in s or re.match(r"ENTRY(_ALIGNED)?\b", s):
        state["in_proc"] = True
    elif ".cfi_endproc" in s or re.match(r"END\b", s):
        state["in_proc"] = False
    else:
        lm = RE_LABEL.match(code)
        if lm and state["in_proc"] and not s.startswith(".macro"):
            code = f".alt_entry {lm.group(1)}\n" + code

    out = code + (sep + comment if sep else "")
    return out if out.endswith("\n") or not line.endswith("\n") else out + "\n"


def port_file(src, dst):
    state = {"in_proc": False}
    with open(src) as f:
        lines = f.readlines()
    out = []
    in_macro = 0
    for line in lines:
        st = line.strip()
        if st.startswith(".macro"):
            in_macro += 1
        if st.startswith(".endm"):
            in_macro -= 1
        # Inside macro bodies cfi state is per-expansion; don't inject alt_entry there.
        saved = dict(state)
        new = port_line(line, state)
        if in_macro:
            new = new.replace(".alt_entry ", "// .alt_entry ", 1) if new.startswith(".alt_entry") else new
            state = saved if ".cfi_" not in st else state
        out.append(new)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    data = "".join(out)
    if not os.path.exists(dst) or open(dst).read() != data:
        with open(dst, "w") as f:
            f.write(data)


def main():
    root, out_root, *paths = sys.argv[1:]
    for p in paths:
        full = os.path.join(root, p)
        files = [full] if os.path.isfile(full) else [
            os.path.join(d, f) for d, _, fs in os.walk(full) for f in fs if f.endswith((".S", ".h"))]
        for f in files:
            port_file(f, os.path.join(out_root, os.path.relpath(f, root)))
    stamp = os.path.join(out_root, ".stamp")
    open(stamp, "w").close()


if __name__ == "__main__":
    main()
