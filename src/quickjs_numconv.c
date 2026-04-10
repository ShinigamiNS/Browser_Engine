/* quickjs_numconv.c
   Number conversion stubs for QuickJS builds without libbf.c.
   These wrap standard C library functions. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* js_dtoa: double to string, returns number of chars written */
int js_dtoa(char *buf, double d, int radix, int n_digits, int flags) {
    (void)radix; (void)n_digits; (void)flags;
    if (isnan(d))      { strcpy(buf, "NaN");      return 3; }
    if (isinf(d))      { strcpy(buf, d>0 ? "Infinity" : "-Infinity"); return (int)strlen(buf); }
    /* Use snprintf for a reasonable representation */
    int len = snprintf(buf, 64, "%.17g", d);
    /* Trim trailing zeros after decimal point */
    if (strchr(buf, '.') && !strchr(buf, 'e')) {
        int i = len - 1;
        while (i > 0 && buf[i] == '0') i--;
        if (buf[i] == '.') i--;
        buf[i+1] = '\0';
        len = i+1;
    }
    return len;
}

/* js_dtoa_max_len: maximum buffer size needed for js_dtoa */
int js_dtoa_max_len(void) { return 64; }

/* js_atod: string to double */
double js_atod(const char *str, const char **endptr, int radix, int flags) {
    (void)flags;
    if (radix == 16 || radix == 0) {
        /* handle 0x prefix */
        const char *s = str;
        while (*s == ' ' || *s == '\t') s++;
        if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
            char *ep;
            double v = (double)strtoull(s+2, &ep, 16);
            if (endptr) *endptr = ep;
            return v;
        }
    }
    char *ep;
    double v = strtod(str, &ep);
    if (endptr) *endptr = ep;
    return v;
}

/* i32toa: int32 to decimal string */
char *i32toa(char *buf, int32_t n) {
    sprintf(buf, "%d", n);
    return buf;
}

/* u32toa: uint32 to decimal string */
char *u32toa(char *buf, uint32_t n) {
    sprintf(buf, "%u", n);
    return buf;
}

/* i64toa: int64 to decimal string */
char *i64toa(char *buf, int64_t n, int radix) {
    if (radix == 10) { sprintf(buf, "%lld", (long long)n); }
    else if (radix == 16) { sprintf(buf, "%llx", (unsigned long long)n); }
    else if (radix == 8)  { sprintf(buf, "%llo", (unsigned long long)n); }
    else { sprintf(buf, "%lld", (long long)n); }
    return buf;
}

/* i64toa_radix: alias */
char *i64toa_radix(char *buf, int64_t n, int radix) {
    return i64toa(buf, n, radix);
}
