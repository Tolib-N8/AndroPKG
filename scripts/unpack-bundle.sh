#!/bin/sh
# Unpacks an app bundle download (.xapk, .apkm, .apks: base APK + config splits) into a split
# package directory andropkg runs like an APK (PackageParser parses the cluster):
#   scripts/unpack-bundle.sh <bundle> <out-dir>
# Kept: the base APK, the arm64-v8a ABI split, the xxhdpi (or only) density split and the
# language splits in $ANDROPKG_LOCALES (default "en ru"); other ABIs/densities are dropped.
set -eu
bundle=$1; out=$2
locales=${ANDROPKG_LOCALES:-en ru}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
unzip -q -o "$bundle" '*.apk' -d "$tmp"
rm -rf "$out"; mkdir -p "$out"
for a in "$tmp"/*.apk "$tmp"/*/*.apk; do
  [ -f "$a" ] || continue
  name=$(basename "$a" .apk)
  # The split's name (config.xxx / split_config.xxx), or none for the base APK.
  cfg=$(echo "$name" | sed -n 's/^\(split_\)\{0,1\}config\.\(.*\)$/\2/p')
  if [ -z "$cfg" ]; then cp "$a" "$out/base.apk"; continue; fi
  case "$cfg" in
    arm64_v8a) ;;
    armeabi*|x86*|mips*) continue ;;
    *dpi) [ "$cfg" = xxhdpi ] || continue ;;
    *) echo " $locales " | grep -q " $cfg " || continue ;;
  esac
  cp "$a" "$out/split_config.$cfg.apk"
done
[ -f "$out/base.apk" ] || { echo "unpack-bundle: no base APK in $bundle"; exit 1; }
echo "split package: $(ls "$out" | tr '\n' ' ')"
