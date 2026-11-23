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
        ANDROPKG_TEST_SCRIPT ANDROPKG_FRAME_PNG ANDROPKG_DEBUG_IME ANDROPKG_LOG_MEMORY ANDROPKG_VDEX_DELAY_MS
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
for want in "sqlite 1 rows: alpha;гамма;" "prefs name=andropkg" "http 200 body=hello" "roundtrip=true ec=true"; do
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
    "asset fd length=25" "accelerometer Accelerometer" "accel 1,2,10" "second created n=1" "second new intent n=2" "hardware bitmap pixel=ff00ff00" "parcel bitmap 4x4 pixel=ff00ff00" "software renderNode ffff0000,ff0000ff" "renderNode layer effect ff00ff00" "nsd manager=true" "nsd discover=true"; do
  grep -q "\[platform\] $want" "$log/platform.log" || fail "platform: $want" platform
done
echo "ok   platform checks"
# Sound: MediaPlayer (raw resource, file + prepareAsync + seek, Ogg Opus), SoundPool (WAV + Ogg
# Vorbis), streaming/static AudioTrack, MediaMetadataRetriever, audio focus, MediaSession.
ANDROPKG_TEST_SCRIPT="wait 500; mediacmd pause" run media "\[media\] done" $(apk media)
for want in "focus=1 max=15 rate=48000" "raw duration=600" "raw completed" "file prepared" "file seek done at 300" \
    "retriever duration=600 mime=audio/wav audio=yes" \
    "file completed" "track played true timestamp=true clock=true" "static marker reached" \
    "codec audio/vorbis 22050x2 ok=true signal=true async audio/vorbis 22050x2 ok=true signal=true" \
    "codec audio/opus 48000x1 ok=true signal=true" "codec audio/mp4a-latm 22050x1 ok=true signal=true" \
    "codec audio/mpeg 22050x1 ok=true signal=true" "codec audio/raw 22050x1 ok=true signal=true" \
    "router selected=true router2 routes=1 system=true" "metrics session=true" "session active controller=Tone" "session onPause" "soundpool loaded 2 status=0" "soundpool playing true true" \
    "opus duration=600" "opus completed at 600" "vorbis retriever duration=600 mime=audio/ogg"; do
  grep -q "\[media\] $want" "$log/media.log" || fail "media: $want" media
done
echo "ok   media checks"
# NDK: Android arm64 ELF libraries (C + C++ on libc++_shared) through the converter + NDK layer.
scripts/prepare-native-libs.sh build/apps/ndk.apk build/android-root/data/app/ndk/lib macos >/dev/null
run ndk "\[ndk\] done" $(apk ndk)
for want in "hello: n=42 d=2.50 hex=0x2a str=ok big=1099511627776" "callback: 36" "threads: 10000" \
    "files: stat=0 size=27 reg=1 entries>0=1 enoent=1" \
    "direct: cap=16 sum=136 written=11223344 setjmp=42 count=2" \
    "stackargs: 1 2 3 4 5 6 7 8 9 10 -11 12 65 1.5 2.5" \
    "aaudio: open=0,0 start=0 callbacks>0=1 frames>0=1 state=STOPPED rate=48000 write=4800" \
    "opensl: results=00000 callbacks>0=1 played>0=1 volume=-300 recorder=12" \
    "net: opt=0 bind=0 connect=0 accept>0=1 peer=2 idle=0 events=3 recv=ping/4 eventfd=5 so_error=0 gai=0 127.0.0.1:443 fam=2" \
    "platform: asset=asset read by native code(25) density>0=1 looper=1 bitmap=4x3 fmt=1 pixel=ffff0000" "cpp: static-init caught=\[too big: 5\] threads=15 seen=5 sum=15" \
    "page4k: counter=8 table=6 bss=523776 names=alpha,beta fns=10,15 catch=Custom(42),logic_error(range)" \
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
# Video: MediaCodec (VideoToolbox) H.264 with B-frames and HEVC onto two SurfaceViews, H.264
# through a TextureView's SurfaceTexture, an MP4 (H.264 + AAC) demuxed by MediaExtractor, and
# an MP4 played by MediaPlayer in a VideoView (which keeps 4:3, at the left).
ANDROPKG_TEST_SCRIPT="wait 3500; pixel 540,300; pixel 540,690; pixel 540,1080; pixel 540,1850; pixel 200,2150" \
  run video "test: script done" $(apk video)
