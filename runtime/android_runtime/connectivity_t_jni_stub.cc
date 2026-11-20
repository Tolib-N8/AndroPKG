// andropkg: libframework-connectivity-tiramisu-jni. Socket tagging feeds per-uid traffic
// accounting in the kernel's BPF maps; there's nothing to account into, so tagging succeeds.
#include <jni.h>

#include "jni_util.h"

namespace {
jint tagSocketFd(JNIEnv*, jclass, jobject, jint, jint) { return 0; }
jint untagSocketFd(JNIEnv*, jclass, jobject) { return 0; }

const JNINativeMethod kMethods[] = {
    {"native_tagSocketFd", "(Ljava/io/FileDescriptor;II)I", reinterpret_cast<void*>(tagSocketFd)},
    {"native_untagSocketFd", "(Ljava/io/FileDescriptor;)I", reinterpret_cast<void*>(untagSocketFd)},
};
}  // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  andropkg::RegisterIfDeclared(env, "android/net/TrafficStats", kMethods, 2);
  return JNI_VERSION_1_6;
}
