/* quickjs_numconv.c
   Number conversion functions for QuickJS builds without dtoa.c/libbf.c.
   These wrap standard C library functions.

   CRITICAL: signatures and RETURN VALUES must match dtoa.h exactly. QuickJS
   calls `len = i32toa(buf, n)` and `js_new_string8_len(ctx, buf, len)`, so the
   *toa functions must return the number of characters written (a length), NOT
   the buffer pointer. Returning the pointer makes `len` an astronomical value
   and every integer->string conversion throws "out of memory". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>

#include "dtoa.h"  /* authoritative prototypes + JS_DTOA_* flags */

/* ── Integer -> decimal/radix string (return length written) ─────────────── */

size_t u32toa(char *buf, uint32_t n) {
    return (size_t)sprintf(buf, "%u", n);
}

size_t i32toa(char *buf, int32_t n) {
    return (size_t)sprintf(buf, "%d", n);
}

size_t u64toa(char *buf, uint64_t n) {
    return (size_t)sprintf(buf, "%llu", (unsigned long long)n);
}

size_t i64toa(char *buf, int64_t n) {
    return (size_t)sprintf(buf, "%lld", (long long)n);
}

size_t u64toa_radix(char *buf, uint64_t n, unsigned int radix) {
    if (radix == 10) return (size_t)sprintf(buf, "%llu", (unsigned long long)n);
    if (radix == 16) return (size_t)sprintf(buf, "%llx", (unsigned long long)n);
    if (radix == 8)  return (size_t)sprintf(buf, "%llo", (unsigned long long)n);
    /* general base 2..36 */
    char tmp[70];
    int i = 0;
    if (radix < 2 || radix > 36) radix = 10;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    while (n > 0) {
        unsigned int d = (unsigned int)(n % radix);
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        n /= radix;
    }
    int len = i;
    for (int j = 0; j < len; j++) buf[j] = tmp[len - 1 - j];
    buf[len] = '\0';
    return (size_t)len;
}

size_t i64toa_radix(char *buf, int64_t n, unsigned int radix) {
    if (n < 0) {
        buf[0] = '-';
        return 1 + u64toa_radix(buf + 1, (uint64_t)(-(n + 1)) + 1, radix);
    }
    return u64toa_radix(buf, (uint64_t)n, radix);
}

/* ── double -> string ────────────────────────────────────────────────────── */

/* Upper bound on the string length js_dtoa may produce. */
int js_dtoa_max_len(double d, int radix, int n_digits, int flags) {
    (void)d; (void)radix; (void)flags;
    /* JS_DTOA_MAX_DIGITS plus room for sign, dot, exponent, radix prefix. */
    int base = (n_digits > 0 ? n_digits : 101) + 16;
    return base < 128 ? 128 : base;
}

/* Convert d to a string in buf per the format flags; return length written. */
int js_dtoa(char *buf, double d, int radix, int n_digits, int flags,
            JSDTOATempMem *tmp_mem) {
    (void)tmp_mem;
    int fmt = flags & JS_DTOA_FORMAT_MASK;

    if (isnan(d)) { strcpy(buf, "NaN"); return 3; }
    if (isinf(d)) {
        strcpy(buf, d > 0 ? "Infinity" : "-Infinity");
        return (int)strlen(buf);
    }
    if (d == 0) {
        /* honor -0 only when explicitly requested */
        if (signbit(d) && (flags & JS_DTOA_MINUS_ZERO)) { strcpy(buf, "-0"); return 2; }
        strcpy(buf, "0");
        return 1;
    }

    /* Non-decimal radix (only valid with FORMAT_FREE): integer-style output. */
    if (radix != 10) {
        int neg = d < 0;
        if (neg) d = -d;
        /* Only integers are produced for radix != 10 in practice. */
        uint64_t iv = (uint64_t)d;
        char tmp[70];
        int i = 0;
        if (iv == 0) tmp[i++] = '0';
        while (iv > 0) {
            unsigned int dd = (unsigned int)(iv % (unsigned)radix);
            tmp[i++] = (char)(dd < 10 ? '0' + dd : 'a' + (dd - 10));
            iv /= (unsigned)radix;
        }
        int len = 0;
        if (neg) buf[len++] = '-';
        for (int j = i - 1; j >= 0; j--) buf[len++] = tmp[j];
        buf[len] = '\0';
        return len;
    }

    int len;
    if (fmt == JS_DTOA_FORMAT_FIXED) {
        /* n_digits significant digits */
        int nd = n_digits < 1 ? 1 : (n_digits > 101 ? 101 : n_digits);
        len = snprintf(buf, JS_DTOA_MAX_DIGITS + 32, "%.*g", nd, d);
        return len;
    }
    if (fmt == JS_DTOA_FORMAT_FRAC) {
        /* n_digits fractional digits */
        int nd = n_digits < 0 ? 0 : (n_digits > 101 ? 101 : n_digits);
        if (flags & JS_DTOA_EXP_ENABLED)
            len = snprintf(buf, JS_DTOA_MAX_DIGITS + 32, "%.*e", nd, d);
        else
            len = snprintf(buf, JS_DTOA_MAX_DIGITS + 32, "%.*f", nd, d);
        return len;
    }

    /* FORMAT_FREE: shortest round-tripping representation. Try increasing
       precision until the value parses back exactly (matches JS semantics
       closely enough for display). */
    for (int prec = 1; prec <= 17; prec++) {
        len = snprintf(buf, 40, "%.*g", prec, d);
        double rt = strtod(buf, NULL);
        if (rt == d) break;
    }
    return (int)strlen(buf);
}

/* ── string -> double ────────────────────────────────────────────────────── */

double js_atod(const char *str, const char **pnext, int radix, int flags,
               JSATODTempMem *tmp_mem) {
    (void)flags; (void)tmp_mem;
    const char *s = str;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    /* Explicit or auto-detected non-decimal integer bases. */
    if (radix == 16 || (radix == 0 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
        const char *p = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? s + 2 : s;
        char *ep;
        double v = (double)strtoull(p, &ep, 16);
        if (pnext) *pnext = (ep == p) ? str : ep;
        return v;
    }
    if (radix == 8) {
        char *ep;
        double v = (double)strtoull(s, &ep, 8);
        if (pnext) *pnext = ep;
        return v;
    }
    if (radix == 2) {
        char *ep;
        double v = (double)strtoull(s, &ep, 2);
        if (pnext) *pnext = ep;
        return v;
    }

    char *ep;
    double v = strtod(s, &ep);
    if (pnext) *pnext = (ep == s) ? str : ep;
    return v;
}
