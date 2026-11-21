// andropkg NDK layer — a JNI function table that Android (AAPCS64) code can call.
//
// JNIEnv's variadic functions (Call<Type>Method(env, obj, mid, ...), CallStatic..., CallNonvirtual...,
// NewObject) and their va_list forms (...V) are compiled for Darwin's calling convention inside
// ART. Android code passes variadic arguments in registers and an AAPCS64 va_list struct, so
// this table replaces exactly those entries (process-wide, via ART's JNIEnvExt::SetTableOverride)
// with wrappers that look at their caller: code in a loaded Android image gets AAPCS64 decoding,
// everything else the Darwin one. Arguments are converted by the method's signature (recorded
// when the method ID was looked up) and passed to the jvalue-array (...A) entry points.
#include <dlfcn.h>
#include <jni.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ndk_internal.h"

extern const struct JNINativeInterface *andropkg_jni_base_table(void) __attribute__((weak));
extern void andropkg_jni_set_table_override(const struct JNINativeInterface *t) __attribute__((weak));
extern const void *andropkg_jni_current_native_code(void) __attribute__((weak));

// ---- who is calling? ---------------------------------------------------------------------------
// The return address alone is not enough: `return (*env)->CallIntMethod(...)` compiles to a tail
// call, leaving the JNI trampoline in ART as the apparent caller. Code in this library never
// calls the variadic JNI entries, and Darwin code reached from an Android native method doesn't
// either, so a call is Android's if it returns into an Android image or into this library, or if
// the native method running on this thread is Android code.
static bool in_this_library(uintptr_t pc) {
    static uintptr_t lo, hi;
    if (!lo) {
        Dl_info d;
        if (dladdr((void *)&in_this_library, &d)) {
            lo = (uintptr_t)d.dli_fbase;
            hi = lo + 64 * 1024 * 1024;  // generous: just this dylib's text
            const struct mach_header_64 *mh = d.dli_fbase;
            const struct load_command *lc = (const void *)(mh + 1);
            for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
                if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
                    hi = lo + ((const struct segment_command_64 *)lc)->vmsize;
                }
            }
        }
    }
    return pc >= lo && pc < hi;
}

static bool android_caller(uintptr_t ra) {
    if (ndk_is_android_pc(ra) || in_this_library(ra)) return true;
    const void *code = andropkg_jni_current_native_code ? andropkg_jni_current_native_code() : NULL;
    return code && ndk_is_android_pc((uintptr_t)code);
}

/// True if `ap` is an AAPCS64 va_list (vs a Darwin one: a pointer to 8-byte argument slots).
static bool is_aapcs_va_list(const void *ap) {
    const ndk_aapcs_va_list *v = ap;
    if (v->gr_offs > 0 || v->gr_offs < -64 || (v->gr_offs & 7)) return false;
    if (v->vr_offs > 0 || v->vr_offs < -128 || (v->vr_offs & 15)) return false;
    pthread_t self = pthread_self();
    uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    uintptr_t bottom = top - pthread_get_stacksize_np(self);
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    uintptr_t p[3] = {(uintptr_t)v->stack, (uintptr_t)v->gr_top, (uintptr_t)v->vr_top};
    for (int i = 0; i < 3; i++)
        if (p[i] < sp || p[i] > top || p[i] < bottom) return false;
    return true;
}

static const struct JNINativeInterface *B;  // ART's own table
static struct JNINativeInterface g_table;

// ---- method signatures (jmethodID -> argument type letters) ------------------------------------

typedef struct { jmethodID mid; char types[32]; } sig_entry;
enum { SIG_CAP = 1 << 14 };
static sig_entry g_sigs[SIG_CAP];
static pthread_rwlock_t g_sig_lock = PTHREAD_RWLOCK_INITIALIZER;

static size_t sig_slot(jmethodID mid) { return ((uintptr_t)mid >> 3) * 0x9E3779B97F4A7C15ULL >> 50 & (SIG_CAP - 1); }

/// "(I[JLjava/lang/String;D)V" -> "I[LD" (one letter per argument; arrays and objects are L).
static void parse_sig(const char *sig, char *out, size_t cap) {
    size_t n = 0;
    const char *p = sig && *sig == '(' ? sig + 1 : "";
    while (*p && *p != ')' && n + 1 < cap) {
        char c = *p;
        if (c == '[') {
            while (*p == '[') p++;
            if (*p == 'L') { while (*p && *p != ';') p++; }
            if (*p) p++;
            out[n++] = 'L';
            continue;
        }
        if (c == 'L') {
            while (*p && *p != ';') p++;
            if (*p) p++;
            out[n++] = 'L';
            continue;
        }
        out[n++] = c;
        p++;
    }
    out[n] = 0;
}

