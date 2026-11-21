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
  # POSIX sh: "VAR=x run ..." assignments outlive the function call; reset them per test.
  unset ANDROPKG_TEST_TAP ANDROPKG_TEST_TYPE ANDROPKG_TEST_SWIPE ANDROPKG_TEST_BACK ANDROPKG_TEST_BACKGROUND \
        ANDROPKG_TEST_SCRIPT ANDROPKG_FRAME_PNG ANDROPKG_DEBUG_IME
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
# BACK (the iOS edge swipe) finishes the root activity when asked to (pre-Android-12 rule).
ANDROPKG_TEST_BACK=1 run back "last activity gone" -Dandropkg.back.finishesRoot=1 $(apk widgets)
# Windows: a centered dialog takes the tap on its button; a tap outside cancels the next one.
ANDROPKG_TEST_SCRIPT="wait 800; tap 540,1200; wait 1200; tap 30,2300" \
  run windows "\[windows\] second dialog cancelled by outside touch" $(apk windows)
grep -q "\[windows\] dialog button clicked" "$log/windows.log" || fail "windows dialog tap" windows
echo "ok   windows dialog tap"
# App components: services, broadcasts, pending intents, toasts, WebView.
run components "\[components\] bound service destroyed" $(apk components)
for want in "service started n=7 id=1" "service destroyed" "bound: pong" "dynamic receiver got dev.andropkg.test.PING" \
    "manifest receiver got dev.andropkg.test.PING" "manifest receiver got PI" "webview page finished"; do
  grep -q "\[components\] $want" "$log/components.log" || fail "components: $want" components
done
grep -q "toast: hello from components" "$log/components.log" || fail "components: toast" components
echo "ok   components checks"
# Platform services: connectivity, notification channels, asset fds, sensors, startActivities.
ANDROPKG_TEST_SCRIPT="wait 300; accel 1,2,9.8" run platform "\[platform\] done" $(apk platform)
for want in "network internet=true" "network callback available" "channel Channel one importance=2" \
    "asset fd length=25" "accelerometer Accelerometer" "accel 1,2,10" "second created n=1" "second new intent n=2"; do
  grep -q "\[platform\] $want" "$log/platform.log" || fail "platform: $want" platform
done
echo "ok   platform checks"
# Sound: MediaPlayer (raw resource, file + prepareAsync + seek, Ogg Opus), SoundPool (WAV + Ogg
# Vorbis), streaming/static AudioTrack, MediaMetadataRetriever, audio focus, MediaSession.
ANDROPKG_TEST_SCRIPT="wait 500; mediacmd pause" run media "\[media\] done" $(apk media)
for want in "focus=1 max=15 rate=48000" "raw duration=600" "raw completed" "file prepared" "file seek done at 300" \
    "retriever duration=600 mime=audio/wav audio=yes" \
    "file completed" "track played true timestamp=true" "static marker reached" \
    "session active controller=Tone" "session onPause" "soundpool loaded 2 status=0" "soundpool playing true true" \
    "opus duration=600" "opus completed at 600" "vorbis retriever duration=600 mime=audio/ogg"; do
  grep -q "\[media\] $want" "$log/media.log" || fail "media: $want" media
done
echo "ok   media checks"
# NDK: Android arm64 ELF libraries (C + C++ on libc++_shared) through the converter + NDK layer.
scripts/prepare-native-libs.sh build/apps/ndk.apk build/android-root/data/app/ndk/lib macos >/dev/null
run ndk "\[ndk\] done" $(apk ndk)
for want in "hello: n=42 d=2.50 hex=0x2a str=ok big=1099511627776" "callback: 36" "threads: 10000" \
    "files: stat=0 size=27 reg=1 entries>0=1 enoent=1" \
    "platform: asset=asset read by native code(25) density>0=1 looper=1 bitmap=4x3 fmt=1 pixel=ffff0000" "cpp: static-init caught=\[too big: 5\] threads=15 seen=5 sum=15" \
    "exec1: argc=2 arg1=hello stdin=from java counter=1 stderr=to stderr exit=3" \
    "exec2: argc=2 arg1=hello stdin=from java counter=1 stderr=to stderr exit=3"; do
  grep -q "\[ndk\] $want" "$log/ndk.log" || fail "ndk: $want" ndk
done
grep -q "unimplemented NDK function" "$log/ndk.log" && fail "ndk: unimplemented function called" ndk
echo "ok   ndk checks"
# NativeActivity (android_native_app_glue): ANativeWindow software frames (RGB_565, scaled), AInputQueue.
scripts/prepare-native-libs.sh build/apps/nativeact.apk build/android-root/data/app/nativeact/lib macos >/dev/null
ANDROPKG_TEST_TAP=200,300 run nativeact "\[nat\] exit" $(apk nativeact)
for want in "android_main sdk=34 internal=yes" "window 1080x2400 -> buffer 160x120" "first frame 160x120 fmt=4" \
    "touch 200,300 pointers=1" "destroy"; do
  grep -q "\[nat\] $want" "$log/nativeact.log" || fail "nativeact: $want" nativeact
done
grep -q "unimplemented NDK function" "$log/nativeact.log" && fail "nativeact: unimplemented function called" nativeact
echo "ok   nativeact checks"
# EGL + GLES 2.0 (macOS: OpenGL 2.1 stand-in): shaders, client arrays, framebuffer 0 readback, 60 Hz pacing.
scripts/prepare-native-libs.sh build/apps/gles.apk build/android-root/data/app/gles/lib macos >/dev/null
ANDROPKG_TEST_SCRIPT="wait 3000; tap 540,1200" run gles "\[gl\] exit" $(apk gles)
for want in "egl 1.4 configs=1 surface 1080x2400" "program linked=1" "center=ff8000 corner=0000ff error=0x0" \
    "fps=(5[5-9]|6[0-5])"; do
  grep -qE "\[gl\] $want" "$log/gles.log" || fail "gles: $want" gles
done
grep -q "unimplemented NDK function" "$log/gles.log" && fail "gles: unimplemented function called" gles
echo "ok   gles checks"
# SurfaceView: a child layer composed below the window (seen through the hole it punches),
# a view over it; GLSurfaceView: EGL10 + GLES20 from Java through the same layers.
ANDROPKG_TEST_SCRIPT="wait 2500; pixel 540,200; pixel 50,520; pixel 540,1500; pixel 1000,2300" \
  run surfaceview "test: script done" $(apk surfaceview)
for want in "540,200 = ffffff" "50,520 = 00ff00" "540,1500 = ff0000" "1000,2300 = ffff00"; do
  grep -q "test: pixel $want" "$log/surfaceview.log" || fail "surfaceview: pixel $want" surfaceview
done
echo "ok   surfaceview checks"
ANDROPKG_TEST_SCRIPT="wait 3000; pixel 540,1200; pixel 30,1200" run glsurface "test: script done" $(apk glsurface)
for want in "\[glsv\] center=ff00ff error=0" "\[glsv\] fps=(5[5-9]|6[0-5])" "test: pixel 540,1200 = ff00ff" \
    "test: pixel 30,1200 = 00ff00"; do
  grep -qE "$want" "$log/glsurface.log" || fail "glsurface: $want" glsurface
done
echo "ok   glsurface checks"
# Host lifecycle: iOS background/foreground pauses+stops and restarts+resumes the activity.
ANDROPKG_TEST_BACKGROUND=1 run background "HelloTest.*onRestart" $(apk hello)
