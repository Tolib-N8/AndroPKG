// andropkg NDK layer — bionic libc: errno, process-level runtime (atexit, stack protector,
// abort, assert), system properties, sysconf, auxv, allocation extras and _FORTIFY_SOURCE.
#include <crt_externs.h>
#include <errno.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "andropkg/tls.h"
#include "ndk_internal.h"

// ---- errno -------------------------------------------------------------------------------------
// errno storage is Darwin's own (one per thread); bionic wrappers store Linux values into it.

static const short kDarwinToLinux[] = {
    [11] = 35,  [35] = 11,  [36] = 115, [37] = 114, [38] = 88,  [39] = 89,  [40] = 90,  [41] = 91,
    [42] = 92,  [43] = 93,  [44] = 94,  [45] = 95,  [46] = 96,  [47] = 97,  [48] = 98,  [49] = 99,
    [50] = 100, [51] = 101, [52] = 102, [53] = 103, [54] = 104, [55] = 105, [56] = 106, [57] = 107,
    [58] = 108, [59] = 109, [60] = 110, [61] = 111, [62] = 40,  [63] = 36,  [64] = 112, [65] = 113,
    [66] = 39,  [67] = 11,  [68] = 87,  [69] = 122, [70] = 116, [71] = 66,  [72] = 22,  [73] = 22,
    [74] = 22,  [75] = 22,  [76] = 22,  [77] = 37,  [78] = 38,  [79] = 22,  [80] = 13,  [81] = 13,
    [82] = 5,   [83] = 5,   [84] = 75,  [85] = 8,   [86] = 8,   [87] = 8,   [88] = 8,   [89] = 125,
    [90] = 43,  [91] = 42,  [92] = 84,  [93] = 61,  [94] = 74,  [95] = 72,  [96] = 61,  [97] = 67,
    [98] = 63,  [99] = 60,  [100] = 71, [101] = 62, [102] = 95, [103] = 1,  [104] = 131, [105] = 130,
    [106] = 105,
};

int ndk_errno_to_linux(int e) {
    if (e > 0 && e < (int)(sizeof kDarwinToLinux / sizeof *kDarwinToLinux) && kDarwinToLinux[e]) return kDarwinToLinux[e];
    return e;  // 1..34 (but 11) are the same
}

int ndk_errno_to_darwin(int e) {
    if (e == 11) return EAGAIN;
    if (e == 35) return EDEADLK;
    if (e <= 34) return e;
    for (int d = 35; d < (int)(sizeof kDarwinToLinux / sizeof *kDarwinToLinux); d++)
        if (kDarwinToLinux[d] == e) return d;
    return EINVAL;
}

void ndk_fix_errno(void) { errno = ndk_errno_to_linux(errno); }

NDK_EXPORT int *bionic___errno(void) { return &errno; }

NDK_EXPORT char *bionic_strerror(int e) {
    static __thread char buf[96];
    snprintf(buf, sizeof buf, "%s", strerror(ndk_errno_to_darwin(e)));
    return buf;
}

// GNU strerror_r (returns char*) is what bionic exports as __gnu_strerror_r; strerror_r is XSI.
NDK_EXPORT int bionic_strerror_r(int e, char *buf, size_t len) {
    return strerror_r(ndk_errno_to_darwin(e), buf, len) == 0 ? 0 : ERANGE;
}
NDK_EXPORT char *bionic___gnu_strerror_r(int e, char *buf, size_t len) {
    strerror_r(ndk_errno_to_darwin(e), buf, len);
    return buf;
}