static void sig_put(jmethodID mid, const char *sig) {
    if (!mid) return;
    pthread_rwlock_wrlock(&g_sig_lock);
    size_t i = sig_slot(mid);
    for (size_t k = 0; k < SIG_CAP; k++, i = (i + 1) & (SIG_CAP - 1)) {
        if (g_sigs[i].mid == mid || !g_sigs[i].mid) {
            g_sigs[i].mid = mid;
            parse_sig(sig, g_sigs[i].types, sizeof g_sigs[i].types);
            break;
        }
    }
    pthread_rwlock_unlock(&g_sig_lock);
}

/// Argument letters for `mid`; falls back to reflection for IDs we never saw looked up.
static void sig_get(JNIEnv *env, jclass cls, jobject obj, jmethodID mid, bool is_static, char *out, size_t cap) {
    pthread_rwlock_rdlock(&g_sig_lock);
    size_t i = sig_slot(mid);
    for (size_t k = 0; k < SIG_CAP && g_sigs[i].mid; k++, i = (i + 1) & (SIG_CAP - 1)) {
        if (g_sigs[i].mid == mid) {
            strlcpy(out, g_sigs[i].types, cap);
            pthread_rwlock_unlock(&g_sig_lock);
            return;
        }
    }
    pthread_rwlock_unlock(&g_sig_lock);
    // Reflection: Method/Constructor.getParameterTypes() -> letters.
    out[0] = 0;
    jclass own = cls ? NULL : B->GetObjectClass(env, obj);
    jobject m = B->ToReflectedMethod(env, cls ? cls : own, mid, is_static);
    if (own) B->DeleteLocalRef(env, own);
    if (!m) { B->ExceptionClear(env); return; }
    jclass exec = B->FindClass(env, "java/lang/reflect/Executable");
    jmethodID gpt = B->GetMethodID(env, exec, "getParameterTypes", "()[Ljava/lang/Class;");
    jobjectArray types = (jobjectArray)B->CallObjectMethodA(env, m, gpt, NULL);
    jclass classCls = B->FindClass(env, "java/lang/Class");
    jmethodID getName = B->GetMethodID(env, classCls, "getName", "()Ljava/lang/String;");
    jsize n = types ? B->GetArrayLength(env, types) : 0;
    size_t o = 0;
    for (jsize k = 0; k < n && o + 1 < cap; k++) {
        jobject t = B->GetObjectArrayElement(env, types, k);
        jstring name = (jstring)B->CallObjectMethodA(env, t, getName, NULL);
        const char *s = B->GetStringUTFChars(env, name, NULL);
        char c = 'L';
        if (!strcmp(s, "int")) c = 'I'; else if (!strcmp(s, "long")) c = 'J'; else if (!strcmp(s, "boolean")) c = 'Z';
        else if (!strcmp(s, "byte")) c = 'B'; else if (!strcmp(s, "char")) c = 'C'; else if (!strcmp(s, "short")) c = 'S';
        else if (!strcmp(s, "float")) c = 'F'; else if (!strcmp(s, "double")) c = 'D';
        out[o++] = c;
        B->ReleaseStringUTFChars(env, name, s);
        B->DeleteLocalRef(env, name);
        B->DeleteLocalRef(env, t);
    }
    out[o] = 0;
    B->DeleteLocalRef(env, m);
}

static jmethodID w_GetMethodID(JNIEnv *env, jclass c, const char *name, const char *sig) {
    jmethodID m = B->GetMethodID(env, c, name, sig);
    sig_put(m, sig);
    return m;
}
static jmethodID w_GetStaticMethodID(JNIEnv *env, jclass c, const char *name, const char *sig) {
    jmethodID m = B->GetStaticMethodID(env, c, name, sig);
    sig_put(m, sig);
    return m;
}

// ---- argument conversion ------------------------------------------------------------------------

