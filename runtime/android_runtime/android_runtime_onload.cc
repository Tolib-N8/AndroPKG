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
extern int register_andropkg_process_stubs(JNIEnv* env);
extern int register_andropkg_framework_stubs(JNIEnv* env);
extern int register_andropkg_parcel(JNIEnv* env);
extern int register_andropkg_window(JNIEnv* env);
extern int register_andropkg_sensors(JNIEnv* env);
extern int register_andropkg_audio(JNIEnv* env);
extern int register_android_view_InputQueue(JNIEnv* env);
extern int register_andropkg_native_activity(JNIEnv* env);
extern int register_android_content_res_ApkAssets(JNIEnv* env);
extern int register_android_content_AssetManager(JNIEnv* env);
extern int register_android_content_StringBlock(JNIEnv* env);
extern int register_android_content_XmlBlock(JNIEnv* env);
extern int register_android_content_res_ResourceTimer(JNIEnv* env);
extern int register_com_android_internal_os_ClassLoaderFactory(JNIEnv* env);
extern int register_android_view_MotionEvent(JNIEnv* env);
extern int register_android_view_KeyEvent(JNIEnv* env);
extern int register_android_view_VelocityTracker(JNIEnv* env);
extern int register_android_view_KeyCharacterMap(JNIEnv* env);
extern int register_android_database_CursorWindow(JNIEnv* env);
extern int register_android_database_SQLiteConnection(JNIEnv* env);
extern int register_android_database_SQLiteGlobal(JNIEnv* env);
extern int register_android_database_SQLiteDebug(JNIEnv* env);
extern int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv* env);
extern int register_android_animation_PropertyValuesHolder(JNIEnv* env);
extern int register_android_text_AndroidCharacter(JNIEnv* env);
extern int register_android_text_Hyphenator(JNIEnv* env);
extern int register_android_util_CharsetUtils(JNIEnv* env);
extern int register_android_content_res_Configuration(JNIEnv* env);
}  // namespace android

// hwui's android.graphics.* JNI (frameworks/base/libs/hwui/apex/jni_runtime.cpp).
extern "C" void init_android_graphics();
extern "C" int register_android_graphics_classes(JNIEnv* env);

namespace android {
static int register_hwui_graphics(JNIEnv* env) {
  init_android_graphics();
  return register_android_graphics_classes(env);
}

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
    {"Process (andropkg)", register_andropkg_process_stubs},
    {"framework stubs (andropkg)", register_andropkg_framework_stubs},
    {"ApkAssets", register_android_content_res_ApkAssets},
    {"AssetManager", register_android_content_AssetManager},
    {"StringBlock", register_android_content_StringBlock},
    {"XmlBlock", register_android_content_XmlBlock},
    {"ResourceTimer", register_android_content_res_ResourceTimer},
    {"ClassLoaderFactory", register_com_android_internal_os_ClassLoaderFactory},
    {"MotionEvent", register_android_view_MotionEvent},
    {"KeyEvent", register_android_view_KeyEvent},
    {"VelocityTracker", register_android_view_VelocityTracker},
    {"KeyCharacterMap", register_android_view_KeyCharacterMap},
    {"android_database_CursorWindow", register_android_database_CursorWindow},
    {"android_database_SQLiteConnection", register_android_database_SQLiteConnection},
    {"android_database_SQLiteGlobal", register_android_database_SQLiteGlobal},
    {"android_database_SQLiteDebug", register_android_database_SQLiteDebug},
    {"com_android_internal_util_VirtualRefBasePtr", register_com_android_internal_util_VirtualRefBasePtr},
    {"android_animation_PropertyValuesHolder", register_android_animation_PropertyValuesHolder},
    {"android_text_AndroidCharacter", register_android_text_AndroidCharacter},
    {"android_text_Hyphenator", register_android_text_Hyphenator},
    {"android_util_CharsetUtils", register_android_util_CharsetUtils},
    {"android_content_res_Configuration", register_android_content_res_Configuration},
    {"Parcel (andropkg)", register_andropkg_parcel},
    {"android.graphics (hwui)", register_hwui_graphics},
    {"window system (andropkg)", register_andropkg_window},
    {"sensors (andropkg)", register_andropkg_sensors},
    {"audio (andropkg)", register_andropkg_audio},
    {"InputQueue", register_android_view_InputQueue},
    {"NativeActivity (andropkg)", register_andropkg_native_activity},
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
  env->PushLocalFrame(4096);
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
