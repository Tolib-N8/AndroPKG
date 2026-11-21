// andropkg (M2d): the window system under ViewRootImpl, without SurfaceFlinger or libgui.
//
//  * DisplayEventReceiver (Choreographer's vsync): a 60 Hz tick on the receiver's Looper.
//  * SurfaceControl / BLASTBufferQueue / Surface: a window is one CPU pixel buffer.
//    Surface.lockCanvas() points the Java Canvas at it (software rendering, as on a device
//    with hardware acceleration off); unlockCanvasAndPost() hands the frame to the host
//    (iOS: a CALayer; macOS dev loop: an optional PNG, ANDROPKG_FRAME_PNG).
//  * SurfaceControl.Transaction, SurfaceSession, InputChannel/InputEventReceiver: accepted
//    and ignored — composition is the host's job, input arrives in M3.
#define LOG_TAG "andropkg"
#include <android/graphics/canvas.h>
#include <android_runtime/AndroidRuntime.h>
#include <android/native_window.h>
#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <time.h>
#include <utils/Looper.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#include "android_os_MessageQueue.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"

// One function for every native of an "inert" class: returns 0 / 0.0 / null whatever the
// signature (x0 and d0 cleared; arguments are caller-saved and ignored).
extern "C" void andropkg_jni_zero();
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _andropkg_jni_zero\n"
    "_andropkg_jni_zero:\n"
    "  mov x0, #0\n"
    "  fmov d0, xzr\n"
    "  ret\n");

// ---- host frame sink ------------------------------------------------------------------

using FrameSink = void (*)(const void* rgba, int width, int height, int stride_bytes, void* ctx);
static std::atomic<FrameSink> gSink{nullptr};
static void* gSinkCtx = nullptr;

/// Called by the host app: every posted window frame (RGBA8888, premultiplied) goes to `sink`.
extern "C" __attribute__((visibility("default"))) void andropkg_set_frame_sink(FrameSink sink, void* ctx) {
  gSinkCtx = ctx;
  gSink = sink;
}

namespace {
void InjectTouch(int action, float x, float y);
void InjectText(const char* utf8);
void InjectDeleteBackward();
void InjectEnter();
void SetImeListener(void (*listener)(int visible));
void InjectKey(int action, int keycode);
void SetHostVisible(bool visible);
}  // namespace

/// The host app moved to the background (0) or foreground (1).
extern "C" __attribute__((visibility("default"))) void andropkg_app_visibility(int visible) {
  SetHostVisible(visible != 0);
}

/// A key event for the focused window: action 0 down / 1 up, Android keycode (4 = BACK).
extern "C" __attribute__((visibility("default"))) void andropkg_inject_key(int action, int keycode) {
  InjectKey(action, keycode);
}

/// Host keyboard -> the focused text editor of the focused window.
extern "C" __attribute__((visibility("default"))) void andropkg_ime_insert_text(const char* utf8) {
  InjectText(utf8);
}
extern "C" __attribute__((visibility("default"))) void andropkg_ime_delete_backward() { InjectDeleteBackward(); }
extern "C" __attribute__((visibility("default"))) void andropkg_ime_enter() { InjectEnter(); }
/// `listener(1)` when a text editor gains focus (show the keyboard), `listener(0)` to hide it.
extern "C" __attribute__((visibility("default"))) void andropkg_set_ime_listener(void (*listener)(int visible)) {
  SetImeListener(listener);
}

/// Called by the host: a touch at window pixel (x, y). action is MotionEvent's
/// (0 down, 1 up, 2 move, 3 cancel). Delivered on the focused window's Looper.
extern "C" __attribute__((visibility("default"))) void andropkg_inject_touch(int action, float x, float y) {
  InjectTouch(action, x, y);
}

namespace {

bool WritePngTo(const char* path, const void* rgba, int w, int h, int stride) {
  SkPixmap pm(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType), rgba,
              static_cast<size_t>(stride));
  SkFILEWStream out(path);
  return out.isValid() && SkPngEncoder::Encode(&out, pm, {});
}

// The last frame shown (tests: "shot" in ANDROPKG_TEST_SCRIPT).
std::mutex gLastFrameLock;
std::vector<uint32_t> gLastFrame;
int gLastW = 0, gLastH = 0;
bool TestsEnabled() {
  static const bool on = getenv("ANDROPKG_TEST_SCRIPT") != nullptr;
  return on;
}

void MaybeStartTests();

// A finished display image (RGBA8888, premultiplied) to the host.
void Present(const void* rgba, int w, int h, int stride) {
  if (TestsEnabled()) {
    std::lock_guard<std::mutex> g(gLastFrameLock);
    gLastFrame.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
      memcpy(&gLastFrame[static_cast<size_t>(y) * w], static_cast<const uint8_t*>(rgba) + static_cast<size_t>(y) * stride,
             static_cast<size_t>(w) * 4);
    }
    gLastW = w;
    gLastH = h;
  }
  if (FrameSink sink = gSink.load()) {
    sink(rgba, w, h, stride, gSinkCtx);
  } else if (const char* path = getenv("ANDROPKG_FRAME_PNG")) {
    if (WritePngTo(path, rgba, w, h, stride)) ALOGI("andropkg: frame %dx%d -> %s", w, h, path);
  }
  MaybeStartTests();
}

// ---- windows ---------------------------------------------------------------------------

struct WindowSurface {
  std::mutex lock;
  int width = 0, height = 0;   // the size the next frame is drawn at
  std::vector<uint32_t> pixels;
  int drawnW = 0, drawnH = 0;  // the size of `pixels`' last frame
  uint64_t frames = 0;
};

struct Layer {  // SurfaceControl
  std::string name;
  int width, height;
  WindowSurface* surface = nullptr;
  WindowSurface* Surface() {
    if (!surface) {
      surface = new WindowSurface;
      surface->width = width;
      surface->height = height;
    }
    return surface;
  }
};

struct Bbq {  // BLASTBufferQueue
  std::string name;
  WindowSurface* surface = new WindowSurface;
};

template <typename T>
T* Ptr(jlong p) {
  return reinterpret_cast<T*>(p);
}

void PostFrame(WindowSurface* s);

void NoopFinalizer(void*) {}
jlong NoopFinalizerPtr(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&NoopFinalizer); }

struct {
  jclass surfaceClass;
  jmethodID surfaceCtor;
  jfieldID rectLeft, rectTop, rectRight, rectBottom;
} gJni;

// SurfaceControl
void EnsureWmNatives(JNIEnv* env);

jlong sc_create(JNIEnv* env, jclass, jobject /*session*/, jstring name, jint w, jint h, jint /*format*/,
                jint /*flags*/, jlong /*parent*/, jobject /*metadata*/) {
  EnsureWmNatives(env);
  auto* layer = new Layer;
  if (name) {
    const char* s = env->GetStringUTFChars(name, nullptr);
    layer->name = s;
    env->ReleaseStringUTFChars(name, s);
  }
  layer->width = w;
  layer->height = h;
  return reinterpret_cast<jlong>(layer);
}
jlong sc_copy(JNIEnv*, jclass, jlong ptr) { return ptr; }
jlong sc_getHandle(JNIEnv*, jclass, jlong ptr) { return ptr; }
jint sc_getLayerId(JNIEnv*, jclass, jlong ptr) { return static_cast<jint>(ptr & 0x7fffffff); }
jlong sc_createTransaction(JNIEnv*, jclass) { return reinterpret_cast<jlong>(new char[1]); }

