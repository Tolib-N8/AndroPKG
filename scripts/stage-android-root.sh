#!/bin/sh
# Copies the files the runtime needs at /apex/... into <out>/android-root (real files, for an
# app bundle). M1 set: ART module boot jars + ICU + tzdata, plus test dex files.
#   scripts/stage-android-root.sh <out-dir>
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
pb="$root/prebuilts/out/android-14"
out="$1/android-root"
rm -rf "$out"
mkdir -p "$out/apex/com.android.art/javalib" "$out/apex/com.android.i18n/javalib" \
         "$out/apex/com.android.i18n/etc/icu" "$out/apex/com.android.tzdata/etc" "$out/test"
for j in core-oj core-libart okhttp bouncycastle apache-xml; do
  cp "$pb/bootclasspath/$j.jar" "$out/apex/com.android.art/javalib/"
done
cp "$pb/bootclasspath/core-icu4j.jar" "$out/apex/com.android.i18n/javalib/"
cp "$pb/apex-etc/com.android.i18n/icu/"*.dat "$out/apex/com.android.i18n/etc/icu/"
cp -R "$pb/apex-etc/com.android.tzdata/." "$out/apex/com.android.tzdata/etc/"
cp "$root/build/tests/hello/classes.dex" "$out/test/hello.dex"
du -sh "$out"
