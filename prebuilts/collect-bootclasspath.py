#!/usr/bin/env python3
"""Builds the Android 14 BOOTCLASSPATH from the extracted image and copies the jars + data.

Ordering follows derive_classpath (packages/modules/SdkExtensions/derive_classpath):
ART APEX fragment, then /system, then the remaining APEX fragments sorted by name.

usage: collect-bootclasspath.py <work-dir> <out-dir>
"""
import os, shutil, sys

BOOTCLASSPATH = 1  # enum Classpath in classpaths.proto


def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7F) << s; s += 7
        if not c & 0x80:
            return v, i


def fields(b):
    i = 0
    while i < len(b):
        key, i = varint(b, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, i = varint(b, i)
        elif wt == 2:
            n, i = varint(b, i); v = b[i:i + n]; i += n
        else:
            raise ValueError(f"wire type {wt}")
        yield num, v


def jars(pb_path):
    out = []
    for num, jar in fields(open(pb_path, "rb").read()):
        if num != 1:
            continue
        f = dict(fields(jar))
        if f.get(2, 0) == BOOTCLASSPATH:
            out.append(f[1].decode())
    return out


def main():
    work, out = sys.argv[1:]
    raw, apexes = os.path.join(work, "raw"), os.path.join(work, "apexes")
    fragments = [("com.android.art", os.path.join(apexes, "com.android.art/fs/etc/classpaths/bootclasspath.pb")),
                 ("system", os.path.join(raw, "classpaths/bootclasspath.pb"))]
    for name in sorted(os.listdir(apexes)):
        p = os.path.join(apexes, name, "fs/etc/classpaths/bootclasspath.pb")
        if name != "com.android.art" and os.path.exists(p):
            fragments.append((name, p))

    bcp_dir = os.path.join(out, "bootclasspath")
    os.makedirs(bcp_dir, exist_ok=True)
    order = []
    for _, pb in fragments:
        for dev in jars(pb):
            if dev.startswith("/apex/"):
                _, _, apex, rest = dev.split("/", 3)
                local = os.path.join(apexes, apex, "fs", rest)
            else:
                local = os.path.join(raw, dev.split("/")[-2], dev.split("/")[-1])
            shutil.copy2(local, os.path.join(bcp_dir, os.path.basename(dev)))
            order.append(dev)
    with open(os.path.join(out, "bootclasspath.txt"), "w") as f:
        f.write("\n".join(order) + "\n")

    # Data the runtime needs at /system and /apex/*/etc.
    shutil.copy2(os.path.join(raw, "framework", "framework-res.apk"), out)
    for d in ("fonts", "zoneinfo"):
        shutil.copytree(os.path.join(raw, d), os.path.join(out, d), dirs_exist_ok=True)
    if os.path.exists(os.path.join(raw, "fonts.xml")):
        shutil.copy2(os.path.join(raw, "fonts.xml"), out)
    for name in sorted(os.listdir(apexes)):
        etc = os.path.join(apexes, name, "fs", "etc")
        if os.path.isdir(etc):
            shutil.copytree(etc, os.path.join(out, "apex-etc", name), dirs_exist_ok=True)

    print(f"BOOTCLASSPATH: {len(order)} jars")
    for d in order:
        print("  " + d)


if __name__ == "__main__":
    main()
