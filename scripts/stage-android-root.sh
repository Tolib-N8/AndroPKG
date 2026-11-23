#!/bin/sh
# Copies the files the runtime needs into <out>/android-root (real files, for an app bundle):
# the full Android 14 BOOTCLASSPATH at its device paths (+ bootclasspath.txt), framework-res,
# ICU + tzdata, CA roots, and the test dex files.
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
  # Preverified classes (scripts/preverify.sh boot): <dir>/oat/arm64/<jar>.vdex
  v="$root/build/android-root$(dirname "$dev")/oat/arm64/$(basename "$dev" .jar).vdex"
  if [ -n "${ANDROPKG_PREVERIFY_BUILD:-}" ] && [ -f "$v" ]; then
    mkdir -p "$out$(dirname "$dev")/oat/arm64"; cp "$v" "$out$(dirname "$dev")/oat/arm64/"
  fi
done < "$pb/bootclasspath.txt"
cp "$pb/bootclasspath.txt" "$out/bootclasspath.txt"
cp "$pb/framework-res.apk" "$out/system/framework/"
cp "$pb/build.prop" "$out/system/build.prop"
mkdir -p "$out/system/etc" "$out/system/fonts"
cp "$pb/fonts.xml" "$out/system/etc/fonts.xml"
rsync -a --delete "$pb/fonts/" "$out/system/fonts/"
mkdir -p "$out/system/usr/keychars" "$out/data"
cp "$root/third_party/aosp/frameworks/base/data/keyboards/Virtual.kcm" "$out/system/usr/keychars/"
cp "$pb/apex-etc/com.android.i18n/icu/"*.dat "$out/apex/com.android.i18n/etc/icu/"
cp -R "$pb/apex-etc/com.android.tzdata/." "$out/apex/com.android.tzdata/etc/"
[ -d "$pb/cacerts" ] || "$root/prebuilts/make-cacerts.sh" >/dev/null
mkdir -p "$out/apex/com.android.conscrypt"
cp -R "$pb/cacerts" "$out/apex/com.android.conscrypt/cacerts"
cp "$root/build/services/services.dex" "$out/system/framework/andropkg-services.dex"
for a in "$root"/build/apps/*.apk "$root"/build/apps-ext/*.apk; do
  [ -f "$a" ] || continue
  cp "$a" "$out/test/"
  v="$(dirname "$a")/oat/arm64/$(basename "$a" .apk).vdex"  # (scripts/preverify.sh <apk>)
  if [ -n "${ANDROPKG_PREVERIFY_BUILD:-}" ] && [ -f "$v" ]; then mkdir -p "$out/test/oat/arm64"; cp "$v" "$out/test/oat/arm64/"; fi
done
# Split packages (scripts/unpack-bundle.sh): directories with a base.apk.
for d in "$root"/build/apps-ext/*/; do
  [ -f "$d/base.apk" ] || continue
  n=$(basename "$d"); rm -rf "$out/test/$n"; mkdir -p "$out/test/$n"; cp "$d"*.apk "$out/test/$n/"
  if [ -n "${ANDROPKG_PREVERIFY_BUILD:-}" ] && [ -d "$d/oat" ]; then cp -R "$d/oat" "$out/test/$n/"; fi
done
for t in "$root"/build/tests/*/classes.dex; do
  cp "$t" "$out/test/$(basename "$(dirname "$t")").dex"
done
du -sh "$out"
