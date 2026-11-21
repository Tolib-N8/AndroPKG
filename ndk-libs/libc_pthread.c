// andropkg NDK layer — bionic pthreads and semaphores on Darwin.
//
// bionic's synchronization objects are smaller than Darwin's (pthread_mutex_t: 40 vs 64
// bytes; pthread_once_t / pthread_key_t: int), so each bionic object holds a pointer to a
// lazily created Darwin object (offset 8; offset 0 keeps bionic's static-initializer bits).
// Error numbers returned by pthread functions are translated to Linux values.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "andropkg/tls.h"
#include "ndk_internal.h"

#define LNX(r) ndk_errno_to_linux(r)

// ---- mutex ---------------------------------------------------------------------------------------

typedef struct { int32_t value; int32_t pad; _Atomic(pthread_mutex_t *) impl; char rest[24]; } b_mutex;  // 40 bytes
typedef long b_mutexattr;  // bits 0-1: type (NORMAL 0, RECURSIVE 1, ERRORCHECK 2), bit 4: pshared
_Static_assert(sizeof(b_mutex) == 40, "bionic pthread_mutex_t");

static int darwin_type(int t) {
    return t == 1 ? PTHREAD_MUTEX_RECURSIVE : t == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
}

static pthread_mutex_t *mutex_impl(b_mutex *m) {
    pthread_mutex_t *p = atomic_load_explicit(&m->impl, memory_order_acquire);
    if (p) return p;
    p = malloc(sizeof *p);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    // Static initializers: PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP sets 1 << 14, ERRORCHECK 2 << 14.
    pthread_mutexattr_settype(&a, darwin_type((m->value >> 14) & 3));
    pthread_mutex_init(p, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&m->impl, &expected, p)) {
        pthread_mutex_destroy(p);
        free(p);
        p = expected;
    }
    return p;
}

NDK_EXPORT int bionic_pthread_mutex_init(b_mutex *m, const b_mutexattr *attr) {
    memset(m, 0, sizeof *m);
    m->value = (int32_t)((attr ? (*attr & 3) : 0) << 14);
    mutex_impl(m);
    return 0;
}
NDK_EXPORT int bionic_pthread_mutex_destroy(b_mutex *m) {
    pthread_mutex_t *p = atomic_exchange(&m->impl, NULL);
    if (p) { pthread_mutex_destroy(p); free(p); }
    return 0;
}
NDK_EXPORT int bionic_pthread_mutex_lock(b_mutex *m) { return LNX(pthread_mutex_lock(mutex_impl(m))); }
NDK_EXPORT int bionic_pthread_mutex_trylock(b_mutex *m) { return LNX(pthread_mutex_trylock(mutex_impl(m))); }
NDK_EXPORT int bionic_pthread_mutex_unlock(b_mutex *m) { return LNX(pthread_mutex_unlock(mutex_impl(m))); }

static struct timespec rel_from_abs(clockid_t clock, const struct timespec *abs) {
    struct timespec now, rel;
    clock_gettime(clock, &now);
    rel.tv_sec = abs->tv_sec - now.tv_sec;
    rel.tv_nsec = abs->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000; rel.tv_sec--; }
    if (rel.tv_sec < 0) rel.tv_sec = rel.tv_nsec = 0;
    return rel;
}

static int mutex_timedlock(b_mutex *m, clockid_t clock, const struct timespec *abs) {
    pthread_mutex_t *p = mutex_impl(m);
    for (;;) {
        int r = pthread_mutex_trylock(p);
        if (r != EBUSY) return LNX(r);
        struct timespec rel = rel_from_abs(clock, abs);
        if (rel.tv_sec == 0 && rel.tv_nsec == 0) return 110;  // ETIMEDOUT
        usleep(500);
    }
}
NDK_EXPORT int bionic_pthread_mutex_timedlock(b_mutex *m, const struct timespec *abs) { return mutex_timedlock(m, CLOCK_REALTIME, abs); }
NDK_EXPORT int bionic_pthread_mutex_timedlock_monotonic_np(b_mutex *m, const struct timespec *abs) { return mutex_timedlock(m, CLOCK_MONOTONIC, abs); }
NDK_EXPORT int bionic_pthread_mutex_clocklock(b_mutex *m, int clock, const struct timespec *abs) {
    return mutex_timedlock(m, clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, abs);
}

