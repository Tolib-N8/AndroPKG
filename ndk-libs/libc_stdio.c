// andropkg NDK layer — bionic stdio and liblog.
//
// FILE* values are Darwin's (opaque to the app), except bionic's static __sF[3] (what
// stdin/stdout/stderr expand to in code built for API < 23), which maps to Darwin's streams.
// Variadic entry points take the AAPCS64 registers explicitly (see va.c).
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#include "ndk_internal.h"

// ---- streams -------------------------------------------------------------------------------------

NDK_EXPORT char bionic___sF[3][152];  // sizeof(FILE) in bionic (LP64)
NDK_EXPORT FILE *bionic_stdin, *bionic_stdout, *bionic_stderr;
__attribute__((constructor)) static void init_streams(void) {
    bionic_stdin = stdin;
    bionic_stdout = stdout;
    bionic_stderr = stderr;
}

static inline FILE *F(FILE *f) {
    uintptr_t p = (uintptr_t)f, b = (uintptr_t)bionic___sF;
    if (p >= b && p < b + sizeof bionic___sF) {
        switch ((p - b) / sizeof bionic___sF[0]) {
            case 0: f = stdin; break;
            case 1: f = stdout; break;
            default: f = stderr; break;
        }
    }
    return ndk_proc_stream(f);  // an in-process "process" has its own std streams (exec.c)
}

#define FIXERR(cond) do { if (cond) ndk_fix_errno(); } while (0)

