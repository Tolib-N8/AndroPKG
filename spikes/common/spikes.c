#include "spikes.h"

#include "andropkg/loader.h"
#include "andropkg/syscall.h"
#include "andropkg/tls.h"

#include <inttypes.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

enum { PASS, FAIL, INFO };

static void report(spike_log_fn log, spike_summary *sum, int kind, const char *name,
                   const char *fmt, ...) {
    char detail[512], line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    static const char *const tags[] = {"PASS", "FAIL", "INFO"};
    snprintf(line, sizeof line, "[SPIKE] %s %s: %s", tags[kind], name, detail);
    log(line);
    if (kind == PASS) sum->passed++;
    else if (kind == FAIL) sum->failed++;
    else sum->info++;
}

static double ns_per_tick(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)tb.numer / tb.denom;
}

// ---------------------------------------------------------------------------------------
// (a) TPIDR_EL0

static inline uint64_t read_tpidr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}
static inline void write_tpidr(uint64_t v) { __asm__ volatile("msr tpidr_el0, %0" ::"r"(v)); }

typedef struct {
    int idx;
    uint64_t initial;
    int mismatches;
    uint64_t first_bad;
} tp_arg;

static void *tp_thread(void *p) {
    tp_arg *a = p;
    a->initial = read_tpidr();
    uint64_t v = 0xA5A5000000001234ull | (uint64_t)(a->idx + 1) << 16;
    write_tpidr(v);
    for (int i = 0; i < 400; i++) {
        if (i % 4 == 0) sched_yield();
        else usleep(150);  // blocking syscall + likely migration between cores
        uint64_t now = read_tpidr();
        if (now != v && a->mismatches++ == 0) a->first_bad = now;
    }
    write_tpidr(a->initial);
    return NULL;
}

static void spike_tpidr(spike_log_fn log, spike_summary *sum) {
    enum { N = 16 };  // more threads than cores to force preemption
    pthread_t th[N];
    tp_arg args[N];
    for (int i = 0; i < N; i++) {
        args[i] = (tp_arg){.idx = i};
        pthread_create(&th[i], NULL, tp_thread, &args[i]);
    }
    int bad = 0;
    uint64_t first_bad = 0;
    bool initial_nonzero = false;
    for (int i = 0; i < N; i++) {
        pthread_join(th[i], NULL);
        bad += args[i].mismatches;
        if (args[i].mismatches && !first_bad) first_bad = args[i].first_bad;
        if (args[i].initial) initial_nonzero = true;
    }
    report(log, sum, INFO, "tpidr_el0.initial", "main=0x%" PRIx64 " thread0=0x%" PRIx64 "%s",
           read_tpidr(), args[0].initial,
           initial_nonzero ? " (non-zero: OS may own it)" : " (zero: unused by OS)");
    // Informational: the converter rewrites tpidr_el0 accesses either way.
    report(log, sum, INFO, "tpidr_el0.preserved",
           bad == 0 ? "%d threads x 400 switches kept their own TPIDR_EL0"
                    : "%d threads: %d mismatches (e.g. read 0x%" PRIx64
                      ") -> OS owns TPIDR_EL0; converter rewrites mrs tpidr_el0 to a TSD slot",
           N, bad, first_bad);
}

// ---------------------------------------------------------------------------------------
// (c) x18

static void spike_x18(spike_log_fn log, spike_summary *sum) {
    const uint64_t magic = 0x1818181818181818ull;
    uint64_t after;
    __asm__ volatile("mov x18, %0" ::"r"(magic));  // x18 is reserved: compiler never touches it
    for (int i = 0; i < 50; i++) usleep(1000);
    __asm__ volatile("mov %0, x18" : "=r"(after));
    report(log, sum, INFO, "x18",
           after == magic ? "preserved across 50 sleeps (still must not rely on it)"
                          : "clobbered (now 0x%" PRIx64 ") -> Android code must not use x18; NDK "
                            "default -ffixed-x18 makes this safe",
           after);
}

// ---------------------------------------------------------------------------------------
// (d) SIGSEGV with PC rewrite

static volatile sig_atomic_t segv_hits;
static void *volatile segv_addr;

static void segv_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    ucontext_t *uc = ucv;
    segv_hits++;
    segv_addr = si->si_addr;
    uc->uc_mcontext->__ss.__x[0] = 0xDEAD;  // "exception object"
    uc->uc_mcontext->__ss.__pc += 4;        // skip the faulting load
}

