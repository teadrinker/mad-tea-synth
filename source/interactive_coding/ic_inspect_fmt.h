#ifndef IC_INSPECT_FMT_H
#define IC_INSPECT_FMT_H

// ===========================================================================
// Hover inspection: the statistics, and the one-line label they render as
// ===========================================================================
// What the live editor shows when you hover an expression: the values that
// expression actually took during the run, summarised to fit in a gap in the
// code beside it.
//
//     5                  one call, i32
//     5f                 one call, f32
//     5.0                one call, f64
//     5 7 9              a few calls, listed in call order
//     5 (10)             ten calls, all the same value
//     3-7 (10)           ten calls, spread over a range
//     1.250 (fx16)       one call, and a type the spelling cannot show
//     [3, 4] (i32)       an array or slice: its elements, always with their type
//     [1, 2] [3, 4] (i32)     ...several calls, each array whole
//     [1, 2] (10 i32)    ...all ten the same
//
// Every real (f32, f64, fixed point) is rounded to three digits past the point
// and padded to them -- 1.5 reads 1.500, f32 as 1.500f -- while a whole value
// stays 5.0 and one under 0.001 keeps its own digits.
//     5.0 (10 fx16)      ...both
//
// A run of reals that were all whole numbers drops the zeros and names its type
// instead: `1 2 6 24 (f64)`, `[1, 2] (f32)`, `2 3 (fx16)`. A single value keeps
// `5.0` / `5.0 (10)`, which is shorter than `5 (f64)` and says the same.
//
// A run whose type CHANGED (one expression in two specialisations of a
// function) says `mixed` where the type would go: `25 36 (mixed)`, or
// `25.0 6.250 (mixed)` when a reading had a fraction.
//
// The values come first and everything ABOUT them goes in the parentheses: the
// call count when there is one to give, then the type when the values do not
// spell it out. Either, both, or neither.
//
// Header-only and `static inline`, so it unit-tests without linking the editor.
//
// Every statistic is kept in double, which is lossless for the three kinds the
// VM reports and keeps the summary free of a per-kind branch. It would NOT be
// lossless for i64, which is one reason i64 is not reported yet.
//
// ARRAYS. An aggregate is not summarised: there is no useful min/max of a list,
// so none is tracked and the history is used differently. It is one flat pool of
// IC_INSPECT_HISTORY numbers filled with WHOLE arrays in call order -- room for
// thirty-two 1-element arrays, sixteen 2-element ones, and so on -- and a call
// that no longer fits is counted and dropped. See ic_inspect_record_array.

#include "vm/vm.h"              // VTKind: VMT_I32 / VMT_F32 / VMT_F64
#include "common/string_pure.h"

// How many values are kept: the FIRST this many. Past the cap a call updates
// min/max and the count and stores nothing, so a ten-thousand-sample loop costs
// a comparison each and is read from its beginning -- the first iterations are
// what a reader wants, and the range and count summarise the tail.
//
// Sized for the results-panel line, which fills to the panel's WIDTH, rather
// than for the in-code label. ~4KB across the 16 slots.
#define IC_INSPECT_HISTORY 32

// Past this many calls the in-code label switches to the range form. Its own
// constant because the label is bounded by the gap it sits in rather than by
// what was recorded.
#define IC_INSPECT_LIST_MAX IC_INSPECT_HISTORY

// The most elements of ONE array a reading ever spells out: the whole pool, since
// a single array is allowed to fill it. Sizes the formatter's scratch space.
#define IC_INSPECT_ARR_ELEMS IC_INSPECT_HISTORY

// Says later calls are not on the line -- without it the first three readings
// of forty read as a complete record of three calls. A token like a value is,
// joined by the same single space rather than carrying one inside it.
#define IC_INSPECT_ELIDE     "..."
#define IC_INSPECT_ELIDE_LEN 3

typedef struct {
    // From the first call. VMT_VOID (0) means nothing was ever reported: the
    // run never reached the expression, or its type is one the VM does not
    // report.
    int       kind;
    // The fixed-point shift riding on `kind` when it is VMT_I32, else 0. Kind
    // and shift together are the whole type, which is what lets a label say
    // `fx16`.
    int       shift;
    int       mixed;                        // a later call disagreed with the type
    int       frac;                         // a real reading was not a whole number
    long long calls;
    double    min, max;                     // NaN calls are counted but excluded; scalars only
    int       has_range;                    // at least one non-NaN value seen
    int       has_nan;                      // ...and at least one NaN
    // The first min(calls, IC_INSPECT_HISTORY) values, in call order. A prefix,
    // never a ring. For an array run it is the elements of the stored arrays laid
    // end to end, and `rec_len` says where one stops.
    double    recent[IC_INSPECT_HISTORY];

    // ---- array runs only (`is_array`; kind/shift then describe one ELEMENT) ----
    int       is_array;
    int       all_same;                     // every call so far equalled the first, element for element
    int       nrec;                         // arrays stored
    int       used;                         // numbers of `recent` in use
    // Per stored array: how many elements are in `recent`, and its real length.
    // They differ when the array was cut -- the sink delivers at most
    // VM_INSPECT_ARR_MAX of them, and the first is kept even when it outgrows the
    // pool. Every other array is stored whole or not at all.
    int       rec_len[IC_INSPECT_HISTORY];
    int       rec_total[IC_INSPECT_HISTORY];
} ICInspect;

