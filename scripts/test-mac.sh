#!/bin/sh
# Runs the macOS regression set: plain-Java tests on ART, then APKs through andropkg's system
# services (lifecycle, first rendered frame). Exits non-zero on the first failure.
#   scripts/test-mac.sh            (expects build/runtime-macos built; builds Java parts)
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
"$root/scripts/build-java.sh" >/dev/null
J=${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}
sdk="$HOME/Library/Android/sdk/platforms/android-37.0/android.jar"
for t in tests/*/; do
  ls "$t"*.java >/dev/null 2>&1 || continue
  out="build/tests/$(basename "$t")"; mkdir -p "$out"
  "$J/bin/javac" --release 8 -nowarn -cp "$sdk" -d "$out" "$t"*.java 2>/dev/null
  JAVA_HOME=$J "$HOME/Library/Android/sdk/build-tools/36.0.0/d8" --min-api 34 --lib "$sdk" --output "$out" "$out"/*.class
done
log=$(mktemp -d)
fail() { echo "FAIL: $1 (log: $log/$2.log)"; exit 1; }
run() {  # run <name> <expected-regex> <dalvikvm args...>
  name=$1; want=$2; shift 2
  ANDROPKG_BCP=full timeout 300 scripts/run-dalvikvm-mac.sh "$@" >"$log/$name.log" 2>&1 || true
  grep -qE "$want" "$log/$name.log" || fail "$name" "$name"
  echo "ok   $name"
}
run hello "^done" -cp build/tests/hello/classes.dex Hello
run looper "looper done" -cp build/tests/looper/classes.dex LooperTest
run resources "resources ok" -cp build/tests/resources/classes.dex ResourcesTest
apk() { echo -Xhidden-api-policy:disabled -Dandropkg.test.autofinish=1 -cp build/services/services.dex \
  dev.andropkg.system.AndroPkgSystem "$root/build/apps/$1.apk" "$root/build/android-root/data/app/$1"; }
run lifecycle "\[lifecycle\] onDestroy" $(apk lifecycle)
frame="$log/hello-frame.png"
# The banner sits at 24..120 dp (63..315 px at 420 dpi): a tap there is a click.
ANDROPKG_TEST_TAP=540,190 ANDROPKG_FRAME_PNG=$frame run hello-apk "HelloTest.*banner tapped 1" $(apk hello)
[ -s "$frame" ] || fail "hello-apk frame" hello-apk
echo "ok   hello-apk frame ($frame)"
# Stock widgets + action bar + system bar insets; a swipe scrolls (and flings) the list.
ANDROPKG_TEST_SWIPE=540,2000,540,600 run widgets "WidgetsTest.*onCreate done" $(apk widgets)
grep -q "Fatal signal" "$log/widgets.log" && fail "widgets crashed" widgets
echo "ok   widgets swipe"
# Own Application + ContentProvider, startActivityForResult, back stack order.
run multi "\[multi\] Main.onDestroy" $(apk multi)
grep -q "picked=second note" "$log/multi.log" || fail "multi result" multi
grep -q "provider rows: first note;second note;" "$log/multi.log" || fail "multi provider" multi
echo "ok   multi result+provider"
# Host keyboard: a tap focuses the EditText (keyboard shown), typed text reaches it.
ANDROPKG_DEBUG_IME=1 ANDROPKG_TEST_TAP=400,680 ANDROPKG_TEST_TYPE="typed" \
  run ime "ime: keyboard shown" $(apk widgets)
# SQLite (CursorWindow), SharedPreferences, files, HTTP over loopback (okhttp + sockets).
run storage "\[storage\] done" $(apk storage)
for want in "sqlite 1 rows: alpha;гамма;" "prefs name=andropkg" "http 200 body=hello"; do
  grep -q "$want" "$log/storage.log" || fail "storage: $want" storage
done
echo "ok   storage checks"
