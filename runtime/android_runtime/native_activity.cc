// andropkg: android.app.NativeActivity natives (ANativeActivity for NDK apps).
//
// After AOSP's core/jni/android_app_NativeActivity.cpp, with two differences: the app library
// is an Android ELF loaded by the NDK layer (libandropkg_ndk: andropkg_ndk_open/sym), and the
// ANativeWindow handed to the app is the window's own surface (window_jni.cc's ANativeWindow_*).
// Callbacks into the app are plain non-variadic C calls, so the AAPCS64/Darwin difference
// doesn't matter; the JNIEnv the app gets is the usual one (its table is the AAPCS64-safe
// override for Android callers).
#define LOG_TAG "NativeActivity"
#include <android/native_activity.h>
#include <android/native_window_jni.h>
#include <android_runtime/android_app_NativeActivity.h>
#include <android_runtime/android_util_AssetManager.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedUtfChars.h>
#include <poll.h>
#include <unistd.h>
#include <utils/Log.h>

#include <string>

#include "android_os_MessageQueue.h"
#include "core_jni_helpers.h"

namespace android {
namespace {

struct {
  jmethodID finish, setWindowFlags, setWindowFormat, showIme, hideIme;
} gNA;

struct Work {
  int32_t cmd, arg1, arg2;
};
enum { CMD_FINISH = 1, CMD_SET_WINDOW_FORMAT, CMD_SET_WINDOW_FLAGS, CMD_SHOW_SOFT_INPUT, CMD_HIDE_SOFT_INPUT };

struct NativeCode : public ANativeActivity {
  NativeCode() {
    memset(static_cast<ANativeActivity*>(this), 0, sizeof(ANativeActivity));
    memset(&cbs, 0, sizeof cbs);
  }
  ANativeActivityCallbacks cbs;
  std::string internalPath, externalPath, obbPathStr;
  ANativeWindow* window = nullptr;
  int32_t lastW = 0, lastH = 0;
  int workRead = -1, workWrite = -1;
  sp<MessageQueue> queue;
  jobject javaAssets = nullptr;
};

void WriteWork(int fd, int32_t cmd, int32_t a1 = 0, int32_t a2 = 0) {
  Work w{cmd, a1, a2};
  ssize_t r;
  do r = write(fd, &w, sizeof w);
  while (r < 0 && errno == EINTR);
  if (r != sizeof w) ALOGW("NativeActivity: writing work: %s", r < 0 ? strerror(errno) : "truncated");
}

int WorkCallback(int fd, int events, void* data) {
  auto* code = static_cast<NativeCode*>(data);
  if ((events & POLLIN) == 0) return 1;
  Work w;
  if (read(fd, &w, sizeof w) != sizeof w) return 1;
  JNIEnv* env = code->env;
  switch (w.cmd) {
    case CMD_FINISH: env->CallVoidMethod(code->clazz, gNA.finish); break;
    case CMD_SET_WINDOW_FORMAT: env->CallVoidMethod(code->clazz, gNA.setWindowFormat, w.arg1); break;
    case CMD_SET_WINDOW_FLAGS: env->CallVoidMethod(code->clazz, gNA.setWindowFlags, w.arg1, w.arg2); break;
    case CMD_SHOW_SOFT_INPUT: env->CallVoidMethod(code->clazz, gNA.showIme, w.arg1); break;
    case CMD_HIDE_SOFT_INPUT: env->CallVoidMethod(code->clazz, gNA.hideIme, w.arg1); break;
  }
  code->queue->raiseAndClearException(env, "NativeActivity work");
  return 1;
}

// The NDK layer (the same entry points ART uses for System.loadLibrary of an Android ELF).
struct Ndk {
  void* (*open)(const char*, char**);
  void* (*sym)(void*, const char*);
};
const Ndk* GetNdk() {
  static Ndk ndk = [] {
    Ndk n{};
    void* h = dlopen("@rpath/libandropkg_ndk.dylib", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
      n.open = reinterpret_cast<void* (*)(const char*, char**)>(dlsym(h, "andropkg_ndk_open"));
      n.sym = reinterpret_cast<void* (*)(void*, const char*)>(dlsym(h, "andropkg_ndk_sym"));
    }
    return n;
  }();
  return ndk.open && ndk.sym ? &ndk : nullptr;
}

thread_local std::string gError;

std::string Utf(JNIEnv* env, jstring s) {
  if (s == nullptr) return {};
  ScopedUtfChars c(env, s);
  return c.c_str() ? c.c_str() : "";
}

jlong loadNativeCode(JNIEnv* env, jobject clazz, jstring path, jstring funcName, jobject messageQueue,
                     jstring internalDataDir, jstring obbDir, jstring externalDataDir, jint sdkVersion,
                     jobject jAssetMgr, jbyteArray savedState, jobject /*classLoader*/, jstring /*libPath*/) {
  std::string libPath = Utf(env, path), func = Utf(env, funcName);
  const Ndk* ndk = GetNdk();
  if (ndk == nullptr) {
    gError = "libandropkg_ndk.dylib unavailable";
    return 0;
  }
  char* err = nullptr;
  void* handle = ndk->open(libPath.c_str(), &err);
  if (handle == nullptr) {
    gError = err ? err : "load failed";
    free(err);
    ALOGW("NativeActivity: loading %s: %s", libPath.c_str(), gError.c_str());
    return 0;
  }
  auto create = reinterpret_cast<ANativeActivity_createFunc*>(ndk->sym(handle, func.c_str()));
  if (create == nullptr) {
    gError = func + " not found in " + libPath;
    ALOGW("NativeActivity: %s", gError.c_str());
    return 0;
  }
  auto* code = new NativeCode;
  code->queue = android_os_MessageQueue_getMessageQueue(env, messageQueue);
  int fds[2];
  if (code->queue == nullptr || pipe(fds) != 0) {
    gError = "no message queue / pipe";
    delete code;
    return 0;
  }
  code->workRead = fds[0];
  code->workWrite = fds[1];
  fcntl(code->workRead, F_SETFL, O_NONBLOCK);
  fcntl(code->workWrite, F_SETFL, O_NONBLOCK);
  code->queue->getLooper()->addFd(code->workRead, 0, ALOOPER_EVENT_INPUT, WorkCallback, code);

  code->callbacks = &code->cbs;
  env->GetJavaVM(&code->vm);
  code->env = env;
  code->clazz = env->NewGlobalRef(clazz);
  code->internalPath = Utf(env, internalDataDir);
  code->externalPath = Utf(env, externalDataDir);
  code->obbPathStr = Utf(env, obbDir);
  code->internalDataPath = code->internalPath.c_str();
  code->externalDataPath = code->externalPath.c_str();
  code->obbPath = code->obbPathStr.c_str();
  code->sdkVersion = sdkVersion;
  code->javaAssets = env->NewGlobalRef(jAssetMgr);
  code->assetManager = NdkAssetManagerForJavaObject(env, jAssetMgr);

  jbyte* saved = savedState ? env->GetByteArrayElements(savedState, nullptr) : nullptr;
  jsize savedSize = savedState ? env->GetArrayLength(savedState) : 0;
  ALOGI("NativeActivity: %s -> %s", libPath.c_str(), func.c_str());
  create(code, saved, static_cast<size_t>(savedSize));
  if (saved) env->ReleaseByteArrayElements(savedState, saved, 0);
  return reinterpret_cast<jlong>(code);
}

jstring getDlError(JNIEnv* env, jobject) {
  jstring s = env->NewStringUTF(gError.c_str());
  gError.clear();
  return s;
}

NativeCode* C(jlong h) { return reinterpret_cast<NativeCode*>(h); }

void unloadNativeCode(JNIEnv* env, jobject, jlong h) {
  NativeCode* code = C(h);
  if (code == nullptr) return;
  if (code->cbs.onDestroy) code->cbs.onDestroy(code);
  code->queue->getLooper()->removeFd(code->workRead);
  close(code->workRead);
  close(code->workWrite);
  env->DeleteGlobalRef(code->clazz);
  env->DeleteGlobalRef(code->javaAssets);
  delete code;  // the library stays loaded (as on Android)
}

#define SIMPLE(name, cb)                                         \
  void name(JNIEnv*, jobject, jlong h) {                         \
    if (NativeCode* c = C(h); c && c->cbs.cb) c->cbs.cb(c);      \
  }
SIMPLE(onStart, onStart)
SIMPLE(onResume, onResume)
SIMPLE(onPause, onPause)
SIMPLE(onStop, onStop)
SIMPLE(onConfigurationChanged, onConfigurationChanged)
SIMPLE(onLowMemory, onLowMemory)

jbyteArray onSaveInstanceState(JNIEnv* env, jobject, jlong h) {
  NativeCode* c = C(h);
  if (c == nullptr || c->cbs.onSaveInstanceState == nullptr) return nullptr;
  size_t len = 0;
  jbyte* state = static_cast<jbyte*>(c->cbs.onSaveInstanceState(c, &len));
  jbyteArray arr = nullptr;
  if (state && len > 0) {
    arr = env->NewByteArray(static_cast<jsize>(len));
    if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len), state);
  }
  free(state);  // the app malloc'd it (bionic malloc is the system malloc)
  return arr;
}