NDK_EXPORT int bionic_pthread_mutexattr_init(b_mutexattr *a) { *a = 0; return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_destroy(b_mutexattr *a) { (void)a; return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_settype(b_mutexattr *a, int t) {
    if (t < 0 || t > 2) return 22;
    *a = (*a & ~3L) | t;
    return 0;
}
NDK_EXPORT int bionic_pthread_mutexattr_gettype(const b_mutexattr *a, int *t) { *t = (int)(*a & 3); return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_setpshared(b_mutexattr *a, int s) { *a = (*a & ~16L) | (s ? 16 : 0); return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_getpshared(const b_mutexattr *a, int *s) { *s = (*a & 16) != 0; return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_setprotocol(b_mutexattr *a, int p) { (void)a; (void)p; return 0; }
NDK_EXPORT int bionic_pthread_mutexattr_getprotocol(const b_mutexattr *a, int *p) { (void)a; *p = 0; return 0; }

// ---- condition variables -------------------------------------------------------------------------

typedef struct { pthread_cond_t c; clockid_t clock; } cond_impl;
typedef struct { int32_t state; int32_t pad; _Atomic(cond_impl *) impl; char rest[32]; } b_cond;  // 48 bytes
typedef long b_condattr;  // bit 0: pshared; bits 1-3: clock (0 REALTIME, 1 MONOTONIC)
_Static_assert(sizeof(b_cond) == 48, "bionic pthread_cond_t");

static cond_impl *cond_get(b_cond *c) {
    cond_impl *p = atomic_load_explicit(&c->impl, memory_order_acquire);
    if (p) return p;
    p = malloc(sizeof *p);
    pthread_cond_init(&p->c, NULL);
    p->clock = ((c->state >> 1) & 7) == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME;
    cond_impl *expected = NULL;
    if (!atomic_compare_exchange_strong(&c->impl, &expected, p)) {
        pthread_cond_destroy(&p->c);
        free(p);
        p = expected;
    }
    return p;
}

NDK_EXPORT int bionic_pthread_cond_init(b_cond *c, const b_condattr *attr) {
    memset(c, 0, sizeof *c);
    c->state = (int32_t)(attr ? *attr : 0);
    cond_get(c);
    return 0;
}
NDK_EXPORT int bionic_pthread_cond_destroy(b_cond *c) {
    cond_impl *p = atomic_exchange(&c->impl, NULL);
    if (p) { pthread_cond_destroy(&p->c); free(p); }
    return 0;
}
NDK_EXPORT int bionic_pthread_cond_signal(b_cond *c) { return LNX(pthread_cond_signal(&cond_get(c)->c)); }
NDK_EXPORT int bionic_pthread_cond_broadcast(b_cond *c) { return LNX(pthread_cond_broadcast(&cond_get(c)->c)); }
NDK_EXPORT int bionic_pthread_cond_wait(b_cond *c, b_mutex *m) { return LNX(pthread_cond_wait(&cond_get(c)->c, mutex_impl(m))); }

static int cond_wait_until(b_cond *c, b_mutex *m, clockid_t clock, const struct timespec *abs) {
    struct timespec rel = rel_from_abs(clock, abs);
    return LNX(pthread_cond_timedwait_relative_np(&cond_get(c)->c, mutex_impl(m), &rel));
}
NDK_EXPORT int bionic_pthread_cond_timedwait(b_cond *c, b_mutex *m, const struct timespec *abs) {
    return cond_wait_until(c, m, cond_get(c)->clock, abs);
}
NDK_EXPORT int bionic_pthread_cond_timedwait_monotonic_np(b_cond *c, b_mutex *m, const struct timespec *abs) {
    return cond_wait_until(c, m, CLOCK_MONOTONIC, abs);
}
NDK_EXPORT int bionic_pthread_cond_timedwait_monotonic(b_cond *c, b_mutex *m, const struct timespec *abs) {
    return cond_wait_until(c, m, CLOCK_MONOTONIC, abs);
}
NDK_EXPORT int bionic_pthread_cond_clockwait(b_cond *c, b_mutex *m, int clock, const struct timespec *abs) {
    return cond_wait_until(c, m, clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, abs);
}
NDK_EXPORT int bionic_pthread_cond_timedwait_relative_np(b_cond *c, b_mutex *m, const struct timespec *rel) {
    return LNX(pthread_cond_timedwait_relative_np(&cond_get(c)->c, mutex_impl(m), rel));
}
NDK_EXPORT int bionic_pthread_condattr_init(b_condattr *a) { *a = 0; return 0; }
NDK_EXPORT int bionic_pthread_condattr_destroy(b_condattr *a) { (void)a; return 0; }
NDK_EXPORT int bionic_pthread_condattr_setclock(b_condattr *a, int clock) {
    if (clock != 0 && clock != 1 && clock != 7) return 22;
    *a = (*a & ~(7L << 1)) | ((clock == 0 ? 0L : 1L) << 1);
    return 0;
}
NDK_EXPORT int bionic_pthread_condattr_getclock(const b_condattr *a, int *clock) { *clock = ((*a >> 1) & 7) ? 1 : 0; return 0; }
NDK_EXPORT int bionic_pthread_condattr_setpshared(b_condattr *a, int s) { *a = (*a & ~1L) | (s ? 1 : 0); return 0; }
NDK_EXPORT int bionic_pthread_condattr_getpshared(const b_condattr *a, int *s) { *s = (int)(*a & 1); return 0; }

// ---- rwlock ---------------------------------------------------------------------------------------

typedef struct { int32_t state; int32_t pad; _Atomic(pthread_rwlock_t *) impl; char rest[40]; } b_rwlock;  // 56 bytes
_Static_assert(sizeof(b_rwlock) == 56, "bionic pthread_rwlock_t");

static pthread_rwlock_t *rw_get(b_rwlock *l) {
    pthread_rwlock_t *p = atomic_load_explicit(&l->impl, memory_order_acquire);
    if (p) return p;
    p = malloc(sizeof *p);
    pthread_rwlock_init(p, NULL);
    pthread_rwlock_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&l->impl, &expected, p)) {
        pthread_rwlock_destroy(p);
        free(p);
        p = expected;
    }
    return p;
}
NDK_EXPORT int bionic_pthread_rwlock_init(b_rwlock *l, const long *attr) { (void)attr; memset(l, 0, sizeof *l); rw_get(l); return 0; }
NDK_EXPORT int bionic_pthread_rwlock_destroy(b_rwlock *l) {
    pthread_rwlock_t *p = atomic_exchange(&l->impl, NULL);
    if (p) { pthread_rwlock_destroy(p); free(p); }
    return 0;
}
NDK_EXPORT int bionic_pthread_rwlock_rdlock(b_rwlock *l) { return LNX(pthread_rwlock_rdlock(rw_get(l))); }
NDK_EXPORT int bionic_pthread_rwlock_wrlock(b_rwlock *l) { return LNX(pthread_rwlock_wrlock(rw_get(l))); }
NDK_EXPORT int bionic_pthread_rwlock_tryrdlock(b_rwlock *l) { return LNX(pthread_rwlock_tryrdlock(rw_get(l))); }
NDK_EXPORT int bionic_pthread_rwlock_trywrlock(b_rwlock *l) { return LNX(pthread_rwlock_trywrlock(rw_get(l))); }
NDK_EXPORT int bionic_pthread_rwlock_unlock(b_rwlock *l) { return LNX(pthread_rwlock_unlock(rw_get(l))); }
static int rw_timed(b_rwlock *l, bool write, const struct timespec *abs, clockid_t clock) {
    for (;;) {
        int r = write ? pthread_rwlock_trywrlock(rw_get(l)) : pthread_rwlock_tryrdlock(rw_get(l));
        if (r != EBUSY) return LNX(r);
        struct timespec rel = rel_from_abs(clock, abs);
        if (rel.tv_sec == 0 && rel.tv_nsec == 0) return 110;
        usleep(500);
    }
}
NDK_EXPORT int bionic_pthread_rwlock_timedrdlock(b_rwlock *l, const struct timespec *abs) { return rw_timed(l, false, abs, CLOCK_REALTIME); }
NDK_EXPORT int bionic_pthread_rwlock_timedwrlock(b_rwlock *l, const struct timespec *abs) { return rw_timed(l, true, abs, CLOCK_REALTIME); }
NDK_EXPORT int bionic_pthread_rwlockattr_init(long *a) { *a = 0; return 0; }
NDK_EXPORT int bionic_pthread_rwlockattr_destroy(long *a) { (void)a; return 0; }
NDK_EXPORT int bionic_pthread_rwlockattr_setpshared(long *a, int s) { *a = s; return 0; }
NDK_EXPORT int bionic_pthread_rwlockattr_getpshared(const long *a, int *s) { *s = (int)*a; return 0; }
NDK_EXPORT int bionic_pthread_rwlockattr_setkind_np(long *a, int k) { (void)a; (void)k; return 0; }
NDK_EXPORT int bionic_pthread_rwlockattr_getkind_np(const long *a, int *k) { (void)a; *k = 0; return 0; }

// ---- spinlock, barrier ---------------------------------------------------------------------------

NDK_EXPORT int bionic_pthread_spin_init(_Atomic int *l, int pshared) { (void)pshared; atomic_store(l, 0); return 0; }
NDK_EXPORT int bionic_pthread_spin_destroy(_Atomic int *l) { (void)l; return 0; }
NDK_EXPORT int bionic_pthread_spin_lock(_Atomic int *l) {
    while (atomic_exchange_explicit(l, 1, memory_order_acquire)) sched_yield();
    return 0;
}
NDK_EXPORT int bionic_pthread_spin_trylock(_Atomic int *l) { return atomic_exchange_explicit(l, 1, memory_order_acquire) ? 16 : 0; }
NDK_EXPORT int bionic_pthread_spin_unlock(_Atomic int *l) { atomic_store_explicit(l, 0, memory_order_release); return 0; }

typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned count, waiting, phase; } barrier_impl;
typedef struct { barrier_impl *impl; char rest[24]; } b_barrier;  // 32 bytes
NDK_EXPORT int bionic_pthread_barrier_init(b_barrier *b, const void *attr, unsigned count) {
    (void)attr;
    if (count == 0) return 22;
    barrier_impl *p = calloc(1, sizeof *p);
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    p->count = count;
    b->impl = p;
    return 0;
}
NDK_EXPORT int bionic_pthread_barrier_destroy(b_barrier *b) {
    if (b->impl) { pthread_mutex_destroy(&b->impl->m); pthread_cond_destroy(&b->impl->c); free(b->impl); b->impl = NULL; }
    return 0;
}
NDK_EXPORT int bionic_pthread_barrier_wait(b_barrier *b) {
    barrier_impl *p = b->impl;
    pthread_mutex_lock(&p->m);
    unsigned phase = p->phase;
    if (++p->waiting == p->count) {
        p->waiting = 0;
        p->phase++;
        pthread_cond_broadcast(&p->c);
        pthread_mutex_unlock(&p->m);
        return -1;  // PTHREAD_BARRIER_SERIAL_THREAD
    }
    while (phase == p->phase) pthread_cond_wait(&p->c, &p->m);
    pthread_mutex_unlock(&p->m);
    return 0;
}
NDK_EXPORT int bionic_pthread_barrierattr_init(long *a) { *a = 0; return 0; }
NDK_EXPORT int bionic_pthread_barrierattr_destroy(long *a) { (void)a; return 0; }

// ---- once, keys --------------------------------------------------------------------------------

NDK_EXPORT int bionic_pthread_once(_Atomic int *once, void (*init)(void)) {
    static pthread_mutex_t m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    if (atomic_load_explicit(once, memory_order_acquire) == 2) return 0;
    pthread_mutex_lock(&m);
    if (atomic_load(once) == 0) {
        atomic_store(once, 1);
        init();
        atomic_store_explicit(once, 2, memory_order_release);
    }
    pthread_mutex_unlock(&m);
    return 0;
}

NDK_EXPORT int bionic_pthread_key_create(int *key, void (*dtor)(void *)) {
    pthread_key_t k;
    int r = pthread_key_create(&k, dtor);
    if (r) return LNX(r);
    *key = (int)k;
    return 0;
}
NDK_EXPORT int bionic_pthread_key_delete(int key) { return LNX(pthread_key_delete((pthread_key_t)key)); }
NDK_EXPORT void *bionic_pthread_getspecific(int key) { return pthread_getspecific((pthread_key_t)key); }
NDK_EXPORT int bionic_pthread_setspecific(int key, const void *v) { return LNX(pthread_setspecific((pthread_key_t)key, v)); }

// ---- threads -------------------------------------------------------------------------------------

typedef struct {
    uint32_t flags;  // bit 0: detached
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} b_attr;
_Static_assert(sizeof(b_attr) == 56, "bionic pthread_attr_t");

NDK_EXPORT int bionic_pthread_attr_init(b_attr *a) {
    memset(a, 0, sizeof *a);
    a->stack_size = 1024 * 1024 - 2 * 4096;  // bionic's default
    a->guard_size = 4096;
    return 0;
}
NDK_EXPORT int bionic_pthread_attr_destroy(b_attr *a) { (void)a; return 0; }
NDK_EXPORT int bionic_pthread_attr_setdetachstate(b_attr *a, int s) { a->flags = (a->flags & ~1u) | (s ? 1u : 0u); return 0; }
NDK_EXPORT int bionic_pthread_attr_getdetachstate(const b_attr *a, int *s) { *s = (int)(a->flags & 1); return 0; }
NDK_EXPORT int bionic_pthread_attr_setstacksize(b_attr *a, size_t s) { if (s < 16384) return 22; a->stack_size = s; return 0; }
NDK_EXPORT int bionic_pthread_attr_getstacksize(const b_attr *a, size_t *s) { *s = a->stack_size; return 0; }
NDK_EXPORT int bionic_pthread_attr_setstack(b_attr *a, void *base, size_t s) { a->stack_base = base; a->stack_size = s; return 0; }
NDK_EXPORT int bionic_pthread_attr_getstack(const b_attr *a, void **base, size_t *s) { *base = a->stack_base; *s = a->stack_size; return 0; }
NDK_EXPORT int bionic_pthread_attr_setguardsize(b_attr *a, size_t s) { a->guard_size = s; return 0; }
NDK_EXPORT int bionic_pthread_attr_getguardsize(const b_attr *a, size_t *s) { *s = a->guard_size; return 0; }
NDK_EXPORT int bionic_pthread_attr_setschedpolicy(b_attr *a, int p) { a->sched_policy = p; return 0; }
NDK_EXPORT int bionic_pthread_attr_getschedpolicy(const b_attr *a, int *p) { *p = a->sched_policy; return 0; }
NDK_EXPORT int bionic_pthread_attr_setschedparam(b_attr *a, const struct sched_param *p) { a->sched_priority = p->sched_priority; return 0; }
NDK_EXPORT int bionic_pthread_attr_getschedparam(const b_attr *a, struct sched_param *p) { p->sched_priority = a->sched_priority; return 0; }
NDK_EXPORT int bionic_pthread_attr_setscope(b_attr *a, int s) { (void)a; return s == 0 ? 0 : 95; }
NDK_EXPORT int bionic_pthread_attr_getscope(const b_attr *a, int *s) { (void)a; *s = 0; return 0; }
NDK_EXPORT int bionic_pthread_attr_setinheritsched(b_attr *a, int s) { (void)a; (void)s; return 0; }
NDK_EXPORT int bionic_pthread_attr_getinheritsched(const b_attr *a, int *s) { (void)a; *s = 0; return 0; }

typedef struct { void *(*fn)(void *); void *arg; ndk_proc *proc; } start_info;

static void *thread_start(void *p) {
    start_info si = *(start_info *)p;
    free(p);
    andropkg_tls_attach_thread();  // bionic TCB before any Android code runs
    ndk_set_current_proc(si.proc);  // threads of an in-process "process" belong to it
    return si.fn(si.arg);
}

NDK_EXPORT int bionic_pthread_create(pthread_t *t, const b_attr *battr, void *(*fn)(void *), void *arg) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (battr) {
        if (battr->flags & 1) pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        size_t ss = battr->stack_size < 65536 ? 65536 : battr->stack_size;
        ss = (ss + 16383) & ~(size_t)16383;
        if (battr->stack_base) pthread_attr_setstack(&a, battr->stack_base, battr->stack_size);
        else pthread_attr_setstacksize(&a, ss);
    } else {
        pthread_attr_setstacksize(&a, 1024 * 1024);
    }
    start_info *si = malloc(sizeof *si);
    si->fn = fn;
    si->arg = arg;
    si->proc = ndk_current_proc();
    int r = pthread_create(t, &a, thread_start, si);
    pthread_attr_destroy(&a);
    if (r) free(si);
    return LNX(r);
}

NDK_EXPORT int bionic_pthread_join(pthread_t t, void **ret) { return LNX(pthread_join(t, ret)); }
NDK_EXPORT int bionic_pthread_detach(pthread_t t) { return LNX(pthread_detach(t)); }
NDK_EXPORT pthread_t bionic_pthread_self(void) { return pthread_self(); }
NDK_EXPORT int bionic_pthread_equal(pthread_t a, pthread_t b) { return pthread_equal(a, b); }
NDK_EXPORT void bionic_pthread_exit(void *ret) { pthread_exit(ret); }
NDK_EXPORT int bionic_pthread_setname_np(pthread_t t, const char *name) {
    if (!pthread_equal(t, pthread_self())) return 0;  // Darwin names only the calling thread
    char buf[64];
    strlcpy(buf, name, sizeof buf);
    return LNX(pthread_setname_np(buf));
}
NDK_EXPORT int bionic_pthread_getname_np(pthread_t t, char *buf, size_t n) { return LNX(pthread_getname_np(t, buf, n)); }
NDK_EXPORT int bionic_pthread_gettid_np(pthread_t t) {
    uint64_t tid = 0;
    pthread_threadid_np(t, &tid);
    return (int)(tid & 0x7fffffff);
}
NDK_EXPORT int bionic_pthread_getattr_np(pthread_t t, b_attr *a) {
    bionic_pthread_attr_init(a);
    size_t size = pthread_get_stacksize_np(t);
    a->stack_size = size;
    a->stack_base = (char *)pthread_get_stackaddr_np(t) - size;  // bionic: the lowest address
    return 0;
}
NDK_EXPORT int bionic_pthread_getcpuclockid(pthread_t t, int *clock) { (void)t; *clock = 3; return 0; }  // THREAD_CPUTIME
static int to_darwin_policy(int p) { return p == 1 ? SCHED_FIFO : p == 2 ? SCHED_RR : SCHED_OTHER; }
NDK_EXPORT int bionic_pthread_setschedparam(pthread_t t, int policy, const struct sched_param *p) {
    pthread_setschedparam(t, to_darwin_policy(policy), p);
    return 0;  // best effort: Android apps often ask for priorities they don't get anyway
}
NDK_EXPORT int bionic_pthread_getschedparam(pthread_t t, int *policy, struct sched_param *p) {
    int dp;
    int r = pthread_getschedparam(t, &dp, p);
    *policy = dp == SCHED_FIFO ? 1 : dp == SCHED_RR ? 2 : 0;
    return LNX(r);
}
NDK_EXPORT int bionic_pthread_setschedprio(pthread_t t, int prio) { (void)t; (void)prio; return 0; }
NDK_EXPORT int bionic_pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void)) {
    return LNX(pthread_atfork(prepare, parent, child));
}

