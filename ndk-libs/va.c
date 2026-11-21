// andropkg NDK layer — AAPCS64 variadic arguments.
//
// Linux/Android (AAPCS64) passes variadic arguments like named ones: x-registers, then
// v-registers for floating point, then 8-byte stack slots; its va_list is a 32-byte struct.
// Darwin arm64 passes every variadic argument on the stack and its va_list is a plain pointer
// to 8-byte slots. A bionic_* variadic wrapper therefore declares fixed parameters that cover
// all argument registers and the first stack slots, rebuilds the AAPCS64 argument sequence
// (ndk_va), and — for printf/scanf — repacks it as a Darwin va_list.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ndk_internal.h"

void ndk_va_from_list(ndk_va *v, ndk_aapcs_va_list *ap, uint64_t *gr_buf, double *fr_buf, int max) {
    // Copy the remaining register-saved arguments out of the caller's save areas.
    memset(v, 0, sizeof *v);
    int gn = 0, fn = 0;
    for (int off = ap->gr_offs; off < 0 && gn < max; off += 8)
        gr_buf[gn++] = *(uint64_t *)((char *)ap->gr_top + off);
    for (int off = ap->vr_offs; off < 0 && fn < max; off += 16)
        memcpy(&fr_buf[fn++], (char *)ap->vr_top + off, sizeof(double));
    v->gr = gr_buf;
    v->gn = gn;
    v->fr = fr_buf;
    v->fn = fn;
    v->vr16 = ap->vr_offs < 0 ? (const uint8_t *)ap->vr_top + ap->vr_offs : NULL;
    v->stack = (const uint64_t *)ap->stack;
}

uint64_t ndk_va_int(ndk_va *v) {
    if (v->gi < v->gn) return v->gr[v->gi++];
    return v->stack[v->si++];
}

double ndk_va_double(ndk_va *v) {
    if (v->fi < v->fn) return v->fr[v->fi++];
    double d;
    memcpy(&d, &v->stack[v->si++], sizeof d);
    return d;
}

/// IEEE binary128 (Android's long double) -> double, round-toward-zero (enough for printing).
static double quad_to_double(uint64_t lo, uint64_t hi) {
    int sign = (int)(hi >> 63);
    int exp = (int)((hi >> 48) & 0x7fff);
    uint64_t mant_hi = hi & 0xffffffffffffULL;  // 48 bits
    uint64_t bits;
    if (exp == 0x7fff) {
        bits = 0x7ffULL << 52 | ((mant_hi | lo) ? 1ULL << 51 : 0);
    } else if (exp == 0) {
        bits = 0;  // zero / quad subnormals (far below double range)
    } else {
        int e = exp - 16383 + 1023;
        uint64_t m = mant_hi << 4 | lo >> 60;  // top 52 bits of the 112-bit fraction
        if (e >= 0x7ff) bits = 0x7ffULL << 52;
        else if (e <= 0) bits = 0;
        else bits = (uint64_t)e << 52 | m;
    }
    bits |= (uint64_t)sign << 63;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

double ndk_va_long_double(ndk_va *v) {
    uint64_t q[2];
    if (v->fi < v->fn) {
        if (v->vr16) {
            memcpy(q, v->vr16 + 16 * v->fi++, 16);
            return quad_to_double(q[0], q[1]);
        }
        return v->fr[v->fi++];  // register capture keeps only the low 64 bits: best effort
    }
    if (v->si & 1) v->si++;  // 16-byte aligned stack slot
    q[0] = v->stack[v->si++];
    q[1] = v->stack[v->si++];
    return quad_to_double(q[0], q[1]);
}

// ---- printf: argument types from the format ---------------------------------------------------

int ndk_printf_args(const char *fmt, ndk_va *v, uint64_t *slots, int max_slots, char *out, size_t cap) {
    int n = 0;
    size_t o = 0;
#define PUT(c) do { if (o + 1 < cap) out[o++] = (c); } while (0)
#define SLOT(x) do { if (n < max_slots) slots[n++] = (x); } while (0)
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            PUT(*p++);
            continue;
        }
        const char *spec = p++;
        if (*p == '%') {
            PUT('%'); PUT('%');
            p++;
            continue;
        }
        // flags
        while (*p && strchr("-+ #0'", *p)) p++;
        // width
        if (*p == '*') { SLOT((uint64_t)(int64_t)(int)ndk_va_int(v)); p++; }
        else while (*p >= '0' && *p <= '9') p++;
        // precision
        if (*p == '.') {
            p++;
            if (*p == '*') { SLOT((uint64_t)(int64_t)(int)ndk_va_int(v)); p++; }
            else while (*p >= '0' && *p <= '9') p++;
        }
        // length
        int is_long_double = 0;
        while (*p && strchr("hlLqjzt", *p)) {
            if (*p == 'L') is_long_double = 1;
            p++;
        }
        char conv = *p ? *p++ : 0;
        switch (conv) {
            case 'm': {  // bionic/glibc: strerror(errno) — errno already holds the Linux value
                extern char *bionic_strerror(int);
                PUT('%'); PUT('s');
                SLOT((uint64_t)(uintptr_t)bionic_strerror(errno));
                continue;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                double d = is_long_double ? ndk_va_long_double(v) : ndk_va_double(v);
                uint64_t bits;
                memcpy(&bits, &d, sizeof bits);
                SLOT(bits);
                break;
            }
            case 0:
                break;
            default:  // integers, chars, pointers, strings, %n
                SLOT(ndk_va_int(v));
                break;
        }
        // Copy the conversion spec, dropping 'L' (Darwin's long double is double; the slot
        // now holds a double) and mapping 'q' (BSD) is identical on both.
        for (const char *c = spec; c < p; c++)
            if (!(is_long_double && *c == 'L')) PUT(*c);
    }
    out[o < cap ? o : cap - 1] = 0;
#undef PUT
#undef SLOT
    return n;
}
