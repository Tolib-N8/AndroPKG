#!/bin/sh
# Builds the iOS runtime + runner app, installs it on a connected iPhone and runs Hello.dex.
#   scripts/runner-ios.sh [MainClass [dex-under-android-root]]   (default: Hello test/hello.dex)
#   scripts/runner-ios.sh --apk <name>        runs build/apps/<name>.apk (or build/apps-ext/<name>.apk,
#                                             third-party APKs, or build/apps-ext/<name>/, a split
#                                             package from scripts/unpack-bundle.sh) as an Android app
#   ANDROPKG_DEVICE=<udid> to pick a device
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$root/Config/Local.xcconfig" ] || { echo "create Config/Local.xcconfig (see .example)"; exit 1; }
[ -d "$root/build/runtime-ios" ] || cmake -S "$root/runtime" -B "$root/build/runtime-ios" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.4 -DCMAKE_OSX_SYSROOT=iphoneos
ninja -C "$root/build/runtime-ios" >/dev/null
J=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
mkdir -p "$root/build/tests/hello"
sdkjar="$HOME/Library/Android/sdk/platforms/android-37.0/android.jar"
for t in "$root"/tests/*/; do
  ls "$t"*.java >/dev/null 2>&1 || continue  # tests/apps/: APKs, built by build-java.sh
  name=$(basename "$t"); outd="$root/build/tests/$name"; mkdir -p "$outd"
  "$J/bin/javac" --release 8 -nowarn -cp "$sdkjar" -d "$outd" "$t"*.java 2>/dev/null
  JAVA_HOME=$J "$HOME/Library/Android/sdk/build-tools/36.0.0/d8" --min-api 34 --lib "$sdkjar" --output "$outd" "$outd"/*.class
done
"$root/scripts/build-java.sh" >/dev/null
"$root/scripts/stage-android-root.sh" "$root/build/ios-stage" >/dev/null
# Native code of every staged APK: converted + signed with the development identity, inside the
# app bundle (android-root/native/<apk>/), where the host points nativeLibraryDir.
for a in "$root/build/ios-stage/android-root/test/"*.apk "$root/build/ios-stage/android-root/test/"*/; do
  [ -e "$a" ] || continue
  n=$(basename "$a" .apk)
  "$root/scripts/prepare-native-libs.sh" "${a%/}" "$root/build/ios-stage/android-root/native/$n" ios >/dev/null
done
(cd "$root/host-app" && xcodegen generate --quiet)
if [ "${1:-}" = --apk ]; then
  # Tests finish on their own; ANDROPKG_INTERACTIVE=1 keeps the app on screen.
  apk="test/$2.apk"  # (a split package is a directory)
  [ -d "$root/build/ios-stage/android-root/test/$2" ] && apk="test/$2"
  envs="\"ANDROPKG_APK\": \"$apk\""
  [ -n "${ANDROPKG_INTERACTIVE:-}" ] || envs="$envs, \"ANDROPKG_AUTOFINISH\": \"1\""
  # Test hooks (ANDROPKG_TEST_TAP=x,y in window pixels, _TYPE, _SWIPE, _BACK, _BACKGROUND)
  # are passed through to the app.
  for v in $(env | sed -n 's/^\(ANDROPKG_TEST_[A-Z_]*\)=.*/\1/p'); do
    envs="$envs, \"$v\": \"$(printenv "$v")\""
  done
else
  main=${1:-Hello}
  dex=${2:-test/$(echo "$main" | tr 'A-Z' 'a-z' | sed 's/test$//').dex}
  envs="\"ANDROPKG_MAIN\": \"$main\", \"ANDROPKG_DEX\": \"$dex\""
fi
dev=${ANDROPKG_DEVICE:-$(xcrun devicectl list devices 2>/dev/null | LC_ALL=C awk '/physical/ && /available/ {for(i=1;i<=NF;i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) {print $i; exit}}')}
[ -n "$dev" ] || { echo "no connected device"; exit 1; }
xcodebuild -project "$root/host-app/AndroPkgRunner.xcodeproj" -scheme AndroPkgRunner -configuration Debug \
  -destination "id=$dev" -derivedDataPath "$root/build/ios-dd" -allowProvisioningUpdates -quiet build
app="$root/build/ios-dd/Build/Products/Debug-iphoneos/AndroPkgRunner.app"
xcrun devicectl device install app --device "$dev" "$app" >/dev/null
xcrun devicectl device process launch --device "$dev" --console --terminate-existing \
  --environment-variables "{\"ANDROPKG_EXIT\": \"1\", $envs}" "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")"
if [ "${1:-}" = --apk ]; then  # first window frame, for checking: build/ios-frame.png
  xcrun devicectl device copy from --device "$dev" --domain-type appDataContainer \
    --domain-identifier "$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")" \
    --source Library/Caches/frame.png --destination "$root/build/ios-frame.png" >/dev/null 2>&1 \
    && echo "first frame: build/ios-frame.png"
fi
