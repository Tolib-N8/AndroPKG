#!/bin/sh
# Converts an Android app into an iOS app: one APK (or a split package's directory), its native
# libraries converted and signed, its classes preverified, its name and launcher icon, and the
# andropkg runtime, in a signed .ipa. The app opens its APK directly (Info.plist AndroPkgAPK).
#   scripts/convert-ipa.sh <app.apk | split-dir> [-o out.ipa] [--bundle-id id] [--install]
# The bundle id defaults to the runner's (Config/Local.xcconfig): free (Personal Team) profiles
# allow only a few apps and app ids, so dev builds share one slot. An IPA for AltStore/SideStore,
# which re-sign it, can take the app's own (--bundle-id <package>).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
usage() { echo "usage: $0 <app.apk | split-dir> [-o out.ipa] [--bundle-id id] [--install]" >&2; exit 2; }
input="" out="" bid="" install=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out=$2; shift 2 ;;
    --bundle-id) bid=$2; shift 2 ;;
    --install) install=1; shift ;;
    -*) usage ;;
    *) [ -z "$input" ] || usage; input=$1; shift ;;
  esac
done
[ -n "$input" ] && [ -e "$input" ] || usage
[ -f "$root/Config/Local.xcconfig" ] || { echo "create Config/Local.xcconfig (see .example)"; exit 1; }
cd "$root"
log() { printf '== %s\n' "$*"; }

# 1. The app, where staging looks for it (build/apps-ext is not in the repository).
name=$(basename "${input%/}" .apk)
src=$(cd "$(dirname "$input")" && pwd)/$(basename "$input")
if [ -d "$src" ]; then
  [ -f "$src/base.apk" ] || { echo "$input: a split package needs base.apk"; exit 1; }
  dst="build/apps-ext/$name"; [ "$src" = "$root/$dst" ] || { rm -rf "$dst"; mkdir -p "$dst"; cp "$src"/*.apk "$dst/"; }
  base="$dst/base.apk"
else
  dst="build/apps-ext/$name.apk"; [ "$src" = "$root/$dst" ] || { mkdir -p build/apps-ext; cp "$src" "$dst"; }
  base="$dst"
fi
apk_rel="test/$name"; [ -d "$dst" ] || apk_rel="test/$name.apk"

# 2. Name and package, from the manifest.
aapt2="$HOME/Library/Android/sdk/build-tools/36.0.0/aapt2"
badging=$("$aapt2" dump badging "$base" 2>/dev/null || true)
pkg=$(printf '%s\n' "$badging" | sed -n "s/^package: name='\([^']*\)'.*/\1/p" | head -1)
label=$(printf '%s\n' "$badging" | sed -n "s/^application-label:'\(.*\)'$/\1/p" | head -1)
[ -n "$label" ] || label=$name
[ -n "$out" ] || out="build/ipa/$name.ipa"
log "$label ($pkg)"

# 3. Runtime, framework and services; classes verified ahead of time.
log "runtime"
[ -d build/runtime-ios ] || cmake -S runtime -B build/runtime-ios -G Ninja -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.4 -DCMAKE_OSX_SYSROOT=iphoneos
ninja -C build/runtime-ios >/dev/null
ninja -C build/runtime-macos >/dev/null   # (preverification and the icon run on the Mac)
scripts/build-java.sh >/dev/null
scripts/preverify.sh boot
scripts/preverify.sh "$dst"

# 4. The launcher icon, drawn by the framework (adaptive and vector icons too).
log "icon"
work="$root/build/ipa/$name"; rm -rf "$work"; mkdir -p "$work"
icon_set="$root/build/ios-stage/Assets.xcassets/AppIcon.appiconset"
rm -rf "$root/build/ios-stage/Assets.xcassets"; mkdir -p "$icon_set"
mkdir -p "build/android-root/data/app/$name"
ANDROPKG_RENDER_ICON="$icon_set/icon.png" ANDROPKG_BCP=full timeout -k 5 120 scripts/run-dalvikvm-mac.sh \
  -Xhidden-api-policy:disabled -cp build/services/services.dex dev.andropkg.system.AndroPkgSystem \
  "$root/$dst" "$root/build/android-root/data/app/$name" >"$work/icon.log" 2>&1 || true
if [ -s "$icon_set/icon.png" ]; then
  cat >"$icon_set/Contents.json" <<'JSON'
{ "images": [ { "idiom": "universal", "platform": "ios", "size": "1024x1024", "filename": "icon.png" } ],
  "info": { "author": "andropkg", "version": 1 } }
JSON
  printf '{ "info": { "author": "andropkg", "version": 1 } }\n' >"$root/build/ios-stage/Assets.xcassets/Contents.json"
  icon_settings="ASSETCATALOG_COMPILER_APPICON_NAME=AppIcon"
else
  echo "no icon (see $work/icon.log)"; rm -rf "$root/build/ios-stage/Assets.xcassets"; icon_settings=""
fi

# 5. Bundle contents: this app only.
log "staging"
ANDROPKG_STAGE_ONLY=$name scripts/stage-android-root.sh build/ios-stage >/dev/null
scripts/prepare-native-libs.sh "$root/$dst" "$root/build/ios-stage/android-root/native/$name" ios >/dev/null
plist="$work/Info.plist"
cp host-app/Info-extra.plist "$plist"
/usr/libexec/PlistBuddy -c "Add :AndroPkgAPK string $apk_rel" "$plist"

# 6. Build and sign.
log "building"
(cd host-app && xcodegen generate --quiet)
set -- PRODUCT_NAME="$name" INFOPLIST_FILE="$plist" INFOPLIST_KEY_CFBundleDisplayName="$label"
[ -z "$bid" ] || set -- "$@" PRODUCT_BUNDLE_IDENTIFIER="$bid"
[ -z "$icon_settings" ] || set -- "$@" "$icon_settings"
dest="generic/platform=iOS"
dev=${ANDROPKG_DEVICE:-$(xcrun devicectl list devices 2>/dev/null | LC_ALL=C awk '/physical/ && (/available/ || /connected/) {for(i=1;i<=NF;i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) {print $i; exit}}')}
[ -z "$install" ] || [ -n "$dev" ] || { echo "--install: no connected device"; exit 1; }
[ -z "$dev" ] || dest="id=$dev"
xcodebuild -project host-app/AndroPkgRunner.xcodeproj -scheme AndroPkgRunner -configuration Release \
  -destination "$dest" -derivedDataPath build/ipa-dd -allowProvisioningUpdates -quiet build "$@"
app="build/ipa-dd/Build/Products/Release-iphoneos/$name.app"
[ -d "$app" ] || { echo "build produced no $app"; exit 1; }

# 7. The .ipa.
rm -rf "$work/Payload"; mkdir -p "$work/Payload"; cp -R "$app" "$work/Payload/"
mkdir -p "$(dirname "$out")"; out=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
rm -f "$out"; (cd "$work" && zip -qry "$out" Payload)
log "$out ($(du -h "$out" | cut -f1))"
if [ -n "$install" ]; then
  xcrun devicectl device install app --device "$dev" "$app" >/dev/null && log "installed on the device"
fi
