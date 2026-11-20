// andropkg: helpers for the framework JNI we implement ourselves.
#pragma once
#include <jni.h>

namespace andropkg {

/// Registers each method the class actually declares (signatures drift between releases;
/// a missing one must not fail the whole table). Returns -1 only if the class is missing.
inline int RegisterIfDeclared(JNIEnv* env, const char* class_name, const JNINativeMethod* methods,
                              int count) {
  jclass c = env->FindClass(class_name);
  if (c == nullptr) {
    env->ExceptionClear();
    return -1;
  }
  for (int i = 0; i < count; i++) {
    const JNINativeMethod& m = methods[i];
    bool exists = env->GetStaticMethodID(c, m.name, m.signature) != nullptr;
    if (!exists) {
      env->ExceptionClear();
      exists = env->GetMethodID(c, m.name, m.signature) != nullptr;
      if (!exists) env->ExceptionClear();
    }
    if (exists && env->RegisterNatives(c, &m, 1) != JNI_OK) env->ExceptionClear();
  }
  env->DeleteLocalRef(c);
  return 0;
}

}  // namespace andropkg
