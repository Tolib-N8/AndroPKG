// andropkg NDK layer — internal interfaces.
//
// Android NDK code (arm64 ELF, bionic ABI) runs in-process next to Darwin code. The loader
// (loader/) links each image; this layer supplies what the images import: the bionic libc
// ABI on top of Darwin's libSystem, the other NDK system libraries (liblog, libdl,
// libandroid, ...), and a JNI function table safe for AAPCS64 variadic callers.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "andropkg/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NDK_EXPORT __attribute__((visibility("default")))
#define NDK_LOG_TAG "andropkg-ndk"

// ---- linker (linker.c) ---------------------------------------------------------------------
typedef struct ndk_lib ndk_lib;

/// Loads an app ELF library (full path, or a bare name searched next to already loaded ones
/// and in the app's native library directory) with its DT_NEEDED closure; runs initializers.
ndk_lib *ndk_load(const char *path_or_name, char *err, size_t errlen);
void *ndk_sym(ndk_lib *lib, const char *name);
/// True if `pc` lies inside a loaded Android image (code compiled for the AAPCS64 ABI).
bool ndk_is_android_pc(uintptr_t pc);
void ndk_set_library_dir(const char *dir);
/// Executables (exec.c): loaded and linked once; each run restores the pristine writable
/// segments and runs the initializers again (a fresh process image).
ndk_lib *ndk_load_executable(const char *path, char *err, size_t errlen);
void ndk_executable_begin(ndk_lib *l);
void ndk_executable_end(ndk_lib *l);
uintptr_t ndk_lib_entry(ndk_lib *l);

// ---- in-process "processes" (exec.c) -------------------------------------------------------
typedef struct ndk_proc ndk_proc;
ndk_proc *ndk_current_proc(void);
void ndk_set_current_proc(ndk_proc *p);
/// The fd a "process" thread means by 0/1/2 (its pipes), else `fd`.
int ndk_proc_fd(int fd);
/// The stream a "process" thread means by stdin/stdout/stderr, else `f`.
void *ndk_proc_stream(void *f);
/// exit() inside a "process": ends that process only. Returns if not in one.
void ndk_proc_exit(int status);
/// __cxa_atexit inside a "process": registered with it. Returns false if not in one.
bool ndk_proc_atexit(void (*fn)(void *), void *arg);

// ---- system libraries (system.c) -----------------------------------------------------------
/// The implementation of an NDK system library symbol (bionic_<name> here, or a Darwin
/// function with the identical ABI), or NULL.
void *ndk_system_impl(const char *name);
/// True if `name` is exported by some NDK system library (libc, libm, liblog, ...).
bool ndk_system_known(const char *name);
/// True if `soname` names an NDK system library (libc.so, libGLESv2.so, ...).
bool ndk_is_system_lib(const char *soname);
/// A GL entry point of the platform's OpenGL (ES) by its GLES name (egl.m), or NULL.
void *ndk_gl_proc(const char *name);
/// A stub that logs "<name> is not implemented" when called and returns 0.
void *ndk_trap_stub(const char *name);

// ---- logging -------------------------------------------------------------------------------
void ndk_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
enum { NDK_LOG_DEBUG = 3, NDK_LOG_INFO = 4, NDK_LOG_WARN = 5, NDK_LOG_ERROR = 6 };

// ---- errno (libc_errno.c) ------------------------------------------------------------------
int ndk_errno_to_linux(int darwin_errno);
int ndk_errno_to_darwin(int linux_errno);
/// After a failed Darwin call: rewrites errno (shared storage) into the Linux value.
void ndk_fix_errno(void);

// ---- virtual kernel files (libc_extra.c) ---------------------------------------------------
const char *ndk_virtual_file(const char *path, char *buf, size_t n);
int ndk_virtual_fd(const char *content);
int ndk_signal_to_darwin(int linux_signal);

// ---- AAPCS64 varargs (va.c) ----------------------------------------------------------------
/// The arguments of an AAPCS64 variadic call: register save areas + stack.
typedef struct {
    const uint64_t *gr; int gi, gn;   // general registers (x-regs not taken by named args)
    const double *fr; int fi, fn;     // FP registers (d-regs)
    const uint8_t *vr16;              // with a real va_list: 16-byte vr slots (for long double)
    const uint64_t *stack; int si;
} ndk_va;

/// An AAPCS64 va_list as passed (by reference) to v*printf-style functions.
typedef struct {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
} ndk_aapcs_va_list;

void ndk_va_from_list(ndk_va *v, ndk_aapcs_va_list *ap, uint64_t *gr_buf, double *fr_buf, int max);
uint64_t ndk_va_int(ndk_va *v);
double ndk_va_double(ndk_va *v);
double ndk_va_long_double(ndk_va *v);

/// Builds a Darwin va_list (an array of 8-byte slots) for a printf format, pulling the
/// arguments from an AAPCS64 call; rewrites the format where bionic and Darwin differ (%m).
/// Returns the number of slots; `fmt_out` receives the (possibly rewritten) format.
int ndk_printf_args(const char *fmt, ndk_va *v, uint64_t *slots, int max_slots, char *fmt_out, size_t fmt_cap);

#ifdef __cplusplus
}
#endif
