// Linux arm64 syscall emulation for code that traps with `svc #0`.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Target of the converter's svc trampolines. Kernel-like ABI: x8 = syscall number,
/// x0..x5 = args, result in x0 (negative Linux errno on failure); every other register
/// (including x16/x17, NZCV and q0-q31) is preserved. Never call from C.
void andropkg_svc_entry(void);

/// C-level dispatcher; returns the Linux-ABI result (>= 0, or -LINUX_ERRNO).
long andropkg_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/// Translates a Darwin errno value to the Linux/bionic value.
int andropkg_errno_to_linux(int darwin_errno);

/// Count of syscalls handled (diagnostics / tests).
extern _Atomic uint64_t andropkg_syscall_count;

#ifdef __cplusplus
}
#endif