NDK_EXPORT void bionic_perror(const char *s) {
    fprintf(stderr, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", bionic_strerror(errno));
}

// ---- atexit / __cxa_atexit / __cxa_finalize ------------------------------------------------------
// Every Android .so registers its C++ static destructors with __cxa_atexit(fn, arg, &__dso_handle)
// and calls __cxa_finalize(&__dso_handle) when unloaded. Destructors run at process exit.

typedef struct { void (*fn)(void *); void *arg; void *dso; } atexit_entry;
static atexit_entry *g_atexit;
static size_t g_natexit, g_capatexit;
static pthread_mutex_t g_atexit_lock = PTHREAD_MUTEX_INITIALIZER;

static void run_atexit(void) { extern void bionic___cxa_finalize(void *); bionic___cxa_finalize(NULL); }

NDK_EXPORT int bionic___cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    if (ndk_proc_atexit(fn, arg)) return 0;
    pthread_mutex_lock(&g_atexit_lock);
    if (g_natexit == g_capatexit) {
        g_capatexit = g_capatexit ? g_capatexit * 2 : 64;
        g_atexit = realloc(g_atexit, g_capatexit * sizeof *g_atexit);
        if (g_natexit == 0) atexit(run_atexit);
    }
    g_atexit[g_natexit++] = (atexit_entry){fn, arg, dso};
    pthread_mutex_unlock(&g_atexit_lock);
    return 0;
}

NDK_EXPORT void bionic___cxa_finalize(void *dso) {
    for (;;) {
        pthread_mutex_lock(&g_atexit_lock);
        atexit_entry e = {0};
        for (size_t i = g_natexit; i-- > 0;) {
            if (g_atexit[i].fn && (!dso || g_atexit[i].dso == dso)) {
                e = g_atexit[i];
                g_atexit[i].fn = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&g_atexit_lock);
        if (!e.fn) return;
        e.fn(e.arg);
    }
}

NDK_EXPORT int bionic_atexit(void (*fn)(void)) { return bionic___cxa_atexit((void (*)(void *))fn, NULL, NULL); }

// C++11 thread_local destructors: run when the thread exits.
typedef struct tl_dtor { void (*fn)(void *); void *obj; struct tl_dtor *next; } tl_dtor;
static pthread_key_t g_tl_key;
static pthread_once_t g_tl_once = PTHREAD_ONCE_INIT;
static void tl_run(void *head) {
    for (tl_dtor *d = head; d;) {
        tl_dtor *next = d->next;
        d->fn(d->obj);
        free(d);
        d = next;
    }
}
static void tl_init(void) { pthread_key_create(&g_tl_key, tl_run); }
NDK_EXPORT int bionic___cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso) {
    (void)dso;
    pthread_once(&g_tl_once, tl_init);
    tl_dtor *d = malloc(sizeof *d);
    d->fn = fn;
    d->obj = obj;
    d->next = pthread_getspecific(g_tl_key);
    pthread_setspecific(g_tl_key, d);
    return 0;
}

// ---- stack protector, abort, assert -----------------------------------------------------------

NDK_EXPORT uintptr_t bionic___stack_chk_guard;
__attribute__((constructor)) static void init_guard(void) { bionic___stack_chk_guard = andropkg_tls_stack_guard(); }

NDK_EXPORT void bionic___stack_chk_fail(void) {
    ndk_log(NDK_LOG_ERROR, "stack corruption detected (-fstack-protector)");
    abort();
}

static char g_abort_message[1024];
NDK_EXPORT void bionic_android_set_abort_message(const char *msg) {
    if (msg) strlcpy(g_abort_message, msg, sizeof g_abort_message);
    ndk_log(NDK_LOG_ERROR, "abort message: %s", g_abort_message);
}

NDK_EXPORT void bionic_abort(void) {
    ndk_proc_exit(134);  // SIGABRT, for a "process" (exec.c)
    ndk_log(NDK_LOG_ERROR, "abort() called from native code%s%s", g_abort_message[0] ? ": " : "", g_abort_message);
    abort();
}

NDK_EXPORT void bionic___assert(const char *file, int line, const char *failed) {
    ndk_log(NDK_LOG_ERROR, "%s:%d: assertion \"%s\" failed", file, line, failed);
    abort();
}
NDK_EXPORT void bionic___assert2(const char *file, int line, const char *function, const char *failed) {
    ndk_log(NDK_LOG_ERROR, "%s:%d: %s: assertion \"%s\" failed", file, line, function, failed);
    abort();
}

NDK_EXPORT void bionic_exit(int status) {
    ndk_proc_exit(status);  // inside an in-process "process": ends just that
    exit(status);
}

// ---- process / thread identity ---------------------------------------------------------------

