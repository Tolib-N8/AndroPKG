// andropkg: framework natives whose subsystem doesn't exist in a single iOS process. Each
// answers the way a device without that feature would.
#include <errno.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/event.h>
#include <unistd.h>

#include "jni_util.h"

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
#define N(arr) static_cast<int>(sizeof arr / sizeof arr[0])

namespace {

// android.os.GraphicsEnvironment: GPU driver/layer selection (libgraphicsenv). There's one
// driver (the host's), no debug layers, no ANGLE switching.
jboolean ge_isDebuggable(JNIEnv*, jclass) { return JNI_FALSE; }
void ge_SS(JNIEnv*, jclass, jstring, jstring) {}
void ge_setGpuStats(JNIEnv*, jclass, jstring, jstring, jlong, jlong, jstring, jint) {}
jboolean ge_setInjectLayersPrSetDumpable(JNIEnv*, jclass) { return JNI_FALSE; }
void ge_setAngleInfo(JNIEnv*, jclass, jstring, jboolean, jstring, jobjectArray) {}
void ge_setLayerPaths(JNIEnv*, jclass, jobject, jstring) {}
void ge_S(JNIEnv*, jclass, jstring) {}
void ge_V(JNIEnv*, jclass) {}
void ge_Z(JNIEnv*, jclass, jboolean) {}

const JNINativeMethod kGraphicsEnvironment[] = {
    M("isDebuggable", "()Z", ge_isDebuggable),
    M("setDriverPathAndSphalLibraries", "(Ljava/lang/String;Ljava/lang/String;)V", ge_SS),
    M("setGpuStats", "(Ljava/lang/String;Ljava/lang/String;JJLjava/lang/String;I)V", ge_setGpuStats),
    M("setInjectLayersPrSetDumpable", "()Z", ge_setInjectLayersPrSetDumpable),
    M("nativeSetAngleInfo", "(Ljava/lang/String;ZLjava/lang/String;[Ljava/lang/String;)V", ge_setAngleInfo),
    M("setLayerPaths", "(Ljava/lang/ClassLoader;Ljava/lang/String;)V", ge_setLayerPaths),
    M("setDebugLayers", "(Ljava/lang/String;)V", ge_S),
    M("setDebugLayersGLES", "(Ljava/lang/String;)V", ge_S),
    M("hintActivityLaunch", "()V", ge_V),
    M("nativeToggleAngleAsSystemDriver", "(Z)V", ge_Z),
};

// android.app.ActivityThread: heap profiling after a zygote fork (there is no zygote) and the
// purge of hwui caches between activities (nothing cached outside the renderer).
const JNINativeMethod kActivityThread[] = {
    M("nPurgePendingResources", "()V", ge_V),
    M("nInitZygoteChildHeapProfiling", "()V", ge_V),
};

// android.os.FileObserver: inotify on Linux, kqueue vnode events here. kqueue reports changes
// to a watched file or directory but not which entry changed, so events carry a null path.
int fo_init(JNIEnv*, jobject) { return kqueue(); }

void fo_observe(JNIEnv* env, jobject thiz, jint kq) {
  jclass c = env->GetObjectClass(thiz);
  jmethodID onEvent = env->GetMethodID(c, "onEvent", "(IIILjava/lang/String;)V");
  env->DeleteLocalRef(c);
  struct kevent ev[16];
  for (;;) {
    int n = kevent(kq, nullptr, 0, ev, 16, nullptr);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    for (int i = 0; i < n; i++) {
      unsigned f = ev[i].fflags;
      int mask = 0;
      if (f & (NOTE_WRITE | NOTE_EXTEND)) mask |= 0x2;  // MODIFY
      if (f & NOTE_ATTRIB) mask |= 0x4;                 // ATTRIB
      if (f & NOTE_DELETE) mask |= 0x400;               // DELETE_SELF
      if (f & NOTE_RENAME) mask |= 0x800;               // MOVE_SELF
      if (mask == 0) continue;
      env->CallVoidMethod(thiz, onEvent, static_cast<jint>(ev[i].ident), mask, 0, nullptr);
      if (env->ExceptionCheck()) env->ExceptionClear();
    }
  }
}

void fo_startWatching(JNIEnv* env, jobject, jint kq, jobjectArray paths, jint, jintArray wfds) {
  for (jsize i = 0, n = env->GetArrayLength(paths); i < n; i++) {
    jint wfd = -1;
    if (auto js = static_cast<jstring>(env->GetObjectArrayElement(paths, i))) {
      const char* p = env->GetStringUTFChars(js, nullptr);
      wfd = open(p, O_EVTONLY | O_CLOEXEC);
      env->ReleaseStringUTFChars(js, p);
      env->DeleteLocalRef(js);
    }
    if (wfd >= 0) {
      struct kevent ev;
      EV_SET(&ev, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
             NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME, 0, nullptr);
      if (kevent(kq, &ev, 1, nullptr, 0, nullptr) != 0) {
        close(wfd);
        wfd = -1;
      }
    }
    env->SetIntArrayRegion(wfds, i, 1, &wfd);
  }
}

void fo_stopWatching(JNIEnv* env, jobject, jint, jintArray wfds) {
  jsize n = env->GetArrayLength(wfds);
  for (jsize i = 0; i < n; i++) {
    jint wfd;
    env->GetIntArrayRegion(wfds, i, 1, &wfd);
    if (wfd >= 0) close(wfd);  // closing the fd removes its kevent
  }
}

const JNINativeMethod kFileObserverThread[] = {
    M("init", "()I", fo_init),
    M("observe", "(I)V", fo_observe),
    M("startWatching", "(I[Ljava/lang/String;I[I)V", fo_startWatching),
    M("stopWatching", "(I[I)V", fo_stopWatching),
};

// android.os.incremental.IncrementalManager (libservices.core on a device): no incremental file
// system, so no file is incremental and none has a v4 signature (ApkSignatureVerifier then reads
// the v3/v2/v1 signatures, as for any installed APK).
jboolean im_false(JNIEnv*, jclass) { return JNI_FALSE; }
jboolean im_isIncrementalFd(JNIEnv*, jclass, jint) { return JNI_FALSE; }
jboolean im_isIncrementalPath(JNIEnv*, jclass, jstring) { return JNI_FALSE; }
jbyteArray im_unsafeGetFileSignature(JNIEnv*, jclass, jstring) { return nullptr; }

const JNINativeMethod kIncrementalManager[] = {
    M("nativeIsEnabled", "()Z", im_false),
    M("nativeIsV2Available", "()Z", im_false),
    M("nativeIsIncrementalFd", "(I)Z", im_isIncrementalFd),
    M("nativeIsIncrementalPath", "(Ljava/lang/String;)Z", im_isIncrementalPath),
    M("nativeUnsafeGetFileSignature", "(Ljava/lang/String;)[B", im_unsafeGetFileSignature),
};

// android.os.Debug (android_os_Debug.cpp reads /proc and mallinfo): the native heap from
// Darwin's malloc zones, the process footprint from task_info. Apps show these (Mindustry's
// HUD) or log them.
malloc_statistics_t HeapStats() {
  malloc_statistics_t st = {};
  malloc_zone_statistics(nullptr, &st);  // all zones
  return st;
}
jlong dbg_heapSize(JNIEnv*, jclass) { return static_cast<jlong>(HeapStats().size_allocated); }
jlong dbg_heapAllocated(JNIEnv*, jclass) { return static_cast<jlong>(HeapStats().size_in_use); }
jlong dbg_heapFree(JNIEnv*, jclass) {
  malloc_statistics_t st = HeapStats();
  return static_cast<jlong>(st.size_allocated - st.size_in_use);
}
jlong FootprintKb() {
  task_vm_info_data_t info = {};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<jlong>(info.phys_footprint / 1024);
}
jlong dbg_getPss(JNIEnv*, jclass) { return FootprintKb(); }
jlong dbg_getRss(JNIEnv*, jclass) { return FootprintKb(); }
jlong dbg_getPssPid(JNIEnv* env, jclass, jint, jlongArray uss, jlongArray memtrack) {
  jlong kb = FootprintKb();
  if (uss != nullptr && env->GetArrayLength(uss) > 0) env->SetLongArrayRegion(uss, 0, 1, &kb);
  return kb;
}
jlong dbg_getRssPid(JNIEnv*, jclass, jint, jlongArray) { return FootprintKb(); }
void dbg_getMemoryInfo(JNIEnv*, jclass, jobject) {}  // (fields stay 0)
jboolean dbg_getMemoryInfoPid(JNIEnv*, jclass, jint, jobject) { return JNI_FALSE; }
void dbg_getMemInfo(JNIEnv*, jclass, jlongArray) {}
jlong dbg_minusOne(JNIEnv*, jclass) { return -1; }
jboolean dbg_false(JNIEnv*, jclass) { return JNI_FALSE; }

const JNINativeMethod kDebug[] = {
    M("getNativeHeapSize", "()J", dbg_heapSize),
    M("getNativeHeapAllocatedSize", "()J", dbg_heapAllocated),
    M("getNativeHeapFreeSize", "()J", dbg_heapFree),
    M("getPss", "()J", dbg_getPss),
    M("getRss", "()J", dbg_getRss),
    M("getPss", "(I[J[J)J", dbg_getPssPid),
    M("getRss", "(I[J)J", dbg_getRssPid),
    M("getMemoryInfo", "(Landroid/os/Debug$MemoryInfo;)V", dbg_getMemoryInfo),
    M("getMemoryInfo", "(ILandroid/os/Debug$MemoryInfo;)Z", dbg_getMemoryInfoPid),
    M("getMemInfo", "([J)V", dbg_getMemInfo),
    M("getZramFreeKb", "()J", dbg_minusOne),
    M("isVmapStack", "()Z", dbg_false),
};

}  // namespace

namespace android {
int register_andropkg_framework_stubs(JNIEnv* env) {
  andropkg::RegisterIfDeclared(env, "android/os/Debug", kDebug, N(kDebug));
  andropkg::RegisterIfDeclared(env, "android/os/GraphicsEnvironment", kGraphicsEnvironment,
                               N(kGraphicsEnvironment));
  andropkg::RegisterIfDeclared(env, "android/app/ActivityThread", kActivityThread, N(kActivityThread));
  andropkg::RegisterIfDeclared(env, "android/os/FileObserver$ObserverThread", kFileObserverThread,
                               N(kFileObserverThread));
  andropkg::RegisterIfDeclared(env, "android/os/incremental/IncrementalManager", kIncrementalManager,
                               N(kIncrementalManager));
  return 0;
}
}  // namespace android
