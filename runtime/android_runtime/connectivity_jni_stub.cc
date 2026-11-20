// andropkg: libframework-connectivity-jni (Connectivity module). iOS has one network view per
// process and no per-network sockets, BPF or netd DNS; binding to a network succeeds with no
// effect, which matches a device that only has the default network.
#include <jni.h>

#include <atomic>

#include "jni_util.h"

namespace {
std::atomic<jlong> gProcessNetwork{0};

jboolean bindProcessToNetworkHandle(JNIEnv*, jclass, jlong handle) {
  gProcessNetwork = handle;
  return JNI_TRUE;
}
jlong getBoundNetworkHandleForProcess(JNIEnv*, jclass) { return gProcessNetwork; }
jboolean bindProcessToNetworkForHostResolution(JNIEnv*, jclass, jint) { return JNI_TRUE; }
jint bindSocketToNetworkHandle(JNIEnv*, jclass, jobject, jlong) { return 0; }
void noopFd(JNIEnv*, jclass, jobject) {}

const JNINativeMethod kMethods[] = {
    {"bindProcessToNetworkHandle", "(J)Z", reinterpret_cast<void*>(bindProcessToNetworkHandle)},
    {"getBoundNetworkHandleForProcess", "()J", reinterpret_cast<void*>(getBoundNetworkHandleForProcess)},
    {"bindProcessToNetworkForHostResolution", "(I)Z",
     reinterpret_cast<void*>(bindProcessToNetworkForHostResolution)},
    {"bindSocketToNetworkHandle", "(Ljava/io/FileDescriptor;J)I", reinterpret_cast<void*>(bindSocketToNetworkHandle)},
    {"attachDropAllBPFFilter", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void*>(noopFd)},
    {"detachBPFFilter", "(Ljava/io/FileDescriptor;)V", reinterpret_cast<void*>(noopFd)},
};
}  // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  andropkg::RegisterIfDeclared(env, "android/net/NetworkUtils", kMethods, sizeof kMethods / sizeof kMethods[0]);
  return JNI_VERSION_1_6;
}
