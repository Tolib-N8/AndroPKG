// andropkg: android.os.Process natives for a single iOS process. Scheduling groups, cgroups,
// freezer and /proc don't exist on iOS; the calls succeed without effect, while time, memory
// and signals to ourselves map onto Darwin.
#include <jni.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <pwd.h>
#include <grp.h>

#include "jni_util.h"

namespace {

jlong getElapsedCpuTime(JNIEnv*, jclass) {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return static_cast<jlong>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

jint getUidForName(JNIEnv* env, jclass, jstring name) {
  const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
  passwd* pw = n ? getpwnam(n) : nullptr;
  if (n) env->ReleaseStringUTFChars(name, n);
  return pw ? static_cast<jint>(pw->pw_uid) : -1;
}

jint getGidForName(JNIEnv* env, jclass, jstring name) {
  const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
  group* gr = n ? getgrnam(n) : nullptr;
  if (n) env->ReleaseStringUTFChars(name, n);
  return gr ? static_cast<jint>(gr->gr_gid) : -1;
}

void noop_II(JNIEnv*, jclass, jint, jint) {}
void noop_I(JNIEnv*, jclass, jint) {}
void noop_Z(JNIEnv*, jclass, jboolean) {}
void noop_IIZ(JNIEnv*, jclass, jint, jint, jboolean) {}
void noop_IZ(JNIEnv*, jclass, jint, jboolean) {}
void noop_V(JNIEnv*, jclass) {}
void noop_III(JNIEnv*, jclass, jint, jint, jint) {}
jint zero_I(JNIEnv*, jclass, jint) { return 0; }
jint zero_II(JNIEnv*, jclass, jint, jint) { return 0; }
jint zero_III(JNIEnv*, jclass, jint, jint, jint) { return 0; }
jboolean false_IZ(JNIEnv*, jclass, jint, jboolean) { return JNI_FALSE; }
jintArray emptyInts(JNIEnv* env, jclass) { return env->NewIntArray(0); }

void setArgV0Native(JNIEnv* env, jclass, jstring text) {
  if (text == nullptr) return;
  const char* s = env->GetStringUTFChars(text, nullptr);
  pthread_setname_np(s);  // closest equivalent: name the main thread
  env->ReleaseStringUTFChars(text, s);
}

void sendSignal(JNIEnv*, jclass, jint pid, jint sig) {
  if (pid == getpid()) kill(pid, sig);  // other "processes" don't exist here
}

jlong getTotalMemory(JNIEnv*, jclass) {
  uint64_t mem = 0;
  size_t len = sizeof mem;
  sysctlbyname("hw.memsize", &mem, &len, nullptr, 0);
  return static_cast<jlong>(mem);
}

jlong getFreeMemory(JNIEnv*, jclass) {
  vm_statistics64_data_t vm;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
      KERN_SUCCESS) {
    return 0;
  }
  return static_cast<jlong>(vm.free_count + vm.inactive_count) * static_cast<jlong>(vm_page_size);
}

jlong residentBytes() {
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS) {
    return 0;
  }
  return static_cast<jlong>(info.resident_size);
}

jlong getPss(JNIEnv*, jclass, jint pid) { return pid == getpid() ? residentBytes() / 1024 : 0; }

jlongArray getRss(JNIEnv* env, jclass, jint pid) {
  jlongArray a = env->NewLongArray(4);
  jlong v[4] = {pid == getpid() ? residentBytes() / 1024 : 0, 0, 0, 0};
  env->SetLongArrayRegion(a, 0, 4, v);
  return a;
}

void readProcLines(JNIEnv*, jclass, jstring, jobjectArray, jlongArray) {}
jintArray getPids(JNIEnv*, jclass, jstring, jintArray) { return nullptr; }
jboolean readProcFile(JNIEnv*, jclass, jstring, jintArray, jobjectArray, jlongArray, jfloatArray) {
  return JNI_FALSE;
}
jboolean parseProcLine(JNIEnv*, jclass, jbyteArray, jint, jint, jintArray, jobjectArray, jlongArray,
                       jfloatArray) {
  return JNI_FALSE;
}
jintArray getPidsForCommands(JNIEnv* env, jclass, jobjectArray) { return env->NewIntArray(0); }
jint nativePidFdOpen(JNIEnv*, jclass, jint, jint) { return -1; }

#define M(name, sig, fn) {name, sig, reinterpret_cast<void*>(fn)}
const JNINativeMethod kMethods[] = {
    M("getElapsedCpuTime", "()J", getElapsedCpuTime),
    M("getUidForName", "(Ljava/lang/String;)I", getUidForName),
    M("getGidForName", "(Ljava/lang/String;)I", getGidForName),
    M("setThreadPriority", "(II)V", noop_II),
    M("setThreadPriority", "(I)V", noop_I),
    M("setCanSelfBackground", "(Z)V", noop_Z),
    M("setThreadGroup", "(II)V", noop_II),
    M("setThreadGroupAndCpuset", "(II)V", noop_II),
    M("setProcessGroup", "(II)V", noop_II),
    M("setProcessFrozen", "(IIZ)V", noop_IIZ),
    M("enableFreezer", "(Z)V", noop_Z),
    M("getProcessGroup", "(I)I", zero_I),
    M("createProcessGroup", "(II)I", zero_II),
    M("getExclusiveCores", "()[I", emptyInts),
    M("getThreadPriority", "(I)I", zero_I),
    M("getThreadScheduler", "(I)I", zero_I),
    M("setThreadScheduler", "(III)V", noop_III),
    M("setSwappiness", "(IZ)Z", false_IZ),
    M("setArgV0Native", "(Ljava/lang/String;)V", setArgV0Native),
    M("setUid", "(I)I", zero_I),
    M("setGid", "(I)I", zero_I),
    M("sendSignal", "(II)V", sendSignal),
    M("sendSignalQuiet", "(II)V", sendSignal),
    M("getFreeMemory", "()J", getFreeMemory),
    M("getTotalMemory", "()J", getTotalMemory),
    M("readProcLines", "(Ljava/lang/String;[Ljava/lang/String;[J)V", readProcLines),
    M("getPids", "(Ljava/lang/String;[I)[I", getPids),
    M("readProcFile", "(Ljava/lang/String;[I[Ljava/lang/String;[J[F)Z", readProcFile),
    M("parseProcLine", "([BII[I[Ljava/lang/String;[J[F)Z", parseProcLine),
    M("getPidsForCommands", "([Ljava/lang/String;)[I", getPidsForCommands),
    M("getPss", "(I)J", getPss),
    M("getRss", "(I)[J", getRss),
    M("killProcessGroup", "(II)I", zero_II),
    M("sendSignalToProcessGroup", "(III)I", zero_III),
    M("freezeCgroupUid", "(IZ)V", noop_IZ),
    M("removeAllProcessGroups", "()V", noop_V),
    M("nativePidFdOpen", "(II)I", nativePidFdOpen),
};
#undef M

}  // namespace

namespace android {
int register_andropkg_process_stubs(JNIEnv* env) {
  return andropkg::RegisterIfDeclared(env, "android/os/Process", kMethods,
                                      sizeof kMethods / sizeof kMethods[0]);
}
}  // namespace android