NDK_EXPORT int bionic_gettid(void) {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)(tid & 0x7fffffff);
}

NDK_EXPORT char **bionic_environ;
NDK_EXPORT const char *bionic___progname = "app_process64";
__attribute__((constructor)) static void init_environ(void) { bionic_environ = *_NSGetEnviron(); }

NDK_EXPORT const char *bionic_getprogname(void) { return bionic___progname; }

// ---- system properties (__system_property_*) ---------------------------------------------------
// Values come from the runtime's property store (build.prop, ro.* set by andropkg).

extern int __system_property_get(const char *name, char *value) __attribute__((weak));

NDK_EXPORT int bionic___system_property_get(const char *name, char *value) {
    value[0] = 0;
    if (__system_property_get) return __system_property_get(name, value);
    return 0;
}

typedef struct { char name[96]; } prop_info;
NDK_EXPORT const prop_info *bionic___system_property_find(const char *name) {
    // A stable handle per name: the value is read again on each __system_property_read*.
    static prop_info infos[256];
    static int n;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    char v[92];
    if (bionic___system_property_get(name, v) <= 0) return NULL;
    pthread_mutex_lock(&m);
    for (int i = 0; i < n; i++) {
        if (strcmp(infos[i].name, name) == 0) { pthread_mutex_unlock(&m); return &infos[i]; }
    }
    prop_info *pi = n < 256 ? &infos[n++] : NULL;
    if (pi) strlcpy(pi->name, name, sizeof pi->name);
    pthread_mutex_unlock(&m);
    return pi;
}

NDK_EXPORT int bionic___system_property_read(const prop_info *pi, char *name, char *value) {
    if (name) strlcpy(name, pi->name, 32);
    return bionic___system_property_get(pi->name, value);
}

NDK_EXPORT void bionic___system_property_read_callback(const prop_info *pi,
        void (*cb)(void *cookie, const char *name, const char *value, uint32_t serial), void *cookie) {
    char v[92];
    bionic___system_property_get(pi->name, v);
    cb(cookie, pi->name, v, 0);
}

// ---- sysconf / auxv / page size --------------------------------------------------------------

NDK_EXPORT long bionic_sysconf(int name) {
    // bionic/libc/include/bits/sysconf.h values.
    switch (name) {
        case 0x0000: return sysconf(_SC_ARG_MAX);
        case 0x0005: return sysconf(_SC_CHILD_MAX);
        case 0x0006: return 100;                            // _SC_CLK_TCK
        case 0x0009: return 2048;                           // _SC_LINE_MAX
        case 0x000a: return 65536;                          // _SC_NGROUPS_MAX
        case 0x000b: return sysconf(_SC_OPEN_MAX);
        case 0x0017: return 1;                              // _SC_JOB_CONTROL
        case 0x0018: return 1;                              // _SC_SAVED_IDS
        case 0x0019: return 200809L;                        // _SC_VERSION
        case 0x001b: return 8;                              // _SC_STREAM_MAX
        case 0x001c: return 6;                              // _SC_TZNAME_MAX
        case 0x0026: return 1024;                           // _SC_IOV_MAX
        case 0x0027:
        case 0x0028: return getpagesize();                  // _SC_PAGESIZE / _SC_PAGE_SIZE
        case 0x0034: return 32;                             // _SC_RTSIG_MAX
        case 0x0037: return 32;                             // _SC_SIGQUEUE_MAX
        case 0x0047:
        case 0x0048: return 1024;                           // _SC_GETGR/GETPW_R_SIZE_MAX
        case 0x004c: return 16384;                          // _SC_THREAD_STACK_MIN
        case 0x0060: return sysconf(_SC_NPROCESSORS_CONF);
        case 0x0061: return sysconf(_SC_NPROCESSORS_ONLN);
        case 0x0062: return sysconf(_SC_PHYS_PAGES);
        case 0x0063: return sysconf(_SC_PHYS_PAGES) / 4;    // _SC_AVPHYS_PAGES (estimate)
        case 0x006f: return 255;                            // _SC_HOST_NAME_MAX
        case 0x0094: return 64;                             // _SC_LEVEL1_DCACHE_LINESIZE
        default:
            errno = 22;  // EINVAL
            return -1;
    }
}

