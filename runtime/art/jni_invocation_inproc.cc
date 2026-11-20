// andropkg: libnativehelper's JniInvocation normally dlopen()s libart.so and forwards
// JNI_CreateJavaVM & co. ART is linked into the same image here, so the invocation API is
// already resolved; these entry points just satisfy callers (e.g. dalvikvm).
#include <stdbool.h>
#include <stddef.h>

struct JniInvocationImpl {
  int unused;
};

extern "C" {
JniInvocationImpl* JniInvocationCreate() {
  static JniInvocationImpl impl;
  return &impl;
}
void JniInvocationDestroy(JniInvocationImpl*) {}
bool JniInvocationInit(JniInvocationImpl*, const char*) { return true; }
const char* JniInvocationGetLibrary(const char* library, char*) {
  return library != nullptr ? library : "libart.dylib";
}
}