void onWindowFocusChanged(JNIEnv*, jobject, jlong h, jboolean focused) {
  if (NativeCode* c = C(h); c && c->cbs.onWindowFocusChanged) c->cbs.onWindowFocusChanged(c, focused ? 1 : 0);
}

void onSurfaceCreated(JNIEnv* env, jobject, jlong h, jobject surface) {
  NativeCode* c = C(h);
  if (c == nullptr) return;
  c->window = ANativeWindow_fromSurface(env, surface);
  if (c->window && c->cbs.onNativeWindowCreated) c->cbs.onNativeWindowCreated(c, c->window);
}

void onSurfaceChanged(JNIEnv* env, jobject, jlong h, jobject surface, jint, jint, jint) {
  NativeCode* c = C(h);
  if (c == nullptr) return;
  ANativeWindow* old = c->window;
  c->window = ANativeWindow_fromSurface(env, surface);
  if (old != c->window) {
    if (old && c->cbs.onNativeWindowDestroyed) c->cbs.onNativeWindowDestroyed(c, old);
    if (c->window) {
      if (c->cbs.onNativeWindowCreated) c->cbs.onNativeWindowCreated(c, c->window);
      c->lastW = ANativeWindow_getWidth(c->window);
      c->lastH = ANativeWindow_getHeight(c->window);
    }
  } else if (c->window) {
    int32_t w = ANativeWindow_getWidth(c->window), hh = ANativeWindow_getHeight(c->window);
    if ((w != c->lastW || hh != c->lastH) && c->cbs.onNativeWindowResized) c->cbs.onNativeWindowResized(c, c->window);
    c->lastW = w;
    c->lastH = hh;
  }
}