NDK_EXPORT int bionic_getpagesize(void) { return getpagesize(); }

NDK_EXPORT unsigned long bionic_getauxval(unsigned long type) {
    switch (type) {
        case 6: return (unsigned long)getpagesize();  // AT_PAGESZ
        case 16:  // AT_HWCAP — what Apple's cores have. Not HWCAP_CPUID (0x800): Linux emulates
                  // EL0 reads of ID_AA64* registers, Darwin raises SIGILL (compiler-rt's cpu
                  // feature detection checks this bit). No SVE, no PACA/PACG (arm64, not arm64e).
            return 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 |  // fp asimd aes pmull sha1 sha2 crc32 atomics fphp asimdhp
                   0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x10000 | 0x20000 |                 // asimdrdm jscvt fcma lrcpc dcpop sha3
                   0x100000 | 0x200000 | 0x800000;                                          // asimddp sha512 asimdfhm
        case 26: return 0;                            // AT_HWCAP2
        case 17: return 100;                          // AT_CLKTCK
        case 23: return 0;                            // AT_SECURE
        default:
            errno = 2;
            return 0;
    }
}

// ---- allocation extras -----------------------------------------------------------------------

NDK_EXPORT void *bionic_memalign(size_t align, size_t size) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}
NDK_EXPORT void *bionic_pvalloc(size_t size) {
    size_t pg = (size_t)getpagesize();
    return bionic_memalign(pg, (size + pg - 1) & ~(pg - 1));
}
NDK_EXPORT size_t bionic_malloc_usable_size(const void *p) { return malloc_size(p); }
NDK_EXPORT void *bionic_reallocarray(void *p, size_t n, size_t sz) {
    if (sz && n > SIZE_MAX / sz) { errno = 12; return NULL; }
    return realloc(p, n * sz);
}
struct bionic_mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
NDK_EXPORT struct bionic_mallinfo bionic_mallinfo(void) {
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    struct bionic_mallinfo mi = {0};
    mi.arena = st.size_allocated;
    mi.uordblks = st.size_in_use;
    mi.fordblks = st.size_allocated - st.size_in_use;
    return mi;
}
NDK_EXPORT int bionic_mallopt(int param, int value) { (void)param; (void)value; return 1; }

// ---- _FORTIFY_SOURCE (*_chk): same semantics, with the checks ----------------------------------

static void fortify_fail(const char *what) {
    ndk_log(NDK_LOG_ERROR, "FORTIFY: %s: prevented buffer overflow", what);
    abort();
}
#define CHK(cond, name) do { if (!(cond)) fortify_fail(name); } while (0)

