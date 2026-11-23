#!/bin/sh
# Verifies every class ahead of time, on the Mac, and writes the vdex files the runtime uses
# instead of verifying classes on each launch (ART's verifier at install time, without dex2oat):
#   scripts/preverify.sh boot          the boot class path's jars, in build/android-root
#                                      (<dir>/oat/arm64/<jar>.vdex; skipped when up to date)
#   scripts/preverify.sh <apk|dir>     an APK, or a split package's base.apk
#                                      (<dir>/oat/arm64/<name>.vdex)
# scripts/stage-android-root.sh copies them into the app bundle. A vdex whose dex checksums no
# longer match is ignored at runtime.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
run() {
  ANDROPKG_BCP=full timeout -k 5 1200 scripts/run-dalvikvm-mac.sh -Xhidden-api-policy:disabled \
    -cp build/services/services.dex dev.andropkg.system.AndroPkgSystem "$@"
}
data="$root/build/android-root/data/app/andropkg-preverify"
mkdir -p "$data"
if [ "$1" = boot ]; then
  stamp=build/android-root/system/framework/oat/arm64/framework.vdex
  if [ -f "$stamp" ] && [ -z "$(find prebuilts/out/android-14/bootclasspath -newer "$stamp" -name '*.jar' | head -1)" ]; then
    exit 0
  fi
  find build/android-root -path '*/oat/arm64/*.vdex' -not -path '*/data/*' -delete
  ANDROPKG_PREVERIFY=boot run "$root/build/apps/hello.apk" "$data" >build/preverify-boot.log 2>&1 || true
  grep -q "preverification done" build/preverify-boot.log || { echo "preverify boot failed: build/preverify-boot.log"; exit 1; }
  echo "preverified: $(find build/android-root -path '*/oat/arm64/*.vdex' -not -path '*/data/*' | wc -l | tr -d ' ') boot jars"
  exit 0
fi
target=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
apk=$target
[ -d "$target" ] && apk="$target/base.apk"
vdex="$(dirname "$apk")/oat/arm64/$(basename "$apk" .apk).vdex"
[ -f "$vdex" ] && [ "$vdex" -nt "$apk" ] && exit 0
rm -f "$vdex"
log="build/preverify-$(basename "${target%/}" .apk).log"
ANDROPKG_PREVERIFY=app ANDROPKG_PREVERIFY_APK="$apk" run "$target" "$data" >"$log" 2>&1 || true
[ -f "$vdex" ] || { echo "preverify $1 failed: $log"; exit 1; }
echo "preverified: $(grep -oE '\([0-9]+ classes\)' "$log" | tail -1) $(basename "${target%/}")"
