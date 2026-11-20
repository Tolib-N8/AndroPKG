// bionic thread pointer emulation.
//
// Android code reads its thread pointer with `mrs Xt, tpidr_el0` (bionic slots, ELF TLS,
// and — on every -fstack-protector function — the stack guard at [tp + 0x28]). XNU owns
// TPIDR_EL0 (it holds the CPU number and is rewritten on context switch), so the converter
// rewrites those reads into: Xt = ((void **)(tpidrro_el0 & ~7))[key], i.e. a Darwin
// pthread TSD slot that holds a pointer to a bionic-layout TCB.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// bionic/libc/platform/bionic/tls_defines.h (arm64)
enum {
    BIONIC_TLS_SLOT_STACK_MTE = -3,
    BIONIC_TLS_SLOT_NATIVE_BRIDGE_GUEST_STATE = -2,
    BIONIC_TLS_SLOT_BIONIC_TLS = -1,
    BIONIC_TLS_SLOT_DTV = 0,
    BIONIC_TLS_SLOT_THREAD_ID = 1,
    BIONIC_TLS_SLOT_APP = 2,
    BIONIC_TLS_SLOT_OPENGL = 3,
    BIONIC_TLS_SLOT_OPENGL_API = 4,
    BIONIC_TLS_SLOT_STACK_GUARD = 5,
    BIONIC_TLS_SLOT_SANITIZER = 6,
    BIONIC_TLS_SLOT_ART_THREAD_SELF = 7,
};

/// Byte offset of the TP slot within the Darwin TSD array (what the trampolines load).
uint64_t andropkg_tls_tsd_offset(void);

/// Ensures the calling thread has a bionic TCB; returns its thread pointer (&slot[0]).
/// Must run on every thread before it executes converted Android code.
void **andropkg_tls_attach_thread(void);

/// Current thread pointer, or NULL if the thread was never attached.
void **andropkg_tls_current(void);

/// Process-wide stack guard (bionic copies the same value into every thread's slot 5).
uintptr_t andropkg_tls_stack_guard(void);

/// Verifies that tpidrro_el0-relative TSD access matches pthread_getspecific().
bool andropkg_tls_selfcheck(char *detail, size_t len);

#ifdef __cplusplus
}
#endif
