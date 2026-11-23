#!/bin/sh
# Runs every third-party APK in build/apps-ext (gitignored: bring your own) on the Mac and
# reports, per app, whether it reached a frame and whether it crashed. Frames and logs go to
# build/corpus/<name>.{png,log}.
#   scripts/corpus-mac.sh [name...]     (default: all; ANDROPKG_CORPUS_SECONDS=10)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
out=build/corpus
mkdir -p "$out"
secs=${ANDROPKG_CORPUS_SECONDS:-10}
apps="$*"
if [ -z "$apps" ]; then
  for a in build/apps-ext/*.apk build/apps-ext/*/; do
    [ -e "$a" ] || continue
    apps="$apps $(basename "${a%/}" .apk)"
  done
fi
printf '%-14s %-8s %-7s %s\n' app frame crash note
for n in $apps; do
  apk=build/apps-ext/$n.apk
  [ -e "$apk" ] || apk=build/apps-ext/$n
  data=$root/build/android-root/data/app/$n
  mkdir -p "$data"
  scripts/prepare-native-libs.sh "$root/$apk" "$data/lib" macos >/dev/null 2>&1
  rm -f "$out/$n.png"
  ANDROPKG_FRAME_PNG="$root/$out/$n.png" ANDROPKG_TEST_SCRIPT="wait $((secs * 1000)); exit" ANDROPKG_BCP=full \
    timeout -k 5 $((secs + 60)) scripts/run-dalvikvm-mac.sh -Xhidden-api-policy:disabled -Dandropkg.test.autofinish=1 \
    -cp build/services/services.dex dev.andropkg.system.AndroPkgSystem "$root/$apk" "$data" >"$out/$n.log" 2>&1
  frame=no  # (the PNG is rewritten on every frame: the timeout can cut one short)
  grep -q "andropkg: frame [0-9]" "$out/$n.log" && frame=yes
  crash=no
  note=""
  if grep -qE "FATAL EXCEPTION|Fatal signal|Abort message" "$out/$n.log"; then
    crash=yes
    note=$(grep -m1 -A2 -E "FATAL EXCEPTION|Fatal signal" "$out/$n.log" | grep -oE "[A-Za-z.]+(Exception|Error)[^ ]*|signal [0-9]+ \([A-Z]+\)" | head -1)
  fi
  printf '%-14s %-8s %-7s %s\n' "$n" "$frame" "$crash" "$note"
done