extern const char *andropkg_translate_path(const char *path, char *buf, size_t len) __attribute__((weak));
NDK_EXPORT FILE *bionic_fopen(const char *p, const char *m) {
    char vbuf[16384], pb[1024];
    const char *v = p ? ndk_virtual_file(p, vbuf, sizeof vbuf) : NULL;
    if (v) return fdopen(ndk_virtual_fd(v), "r");
    if (p && andropkg_translate_path) p = andropkg_translate_path(p, pb, sizeof pb);
    FILE *f = fopen(p, m);
    FIXERR(!f);
    return f;
}
NDK_EXPORT FILE *bionic_fopen64(const char *p, const char *m) { return bionic_fopen(p, m); }
NDK_EXPORT FILE *bionic_fdopen(int fd, const char *m) { FILE *f = fdopen(fd, m); FIXERR(!f); return f; }
NDK_EXPORT FILE *bionic_freopen(const char *p, const char *m, FILE *f) { FILE *r = freopen(p, m, F(f)); FIXERR(!r); return r; }
NDK_EXPORT FILE *bionic_tmpfile(void) { FILE *f = tmpfile(); FIXERR(!f); return f; }
NDK_EXPORT FILE *bionic_popen(const char *c, const char *m) { (void)c; (void)m; errno = 38; return NULL; }  // no processes
NDK_EXPORT int bionic_pclose(FILE *f) { (void)f; errno = 10; return -1; }
NDK_EXPORT FILE *bionic_fmemopen(void *b, size_t n, const char *m) { FILE *f = fmemopen(b, n, m); FIXERR(!f); return f; }
NDK_EXPORT FILE *bionic_open_memstream(char **p, size_t *n) { FILE *f = open_memstream(p, n); FIXERR(!f); return f; }
NDK_EXPORT int bionic_fclose(FILE *f) { int r = fclose(F(f)); FIXERR(r); return r; }
NDK_EXPORT int bionic_fflush(FILE *f) { int r = fflush(f ? F(f) : NULL); FIXERR(r); return r; }
NDK_EXPORT size_t bionic_fread(void *b, size_t s, size_t n, FILE *f) { size_t r = fread(b, s, n, F(f)); FIXERR(r < n && ferror(F(f))); return r; }
NDK_EXPORT size_t bionic_fwrite(const void *b, size_t s, size_t n, FILE *f) { size_t r = fwrite(b, s, n, F(f)); FIXERR(r < n); return r; }
NDK_EXPORT size_t bionic___fread_chk(void *b, size_t s, size_t n, FILE *f, size_t blen) { (void)blen; return bionic_fread(b, s, n, f); }
NDK_EXPORT size_t bionic___fwrite_chk(const void *b, size_t s, size_t n, FILE *f, size_t blen) { (void)blen; return bionic_fwrite(b, s, n, f); }
NDK_EXPORT char *bionic_fgets(char *b, int n, FILE *f) { return fgets(b, n, F(f)); }
NDK_EXPORT int bionic_fputs(const char *s, FILE *f) { return fputs(s, F(f)); }
NDK_EXPORT int bionic_puts(const char *s) { FILE *o = F(stdout); return fputs(s, o) < 0 || fputc('\n', o) < 0 ? EOF : 1; }
NDK_EXPORT int bionic_fputc(int c, FILE *f) { return fputc(c, F(f)); }
NDK_EXPORT int bionic_putc(int c, FILE *f) { return putc(c, F(f)); }
NDK_EXPORT int bionic_putc_unlocked(int c, FILE *f) { return putc_unlocked(c, F(f)); }
NDK_EXPORT int bionic_putchar(int c) { return fputc(c, F(stdout)); }
NDK_EXPORT int bionic_fgetc(FILE *f) { return fgetc(F(f)); }
NDK_EXPORT int bionic_getc(FILE *f) { return getc(F(f)); }
NDK_EXPORT int bionic_getc_unlocked(FILE *f) { return getc_unlocked(F(f)); }
NDK_EXPORT int bionic_getchar(void) { return fgetc(F(stdin)); }
NDK_EXPORT int bionic_ungetc(int c, FILE *f) { return ungetc(c, F(f)); }
NDK_EXPORT int bionic_fseek(FILE *f, long o, int w) { int r = fseek(F(f), o, w); FIXERR(r); return r; }
NDK_EXPORT int bionic_fseeko(FILE *f, off_t o, int w) { int r = fseeko(F(f), o, w); FIXERR(r); return r; }
NDK_EXPORT int bionic_fseeko64(FILE *f, off_t o, int w) { return bionic_fseeko(f, o, w); }
NDK_EXPORT long bionic_ftell(FILE *f) { long r = ftell(F(f)); FIXERR(r < 0); return r; }
NDK_EXPORT off_t bionic_ftello(FILE *f) { off_t r = ftello(F(f)); FIXERR(r < 0); return r; }
NDK_EXPORT off_t bionic_ftello64(FILE *f) { return bionic_ftello(f); }
NDK_EXPORT int bionic_fgetpos(FILE *f, fpos_t *p) { return fgetpos(F(f), p); }
NDK_EXPORT int bionic_fsetpos(FILE *f, const fpos_t *p) { return fsetpos(F(f), p); }
NDK_EXPORT void bionic_rewind(FILE *f) { rewind(F(f)); }
NDK_EXPORT int bionic_feof(FILE *f) { return feof(F(f)); }
NDK_EXPORT int bionic_ferror(FILE *f) { return ferror(F(f)); }
NDK_EXPORT void bionic_clearerr(FILE *f) { clearerr(F(f)); }
NDK_EXPORT int bionic_fileno(FILE *f) { return fileno(F(f)); }
NDK_EXPORT int bionic_setvbuf(FILE *f, char *b, int mode, size_t n) {
    // bionic: _IOFBF 0, _IOLBF 1, _IONBF 2 — Darwin: _IOFBF 0, _IOLBF 1, _IONBF 2.
    return setvbuf(F(f), b, mode, n);
}
NDK_EXPORT void bionic_setbuf(FILE *f, char *b) { setbuf(F(f), b); }
NDK_EXPORT void bionic_setlinebuf(FILE *f) { setlinebuf(F(f)); }
NDK_EXPORT void bionic_flockfile(FILE *f) { flockfile(F(f)); }
NDK_EXPORT void bionic_funlockfile(FILE *f) { funlockfile(F(f)); }
NDK_EXPORT int bionic_ftrylockfile(FILE *f) { return ftrylockfile(F(f)); }
NDK_EXPORT ssize_t bionic_getline(char **l, size_t *n, FILE *f) { return getline(l, n, F(f)); }
NDK_EXPORT ssize_t bionic_getdelim(char **l, size_t *n, int d, FILE *f) { return getdelim(l, n, d, F(f)); }
NDK_EXPORT int bionic_remove(const char *p) { int r = remove(p); FIXERR(r); return r; }
NDK_EXPORT int bionic_rename(const char *a, const char *b) { int r = rename(a, b); FIXERR(r); return r; }
NDK_EXPORT wint_t bionic_fgetwc(FILE *f) { return fgetwc(F(f)); }
NDK_EXPORT wint_t bionic_fputwc(wchar_t c, FILE *f) { return fputwc(c, F(f)); }
NDK_EXPORT int bionic_fwide(FILE *f, int m) { return fwide(F(f), m); }

// ---- printf family ---------------------------------------------------------------------------------

#define VA_PARAMS                                                                                   \
    uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6,       \
    uint64_t x7, double d0, double d1, double d2, double d3, double d4, double d5, double d6,        \
    double d7, uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5,         \
    uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12,    \
    uint64_t s13, uint64_t s14, uint64_t s15