// An i64 reading arrives as its bit pattern in the double (VMInspectFn), so it is
// compared and printed through these, never as the double it looks like: 0 and
// INT64_MIN would compare equal as +0.0/-0.0, and a NaN pattern not even to itself.
static inline long long ic_ins_bits(double v) { union { double d; long long i; } u; u.d = v; return u.i; }
static inline double ic_ins_from_ll(long long v) { union { double d; long long i; } u; u.i = v; return u.d; }
static inline int ic_ins_lt(int kind, double a, double b) { return kind == VMT_I64 ? ic_ins_bits(a) < ic_ins_bits(b) : a < b; }
static inline int ic_ins_eq(int kind, double a, double b) { return kind == VMT_I64 ? ic_ins_bits(a) == ic_ins_bits(b) : (a == b || (a != a && b != b)); }

// The kind a run is formatted as. A type that changed mid-run is neither, so it
// falls back to the plainest reading -- except when the stored values are i64
// bit patterns, which read as f64 would be garbage.
static inline int ic_ins_fmt_kind(int mixed, int kind) { return mixed && kind != VMT_I64 ? VMT_F64 : kind; }

// Neither nan/inf nor a magnitude past 2^53 counts against it: the first are
// spelled the same either way, and every double that large is whole anyway, so
// there is no fraction it could show.
static inline int ic_ins_is_whole(double v) {
    if (v != v || v >= 9007199254740992.0 || v <= -9007199254740992.0) return 1;
    return v == (double)(long long)v;
}

static inline int ic_ins_is_real(int kind, int shift) { return kind == VMT_F32 || kind == VMT_F64 || (kind == VMT_I32 && shift > 0); }

// True when every reading of a real run was a whole number, so the values can be
// printed as integers with the type named in the note instead.
// Every reading was the same value: min == max with no NaN beside them, or NaN
// throughout.
static inline int ic_ins_all_same(const ICInspect *s, int kind) {
    if (!s->has_range) return s->has_nan;
    return !s->has_nan && ic_ins_eq(kind, s->min, s->max);
}

// A mixed run counts too, unless it holds i64 bit patterns: whatever its types,
// no reading had a fraction to show.
static inline int ic_ins_whole(const ICInspect *s) {
    if (s->frac || s->calls <= 0) return 0;
    return s->mixed ? s->kind != VMT_I64 : ic_ins_is_real(s->kind, s->shift);
}

// The type in a note: `mixed` for a run whose type changed, else as given.
static inline void ic_ins_mixed_type(const ICInspect *s, char *type) {
    if (s->mixed) s_strcpy(type, "mixed");
}

static inline void ic_inspect_reset(ICInspect *s) {
    if (!s) return;
    s->kind = VMT_VOID;
    s->shift = 0;
    s->mixed = 0;
    s->frac = 0;
    s->calls = 0;
    s->min = s->max = 0.0;
    s->has_range = 0;
    s->has_nan = 0;
    for (int i = 0; i < IC_INSPECT_HISTORY; i++) s->recent[i] = 0.0;
    s->is_array = 0;
    s->all_same = 0;
    s->nrec = 0;
    s->used = 0;
    for (int i = 0; i < IC_INSPECT_HISTORY; i++) s->rec_len[i] = s->rec_total[i] = 0;
}

// One reported value -- the VMInspectFn sink's whole job. `shift` is part of
// the type, so a run that changed it counts as mixed just as a kind does.
static inline void ic_inspect_record(ICInspect *s, int kind, int shift, double v) {
    if (!s) return;
    // A scalar after an array (or the reverse, below) is a different shape, not
    // another sample: counted, flagged, and kept out of a history it does not fit.
    if (s->is_array) {
        s->mixed = 1;
        if (s->calls < 0x7FFFFFFFFFFFFFFFLL) s->calls++;
        return;
    }
    if (s->calls == 0) { s->kind = kind; s->shift = shift; }
    else if (kind != s->kind || shift != s->shift) s->mixed = 1;
    if (ic_ins_is_real(kind, shift) && !ic_ins_is_whole(v)) s->frac = 1;

    // One history never holds both bit patterns and numbers: a mixed reading is
    // converted to the first one's encoding.
    if (kind == VMT_I64 && s->kind != VMT_I64) v = (double)ic_ins_bits(v);
    else if (kind != VMT_I64 && s->kind == VMT_I64) v = ic_ins_from_ll(v > -9.2e18 && v < 9.2e18 ? (long long)v : 0);

    // NaN is kept among the values but excluded from the range: it compares
    // false against everything, so min/max would swallow every real value.
    if (s->kind == VMT_I64 || v == v) {
        if (!s->has_range) { s->min = s->max = v; s->has_range = 1; }
        else { if (ic_ins_lt(s->kind, v, s->min)) s->min = v; if (ic_ins_lt(s->kind, s->max, v)) s->max = v; }
    } else s->has_nan = 1;

    // Full: the range and count above are this call's whole record.
    if (s->calls < (long long)IC_INSPECT_HISTORY) s->recent[(int)s->calls] = v;
    if (s->calls < 0x7FFFFFFFFFFFFFFFLL) s->calls++;
}