void onSurfaceRedrawNeeded(JNIEnv*, jobject, jlong h, jobject) {
  NativeCode* c = C(h);
  if (c && c->window && c->cbs.onNativeWindowRedrawNeeded) c->cbs.onNativeWindowRedrawNeeded(c, c->window);
}

void onSurfaceDestroyed(JNIEnv*, jobject, jlong h) {
  NativeCode* c = C(h);
  if (c == nullptr) return;
  if (c->window && c->cbs.onNativeWindowDestroyed) c->cbs.onNativeWindowDestroyed(c, c->window);
  c->window = nullptr;
}

void onInputQueueCreated(JNIEnv*, jobject, jlong h, jlong q) {
  NativeCode* c = C(h);
  if (c && c->cbs.onInputQueueCreated) c->cbs.onInputQueueCreated(c, reinterpret_cast<AInputQueue*>(q));
}
void onInputQueueDestroyed(JNIEnv*, jobject, jlong h, jlong q) {
  NativeCode* c = C(h);
  if (c && c->cbs.onInputQueueDestroyed) c->cbs.onInputQueueDestroyed(c, reinterpret_cast<AInputQueue*>(q));
}

void onContentRectChanged(JNIEnv*, jobject, jlong h, jint x, jint y, jint w, jint hh) {
  NativeCode* c = C(h);
  if (c && c->cbs.onContentRectChanged) {
    ARect r{x, y, x + w, y + hh};
    c->cbs.onContentRectChanged(c, &r);
  }
}