/// Builds `v`: the AAPCS64 variadic arguments after `named` integer-class named parameters.
#define VA_START(v, named)                                                                          \
    uint64_t _gr[8] = {x0, x1, x2, x3, x4, x5, x6, x7};                                             \
    double _fr[8] = {d0, d1, d2, d3, d4, d5, d6, d7};                                               \
    uint64_t _st[16] = {s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15};      \
    ndk_va v = {.gr = _gr + (named), .gn = 8 - (named), .fr = _fr, .fn = 8, .stack = _st}

enum { MAX_SLOTS = 128, MAX_FMT = 4096 };

/// vsnprintf for an AAPCS64 argument sequence.
static int format_v(char *buf, size_t n, const char *fmt, ndk_va *v) {
    uint64_t slots[MAX_SLOTS];
    char f[MAX_FMT];
    ndk_printf_args(fmt, v, slots, MAX_SLOTS, f, sizeof f);
    return vsnprintf(buf, n, f, (va_list)(void *)slots);
}

/// Formats into a malloc'ed string (any length).
static char *format_alloc(const char *fmt, ndk_va *v, int *len) {
    uint64_t slots[MAX_SLOTS];
    char f[MAX_FMT];
    ndk_printf_args(fmt, v, slots, MAX_SLOTS, f, sizeof f);
    char *out = NULL;
    *len = vasprintf(&out, f, (va_list)(void *)slots);
    return out;
}

static int write_v(FILE *f, const char *fmt, ndk_va *v) {
    uint64_t slots[MAX_SLOTS];
    char fm[MAX_FMT];
    ndk_printf_args(fmt, v, slots, MAX_SLOTS, fm, sizeof fm);
    return vfprintf(F(f), fm, (va_list)(void *)slots);
}

#define FROM_LIST(v, ap) \
    uint64_t _grb[8]; double _frb[8]; ndk_va v; ndk_va_from_list(&v, (ndk_aapcs_va_list *)(ap), _grb, _frb, 8)

NDK_EXPORT int bionic_printf(VA_PARAMS) { VA_START(v, 1); return write_v(stdout, (const char *)x0, &v); }
NDK_EXPORT int bionic_fprintf(VA_PARAMS) { VA_START(v, 2); return write_v((FILE *)x0, (const char *)x1, &v); }
NDK_EXPORT int bionic_dprintf(VA_PARAMS) {
    VA_START(v, 2);
    int len;
    char *s = format_alloc((const char *)x1, &v, &len);
    if (s) { write((int)x0, s, (size_t)len); free(s); }
    return len;
}
NDK_EXPORT int bionic_sprintf(VA_PARAMS) { VA_START(v, 2); return format_v((char *)x0, SIZE_MAX >> 1, (const char *)x1, &v); }
NDK_EXPORT int bionic_snprintf(VA_PARAMS) { VA_START(v, 3); return format_v((char *)x0, (size_t)x1, (const char *)x2, &v); }
NDK_EXPORT int bionic_asprintf(VA_PARAMS) {
    VA_START(v, 2);
    int len;
    *(char **)x0 = format_alloc((const char *)x1, &v, &len);
    return len;
}
// __sprintf_chk(buf, flags, buflen, fmt, ...), __snprintf_chk(buf, n, flags, buflen, fmt, ...)
NDK_EXPORT int bionic___sprintf_chk(VA_PARAMS) { VA_START(v, 4); return format_v((char *)x0, (size_t)x2, (const char *)x3, &v); }
NDK_EXPORT int bionic___snprintf_chk(VA_PARAMS) { VA_START(v, 5); return format_v((char *)x0, (size_t)x1, (const char *)x4, &v); }

NDK_EXPORT int bionic_vprintf(const char *fmt, void *ap) { FROM_LIST(v, ap); return write_v(stdout, fmt, &v); }
NDK_EXPORT int bionic_vfprintf(FILE *f, const char *fmt, void *ap) { FROM_LIST(v, ap); return write_v(f, fmt, &v); }
NDK_EXPORT int bionic_vsprintf(char *b, const char *fmt, void *ap) { FROM_LIST(v, ap); return format_v(b, SIZE_MAX >> 1, fmt, &v); }
NDK_EXPORT int bionic_vsnprintf(char *b, size_t n, const char *fmt, void *ap) { FROM_LIST(v, ap); return format_v(b, n, fmt, &v); }
NDK_EXPORT int bionic___vsprintf_chk(char *b, int flags, size_t blen, const char *fmt, void *ap) {
    (void)flags; FROM_LIST(v, ap); return format_v(b, blen, fmt, &v);
}
NDK_EXPORT int bionic___vsnprintf_chk(char *b, size_t n, int flags, size_t blen, const char *fmt, void *ap) {
    (void)flags; (void)blen; FROM_LIST(v, ap); return format_v(b, n, fmt, &v);
}
NDK_EXPORT int bionic_vasprintf(char **out, const char *fmt, void *ap) {
    FROM_LIST(v, ap);
    int len;
    *out = format_alloc(fmt, &v, &len);
    return len;
}
NDK_EXPORT int bionic_vdprintf(int fd, const char *fmt, void *ap) {
    FROM_LIST(v, ap);
    int len;
    char *s = format_alloc(fmt, &v, &len);
    if (s) { write(fd, s, (size_t)len); free(s); }
    return len;
}

