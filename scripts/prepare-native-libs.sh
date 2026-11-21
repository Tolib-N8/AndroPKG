#!/bin/sh
# Extracts an APK's arm64 native libraries and converts each into a signed Mach-O wrapper
# (lib/arm64-v8a/libfoo.so -> <out>/libfoo.so + <out>/libfoo.so.dylib) for andropkg's NDK layer.
#   scripts/prepare-native-libs.sh <app.apk> <out-dir> [macos|ios] [codesign-identity]
# macOS: ad-hoc signature. iOS: the identity (default: the first Apple Development one).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
apk=$1; out=$2; platform=${3:-macos}; identity=${4:-}
conv="$root/converter/.build/release/andropkg"
[ -x "$conv" ] || (cd "$root/converter" && swift build -c release >/dev/null)
unzip -l "$apk" 'lib/arm64-v8a/*.so' >/dev/null 2>&1 || exit 0  # no native code
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unzip -q -o "$apk" 'lib/arm64-v8a/*.so' -d "$tmp"
if [ "$platform" = ios ] && [ -z "$identity" ]; then
  identity=$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development[^"]*\)".*/\1/p' | head -1)
fi
for so in "$tmp"/lib/arm64-v8a/*.so; do
  name=$(basename "$so")
  # System.mapLibraryName() says lib<x>.dylib on this runtime (its own JNI libraries are
  # dylibs); the app's lib<x>.so is found under that name too.
  ln -sf "$name" "$out/${name%.so}.dylib"
  # Skip if unchanged since the last conversion.
  if [ -f "$out/$name.dylib" ] && cmp -s "$so" "$out/$name"; then continue; fi
  cp "$so" "$out/$name"
  if [ "$platform" = ios ]; then
    "$conv" wrap-elf "$so" -o "$out/$name.dylib" --platform ios --install-name "@rpath/$name.dylib" --sign "$identity"
  else
    "$conv" wrap-elf "$so" -o "$out/$name.dylib" --platform macos --install-name "@rpath/$name.dylib" --adhoc
  fi
done
echo "native libs: $(ls "$out"/*.so.dylib 2>/dev/null | wc -l | tr -d ' ') converted into $out"