static void spike_segv(spike_log_fn log, spike_summary *sum) {
    struct sigaction sa = {0}, old_segv, old_bus;
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    uint64_t r = 0;
    const uintptr_t bad = 0x10;  // null + field offset, like an implicit null check
    for (int i = 0; i < 3; i++)
        __asm__ volatile("mov x1, %1\n\tldr x0, [x1]\n\tmov %0, x0" : "=r"(r) : "r"(bad) : "x0", "x1", "memory");

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    if (segv_hits == 3 && r == 0xDEAD && segv_addr == (void *)bad)
        report(log, sum, PASS, "sigsegv.resume", "3 faults at %p caught, PC rewritten, resumed",
               (void *)bad);
    else
        report(log, sum, FAIL, "sigsegv.resume", "hits=%d r=0x%" PRIx64 " addr=%p",
               (int)segv_hits, r, segv_addr);
}

static void spike_tsd(spike_log_fn log, spike_summary *sum) {
    char d[256];
    bool ok = andropkg_tls_selfcheck(d, sizeof d);
    report(log, sum, ok ? PASS : FAIL, "tls.tsd_slot",
           "%s -> tpidrro_el0 TSD slot %s carry the bionic TP", d, ok ? "can" : "can NOT");
}

// ---------------------------------------------------------------------------------------
// (e) Memory below 4 GB — ART's Java heap uses 32-bit compressed references.

static uint64_t pagezero_size(void) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        const struct segment_command_64 *seg = (const void *)lc;
        if (lc->cmd == LC_SEGMENT_64 && strcmp(seg->segname, "__PAGEZERO") == 0) return seg->vmsize;
    }
    return 0;
}