// BLASTBufferQueue
jlong bbq_create(JNIEnv* env, jclass, jstring name, jboolean) {
  auto* q = new Bbq;
  if (name) {
    const char* s = env->GetStringUTFChars(name, nullptr);
    q->name = s;
    env->ReleaseStringUTFChars(name, s);
  }
  return reinterpret_cast<jlong>(q);
}
jobject bbq_getSurface(JNIEnv* env, jclass, jlong ptr, jboolean) {
  return env->NewObject(gJni.surfaceClass, gJni.surfaceCtor, reinterpret_cast<jlong>(Ptr<Bbq>(ptr)->surface));
}
void bbq_update(JNIEnv*, jclass, jlong ptr, jlong sc, jlong w, jlong h, jint /*format*/) {
  WindowSurface* s = Ptr<Bbq>(ptr)->surface;
  if (sc != 0) Ptr<Layer>(sc)->surface = s;  // the window's frames come from this queue
  std::lock_guard<std::mutex> g(s->lock);
  s->width = static_cast<int>(w);
  s->height = static_cast<int>(h);
}
jboolean bbq_isSameSurfaceControl(JNIEnv*, jclass, jlong, jlong) { return JNI_TRUE; }

// Surface
jboolean s_isValid(JNIEnv*, jclass, jlong ptr) { return ptr != 0; }
jint s_getWidth(JNIEnv*, jclass, jlong ptr) { return Ptr<WindowSurface>(ptr)->width; }
jint s_getHeight(JNIEnv*, jclass, jlong ptr) { return Ptr<WindowSurface>(ptr)->height; }
jlong s_getNextFrameNumber(JNIEnv*, jclass, jlong ptr) { return static_cast<jlong>(Ptr<WindowSurface>(ptr)->frames + 1); }
jlong s_fromSurfaceControl(JNIEnv*, jclass, jlong /*surface*/, jlong sc) {
  return reinterpret_cast<jlong>(Ptr<Layer>(sc)->Surface());
}
jlong s_createFromSurfaceControl(JNIEnv*, jclass, jlong sc) {
  return reinterpret_cast<jlong>(Ptr<Layer>(sc)->Surface());
}
jlong s_fromBlastBufferQueue(JNIEnv*, jclass, jlong /*surface*/, jlong bbq) {
  return reinterpret_cast<jlong>(Ptr<Bbq>(bbq)->surface);
}

jlong s_lockCanvas(JNIEnv* env, jclass, jlong ptr, jobject canvasObj, jobject dirtyRect) {
  WindowSurface* s = Ptr<WindowSurface>(ptr);
  if (s == nullptr || s->width <= 0 || s->height <= 0) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "surface has no size");
    return 0;
  }
  s->lock.lock();
  size_t n = static_cast<size_t>(s->width) * s->height;
  if (s->pixels.size() != n) s->pixels.assign(n, 0);
  s->drawnW = s->width;
  s->drawnH = s->height;
  ANativeWindow_Buffer buffer = {};
  buffer.width = s->width;
  buffer.height = s->height;
  buffer.stride = s->width;
  buffer.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  buffer.bits = s->pixels.data();
  android::graphics::Canvas canvas(env, canvasObj);
  canvas.setBuffer(&buffer, ADATASPACE_SRGB);
  if (dirtyRect) {  // the whole window is redrawn: one buffer, nothing to preserve per frame
    env->SetIntField(dirtyRect, gJni.rectLeft, 0);
    env->SetIntField(dirtyRect, gJni.rectTop, 0);
    env->SetIntField(dirtyRect, gJni.rectRight, s->width);
    env->SetIntField(dirtyRect, gJni.rectBottom, s->height);
  }
  return ptr;
}

void s_unlockCanvasAndPost(JNIEnv* env, jclass, jlong ptr, jobject canvasObj) {
  WindowSurface* s = Ptr<WindowSurface>(ptr);
  android::graphics::Canvas canvas(env, canvasObj);
  canvas.setBuffer(nullptr, ADATASPACE_UNKNOWN);
  s->frames++;
  s->lock.unlock();
  PostFrame(s);
}

// ---- vsync (Choreographer) ---------------------------------------------------------------

constexpr nsecs_t kVsyncPeriod = 1000000000LL / 60;

struct {
  jmethodID dispatchVsync;
  jfieldID preferredFrameTimelineIndex, frameTimelinesLength, frameInterval, frameTimelines;
  jfieldID ftVsyncId, ftExpectedPresentationTime, ftDeadline;
  jmethodID referenceGet;
  jclass dataClass, timelineClass;
  jmethodID dataCtor;      // VsyncEventData(FrameTimeline[], int preferredIndex, int length, long interval)
  jmethodID timelineCtor;  // FrameTimeline(long vsyncId, long expectedPresentTime, long deadline)
} gVsync;