NDK_EXPORT void *bionic___memcpy_chk(void *d, const void *s, size_t n, size_t dlen) { CHK(n <= dlen, "memcpy"); return memcpy(d, s, n); }
NDK_EXPORT void *bionic___memmove_chk(void *d, const void *s, size_t n, size_t dlen) { CHK(n <= dlen, "memmove"); return memmove(d, s, n); }
NDK_EXPORT void *bionic___memset_chk(void *d, int c, size_t n, size_t dlen) { CHK(n <= dlen, "memset"); return memset(d, c, n); }
NDK_EXPORT char *bionic___strcpy_chk(char *d, const char *s, size_t dlen) { CHK(strlen(s) < dlen, "strcpy"); return strcpy(d, s); }
NDK_EXPORT char *bionic___stpcpy_chk(char *d, const char *s, size_t dlen) { CHK(strlen(s) < dlen, "stpcpy"); return stpcpy(d, s); }
NDK_EXPORT char *bionic___strcat_chk(char *d, const char *s, size_t dlen) { CHK(strlen(d) + strlen(s) < dlen, "strcat"); return strcat(d, s); }
NDK_EXPORT char *bionic___strncpy_chk(char *d, const char *s, size_t n, size_t dlen) { CHK(n <= dlen, "strncpy"); return strncpy(d, s, n); }
NDK_EXPORT char *bionic___strncpy_chk2(char *d, const char *s, size_t n, size_t dlen, size_t slen) { (void)slen; CHK(n <= dlen, "strncpy"); return strncpy(d, s, n); }
NDK_EXPORT char *bionic___stpncpy_chk(char *d, const char *s, size_t n, size_t dlen) { CHK(n <= dlen, "stpncpy"); return stpncpy(d, s, n); }
NDK_EXPORT char *bionic___stpncpy_chk2(char *d, const char *s, size_t n, size_t dlen, size_t slen) { (void)slen; CHK(n <= dlen, "stpncpy"); return stpncpy(d, s, n); }
NDK_EXPORT char *bionic___strncat_chk(char *d, const char *s, size_t n, size_t dlen) { CHK(strlen(d) + strnlen(s, n) < dlen, "strncat"); return strncat(d, s, n); }
NDK_EXPORT size_t bionic___strlen_chk(const char *s, size_t slen) { size_t n = strlen(s); CHK(n < slen, "strlen"); return n; }
NDK_EXPORT char *bionic___strchr_chk(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
NDK_EXPORT char *bionic___strrchr_chk(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
NDK_EXPORT size_t bionic___strlcpy_chk(char *d, const char *s, size_t n, size_t dlen) { CHK(n <= dlen, "strlcpy"); return strlcpy(d, s, n); }
NDK_EXPORT size_t bionic___strlcat_chk(char *d, const char *s, size_t n, size_t dlen) { CHK(n <= dlen, "strlcat"); return strlcat(d, s, n); }
NDK_EXPORT void *bionic___memchr_chk(const void *s, int c, size_t n, size_t slen) { CHK(n <= slen, "memchr"); return memchr(s, c, n); }
NDK_EXPORT void *bionic_memrchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return NULL;
}
NDK_EXPORT void *bionic___memrchr_chk(const void *s, int c, size_t n, size_t slen) { CHK(n <= slen, "memrchr"); return bionic_memrchr(s, c, n); }
NDK_EXPORT char *bionic___fgets_chk(char *b, int n, FILE *f, size_t blen) {
    extern char *bionic_fgets(char *, int, FILE *);
    CHK((size_t)n <= blen, "fgets");
    return bionic_fgets(b, n, f);
}
NDK_EXPORT void bionic___FD_SET_chk(int fd, fd_set *set, size_t setlen) { CHK(fd >= 0 && fd < 1024 && setlen >= sizeof(fd_set), "FD_SET"); FD_SET(fd, set); }
NDK_EXPORT void bionic___FD_CLR_chk(int fd, fd_set *set, size_t setlen) { CHK(fd >= 0 && fd < 1024 && setlen >= sizeof(fd_set), "FD_CLR"); FD_CLR(fd, set); }
NDK_EXPORT int bionic___FD_ISSET_chk(int fd, const fd_set *set, size_t setlen) { CHK(fd >= 0 && fd < 1024 && setlen >= sizeof(fd_set), "FD_ISSET"); return FD_ISSET(fd, set); }
NDK_EXPORT int bionic___umask_chk(int mode) { return umask((mode_t)mode); }
NDK_EXPORT mode_t bionic_umask(mode_t mode) { return umask(mode); }

// basename/dirname: bionic's never modify their argument (POSIX allows Darwin's to).
NDK_EXPORT char *bionic_basename(const char *path) {
    static __thread char buf[1024];
    const char *s = strrchr(path, '/');
    strlcpy(buf, s ? s + 1 : path, sizeof buf);
    return buf;
}
NDK_EXPORT char *bionic___gnu_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return (char *)(s ? s + 1 : path);
}

