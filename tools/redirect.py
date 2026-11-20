#!/usr/bin/env python3
"""redirect.py <out-file> <program> [args...] — runs program, writes its stdout to out-file
(portable replacement for shell '>' inside CMake custom commands)."""
import os, subprocess, sys
out, cmd = sys.argv[1], sys.argv[2:]
if cmd[0].endswith(".py"):
    cmd = [sys.executable] + cmd
os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
data = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout
with open(out, "wb") as f:
    f.write(data)
