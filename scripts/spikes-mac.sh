#!/bin/sh
# Build + run the M0 spikes on this Mac (Apple silicon): test ELF -> wrap -> load -> check.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/spikes"
"$root/spikes/testlib/build.sh" "$out" >/dev/null
(cd "$root/converter" && swift build -q)
conv="$root/converter/.build/debug/andropkg"
for v in libspike libspike_relr; do
  "$conv" wrap-elf "$out/$v.so" -o "$out/$v.mac.dylib" --platform macos --adhoc >/dev/null
done
clang -O2 -Wall -Wextra -Werror -std=c11 \
  -I"$root/loader/include" -I"$root/shim/include" -I"$root/spikes/common" \
  "$root/spikes/host-mac/main.c" "$root/spikes/common/spikes.c" \
  "$root/loader/src/elf_loader.c" "$root/shim/src/syscall.c" "$root/shim/src/tls.c" "$root/shim/src/svc_entry.S" \
  -o "$out/spikes-mac"
exec "$out/spikes-mac" rela "$out/libspike.mac.dylib" aps2+relr "$out/libspike_relr.mac.dylib"
