#include "string_pure.h"
#include "tsys.h"
#include <stdarg.h>

// ---- StrBuf ----

void sb_init(StrBuf *sb, Tsys *sys) {
    sb->buf = 0; sb->len = 0; sb->cap = 0; sb->ok = 1; sb->sys = sys;
}

int sb_reserve(StrBuf *sb, int need) {
    if (!sb->ok) return 0;
    if (sb->len + need + 1 <= sb->cap) return 1;
    int nc = sb->cap ? sb->cap * 2 : 512;
    while (nc < sb->len + need + 1) nc *= 2;
    char *nb = (char *)sb->sys->realloc(sb->buf, (size_t)nc);
    if (!nb) { sb->ok = 0; return 0; }
    sb->buf = nb; sb->cap = nc;
    return 1;
}

void sb_char(StrBuf *sb, char c) {
    if (!sb_reserve(sb, 1)) return;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len]   = '\0';
}

void sb_str(StrBuf *sb, const char *s) {
    if (!s) return;
    int n = 0; while (s[n]) n++;
    if (!sb_reserve(sb, n)) return;
    sb->sys->memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_int(StrBuf *sb, long long v) {
    if (v == 0) { sb_char(sb, '0'); return; }
    char tmp[24]; int n = 0;
    int neg = (v < 0);
    unsigned long long u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
    while (u > 0) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) sb_char(sb, '-');
    for (int i = n - 1; i >= 0; i--) sb_char(sb, tmp[i]);
}

