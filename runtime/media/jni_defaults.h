// andropkg: neutral natives for framework classes whose native side talks to absent daemons.
//
// RegisterNativeDefaults() binds every `native` method of a class to a stub returning
// 0 / false / null (0 is also the framework's SUCCESS status); real implementations are
// registered afterwards and replace the stubs they cover. Arguments are ignored, which is
// safe in the arm64 (and x86-64) calling convention.
#pragma once
#include <jni.h>

#include <string>

namespace andropkg_media {

inline void StubVoid(JNIEnv*, jobject) {}
inline jlong StubInt(JNIEnv*, jobject) { return 0; }  // fills x0: right for Z/B/C/S/I/J too
inline jfloat StubFloat(JNIEnv*, jobject) { return 0.f; }
inline jdouble StubDouble(JNIEnv*, jobject) { return 0.0; }
inline jobject StubObject(JNIEnv*, jobject) { return nullptr; }

inline std::string Descriptor(JNIEnv* env, jclass type) {
  jclass cls = env->FindClass("java/lang/Class");
  jmethodID getName = env->GetMethodID(cls, "getName", "()Ljava/lang/String;");
  jmethodID isPrimitive = env->GetMethodID(cls, "isPrimitive", "()Z");
  jstring jn = static_cast<jstring>(env->CallObjectMethod(type, getName));
  const char* n = env->GetStringUTFChars(jn, nullptr);
  std::string name(n);
  env->ReleaseStringUTFChars(jn, n);
  env->DeleteLocalRef(jn);
  bool prim = env->CallBooleanMethod(type, isPrimitive);
  env->DeleteLocalRef(cls);
  if (prim) {
    static const char* kPrim[][2] = {{"boolean", "Z"}, {"byte", "B"}, {"char", "C"}, {"short", "S"},
                                     {"int", "I"},     {"long", "J"}, {"float", "F"}, {"double", "D"},
                                     {"void", "V"}};
    for (auto& p : kPrim) if (name == p[0]) return p[1];
    return "V";
  }
  for (char& c : name) if (c == '.') c = '/';
  return name[0] == '[' ? name : "L" + name + ";";
}

/// Binds all native methods of `class_name` to neutral stubs. Returns the number bound.
inline int RegisterNativeDefaults(JNIEnv* env, const char* class_name) {
  jclass target = env->FindClass(class_name);
  if (target == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  jclass classCls = env->FindClass("java/lang/Class");
  jclass methodCls = env->FindClass("java/lang/reflect/Method");
  jmethodID getDeclaredMethods = env->GetMethodID(classCls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
  jmethodID getModifiers = env->GetMethodID(methodCls, "getModifiers", "()I");
  jmethodID getName = env->GetMethodID(methodCls, "getName", "()Ljava/lang/String;");
  jmethodID getReturnType = env->GetMethodID(methodCls, "getReturnType", "()Ljava/lang/Class;");
  jmethodID getParameterTypes = env->GetMethodID(methodCls, "getParameterTypes", "()[Ljava/lang/Class;");
  jobjectArray methods = static_cast<jobjectArray>(env->CallObjectMethod(target, getDeclaredMethods));
  int bound = 0;
  for (jsize i = 0, n = methods ? env->GetArrayLength(methods) : 0; i < n; i++) {
    jobject m = env->GetObjectArrayElement(methods, i);
    if ((env->CallIntMethod(m, getModifiers) & 0x0100 /* NATIVE */) == 0) {
      env->DeleteLocalRef(m);
      continue;
    }
    std::string sig = "(";
    jobjectArray params = static_cast<jobjectArray>(env->CallObjectMethod(m, getParameterTypes));
    for (jsize p = 0, np = env->GetArrayLength(params); p < np; p++) {
      jclass t = static_cast<jclass>(env->GetObjectArrayElement(params, p));
      sig += Descriptor(env, t);
      env->DeleteLocalRef(t);
    }
    env->DeleteLocalRef(params);
    jclass rt = static_cast<jclass>(env->CallObjectMethod(m, getReturnType));
    std::string ret = Descriptor(env, rt);
    env->DeleteLocalRef(rt);
    sig += ")" + ret;
    void* fn;
    switch (ret[0]) {
      case 'V': fn = reinterpret_cast<void*>(StubVoid); break;
      case 'F': fn = reinterpret_cast<void*>(StubFloat); break;
      case 'D': fn = reinterpret_cast<void*>(StubDouble); break;
      case 'L': case '[': fn = reinterpret_cast<void*>(StubObject); break;
      default: fn = reinterpret_cast<void*>(StubInt); break;
    }
    jstring jn = static_cast<jstring>(env->CallObjectMethod(m, getName));
    const char* name = env->GetStringUTFChars(jn, nullptr);
    JNINativeMethod nm = {name, sig.c_str(), fn};
    if (env->RegisterNatives(target, &nm, 1) == JNI_OK) bound++; else env->ExceptionClear();
    env->ReleaseStringUTFChars(jn, name);
    env->DeleteLocalRef(jn);
    env->DeleteLocalRef(m);
  }
  if (methods) env->DeleteLocalRef(methods);
  env->DeleteLocalRef(methodCls);
  env->DeleteLocalRef(classCls);
  env->DeleteLocalRef(target);
  return bound;
}

/// Registers `methods` that the class declares (others are skipped, not fatal).
inline void RegisterDeclared(JNIEnv* env, const char* class_name, const JNINativeMethod* methods, int count) {
  jclass c = env->FindClass(class_name);
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  for (int i = 0; i < count; i++) {
    if (env->RegisterNatives(c, &methods[i], 1) != JNI_OK) env->ExceptionClear();
  }
  env->DeleteLocalRef(c);
}

}  // namespace andropkg_media