// One array or slice reported -- the VMInspectArrFn sink's whole job. `kind` and
// `shift` are the ELEMENT's; `v` holds the first `n` of `total` elements.
//
// The array is stored if it fits in what is left of the pool, else it is only
// counted. The one exception is the very first: an array longer than the whole
// pool would otherwise leave the history empty, so it is kept truncated -- which
// is why a record has both a stored and a real length.
//
// No range is kept. Whether this call equalled the first is tracked instead
// (`all_same`), because ten identical arrays are one reading and a label that
// says so is worth more than ten copies of it. A first array that was cut can
// never be confirmed equal, so it never collapses.
static inline void ic_inspect_record_array(ICInspect *s, int kind, int shift,
                                           int total, const double *v, int n) {
    if (!s) return;
    if (total < 0) total = 0;
    if (n < 0) n = 0;
    if (n > total) n = total;
    if (ic_ins_is_real(kind, shift))
        for (int i = 0; i < n && !s->frac; i++) if (!ic_ins_is_whole(v[i])) s->frac = 1;

    if (s->calls == 0) {
        s->kind = kind; s->shift = shift;
        s->is_array = 1;
        s->all_same = 1;
    } else if (!s->is_array || kind != s->kind || shift != s->shift) {
        s->mixed = 1;
        s->all_same = 0;                             // a different type is a different reading
    } else if (s->all_same) {
        int len0 = s->nrec > 0 ? s->rec_len[0] : 0;
        if (s->nrec == 0 || total != s->rec_total[0] || len0 < total || n < total) {
            s->all_same = 0;
        } else {
            for (int i = 0; i < n; i++) {
                // NaN equals NaN here: an array of NaNs is unchanged, not different.
                if (!ic_ins_eq(kind, v[i], s->recent[i])) { s->all_same = 0; break; }
            }
        }
    }

    // Bit patterns and numbers never share the pool: a mixed one is only counted.
    if (s->is_array && s->nrec < IC_INSPECT_HISTORY && (kind == VMT_I64) == (s->kind == VMT_I64)) {
        int room = IC_INSPECT_HISTORY - s->used;
        int keep = -1;
        if (n <= room)            keep = n;
        else if (s->nrec == 0)    keep = room;       // the first is never left out
        if (keep >= 0) {
            for (int i = 0; i < keep; i++) s->recent[s->used + i] = v[i];
            s->rec_len[s->nrec]   = keep;
            s->rec_total[s->nrec] = total;
            s->used += keep;
            s->nrec++;
        }
    }
    if (s->calls < 0x7FFFFFFFFFFFFFFFLL) s->calls++;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Append `src` at dst[*len], never past dst_size-1. Returns 0 once the buffer
// is full, so a caller can stop early; stays NUL-terminated either way.
static inline int ic_ins_append(char *dst, int dst_size, int *len, const char *src) {
    if (!dst || dst_size <= 0) return 0;
    int i = 0;
    while (src[i] && *len < dst_size - 1) dst[(*len)++] = src[i++];
    dst[*len] = '\0';
    return src[i] == '\0';
}

static inline int ic_ins_append_ll(char *dst, int dst_size, int *len, long long v) {
    char tmp[24];
    int  n = 0;
    int  neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) tmp[n++] = '-';
    char out[25];
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return ic_ins_append(dst, dst_size, len, out);
}

// How far a fixed-point reading is rounded for display. A shift of 16 spaces
// its values 0.0000153 apart, so the exact one is a screenful of digits saying
// nothing -- 0.3 reads as 0.30000305175781250.
#define IC_INSPECT_FX_ROUND  1000.0     // three digits after the point
#define IC_INSPECT_FX_DIGITS 3          // ...and the width they are padded out to

