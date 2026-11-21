// andropkg NDK layer — bionic locales, multibyte conversion and long double.
//
// bionic supports only the C and C.UTF-8 locales and always converts multibyte as UTF-8; its
// locale_t is an opaque pointer and its mbstate_t is 8 bytes (Darwin's is 128), so locales are
// a token and UTF-8 conversion is implemented here. Android's long double is IEEE binary128
// (passed and returned in q registers); Darwin's is double: converted at the boundary.
#include <ctype.h>
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "ndk_internal.h"

// ---- locales -------------------------------------------------------------------------------------

static int g_c_locale;  // the only locale object
#define C_LOCALE ((void *)&g_c_locale)
static __thread void *t_locale;

NDK_EXPORT void *bionic_newlocale(int mask, const char *name, void *base) {
    (void)mask; (void)base;
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX") && strcmp(name, "C.UTF-8") &&
        strcmp(name, "en_US.UTF-8") && strcmp(name, "C.utf8")) {
        errno = 2;  // ENOENT, as bionic does for unknown locales
        return NULL;
    }
    return C_LOCALE;
}
NDK_EXPORT void *bionic_duplocale(void *l) { return l ? l : C_LOCALE; }
NDK_EXPORT void bionic_freelocale(void *l) { (void)l; }
NDK_EXPORT void *bionic_uselocale(void *l) {
    void *old = t_locale ? t_locale : (void *)-1L;  // LC_GLOBAL_LOCALE is (locale_t)-1
    if (l) t_locale = l;
    return old;
}
NDK_EXPORT char *bionic_setlocale(int category, const char *name) {
    (void)category;
    if (name && *name && strcmp(name, "C") && strcmp(name, "POSIX") && strcmp(name, "C.UTF-8") && strcmp(name, "C.utf8"))
        return NULL;
    return "C.UTF-8";
}
NDK_EXPORT size_t bionic___ctype_get_mb_cur_max(void) { return 4; }
NDK_EXPORT struct lconv *bionic_localeconv(void) { return localeconv(); }  // C locale: same layout
NDK_EXPORT char *bionic_nl_langinfo(int item) {
    switch (item) {
        case 14: return "UTF-8";  // CODESET
        case 0x10000: return ".";  // RADIXCHAR
        case 0x10001: return "";   // THOUSEP
        default: return "";
    }
}
NDK_EXPORT char *bionic_nl_langinfo_l(int item, void *l) { (void)l; return bionic_nl_langinfo(item); }

// *_l: the locale is always C.
#define L_CTYPE(n) NDK_EXPORT int bionic_##n##_l(int c, void *l) { (void)l; return n(c); }
L_CTYPE(isalnum) L_CTYPE(isalpha) L_CTYPE(isblank) L_CTYPE(iscntrl) L_CTYPE(isdigit) L_CTYPE(isgraph)
L_CTYPE(islower) L_CTYPE(isprint) L_CTYPE(ispunct) L_CTYPE(isspace) L_CTYPE(isupper) L_CTYPE(isxdigit)
L_CTYPE(tolower) L_CTYPE(toupper)
#define L_WCTYPE(n) NDK_EXPORT int bionic_##n##_l(wint_t c, void *l) { (void)l; return n(c); }
L_WCTYPE(iswalnum) L_WCTYPE(iswalpha) L_WCTYPE(iswblank) L_WCTYPE(iswcntrl) L_WCTYPE(iswdigit) L_WCTYPE(iswgraph)
L_WCTYPE(iswlower) L_WCTYPE(iswprint) L_WCTYPE(iswpunct) L_WCTYPE(iswspace) L_WCTYPE(iswupper) L_WCTYPE(iswxdigit)
NDK_EXPORT wint_t bionic_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
NDK_EXPORT wint_t bionic_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }
NDK_EXPORT int bionic_iswctype_l(wint_t c, wctype_t t, void *l) { (void)l; return iswctype(c, t); }
NDK_EXPORT wctype_t bionic_wctype_l(const char *n, void *l) { (void)l; return wctype(n); }
NDK_EXPORT int bionic_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcoll(a, b); }
NDK_EXPORT size_t bionic_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
NDK_EXPORT int bionic_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscoll(a, b); }
NDK_EXPORT size_t bionic_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }
NDK_EXPORT int bionic_strcasecmp_l(const char *a, const char *b, void *l) { (void)l; return strcasecmp(a, b); }
NDK_EXPORT int bionic_strncasecmp_l(const char *a, const char *b, size_t n, void *l) { (void)l; return strncasecmp(a, b, n); }
NDK_EXPORT double bionic_strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
NDK_EXPORT float bionic_strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
NDK_EXPORT long bionic_strtol_l(const char *s, char **e, int b, void *l) { (void)l; return strtol(s, e, b); }
NDK_EXPORT unsigned long bionic_strtoul_l(const char *s, char **e, int b, void *l) { (void)l; return strtoul(s, e, b); }
NDK_EXPORT long long bionic_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
NDK_EXPORT unsigned long long bionic_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
NDK_EXPORT size_t bionic_strftime_l(char *s, size_t n, const char *f, const struct tm *t, void *l) { (void)l; return strftime(s, n, f, t); }