for want in "video/avc units=30 rendered=30 format=320x240" "video/hevc units=30 rendered=30 format=320x240" \
            "video/avc (texture) units=30 rendered=30 format=320x240" \
            "extractor tracks=video/avc audio/mp4a-latm size=320x240 csd=true samples=30 sync=2 rendered=30" \
            "videoview prepared 320x240 duration=2000" "videoview completed" \
            "codec c2.android.avc.decoder video/avc 1080p=true profiles=3 adaptive=true" \
            "codec c2.android.hevc.decoder video/hevc 1080p=true profiles=2 adaptive=true"; do
  grep -qF "[video] $want" "$log/video.log" || fail "video: $want" video
done
grep -qE "test: pixel 540,300 = 0[0-9a-f]f[0-9a-f]0[0-9a-f]" "$log/video.log" || fail "video: AVC frame not green" video
grep -qE "test: pixel 540,690 = f[0-9a-f]0[0-9a-f]0[0-9a-f]" "$log/video.log" || fail "video: HEVC frame not red" video
grep -qE "test: pixel 540,1080 = 0[0-9a-f]0[0-9a-f]f[0-9a-f]" "$log/video.log" || fail "video: TextureView frame not blue" video
grep -qE "test: pixel 540,1850 = f[0-9a-f]f[0-9a-f]0[0-9a-f]" "$log/video.log" || fail "video: MP4 frame not yellow" video
grep -qE "test: pixel 200,2150 = f[0-9a-f]0[0-9a-f]f[0-9a-f]" "$log/video.log" || fail "video: VideoView frame not magenta" video
echo "ok   video checks"
# Multi-touch: a two-finger pinch (POINTER_DOWN/UP, moves with both fingers, ScaleGestureDetector).
ANDROPKG_TEST_SCRIPT="wait 1500; pinch 540,1200,200,700; wait 500; exit" run touch \
  "TouchTest.*pointers=2 pointerDown=1 pointerUp=1 twoFingerMoves=true zoomed=true" $(apk touch)

# GPU memory: a window redrawn as fast as it can (no video) must not grow the Metal allocation
# (autoreleased Metal objects on ART threads once leaked a display-sized texture per frame).
ANDROPKG_LOG_MEMORY=1 ANDROPKG_TEST_SCRIPT="wait 6000; exit" run gpu-memory "test: script done" \
  -Dandropkg.video.soak=anim $(apk video)
first=$(grep -m1 -oE "device [0-9]+ MB" "$log/gpu-memory.log" | grep -oE "[0-9]+")
last=$(grep -oE "device [0-9]+ MB" "$log/gpu-memory.log" | tail -1 | grep -oE "[0-9]+")
[ -n "$first" ] && [ -n "$last" ] && [ $((last - first)) -lt 64 ] || fail "gpu-memory: Metal allocation grew ${first} -> ${last} MB" gpu-memory
echo "ok   gpu memory (${first} -> ${last} MB)"
# Verification cache: a launch records the classes it verified into vdex files (dalvik-cache);
# the next launch uses them. The APK is in a read-only directory, as in an iOS app bundle
# (OatFileAssistant then also looks for an oat file next to the cached vdex).
ro="$root/build/test-readonly"; [ -d "$ro" ] && chmod -R u+w "$ro"; rm -rf "$ro"; mkdir -p "$ro"
cp "$root/build/apps/hello.apk" "$ro/"; chmod a-w "$ro"
rm -f "$root"/build/android-root/data/dalvik-cache/arm64/*test-readonly*
roapk() { echo -Xhidden-api-policy:disabled -cp build/services/services.dex dev.andropkg.system.AndroPkgSystem \
  "$ro/hello.apk" "$root/build/android-root/data/app/hello"; }
ANDROPKG_VDEX_DELAY_MS=200 ANDROPKG_TEST_SCRIPT="wait 4000; exit" run vdex-write \
  "verification cache .*test-readonly@hello.apk@classes.vdex" $(roapk)
ANDROPKG_TEST_SCRIPT="exit" run vdex-read "test: script done" -verbose:oat $(roapk)
grep -qE "Successfully loaded .*test-readonly@hello.apk@classes.vdex" "$log/vdex-read.log" || fail "vdex-read: cache not used" vdex-read
echo "ok   verification cache used"
ANDROPKG_TEST_SCRIPT="wait 3000; pixel 540,1200; pixel 30,1200" run glsurface "test: script done" $(apk glsurface)
for want in "\[glsv\] center=ff00ff error=0" "\[glsv\] fps=(5[5-9]|6[0-5])" "test: pixel 540,1200 = ff00ff" \
    "test: pixel 30,1200 = 00ff00"; do
  grep -qE "$want" "$log/glsurface.log" || fail "glsurface: $want" glsurface
done
echo "ok   glsurface checks"
# Host lifecycle: iOS background/foreground pauses+stops and restarts+resumes the activity.
ANDROPKG_TEST_BACKGROUND=1 run background "HelloTest.*onRestart" $(apk hello)
