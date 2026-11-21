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
done
ls -la "$out/services.dex" "$root"/build/apps/*.apk