// __pthread_cleanup_push/pop (the pthread_cleanup_push macros)
typedef struct b_cleanup { struct b_cleanup *prev; void (*routine)(void *); void *arg; } b_cleanup;
static __thread b_cleanup *t_cleanup;
NDK_EXPORT void bionic___pthread_cleanup_push(b_cleanup *c, void (*routine)(void *), void *arg) {
    c->prev = t_cleanup;
    c->routine = routine;
    c->arg = arg;
    t_cleanup = c;
}
NDK_EXPORT void bionic___pthread_cleanup_pop(b_cleanup *c, int execute) {
    t_cleanup = c->prev;
    if (execute) c->routine(c->arg);
}

// sched_* (policy numbering differs; priorities are advisory)
NDK_EXPORT int bionic_sched_yield(void) { return sched_yield(); }
NDK_EXPORT int bionic_sched_get_priority_max(int p) { return p == 0 ? 0 : 99; }
NDK_EXPORT int bionic_sched_get_priority_min(int p) { return p == 0 ? 0 : 1; }
NDK_EXPORT int bionic_sched_getscheduler(int pid) { (void)pid; return 0; }
NDK_EXPORT int bionic_sched_setscheduler(int pid, int p, const struct sched_param *sp) { (void)pid; (void)p; (void)sp; return 0; }
NDK_EXPORT int bionic_sched_getparam(int pid, struct sched_param *sp) { (void)pid; sp->sched_priority = 0; return 0; }
NDK_EXPORT int bionic_sched_setparam(int pid, const struct sched_param *sp) { (void)pid; (void)sp; return 0; }
NDK_EXPORT int bionic_sched_getcpu(void) { return 0; }
NDK_EXPORT int bionic_sched_setaffinity(int pid, size_t n, const void *set) { (void)pid; (void)n; (void)set; return 0; }
NDK_EXPORT int bionic_sched_getaffinity(int pid, size_t n, void *set) {
    (void)pid;
    memset(set, 0, n);
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < cpus && (size_t)(i / 8) < n; i++) ((unsigned char *)set)[i / 8] |= (unsigned char)(1 << (i % 8));
    return 0;
}
NDK_EXPORT int bionic___sched_cpucount(size_t n, const void *set) {
    int c = 0;
    for (size_t i = 0; i < n; i++) c += __builtin_popcount(((const unsigned char *)set)[i]);
    return c;
}

