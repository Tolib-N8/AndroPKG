// andropkg NDK layer — the NDK system libraries' symbols.
//
// For an import `name` of an Android image:
//   1. bionic_<name> exported by this library: an implementation with the bionic ABI
//      (structures, flag values, errno, AAPCS64 varargs) on top of Darwin;
//   2. a Darwin function on the direct list (ndk-libs/direct.txt): same name, identical ABI
//      (pure functions: string, ctype, math, ...);
//   3. otherwise, if some NDK system library exports it (ndk-libs/symbols/*.txt), the image
//      gets a trap stub that names the missing function when called.
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
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
    if (dladdr((void *)&ndk_system_impl, &d)) g_self = dlopen(d.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!g_self) g_self = RTLD_DEFAULT;
}

static void *self_handle(void) {
    pthread_once(&g_self_once, find_self);
    return g_self;
}

void *ndk_system_impl(const char *name) {
    char buf[256];
    if (strlen(name) + 8 >= sizeof buf) return NULL;
    snprintf(buf, sizeof buf, "bionic_%s", name);
    void *p = dlsym(self_handle(), buf);
    if (p) return p;
    if (in_sorted(kDirectSymbols, sizeof kDirectSymbols / sizeof *kDirectSymbols, name)) return dlsym(RTLD_DEFAULT, name);
    return NULL;
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