nsecs_t MonotonicNow() {  // System.nanoTime()'s clock
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<nsecs_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

class VsyncReceiver : public android::MessageHandler {
 public:
  VsyncReceiver(JNIEnv* env, jobject receiverWeak, jobject dataWeak, android::sp<android::MessageQueue> mq)
      : receiver_(env->NewGlobalRef(receiverWeak)), data_(env->NewGlobalRef(dataWeak)), queue_(mq) {}

  void Schedule() {
    if (pending_) return;
    pending_ = true;
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    nsecs_t next = (now / kVsyncPeriod + 1) * kVsyncPeriod;
    if (getenv("ANDROPKG_DEBUG_VSYNC")) ALOGI("vsync: schedule now=%lld next=%lld", (long long)now, (long long)next);
    queue_->getLooper()->sendMessageAtTime(next, this, android::Message(0));
  }

  void handleMessage(const android::Message&) override {
    pending_ = false;
    if (getenv("ANDROPKG_DEBUG_VSYNC")) ALOGI("vsync: tick %lld", (long long)frame_);
    JNIEnv* env = android::AndroidRuntime::getJNIEnv();
    jobject receiver = env->CallObjectMethod(receiver_, gVsync.referenceGet);
    jobject data = env->CallObjectMethod(data_, gVsync.referenceGet);
    if (receiver && data) {
      nsecs_t ts = MonotonicNow();
      frame_++;
      env->SetIntField(data, gVsync.preferredFrameTimelineIndex, 0);
      env->SetIntField(data, gVsync.frameTimelinesLength, 1);
      env->SetLongField(data, gVsync.frameInterval, kVsyncPeriod);
      jobjectArray timelines = static_cast<jobjectArray>(env->GetObjectField(data, gVsync.frameTimelines));
      if (timelines && env->GetArrayLength(timelines) > 0) {
        jobject t = env->GetObjectArrayElement(timelines, 0);
        env->SetLongField(t, gVsync.ftVsyncId, frame_);
        env->SetLongField(t, gVsync.ftExpectedPresentationTime, ts + kVsyncPeriod);
        env->SetLongField(t, gVsync.ftDeadline, ts + kVsyncPeriod);
        env->DeleteLocalRef(t);
      }
      if (timelines) env->DeleteLocalRef(timelines);
      env->CallVoidMethod(receiver, gVsync.dispatchVsync, static_cast<jlong>(ts), jlong{0},
                          static_cast<jint>(frame_));
    }
    if (receiver) env->DeleteLocalRef(receiver);
    if (data) env->DeleteLocalRef(data);
    queue_->raiseAndClearException(env, "dispatchVsync");
  }

 private:
  jobject receiver_;
  jobject data_;
  android::sp<android::MessageQueue> queue_;
  bool pending_ = false;
  int64_t frame_ = 0;

 public:
  int64_t frame() const { return frame_; }
};

jlong der_init(JNIEnv* env, jclass, jobject receiverWeak, jobject dataWeak, jobject messageQueue, jint, jint,
               jlong) {
  android::sp<android::MessageQueue> mq = android::android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (mq == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  auto* r = new VsyncReceiver(env, receiverWeak, dataWeak, mq);
  r->incStrong(nullptr);  // owned by the Java object (never finalized away from its Looper)
  return reinterpret_cast<jlong>(r);
}
void der_scheduleVsync(JNIEnv*, jclass, jlong ptr) { reinterpret_cast<VsyncReceiver*>(ptr)->Schedule(); }

// The vsync a frame would land on now (Choreographer.getVsyncEventData, back animations).
jobject der_getLatestVsyncEventData(JNIEnv* env, jclass, jlong ptr) {
  nsecs_t ts = MonotonicNow();
  nsecs_t next = (ts / kVsyncPeriod + 1) * kVsyncPeriod;
  jlong id = ptr ? reinterpret_cast<VsyncReceiver*>(ptr)->frame() + 1 : 1;
  jobject t = env->NewObject(gVsync.timelineClass, gVsync.timelineCtor, id, static_cast<jlong>(next),
                             static_cast<jlong>(next));
  jobjectArray arr = env->NewObjectArray(1, gVsync.timelineClass, t);
  jobject data = env->NewObject(gVsync.dataClass, gVsync.dataCtor, arr, 0, 1, static_cast<jlong>(kVsyncPeriod));
  env->DeleteLocalRef(t);
  env->DeleteLocalRef(arr);
  return data;
}

// ---- input (M3): host touches -> the focused window's InputEventReceiver ------------------

struct {
  jclass motionEventClass;
  jmethodID obtain;        // MotionEvent.obtain(long downTime, long eventTime, int action, float x, float y, int meta)
  jmethodID setSource;
  jmethodID recycle;
  jmethodID dispatchInputEvent;  // InputEventReceiver.dispatchInputEvent(int seq, InputEvent)
  jclass keyEventClass;
  jmethodID keyEventCtor;  // KeyEvent(long downTime, long eventTime, int action, int code, int repeat)
  jmethodID keySetSource;
  jmethodID onFocusEvent;  // InputEventReceiver.onFocusEvent(boolean)
  jmethodID offsetLocation;
  jfieldID channelPtr;     // InputChannel.mPtr: the window id WindowManagerImpl assigned
} gInput;

// Host input for the focused window, delivered on its Looper: a touch, or keyboard text.
struct TouchEvent {
  enum Kind { kTouch, kText, kDeleteBackward, kEnter, kKey, kFocus, kVisibility } kind = kTouch;
  int keycode = 0;
  int action = 0;
  float x = 0, y = 0;    // display coordinates
  float ox = 0, oy = 0;  // the target window's origin on the display
  std::string text;
};

// dev.andropkg.system.HostIme (services; system class loader): the host keyboard as IME.
struct {
  jclass clazz;
  jmethodID onTouchUp, commitText, deleteBackward, enter;
} gIme;
using ImeListener = void (*)(int visible);
std::atomic<ImeListener> gImeListener{nullptr};

void ime_setKeyboardVisible(JNIEnv*, jclass, jboolean visible) {
  if (ImeListener l = gImeListener.load()) l(visible ? 1 : 0);
  if (getenv("ANDROPKG_DEBUG_IME")) ALOGI("ime: keyboard %s", visible ? "shown" : "hidden");
}

// dev.andropkg.system.HostApp: the host app's foreground/background state.
struct {
  jclass clazz;
  jmethodID setVisible;
} gHostApp;

jclass LoadServicesClass(JNIEnv* env, const char* name) {
  jclass loaderClass = env->FindClass("java/lang/ClassLoader");
  jmethodID getSystem = env->GetStaticMethodID(loaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  jobject loader = env->CallStaticObjectMethod(loaderClass, getSystem);
  jstring jname = env->NewStringUTF(name);
  jclass c = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, jname));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return nullptr;
  }
  return c;
}

bool InitHostApp(JNIEnv* env) {
  if (gHostApp.clazz) return true;
  jclass c = LoadServicesClass(env, "dev.andropkg.system.HostApp");
  if (c == nullptr) return false;
  gHostApp.setVisible = env->GetStaticMethodID(c, "setVisible", "(Z)V");
  gHostApp.clazz = static_cast<jclass>(env->NewGlobalRef(c));
  return true;
}

bool InitIme(JNIEnv* env) {
  if (gIme.clazz) return true;
  jclass loaderClass = env->FindClass("java/lang/ClassLoader");
  jmethodID getSystem = env->GetStaticMethodID(loaderClass, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  jmethodID loadClass = env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
  jobject loader = env->CallStaticObjectMethod(loaderClass, getSystem);
  jstring name = env->NewStringUTF("dev.andropkg.system.HostIme");
  jclass c = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, name));
  if (env->ExceptionCheck() || c == nullptr) {
    env->ExceptionClear();
    return false;  // not running under andropkg's services
  }
  JNINativeMethod m = {"nativeSetKeyboardVisible", "(Z)V", reinterpret_cast<void*>(ime_setKeyboardVisible)};
  env->RegisterNatives(c, &m, 1);
  gIme.onTouchUp = env->GetStaticMethodID(c, "onTouchUp", "()V");
  gIme.commitText = env->GetStaticMethodID(c, "commitText", "(Ljava/lang/String;)V");
  gIme.deleteBackward = env->GetStaticMethodID(c, "deleteBackward", "()V");
  gIme.enter = env->GetStaticMethodID(c, "enter", "()V");
  gIme.clazz = static_cast<jclass>(env->NewGlobalRef(c));
  return true;
}

class InputReceiver : public android::MessageHandler {
 public:
  InputReceiver(JNIEnv* env, jobject receiverWeak, android::sp<android::MessageQueue> mq)
      : receiver_(env->NewGlobalRef(receiverWeak)), queue_(mq) {}

  void Post(const TouchEvent& e) {
    {
      std::lock_guard<std::mutex> g(lock_);
      pending_.push_back(e);
    }
    queue_->getLooper()->sendMessage(this, android::Message(0));
  }

