#!/bin/sh
# Runs a dex on the macOS build of the andropkg runtime (M1 dev loop).
#   scripts/run-dalvikvm-mac.sh [extra vm options...] -cp <dex|jar> <MainClass> [args...]
# Lays out an Android-like root (apex/system/data) from prebuilts/out/android-14 on first use.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
pb="$root/prebuilts/out/android-14"
fs="$root/build/android-root"
bin="$root/build/runtime-macos"
[ -d "$pb/bootclasspath" ] || { echo "run prebuilts/extract-android14.sh first"; exit 1; }

if [ ! -d "$fs" ]; then
  mkdir -p "$fs/apex/com.android.art/javalib" "$fs/apex/com.android.i18n/javalib" \
           "$fs/apex/com.android.conscrypt/javalib" "$fs/system/framework" "$fs/data/dalvik-cache"
  for a in "$pb"/apex-etc/*; do ln -sfn "$a" "$fs/apex/$(basename "$a")/etc" 2>/dev/null || { mkdir -p "$fs/apex/$(basename "$a")"; ln -sfn "$a" "$fs/apex/$(basename "$a")/etc"; }; done
  while read -r dev; do
    [ -n "$dev" ] || continue
    mkdir -p "$fs$(dirname "$dev")"
    ln -sfn "$pb/bootclasspath/$(basename "$dev")" "$fs$dev"
  done < "$pb/bootclasspath.txt"
  ln -sfn "$pb/framework-res.apk" "$fs/system/framework/framework-res.apk"
  ln -sfn "$pb/fonts" "$fs/system/fonts"
fi

# Boot classpath: ANDROPKG_BCP=full -> the whole Android 14 BOOTCLASSPATH (framework etc.);
# default -> the ART module + ICU only (plain Java programs).
bcp=""
if [ "${ANDROPKG_BCP:-}" = full ]; then
  jars=$(cat "$pb/bootclasspath.txt")
else
  jars="/apex/com.android.art/javalib/core-oj.jar /apex/com.android.art/javalib/core-libart.jar
        /apex/com.android.art/javalib/okhttp.jar /apex/com.android.art/javalib/bouncycastle.jar
        /apex/com.android.art/javalib/apache-xml.jar /apex/com.android.i18n/javalib/core-icu4j.jar"
fi
for j in $jars; do
  bcp="${bcp:+$bcp:}$fs$j"
done

export ANDROID_ROOT="$fs/system" ANDROID_ART_ROOT="$fs/apex/com.android.art" \
       ANDROID_I18N_ROOT="$fs/apex/com.android.i18n" ANDROID_TZDATA_ROOT="$fs/apex/com.android.tzdata" \
       ANDROID_DATA="$fs/data"
exec "$bin/dalvikvm" -Xbootclasspath:"$bcp" -Xbootclasspath-locations:"$bcp" \
  -Xusejit:false -Xint "$@"
