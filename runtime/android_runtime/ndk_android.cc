// andropkg: NDK platform APIs (libandroid / libjnigraphics) implemented in the runtime.
//
// Android code calls these by name (ndk-libs resolves libandroid/libjnigraphics/libnativewindow
// symbols to the runtime's): plain C functions, so the Darwin build has the right ABI. AAsset*
// and AConfiguration* are AOSP's own (frameworks/base/native/android); this file adds the
// rest that the framework pieces here can back.
#ifndef __REMOVED_IN
#define __REMOVED_IN(api)  // bionic's availability macros (sys/cdefs.h) aren't in Darwin's
#endif
#ifndef __INTRODUCED_IN
#define __INTRODUCED_IN(api)
#endif
#include <android/bitmap.h>
#include <android/graphics/bitmap.h>
#include <android/looper.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utils/Looper.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

using android::Looper;
using android::sp;

// ---- ALooper -------------------------------------------------------------------------------------

static Looper* L(ALooper* l) { return reinterpret_cast<Looper*>(l); }

EXPORT ALooper* ALooper_forThread() { return reinterpret_cast<ALooper*>(Looper::getForThread().get()); }
EXPORT ALooper* ALooper_prepare(int opts) {
  return reinterpret_cast<ALooper*>(Looper::prepare(opts).get());
}
EXPORT void ALooper_acquire(ALooper* l) { L(l)->incStrong(reinterpret_cast<void*>(ALooper_acquire)); }
EXPORT void ALooper_release(ALooper* l) { L(l)->decStrong(reinterpret_cast<void*>(ALooper_acquire)); }
EXPORT int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
  sp<Looper> looper = Looper::getForThread();
  if (looper == nullptr) return ALOOPER_POLL_ERROR;
  return looper->pollOnce(timeoutMillis, outFd, outEvents, outData);
}
EXPORT int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
  sp<Looper> looper = Looper::getForThread();
  if (looper == nullptr) return ALOOPER_POLL_ERROR;
  return looper->pollAll(timeoutMillis, outFd, outEvents, outData);
}
EXPORT void ALooper_wake(ALooper* l) { L(l)->wake(); }
EXPORT int ALooper_addFd(ALooper* l, int fd, int ident, int events, ALooper_callbackFunc callback, void* data) {
  return L(l)->addFd(fd, ident, events, callback, data);
}
EXPORT int ALooper_removeFd(ALooper* l, int fd) { return L(l)->removeFd(fd); }

// ---- AndroidBitmap (libjnigraphics) over hwui's ABitmap ------------------------------------------

EXPORT int AndroidBitmap_getInfo(JNIEnv* env, jobject jbitmap, AndroidBitmapInfo* info) {
  if (env == nullptr || jbitmap == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  ABitmap* b = ABitmap_acquireBitmapFromJava(env, jbitmap);
  if (b == nullptr) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  if (info) *info = ABitmap_getInfo(b);
  ABitmap_releaseRef(b);
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

EXPORT int32_t AndroidBitmap_getDataSpace(JNIEnv* env, jobject jbitmap) {
  ABitmap* b = env && jbitmap ? ABitmap_acquireBitmapFromJava(env, jbitmap) : nullptr;
  if (b == nullptr) return 0;  // ADATASPACE_UNKNOWN
  int32_t ds = ABitmap_getDataSpace(b);
  ABitmap_releaseRef(b);
  return ds;
}

EXPORT int AndroidBitmap_lockPixels(JNIEnv* env, jobject jbitmap, void** addr) {
  if (env == nullptr || jbitmap == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  ABitmap* b = ABitmap_acquireBitmapFromJava(env, jbitmap);  // the reference is the lock
  void* pixels = b ? ABitmap_getPixels(b) : nullptr;
  if (pixels == nullptr) {
    if (b) ABitmap_releaseRef(b);
    return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  }
  if (addr) *addr = pixels;
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

EXPORT int AndroidBitmap_unlockPixels(JNIEnv* env, jobject jbitmap) {
  if (env == nullptr || jbitmap == nullptr) return ANDROID_BITMAP_RESULT_BAD_PARAMETER;
  ABitmap* b = ABitmap_acquireBitmapFromJava(env, jbitmap);
  if (b == nullptr) return ANDROID_BITMAP_RESULT_JNI_EXCEPTION;
  ABitmap_notifyPixelsChanged(b);
  ABitmap_releaseRef(b);  // this call's reference
  ABitmap_releaseRef(b);  // the lock's
  return ANDROID_BITMAP_RESULT_SUCCESS;
}

EXPORT int AndroidBitmap_getHardwareBuffer(JNIEnv*, jobject, AHardwareBuffer** out) {
  if (out) *out = nullptr;
  return ANDROID_BITMAP_RESULT_BAD_PARAMETER;  // no hardware bitmaps (software rendering)
}

// ---- ATrace (libandroid): no systrace here ---------------------------------------------------

EXPORT void ATrace_beginSection(const char*) {}
EXPORT void ATrace_endSection() {}
EXPORT bool ATrace_isEnabled() { return false; }
EXPORT void ATrace_beginAsyncSection(const char*, int32_t) {}
EXPORT void ATrace_endAsyncSection(const char*, int32_t) {}
EXPORT void ATrace_setCounter(const char*, int64_t) {}

// ---- ASharedMemory: an unlinked temporary file stands in for ashmem -----------------------------

EXPORT int ASharedMemory_create(const char*, size_t size) {
  char path[1024];
  const char* tmp = getenv("TMPDIR");
  snprintf(path, sizeof path, "%s/andropkg-ashmem-XXXXXX", tmp ? tmp : "/tmp");
  int fd = mkstemp(path);
  if (fd < 0) return -1;
  unlink(path);
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
EXPORT size_t ASharedMemory_getSize(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
}
EXPORT int ASharedMemory_setProt(int, int) { return 0; }
EXPORT int ASharedMemory_dupFromJava(JNIEnv* env, jobject sharedMemory) {
  jclass c = env->GetObjectClass(sharedMemory);
  jmethodID getFd = env->GetMethodID(c, "getFd", "()I");
  env->DeleteLocalRef(c);
  if (getFd == nullptr) {
    env->ExceptionClear();
    return -1;
  }
  int fd = env->CallIntMethodA(sharedMemory, getFd, nullptr);
  return fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
}
