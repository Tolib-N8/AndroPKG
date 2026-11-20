// andropkg: libstats_jni (StatsD module) — framework telemetry written to the statsd daemon.
// There is no statsd here; atoms are dropped.
#include <jni.h>

#include "jni_util.h"

namespace {
void writeImpl(JNIEnv*, jclass, jbyteArray, jint, jint) {}

const JNINativeMethod kMethods[] = {
    {"writeImpl", "([BII)V", reinterpret_cast<void*>(writeImpl)},
};
}  // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  andropkg::RegisterIfDeclared(env, "android/util/StatsLog", kMethods, 1);
  return JNI_VERSION_1_6;
}
