#ifndef VM_FMT_H
#define VM_FMT_H

// Number/byte formatting into a caller-owned byte buffer: the runtime half of the
// VM's string building (IR_FMT, see the IROp comments in vm_types.h).
//
// The model is zig's std.fmt.bufPrint rather than rust's format!: there is no
// allocator anywhere in the VM, so the destination buffer is *part of the
// operation* and the result is a subslice of it. Every function takes (dst, cap,
// at) and returns the new cursor; nothing is written at or past `cap`, so an
// undersized buffer truncates silently. That is the only failure mode, and it needs
// no error path. No libc: the digits are produced by hand.
//
// IMPORTANT: vm_emit_c.c carries a second, textual copy of these functions (the
// EC_FMT_* strings) which it writes into generated C. Change one, change the other.
// tests/test_strfmt.c pins the interpreter's output and checks the emitted C
// defines and calls the matching helper, but it cannot compile the emitted text, so
// a divergence *inside* a helper body is on the author to avoid.
//
// `static inline` so a build that never reaches IR_FMT doesn't warn about unused
// statics.

// Longest decimal run any helper here can produce, plus the sign.
#define VM_FMT_MAX_DIGITS 20

static inline int vm_fmt_bytes(unsigned char *dst, int cap, int at,
                               const unsigned char *src, int n) {
    if (at < 0) at = 0;
    for (int i = 0; i < n && at < cap; i++) dst[at++] = src[i];
    return at;
}

// Unsigned decimal, left-padded with '0' to `min_digits` (0 = no padding, but
// a zero value still prints "0"). The 32-bit form exists so the common integer
// paths don't drag in a 64-bit divide helper on 32-bit targets.
static inline int vm_fmt_udec32(unsigned char *dst, int cap, int at,
                                unsigned int v, int min_digits) {
    unsigned char tmp[VM_FMT_MAX_DIGITS];
    int n = 0;
    if (min_digits > 10) min_digits = 10;
    while (v) { tmp[n++] = (unsigned char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n < min_digits) tmp[n++] = '0';
    if (n == 0) tmp[n++] = '0';
    while (n > 0 && at < cap) dst[at++] = tmp[--n];
    return at;
}

static inline int vm_fmt_udec64(unsigned char *dst, int cap, int at,
                                unsigned long long v, int min_digits) {
    unsigned char tmp[VM_FMT_MAX_DIGITS];
    int n = 0;
    if (min_digits > 19) min_digits = 19;
    while (v) { tmp[n++] = (unsigned char)('0' + (int)(v % 10ull)); v /= 10ull; }
    while (n < min_digits) tmp[n++] = '0';
    if (n == 0) tmp[n++] = '0';
    while (n > 0 && at < cap) dst[at++] = tmp[--n];
    return at;
}

static inline int vm_fmt_i32(unsigned char *dst, int cap, int at, int v) {
    if (at < 0) at = 0;
    unsigned int u;
    if (v < 0) {
        if (at < cap) dst[at++] = '-';
        u = (unsigned int)(-(long long)v);   // via long long so INT_MIN is safe
    } else u = (unsigned int)v;
    return vm_fmt_udec32(dst, cap, at, u, 0);
}

static inline int vm_fmt_i64(unsigned char *dst, int cap, int at, long long v) {
    if (at < 0) at = 0;
    unsigned long long u;
    if (v < 0) {
        if (at < cap) dst[at++] = '-';
        u = (unsigned long long)(~(unsigned long long)v) + 1ull;  // -v, LLONG_MIN safe
    } else u = (unsigned long long)v;
    return vm_fmt_udec64(dst, cap, at, u, 0);
}

// Fixed-point QM.N: `raw` is the stored integer, `shift` is N. Rounds the
// fractional part half-away-from-zero at `decimals` places; `decimals == 0`
// prints just the rounded-down integer part with no '.'.
static inline int vm_fmt_fx(unsigned char *dst, int cap, int at,
                            int raw, int shift, int decimals) {
    if (at < 0) at = 0;
    if (shift <= 0) return vm_fmt_i32(dst, cap, at, raw);
    if (shift > 31) shift = 31;
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;

    long long r = raw;
    if (r < 0) { if (at < cap) dst[at++] = '-'; r = -r; }

    long long one = 1LL << shift;
    long long ip  = r >> shift;
    long long fp  = r & (one - 1);
    long long scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    // fp < 2^31 and scale <= 1e9, so the product stays well inside i64.
    long long fr = (fp * scale + (one >> 1)) >> shift;
    if (fr >= scale) { fr -= scale; ip += 1; }

    at = vm_fmt_udec32(dst, cap, at, (unsigned int)ip, 0);
    if (decimals > 0) {
        if (at < cap) dst[at++] = '.';
        at = vm_fmt_udec32(dst, cap, at, (unsigned int)fr, decimals);
    }
    return at;
}

// Plain decimal notation (no exponent form). Values too large for the integer
// part to be represented print as "big"; NaN prints as "nan". Neither is
// reachable from ordinary script arithmetic, but silently emitting garbage
// bytes into a draw string would be worse than a marker.
static inline int vm_fmt_f64(unsigned char *dst, int cap, int at,
                             double v, int decimals) {
    if (at < 0) at = 0;
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;
    if (!(v == v)) return vm_fmt_bytes(dst, cap, at, (const unsigned char *)"nan", 3);
    if (v < 0) { if (at < cap) dst[at++] = '-'; v = -v; }
    if (!(v < 1.8e19)) return vm_fmt_bytes(dst, cap, at, (const unsigned char *)"big", 3);

    long long scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    unsigned long long fr = (unsigned long long)(frac * (double)scale + 0.5);
    if (fr >= (unsigned long long)scale) { fr -= (unsigned long long)scale; ip += 1ull; }

    at = vm_fmt_udec64(dst, cap, at, ip, 0);
    if (decimals > 0) {
        if (at < cap) dst[at++] = '.';
        at = vm_fmt_udec64(dst, cap, at, fr, decimals);
    }
    return at;
}

#endif // VM_FMT_H