// ---- UTF-8 multibyte (bionic mbstate_t: seq[4] bytes pending + count in reserved[0]) -------------

typedef struct { unsigned char seq[4]; unsigned char reserved[4]; } b_mbstate;
static b_mbstate g_mbs_internal;

NDK_EXPORT int bionic_mbsinit(const b_mbstate *ps) { return !ps || ps->reserved[0] == 0; }

static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return -1;
}

NDK_EXPORT size_t bionic_mbrtowc(wchar_t *pwc, const char *s, size_t n, b_mbstate *ps) {
    if (!ps) ps = &g_mbs_internal;
    if (!s) { memset(ps, 0, sizeof *ps); return 0; }
    if (n == 0) return (size_t)-2;
    unsigned char buf[4];
    int have = ps->reserved[0];
    memcpy(buf, ps->seq, (size_t)have);
    size_t used = 0;
    int need = have ? utf8_len(buf[0]) : utf8_len((unsigned char)s[0]);
    if (need < 0) { errno = 84; memset(ps, 0, sizeof *ps); return (size_t)-1; }  // EILSEQ
    while (have < need && used < n) buf[have++] = (unsigned char)s[used++];
    if (have < need) {  // incomplete: keep the bytes
        memcpy(ps->seq, buf, (size_t)have);
        ps->reserved[0] = (unsigned char)have;
        return (size_t)-2;
    }
    uint32_t c = need == 1 ? buf[0] : need == 2 ? buf[0] & 0x1F : need == 3 ? buf[0] & 0x0F : buf[0] & 0x07;
    for (int i = 1; i < need; i++) {
        if ((buf[i] & 0xC0) != 0x80) { errno = 84; memset(ps, 0, sizeof *ps); return (size_t)-1; }
        c = c << 6 | (buf[i] & 0x3F);
    }
    memset(ps, 0, sizeof *ps);
    if (pwc) *pwc = (wchar_t)c;
    return c == 0 ? 0 : used;
}
NDK_EXPORT size_t bionic_mbrlen(const char *s, size_t n, b_mbstate *ps) {
    static b_mbstate st;
    return bionic_mbrtowc(NULL, s, n, ps ? ps : &st);
}
NDK_EXPORT int bionic_mbtowc(wchar_t *pwc, const char *s, size_t n) {
    static b_mbstate st;
    if (!s) { memset(&st, 0, sizeof st); return 0; }
    size_t r = bionic_mbrtowc(pwc, s, n, &st);
    if (r == (size_t)-2) { memset(&st, 0, sizeof st); errno = 84; return -1; }
    return (int)r;
}
NDK_EXPORT int bionic_mblen(const char *s, size_t n) { return bionic_mbtowc(NULL, s, n); }

