#!/bin/sh
# Builds andropkg's Java parts and test APKs (needs JDK 17+ and Android SDK build-tools 36).
#   build/services/services.dex          andropkg in-process system services
#   build/apps/<name>.apk                tests/apps/<name>
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
J=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
BT=${ANDROID_BUILD_TOOLS:-$HOME/Library/Android/sdk/build-tools/36.0.0}
SDK=${ANDROID_PLATFORM_JAR:-$HOME/Library/Android/sdk/platforms/android-37.0/android.jar}
d8() { JAVA_HOME=$J "$BT/d8" --min-api 34 --lib "$SDK" "$@"; }
javac() { "$J/bin/javac" --release 8 -nowarn -Xlint:-options "$@"; }

out="$root/build/services"; rm -rf "$out"; mkdir -p "$out/classes"
javac -cp "$SDK" -d "$out/classes" $(find "$root/services/src" -name '*.java')
d8 --output "$out" $(find "$out/classes" -name '*.class')
mv "$out/classes.dex" "$out/services.dex"

NDK=${ANDROID_NDK:-$(ls -d "$HOME"/Library/Android/sdk/ndk/*/ 2>/dev/null | sort | tail -1)}
NDK=${NDK%/}
NDK_BIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
NDK_SYSROOT="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/sysroot"
for app in "$root"/tests/apps/*/; do
  name=$(basename "$app"); w="$root/build/apps/$name"; rm -rf "$w"; mkdir -p "$w/classes" "$w/res"
  "$BT/aapt2" compile --dir "$app/res" -o "$w/res.zip"
  assets=""; [ -d "$app/assets" ] && assets="-A $app/assets -0 bin"  # .bin stored: openFd-able
  "$BT/aapt2" link -I "$SDK" --manifest "$app/AndroidManifest.xml" -o "$w/base.apk" "$w/res.zip" \
    --java "$w/gen" --min-sdk-version 24 --target-sdk-version 34 $assets
  javac -cp "$SDK" -d "$w/classes" $(find "$app/src" "$w/gen" -name '*.java')
  d8 --output "$w" $(find "$w/classes" -name '*.class')
  cp "$w/base.apk" "$root/build/apps/$name.apk"
  (cd "$w" && zip -q -j "$root/build/apps/$name.apk" classes.dex)
  # jni/<lib>/*.c|*.cpp -> lib/arm64-v8a/lib<lib>.so (real NDK toolchain, 16 KB pages).
  if [ -d "$app/jni" ]; then
    [ -n "$NDK" ] || { echo "build-java: $name needs the Android NDK (set ANDROID_NDK)"; exit 1; }
    mkdir -p "$w/lib/arm64-v8a"
    for lib in "$app"/jni/*/; do
      lname=$(basename "$lib")
      cxx=$(ls "$lib"*.cpp 2>/dev/null | head -1)
      cc="$NDK_BIN/aarch64-linux-android24-clang"; [ -n "$cxx" ] && cc="$NDK_BIN/aarch64-linux-android24-clang++"
      "$cc" -shared -fPIC -O2 -fstack-protector-strong -Wl,-z,max-page-size=16384 -Wl,-soname,lib$lname.so \
        -o "$w/lib/arm64-v8a/lib$lname.so" $(ls "$lib"*.c "$lib"*.cpp 2>/dev/null) -llog
      [ -n "$cxx" ] && cp "$NDK_SYSROOT/usr/lib/aarch64-linux-android/libc++_shared.so" "$w/lib/arm64-v8a/"
    done
    (cd "$w" && zip -q -r "$root/build/apps/$name.apk" lib)
  fi
done
ls -la "$out/services.dex" "$root"/build/apps/*.apk
