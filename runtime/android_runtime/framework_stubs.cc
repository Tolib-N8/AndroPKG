// andropkg: framework natives whose subsystem doesn't exist in a single iOS process. Each
// answers the way a device without that feature would.
#include <jni.h>

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

}  // namespace

namespace android {
int register_andropkg_framework_stubs(JNIEnv* env) {
  andropkg::RegisterIfDeclared(env, "android/os/GraphicsEnvironment", kGraphicsEnvironment,
                               N(kGraphicsEnvironment));
  andropkg::RegisterIfDeclared(env, "android/app/ActivityThread", kActivityThread, N(kActivityThread));
  return 0;
}
}  // namespace android
