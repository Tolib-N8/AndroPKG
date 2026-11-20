// andropkg: entry point of libandroid_runtime.dylib (the framework's native half).
//
// On Android, zygote's AndroidRuntime::startReg() registers ~200 JNI tables before any app
// code runs. There is no zygote here: ART loads this library at startup
// (Runtime::InitNativeMethods, andropkg patch) and JNI_OnLoad registers the subset we build.
// Framework natives that aren't registered fail lazily with UnsatisfiedLinkError when called.
#include <android_runtime/AndroidRuntime.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Log.h>

#include <string.h>

#include <string>

namespace andropkg {
void LoadSystemProperties();
}

namespace android {

static JavaVM* gVm = nullptr;

int AndroidRuntime::registerNativeMethods(JNIEnv* env, const char* className,
                                          const JNINativeMethod* gMethods, int numMethods) {
  return jniRegisterNativeMethods(env, className, gMethods, numMethods);
}

JavaVM* AndroidRuntime::getJavaVM() { return gVm; }

JNIEnv* AndroidRuntime::getJNIEnv() {
  JNIEnv* env = nullptr;
  if (gVm == nullptr || gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_4) != JNI_OK) {
    return nullptr;
  }
  return env;
}

jclass AndroidRuntime::findClass(JNIEnv* env, const char* className) {
  return env->FindClass(className);
}

char* AndroidRuntime::toSlashClassName(const char* className) {
  char* result = strdup(className);
  for (char* cp = result; *cp != '\0'; cp++) {
    if (*cp == '.') *cp = '/';
  }
  return result;
}

jstring AndroidRuntime::NewStringLatin1(JNIEnv* env, const char* bytes) {
  return env->NewStringUTF(bytes);  // ASCII-only callers in the subset we build
}

// ---- registration -----------------------------------------------------------------------
extern int register_android_os_SystemClock(JNIEnv* env);
extern int register_android_util_Log(JNIEnv* env);
extern int register_android_util_EventLog(JNIEnv* env);
extern int register_android_os_MessageQueue(JNIEnv* env);
extern int register_android_os_Trace(JNIEnv* env);
extern int register_android_os_SystemProperties(JNIEnv* env);
extern int register_andropkg_binder_stubs(JNIEnv* env);
extern int register_android_content_res_ApkAssets(JNIEnv* env);
extern int register_android_content_AssetManager(JNIEnv* env);
extern int register_android_content_StringBlock(JNIEnv* env);
extern int register_android_content_XmlBlock(JNIEnv* env);
extern int register_android_content_res_ResourceTimer(JNIEnv* env);

struct RegJni {
  const char* name;
  int (*fn)(JNIEnv*);
};

static const RegJni kRegistrations[] = {
    {"SystemClock", register_android_os_SystemClock},
    {"Log", register_android_util_Log},
    {"EventLog", register_android_util_EventLog},
    {"MessageQueue", register_android_os_MessageQueue},
    {"Trace", register_android_os_Trace},
    {"SystemProperties", register_android_os_SystemProperties},
    {"Binder (andropkg stubs)", register_andropkg_binder_stubs},
    {"ApkAssets", register_android_content_res_ApkAssets},
    {"AssetManager", register_android_content_AssetManager},
    {"StringBlock", register_android_content_StringBlock},
    {"XmlBlock", register_android_content_XmlBlock},
    {"ResourceTimer", register_android_content_res_ResourceTimer},
};

}  // namespace android

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  android::gVm = vm;
  JNIEnv* env = android::AndroidRuntime::getJNIEnv();
  // Plain-Java runs (no framework.jar on the boot classpath): nothing to register.
  jclass probe = env->FindClass("android/os/SystemClock");
  if (probe == nullptr) {
    env->ExceptionClear();
    return JNI_VERSION_1_6;
  }
  env->DeleteLocalRef(probe);
  andropkg::LoadSystemProperties();  // before any framework class initializer runs
  // Registration helpers leave local refs (FindClass...); ART asserts the table is empty
  // after startup, so release them all at once.
  env->PushLocalFrame(512);
  for (const auto& r : android::kRegistrations) {
    if (r.fn(env) < 0) {
      ALOGE("andropkg: registering %s natives failed", r.name);
      env->PopLocalFrame(nullptr);
      return JNI_ERR;
    }
  }
  env->PopLocalFrame(nullptr);
  return JNI_VERSION_1_6;
}