// Fixed-point readings only, and only where rounding leaves something to read:
// below the third digit the rounded value is 0 or 0.001, which says less than
// the exact one, so small values are left alone. Non-finite and huge values are
// passed through -- the multiply would overflow the cast.
static inline double ic_ins_round_fx(double v) {
    double a = v < 0 ? -v : v;
    if (!(a >= 1.0 / IC_INSPECT_FX_ROUND) || !(a < 1e12)) return v;
    double scaled = v * IC_INSPECT_FX_ROUND;
    long long r = (long long)(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
    return (double)r / IC_INSPECT_FX_ROUND;
}

// ic_ins_round_fx spelled straight from the rounded integer. Dividing back to a
// double and printing that breaks under -ffast-math (the wasm builds): r / 1000.0
// becomes r * 0.001, 1001 * 0.001 is 1.0010000000000001, and SHORTEST prints it.
// Same range and same text as round + pad. Returns 0 when v is out of range.
static inline int ic_ins_fixed3(double v, char *dst, int dst_size) {
    double a = v < 0 ? -v : v;
    if (!(a >= 1.0 / IC_INSPECT_FX_ROUND) || !(a < 1e12)) return 0;
    long long r = (long long)(a * IC_INSPECT_FX_ROUND + 0.5);
    int len = 0;
    if (v < 0) ic_ins_append(dst, dst_size, &len, "-");
    ic_ins_append_ll(dst, dst_size, &len, r / 1000);
    int f = (int)(r % 1000);
    char frac[6] = { '.', '0', '\0', '\0', '\0', '\0' };
    if (f) { frac[1] = (char)('0' + f / 100); frac[2] = (char)('0' + f / 10 % 10); frac[3] = (char)('0' + f % 10); }
    ic_ins_append(dst, dst_size, &len, frac);
    return len;
}

// The rounded reading padded out to IC_INSPECT_FX_DIGITS, so a row of them
// lines up and reads as one precision: 1.2 becomes 1.200 beside 33.550.
//
// A whole number is the exception -- 23.0 has no fraction to pad, and 23.000
// would claim three digits of reading where there is none. So is anything the
// rounding did not touch: a value below the third digit keeps the digits it
// has, and an exponent form has no fraction in the sense meant here.
static inline int ic_ins_pad_fx(char *dst, int dst_size, int len) {
    int dot = -1;
    for (int i = 0; i < len; i++) {
        if (dst[i] == '.') dot = i;
        else if (dst[i] == 'e' || dst[i] == 'E') return len;
    }
    if (dot < 0) return len;
    int frac = len - dot - 1;
    if (frac >= IC_INSPECT_FX_DIGITS) return len;
    if (frac == 1 && dst[dot + 1] == '0') return len;   // the whole-number form
    while (frac < IC_INSPECT_FX_DIGITS && len < dst_size - 1) {
        dst[len++] = '0';
        frac++;
    }
    dst[len] = '\0';
    return len;
}

// One value, spelled the way its kind is written in source:
//   i32 -> 5      f32 -> 5f      f64 -> 5.0      fx16 -> 5.0 / 1.200
//
// SHORTEST is not optional for a value carried in a double: the default %g
// precision of 6 significant digits silently rewrites 110566002.1 as
// 110566000, and a readout that quietly changes the value is worse than no
// readout.
//
// f32 is the exception, and %g's 6 digits are the RIGHT precision for it: the
// value in hand is a float widened to double, so the shortest form that reads
// back as that DOUBLE spells out the widening -- 0.3f as 0.30000001192092896.
// A float carries about 7 decimal digits, so six of them lose at most the last
// one, and none of the digits shown is an artefact.
static inline int ic_ins_num(double v, int kind, int shift, char *dst, int dst_size) {
    int len = 0;
    dst[0] = '\0';
    if (kind == VMT_I64) {
        ic_ins_append_ll(dst, dst_size, &len, ic_ins_bits(v));
        return len;
    }
    char num[S_FROM_NUMBER_MAX_CHARS];
    int real = kind == VMT_F32 || kind == VMT_F64 || shift > 0;
    if (real && kind != VMT_F32) {
        len = ic_ins_fixed3(v, dst, dst_size);
        if (len) return len;
    }
    if (real) v = ic_ins_round_fx(v);
    s_from_number_flags(v, num, kind == VMT_F32 ? 0 : S_FROM_NUMBER_FLAG_SHORTEST);

    if (!ic_ins_append(dst, dst_size, &len, num)) return len;

    // Neither suffix belongs on a non-finite reading: "nanf", "inf.0".
    int finite = 1;
    for (const char *c = num; *c; c++)
        if (*c == 'n' || *c == 'i') { finite = 0; break; }
    if (!finite) return len;

    if (kind == VMT_F32) {
        // Padded before the suffix: 1.500f, not 1.5f00.
        len = ic_ins_pad_fx(dst, dst_size, len);
        ic_ins_append(dst, dst_size, &len, "f");
    } else if (real) {
        // Always a point or an exponent, so a whole real reads as 5.0.
        int has_point = 0;
        for (const char *c = num; *c; c++)
            if (*c == '.' || *c == 'e' || *c == 'E') { has_point = 1; break; }
        if (!has_point) ic_ins_append(dst, dst_size, &len, ".0");
        len = ic_ins_pad_fx(dst, dst_size, len);
    }
    return len;
}

// The type name a label has to state, or "" when the values spell it out. `5`,
// `5f` and `5.0` each say what they are; `5.0` the fx16 and `5` the i64 do not.
//
// Written into `dst` (8 bytes is enough) rather than returned, so a caller can
// MEASURE it before committing -- which the results-panel line has to do.
static inline void ic_ins_type_name(int kind, int shift, char *dst, int dst_size) {
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    int len = 0;
    if (kind == VMT_I64) { ic_ins_append(dst, dst_size, &len, "i64"); return; }
    if (kind != VMT_I32 || shift <= 0) return;
    ic_ins_append(dst, dst_size, &len, "fx");
    ic_ins_append_ll(dst, dst_size, &len, (long long)shift);
}

// The parenthesised note after the values: the call count, the type name, or
// both -- `(10)`, `(fx16)`, `(10 fx16)`. Empty when there is neither. One
// function because the two are one group, budgeted, kept and dropped together:
// a bare `(2)` after `1.5 2.5` reads as f64.
//
// `calls < 0` is "no count here": the forms where the values on screen already
// are the whole record.
static inline void ic_ins_note(long long calls, const char *type, char *dst, int dst_size) {
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    int has_type = type && type[0];
    if (calls < 0 && !has_type) return;
    int len = 0;
    ic_ins_append(dst, dst_size, &len, " (");
    if (calls >= 0) ic_ins_append_ll(dst, dst_size, &len, calls);
    if (calls >= 0 && has_type) ic_ins_append(dst, dst_size, &len, " ");
    if (has_type) ic_ins_append(dst, dst_size, &len, type);
    ic_ins_append(dst, dst_size, &len, ")");
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

// The element type an array reading always states -- `[3, 4] (i32)`. Unlike a
// scalar, where `5` / `5f` / `5.0` spell the type themselves, an array is the one
// thing whose elements' type is worth saying every time: a list of whole numbers
// could as well be any of them.
static inline void ic_ins_arr_type_name(int kind, int shift, char *dst, int dst_size) {
    if (!dst || dst_size <= 0) return;
    dst[0] = '\0';
    int len = 0;
    if (kind == VMT_I32 && shift > 0) {
        ic_ins_append(dst, dst_size, &len, "fx");
        ic_ins_append_ll(dst, dst_size, &len, (long long)shift);
    } else if (kind == VMT_I32) {
        ic_ins_append(dst, dst_size, &len, "i32");
    } else if (kind == VMT_F32) {
        ic_ins_append(dst, dst_size, &len, "f32");
    } else if (kind == VMT_I64) {
        ic_ins_append(dst, dst_size, &len, "i64");
    } else {
        ic_ins_append(dst, dst_size, &len, "f64");
    }
}

// `[3, 4]` from the first `n` of `total` elements, in at most `max_chars`. When
// the list is cut -- the caller only had `n` of them, or they do not all fit --
// it ends `, ...]`, so a partial list never passes for a whole one. Returns the
// length, or 0 when not even `[...]` fits (nothing is written then).
static inline int ic_ins_arr_text(const double *v, int n, int total, int kind, int shift,
                                  char *dst, int dst_size, int max_chars) {
    if (!dst || dst_size <= 0) return 0;
    dst[0] = '\0';
    int cap = max_chars < dst_size - 1 ? max_chars : dst_size - 1;
    if (total <= 0) {
        if (cap < 2 || dst_size < 3) return 0;
        dst[0] = '['; dst[1] = ']'; dst[2] = '\0';
        return 2;
    }
    if (n > total) n = total;
    if (n > IC_INSPECT_ARR_ELEMS) n = IC_INSPECT_ARR_ELEMS;
    if (n < 0) n = 0;

    // Widths first, so how many elements fit is one decision rather than a
    // write-and-retreat.
    char num[IC_INSPECT_ARR_ELEMS][S_FROM_NUMBER_MAX_CHARS + 4];
    int  w[IC_INSPECT_ARR_ELEMS];
    for (int i = 0; i < n; i++) w[i] = ic_ins_num(v[i], kind, shift, num[i], (int)sizeof(num[i]));

    // Largest k whose `[e0, ..., ek-1` (plus `, ...` if any are left out) and
    // closing bracket fit.
    int k = n;
    for (; k >= 0; k--) {
        int need = 2;                                   // the brackets
        for (int i = 0; i < k; i++) need += w[i] + (i ? 2 : 0);
        if (k < total) need += k ? 5 : 3;               // ", ..." / "..."
        if (need <= cap) break;
    }
    if (k < 0) return 0;

    int len = 0;
    ic_ins_append(dst, dst_size, &len, "[");
    for (int i = 0; i < k; i++) {
        if (i) ic_ins_append(dst, dst_size, &len, ", ");
        ic_ins_append(dst, dst_size, &len, num[i]);
    }
    if (k < total) ic_ins_append(dst, dst_size, &len, k ? ", ..." : "...");
    ic_ins_append(dst, dst_size, &len, "]");
    return len;
}

// Stored array `k` of an array run: where its elements start in `recent`.
static inline int ic_ins_rec_start(const ICInspect *s, int k) {
    int at = 0;
    for (int i = 0; i < k; i++) at += s->rec_len[i];
    return at;
}

// One line for an array run, label and results panel both:
//
//     [3, 4] (i32)                    one call
//     [1, 2] [3, 4] (i32)             several, each whole
//     [1, 2] (10 i32)                 ten identical calls, shown once
//     [1, 2] [3, 4] ... (40 i32)      later calls did not fit
//
// The arrays shown are the first that fit, in call order, and whatever is not on
// the line is what the trailing `...` and the count account for. The count goes
// in when values were collapsed or left out -- or always, when `always_count` is
// set (the results panel states it on every line).
//
// `max_chars` bounds the whole line including `prefix`, which is never cut. If
// the FIRST array alone is too wide it is shortened to fit, `[1, 2, 3, ...]`;
// a later one that does not fit is left off whole instead.
static inline int ic_ins_arrays_line(const ICInspect *s, const char *prefix,
                                     char *dst, int dst_size, int max_chars,
                                     int always_count) {
    dst[0] = '\0';
    if (max_chars > dst_size - 1) max_chars = dst_size - 1;
    if (max_chars <= 0) return 0;
    int cap = max_chars + 1;
    int len = 0;
    if (prefix && !ic_ins_append(dst, cap, &len, prefix)) return len;

    // A type that changed mid-run is not to be trusted either way.
    int kind  = ic_ins_fmt_kind(s->mixed, s->kind);
    int shift = s->mixed ? 0       : s->shift;
    // Whole reals are spelled as i32 is; the note already names the real type.
    int vkind  = ic_ins_whole(s) ? VMT_I32 : kind;
    int vshift = ic_ins_whole(s) ? 0       : shift;

    // Ten identical arrays are one reading. Only a first array that was stored
    // whole can be called identical to the rest.
    int collapsed = s->calls > 1 && s->all_same && s->nrec > 0
                 && s->rec_len[0] == s->rec_total[0];
    int nshow     = collapsed ? 1 : s->nrec;
    int omitted   = !collapsed && s->calls > (long long)s->nrec;

    char type[8], tail[40];
    ic_ins_arr_type_name(kind, shift, type, (int)sizeof(type));
    ic_ins_mixed_type(s, type);
    ic_ins_note((collapsed || omitted || always_count) ? s->calls : -1, type, tail, (int)sizeof(tail));
    int tl = (int)s_strlen(tail);
    int budget = max_chars - len - tl;

    // The widest an array token gets: every element at full width, plus brackets
    // and separators.
    char tok[IC_INSPECT_ARR_ELEMS * (S_FROM_NUMBER_MAX_CHARS + 6) + 8];
    const int unbounded = (int)sizeof(tok);
    int fit = 0, marked = 0, first_room = unbounded;
    for (int pass = 0; pass < 2; pass++) {
        int room = budget - (pass ? IC_INSPECT_ELIDE_LEN + 1 : 0);
        int used = 0;
        fit = 0;
        first_room = unbounded;
        for (int k = 0; k < nshow; k++) {
            int start = ic_ins_rec_start(s, k);
            int left  = room - used - (fit ? 1 : 0);
            int n = ic_ins_arr_text(s->recent + start, s->rec_len[k], s->rec_total[k],
                                    vkind, vshift, tok, (int)sizeof(tok), unbounded);
            if (n > left) {
                // Only the first may shrink to fit; anything later is left off whole.
                if (fit) break;
                n = ic_ins_arr_text(s->recent + start, s->rec_len[k], s->rec_total[k],
                                    vkind, vshift, tok, (int)sizeof(tok), left);
                if (n <= 0) break;
                first_room = left;
                fit++;
                break;                                   // a shortened one ends the line
            }
            used += n + (fit ? 1 : 0);
            fit++;
        }
        int missing = fit < nshow || omitted;
        if (pass == 0 && !missing) break;
        // With an array on the line the marker is already paid for; with none it
        // has to fit on its own.
        if (pass) marked = fit > 0 || budget >= IC_INSPECT_ELIDE_LEN;
    }
    int missing = fit < nshow || omitted;
    // No room for the marker means no room for a partial reading either.
    if (!marked && missing) fit = 0;

    for (int k = 0; k < fit; k++) {
        int start = ic_ins_rec_start(s, k);
        if (k) ic_ins_append(dst, cap, &len, " ");
        ic_ins_arr_text(s->recent + start, s->rec_len[k], s->rec_total[k],
                        vkind, vshift, tok, (int)sizeof(tok), k ? unbounded : first_room);
        if (!ic_ins_append(dst, cap, &len, tok)) break;
    }
    if (marked) {
        if (fit) ic_ins_append(dst, cap, &len, " ");
        ic_ins_append(dst, cap, &len, IC_INSPECT_ELIDE);
    }
    if (max_chars - len >= tl) ic_ins_append(dst, cap, &len, tail);
    return len;
}

// The whole label (the forms are at the top of this file). Returns its length,
// or 0 for nothing to show -- the expression was never evaluated, or its type
// is not one the VM reports.
//
// The count goes in only for the forms that COLLAPSE their values; the type
// name joins it when the values cannot spell the type out, and takes the
// parentheses alone when there is no count. Equal-value is tested before the
// list so eight identical readings give `5 (8)`, not `5 5 5 5 5 5 5 5`.
static inline int ic_inspect_format_line(const ICInspect *s, const char *prefix,
                                         char *dst, int dst_size, int max_chars);

static inline int ic_inspect_format(const ICInspect *s, char *dst, int dst_size) {
    if (!s || !dst || dst_size <= 0) return 0;
    dst[0] = '\0';
    if (s->calls == 0) return 0;
    if (s->is_array) return ic_ins_arrays_line(s, NULL, dst, dst_size, dst_size - 1, 0);

    // A type that changed mid-run is neither: fall back to the plainest
    // reading rather than claiming an `fx16` half the readings were not, and
    // say `mixed` instead.
    int kind  = ic_ins_fmt_kind(s->mixed, s->kind);
    int shift = s->mixed ? 0       : s->shift;
    int len   = 0;
    char num[S_FROM_NUMBER_MAX_CHARS + 4];
    char type[8], note[40];
    ic_ins_type_name(kind, shift, type, (int)sizeof(type));
    int same   = ic_ins_all_same(s, kind);
    int single = s->calls == 1 || same;
    int vkind = kind, vshift = shift;
    if (!single && ic_ins_whole(s)) {
        ic_ins_arr_type_name(kind, shift, type, (int)sizeof(type));
        vkind = VMT_I32; vshift = 0;
    }
    ic_ins_mixed_type(s, type);

    // -1 until a branch that COLLAPSED its values asks for the count.
    long long shown = -1;

    if (s->calls == 1) {
        ic_ins_num(s->recent[0], vkind, vshift, num, (int)sizeof(num));
        ic_ins_append(dst, dst_size, &len, num);
    } else if (same) {
        ic_ins_num(s->has_range ? s->min : s->recent[0], vkind, vshift, num, (int)sizeof(num));
        ic_ins_append(dst, dst_size, &len, num);
        shown = s->calls;
    } else if (s->calls <= IC_INSPECT_LIST_MAX) {
        for (int i = 0; i < (int)s->calls; i++) {
            ic_ins_num(s->recent[i], vkind, vshift, num, (int)sizeof(num));
            if ((i && !ic_ins_append(dst, dst_size, &len, " ")) || !ic_ins_append(dst, dst_size, &len, num))
                return ic_inspect_format_line(s, NULL, dst, dst_size, dst_size - 1);
        }
    } else {
        ic_ins_num(s->min, vkind, vshift, num, (int)sizeof(num));
        ic_ins_append(dst, dst_size, &len, num);
        ic_ins_append(dst, dst_size, &len, "-");
        ic_ins_num(s->max, vkind, vshift, num, (int)sizeof(num));
        ic_ins_append(dst, dst_size, &len, num);
        shown = s->calls;
    }

    // Out of room: the results-panel form ends on `...` and a count instead of
    // on half a number or half a note.
    ic_ins_note(shown, type, note, (int)sizeof(note));
    if (!ic_ins_append(dst, dst_size, &len, note))
        return ic_inspect_format_line(s, NULL, dst, dst_size, dst_size - 1);
    return len;
}

// ---------------------------------------------------------------------------
// The results-panel line
// ---------------------------------------------------------------------------

// One line for the results panel: the values a site actually took, filled to
// the available width, with the total call count on the end.
//
//     Inspect L12: 5 7 9 11 13 (5)
//     Inspect L12: 5 7 9 11 ... (42)      <- later calls did not fit
//     Inspect L7: - (0)                   <- pinned, but never executed
//     Inspect L3: 1.5 2.5 (2 fx16)        <- the type, when the values hide it
//
// `prefix` is verbatim and never truncated -- a line whose label is cut
// identifies nothing. Values are the FIRST that fit, in call order, and the
// marker is on the END because the history is a prefix, so what is missing is
// always the tail.
//
// Returns the line's length, or 0 if not even the prefix fits.
static inline int ic_inspect_format_line(const ICInspect *s, const char *prefix,
                                         char *dst, int dst_size, int max_chars) {
    if (!s || !dst || dst_size <= 0) return 0;
    dst[0] = '\0';
    if (max_chars > dst_size - 1) max_chars = dst_size - 1;
    if (max_chars <= 0) return 0;
    // Everything below writes through this, so the width limit and the buffer
    // limit are one check.
    int cap = max_chars + 1;

    if (s->is_array) return ic_ins_arrays_line(s, prefix, dst, dst_size, max_chars, 1);

    int len = 0;
    if (prefix && !ic_ins_append(dst, cap, &len, prefix)) return len;

    // Same mixed-type rule as ic_inspect_format, hoisted above the tail
    // because the tail carries the type name.
    int kind  = ic_ins_fmt_kind(s->mixed, s->kind);
    int shift = s->mixed ? 0       : s->shift;

    // Measured before anything is committed, so the values can be budgeted
    // against the room actually left. Unlike the label, this line ALWAYS states
    // its count: it shows only the values that fit.
    char tail[40];
    char type[8];
    ic_ins_type_name(kind, shift, type, (int)sizeof(type));
    int vkind = kind, vshift = shift;
    if (s->calls > 1 && ic_ins_whole(s)) {
        ic_ins_arr_type_name(kind, shift, type, (int)sizeof(type));
        vkind = VMT_I32; vshift = 0;
    }
    ic_ins_mixed_type(s, type);
    ic_ins_note(s->calls, type, tail, (int)sizeof(tail));
    int tl = (int)s_strlen(tail);

    int budget = max_chars - len - tl;

    if (s->calls == 0) {
        // Pinned and never reached. Saying so is the point: a blank line looks
        // like a pin that is not working.
        if (budget >= 1) ic_ins_append(dst, cap, &len, "-");
        if (max_chars - len >= tl) ic_ins_append(dst, cap, &len, tail);
        return len;
    }

    // Past IC_INSPECT_HISTORY the rest was never stored, so `have < calls` is
    // what puts the marker on a line with room to spare.
    int have = (int)(s->calls < (long long)IC_INSPECT_HISTORY
                     ? s->calls : (long long)IC_INSPECT_HISTORY);

    // How many fit, from the FIRST call forwards. Twice, because the marker is
    // only paid for if something is actually hidden, and paying for it can hide
    // one more value in turn.
    char num[S_FROM_NUMBER_MAX_CHARS + 4];
    int  fit    = 0;
    int  marked = 0;
    for (int pass = 0; pass < 2; pass++) {
        // Pass 1 pays for the marker AND the space in front of it; if nothing
        // fits after that, the `marked` test below hands the space back.
        int room = budget - (pass ? IC_INSPECT_ELIDE_LEN + 1 : 0);
        int used = 0;
        fit = 0;
        for (int k = 0; k < have; k++) {
            int n = ic_ins_num(s->recent[k], vkind, vshift, num, (int)sizeof(num));
            int cost = n + (fit ? 1 : 0);   // the space before it, once there is one
            if (used + cost > room) break;
            used += cost;
            fit++;
        }
        // Everything the run produced is on the line: no marker, no second pass.
        if (pass == 0 && (long long)fit >= s->calls) break;
        // With a value on the line the marker is already paid for; with none it
        // has to fit on its own.
        if (pass) marked = fit > 0 || budget >= IC_INSPECT_ELIDE_LEN;
    }

    // No room for the marker means no room for a partial reading either.
    if (!marked && (long long)fit < s->calls) fit = 0;

    for (int k = 0; k < fit; k++) {
        if (k) ic_ins_append(dst, cap, &len, " ");
        ic_ins_num(s->recent[k], vkind, vshift, num, (int)sizeof(num));
        if (!ic_ins_append(dst, cap, &len, num)) break;
    }
    if (marked) {
        if (fit) ic_ins_append(dst, cap, &len, " ");
        ic_ins_append(dst, cap, &len, IC_INSPECT_ELIDE);
    }

    // All or nothing: half a count ("(4", or a bare " (") reads as a value.
    if (max_chars - len >= tl) ic_ins_append(dst, cap, &len, tail);
    return len;
}

// True when `label` only restates the source text `span` -- `6` over the
// literal 6. Exact match, or a single padded real (`2.500`, `2.500f`) whose
// padding zeros are all that separates it from the literal `2.5` / `2.5f`. A
// label carrying a count or type note has a space in it and never matches:
// that part is new information.
static inline int ic_inspect_label_is_source(const char *span, const char *label) {
    if (!span || !label || !span[0]) return 0;
    if (s_strcmp(span, label) == 0) return 1;
    char tmp[S_FROM_NUMBER_MAX_CHARS + 8];
    int n = (int)s_strlen(label);
    if (n <= 0 || n >= (int)sizeof(tmp)) return 0;
    int dot = -1, suffix = 0;
    for (int i = 0; i < n; i++) {
        char c = label[i];
        if (c == ' ' || c == 'e' || c == 'E') return 0;
        if (c == '.') dot = i;
        tmp[i] = c;
    }
    if (dot < 0) return 0;
    if (tmp[n - 1] == 'f') suffix = 1;
    int end = n - suffix;                       // one past the last digit
    while (end - dot - 1 > 1 && tmp[end - 1] == '0') end--;
    if (suffix) tmp[end++] = 'f';
    tmp[end] = '\0';
    return s_strcmp(span, tmp) == 0;
}

#define IC_INSPECT_SPLIT_MAX_SPACES 256

// Where a long label breaks over `rows` (2 or 3) rows: the spaces separating
// values that make the widest row narrowest, the earlier of a tie. Never inside
// the `( )` note or just before it, and inside `[ ]` only when there are not
// enough spaces between whole arrays. Writes the rows - 1 spaces' indices to
// `cuts` (each row is the text between them) and returns the widest row's
// width, or -1.
static inline int ic_inspect_split(const char *label, int rows, int *cuts) {
    if (!label || rows < 2 || rows > 3) return -1;
    int n = (int)s_strlen(label);
    int sp[IC_INSPECT_SPLIT_MAX_SPACES];
    unsigned char in_brack[IC_INSPECT_SPLIT_MAX_SPACES];
    int k = 0, paren = 0, brack = 0;
    for (int i = 0; i < n && k < IC_INSPECT_SPLIT_MAX_SPACES; i++) {
        char c = label[i];
        if      (c == '(') paren++;
        else if (c == ')') paren--;
        else if (c == '[') brack++;
        else if (c == ']') brack--;
        else if (c == ' ' && paren == 0 && label[i + 1] != '(' && i > 0) {
            sp[k] = i;
            in_brack[k++] = brack > 0;
        }
    }
    for (int tier = 0; tier < 2; tier++) {
        int best_w = -1, best_a = -1, best_b = -1;
        for (int a = 0; a < k; a++) {
            if (!tier && in_brack[a]) continue;
            if (rows == 2) {
                int w = sp[a] > n - sp[a] - 1 ? sp[a] : n - sp[a] - 1;
                if (best_w < 0 || w < best_w) { best_w = w; best_a = a; }
                continue;
            }
            for (int b = a + 1; b < k; b++) {
                if (!tier && in_brack[b]) continue;
                int w = sp[a];
                if (sp[b] - sp[a] - 1 > w) w = sp[b] - sp[a] - 1;
                if (n - sp[b] - 1 > w)     w = n - sp[b] - 1;
                if (best_w < 0 || w < best_w) { best_w = w; best_a = a; best_b = b; }
            }
        }
        if (best_w >= 0) {
            cuts[0] = sp[best_a];
            if (rows == 3) cuts[1] = sp[best_b];
            return best_w;
        }
    }
    return -1;
}

static inline int ic_inspect_split2(const char *label) {
    int cut;
    return ic_inspect_split(label, 2, &cut) >= 0 ? cut : -1;
}

// Cut a label to `max_chars` cells, marking the cut so a truncated reading
// never passes for a complete one:
//
//     "0.00001 0.00002 0.00003 0.00004"  at 18  ->  "0.00001 0.00002..."
//
// Returns the new length. Below four cells not even the marker fits, so the
// label is emptied and the caller draws nothing.
static inline int ic_inspect_truncate(char *dst, int max_chars) {
    if (!dst) return 0;
    int len = (int)s_strlen(dst);
    if (max_chars >= len) return len;
    if (max_chars < 4) { dst[0] = '\0'; return 0; }
    int keep = max_chars - 3;
    dst[keep] = '.'; dst[keep + 1] = '.'; dst[keep + 2] = '.'; dst[keep + 3] = '\0';
    return max_chars;
}

#endif // IC_INSPECT_FMT_H
