// andropkg: android.os.Binder natives for a single-process world (no binder driver).
// Every caller is "us": calling identity is our own uid/pid; there is nothing to flush.
// Full Binder object support (local IBinder transactions) comes with the in-process
// service manager (M2c).
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <unistd.h>

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

int register_andropkg_binder_stubs(JNIEnv* env) {
  // Register only methods the framework jar actually declares (signatures differ by release).
  jclass c = env->FindClass("android/os/Binder");
  if (c == nullptr) return -1;
  for (const auto& m : kBinderMethods) {
    bool exists = env->GetStaticMethodID(c, m.name, m.signature) != nullptr;
    if (!exists) {
      env->ExceptionClear();
      exists = env->GetMethodID(c, m.name, m.signature) != nullptr;
      if (!exists) env->ExceptionClear();
    }
    if (exists && env->RegisterNatives(c, &m, 1) != JNI_OK) env->ExceptionClear();
  }
  return 0;
}

}  // namespace android
