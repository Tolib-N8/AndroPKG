// andropkg: libcore.math.NativeBN (BoringSSL BIGNUM fast paths for java.math.BigInteger) is not
// registered until BoringSSL is built (M3). BigInteger operations that need it will throw
// UnsatisfiedLinkError. Replaces libcore/luni/src/main/native/libcore_math_NativeBN.cpp.
#include <jni.h>

void register_libcore_math_NativeBN(JNIEnv*) {}
