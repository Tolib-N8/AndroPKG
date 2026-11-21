// Linux arm64 syscall → Darwin translation (M0: the handful the spikes exercise; grows
// with the libc shim in M4).
#include "andropkg/syscall.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

_Atomic uint64_t andropkg_syscall_count;

enum {
    LNX_ENOSYS = 38,
    LNX_EINVAL = 22,
};

// Linux arm64 (asm-generic) syscall numbers.
enum {
    NR_close = 57,
    NR_read = 63,
    NR_write = 64,
    NR_exit = 93,
    NR_exit_group = 94,
    NR_clock_gettime = 113,
    NR_sched_yield = 124,
    NR_getpid = 172,
    NR_getppid = 173,
    NR_getuid = 174,
    NR_geteuid = 175,
    NR_getgid = 176,
    NR_getegid = 177,
    NR_gettid = 178,
    NR_futex = 98,
    NR_getrandom = 278,
    NR_membarrier = 283,
    NR_getcpu = 168,
};

// runtime/compat/src/futex_darwin.cc (part of the andropkg runtime this library runs in).
extern int andropkg_futex(volatile int *uaddr, int op, int val, const struct timespec *timeout,
                          volatile int *uaddr2, int val3) __attribute__((weak));

int andropkg_errno_to_linux(int e) {
    if (e > 0 && e <= 34) return e;  // EPERM..ERANGE are identical
    switch (e) {
    case EAGAIN: return 11;
    case EINPROGRESS: return 115;
    case EALREADY: return 114;
    case ENOTSOCK: return 88;
    case EDESTADDRREQ: return 89;
    case EMSGSIZE: return 90;
    case EPROTOTYPE: return 91;
    case ENOPROTOOPT: return 92;
    case EPROTONOSUPPORT: return 93;
    case ESOCKTNOSUPPORT: return 94;
    case ENOTSUP: return 95;
    case EOPNOTSUPP: return 95;
    case EPFNOSUPPORT: return 96;
    case EAFNOSUPPORT: return 97;
    case EADDRINUSE: return 98;
    case EADDRNOTAVAIL: return 99;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ENETRESET: return 102;
    case ECONNABORTED: return 103;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EISCONN: return 106;
    case ENOTCONN: return 107;
    case ESHUTDOWN: return 108;
    case ETOOMANYREFS: return 109;
    case ETIMEDOUT: return 110;
    case ECONNREFUSED: return 111;
    case ELOOP: return 40;
    case ENAMETOOLONG: return 36;
    case EHOSTDOWN: return 112;
    case EHOSTUNREACH: return 113;
    case ENOTEMPTY: return 39;
    case EUSERS: return 87;
    case EDQUOT: return 122;
    case ESTALE: return 116;
    case EREMOTE: return 66;
    case ENOLCK: return 37;
    case ENOSYS: return 38;
    case EOVERFLOW: return 75;
    case ECANCELED: return 125;
    case EIDRM: return 43;
    case ENOMSG: return 42;
    case EILSEQ: return 84;
    case ENOATTR: return 61;  // ENODATA
    case EBADMSG: return 74;
    case EMULTIHOP: return 72;
    case ENODATA: return 61;
    case ENOLINK: return 67;
    case ENOSR: return 63;
    case ENOSTR: return 60;
    case EPROTO: return 71;
    case ETIME: return 62;
    case ENOTRECOVERABLE: return 131;
    case EOWNERDEAD: return 130;
    default: return LNX_EINVAL;
    }
}

static long ret(long r) { return r < 0 ? -andropkg_errno_to_linux(errno) : r; }

static int linux_clock_to_darwin(long id, clockid_t *out) {
    switch (id) {
    case 0: case 5: *out = CLOCK_REALTIME; return 0;           // REALTIME, REALTIME_COARSE
    case 1: case 6: case 7: *out = CLOCK_MONOTONIC; return 0;  // MONOTONIC, *_COARSE, BOOTTIME
    case 2: *out = CLOCK_PROCESS_CPUTIME_ID; return 0;
    case 3: *out = CLOCK_THREAD_CPUTIME_ID; return 0;
    case 4: *out = CLOCK_MONOTONIC_RAW; return 0;
    default: return -1;
    }
}

long andropkg_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)a3; (void)a4; (void)a5;
    atomic_fetch_add_explicit(&andropkg_syscall_count, 1, memory_order_relaxed);
    switch (nr) {
    case NR_read: return ret(read((int)a0, (void *)a1, (size_t)a2));
    case NR_write: return ret(write((int)a0, (const void *)a1, (size_t)a2));
    case NR_close: return ret(close((int)a0));
    case NR_exit: pthread_exit(NULL);
    case NR_exit_group: _exit((int)a0);
    case NR_sched_yield: return ret(sched_yield());
    case NR_getpid: return getpid();
    case NR_getppid: return getppid();
    case NR_getuid: return getuid();
    case NR_geteuid: return geteuid();
    case NR_getgid: return getgid();
    case NR_getegid: return getegid();
    case NR_gettid: {
        uint64_t tid = 0;
        pthread_threadid_np(NULL, &tid);
        return (long)(tid & 0x7fffffff);
    }
    case NR_clock_gettime: {
        clockid_t c;
        if (linux_clock_to_darwin(a0, &c) != 0) return -LNX_EINVAL;
        return ret(clock_gettime(c, (struct timespec *)a1));  // timespec layout matches on LP64
    }
    case NR_futex: {
        if (!andropkg_futex) return -LNX_ENOSYS;
        int r = andropkg_futex((volatile int *)a0, (int)a1, (int)a2, (const struct timespec *)a3,
                               (volatile int *)a4, (int)a5);
        return r < 0 ? -andropkg_errno_to_linux(errno) : r;
    }
    case NR_getrandom:
        arc4random_buf((void *)a0, (size_t)a1);
        return a1;
    case NR_membarrier:
        return 0;
    case NR_getcpu:
        if (a0) *(unsigned *)a0 = 0;
        if (a1) *(unsigned *)a1 = 0;
        return 0;
    default:
        return -LNX_ENOSYS;
    }
}