  void handleMessage(const android::Message&) override {
    JNIEnv* env = android::AndroidRuntime::getJNIEnv();
    for (;;) {
      TouchEvent e;
      {
        std::lock_guard<std::mutex> g(lock_);
        if (pending_.empty()) break;
        e = pending_.front();
        pending_.pop_front();
      }
      if (e.kind == TouchEvent::kVisibility) {
        if (InitHostApp(env)) {
          env->CallStaticVoidMethod(gHostApp.clazz, gHostApp.setVisible, static_cast<jboolean>(e.action != 0));
        }
        queue_->raiseAndClearException(env, "HostApp.setVisible");
        continue;
      }
      if (e.kind == TouchEvent::kFocus) {  // what InputDispatcher sends a newly focused window
        jobject receiver = env->CallObjectMethod(receiver_, gVsync.referenceGet);
        if (receiver) {
          env->CallVoidMethod(receiver, gInput.onFocusEvent, static_cast<jboolean>(e.action != 0));
          env->DeleteLocalRef(receiver);
        }
        queue_->raiseAndClearException(env, "onFocusEvent");
        continue;
      }
      if (e.kind == TouchEvent::kKey) {  // a hardware-style key (BACK from the host's gesture)
        jlong now = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;
        if (e.action == 0) key_down_time_ = now;
        jobject ev = env->NewObject(gInput.keyEventClass, gInput.keyEventCtor, key_down_time_, now, e.action,
                                    e.keycode, 0);
        env->CallVoidMethod(ev, gInput.keySetSource, 0x101 /* InputDevice.SOURCE_KEYBOARD */);
        jobject receiver = env->CallObjectMethod(receiver_, gVsync.referenceGet);
        if (receiver) {
          env->CallVoidMethod(receiver, gInput.dispatchInputEvent, ++seq_, ev);
          env->DeleteLocalRef(receiver);
        }
        env->DeleteLocalRef(ev);
        queue_->raiseAndClearException(env, "dispatchInputEvent(key)");
        continue;
      }
      if (e.kind != TouchEvent::kTouch) {
        if (InitIme(env)) {
          if (e.kind == TouchEvent::kText) {
            jstring s = env->NewStringUTF(e.text.c_str());
            env->CallStaticVoidMethod(gIme.clazz, gIme.commitText, s);
            env->DeleteLocalRef(s);
          } else {
            env->CallStaticVoidMethod(gIme.clazz, e.kind == TouchEvent::kEnter ? gIme.enter : gIme.deleteBackward);
          }
        }
        queue_->raiseAndClearException(env, "host keyboard");
        continue;
      }
      jlong now = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;  // SystemClock.uptimeMillis()
      if (e.action == 0 /* ACTION_DOWN */) down_time_ = now;
      jobject ev = env->CallStaticObjectMethod(gInput.motionEventClass, gInput.obtain, down_time_, now, e.action,
                                               e.x, e.y, 0);
      env->CallVoidMethod(ev, gInput.setSource, 0x1002 /* InputDevice.SOURCE_TOUCHSCREEN */);
      // Window coordinates; getRawX/Y stay display coordinates, as on a device.
      if (e.ox != 0 || e.oy != 0) env->CallVoidMethod(ev, gInput.offsetLocation, -e.ox, -e.oy);
      jobject receiver = env->CallObjectMethod(receiver_, gVsync.referenceGet);
      if (receiver) {
        env->CallVoidMethod(receiver, gInput.dispatchInputEvent, ++seq_, ev);
        env->DeleteLocalRef(receiver);
      }
      env->DeleteLocalRef(ev);
      queue_->raiseAndClearException(env, "dispatchInputEvent");
      if (e.action == 1 /* ACTION_UP */ && InitIme(env)) {
        // After the view tree handled the tap (focus may have moved to a text editor).
        env->CallStaticVoidMethod(gIme.clazz, gIme.onTouchUp);
        queue_->raiseAndClearException(env, "HostIme.onTouchUp");
      }
    }
  }

 private:
  jobject receiver_;
  android::sp<android::MessageQueue> queue_;
  std::mutex lock_;
  std::deque<TouchEvent> pending_;
  jlong down_time_ = 0;
  jlong key_down_time_ = 0;
  jint seq_ = 0;
};

// ---- windows: frames, z-order, focus, touch routing, composition --------------------------
//
// WindowManagerImpl (services) computes each window's frame and z-order as WMS does and reports
// them here (nativeSetWindow). The visible windows' frames are composed in z-order, with
// FLAG_DIM_BEHIND dimming, into one display-size image for the host. Touches go to the window
// under the finger (or to a touch-modal window), keys and text to the focused window.

constexpr int kFlagDimBehind = 0x2, kFlagNotFocusable = 0x8, kFlagNotTouchable = 0x10,
              kFlagNotTouchModal = 0x20, kFlagWatchOutsideTouch = 0x40000;

struct HostWindow {
  int id = 0;
  Layer* layer = nullptr;
  InputReceiver* receiver = nullptr;
  int fx = 0, fy = 0, fw = 0, fh = 0;  // frame (input, window coordinates' origin)
  int sx = 0, sy = 0;                  // surface origin (frame minus surface insets)
  int z = 0, flags = 0;
  float dim = 0;
  bool visible = false;
  bool placed = false;  // nativeSetWindow called at least once
};

std::mutex gWmLock;                            // before any WindowSurface::lock
std::map<int, HostWindow> gWindows;            // by window id (InputChannel.mPtr)
std::vector<InputReceiver*> gOrphanReceivers;  // windows not added through WindowManagerImpl
InputReceiver* gFocused = nullptr;
int gTouchTarget = -1;  // the window that owns the current gesture
int gDisplayW = 0, gDisplayH = 0;
std::vector<uint32_t> gScreen;

std::vector<HostWindow*> VisibleTopDownLocked() {
  std::vector<HostWindow*> v;
  for (auto& [id, w] : gWindows) {
    if (w.placed && w.visible) v.push_back(&w);
  }
  std::sort(v.begin(), v.end(), [](HostWindow* a, HostWindow* b) { return a->z > b->z; });
  return v;
}

// The top visible focusable window gets focus, as InputDispatcher would give it.
void UpdateFocusLocked() {
  InputReceiver* f = nullptr;
  for (HostWindow* w : VisibleTopDownLocked()) {
    if (w->receiver && !(w->flags & kFlagNotFocusable)) {
      f = w->receiver;
      break;
    }
  }
  if (f == nullptr && !gOrphanReceivers.empty()) f = gOrphanReceivers.back();
  if (f == gFocused) return;
  TouchEvent e;
  e.kind = TouchEvent::kFocus;
  if (gFocused) {
    e.action = 0;
    gFocused->Post(e);
  }
  gFocused = f;
  if (f) {
    e.action = 1;
    f->Post(e);
  }
}