// ---- syscall(2): Linux numbers through the shim's dispatcher (also behind `svc #0`) ------------
#include "andropkg/syscall.h"
// Linux arm64 syscall numbers served by the NDK layer's translating wrappers (paths, flags,
// structs): raw syscall() users (root checkers, crash reporters) get the same semantics.
typedef long (*sysfn)(long, long, long, long, long, long);
#define W(name) extern long bionic_##name();
#define WRAPPERS(X) X(openat) X(fstat) X(fstatat) X(readlinkat) X(faccessat) X(unlinkat) X(mkdirat) \
    X(renameat) X(lseek) X(pipe2) X(dup) X(dup3) X(ppoll) X(prctl) X(uname) X(nanosleep)                \
    X(clock_nanosleep) X(kill) X(madvise) X(mmap) X(munmap) X(mprotect) X(sched_getaffinity) X(statfs) \
    X(fstatfs) X(getrlimit) X(eventfd) X(epoll_create1) X(epoll_ctl) X(epoll_pwait) X(socket)           \
    X(connect) X(sendto) X(recvfrom) X(sigaltstack) X(sigprocmask) X(ioctl) X(fcntl) X(sigaction)
WRAPPERS(W)
#undef W
static sysfn syscall_wrapper(long nr) {
    switch (nr) {
    case 56: return (sysfn)bionic_openat;
    case 80: return (sysfn)bionic_fstat;
    case 79: return (sysfn)bionic_fstatat;
    case 78: return (sysfn)bionic_readlinkat;
    case 48: return (sysfn)bionic_faccessat;
    case 35: return (sysfn)bionic_unlinkat;
    case 34: return (sysfn)bionic_mkdirat;
    case 38: return (sysfn)bionic_renameat;
    case 62: return (sysfn)bionic_lseek;
    case 59: return (sysfn)bionic_pipe2;
    case 23: return (sysfn)bionic_dup;
    case 24: return (sysfn)bionic_dup3;
    case 73: return (sysfn)bionic_ppoll;
    case 167: return (sysfn)bionic_prctl;
    case 160: return (sysfn)bionic_uname;
    case 101: return (sysfn)bionic_nanosleep;
    case 115: return (sysfn)bionic_clock_nanosleep;
    case 129: return (sysfn)bionic_kill;
    case 233: return (sysfn)bionic_madvise;
    case 222: return (sysfn)bionic_mmap;
    case 215: return (sysfn)bionic_munmap;
    case 226: return (sysfn)bionic_mprotect;
    case 123: return (sysfn)bionic_sched_getaffinity;
    case 43: return (sysfn)bionic_statfs;
    case 44: return (sysfn)bionic_fstatfs;
    case 163: return (sysfn)bionic_getrlimit;
    case 19: return (sysfn)bionic_eventfd;  // eventfd2(initval, flags)
    case 20: return (sysfn)bionic_epoll_create1;
    case 21: return (sysfn)bionic_epoll_ctl;
    case 22: return (sysfn)bionic_epoll_pwait;
    case 198: return (sysfn)bionic_socket;
    case 203: return (sysfn)bionic_connect;
    case 206: return (sysfn)bionic_sendto;
    case 207: return (sysfn)bionic_recvfrom;
    case 132: return (sysfn)bionic_sigaltstack;
    case 135: return (sysfn)bionic_sigprocmask;  // rt_sigprocmask(how, set, old, sigsetsize)
    case 134: return (sysfn)bionic_sigaction;    // rt_sigaction(sig, act, old, sigsetsize)
    case 29: return (sysfn)bionic_ioctl;
    case 25: return (sysfn)bionic_fcntl;
    }
    return NULL;
}

NDK_EXPORT long bionic_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    sysfn f = syscall_wrapper(nr);
    if (f) {  // (errno set by it)
        long r = f(a0, a1, a2, a3, a4, a5);
        bool wide = nr == 222 || nr == 62 || nr == 206 || nr == 207;  // mmap, lseek, sendto, recvfrom
        return wide ? r : (long)(int)r;  // the others return int: the upper half is undefined
    }
    long r = andropkg_syscall(nr, a0, a1, a2, a3, a4, a5);
    if (r < 0 && r > -4096) {
        if (r == -38) ndk_log(NDK_LOG_WARN, "syscall %ld is not emulated", nr);
        errno = (int)-r;
        return -1;
    }
    return r;
}
NDK_EXPORT ssize_t bionic_getrandom(void *buf, size_t n, unsigned flags) { (void)flags; arc4random_buf(buf, n); return (ssize_t)n; }
NDK_EXPORT int bionic_getentropy(void *buf, size_t n) { arc4random_buf(buf, n); return 0; }