static void to_jvalues(const char *types, ndk_va *v, jvalue *out) {
    for (int i = 0; types[i]; i++) {
        switch (types[i]) {
            case 'Z': out[i].z = (jboolean)ndk_va_int(v); break;
            case 'B': out[i].b = (jbyte)ndk_va_int(v); break;
            case 'C': out[i].c = (jchar)ndk_va_int(v); break;
            case 'S': out[i].s = (jshort)ndk_va_int(v); break;
            case 'I': out[i].i = (jint)ndk_va_int(v); break;
            case 'J': out[i].j = (jlong)ndk_va_int(v); break;
            case 'F': out[i].f = (jfloat)ndk_va_double(v); break;  // promoted to double
            case 'D': out[i].d = ndk_va_double(v); break;
            default: out[i].l = (jobject)(uintptr_t)ndk_va_int(v); break;
        }
    }
}

#define VA_REST                                                                                       \
    uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7, double d0, double d1, double d2, \
    double d3, double d4, double d5, double d6, double d7, uint64_t s0, uint64_t s1, uint64_t s2,       \
    uint64_t s3, uint64_t s4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9,          \
    uint64_t s10, uint64_t s11, uint64_t s12, uint64_t s13, uint64_t s14, uint64_t s15
// The variadic part begins after `named` registers among x3..x7 (named = 0 for (env, a, b, ...)).
// A Darwin caller put every variadic argument on the stack: only s0.. count.
#define VA_BUILD(v, named)                                                                            \
    uint64_t _gr[5] = {x3, x4, x5, x6, x7};                                                           \
    double _fr[8] = {d0, d1, d2, d3, d4, d5, d6, d7};                                                 \
    uint64_t _st[16] = {s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15};        \
    ndk_va v = {.stack = _st};                                                                        \
    if (android_caller((uintptr_t)__builtin_return_address(0))) {                                     \
        v.gr = _gr + (named); v.gn = 5 - (named); v.fr = _fr; v.fn = 8;                               \
    }

static void va_from_any(ndk_va *v, void *ap, uintptr_t caller, uint64_t *gr, double *fr) {
    (void)caller;
    if (is_aapcs_va_list(ap)) {
        ndk_va_from_list(v, (ndk_aapcs_va_list *)ap, gr, fr, 8);
    } else {
        memset(v, 0, sizeof *v);
        v->stack = (const uint64_t *)ap;  // a Darwin va_list: consecutive 8-byte slots
    }
}

// ---- wrappers ---------------------------------------------------------------------------------------

#define ARGS_MAX 32

#define DEFINE_CALLS(Type, jtype)                                                                      \
    static jtype w_Call##Type##Method(JNIEnv *env, jobject obj, jmethodID mid, VA_REST) {               \
        VA_BUILD(v, 0);                                                                                 \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, NULL, obj, mid, false, t, sizeof t);                             \
        to_jvalues(t, &v, a);                                                                           \
        return B->Call##Type##MethodA(env, obj, mid, a);                                                \
    }                                                                                                   \
    static jtype w_Call##Type##MethodV(JNIEnv *env, jobject obj, jmethodID mid, void *ap) {             \
        uint64_t gr[8]; double fr[8]; ndk_va v;                                                         \
        va_from_any(&v, ap, (uintptr_t)__builtin_return_address(0), gr, fr);                            \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, NULL, obj, mid, false, t, sizeof t);                             \
        to_jvalues(t, &v, a);                                                                           \
        return B->Call##Type##MethodA(env, obj, mid, a);                                                \
    }                                                                                                   \
    static jtype w_CallStatic##Type##Method(JNIEnv *env, jclass cls, jmethodID mid, VA_REST) {          \
        VA_BUILD(v, 0);                                                                                 \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, cls, NULL, mid, true, t, sizeof t);                                                      \
        to_jvalues(t, &v, a);                                                                           \
        return B->CallStatic##Type##MethodA(env, cls, mid, a);                                          \
    }                                                                                                   \
    static jtype w_CallStatic##Type##MethodV(JNIEnv *env, jclass cls, jmethodID mid, void *ap) {        \
        uint64_t gr[8]; double fr[8]; ndk_va v;                                                         \
        va_from_any(&v, ap, (uintptr_t)__builtin_return_address(0), gr, fr);                            \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, cls, NULL, mid, true, t, sizeof t);                                                      \
        to_jvalues(t, &v, a);                                                                           \
        return B->CallStatic##Type##MethodA(env, cls, mid, a);                                          \
    }                                                                                                   \
    /* CallNonvirtual: (env, obj, cls, mid, ...) — one more named register (x3). */                     \
    static jtype w_CallNonvirtual##Type##Method(JNIEnv *env, jobject obj, jclass cls, jmethodID mid,    \
                                               uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,      \
                                               double d0, double d1, double d2, double d3, double d4,   \
                                               double d5, double d6, double d7, uint64_t s0,            \
                                               uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4,      \
                                               uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8,      \
                                               uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12,   \
                                               uint64_t s13, uint64_t s14, uint64_t s15) {              \
        uint64_t x3 = 0;                                                                                \
        VA_BUILD(v, 0);                                                                                 \
        _gr[0] = x4; _gr[1] = x5; _gr[2] = x6; _gr[3] = x7; (void)x3;                                   \
        if (v.gn) v.gn = 4;                                                                             \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, cls, NULL, mid, false, t, sizeof t);                                                     \
        to_jvalues(t, &v, a);                                                                           \
        return B->CallNonvirtual##Type##MethodA(env, obj, cls, mid, a);                                 \
    }                                                                                                   \
    static jtype w_CallNonvirtual##Type##MethodV(JNIEnv *env, jobject obj, jclass cls, jmethodID mid,   \
                                                void *ap) {                                             \
        uint64_t gr[8]; double fr[8]; ndk_va v;                                                         \
        va_from_any(&v, ap, (uintptr_t)__builtin_return_address(0), gr, fr);                            \
        char t[ARGS_MAX]; jvalue a[ARGS_MAX];                                                           \
        sig_get(env, cls, NULL, mid, false, t, sizeof t);                                                     \
        to_jvalues(t, &v, a);                                                                           \
        return B->CallNonvirtual##Type##MethodA(env, obj, cls, mid, a);                                 \
    }