void sb_append(StrBuf *sb, const char *s, int n) {
    if (!sb_reserve(sb, n)) return;
    sb->sys->memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

// ---- string functions ----

char *s_strcat(char *dest, const char *src) {
    char *ret = dest;
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = '\0';
    return ret;
}

char *s_strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

char *s_strcpy(char *dest, const char *src) {
    if (!src) return dest;
    char *d = dest;
    while ((*d++ = *src++) != '\0') {}
    return dest;
}

char *s_strncpy(char *dest, const char *src, size_t n) {
    if (!dest || n == 0) return dest;
    size_t i = 0;
    if (src) {
        while (src[i] && i < n - 1) { dest[i] = src[i]; i++; }
    }
    dest[i] = '\0';
    return dest;
}

char *s_strdup(const char *s, void *(*allocator)(size_t)) {
    size_t len = 0;
    while (s[len]) len++;
    char *d = (char *)allocator(len + 1);
    if (d) {
        for (size_t i = 0; i <= len; i++) d[i] = s[i];
    }
    return d;
}




size_t s_strlen(const char *s) {
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int s_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int s_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

const char *s_strstr(const char *hay, const char *needle) {
    if (!*needle) return hay;
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return p;
    }
    return (const char *)0;
}


// ---- exact decimal <-> double ----
// A small fixed-size bignum. The largest number built is D * 2^1075 for a
// S_NUM_DIGITS-digit D (780 digits -- a double's halfway points need at most 767 to
// tell apart), which stays under 130 32-bit words.
#define S_BIG_WORDS 130
#define S_NUM_DIGITS 780
typedef struct { unsigned int w[S_BIG_WORDS]; int n; } SBig;

static void s_big_set(SBig *b, unsigned long long v) {
    b->n = 0;
    while (v) { b->w[b->n++] = (unsigned int)v; v >>= 32; }
}

static void s_big_mul_small(SBig *b, unsigned int m) {
    unsigned long long carry = 0;
    for (int i = 0; i < b->n; i++) {
        unsigned long long t = (unsigned long long)b->w[i] * m + carry;
        b->w[i] = (unsigned int)t;
        carry = t >> 32;
    }
    if (carry && b->n < S_BIG_WORDS) b->w[b->n++] = (unsigned int)carry;
}

static void s_big_add_small(SBig *b, unsigned int a) {
    unsigned long long carry = a;
    for (int i = 0; i < b->n && carry; i++) {
        unsigned long long t = (unsigned long long)b->w[i] + carry;
        b->w[i] = (unsigned int)t;
        carry = t >> 32;
    }
    if (carry && b->n < S_BIG_WORDS) b->w[b->n++] = (unsigned int)carry;
}

static void s_big_mul_pow10(SBig *b, int e) {
    for (; e >= 9; e -= 9) s_big_mul_small(b, 1000000000u);
    static const unsigned int P10[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    if (e > 0) s_big_mul_small(b, P10[e]);
}

static void s_big_shl(SBig *b, int bits) {
    if (b->n == 0 || bits <= 0) return;
    int words = bits / 32, r = bits % 32;
    if (b->n + words + 1 > S_BIG_WORDS) return;
    b->w[b->n + words] = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        unsigned int v = b->w[i];
        b->w[i + words + 1] |= r ? v >> (32 - r) : 0;
        b->w[i + words] = v << r;
    }
    for (int i = 0; i < words; i++) b->w[i] = 0;
    b->n += words + 1;
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
}

static int s_big_cmp(const SBig *a, const SBig *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

// Compare D * 10^e (D given as its digits) with h * 2^t, both as exact integers.
static int s_cmp_dec_bin(const char *digits, int nd, int e, unsigned long long h, int t) {
    SBig a, b;
    s_big_set(&a, 0);
    for (int i = 0; i < nd; i++) { s_big_mul_small(&a, 10); s_big_add_small(&a, (unsigned int)(digits[i] - '0')); }
    s_big_set(&b, h);
    if (e >= 0) s_big_mul_pow10(&a, e); else s_big_mul_pow10(&b, -e);
    if (t >= 0) s_big_shl(&b, t);       else s_big_shl(&a, -t);
    return s_big_cmp(&a, &b);
}

static unsigned int s_big_divmod_small(SBig *b, unsigned int d) {
    unsigned long long rem = 0;
    for (int i = b->n - 1; i >= 0; i--) {
        unsigned long long cur = (rem << 32) | b->w[i];
        b->w[i] = (unsigned int)(cur / d);
        rem = cur % d;
    }
    while (b->n > 0 && b->w[b->n - 1] == 0) b->n--;
    return (unsigned int)rem;
}

typedef union { double d; unsigned long long u; } SBits;

// Every decimal digit of |v| (finite, nonzero), most significant first, trailing
// zeros dropped: |v| = 0.d1d2d3... * 10^(*dexp + 1). A double is m * 2^k, which is
// m * 5^-k * 10^k when k is negative, so the digits are those of an exact integer.
static int s_exact_digits(double v, char *out, int cap, int *dexp) {
    SBits b;
    b.d = v;
    unsigned long long ef = (b.u >> 52) & 0x7ff, mf = b.u & ((1ull << 52) - 1);
    unsigned long long m = ef ? (mf | (1ull << 52)) : mf;
    int k = ef ? (int)ef - 1075 : -1074;
    SBig n;
    s_big_set(&n, m);
    int p10 = 0;
    if (k >= 0) s_big_shl(&n, k);
    else {
        int r = -k;
        for (; r >= 13; r -= 13) s_big_mul_small(&n, 1220703125u);   // 5^13
        for (; r > 0; r--) s_big_mul_small(&n, 5);
        p10 = k;
    }
    char rev[S_NUM_DIGITS + 16];
    int nr = 0;
    while (n.n > 0 && nr + 9 <= (int)sizeof(rev)) {
        unsigned int chunk = s_big_divmod_small(&n, 1000000000u);
        for (int i = 0; i < 9 && (n.n > 0 || chunk); i++) { rev[nr++] = (char)('0' + chunk % 10); chunk /= 10; }
    }
    int nd = 0;
    while (nr > 0 && nd < cap) out[nd++] = rev[--nr];
    *dexp = nd - 1 + p10 + nr;
    while (nd > 1 && out[nd - 1] == '0') nd--;
    return nd;
}

// Parse a decimal string to double, correctly rounded (as strtod): an estimate,
// then corrected against the exact halfway points to its neighbours.
double s_to_number(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if      (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }

    // The value is D * 10^E, D the significant digits -- at most S_NUM_DIGITS of
    // them. `sticky` records a nonzero digit dropped past that, which can only
    // matter at an exact tie.
    char dig[S_NUM_DIGITS];
    int nd = 0, E = 0, sticky = 0, frac = 0;
    for (;; s++) {
        char c = *s;
        if (c == '.' && !frac) { frac = 1; continue; }
        if (c < '0' || c > '9') break;
        if (nd == 0 && c == '0') { if (frac) E--; continue; }
        if (nd < S_NUM_DIGITS) { dig[nd++] = c; if (frac) E--; }
        else { if (c != '0') sticky = 1; if (!frac) E++; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if      (*s == '-') { eneg = 1; s++; }
        else if (*s == '+') { s++; }
        // Saturated: past 400 the result is already inf or 0, and a typed
        // 1e-320116277 must not overflow the exponent.
        int e = 0;
        while (*s >= '0' && *s <= '9') { e = e * 10 + (*s++ - '0'); if (e > 400) e = 400; }
        E += eneg ? -e : e;
    }
    while (nd > 0 && dig[nd - 1] == '0') { nd--; E++; }
    SBits b;
    if (nd == 0 || nd + E < -325) { b.d = 0.0; if (neg) b.u |= 1ull << 63; return b.d; }
    if (nd + E > 310)             { b.u = 0x7ff0000000000000ull; if (neg) b.u |= 1ull << 63; return b.d; }

    // Estimate: the leading 19 digits exactly, then scaled -- a few ulp out at most.
    static const double P10[23] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
    int k = nd < 19 ? nd : 19;
    unsigned long long head = 0;
    for (int i = 0; i < k; i++) head = head * 10 + (unsigned long long)(dig[i] - '0');
    double x = (double)head;
    int e10 = E + (nd - k);
#if !((defined(__i386__) && !defined(__SSE2_MATH__)) || (defined(_M_IX86) && (!defined(_M_IX86_FP) || _M_IX86_FP < 2)))
    // Exact fast path: at most 15 digits is an exact double, and so is 10^e10
    // up to 22, so one IEEE multiply or divide is already correctly rounded.
    // Not under x87, whose extended precision would round twice.
    if (nd <= 15 && !sticky && e10 >= -22 && e10 <= 22) {
        b.d = e10 >= 0 ? x * P10[e10] : x / P10[-e10];
        if (neg) b.u |= 1ull << 63;
        return b.d;
    }
#endif
    while (e10 > 22)  { x *= 1e22; e10 -= 22; }
    if (e10 > 0) x *= P10[e10];
    while (e10 < -22) { x /= 1e22; e10 += 22; }
    if (e10 < 0) x /= P10[-e10];

    // Correct it: step to a neighbour while the exact value lies past the halfway
    // point between them, and break an exact tie to the even mantissa.
    b.d = x;
    const unsigned long long MAXF = 0x7fefffffffffffffull;
    if (b.u > MAXF) b.u = MAXF;
    for (int iter = 0; iter < 64; iter++) {
        unsigned long long ef = b.u >> 52, mf = b.u & ((1ull << 52) - 1);
        unsigned long long m = ef ? (mf | (1ull << 52)) : mf;
        int ke = ef ? (int)ef - 1075 : -1074;
        int c = s_cmp_dec_bin(dig, nd, E, 2 * m + 1, ke - 1);
        if (c == 0 && sticky) c = 1;
        if (c > 0 || (c == 0 && (m & 1))) {
            if (b.u == MAXF) { b.u = 0x7ff0000000000000ull; break; }
            b.u++;
            if (c == 0) break;
            continue;
        }
        if (m == 0) break;
        int narrow = (mf == 0 && ef > 1);
        c = s_cmp_dec_bin(dig, nd, E, narrow ? 4 * m - 1 : 2 * m - 1, narrow ? ke - 2 : ke - 1);
        if (c == 0 && sticky) c = 1;
        if (c < 0 || (c == 0 && (m & 1))) { b.u--; if (c == 0) break; continue; }
        break;
    }
    if (neg) b.u |= 1ull << 63;
    return b.d;
}

// Format a double as a decimal string, matching printf("%g", d) at precision 6.
// - integer-valued doubles in long-long range are printed without a decimal point
// - 6 significant digits, trailing zeros stripped
// - scientific notation (e+XX) when exponent < -4 or >= 6

// Extract `nsig` significant digits from a value already normalised into
// [1, 10), rounding from the (nsig+1)-th digit and stripping trailing zeros.
// `*exp` is bumped when the rounding carries out of the leading digit
// (9.99... -> 10 -> 1.0 with one more exponent).  Returns the kept digit count.
static int s_sig_digits(double d, int nsig, char *sig, int *exp) {
    for (int i = 0; i < nsig; i++) {
        int digit = (int)d;
        if (digit > 9) digit = 9;
        sig[i] = (char)('0' + digit);
        d = (d - digit) * 10.0;
    }
    // Round from the (nsig+1)-th digit.
    if ((int)d >= 5) {
        for (int i = nsig - 1; i >= 0; i--) {
            if (++sig[i] <= '9') break;
            sig[i] = '0';
            if (i == 0) { sig[0] = '1'; (*exp)++; } // all 9s carried
        }
    }
    // Strip trailing zeros (matching %g).
    while (nsig > 1 && sig[nsig - 1] == '0') nsig--;
    return nsig;
}

// %g: scientific notation when exp < -4 or exp >= 6 (precision 6), unless a
// form is forced.
static int s_use_exp_form(int flags, int exp) {
    if (flags & S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM) return 0;
    if (flags & S_FROM_NUMBER_FLAG_FORCE_EXP_FORM)     return 1;
    return !(exp >= -4 && exp <= 5);
}

// Write the digit string (no sign) in fixed or scientific form.
// Returns the number of characters written; does not null-terminate.
static int s_write_sig(char *dst, const char *sig, int nsig, int exp, int use_exp) {
    int o = 0;
    if (!use_exp) {
        // Fixed notation.
        if (exp >= 0) {
            int idigits = exp + 1;
            for (int i = 0; i < idigits; i++)
                dst[o++] = (i < nsig) ? sig[i] : '0';
            if (nsig > idigits) {
                dst[o++] = '.';
                for (int i = idigits; i < nsig; i++) dst[o++] = sig[i];
            }
        } else {
            // 0.000...digits
            dst[o++] = '0'; dst[o++] = '.';
            for (int i = 0; i < -(exp + 1); i++) dst[o++] = '0';
            for (int i = 0; i < nsig; i++) dst[o++] = sig[i];
        }
    } else {
        // Scientific notation: 1.23456e+07
        dst[o++] = sig[0];
        if (nsig > 1) {
            dst[o++] = '.';
            for (int i = 1; i < nsig; i++) dst[o++] = sig[i];
        }
        dst[o++] = 'e';
        if (exp >= 0) { dst[o++] = '+'; } else { dst[o++] = '-'; exp = -exp; }
        if (exp >= 100) dst[o++] = (char)('0' + exp / 100);
        dst[o++] = (char)('0' + (exp / 10) % 10);
        dst[o++] = (char)('0' + exp % 10);
    }
    return o;
}


 void s_from_number(double d, char *dst) { s_from_number_flags(d, dst, 0); }
 int s_from_number_flags(double d, char *dst, int flags) {
    int o = 0;
    const double orig = d;

    // Non-finite values first, because the scaling below cannot survive them:
    // it multiplies by 0.1 until the value lands in [1, 10), and inf * 0.1 is
    // still inf, so `while (d >= 10.0)` never terminates. (NaN takes the
    // opposite route -- every comparison is false, so it falls straight
    // through the scaling and hands garbage to s_sig_digits.) Reachable from
    // ordinary script arithmetic: `#disable safe_div_by_zero` then 1.0/0.0.
    if (d != d) {
        dst[o++] = 'n'; dst[o++] = 'a'; dst[o++] = 'n'; dst[o] = '\0';
        return o;
    }
    if (d < -1.7976931348623157e308 || d > 1.7976931348623157e308) {
        if (d < 0.0) dst[o++] = '-';
        dst[o++] = 'i'; dst[o++] = 'n'; dst[o++] = 'f'; dst[o] = '\0';
        return o;
    }

    // Integer-valued in safe long-long range: print without decimal point.
    if (d > -1e18 && d < 1e18 && d == (double)(long long)d) {
        long long v = (long long)d;
        int neg = (v < 0);
        unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL
                                   : (unsigned long long)v;
        char tmp[24]; int p = 0;
        if (u == 0) tmp[p++] = '0';
        while (u) { tmp[p++] = (char)('0' + u % 10); u /= 10; }
        if (neg) dst[o++] = '-';
        while (p > 0) dst[o++] = tmp[--p];
        dst[o] = '\0';
        return o;
    }

    // General case: 6 significant digits, %g-style.
// We never cast d to long long here, so there is no overflow for
// large values like 1e20.
    if (d < 0.0) { dst[o++] = '-'; d = -d; }

    if (d == 0.0) { dst[o++] = '0'; dst[o] = '\0'; return o; }

    // Scale d into [1, 10) and track the base-10 exponent.
// Coarse steps first to avoid O(exponent) iterations for extreme values.
    int exp = 0;
//    if (d >= 1e256) { d *= 1e-256; exp += 256; } // better without loops?
//    if (d >= 1e128) { d *= 1e-128; exp += 128; } 
//    if (d >= 1e64)  { d *= 1e-64;  exp += 64;  }
//    if (d >= 1e32)  { d *= 1e-32;  exp += 32;  }
//    if (d >= 1e16)  { d *= 1e-16;  exp += 16;  }
//    if (d >= 1e8)   { d *= 1e-8;   exp += 8;   }
//    if (d >= 1e4)   { d *= 1e-4;   exp += 4;   } 
//    if (d >= 1e2)   { d *= 1e-2;   exp += 2;   } 
//    if (d >= 1e1)   { d *= 1e-1;   exp += 1;   } 
//    if (d < 1e-256) { d *= 1e256; exp -= 256; }
//    if (d < 1e-128) { d *= 1e128; exp -= 128; } 
//    if (d < 1e-64)  { d *= 1e64;  exp -= 64;  }
//    if (d < 1e-32)  { d *= 1e32;  exp -= 32;  }
//    if (d < 1e-16)  { d *= 1e16;  exp -= 16;  }
//    if (d < 1e-8)   { d *= 1e8;   exp -= 8;   }
//    if (d < 1e-4)   { d *= 1e4;   exp -= 4;   } 
//    if (d < 1e-2)   { d *= 1e2;   exp -= 2;   } 
//    if (d < 1.0)    { d *= 1e1;   exp -= 1;   } 

    if (d >= 1e256) { d *= 1e-256; exp += 256; }
    if (d >= 1e64)  { d *= 1e-64;  exp += 64;  }
    if (d >= 1e32)  { d *= 1e-32;  exp += 32;  }
    if (d >= 1e16)  { d *= 1e-16;  exp += 16;  }
    if (d >= 1e8)   { d *= 1e-8;   exp += 8;   }
    while (d >= 10.0) { d *= 0.1; exp++; }
    if (d < 1e-256) { d *= 1e256; exp -= 256; }
    if (d < 1e-64)  { d *= 1e64;  exp -= 64;  }
    if (d < 1e-32)  { d *= 1e32;  exp -= 32;  }
    if (d < 1e-16)  { d *= 1e16;  exp -= 16;  }
    if (d < 1e-8)   { d *= 1e8;   exp -= 8;   }
    while (d < 1.0) { d *= 10.0; exp--; }
    // d is now in [1, 10).

    // Extract the significant digits.  %g stops at 6, which is fine for a
    // readout but destroys any value carrying more than 6 of them.  A caller
    // whose text IS the value (a source literal, generated code) passes
    // SHORTEST and gets the fewest digits that s_to_number reads back as the
    // exact same double -- without it, scrubbing a literal to 110566002.1
    // rewrote it as 110566000.
    char sig[S_FROM_NUMBER_MAX_SIG + 4];
    int  base_exp = exp;
    int  nsig;
    if (flags & S_FROM_NUMBER_FLAG_SHORTEST) {
        // The exact digits, rounded to 1, 2, ... places until the result reads
        // back as the same double; 17 always does, since s_to_number is exact.
        char full[S_NUM_DIGITS + 16];
        int e0;
        int nfull = s_exact_digits(orig, full, (int)sizeof(full), &e0);
        char tmp[S_FROM_NUMBER_MAX_CHARS];
        nsig = 0;
        for (int try_n = 1; try_n <= S_FROM_NUMBER_MAX_SIG; try_n++) {
            int n = nfull < try_n ? nfull : try_n;
            exp = e0;
            for (int i = 0; i < n; i++) sig[i] = full[i];
            if (nfull > n) {
                // Round half to even on the exact tail.
                int up = full[n] > '5';
                if (full[n] == '5') {
                    int rest = 0;
                    for (int i = n + 1; i < nfull; i++) if (full[i] != '0') rest = 1;
                    up = rest || ((sig[n - 1] - '0') & 1);
                }
                if (up) {
                    int i = n - 1;
                    while (i >= 0 && sig[i] == '9') sig[i--] = '0';
                    if (i >= 0) sig[i]++;
                    else { sig[0] = '1'; exp++; }
                }
            }
            while (n > 1 && sig[n - 1] == '0') n--;
            int t = 0;
            if (o) tmp[t++] = '-';   // o is 1 here only when a minus was written
            t += s_write_sig(tmp + t, sig, n, exp, s_use_exp_form(flags, exp));
            tmp[t] = '\0';
            if (s_to_number(tmp) == orig) { nsig = n; break; }
        }
        if (!nsig) { exp = base_exp; nsig = s_sig_digits(d, S_FROM_NUMBER_MAX_SIG, sig, &exp); }
    } else {
        nsig = s_sig_digits(d, 6, sig, &exp);
    }

    o += s_write_sig(dst + o, sig, nsig, exp, s_use_exp_form(flags, exp));
    dst[o] = '\0';
    return o;
}





// Supports %s, %d, %x, %c, %f, %g, and %%
int s_vsnprintf(char *str, size_t size, const char *format, va_list args) {
    if (size == 0) return 0;
    size_t out = 0;

    while (*format) {
        if (*format == '%') {
            format++; // Move past '%'
            if (*format == '\0') break;

            // 1. Explicitly handle literal '%'
            if (*format == '%') {
                if (out < size - 1) str[out++] = '%';
                format++;
                continue;
            }

            int width = 0;
            // Parse multi-digit widths
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            // 2. Parse precision BEFORE checking the type specifier
            int precision = -1; // -1 indicates no precision was provided
            if (*format == '.') {
                format++;
                precision = 0;
                while (*format >= '0' && *format <= '9') {
                    precision = precision * 10 + (*format - '0');
                    format++;
                }
            }

            // Length modifier
            int is_longlong = 0;
            if (*format == 'l') {
                format++;
                if (*format == 'l') { is_longlong = 1; format++; }
            }

            if (*format == 's') {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                
                // 3. Apply width padding to strings
                int len = 0;
                while (s[len]) len++;
                
                while (width > len && out < size - 1) {
                    str[out++] = ' ';
                    width--;
                }
                
                while (*s) {
                    if (out < size - 1) {
                        str[out++] = *s;
                    }
                    s++;
                }
                format++;
            } 
            else if (*format == 'd' || *format == 'x') {
                int is_hex = (*format == 'x');
                char buf[64];
                int p = 0;
                if (is_longlong) {
                    long long val = va_arg(args, long long);
                    unsigned long long uval;
                    int neg = 0;
                    if (!is_hex && val < 0) {
                        neg = 1;
                        uval = (unsigned long long)(-val);
                    } else {
                        uval = (unsigned long long)val;
                    }
                    if (uval == 0) {
                        buf[p++] = '0';
                    } else {
                        unsigned long long base = is_hex ? 16ULL : 10ULL;
                        while (uval > 0) {
                            int rem = (int)(uval % base);
                            if (rem < 10) buf[p++] = '0' + rem;
                            else buf[p++] = 'a' + (rem - 10);
                            uval /= base;
                        }
                    }
                    if (neg) buf[p++] = '-';
                } else {
                    int val = va_arg(args, int);
                    unsigned int uval;
                    int neg = 0;
                    if (!is_hex && val < 0) {
                        neg = 1;
                        uval = 0u - (unsigned int)val;
                    } else {
                        uval = (unsigned int)val;
                    }
                    if (uval == 0) {
                        buf[p++] = '0';
                    } else {
                        unsigned int base = is_hex ? 16 : 10;
                        while (uval > 0) {
                            int rem = uval % base;
                            if (rem < 10) buf[p++] = '0' + rem;
                            else buf[p++] = 'a' + (rem - 10);
                            uval /= base;
                        }
                    }
                    if (neg) buf[p++] = '-';
                }
                
                // Add padding if width is larger than string length
                while (p < width && p < (int)sizeof(buf) - 1) {
                    buf[p++] = ' ';
                }

                // Copy to output buffer safely
                while (p > 0) {
                    if (out < size - 1) {
                        str[out++] = buf[--p];
                    } else {
                        --p;
                    }
                }
                format++;
            }
            // 5. Explicitly handle chars so we actually consume va_arg
            else if (*format == 'c') {
                char c = (char)va_arg(args, int);
                if (out < size - 1) str[out++] = c;
                format++;
            }
            else if (*format == 'g' || *format == 'f') {
                double number = va_arg(args, double); 
                char numtmp[S_FROM_NUMBER_MAX_CHARS];
                
                // If s_from_number supports precision, you can now pass `precision` to it.
                s_from_number(number, numtmp);
                
                const char *s = numtmp;
                while (*s) {    
                    if (out < size - 1) {
                        str[out++] = *s;
                    }
                    s++;
                }
                format++;
            } else {
                // Unknown specifier, just print the character
                if (out < size - 1) str[out++] = *format;
                format++;
            }
        } else {
            if (out < size - 1) str[out++] = *format;
            format++;
        }
    }

    str[out] = '\0';
    return out; 
}

int s_snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = s_vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}
