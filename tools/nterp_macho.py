#!/usr/bin/env python3
"""Generates nterp's arm64 assembly (gen_mterp.py) and ports it to Mach-O.

    nterp_macho.py <aosp-root> <out.S>

On top of elf2macho_asm.py's rewrites: the per-opcode/helper names nterp defines with
NAME_START/NAME_END (debugger symbols only) become assembler-private labels, since Mach-O
allows no non-private labels between .cfi_startproc and .cfi_endproc.
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

aosp, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import elf2macho_asm  # noqa: E402

mterp = os.path.join(aosp, "art/runtime/interpreter/mterp")
with tempfile.TemporaryDirectory() as tmp:
    gen = os.path.join(tmp, "mterp_arm64ng.S")
    subprocess.check_call([sys.executable, os.path.join(mterp, "gen_mterp.py"), gen] +
                          sorted(glob.glob(os.path.join(mterp, "arm64ng/*.S"))))
    src = open(gen).read()
    # GNU as also splits macro arguments at blanks; one invocation relies on it.
    src = re.sub(r"(\bw\d+) (\.L\w)", r"\1, \2", src)
    open(gen, "w").write(src)
    ported = os.path.join(tmp, "ported.S")
    elf2macho_asm.port_file(gen, ported)
    text = open(ported).read()

names = set(re.findall(r"^\s*NAME_(?:START|END)\s+([A-Za-z_]\w*)", text, re.M))
names.discard("name")
# NAME_START/NAME_END themselves: a private label, no symbol attributes.
text = re.sub(r"\.macro NAME_START name\n(?:.*\n)*?\.endm",
              ".macro NAME_START name\n    .balign 16\n\\\\name:\n.endm", text, count=1)
text = re.sub(r"\.macro NAME_END name\n(?:.*\n)*?\.endm", ".macro NAME_END name\n.endm", text, count=1)
if names:
    pat = re.compile(r"(?<![\w$])(" + "|".join(sorted(map(re.escape, names), key=len, reverse=True)) + r")(?![\w$])")
    text = pat.sub(r"L\1", text)
# tbz/tbnz reach +-32 KB; with andropkg's 256-byte handlers (nterp.h) the slow paths after the
# 64 KB handler table are farther than that from the handlers they return to.
counter = [0]
def far_tb(m):
    counter[0] += 1
    inv = {"tbz": "tbnz", "tbnz": "tbz"}[m.group(2)]
    lbl = f"Lfar_tb{counter[0]}"
    return (f"{m.group(1)}{inv} {m.group(3)}, {m.group(4)}, {lbl}\n"
            f"{m.group(1)}b {m.group(5)}\n{lbl}:")
text = re.sub(r"^(\s*)(tbz|tbnz)\s+(\w+),\s*(#\w+),\s*(L[A-Za-z_]\w*)[ \t]*$", far_tb, text, flags=re.M)
# Labels a template builds as nterp_${opcode} are already substituted by gen_mterp.
text = text.replace(".alt_entry L", "// .alt_entry L")
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, "w").write(text)