NDK_EXPORT size_t bionic_wcrtomb(char *s, wchar_t wc, b_mbstate *ps) {
    if (ps) memset(ps, 0, sizeof *ps);
    char tmp[4];
    if (!s) { s = tmp; wc = 0; }
    uint32_t c = (uint32_t)wc;
    if (c < 0x80) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = (char)(0xC0 | c >> 6); s[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c >= 0xD800 && c <= 0xDFFF) { errno = 84; return (size_t)-1; }
    if (c < 0x10000) { s[0] = (char)(0xE0 | c >> 12); s[1] = (char)(0x80 | (c >> 6 & 0x3F)); s[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    if (c < 0x110000) {
        s[0] = (char)(0xF0 | c >> 18); s[1] = (char)(0x80 | (c >> 12 & 0x3F));
        s[2] = (char)(0x80 | (c >> 6 & 0x3F)); s[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
    errno = 84;
    return (size_t)-1;
}
NDK_EXPORT int bionic_wctomb(char *s, wchar_t wc) { return s ? (int)bionic_wcrtomb(s, wc, NULL) : 0; }

NDK_EXPORT size_t bionic_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, b_mbstate *ps) {
    static b_mbstate st;
    if (!ps) ps = &st;
    const char *s = *src;
    size_t n = 0;
    while (nms > 0 && (!dst || n < len)) {
        wchar_t wc;
        size_t r = bionic_mbrtowc(&wc, s, nms, ps);
        if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
        if (r == (size_t)-2) { s += nms; break; }
        if (r == 0) { if (dst) { dst[n] = 0; *src = NULL; } return n; }
        if (dst) dst[n] = wc;
        n++;
        s += r;
        nms -= r;
    }
    if (dst) *src = s;
    return n;
}
NDK_EXPORT size_t bionic_mbsrtowcs(wchar_t *dst, const char **src, size_t len, b_mbstate *ps) {
    return bionic_mbsnrtowcs(dst, src, SIZE_MAX, len, ps);
}
NDK_EXPORT size_t bionic_mbstowcs(wchar_t *dst, const char *src, size_t len) {
    const char *s = src;
    return bionic_mbsrtowcs(dst, &s, len, NULL);
}
NDK_EXPORT size_t bionic_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, b_mbstate *ps) {
    (void)ps;
    const wchar_t *s = *src;
    size_t n = 0;
    char buf[4];
    while (nwc-- > 0) {
        size_t r = bionic_wcrtomb(buf, *s, NULL);
        if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
        if (dst && n + r > len) break;
        if (dst) memcpy(dst + n, buf, r);
        if (*s == 0) { if (dst) *src = NULL; return n; }
        n += r;
        s++;
    }
    if (dst) *src = s;
    return n;
}
NDK_EXPORT size_t bionic_wcsrtombs(char *dst, const wchar_t **src, size_t len, b_mbstate *ps) {
    return bionic_wcsnrtombs(dst, src, SIZE_MAX, len, ps);
}
NDK_EXPORT size_t bionic_wcstombs(char *dst, const wchar_t *src, size_t len) {
    const wchar_t *s = src;
    return bionic_wcsrtombs(dst, &s, len, NULL);
}

// ---- long double (binary128 in q registers) ----------------------------------------------------

typedef uint64_t q128 __attribute__((vector_size(16)));  // returned/passed in q0..q7

static double q2d(q128 q) {
    uint64_t lo = q[0], hi = q[1];
    int sign = (int)(hi >> 63), exp = (int)((hi >> 48) & 0x7fff);
    uint64_t bits;
    if (exp == 0x7fff) bits = 0x7ffULL << 52 | (((hi & 0xffffffffffffULL) | lo) ? 1ULL << 51 : 0);
    else if (exp == 0) bits = 0;
    else {
        int e = exp - 16383 + 1023;
        uint64_t m = (hi & 0xffffffffffffULL) << 4 | lo >> 60;
        bits = e >= 0x7ff ? 0x7ffULL << 52 : e <= 0 ? 0 : (uint64_t)e << 52 | m;
    }
    bits |= (uint64_t)sign << 63;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static q128 d2q(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    uint64_t sign = bits >> 63, exp = (bits >> 52) & 0x7ff, m = bits & 0xfffffffffffffULL;
    uint64_t hi, lo;
    if (exp == 0x7ff) { hi = 0x7fffULL << 48 | (m ? 1ULL << 47 : 0); lo = 0; }
    else if (exp == 0 && m == 0) { hi = 0; lo = 0; }
    else if (exp == 0) {  // double subnormal: normalize
        int shift = __builtin_clzll(m) - 11;
        m <<= shift;
        m &= 0xfffffffffffffULL;
        uint64_t e = (uint64_t)(1 - shift - 1023 + 16383);
        hi = e << 48 | m >> 4;
        lo = m << 60;
    } else {
        uint64_t e = exp - 1023 + 16383;
        hi = e << 48 | m >> 4;
        lo = m << 60;
    }
    hi |= sign << 63;
    q128 q = {lo, hi};
    return q;
}

NDK_EXPORT q128 bionic_strtold(const char *s, char **e) { return d2q(strtod(s, e)); }
NDK_EXPORT q128 bionic_strtold_l(const char *s, char **e, void *l) { (void)l; return d2q(strtod(s, e)); }
NDK_EXPORT q128 bionic_wcstold(const wchar_t *s, wchar_t **e) { return d2q(wcstod(s, e)); }

#define LD1(n, f) NDK_EXPORT q128 bionic_##n(q128 x) { return d2q(f(q2d(x))); }
#define LD2(n, f) NDK_EXPORT q128 bionic_##n(q128 x, q128 y) { return d2q(f(q2d(x), q2d(y))); }
LD1(sinl, sin) LD1(cosl, cos) LD1(tanl, tan) LD1(asinl, asin) LD1(acosl, acos) LD1(atanl, atan)
LD1(sinhl, sinh) LD1(coshl, cosh) LD1(tanhl, tanh) LD1(asinhl, asinh) LD1(acoshl, acosh) LD1(atanhl, atanh)
LD1(expl, exp) LD1(exp2l, exp2) LD1(expm1l, expm1) LD1(logl, log) LD1(log10l, log10) LD1(log2l, log2)
LD1(log1pl, log1p) LD1(logbl, logb) LD1(sqrtl, sqrt) LD1(cbrtl, cbrt) LD1(fabsl, fabs) LD1(floorl, floor)
LD1(ceill, ceil) LD1(roundl, round) LD1(truncl, trunc) LD1(rintl, rint) LD1(nearbyintl, nearbyint)
LD1(erfl, erf) LD1(erfcl, erfc) LD1(tgammal, tgamma) LD1(lgammal, lgamma)
LD2(powl, pow) LD2(atan2l, atan2) LD2(fmodl, fmod) LD2(hypotl, hypot) LD2(copysignl, copysign)
LD2(fmaxl, fmax) LD2(fminl, fmin) LD2(fdiml, fdim) LD2(remainderl, remainder) LD2(nextafterl, nextafter)
NDK_EXPORT q128 bionic_fmal(q128 x, q128 y, q128 z) { return d2q(fma(q2d(x), q2d(y), q2d(z))); }
NDK_EXPORT q128 bionic_ldexpl(q128 x, int e) { return d2q(ldexp(q2d(x), e)); }
NDK_EXPORT q128 bionic_scalbnl(q128 x, int e) { return d2q(scalbn(q2d(x), e)); }
NDK_EXPORT q128 bionic_frexpl(q128 x, int *e) { return d2q(frexp(q2d(x), e)); }
NDK_EXPORT q128 bionic_modfl(q128 x, q128 *ip) { double i; double f = modf(q2d(x), &i); *ip = d2q(i); return d2q(f); }
NDK_EXPORT long bionic_lrintl(q128 x) { return lrint(q2d(x)); }
NDK_EXPORT long bionic_lroundl(q128 x) { return lround(q2d(x)); }
NDK_EXPORT long long bionic_llrintl(q128 x) { return llrint(q2d(x)); }
NDK_EXPORT long long bionic_llroundl(q128 x) { return llround(q2d(x)); }
NDK_EXPORT int bionic_ilogbl(q128 x) { return ilogb(q2d(x)); }
NDK_EXPORT q128 bionic_nanl(const char *t) { return d2q(nan(t)); }
NDK_EXPORT int bionic___signbitl(q128 x) { return (int)(x[1] >> 63); }
NDK_EXPORT int bionic___isinfl(q128 x) { return isinf(q2d(x)); }
NDK_EXPORT int bionic___isnanl(q128 x) { return isnan(q2d(x)); }
NDK_EXPORT int bionic___fpclassifyl(q128 x) {
    switch (fpclassify(q2d(x))) {  // bionic FP_* values: INFINITE 1, NAN 2, NORMAL 4, SUBNORMAL 8, ZERO 0x10
        case FP_INFINITE: return 1;
        case FP_NAN: return 2;
        case FP_NORMAL: return 4;
        case FP_SUBNORMAL: return 8;
        default: return 0x10;
    }
}
NDK_EXPORT void bionic_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
NDK_EXPORT void bionic_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
NDK_EXPORT void bionic_sincosl(q128 x, q128 *s, q128 *c) { double d = q2d(x); *s = d2q(sin(d)); *c = d2q(cos(d)); }
NDK_EXPORT double bionic_significand(double x) { int e; return frexp(x, &e) * 2; }
NDK_EXPORT float bionic_significandf(float x) { int e; return frexpf(x, &e) * 2; }
NDK_EXPORT double bionic_drem(double x, double y) { return remainder(x, y); }
NDK_EXPORT float bionic_dremf(float x, float y) { return remainderf(x, y); }
NDK_EXPORT double bionic_gamma(double x) { return lgamma(x); }
NDK_EXPORT float bionic_gammaf(float x) { return lgammaf(x); }

// fpclassify family for double/float (bionic's FP_* constants differ from Darwin's)
NDK_EXPORT int bionic___fpclassifyd(double x) {
    switch (fpclassify(x)) {
        case FP_INFINITE: return 1; case FP_NAN: return 2; case FP_NORMAL: return 4; case FP_SUBNORMAL: return 8; default: return 0x10;
    }
}
NDK_EXPORT int bionic___fpclassifyf(float x) { return bionic___fpclassifyd(x); }
NDK_EXPORT int bionic___fpclassify(double x) { return bionic___fpclassifyd(x); }
NDK_EXPORT int bionic___isinf(double x) { return isinf(x); }
NDK_EXPORT int bionic___isinff(float x) { return isinf(x); }
NDK_EXPORT int bionic___isnan(double x) { return isnan(x); }
NDK_EXPORT int bionic___isnanf(float x) { return isnan(x); }
NDK_EXPORT int bionic___isfinite(double x) { return isfinite(x); }
NDK_EXPORT int bionic___isfinitef(float x) { return isfinite(x); }
NDK_EXPORT int bionic___isnormal(double x) { return isnormal(x); }
NDK_EXPORT int bionic___isnormalf(float x) { return isnormal(x); }
NDK_EXPORT int bionic___signbit(double x) { return signbit(x) != 0; }
NDK_EXPORT int bionic___signbitf(float x) { return signbit(x) != 0; }
NDK_EXPORT int bionic_isinf(double x) { return isinf(x); }
NDK_EXPORT int bionic_isnan(double x) { return isnan(x); }
NDK_EXPORT int bionic_isinff(float x) { return isinf(x); }
NDK_EXPORT int bionic_isnanf(float x) { return isnan(x); }
