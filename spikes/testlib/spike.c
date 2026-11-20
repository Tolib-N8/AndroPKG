// Freestanding Android/arm64 shared object used by the M0 spikes.
// Exercises: RELATIVE + GLOB_DAT + JUMP_SLOT + ABS64 relocations, init_array,
// raw Linux syscalls via `svc #0`, and a data import from the host.

extern int host_callback(int);   // resolved by the andropkg loader (JUMP_SLOT)
extern int host_counter;         // data import (GLOB_DAT)

static int ctor_value;
static volatile int ctor_seed = 42;  // volatile: stops clang evaluating the ctor at compile time
__attribute__((constructor)) static void spike_init(void) { ctor_value = ctor_seed; }

const char *spike_messages[] = {"hello", "world"};  // RELATIVE relocs (exported => not folded)
int (*spike_cb_ptr)(int) = host_callback;                   // ABS64 against import

int spike_add(int a, int b) { return a + b; }
int spike_call_import(int x) { return host_callback(x) * 2; }
int spike_call_via_ptr(int x) { return spike_cb_ptr(x) + 1; }
int spike_ctor_value(void) { return ctor_value; }
const char *spike_message(int i) { return spike_messages[i & 1]; }
int spike_read_counter(void) { return host_counter; }

static long linux_syscall3(long nr, long a, long b, long c) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

long spike_getpid(void) { return linux_syscall3(172, 0, 0, 0); }          // __NR_getpid
long spike_write(const char *s, long n) { return linux_syscall3(64, 1, (long)s, n); }  // __NR_write
long spike_bad_syscall(void) { return linux_syscall3(0x7fff, 0, 0, 0); }   // expect -ENOSYS

// Callee-saved/scratch registers must survive a patched syscall exactly like a real kernel trap.
long spike_regs_survive_svc(void) {
    long ok;
    __asm__ volatile(
        "mov x9, #0x111\n"  "mov x10, #0x222\n" "mov x15, #0x333\n"
        "mov x16, #0x444\n" "mov x17, #0x555\n" "mov x19, #0x666\n"
        "fmov d0, x9\n"      "fmov d7, x10\n"     "fmov d16, x15\n"
        "mov x8, #172\n"     "svc #0\n"
        "mov %0, #1\n"
        "cmp x9, #0x111\n"  "csel %0, %0, xzr, eq\n"
        "cmp x10, #0x222\n" "csel %0, %0, xzr, eq\n"
        "cmp x15, #0x333\n" "csel %0, %0, xzr, eq\n"
        "cmp x16, #0x444\n" "csel %0, %0, xzr, eq\n"
        "cmp x17, #0x555\n" "csel %0, %0, xzr, eq\n"
        "cmp x19, #0x666\n" "csel %0, %0, xzr, eq\n"
        "fmov x11, d0\n"     "cmp x11, #0x111\n" "csel %0, %0, xzr, eq\n"
        "fmov x11, d7\n"     "cmp x11, #0x222\n" "csel %0, %0, xzr, eq\n"
        "fmov x11, d16\n"    "cmp x11, #0x333\n" "csel %0, %0, xzr, eq\n"
        : "=&r"(ok)
        :
        : "x0", "x8", "x9", "x10", "x11", "x15", "x16", "x17", "x19", "d0", "d7", "d16", "cc", "memory");
    return ok;
}

// bionic thread pointer: `mrs xN, tpidr_el0` is rewritten by the converter.
long spike_tp_slot(int idx) { return ((long *)__builtin_thread_pointer())[idx]; }

// With -fstack-protector-strong, Android clang loads the guard from [tpidr_el0 + 0x28]
// (TLS_SLOT_STACK_GUARD) in the prologue and re-checks it in the epilogue.
int spike_stack_protected(const char *s) {
    char buf[64];
    int n = 0;
    while (s[n] && n < 63) { buf[n] = s[n]; n++; }
    buf[n] = 0;
    volatile char *p = buf;
    return p[0] + n;
}