void ComposeLocked() {
  std::vector<HostWindow*> drawn;
  for (HostWindow* w : VisibleTopDownLocked()) {
    if (w->layer && w->layer->surface && w->layer->surface->frames > 0) drawn.push_back(w);
  }
  if (drawn.empty() || gDisplayW <= 0 || gDisplayH <= 0) return;
  std::reverse(drawn.begin(), drawn.end());  // bottom first
  if (drawn.size() == 1 && drawn[0]->dim == 0 && drawn[0]->sx == 0 && drawn[0]->sy == 0) {
    WindowSurface* s = drawn[0]->layer->surface;
    std::lock_guard<std::mutex> g(s->lock);
    if (s->drawnW == gDisplayW && s->drawnH == gDisplayH) {  // one full-screen window: as is
      Present(s->pixels.data(), s->drawnW, s->drawnH, s->drawnW * 4);
      return;
    }
  }
  gScreen.assign(static_cast<size_t>(gDisplayW) * gDisplayH, 0xff000000u);  // opaque black
  SkImageInfo info = SkImageInfo::Make(gDisplayW, gDisplayH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  std::unique_ptr<SkCanvas> canvas = SkCanvas::MakeRasterDirect(info, gScreen.data(), gDisplayW * 4);
  for (HostWindow* w : drawn) {
    if (w->dim > 0) canvas->drawColor(SkColorSetARGB(static_cast<U8CPU>(w->dim * 255), 0, 0, 0));
    WindowSurface* s = w->layer->surface;
    std::lock_guard<std::mutex> g(s->lock);
    if (s->drawnW <= 0 || s->pixels.size() < static_cast<size_t>(s->drawnW) * s->drawnH) continue;
    SkBitmap bm;
    bm.installPixels(SkImageInfo::Make(s->drawnW, s->drawnH, kRGBA_8888_SkColorType, kPremul_SkAlphaType),
                     s->pixels.data(), static_cast<size_t>(s->drawnW) * 4);
    bm.setImmutable();
    canvas->drawImage(bm.asImage(), w->sx, w->sy);
  }
  Present(gScreen.data(), gDisplayW, gDisplayH, gDisplayW * 4);
}

void PostFrame(WindowSurface* s) {
  std::lock_guard<std::mutex> g(gWmLock);
  bool anyPlaced = false;
  for (auto& [id, w] : gWindows) {
    if (!w.placed) continue;
    anyPlaced = true;
    if (w.layer && w.layer->surface == s) {
      if (w.visible) ComposeLocked();
      return;
    }
  }
  if (!anyPlaced) {  // no window manager: show the surface as it is
    std::lock_guard<std::mutex> sg(s->lock);
    Present(s->pixels.data(), s->drawnW, s->drawnH, s->drawnW * 4);
  }
}

// WindowManagerImpl.nativeSetWindow(...): a window's frame, surface, z-order and flags.
void wm_setWindow(JNIEnv*, jclass, jint id, jlong sc, jint fx, jint fy, jint fw, jint fh, jint sx, jint sy,
                  jint sw, jint sh, jint z, jint flags, jfloat dim, jboolean visible, jint dw, jint dh) {
  std::lock_guard<std::mutex> g(gWmLock);
  HostWindow& w = gWindows[id];
  w.id = id;
  bool wasShown = w.placed && w.visible;
  if (sc != 0) {
    w.layer = Ptr<Layer>(sc);
    w.layer->width = sw;
    w.layer->height = sh;
    WindowSurface* s = w.layer->Surface();
    std::lock_guard<std::mutex> sg(s->lock);
    s->width = sw;  // the window's next frame is drawn at its new size
    s->height = sh;
  }
  w.fx = fx, w.fy = fy, w.fw = fw, w.fh = fh, w.sx = sx, w.sy = sy;
  w.z = z;
  w.flags = flags;
  w.dim = (flags & kFlagDimBehind) ? dim : 0;
  w.visible = visible;
  w.placed = true;
  gDisplayW = dw;
  gDisplayH = dh;
  UpdateFocusLocked();
  if (wasShown || visible) ComposeLocked();
}

void wm_removeWindow(JNIEnv*, jclass, jint id) {
  std::lock_guard<std::mutex> g(gWmLock);
  auto it = gWindows.find(id);
  if (it == gWindows.end()) return;
  bool wasShown = it->second.placed && it->second.visible;
  if (it->second.receiver) gOrphanReceivers.erase(
      std::remove(gOrphanReceivers.begin(), gOrphanReceivers.end(), it->second.receiver), gOrphanReceivers.end());
  gWindows.erase(it);
  if (gTouchTarget == id) gTouchTarget = -1;
  UpdateFocusLocked();
  if (wasShown) ComposeLocked();
}

std::atomic<bool> gWmNativesRegistered{false};
void EnsureWmNatives(JNIEnv* env) {
  if (gWmNativesRegistered.load()) return;
  jclass c = LoadServicesClass(env, "dev.andropkg.system.WindowManagerImpl");
  if (c == nullptr) return;  // not running under andropkg's services
  static const JNINativeMethod kMethods[] = {
      {"nativeSetWindow", "(IJIIIIIIIIIIFZII)V", reinterpret_cast<void*>(wm_setWindow)},
      {"nativeRemoveWindow", "(I)V", reinterpret_cast<void*>(wm_removeWindow)},
  };
  if (env->RegisterNatives(c, kMethods, 2) == JNI_OK) {
    gWmNativesRegistered = true;
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(c);
}

jlong ier_init(JNIEnv* env, jclass, jobject receiverWeak, jobject inputChannel, jobject messageQueue) {
  android::sp<android::MessageQueue> mq = android::android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (mq == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  auto* r = new InputReceiver(env, receiverWeak, mq);
  r->incStrong(nullptr);
  int id = inputChannel ? static_cast<int>(env->GetLongField(inputChannel, gInput.channelPtr)) : 0;
  std::lock_guard<std::mutex> g(gWmLock);
  if (id > 0) {
    gWindows[id].id = id;
    gWindows[id].receiver = r;
  } else {
    gOrphanReceivers.push_back(r);
  }
  UpdateFocusLocked();
  return reinterpret_cast<jlong>(r);
}

void ier_dispose(JNIEnv*, jclass, jlong ptr) {
  auto* r = reinterpret_cast<InputReceiver*>(ptr);
  std::lock_guard<std::mutex> g(gWmLock);
  for (auto& [id, w] : gWindows) {
    if (w.receiver == r) w.receiver = nullptr;
  }
  gOrphanReceivers.erase(std::remove(gOrphanReceivers.begin(), gOrphanReceivers.end(), r), gOrphanReceivers.end());
  if (gFocused == r) gFocused = nullptr;
  UpdateFocusLocked();
}

void InjectTouch(int action, float x, float y) {
  TouchEvent e;
  e.action = action;
  e.x = x;
  e.y = y;
  std::lock_guard<std::mutex> g(gWmLock);
  if (action == 0 /* DOWN */) {
    gTouchTarget = -1;
    for (HostWindow* w : VisibleTopDownLocked()) {
      if (!w->receiver || (w->flags & kFlagNotTouchable)) continue;
      bool inside = x >= w->fx && x < w->fx + w->fw && y >= w->fy && y < w->fy + w->fh;
      // A focusable window without FLAG_NOT_TOUCH_MODAL takes touches outside it too.
      bool modal = !(w->flags & (kFlagNotTouchModal | kFlagNotFocusable));
      if (inside || modal) {
        gTouchTarget = w->id;
        break;
      }
      if (w->flags & kFlagWatchOutsideTouch) {  // e.g. a PopupWindow: dismiss on outside taps
        TouchEvent o = e;
        o.action = 4;  // ACTION_OUTSIDE
        o.ox = static_cast<float>(w->fx);
        o.oy = static_cast<float>(w->fy);
        w->receiver->Post(o);
      }
    }
  }
  InputReceiver* r = nullptr;
  auto it = gWindows.find(gTouchTarget);
  if (it != gWindows.end() && it->second.receiver) {
    r = it->second.receiver;
    e.ox = static_cast<float>(it->second.fx);
    e.oy = static_cast<float>(it->second.fy);
  } else if (gWindows.empty() && !gOrphanReceivers.empty()) {
    r = gOrphanReceivers.back();
  }
  static const bool debug = getenv("ANDROPKG_DEBUG_INPUT") != nullptr;
  if (debug) ALOGI("input: touch %d at %.0f,%.0f -> window %d%s", action, x, y, gTouchTarget, r ? "" : " (dropped)");
  if (r) r->Post(e);
  if (action == 1 || action == 3) gTouchTarget = -1;
}

void PostToFocused(const TouchEvent& e) {
  std::lock_guard<std::mutex> g(gWmLock);
  if (gFocused) gFocused->Post(e);
}

void InjectKeyboard(TouchEvent::Kind kind, const char* text) {
  TouchEvent e;
  e.kind = kind;
  if (text) e.text = text;
  PostToFocused(e);
}

void InjectKey(int action, int keycode) {
  TouchEvent e;
  e.kind = TouchEvent::kKey;
  e.action = action;
  e.keycode = keycode;
  PostToFocused(e);
}

void SetHostVisible(bool visible) {
  TouchEvent e;
  e.kind = TouchEvent::kVisibility;
  e.action = visible ? 1 : 0;
  std::lock_guard<std::mutex> g(gWmLock);
  InputReceiver* r = gFocused;  // any window: all share the main Looper
  for (auto& [id, w] : gWindows) {
    if (!r && w.receiver) r = w.receiver;
  }
  if (r) r->Post(e);
}

void InjectText(const char* utf8) { InjectKeyboard(TouchEvent::kText, utf8); }
void InjectDeleteBackward() { InjectKeyboard(TouchEvent::kDeleteBackward, nullptr); }
void InjectEnter() { InjectKeyboard(TouchEvent::kEnter, nullptr); }
void SetImeListener(void (*listener)(int visible)) { gImeListener = listener; }

// ---- tests ------------------------------------------------------------------------------
//
// Started by the first frame shown:
//   ANDROPKG_TEST_TAP=x,y [+ ANDROPKG_TEST_TYPE=text], ANDROPKG_TEST_SWIPE=x1,y1,x2,y2,
//   ANDROPKG_TEST_BACK=1, ANDROPKG_TEST_BACKGROUND=1 — one action each;
//   ANDROPKG_TEST_SCRIPT="cmd; cmd; ..." — a scenario, with commands
//     wait MS | tap X,Y | swipe X1,Y1,X2,Y2 | type TEXT | del | enter | key CODE | back |
//     accel X,Y,Z | shake (accelerometer samples) | bg | fg |
//     shot PATH (the screen as shown now, as PNG) | log TEXT

void Tap(float x, float y) {
  InjectTouch(0, x, y);
  usleep(80 * 1000);
  InjectTouch(1, x, y);
}

void Swipe(float x, float y, float x2, float y2) {
  InjectTouch(0, x, y);
  constexpr int kSteps = 20;
  for (int i = 1; i <= kSteps; i++) {
    usleep(15 * 1000);
    InjectTouch(2, x + (x2 - x) * i / kSteps, y + (y2 - y) * i / kSteps);
  }
  InjectTouch(1, x2, y2);
}

extern "C" void andropkg_sensor_push(int type, const float* values, int count);

// Accelerometer samples at 50 Hz for `ms`; with `shake`, the x axis swings +-2.5 g.
void PushAccel(float x, float y, float z, int ms, bool shake) {
  for (int t = 0, i = 0; t < ms; t += 20, i++) {
    float v[3] = {shake ? (i % 2 ? 25.f : -25.f) : x, y, z};
    andropkg_sensor_push(1 /* TYPE_ACCELEROMETER */, v, 3);
    usleep(20 * 1000);
  }
}

void RunTestScript(std::string script) {
  usleep(300 * 1000);
  size_t pos = 0;
  while (pos <= script.size()) {
    size_t end = script.find(';', pos);
    if (end == std::string::npos) end = script.size();
    std::string cmd = script.substr(pos, end - pos);
    pos = end + 1;
    size_t b = cmd.find_first_not_of(" \t\n");
    if (b == std::string::npos) continue;
    cmd = cmd.substr(b);
    size_t sp = cmd.find(' ');
    std::string op = cmd.substr(0, sp), arg = sp == std::string::npos ? "" : cmd.substr(sp + 1);
    float x = 0, y = 0, x2 = 0, y2 = 0;
    ALOGI("test: %s", cmd.c_str());
    if (op == "wait") {
      usleep(static_cast<useconds_t>(atoi(arg.c_str())) * 1000);
    } else if (op == "tap" && sscanf(arg.c_str(), "%f,%f", &x, &y) == 2) {
      Tap(x, y);
    } else if (op == "swipe" && sscanf(arg.c_str(), "%f,%f,%f,%f", &x, &y, &x2, &y2) == 4) {
      Swipe(x, y, x2, y2);
    } else if (op == "type") {
      InjectKeyboard(TouchEvent::kText, arg.c_str());
    } else if (op == "del") {
      InjectKeyboard(TouchEvent::kDeleteBackward, nullptr);
    } else if (op == "enter") {
      InjectKeyboard(TouchEvent::kEnter, nullptr);
    } else if (op == "key" || op == "back") {
      int code = op == "back" ? 4 : atoi(arg.c_str());
      InjectKey(0, code);
      usleep(50 * 1000);
      InjectKey(1, code);
    } else if (op == "accel" && sscanf(arg.c_str(), "%f,%f,%f", &x, &y, &x2) == 3) {
      PushAccel(x, y, x2, 400, false);
    } else if (op == "shake") {
      PushAccel(0, 0, 9.81f, 800, true);
    } else if (op == "bg" || op == "fg") {
      SetHostVisible(op == "fg");
    } else if (op == "shot") {
      // A relative path is under $TMPDIR (on iOS: the app container's tmp/).
      std::string path = arg;
      const char* tmp = getenv("TMPDIR");
      if (!path.empty() && path[0] != '/' && tmp) path = std::string(tmp) + "/" + path;
      std::lock_guard<std::mutex> g(gLastFrameLock);
      if (gLastW > 0 && WritePngTo(path.c_str(), gLastFrame.data(), gLastW, gLastH, gLastW * 4)) {
        ALOGI("test: shot %dx%d -> %s", gLastW, gLastH, path.c_str());
      }
    } else if (op == "log") {
      // logged above
    } else {
      ALOGW("test: unknown command '%s'", cmd.c_str());
    }
  }
  ALOGI("test: script done");
}

void MaybeStartTests() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  float x = 0, y = 0, x2 = 0, y2 = 0;
  if (const char* script = getenv("ANDROPKG_TEST_SCRIPT")) {
    std::thread(RunTestScript, std::string(script)).detach();
  }
  if (const char* tap = getenv("ANDROPKG_TEST_TAP"); tap && sscanf(tap, "%f,%f", &x, &y) == 2) {
    std::thread([x, y] {
      usleep(300 * 1000);
      Tap(x, y);
      if (const char* type = getenv("ANDROPKG_TEST_TYPE")) {  // then type into what got focus
        usleep(300 * 1000);
        InjectKeyboard(TouchEvent::kText, type);
      }
    }).detach();
  }
  if (getenv("ANDROPKG_TEST_BACKGROUND")) {  // background, then foreground again
    std::thread([] {
      usleep(400 * 1000);
      SetHostVisible(false);
      usleep(400 * 1000);
      SetHostVisible(true);
    }).detach();
  }
  if (getenv("ANDROPKG_TEST_BACK")) {  // press BACK once
    std::thread([] {
      usleep(500 * 1000);
      InjectKey(0, 4);
      usleep(50 * 1000);
      InjectKey(1, 4);
    }).detach();
  }
  if (const char* sw = getenv("ANDROPKG_TEST_SWIPE"); sw && sscanf(sw, "%f,%f,%f,%f", &x, &y, &x2, &y2) == 4) {
    std::thread([x, y, x2, y2] {
      usleep(300 * 1000);
      Swipe(x, y, x2, y2);
    }).detach();
  }
}

// ---- registration -----------------------------------------------------------------------

/// Registers andropkg_jni_zero for every native method `className` declares.
void ZeroFillNatives(JNIEnv* env, const char* className) {
  jclass c = env->FindClass(className);
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  jclass classClass = env->FindClass("java/lang/Class");
  jclass methodClass = env->FindClass("java/lang/reflect/Method");
  jmethodID getDeclaredMethods = env->GetMethodID(classClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
  jmethodID getModifiers = env->GetMethodID(methodClass, "getModifiers", "()I");
  jmethodID getName = env->GetMethodID(methodClass, "getName", "()Ljava/lang/String;");
  jmethodID getParameterTypes = env->GetMethodID(methodClass, "getParameterTypes", "()[Ljava/lang/Class;");
  jmethodID getReturnType = env->GetMethodID(methodClass, "getReturnType", "()Ljava/lang/Class;");
  jmethodID className_ = env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");

  auto descriptor = [&](jobject type) {
    jstring n = static_cast<jstring>(env->CallObjectMethod(type, className_));
    const char* s = env->GetStringUTFChars(n, nullptr);
    std::string name = s, out;
    env->ReleaseStringUTFChars(n, s);
    env->DeleteLocalRef(n);
    static const std::pair<const char*, const char*> kPrim[] = {
        {"int", "I"}, {"long", "J"}, {"boolean", "Z"}, {"float", "F"}, {"double", "D"},
        {"byte", "B"}, {"char", "C"}, {"short", "S"}, {"void", "V"}};
    for (auto& [k, v] : kPrim) {
      if (name == k) return std::string(v);
    }
    for (char& ch : name) {
      if (ch == '.') ch = '/';
    }
    return name[0] == '[' ? name : "L" + name + ";";
  };

  jobjectArray methods = static_cast<jobjectArray>(env->CallObjectMethod(c, getDeclaredMethods));
  jsize count = methods ? env->GetArrayLength(methods) : 0;
  for (jsize i = 0; i < count; i++) {
    jobject m = env->GetObjectArrayElement(methods, i);
    if ((env->CallIntMethod(m, getModifiers) & 0x100 /* NATIVE */) != 0) {
      std::string sig = "(";
      jobjectArray params = static_cast<jobjectArray>(env->CallObjectMethod(m, getParameterTypes));
      for (jsize k = 0, np = env->GetArrayLength(params); k < np; k++) {
        jobject p = env->GetObjectArrayElement(params, k);
        sig += descriptor(p);
        env->DeleteLocalRef(p);
      }
      env->DeleteLocalRef(params);
      jobject ret = env->CallObjectMethod(m, getReturnType);
      sig += ")" + descriptor(ret);
      env->DeleteLocalRef(ret);
      jstring n = static_cast<jstring>(env->CallObjectMethod(m, getName));
      const char* name = env->GetStringUTFChars(n, nullptr);
      // NativeAllocationRegistry calls whatever a *Finalizer() native returns: never 0.
      bool finalizer = strstr(name, "Finalizer") != nullptr && sig == "()J";
      JNINativeMethod nm = {name, sig.c_str(),
                            finalizer ? reinterpret_cast<void*>(&NoopFinalizerPtr)
                                      : reinterpret_cast<void*>(&andropkg_jni_zero)};
      if (env->RegisterNatives(c, &nm, 1) != JNI_OK) env->ExceptionClear();
      env->ReleaseStringUTFChars(n, name);
      env->DeleteLocalRef(n);
    }
    env->DeleteLocalRef(m);
  }
  if (methods) env->DeleteLocalRef(methods);
  env->DeleteLocalRef(classClass);
  env->DeleteLocalRef(methodClass);
  env->DeleteLocalRef(c);
}

void Register(JNIEnv* env, const char* className, const JNINativeMethod* methods, size_t n) {
  jclass c = env->FindClass(className);
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if (env->RegisterNatives(c, &methods[i], 1) != JNI_OK) {
      env->ExceptionClear();
      ALOGW("andropkg: %s.%s%s not registered", className, methods[i].name, methods[i].signature);
    }
  }
  env->DeleteLocalRef(c);
}

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
#define REGISTER(cls, arr) Register(env, cls, arr, sizeof arr / sizeof arr[0])

}  // namespace

namespace {
// An inert HardwareRenderer: views that insist on one (the text Magnifier, SurfaceView's
// renderers) get a real root RenderNode and a proxy that draws nothing, instead of an
// UnsatisfiedLinkError. Windows themselves draw through the software path.
jclass gRenderNodeClass;
jmethodID gRenderNodeCreate;  // RenderNode.nCreate(String)
jlong hr_createRootRenderNode(JNIEnv* env, jclass) {
  jstring name = env->NewStringUTF("RootRenderNode");
  jlong node = env->CallStaticLongMethod(gRenderNodeClass, gRenderNodeCreate, name);
  env->DeleteLocalRef(name);
  return node;
}
jlong hr_createProxy(JNIEnv*, jclass, jboolean, jlong) {
  static char proxy;  // never dereferenced: every per-renderer native is inert
  return reinterpret_cast<jlong>(&proxy);
}
}  // namespace

namespace android {
int register_android_view_ThreadedRenderer(JNIEnv* env);

int register_andropkg_window(JNIEnv* env) {
  ZeroFillNatives(env, "android/graphics/HardwareRenderer");
  register_android_view_ThreadedRenderer(env);  // the process-wide config natives, as before
  jclass rn = env->FindClass("android/graphics/RenderNode");
  gRenderNodeClass = static_cast<jclass>(env->NewGlobalRef(rn));
  gRenderNodeCreate = env->GetStaticMethodID(rn, "nCreate", "(Ljava/lang/String;)J");
  static const JNINativeMethod kHardwareRenderer[] = {
      M("nCreateRootRenderNode", "()J", hr_createRootRenderNode),
      M("nCreateProxy", "(ZJ)J", hr_createProxy),
  };
  REGISTER("android/graphics/HardwareRenderer", kHardwareRenderer);

  for (const char* inert : {"android/view/SurfaceControl", "android/view/SurfaceSession",
                            "android/graphics/BLASTBufferQueue", "android/view/Surface",
                            "android/view/InputChannel", "android/view/InputEventReceiver",
                            "android/view/InputEventSender", "android/view/DisplayEventReceiver"}) {
    ZeroFillNatives(env, inert);
  }

  jclass surface = env->FindClass("android/view/Surface");
  gJni.surfaceClass = static_cast<jclass>(env->NewGlobalRef(surface));
  gJni.surfaceCtor = env->GetMethodID(surface, "<init>", "(J)V");
  jclass rect = env->FindClass("android/graphics/Rect");
  gJni.rectLeft = env->GetFieldID(rect, "left", "I");
  gJni.rectTop = env->GetFieldID(rect, "top", "I");
  gJni.rectRight = env->GetFieldID(rect, "right", "I");
  gJni.rectBottom = env->GetFieldID(rect, "bottom", "I");

  jclass der = env->FindClass("android/view/DisplayEventReceiver");
  gVsync.dispatchVsync = env->GetMethodID(der, "dispatchVsync", "(JJI)V");
  jclass data = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData");
  gVsync.preferredFrameTimelineIndex = env->GetFieldID(data, "preferredFrameTimelineIndex", "I");
  gVsync.frameTimelinesLength = env->GetFieldID(data, "frameTimelinesLength", "I");
  gVsync.frameInterval = env->GetFieldID(data, "frameInterval", "J");
  gVsync.frameTimelines =
      env->GetFieldID(data, "frameTimelines", "[Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;");
  jclass ft = env->FindClass("android/view/DisplayEventReceiver$VsyncEventData$FrameTimeline");
  gVsync.ftVsyncId = env->GetFieldID(ft, "vsyncId", "J");
  gVsync.ftExpectedPresentationTime = env->GetFieldID(ft, "expectedPresentationTime", "J");
  gVsync.ftDeadline = env->GetFieldID(ft, "deadline", "J");
  gVsync.dataClass = static_cast<jclass>(env->NewGlobalRef(data));
  gVsync.timelineClass = static_cast<jclass>(env->NewGlobalRef(ft));
  gVsync.dataCtor = env->GetMethodID(
      data, "<init>", "([Landroid/view/DisplayEventReceiver$VsyncEventData$FrameTimeline;IIJ)V");
  gVsync.timelineCtor = env->GetMethodID(ft, "<init>", "(JJJ)V");
  jclass ref = env->FindClass("java/lang/ref/Reference");
  gVsync.referenceGet = env->GetMethodID(ref, "get", "()Ljava/lang/Object;");
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    return -1;
  }

  static const JNINativeMethod kSurfaceControl[] = {
      M("nativeCreate", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIIIJLandroid/os/Parcel;)J", sc_create),
      M("nativeGetNativeSurfaceControlFinalizer", "()J", NoopFinalizerPtr),
      M("nativeCopyFromSurfaceControl", "(J)J", sc_copy),
      M("nativeCreateTransaction", "()J", sc_createTransaction),
      M("nativeGetNativeTransactionFinalizer", "()J", NoopFinalizerPtr),
      M("nativeGetHandle", "(J)J", sc_getHandle),
      M("nativeGetLayerId", "(J)I", sc_getLayerId),
  };
  static const JNINativeMethod kSurfaceSession[] = {
      M("nativeCreate", "()J", sc_createTransaction),
  };
  static const JNINativeMethod kBlast[] = {
      M("nativeCreate", "(Ljava/lang/String;Z)J", bbq_create),
      M("nativeGetSurface", "(JZ)Landroid/view/Surface;", bbq_getSurface),
      M("nativeUpdate", "(JJJJI)V", bbq_update),
      M("nativeIsSameSurfaceControl", "(JJ)Z", bbq_isSameSurfaceControl),
  };
  static const JNINativeMethod kSurface[] = {
      M("nativeIsValid", "(J)Z", s_isValid),
      M("nativeGetWidth", "(J)I", s_getWidth),
      M("nativeGetHeight", "(J)I", s_getHeight),
      M("nativeGetNextFrameNumber", "(J)J", s_getNextFrameNumber),
      M("nativeGetFromSurfaceControl", "(JJ)J", s_fromSurfaceControl),
      M("nativeCreateFromSurfaceControl", "(J)J", s_createFromSurfaceControl),
      M("nativeGetFromBlastBufferQueue", "(JJ)J", s_fromBlastBufferQueue),
      M("nativeLockCanvas", "(JLandroid/graphics/Canvas;Landroid/graphics/Rect;)J", s_lockCanvas),
      M("nativeUnlockCanvasAndPost", "(JLandroid/graphics/Canvas;)V", s_unlockCanvasAndPost),
  };
  static const JNINativeMethod kDisplayEventReceiver[] = {
      M("nativeInit", "(Ljava/lang/ref/WeakReference;Ljava/lang/ref/WeakReference;Landroid/os/MessageQueue;IIJ)J",
        der_init),
      M("nativeGetDisplayEventReceiverFinalizer", "()J", NoopFinalizerPtr),
      M("nativeScheduleVsync", "(J)V", der_scheduleVsync),
      M("nativeGetLatestVsyncEventData", "(J)Landroid/view/DisplayEventReceiver$VsyncEventData;",
        der_getLatestVsyncEventData),
  };
  REGISTER("android/view/SurfaceControl", kSurfaceControl);
  REGISTER("android/view/SurfaceSession", kSurfaceSession);
  REGISTER("android/graphics/BLASTBufferQueue", kBlast);
  REGISTER("android/view/Surface", kSurface);
  REGISTER("android/view/DisplayEventReceiver", kDisplayEventReceiver);

  jclass me = env->FindClass("android/view/MotionEvent");
  gInput.motionEventClass = static_cast<jclass>(env->NewGlobalRef(me));
  gInput.obtain = env->GetStaticMethodID(me, "obtain", "(JJIFFI)Landroid/view/MotionEvent;");
  gInput.setSource = env->GetMethodID(me, "setSource", "(I)V");
  gInput.recycle = env->GetMethodID(me, "recycle", "()V");
  jclass ke = env->FindClass("android/view/KeyEvent");
  gInput.keyEventClass = static_cast<jclass>(env->NewGlobalRef(ke));
  gInput.keyEventCtor = env->GetMethodID(ke, "<init>", "(JJIII)V");
  gInput.keySetSource = env->GetMethodID(ke, "setSource", "(I)V");
  jclass ier = env->FindClass("android/view/InputEventReceiver");
  gInput.dispatchInputEvent = env->GetMethodID(ier, "dispatchInputEvent", "(ILandroid/view/InputEvent;)V");
  gInput.onFocusEvent = env->GetMethodID(ier, "onFocusEvent", "(Z)V");
  gInput.offsetLocation = env->GetMethodID(me, "offsetLocation", "(FF)V");
  gInput.channelPtr = env->GetFieldID(env->FindClass("android/view/InputChannel"), "mPtr", "J");
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    return -1;
  }
  static const JNINativeMethod kInputEventReceiver[] = {
      M("nativeInit", "(Ljava/lang/ref/WeakReference;Landroid/view/InputChannel;Landroid/os/MessageQueue;)J",
        ier_init),
      M("nativeDispose", "(J)V", ier_dispose),
  };
  REGISTER("android/view/InputEventReceiver", kInputEventReceiver);
  return 0;
}
}  // namespace android