const JNINativeMethod kMethods[] = {
    {"loadNativeCode",
     "(Ljava/lang/String;Ljava/lang/String;Landroid/os/MessageQueue;Ljava/lang/String;Ljava/lang/String;"
     "Ljava/lang/String;ILandroid/content/res/AssetManager;[BLjava/lang/ClassLoader;Ljava/lang/String;)J",
     reinterpret_cast<void*>(loadNativeCode)},
    {"getDlError", "()Ljava/lang/String;", reinterpret_cast<void*>(getDlError)},
    {"unloadNativeCode", "(J)V", reinterpret_cast<void*>(unloadNativeCode)},
    {"onStartNative", "(J)V", reinterpret_cast<void*>(onStart)},
    {"onResumeNative", "(J)V", reinterpret_cast<void*>(onResume)},
    {"onSaveInstanceStateNative", "(J)[B", reinterpret_cast<void*>(onSaveInstanceState)},
    {"onPauseNative", "(J)V", reinterpret_cast<void*>(onPause)},
    {"onStopNative", "(J)V", reinterpret_cast<void*>(onStop)},
    {"onConfigurationChangedNative", "(J)V", reinterpret_cast<void*>(onConfigurationChanged)},
    {"onLowMemoryNative", "(J)V", reinterpret_cast<void*>(onLowMemory)},
    {"onWindowFocusChangedNative", "(JZ)V", reinterpret_cast<void*>(onWindowFocusChanged)},
    {"onSurfaceCreatedNative", "(JLandroid/view/Surface;)V", reinterpret_cast<void*>(onSurfaceCreated)},
    {"onSurfaceChangedNative", "(JLandroid/view/Surface;III)V", reinterpret_cast<void*>(onSurfaceChanged)},
    {"onSurfaceRedrawNeededNative", "(JLandroid/view/Surface;)V", reinterpret_cast<void*>(onSurfaceRedrawNeeded)},
    {"onSurfaceDestroyedNative", "(J)V", reinterpret_cast<void*>(onSurfaceDestroyed)},
    {"onInputQueueCreatedNative", "(JJ)V", reinterpret_cast<void*>(onInputQueueCreated)},
    {"onInputQueueDestroyedNative", "(JJ)V", reinterpret_cast<void*>(onInputQueueDestroyed)},
    {"onContentRectChangedNative", "(JIIII)V", reinterpret_cast<void*>(onContentRectChanged)},
};

}  // namespace

void android_NativeActivity_finish(ANativeActivity* a) {
  WriteWork(static_cast<NativeCode*>(a)->workWrite, CMD_FINISH);
}
void android_NativeActivity_setWindowFormat(ANativeActivity* a, int32_t format) {
  WriteWork(static_cast<NativeCode*>(a)->workWrite, CMD_SET_WINDOW_FORMAT, format);
}
void android_NativeActivity_setWindowFlags(ANativeActivity* a, int32_t values, int32_t mask) {
  WriteWork(static_cast<NativeCode*>(a)->workWrite, CMD_SET_WINDOW_FLAGS, values, mask);
}
void android_NativeActivity_showSoftInput(ANativeActivity* a, int32_t flags) {
  WriteWork(static_cast<NativeCode*>(a)->workWrite, CMD_SHOW_SOFT_INPUT, flags);
}
void android_NativeActivity_hideSoftInput(ANativeActivity* a, int32_t flags) {
  WriteWork(static_cast<NativeCode*>(a)->workWrite, CMD_HIDE_SOFT_INPUT, flags);
}

}  // namespace android

// libandroid's ANativeActivity_* (native/android/native_activity.cpp).
#define EXPORT extern "C" __attribute__((visibility("default")))
EXPORT void ANativeActivity_finish(ANativeActivity* a) { android::android_NativeActivity_finish(a); }
EXPORT void ANativeActivity_setWindowFormat(ANativeActivity* a, int32_t f) {
  android::android_NativeActivity_setWindowFormat(a, f);
}
EXPORT void ANativeActivity_setWindowFlags(ANativeActivity* a, uint32_t add, uint32_t remove) {
  android::android_NativeActivity_setWindowFlags(a, static_cast<int32_t>(add), static_cast<int32_t>(add | remove));
}
EXPORT void ANativeActivity_showSoftInput(ANativeActivity* a, uint32_t flags) {
  android::android_NativeActivity_showSoftInput(a, static_cast<int32_t>(flags));
}
EXPORT void ANativeActivity_hideSoftInput(ANativeActivity* a, uint32_t flags) {
  android::android_NativeActivity_hideSoftInput(a, static_cast<int32_t>(flags));
}

namespace android {
int register_andropkg_native_activity(JNIEnv* env) {
  jclass c = FindClassOrDie(env, "android/app/NativeActivity");
  gNA.finish = GetMethodIDOrDie(env, c, "finish", "()V");
  gNA.setWindowFlags = GetMethodIDOrDie(env, c, "setWindowFlags", "(II)V");
  gNA.setWindowFormat = GetMethodIDOrDie(env, c, "setWindowFormat", "(I)V");
  gNA.showIme = GetMethodIDOrDie(env, c, "showIme", "(I)V");
  gNA.hideIme = GetMethodIDOrDie(env, c, "hideIme", "(I)V");
  return RegisterMethodsOrDie(env, "android/app/NativeActivity", kMethods, NELEM(kMethods));
}
}  // namespace android
