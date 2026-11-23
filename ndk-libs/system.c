// andropkg NDK layer — the NDK system libraries' symbols.
//
// For an import `name` of an Android image:
//   1. bionic_<name> exported by this library: an implementation with the bionic ABI
//      (structures, flag values, errno, AAPCS64 varargs) on top of Darwin;
//   2. a Darwin function on the direct list (ndk-libs/direct.txt): same name, identical ABI
//      (pure functions: string, ctype, math, ...);
//   3. a libandroid/libjnigraphics/libnativewindow function the runtime implements (by name);
//   4. otherwise, if some NDK system library exports it (ndk-libs/symbols/*.txt), the image
//      gets a trap stub that names the missing function when called.
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndk_internal.h"
#include "ndk_symbols.inc"  // generated: kSystemSymbols[], kDirectSymbols[], kSystemLibs[]

extern int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap);

void ndk_log(int prio, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio, NDK_LOG_TAG, fmt, ap);
    va_end(ap);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static bool in_sorted(const char *const *list, size_t n, const char *name) {
    return bsearch(&name, list, n, sizeof *list, cmp_str) != NULL;
}

bool ndk_system_known(const char *name) {
    return in_sorted(kSystemSymbols, sizeof kSystemSymbols / sizeof *kSystemSymbols, name);
}

bool ndk_is_system_lib(const char *soname) {
    return in_sorted(kSystemLibs, sizeof kSystemLibs / sizeof *kSystemLibs, soname);
}

static void *g_self;
static pthread_once_t g_self_once = PTHREAD_ONCE_INIT;

static void find_self(void) {
    Dl_info d;
    // RTLD_FIRST: dlsym searches this image only, not its dependencies (libSystem, ...): most
    // lookups here miss (an app's own symbols), and a miss through the dependencies is slow.
    if (dladdr((void *)&ndk_system_impl, &d)) g_self = dlopen(d.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_FIRST);
    if (!g_self) g_self = RTLD_DEFAULT;
}

static void *self_handle(void) {
    pthread_once(&g_self_once, find_self);
    return g_self;
}

static void *g_libsystem;
static pthread_once_t g_libsystem_once = PTHREAD_ONCE_INIT;

static void find_libsystem(void) {
    g_libsystem = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!g_libsystem) g_libsystem = RTLD_DEFAULT;
}

// Darwin's own functions (the direct list): looked up in libSystem and the libraries it
// re-exports, not with RTLD_DEFAULT, which searches every image in the process (hundreds:
// most of a large app's library loading went there).
static void *darwin_symbol(const char *name) {
    pthread_once(&g_libsystem_once, find_libsystem);
    void *p = dlsym(g_libsystem, name);
    return p ? p : dlsym(RTLD_DEFAULT, name);
}

static void *system_impl_uncached(const char *name) {
    char buf[256];
    if (strlen(name) + 8 >= sizeof buf) return NULL;
    snprintf(buf, sizeof buf, "bionic_%s", name);
    void *p = dlsym(self_handle(), buf);
    if (p) return p;
    if ((p = andropkg_gl_symbol(name))) return p;  // EGL and the GL wrappers (runtime/opengl/egl.m)
    if (in_sorted(kDirectSymbols, sizeof kDirectSymbols / sizeof *kDirectSymbols, name)) return darwin_symbol(name);
    // libandroid / libjnigraphics / libnativewindow: implemented by the runtime itself
    // (runtime/android_runtime/ndk_android.cc, AOSP's asset_manager.cpp, ...), same C ABI.
    if (in_sorted(kPlatformSymbols, sizeof kPlatformSymbols / sizeof *kPlatformSymbols, name)) return dlsym(RTLD_DEFAULT, name);
    // libGLES*: the platform's GL (egl.m), where the ABI allows a direct call.
    if (in_sorted(kGlSymbols, sizeof kGlSymbols / sizeof *kGlSymbols, name)) return ndk_gl_proc(name);
    return NULL;
}

// Every library of an app imports the same libc/libm/... symbols, and each lookup that misses
// costs several dlsym calls: the results (misses too) are kept, by name. The NDK system
// libraries cannot change while the process runs.
#define NDK_SYM_CACHE (1 << 15)
static struct { const char *name; void *p; } g_sym_cache[NDK_SYM_CACHE];
static int g_sym_cache_n;
static pthread_mutex_t g_sym_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

void *ndk_system_impl(const char *name) {
    uint32_t i = name_hash(name) & (NDK_SYM_CACHE - 1);
    pthread_mutex_lock(&g_sym_cache_lock);
    for (; g_sym_cache[i].name; i = (i + 1) & (NDK_SYM_CACHE - 1)) {
        if (strcmp(g_sym_cache[i].name, name) == 0) {
            void *p = g_sym_cache[i].p;
            pthread_mutex_unlock(&g_sym_cache_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&g_sym_cache_lock);
    void *p = system_impl_uncached(name);
    pthread_mutex_lock(&g_sym_cache_lock);
    if (g_sym_cache_n < NDK_SYM_CACHE / 2) {  // (half full at most: probes stay short)
        i = name_hash(name) & (NDK_SYM_CACHE - 1);
        while (g_sym_cache[i].name && strcmp(g_sym_cache[i].name, name) != 0) i = (i + 1) & (NDK_SYM_CACHE - 1);
        if (!g_sym_cache[i].name) {
            g_sym_cache[i].p = p;
            g_sym_cache[i].name = strdup(name);
            g_sym_cache_n++;
        }
    }
    pthread_mutex_unlock(&g_sym_cache_lock);
    return p;
}

// ---- trap stubs --------------------------------------------------------------------------------

#define NDK_TRAPS 2048
static const char *g_trap_names[NDK_TRAPS];
static _Atomic bool g_trap_reported[NDK_TRAPS];
static _Atomic int g_ntraps;

static long trap_hit(int i) {
    if (!atomic_exchange(&g_trap_reported[i], true))
        ndk_log(NDK_LOG_WARN, "unimplemented NDK function called: %s (returning 0)", g_trap_names[i]);
    return 0;
}

#include "ndk_traps.inc"  // generated: static long trap_N(void) { return trap_hit(N); } + kTraps[]

void *ndk_trap_stub(const char *name) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    int n = atomic_load(&g_ntraps);
    for (int i = 0; i < n; i++) {
        if (strcmp(g_trap_names[i], name) == 0) {
            pthread_mutex_unlock(&m);
            return (void *)kTraps[i];
        }
    }
    void *p = NULL;
    if (n < NDK_TRAPS) {
        g_trap_names[n] = strdup(name);
        atomic_store(&g_ntraps, n + 1);
        p = (void *)kTraps[n];
    }
    pthread_mutex_unlock(&m);
    return p;
}
