#!/bin/sh
# Extracts an APK's arm64 native libraries and converts each into a signed Mach-O wrapper
# (lib/arm64-v8a/libfoo.so -> <out>/libfoo.so + <out>/libfoo.so.dylib) for andropkg's NDK layer.
#   scripts/prepare-native-libs.sh <app.apk | split-package-dir> <out-dir> [macos|ios] [codesign-identity]
# A split package directory (scripts/unpack-bundle.sh): the libraries of all its APKs (the ABI
# split config.arm64_v8a.apk holds them).
# macOS: ad-hoc signature. iOS: the identity (default: the first Apple Development one).
# Signed by the converter itself (NativeLibraries / MachOSigner), not codesign: the on-device
# installer signs the same way, so this path is the one tested.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
apk=$1; out=$2; platform=${3:-macos}; identity=${4:-}
conv="$root/converter/.build/release/andropkg"
(cd "$root/converter" && swift build -c release >/dev/null)
if [ -d "$apk" ]; then apks=$(ls "$apk"/*.apk); else apks=$apk; fi
if [ "$platform" = ios ]; then
  if [ -z "$identity" ]; then
    identity=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development[^"]*\)".*/\1/p' | head -1)
  fi
  set -- --platform ios --identity "$identity"
else
  set -- --platform macos --adhoc
fi
# A library that can't be converted (e.g. ELF linked for 4 KB pages) is left out with a warning:
# loading it fails like a missing library, which apps often handle (optional codecs, extras).
# shellcheck disable=SC2086
"$conv" prepare-native $apks -o "$out" "$@"
