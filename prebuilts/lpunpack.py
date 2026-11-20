#!/usr/bin/env python3
"""Minimal extractor for Android emulator system.img: GPT disk -> 'super' (dynamic partitions,
liblp metadata) -> logical partitions (system, product, ...). Linear extents only.

usage: lpunpack.py <system.img> <out-dir> [partition ...]
"""
import os, struct, sys

SECTOR = 512

def gpt_partitions(f):
    f.seek(SECTOR)
    hdr = f.read(92)
    assert hdr[:8] == b"EFI PART", "not a GPT disk"
    entries_lba, n, size = struct.unpack_from("<QII", hdr, 72)
    f.seek(entries_lba * SECTOR)
    parts = []
    for _ in range(n):
        e = f.read(size)
        first, last = struct.unpack_from("<QQ", e, 32)
        name = e[56:128].decode("utf-16le").rstrip("\0")
        if first:
            parts.append((name, first * SECTOR, (last - first + 1) * SECTOR))
    return parts

def lp_partitions(f, base):
    f.seek(base + 4096)                       # LP_PARTITION_RESERVED_BYTES
    geo = f.read(52)
    magic, struct_size = struct.unpack_from("<II", geo, 0)
    assert magic == 0x616C4467, "no liblp geometry"
    max_size, slots, block = struct.unpack_from("<III", geo, 40)
    meta = base + 4096 + 2 * 4096             # primary metadata, slot 0
    f.seek(meta)
    h = f.read(256)
    assert struct.unpack_from("<I", h, 0)[0] == 0x414C5030, "no liblp metadata header"
    hsize = struct.unpack_from("<I", h, 8)[0]
    # tables descriptors: offset,num_entries,entry_size (x4: partitions, extents, groups, block_devices)
    desc = [struct.unpack_from("<III", h, 80 + 12 * i) for i in range(4)]
    tables = meta + hsize
    def table(i):
        off, num, esz = desc[i]
        f.seek(tables + off)
        return [f.read(esz) for _ in range(num)]
    extents = [struct.unpack_from("<QIQI", e) for e in table(1)]  # num_sectors, type, target_data, source
    out = []
    for p in table(0):
        name = p[:36].rstrip(b"\0").decode()
        attrs, first_ext, num_ext, group = struct.unpack_from("<IIII", p, 36)
        segs = []
        for nsec, etype, target, _src in extents[first_ext:first_ext + num_ext]:
            assert etype == 0, f"{name}: non-linear extent"
            segs.append((base + target * SECTOR, nsec * SECTOR))
        out.append((name, segs))
    return out

def main():
    img, outdir, *want = sys.argv[1:]
    os.makedirs(outdir, exist_ok=True)
    with open(img, "rb") as f:
        parts = gpt_partitions(f)
        print("GPT:", ", ".join(f"{n}({s // 2**20}M)" for n, _, s in parts))
        name, off, _ = next(p for p in parts if p[0] == "super")
        for lname, segs in lp_partitions(f, off):
            size = sum(s for _, s in segs)
            print(f"  {lname}: {size // 2**20} MiB in {len(segs)} extent(s)")
            if size == 0 or (want and lname not in want):
                continue
            with open(os.path.join(outdir, lname + ".img"), "wb") as o:
                for src, n in segs:
                    f.seek(src)
                    while n:
                        chunk = f.read(min(n, 1 << 24)); o.write(chunk); n -= len(chunk)

if __name__ == "__main__":
    main()