DEFINE_CALLS(Object, jobject)
DEFINE_CALLS(Boolean, jboolean)
DEFINE_CALLS(Byte, jbyte)
DEFINE_CALLS(Char, jchar)
DEFINE_CALLS(Short, jshort)
DEFINE_CALLS(Int, jint)
DEFINE_CALLS(Long, jlong)
DEFINE_CALLS(Float, jfloat)
DEFINE_CALLS(Double, jdouble)
DEFINE_CALLS(Void, void)

static jobject w_NewObject(JNIEnv *env, jclass cls, jmethodID mid, VA_REST) {
    VA_BUILD(v, 0);
    char t[ARGS_MAX];
    jvalue a[ARGS_MAX];
    sig_get(env, cls, NULL, mid, false, t, sizeof t);
    to_jvalues(t, &v, a);
    return B->NewObjectA(env, cls, mid, a);
}
static jobject w_NewObjectV(JNIEnv *env, jclass cls, jmethodID mid, void *ap) {
    uint64_t gr[8];
    double fr[8];
    ndk_va v;
    va_from_any(&v, ap, (uintptr_t)__builtin_return_address(0), gr, fr);
    char t[ARGS_MAX];
    jvalue a[ARGS_MAX];
    sig_get(env, cls, NULL, mid, false, t, sizeof t);
    to_jvalues(t, &v, a);
    return B->NewObjectA(env, cls, mid, a);
}

#define SET(Type)                                                                     \
    g_table.Call##Type##Method = (void *)w_Call##Type##Method;                        \
    g_table.Call##Type##MethodV = (void *)w_Call##Type##MethodV;                      \
    g_table.CallStatic##Type##Method = (void *)w_CallStatic##Type##Method;            \
    g_table.CallStatic##Type##MethodV = (void *)w_CallStatic##Type##MethodV;          \
    g_table.CallNonvirtual##Type##Method = (void *)w_CallNonvirtual##Type##Method;    \
    g_table.CallNonvirtual##Type##MethodV = (void *)w_CallNonvirtual##Type##MethodV;

void ndk_jni_install(void) {
    if (!andropkg_jni_base_table || !andropkg_jni_set_table_override) {
        ndk_log(NDK_LOG_ERROR, "runtime lacks the JNI table hooks: variadic JNI calls from native code will break");
        return;
    }
    B = andropkg_jni_base_table();
    g_table = *B;
    g_table.GetMethodID = w_GetMethodID;
    g_table.GetStaticMethodID = w_GetStaticMethodID;
    SET(Object) SET(Boolean) SET(Byte) SET(Char) SET(Short) SET(Int) SET(Long) SET(Float) SET(Double) SET(Void)
    g_table.NewObject = (void *)w_NewObject;
    g_table.NewObjectV = (void *)w_NewObjectV;
    andropkg_jni_set_table_override(&g_table);
    ndk_log(NDK_LOG_INFO, "JNI table for native code installed");
}
