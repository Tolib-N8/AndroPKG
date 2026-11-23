# Roadmap

andropkg runs Android 14 apps on iOS by porting the Android runtime and framework, not by
virtualizing Android. The milestones below track that work, from the first feasibility spikes
to running real apps quickly.

**Legend:** ✅ done · 🔄 in progress · ⏳ planned

---

## ✅ M0: Feasibility spikes (iPhone, no JIT)

These questions decided whether the approach could work at all.

- ✅ ELF code from an Android `.so` runs after being wrapped into a signed Mach-O and loaded with
  `dlopen`.
- ✅ Relocations work: RELA, Android's packed APS2 and RELR, with `init_array`.
- ✅ `svc #0` is rewritten into syscall trampolines (about 22 ns per call, all registers kept).
- ✅ Thread-local storage works. XNU owns `TPIDR_EL0`, so `mrs tpidr_el0` is rewritten into a
  load from a Darwin TSD slot.
- ✅ A SIGSEGV handler with PC redirection works, which ART's implicit null checks need.

## ✅ M1: ART and libcore on iOS

- ✅ Android 14 ART builds for iOS without Soong (`Android.bp` → CMake).
- ✅ **Base-relative compressed references.** iOS has no memory below 4 GB, so ART uses a 4 GB
  reference arena.
- ✅ ART's ELF assembly is ported to Mach-O at build time.
- ✅ A Linux compatibility layer: futex, epoll on kqueue, 16 KB pages, signals.
- ✅ The concurrent copying GC with read barriers, ICU, tzdata and BoringSSL/Conscrypt.

## ✅ M2: The framework in one process

- ✅ `framework.jar` and `framework-res` load from Android 14's own image.
- ✅ In-process system services replace binder IPC: activities, windows, packages, input,
  display, audio, connectivity, notifications, accounts, storage and more.
- ✅ `ActivityThread` starts the app, and activities render through hwui.

## ✅ M3: Interactive apps

- ✅ Touch input with batched move history (multi-touch since), and the iOS keyboard as the IME.
- ✅ Activity lifecycle with results, translucent activities, recreation, and back gestures.
- ✅ Resources and configuration (density, dark theme), SharedPreferences, SQLite and networking.
- ✅ Android paths are mapped into the iOS sandbox.
- ✅ An AndroidKeyStore (software provider) and AccountManager.

## ✅ M4: Native code (NDK)

- ✅ Unmodified NDK libraries run: libc, libm, libdl, liblog, libandroid, libz, jnigraphics.
- ✅ Libraries built for 4 KB pages are relaid out for 16 KB pages. Code addressing is fixed up
  (ADRP, ADR, literal loads) along with unwind tables.
- ✅ EGL and GLES 2/3 for NativeActivity and `GLSurfaceView`.
- ✅ AAudio and OpenSL ES on Core Audio.

## ✅ M5: Media, web and delivery

- ✅ `MediaCodec` audio decoders (AAC, MP3, Opus, Vorbis), `MediaExtractor` and `MediaPlayer`.
  ExoPlayer/Media3 play.
- ✅ **Video:** H.264 and HEVC through VideoToolbox, shown on `SurfaceView`, `TextureView`
  (`SurfaceTexture`) and `VideoView`, zero-copy as Metal textures.
- ✅ `WebView` on WKWebView, with a JavaScript bridge and cookies.
- ✅ An iOS host app that runs an APK; the runner bundles the APK, the runtime and the
  framework into it (`scripts/runner-ios.sh`).

## ✅ Performance, phase 1

- ✅ **nterp.** ART's assembly interpreter is ported to Mach-O with compressed references. It is
  2–7× faster than the C++ switch interpreter.
- ✅ **GPU rendering.**
  - Skia Ganesh on Metal, with windows composed into a `CAMetalLayer`.
  - Double-buffered window targets with no per-frame copies.
  - `RenderNode` layers on the GPU.
- ✅ **Render scale.** Android renders at 2 px/pt on 3× screens.
- ✅ **GPU memory.** Metal objects on ART threads are released in autorelease pools, and a
  regression test guards against leaks.
- ✅ **Verification cache.** Without dex2oat, every class an app uses was verified on each
  launch (79% of Spotify's main thread at startup). The first launch now records the classes it
  verified into a vdex in the app's data directory, and later launches only check their
  recorded dependencies. Spotify reaches its first frame about 45% sooner on the Mac.

## 🔄 Real-world compatibility

Real apps drive this work. Each app's failures turn into general fixes and regression tests.

| App | State |
|---|---|
| **Yandex Music** | ✅ Sign-in (WebView), persisted session, Compose UI, playback, video covers. |
| **Spotify** | ✅ Launch (about 2 s to first frame) and sign-in. Content depends on the account region. |

- 🔄 Build a corpus of 10–20 popular APKs (messengers, social, media, games, utilities) and
  fix the shared gaps.
- ✅ Multi-touch: pointer down/up, moves with every finger, batched history.
- ⏳ Hardware keyboards and mouse/trackpad pointers.
- ⏳ Notifications delivered as iOS notifications, and background playback controls.
- ⏳ Camera (Camera2 on AVCaptureSession) and the photo picker.
- ⏳ `NsdManager` / service discovery, and Bluetooth basics.
- ⏳ A microG-style replacement for common Google Play Services APIs.

## ⏳ M6: Performance, phase 2

- ⏳ **AOT compilation.** Run dex2oat for the framework and for apps, then sign and package its
  output like native `.so` files. The optimizing compiler needs to learn the compressed
  reference arena. This brings compiled-code speed with no JIT.
- ⏳ **A boot image** with preinitialized framework classes and heap, for near-instant start.
- ⏳ **A hardware renderer.** Record display lists on the UI thread and render them on a render
  thread on the GPU, as Android does.
- ⏳ **An encoder side.** `MediaCodec` encoders (VideoToolbox) and `MediaRecorder`.

## ⏳ Delivery

- ⏳ Convert APKs to IPAs from the command line (`andropkg convert app.apk -o app.ipa`).
- ⏳ Sign on the device from the host app, so APKs install without a computer.
- ⏳ Packaging for AltStore/SideStore.
