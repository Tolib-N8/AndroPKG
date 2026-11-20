#!/bin/sh
# Builds the spike test library as a real Android arm64 ELF (no NDK needed: freestanding).
# Variants: plain RELA (--pack-dyn-relocs=none) and the clang default for android API>=28
# (--pack-dyn-relocs=android+relr: APS2-packed RELA + DT_ANDROID_RELR).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-"$here/../../build/spikes"}
mkdir -p "$out"
CC=${CC:-/opt/homebrew/opt/llvm/bin/clang}
FLAGS="--target=aarch64-linux-android29 -fPIC -O2 -fstack-protector-strong -ffreestanding -nostdlib -shared -fuse-ld=lld \
  -Wl,-z,max-page-size=16384 -Wl,--hash-style=both -Wl,-soname,libspike.so"
$CC $FLAGS -Wl,--pack-dyn-relocs=none -o "$out/libspike.so" "$here/spike.c"
$CC $FLAGS -o "$out/libspike_relr.so" "$here/spike.c"
echo "built $out/libspike.so $out/libspike_relr.so"
