#!/bin/sh
# Copies the files the runtime needs into <out>/android-root (real files, for an app bundle):
# the full Android 14 BOOTCLASSPATH at its device paths (+ bootclasspath.txt), framework-res,
# ICU + tzdata, and the test dex files.
#   scripts/stage-android-root.sh <out-dir>
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
pb="$root/prebuilts/out/android-14"
out="$1/android-root"
rm -rf "$out"
mkdir -p "$out/apex/com.android.i18n/etc/icu" "$out/apex/com.android.tzdata/etc" "$out/test" \
         "$out/system/framework"
while read -r dev; do
  [ -n "$dev" ] || continue
  mkdir -p "$out$(dirname "$dev")"
  cp "$pb/bootclasspath/$(basename "$dev")" "$out$dev"
done < "$pb/bootclasspath.txt"
cp "$pb/bootclasspath.txt" "$out/bootclasspath.txt"
cp "$pb/framework-res.apk" "$out/system/framework/"
cp "$pb/build.prop" "$out/system/build.prop"
mkdir -p "$out/system/etc" "$out/system/fonts"
cp "$pb/fonts.xml" "$out/system/etc/fonts.xml"
rsync -a --delete "$pb/fonts/" "$out/system/fonts/"
mkdir -p "$out/system/usr/keychars"
cp "$root/third_party/aosp/frameworks/base/data/keyboards/Virtual.kcm" "$out/system/usr/keychars/"
cp "$pb/apex-etc/com.android.i18n/icu/"*.dat "$out/apex/com.android.i18n/etc/icu/"
cp -R "$pb/apex-etc/com.android.tzdata/." "$out/apex/com.android.tzdata/etc/"
cp "$root/build/services/services.dex" "$out/system/framework/andropkg-services.dex"
for a in "$root"/build/apps/*.apk; do
  [ -f "$a" ] && cp "$a" "$out/test/"
done
for t in "$root"/build/tests/*/classes.dex; do
  cp "$t" "$out/test/$(basename "$(dirname "$t")").dex"
done
du -sh "$out"
