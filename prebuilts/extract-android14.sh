#!/bin/sh
# Extracts the Java half of Android 14 (API 34) from the official arm64 emulator image:
# boot classpath jars (from APEXes + /system/framework), framework-res.apk, ICU data,
# tzdata, fonts. Output: prebuilts/out/android-14/  (gitignored; ~300 MB)
#
# Needs: curl, unzip, python3, Docker (for debugfs on ext4 images). ~3 GB temporary disk.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work="$here/../build/prebuilts"
out="$here/out/android-14"
img_zip=arm64-v8a-34_r04.zip
img_url=https://dl.google.com/android/repository/sys-img/android/$img_zip
img_sha1=${ANDROPKG_IMG_SHA1:-}  # optional pin

mkdir -p "$work/dl" "$out"
cd "$work"

if [ ! -f dl/arm64-v8a/system.img ]; then
  [ -f "dl/$img_zip" ] || curl -sSfL -o "dl/$img_zip" "$img_url"
  [ -z "$img_sha1" ] || echo "$img_sha1  dl/$img_zip" | shasum -c -
  (cd dl && unzip -o -q "$img_zip" arm64-v8a/system.img arm64-v8a/build.prop && rm "$img_zip")
fi
[ -f parts/system.img ] || python3 -I "$here/lpunpack.py" dl/arm64-v8a/system.img parts system

# Everything ext4-related runs in one container: dump /system pieces, then each APEX payload.
rm -rf raw apexes && mkdir -p raw apexes
docker run --rm -v "$work:/w" -v "$here:/tools:ro" debian:bookworm-slim sh -eu -c '
  apt-get -qq update >/dev/null && apt-get -qq install -y e2fsprogs unzip >/dev/null 2>&1
  for d in /system/apex /system/framework /system/etc/classpaths /system/fonts /system/etc/fonts.xml /system/usr/share/zoneinfo; do
    debugfs -R "rdump $d /w/raw" /w/parts/system.img 2>/dev/null
  done
  cd /w/apexes
  for a in /w/raw/apex/*.apex /w/raw/apex/*.capex; do
    n=$(basename "$a"); n=${n%.*}; mkdir -p "$n"; cd "$n"
    case "$a" in
      *.capex) unzip -o -q "$a" original_apex && unzip -o -q original_apex apex_payload.img apex_manifest.pb && rm original_apex ;;
      *)       unzip -o -q "$a" apex_payload.img apex_manifest.pb ;;
    esac
    mkdir -p fs
    for d in javalib etc; do debugfs -R "rdump /$d fs" apex_payload.img 2>/dev/null || true; done
    rm -f apex_payload.img; cd ..
  done
  chown -R '"$(id -u):$(id -g)"' /w/raw /w/apexes'

python3 -I "$here/collect-bootclasspath.py" "$work" "$out"
cp dl/arm64-v8a/build.prop "$out/build.prop"
"$here/make-cacerts.sh"
echo "done: $out"; du -sh "$out"/*