static void spike_low4gb(spike_log_fn log, spike_summary *sum) {
    report(log, sum, INFO, "low4gb.pagezero", "__PAGEZERO = %#llx (main image at %p)",
           (unsigned long long)pagezero_size(), (void *)_dyld_get_image_header(0));
    const vm_size_t size = 256ull << 20;  // ART reserves big regions
    const vm_address_t candidates[] = {0x10000000, 0x40000000, 0x80000000, 0xC0000000};
    int ok = 0;
    char detail[256] = "";
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        vm_address_t a = candidates[i];
        // VM_FLAGS_FIXED fails (instead of clobbering) if anything is mapped there.
        kern_return_t kr = vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED);
        if (kr == KERN_SUCCESS) {
            volatile uint32_t *p = (volatile uint32_t *)a;
            p[0] = 0xA5A5A5A5u;
            p[size / 4 - 1] = 0x5A5A5A5Au;
            if (p[0] == 0xA5A5A5A5u && p[size / 4 - 1] == 0x5A5A5A5Au) ok++;
            vm_deallocate(mach_task_self(), a, size);
        }
        size_t n = strlen(detail);
        snprintf(detail + n, sizeof detail - n, "%s%#llx:%s", n ? " " : "",
                 (unsigned long long)candidates[i], kr == KERN_SUCCESS ? "ok" : mach_error_string(kr));
    }
    // ART's own strategy on non-Linux: mmap with a low hint and accept only < 4 GB.
    void *hint = mmap((void *)0x20000000, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    bool hint_low = hint != MAP_FAILED && (uintptr_t)hint + size <= 0x100000000ull;
    if (hint != MAP_FAILED) munmap(hint, size);
    // Informational: confirms the OS forbids it (ART's default assumption breaks).
    report(log, sum, INFO, "low4gb.alloc",
           "below-4GB: 256MB fixed: %s | mmap hint 0x20000000 -> %p%s", detail, hint,
           hint_low ? " (below 4GB)" : " (NOT below 4GB -> ART needs base-relative refs)");
    (void)ok;

    // The workaround: one PROT_NONE reservation used as the compressed-reference heap arena
    // (ref = ptr - base, null = 0). No alignment needed. Find the largest contiguous size
    // iOS grants, then commit + write the top slice to prove the whole range is usable.
    static const size_t sizes_mb[] = {4096, 3072, 2048, 1536, 1024, 768, 512};
    size_t got = 0;
    uintptr_t base = 0;
    bool arena_ok = false;
    for (size_t i = 0; i < sizeof sizes_mb / sizeof *sizes_mb && !got; i++) {
        size_t arena = sizes_mb[i] << 20;
        void *r = mmap(NULL, arena, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) continue;
        got = sizes_mb[i];
        base = (uintptr_t)r;
        size_t slice_sz = 64u << 20;
        void *slice = mmap((void *)(base + arena - slice_sz), slice_sz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (slice != MAP_FAILED) {
            volatile uint64_t *q = slice;
            q[0] = 1; q[slice_sz / 8 - 1] = 2;
            arena_ok = q[0] == 1 && q[slice_sz / 8 - 1] == 2;
        }
        munmap(r, arena);
    }
    report(log, sum, arena_ok ? PASS : FAIL, "low4gb.arena",
           "largest PROT_NONE reservation: %zu MB at %#lx; top slice committed+written: %s", got,
           (unsigned long)base, arena_ok ? "ok -> compressed refs = ptr - base" : "FAILED");
}

void spike_run_cpu(spike_log_fn log, spike_summary *sum) {
    spike_low4gb(log, sum);
    spike_tpidr(log, sum);
    spike_tsd(log, sum);
    spike_x18(log, sum);
    spike_segv(log, sum);
}

// ---------------------------------------------------------------------------------------
// (b) wrapped ELF

static int host_counter = 7;
static int host_callback(int x) { return x + 100; }

static void host_stack_chk_fail(void) {
    fputs("[SPIKE] FAIL __stack_chk_fail called: stack guard mismatch\n", stdout);
    abort();
}

static void *spike_resolver(const char *name, void *ctx) {
    (void)ctx;
    if (strcmp(name, "host_callback") == 0) return (void *)host_callback;
    if (strcmp(name, "__stack_chk_fail") == 0) return (void *)host_stack_chk_fail;
    if (strcmp(name, "host_counter") == 0) return &host_counter;
    return NULL;
}

enum { TLS_THREADS = 8 };
typedef struct {
    long (*tp_slot)(int);
    int (*stack_prot)(const char *);
    int bad;
} tls_arg;

static void *tls_thread(void *p) {
    tls_arg *a = p;
    void **tp = andropkg_tls_attach_thread();
    for (int i = 0; i < 2000; i++) {
        if (i % 64 == 0) sched_yield();
        if (a->tp_slot(BIONIC_TLS_SLOT_THREAD_ID) != (long)tp[BIONIC_TLS_SLOT_THREAD_ID]) a->bad++;
        if (a->tp_slot(BIONIC_TLS_SLOT_STACK_GUARD) != (long)andropkg_tls_stack_guard()) a->bad++;
        if (a->stack_prot("xyz") != 'x' + 3) a->bad++;
    }
    return NULL;
}

#define CHECK(name, cond, ...)                                              \
    report(log, sum, (cond) ? PASS : FAIL, name, __VA_ARGS__)

void spike_run_elf(const char *path, const char *label, spike_log_fn log, spike_summary *sum) {
    char err[512] = "", name[96];
#define N(s) (snprintf(name, sizeof name, "elf[%s].%s", label, s), name)

    uint64_t t0 = mach_absolute_time();
    apk_image *img = apk_image_open(path, spike_resolver, NULL, err, sizeof err);
    double open_us = (mach_absolute_time() - t0) * ns_per_tick() / 1000.0;
    if (!img) {
        report(log, sum, FAIL, N("open"), "%s", err);
        return;
    }
    const apk_wrap_info *wi = apk_image_wrap_info(img);
    report(log, sum, PASS, N("open"),
           "soname=%s base=0x%lx rewritten: svc=%u mrs-tp=%u msr-tp=%u (%.0f us incl. dlopen)",
           apk_image_soname(img), (unsigned long)apk_image_base(img), wi->svc_count,
           wi->tp_read_count, wi->tp_write_count, open_us);

    int (*add)(int, int) = apk_image_sym(img, "spike_add");
    int (*ctor)(void) = apk_image_sym(img, "spike_ctor_value");
    const char *(*msg)(int) = apk_image_sym(img, "spike_message");
    int (*call_import)(int) = apk_image_sym(img, "spike_call_import");
    int (*via_ptr)(int) = apk_image_sym(img, "spike_call_via_ptr");
    int (*counter)(void) = apk_image_sym(img, "spike_read_counter");
    long (*sys_getpid)(void) = apk_image_sym(img, "spike_getpid");
    long (*sys_write)(const char *, long) = apk_image_sym(img, "spike_write");
    long (*sys_bad)(void) = apk_image_sym(img, "spike_bad_syscall");
    long (*regs)(void) = apk_image_sym(img, "spike_regs_survive_svc");
    long (*tp_slot)(int) = apk_image_sym(img, "spike_tp_slot");
    int (*stack_prot)(const char *) = apk_image_sym(img, "spike_stack_protected");
    if (!add || !ctor || !msg || !call_import || !via_ptr || !counter || !sys_getpid || !sys_write ||
        !sys_bad || !regs || !tp_slot || !stack_prot) {
        report(log, sum, FAIL, N("dlsym"), "missing exported symbol(s)");
        return;
    }

    CHECK(N("exec"), add(2, 3) == 5, "spike_add(2,3)=%d (unmodified arm64 ELF code runs)", add(2, 3));
    CHECK(N("init_array"), ctor() == 42, "constructor ran, value=%d", ctor());
    CHECK(N("reloc.relative"), !strcmp(msg(0), "hello") && !strcmp(msg(1), "world"),
          "pointer table -> \"%s\" \"%s\"", msg(0), msg(1));
    CHECK(N("reloc.jump_slot"), call_import(1) == 202, "PLT call to host -> %d", call_import(1));
    CHECK(N("reloc.abs64"), via_ptr(1) == 102, "fn pointer to host -> %d", via_ptr(1));
    host_counter = 9;
    CHECK(N("reloc.glob_dat"), counter() == 9, "data import via GOT -> %d", counter());

    uint64_t before = andropkg_syscall_count;
    long pid = sys_getpid();
    CHECK(N("svc.getpid"), pid == getpid(), "svc #0 getpid -> %ld (real %d)", pid, getpid());
    static const char hello[] = "[SPIKE]      hello from an Android ELF via svc #0 write()\n";
    long w = sys_write(hello, sizeof hello - 1);
    CHECK(N("svc.write"), w == (long)sizeof hello - 1, "write returned %ld", w);
    long bad = sys_bad(), regs_ok = regs();
    CHECK(N("svc.enosys"), bad == -38, "unknown syscall -> %ld (expect -ENOSYS)", bad);
    CHECK(N("svc.regs"), regs_ok == 1, "x9-x17,x19,d0/d7/d16 preserved across trap");
    CHECK(N("svc.counted"), andropkg_syscall_count - before == 4, "dispatcher saw %" PRIu64 " calls",
          andropkg_syscall_count - before);

    // bionic thread pointer via rewritten `mrs tpidr_el0`, on several threads at once.
    tls_arg targs[TLS_THREADS];
    pthread_t th[TLS_THREADS];
    for (int i = 0; i < TLS_THREADS; i++) {
        targs[i] = (tls_arg){.tp_slot = tp_slot, .stack_prot = stack_prot};
        pthread_create(&th[i], NULL, tls_thread, &targs[i]);
    }
    int tls_bad = 0;
    for (int i = 0; i < TLS_THREADS; i++) {
        pthread_join(th[i], NULL);
        tls_bad += targs[i].bad;
    }
    CHECK(N("tls.thread_pointer"), tls_bad == 0,
          "%d threads x 2000 reads: tp slot THREAD_ID/STACK_GUARD match own TCB (%d mismatches)",
          TLS_THREADS, tls_bad);
    andropkg_tls_attach_thread();
    int sp = stack_prot("abcdef");
    CHECK(N("tls.stack_protector"), sp == 'a' + 6, "-fstack-protector-strong function returned %d", sp);

    const int ITERS = 200000;
    t0 = mach_absolute_time();
    for (int i = 0; i < ITERS; i++) sys_getpid();
    double ns = (mach_absolute_time() - t0) * ns_per_tick() / ITERS;
    t0 = mach_absolute_time();
    for (int i = 0; i < ITERS; i++) add(i, 1);
    double ns_call = (mach_absolute_time() - t0) * ns_per_tick() / ITERS;
    andropkg_tls_attach_thread();
    t0 = mach_absolute_time();
    for (int i = 0; i < ITERS; i++) tp_slot(1);
    double ns_tp = (mach_absolute_time() - t0) * ns_per_tick() / ITERS;
    report(log, sum, INFO, N("perf"),
           "plain call %.1f ns; rewritten mrs-tp read %.1f ns; patched svc getpid %.1f ns", ns_call,
           ns_tp, ns);
#undef N
}