// ---- semaphores (bionic sem_t: 16 bytes; Darwin has no unnamed semaphores) ----------------------

typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned value; } sem_impl;
typedef struct { uint32_t pad; uint32_t pad2; sem_impl *impl; } b_sem;  // 16 bytes
_Static_assert(sizeof(b_sem) == 16, "bionic sem_t");

NDK_EXPORT int bionic_sem_init(b_sem *s, int pshared, unsigned value) {
    (void)pshared;
    sem_impl *p = calloc(1, sizeof *p);
    pthread_mutex_init(&p->m, NULL);
    pthread_cond_init(&p->c, NULL);
    p->value = value;
    s->impl = p;
    return 0;
}
NDK_EXPORT int bionic_sem_destroy(b_sem *s) {
    if (s->impl) { pthread_mutex_destroy(&s->impl->m); pthread_cond_destroy(&s->impl->c); free(s->impl); s->impl = NULL; }
    return 0;
}
NDK_EXPORT int bionic_sem_post(b_sem *s) {
    pthread_mutex_lock(&s->impl->m);
    s->impl->value++;
    pthread_cond_signal(&s->impl->c);
    pthread_mutex_unlock(&s->impl->m);
    return 0;
}
static int sem_wait_until(b_sem *s, clockid_t clock, const struct timespec *abs) {
    sem_impl *p = s->impl;
    pthread_mutex_lock(&p->m);
    while (p->value == 0) {
        if (abs) {
            struct timespec rel = rel_from_abs(clock, abs);
            if ((rel.tv_sec == 0 && rel.tv_nsec == 0) ||
                pthread_cond_timedwait_relative_np(&p->c, &p->m, &rel) == ETIMEDOUT) {
                if (p->value) break;
                pthread_mutex_unlock(&p->m);
                errno = 110;
                return -1;
            }
        } else {
            pthread_cond_wait(&p->c, &p->m);
        }
    }
    p->value--;
    pthread_mutex_unlock(&p->m);
    return 0;
}
NDK_EXPORT int bionic_sem_wait(b_sem *s) { return sem_wait_until(s, CLOCK_REALTIME, NULL); }
NDK_EXPORT int bionic_sem_timedwait(b_sem *s, const struct timespec *abs) { return sem_wait_until(s, CLOCK_REALTIME, abs); }
NDK_EXPORT int bionic_sem_timedwait_monotonic_np(b_sem *s, const struct timespec *abs) { return sem_wait_until(s, CLOCK_MONOTONIC, abs); }
NDK_EXPORT int bionic_sem_clockwait(b_sem *s, int clock, const struct timespec *abs) {
    return sem_wait_until(s, clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, abs);
}
NDK_EXPORT int bionic_sem_trywait(b_sem *s) {
    sem_impl *p = s->impl;
    pthread_mutex_lock(&p->m);
    int r = 0;
    if (p->value) p->value--;
    else { errno = 11; r = -1; }  // EAGAIN
    pthread_mutex_unlock(&p->m);
    return r;
}
NDK_EXPORT int bionic_sem_getvalue(b_sem *s, int *v) { *v = (int)s->impl->value; return 0; }