// ---- scanf family: every variadic argument is a pointer ---------------------------------------------

static int pointers(ndk_va *v, uint64_t *slots, int max) {
    for (int i = 0; i < max; i++) slots[i] = ndk_va_int(v);
    return max;
}
NDK_EXPORT int bionic_sscanf(VA_PARAMS) {
    VA_START(v, 2);
    uint64_t p[22];
    pointers(&v, p, 22);
    return vsscanf((const char *)x0, (const char *)x1, (va_list)(void *)p);
}
NDK_EXPORT int bionic_fscanf(VA_PARAMS) {
    VA_START(v, 2);
    uint64_t p[22];
    pointers(&v, p, 22);
    return vfscanf(F((FILE *)x0), (const char *)x1, (va_list)(void *)p);
}
NDK_EXPORT int bionic_scanf(VA_PARAMS) {
    VA_START(v, 1);
    uint64_t p[23];
    pointers(&v, p, 23);
    return vfscanf(F(stdin), (const char *)x0, (va_list)(void *)p);
}
NDK_EXPORT int bionic_vsscanf(const char *s, const char *fmt, void *ap) {
    FROM_LIST(v, ap);
    uint64_t p[24];
    pointers(&v, p, 24);
    return vsscanf(s, fmt, (va_list)(void *)p);
}
NDK_EXPORT int bionic_vfscanf(FILE *f, const char *fmt, void *ap) {
    FROM_LIST(v, ap);
    uint64_t p[24];
    pointers(&v, p, 24);
    return vfscanf(F(f), fmt, (va_list)(void *)p);
}

// ---- liblog --------------------------------------------------------------------------------------

extern int __android_log_write(int prio, const char *tag, const char *msg);

NDK_EXPORT int bionic___android_log_write(int prio, const char *tag, const char *msg) {
    return __android_log_write(prio, tag ? tag : "native", msg ? msg : "");
}
NDK_EXPORT int bionic___android_log_buf_write(int buf, int prio, const char *tag, const char *msg) {
    (void)buf;
    return bionic___android_log_write(prio, tag, msg);
}
static int log_v(int prio, const char *tag, const char *fmt, ndk_va *v) {
    char msg[4096];
    format_v(msg, sizeof msg, fmt ? fmt : "", v);
    return bionic___android_log_write(prio, tag, msg);
}
NDK_EXPORT int bionic___android_log_print(VA_PARAMS) { VA_START(v, 3); return log_v((int)x0, (const char *)x1, (const char *)x2, &v); }
NDK_EXPORT int bionic___android_log_buf_print(VA_PARAMS) { VA_START(v, 4); return log_v((int)x1, (const char *)x2, (const char *)x3, &v); }
NDK_EXPORT int bionic___android_log_vprint(int prio, const char *tag, const char *fmt, void *ap) {
    FROM_LIST(v, ap);
    return log_v(prio, tag, fmt, &v);
}
NDK_EXPORT void bionic___android_log_assert(VA_PARAMS) {
    VA_START(v, 3);
    const char *cond = (const char *)x0, *tag = (const char *)x1, *fmt = (const char *)x2;
    char msg[4096];
    if (fmt) format_v(msg, sizeof msg, fmt, &v);
    else snprintf(msg, sizeof msg, "Assertion failed: %s", cond ? cond : "");
    bionic___android_log_write(7 /* FATAL */, tag, msg);
    extern void bionic_android_set_abort_message(const char *);
    bionic_android_set_abort_message(msg);
    abort();
}
NDK_EXPORT int bionic___android_log_is_loggable(int prio, const char *tag, int def) { (void)tag; (void)def; return prio >= 3; }
NDK_EXPORT int bionic___android_log_is_loggable_len(int prio, const char *tag, size_t len, int def) { (void)len; return bionic___android_log_is_loggable(prio, tag, def); }
NDK_EXPORT void bionic___android_log_set_minimum_priority(int prio) { (void)prio; }
NDK_EXPORT int bionic___android_log_get_minimum_priority(void) { return 2; }
NDK_EXPORT void bionic___android_log_set_default_tag(const char *tag) { (void)tag; }
