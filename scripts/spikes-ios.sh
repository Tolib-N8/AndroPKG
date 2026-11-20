#!/bin/sh
# Build the M0 spike app, install it on a connected iPhone, run it and stream the results.
#   scripts/spikes-ios.sh [device-udid]     (default: first connected physical device)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/spikes"
gen="$root/spikes/ios/Generated"
[ -f "$root/Config/Local.xcconfig" ] || { echo "create Config/Local.xcconfig (see .example)"; exit 1; }

"$root/spikes/testlib/build.sh" "$out" >/dev/null
(cd "$root/converter" && swift build -q)
mkdir -p "$gen"
for v in libspike libspike_relr; do
  "$root/converter/.build/debug/andropkg" wrap-elf "$out/$v.so" -o "$gen/$v.elf.dylib" \
    --platform ios --install-name "@rpath/$v.elf.dylib" >/dev/null
done

(cd "$root/spikes/ios" && xcodegen generate --quiet)
dev=${1:-$(xcrun devicectl list devices 2>/dev/null | awk '/physical/ && /available/ {for(i=1;i<=NF;i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) {print $i; exit}}')}
[ -n "$dev" ] || { echo "no connected device"; exit 1; }

xcodebuild -project "$root/spikes/ios/AndroPkgSpikes.xcodeproj" -scheme AndroPkgSpikes \
  -configuration Debug -destination "id=$dev" -derivedDataPath "$root/build/ios-dd" \
  -allowProvisioningUpdates -quiet build
app="$root/build/ios-dd/Build/Products/Debug-iphoneos/AndroPkgSpikes.app"
bundle=$(/usr/libexec/PlistBuddy -c 'Print CFBundleIdentifier' "$app/Info.plist")

xcrun devicectl device install app --device "$dev" "$app" >/dev/null
xcrun devicectl device process launch --device "$dev" --console --terminate-existing \
  --environment-variables '{"ANDROPKG_SPIKE_EXIT": "1"}' "$bundle"
