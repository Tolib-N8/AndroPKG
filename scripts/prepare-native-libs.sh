#!/bin/sh
# Extracts an APK's arm64 native libraries and converts each into a signed Mach-O wrapper
# (lib/arm64-v8a/libfoo.so -> <out>/libfoo.so + <out>/libfoo.so.dylib) for andropkg's NDK layer.
#   scripts/prepare-native-libs.sh <app.apk | split-package-dir> <out-dir> [macos|ios] [codesign-identity]
# A split package directory (scripts/unpack-bundle.sh): the libraries of all its APKs (the ABI
# split config.arm64_v8a.apk holds them).
# macOS: ad-hoc signature. iOS: the identity (default: the first Apple Development one).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
apk=$1; out=$2; platform=${3:-macos}; identity=${4:-}
conv="$root/converter/.build/release/andropkg"
[ -x "$conv" ] || (cd "$root/converter" && swift build -c release >/dev/null)
if [ -d "$apk" ]; then apks=$(ls "$apk"/*.apk); else apks=$apk; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for a in $apks; do
  unzip -l "$a" 'lib/arm64-v8a/*.so' >/dev/null 2>&1 && unzip -q -o "$a" 'lib/arm64-v8a/*.so' -d "$tmp"
done
ls "$tmp"/lib/arm64-v8a/*.so >/dev/null 2>&1 || exit 0  # no native code
mkdir -p "$out"
if [ "$platform" = ios ] && [ -z "$identity" ]; then
  identity=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development[^"]*\)".*/\1/p' | head -1)
fi
for so in "$tmp"/lib/arm64-v8a/*.so; do
  name=$(basename "$so")
  # System.mapLibraryName() says lib<x>.dylib on this runtime (its own JNI libraries are
  # dylibs); the app's lib<x>.so is found under that name too.
  ln -sf "$name" "$out/${name%.so}.dylib"
  # Skip if unchanged since the last conversion.
  if [ -f "$out/$name.dylib" ] && cmp -s "$so" "$out/$name"; then chmod 755 "$out/$name"; continue; fi
  cp "$so" "$out/$name"
  chmod 755 "$out/$name"  # as the package installer extracts them (apps exec some: helpers)
  if [ "$platform" = ios ]; then
    set -- --platform ios --install-name "@rpath/$name.dylib" --sign "$identity"
  else
    set -- --platform macos --install-name "@rpath/$name.dylib" --adhoc
  fi
  # A library that can't be converted (e.g. ELF linked for 4 KB pages) is left out with a warning:
  # loading it fails like a missing library, which apps often handle (optional codecs, extras).
  if ! "$conv" wrap-elf "$so" -o "$out/$name.dylib" "$@"; then
    echo "warning: $name not converted; the app won't be able to load it" >&2
    rm -f "$out/$name" "$out/$name.dylib" "$out/${name%.so}.dylib"
  fi
done
echo "native libs: $(ls "$out"/*.so.dylib 2>/dev/null | wc -l | tr -d ' ') converted into $out"
