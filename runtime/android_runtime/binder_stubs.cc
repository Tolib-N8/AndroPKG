// andropkg: android.os.Binder natives for a single-process world (no binder driver).
// Every caller is "us": calling identity is our own uid/pid; there is nothing to flush.
// Full Binder object support (local IBinder transactions) comes with the in-process
// service manager (M2c).
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <unistd.h>

#include "jni_util.h"

namespace android {

static jint Binder_getCallingPid(JNIEnv*, jclass) { return getpid(); }
static jint Binder_getCallingUid(JNIEnv*, jclass) { return static_cast<jint>(getuid()); }
static jboolean Binder_isDirectlyHandlingTransactionNative(JNIEnv*, jclass) { return JNI_FALSE; }
static jlong Binder_clearCallingIdentity(JNIEnv*, jclass) {
  return (static_cast<jlong>(getuid()) << 32) | static_cast<jlong>(getpid());
}
static void Binder_restoreCallingIdentity(JNIEnv*, jclass, jlong) {}
static void Binder_flushPendingCommands(JNIEnv*, jclass) {}
static void Binder_setThreadStrictModePolicy(JNIEnv*, jclass, jint) {}
static jint Binder_getThreadStrictModePolicy(JNIEnv*, jclass) { return 0; }
static jlong Binder_setCallingWorkSourceUid(JNIEnv*, jclass, jint) { return 0; }
static jint Binder_getCallingWorkSourceUid(JNIEnv*, jclass) { return -1; }
static jlong Binder_clearCallingWorkSource(JNIEnv*, jclass) { return 0; }
static void Binder_restoreCallingWorkSource(JNIEnv*, jclass, jlong) {}
static void Binder_markVintfStability(JNIEnv*, jobject) {}
static void Binder_forceDowngradeToSystemStability(JNIEnv*, jobject) {}
static void Binder_blockUntilThreadAvailable(JNIEnv*, jclass) {}
static jlong Binder_getNativeBBinderHolder(JNIEnv*, jclass) { return 1; }  // opaque non-null
static jlong Binder_getNativeFinalizer(JNIEnv*, jclass) { return 0; }

static const JNINativeMethod kBinderMethods[] = {
    {"getCallingPid", "()I", reinterpret_cast<void*>(Binder_getCallingPid)},
    {"getCallingUid", "()I", reinterpret_cast<void*>(Binder_getCallingUid)},
    {"isDirectlyHandlingTransactionNative", "()Z",
     reinterpret_cast<void*>(Binder_isDirectlyHandlingTransactionNative)},
    {"clearCallingIdentity", "()J", reinterpret_cast<void*>(Binder_clearCallingIdentity)},
    {"restoreCallingIdentity", "(J)V", reinterpret_cast<void*>(Binder_restoreCallingIdentity)},
    {"flushPendingCommands", "()V", reinterpret_cast<void*>(Binder_flushPendingCommands)},
    {"setThreadStrictModePolicy", "(I)V", reinterpret_cast<void*>(Binder_setThreadStrictModePolicy)},
    {"getThreadStrictModePolicy", "()I", reinterpret_cast<void*>(Binder_getThreadStrictModePolicy)},
    {"setCallingWorkSourceUid", "(I)J", reinterpret_cast<void*>(Binder_setCallingWorkSourceUid)},
    {"getCallingWorkSourceUid", "()I", reinterpret_cast<void*>(Binder_getCallingWorkSourceUid)},
    {"clearCallingWorkSource", "()J", reinterpret_cast<void*>(Binder_clearCallingWorkSource)},
    {"restoreCallingWorkSource", "(J)V", reinterpret_cast<void*>(Binder_restoreCallingWorkSource)},
    {"markVintfStability", "()V", reinterpret_cast<void*>(Binder_markVintfStability)},
    {"forceDowngradeToSystemStability", "()V",
     reinterpret_cast<void*>(Binder_forceDowngradeToSystemStability)},
    {"blockUntilThreadAvailable", "()V", reinterpret_cast<void*>(Binder_blockUntilThreadAvailable)},
    {"getNativeBBinderHolder", "()J", reinterpret_cast<void*>(Binder_getNativeBBinderHolder)},
    {"getNativeFinalizer", "()J", reinterpret_cast<void*>(Binder_getNativeFinalizer)},
};

// com.android.internal.os.BinderInternal
static void BinderInternal_handleGc(JNIEnv*, jclass) {}
static void BinderInternal_joinThreadPool(JNIEnv*, jclass) {}
static void BinderInternal_disableBackgroundScheduling(JNIEnv*, jclass, jboolean) {}
static void BinderInternal_setMaxThreads(JNIEnv*, jclass, jint) {}
static void BinderInternal_nSetBinderProxyCountEnabled(JNIEnv*, jclass, jboolean) {}

static const JNINativeMethod kBinderInternalMethods[] = {
    {"handleGc", "()V", reinterpret_cast<void*>(BinderInternal_handleGc)},
    {"joinThreadPool", "()V", reinterpret_cast<void*>(BinderInternal_joinThreadPool)},
    {"disableBackgroundScheduling", "(Z)V", reinterpret_cast<void*>(BinderInternal_disableBackgroundScheduling)},
    {"setMaxThreads", "(I)V", reinterpret_cast<void*>(BinderInternal_setMaxThreads)},
    {"nSetBinderProxyCountEnabled", "(Z)V", reinterpret_cast<void*>(BinderInternal_nSetBinderProxyCountEnabled)},
};

// android.graphics: the few natives an activity touches without a window (hwui proper is M2d).
static void Canvas_nSetCompatibilityVersion(JNIEnv*, jclass, jint) {}
static const JNINativeMethod kCanvasMethods[] = {
    {"nSetCompatibilityVersion", "(I)V", reinterpret_cast<void*>(Canvas_nSetCompatibilityVersion)},
};

int register_andropkg_binder_stubs(JNIEnv* env) {
  andropkg::RegisterIfDeclared(env, "com/android/internal/os/BinderInternal", kBinderInternalMethods,
                               sizeof kBinderInternalMethods / sizeof kBinderInternalMethods[0]);
  andropkg::RegisterIfDeclared(env, "android/graphics/Canvas", kCanvasMethods,
                               sizeof kCanvasMethods / sizeof kCanvasMethods[0]);
  return andropkg::RegisterIfDeclared(env, "android/os/Binder", kBinderMethods,
                                      sizeof kBinderMethods / sizeof kBinderMethods[0]);
}

}  // namespace android
