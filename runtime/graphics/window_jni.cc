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

#include <atomic>
#include <deque>
#include <thread>
#include <mutex>
#include <string>
#include <vector>

#include "android_os_MessageQueue.h"
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

namespace { void InjectTouch(int action, float x, float y); }

/// Called by the host: a touch at window pixel (x, y). action is MotionEvent's
/// (0 down, 1 up, 2 move, 3 cancel). Delivered on the focused window's Looper.
extern "C" __attribute__((visibility("default"))) void andropkg_inject_touch(int action, float x, float y) {
  InjectTouch(action, x, y);
}

namespace {

void WritePng(const void* rgba, int w, int h, int stride) {
  const char* path = getenv("ANDROPKG_FRAME_PNG");
  if (path == nullptr) return;
  SkPixmap pm(SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType), rgba,
              static_cast<size_t>(stride));
  SkFILEWStream out(path);
  if (out.isValid() && SkPngEncoder::Encode(&out, pm, {})) ALOGI("andropkg: frame %dx%d -> %s", w, h, path);
}

// ---- windows ---------------------------------------------------------------------------

struct WindowSurface {
  std::mutex lock;
  int width = 0, height = 0;
  std::vector<uint32_t> pixels;
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

void MaybeTestTap(uint64_t frame);

void NoopFinalizer(void*) {}
jlong NoopFinalizerPtr(JNIEnv*, jclass) { return reinterpret_cast<jlong>(&NoopFinalizer); }

struct {
  jclass surfaceClass;
  jmethodID surfaceCtor;
  jfieldID rectLeft, rectTop, rectRight, rectBottom;
} gJni;

// SurfaceControl
jlong sc_create(JNIEnv* env, jclass, jobject /*session*/, jstring name, jint w, jint h, jint /*format*/,
                jint /*flags*/, jlong /*parent*/, jobject /*metadata*/) {
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
void bbq_update(JNIEnv*, jclass, jlong ptr, jlong /*sc*/, jlong w, jlong h, jint /*format*/) {
  WindowSurface* s = Ptr<Bbq>(ptr)->surface;
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
  MaybeTestTap(s->frames);
  int stride = s->width * 4;
  if (FrameSink sink = gSink.load()) {
    sink(s->pixels.data(), s->width, s->height, stride, gSinkCtx);
  } else {
    WritePng(s->pixels.data(), s->width, s->height, stride);
  }
  s->lock.unlock();
}

// ---- vsync (Choreographer) ---------------------------------------------------------------

constexpr nsecs_t kVsyncPeriod = 1000000000LL / 60;

struct {
  jmethodID dispatchVsync;
  jfieldID preferredFrameTimelineIndex, frameTimelinesLength, frameInterval, frameTimelines;
  jfieldID ftVsyncId, ftExpectedPresentationTime, ftDeadline;
  jmethodID referenceGet;
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

// ---- input (M3): host touches -> the focused window's InputEventReceiver ------------------

struct {
  jclass motionEventClass;
  jmethodID obtain;        // MotionEvent.obtain(long downTime, long eventTime, int action, float x, float y, int meta)
  jmethodID setSource;
  jmethodID recycle;
  jmethodID dispatchInputEvent;  // InputEventReceiver.dispatchInputEvent(int seq, InputEvent)
} gInput;

struct TouchEvent {
  int action;
  float x, y;
};

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
      jlong now = systemTime(SYSTEM_TIME_MONOTONIC) / 1000000;  // SystemClock.uptimeMillis()
      if (e.action == 0 /* ACTION_DOWN */) down_time_ = now;
      jobject ev = env->CallStaticObjectMethod(gInput.motionEventClass, gInput.obtain, down_time_, now, e.action,
                                               e.x, e.y, 0);
      env->CallVoidMethod(ev, gInput.setSource, 0x1002 /* InputDevice.SOURCE_TOUCHSCREEN */);
      jobject receiver = env->CallObjectMethod(receiver_, gVsync.referenceGet);
      if (receiver) {
        env->CallVoidMethod(receiver, gInput.dispatchInputEvent, ++seq_, ev);
        env->DeleteLocalRef(receiver);
      }
      env->DeleteLocalRef(ev);
      queue_->raiseAndClearException(env, "dispatchInputEvent");
    }
  }

 private:
  jobject receiver_;
  android::sp<android::MessageQueue> queue_;
  std::mutex lock_;
  std::deque<TouchEvent> pending_;
  jlong down_time_ = 0;
  jint seq_ = 0;
};

std::mutex gReceiversLock;
std::vector<InputReceiver*> gReceivers;  // most recent last: the focused window

jlong ier_init(JNIEnv* env, jclass, jobject receiverWeak, jobject /*inputChannel*/, jobject messageQueue) {
  android::sp<android::MessageQueue> mq = android::android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (mq == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  auto* r = new InputReceiver(env, receiverWeak, mq);
  r->incStrong(nullptr);
  std::lock_guard<std::mutex> g(gReceiversLock);
  gReceivers.push_back(r);
  return reinterpret_cast<jlong>(r);
}

void ier_dispose(JNIEnv*, jclass, jlong ptr) {
  std::lock_guard<std::mutex> g(gReceiversLock);
  for (auto it = gReceivers.begin(); it != gReceivers.end(); ++it) {
    if (reinterpret_cast<jlong>(*it) == ptr) {
      gReceivers.erase(it);
      break;
    }
  }
}

void InjectTouch(int action, float x, float y) {
  std::lock_guard<std::mutex> g(gReceiversLock);
  if (!gReceivers.empty()) gReceivers.back()->Post({action, x, y});
}

// Tests: ANDROPKG_TEST_TAP=x,y taps once, ANDROPKG_TEST_SWIPE=x1,y1,x2,y2 drags (300 ms),
// shortly after the first frame.
void MaybeTestTap(uint64_t frame) {
  if (frame != 1) return;
  float x = 0, y = 0, x2 = 0, y2 = 0;
  if (const char* tap = getenv("ANDROPKG_TEST_TAP"); tap && sscanf(tap, "%f,%f", &x, &y) == 2) {
    std::thread([x, y] {
      usleep(300 * 1000);
      InjectTouch(0, x, y);
      usleep(80 * 1000);
      InjectTouch(1, x, y);
    }).detach();
  }
  if (const char* sw = getenv("ANDROPKG_TEST_SWIPE"); sw && sscanf(sw, "%f,%f,%f,%f", &x, &y, &x2, &y2) == 4) {
    std::thread([x, y, x2, y2] {
      usleep(300 * 1000);
      InjectTouch(0, x, y);
      constexpr int kSteps = 20;
      for (int i = 1; i <= kSteps; i++) {
        usleep(15 * 1000);
        InjectTouch(2, x + (x2 - x) * i / kSteps, y + (y2 - y) * i / kSteps);
      }
      InjectTouch(1, x2, y2);
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

namespace android {
int register_andropkg_window(JNIEnv* env) {
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
  jclass ier = env->FindClass("android/view/InputEventReceiver");
  gInput.dispatchInputEvent = env->GetMethodID(ier, "dispatchInputEvent", "(ILandroid/view/InputEvent;)V");
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
