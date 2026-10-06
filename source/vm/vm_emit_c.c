// vm_emit_c.c
// Walk the VM's IR tree and generate C source code.
//
// Pipeline:  IRNode tree  -->  vm_emit_c()  -->  char* (C source)
//
// Design notes:
//   - No libc; all allocation goes through vm->sys (malloc/realloc/free).
//   - Internal bookkeeping (name table, scratch) uses the VM's arena so it
//     does not need to be freed explicitly.
//   - The returned string is malloc'd; the caller frees it with sys->free.
//   - Expressions are always wrapped in () so operator precedence is never
//     ambiguous in the output regardless of nesting depth.
//   - Fixed arrays and slices become C struct typedefs so they can be passed
//     and returned by value:
//         int_3   = typedef struct { int   data[3]; } int_3;
//         int_slice = typedef struct { int*  data; int len; } int_slice;
//     Both index via .data[i]; slices also carry .len.
//   - Local variables are declared (and zero-initialised) at the top of each
//     function body, matching the VM's zeroed-frame semantics.  Any symbol the
//     body never reads gets a `(void)x;` right after the declarations so the
//     output stays clean under -Wall -Wextra.
//   - Template specialisations that share a name get type-mangled C names
//     (e.g. "sq" specialised for i32 and f64 becomes "sq_i32" / "sq_f64").

#include "vm_emit_c.h"
#include "vm_emit_c_dialect.h"
#include "vm_emit_shared.h"
#include "parser/tokens.h"
#include "common/string_pure.h"

#include <stdint.h>

// ============================================================================
// Small helpers (no libc)
// ============================================================================

static int ec_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int ec_is_array(VTKind k) {
    return k == VMT_ARR_I32 || k == VMT_ARR_F32 || k == VMT_ARR_F64 || k == VMT_ARR_I64;
}

static int ec_is_slice(VTKind k) {
    return k == VMT_SLICE_I32 || k == VMT_SLICE_F32 || k == VMT_SLICE_F64 || k == VMT_SLICE_I64;
}

static int ec_is_aggregate(VTKind k) { return ec_is_array(k) || ec_is_slice(k); }

// Get the scalar element kind for array or slice.
static VTKind ec_agg_elem(VTKind k) {
    if (k == VMT_ARR_I32 || k == VMT_SLICE_I32) return VMT_I32;
    if (k == VMT_ARR_F32 || k == VMT_SLICE_F32) return VMT_F32;
    if (k == VMT_ARR_F64 || k == VMT_SLICE_F64) return VMT_F64;
    if (k == VMT_ARR_I64 || k == VMT_SLICE_I64) return VMT_I64;
    return VMT_VOID;
}

static VTKind ec_arr_elem(VTKind k) { return ec_agg_elem(k); }

static const char *ec_scalar_type(VTKind k) {
    switch (k) {
        case VMT_I32: return "int";
        case VMT_I64: return "long long";
        case VMT_F32: return "float";
        case VMT_F64: return "double";
        default:     return "void";
    }
}

// Return C element-type name for an aggregate kind.
static const char *ec_elem_type(VTKind k) {
    return ec_scalar_type(ec_agg_elem(k));
}

// The C storage type of a packed array's elements. u8 and u16 have exact C
// counterparts, so they are declared natively and indexed directly -- the VM's
// packed layout is byte-identical to them. u1/u2/u4 have no C type and stay packed
// in int words, read and written with the same shift/mask the interpreter uses.
static int ec_pack_is_native(int pack_bits) {
    return pack_bits == 8 || pack_bits == 16;
}

static const char *ec_pack_type(int pack_bits) {
    if (pack_bits == 8)  return "unsigned char";
    if (pack_bits == 16) return "unsigned short";
    return "int";
}

// Identifier-safe spelling of an element type, for typedef and mangled names.
// Distinct from ec_elem_type because that returns C *type text*: using it to
// build a name produces "long long_3" for [3]i64 and "unsigned char_9" for
// [9]u8, neither of which is an identifier.
static const char *ec_elem_name(VMType t) {
    if (t.pack_bits == 1)  return "u1";
    if (t.pack_bits == 2)  return "u2";
    if (t.pack_bits == 4)  return "u4";
    if (t.pack_bits == 8)  return "u8";
    if (t.pack_bits == 16) return "u16";
    switch (ec_agg_elem(t.kind)) {
        case VMT_I32: return "int";
        case VMT_I64: return "i64";
        case VMT_F32: return "float";
        case VMT_F64: return "double";
        default:     return "void";
    }
}

// Number of int words backing a packed array of `n` elements.
static int ec_pack_words(int n, int bits) { return (n * bits + 31) / 32; }

// The u1/u2/u4 accessors. Emitted only when such a type is actually used, and
// include-guarded because callers concatenate several func_emit_c results into one
// translation unit.
static const char *EC_PACK_HELPERS =
"#ifndef VM_PACK_HELPERS\n"
"#define VM_PACK_HELPERS\n"
"static int vm_pack_get(const int *w, int i, int bits) {\n"
"    int epw = 32 / bits;\n"
"    return (int)(((unsigned)w[i / epw] >> ((i % epw) * bits)) & ((1u << bits) - 1u));\n"
"}\n"
"static void vm_pack_set(int *w, int i, int bits, int v) {\n"
"    int epw = 32 / bits, sh = (i % epw) * bits;\n"
"    unsigned m = (1u << bits) - 1u;\n"
"    w[i / epw] = (int)(((unsigned)w[i / epw] & ~(m << sh)) | (((unsigned)v & m) << sh));\n"
"}\n"
"#endif\n";

// The string-building helpers (IR_FMT). Textually equivalent to vm_fmt.h, which is
// the interpreter's copy -- change one, change the other; tests/test_strfmt.c
// compiles the emitted C and compares against the interpreter. Emitted per-kind so
// a script that only formats integers doesn't drag 64-bit division or double
// arithmetic into a small build, and include-guarded because several func_emit_c
// results get concatenated into one translation unit.
//
// Every function takes (dst, cap, at) and returns the new cursor, writing nothing
// at or past cap: an undersized buffer truncates, it never overflows.
static const char *EC_FMT_HEAD =
"#ifndef VM_FMT_HELPERS\n"
"#define VM_FMT_HELPERS\n";
static const char *EC_FMT_BYTES =
"static int vm_fmt_bytes(unsigned char *dst, int cap, int at, const unsigned char *src, int n) {\n"
"    if (at < 0) at = 0;\n"
"    for (int i = 0; i < n && at < cap; i++) dst[at++] = src[i];\n"
"    return at;\n"
"}\n";
static const char *EC_FMT_UDEC32 =
"static int vm_fmt_udec32(unsigned char *dst, int cap, int at, unsigned int v, int min_digits) {\n"
"    unsigned char tmp[20]; int n = 0;\n"
"    if (min_digits > 10) min_digits = 10;\n"
"    while (v) { tmp[n++] = (unsigned char)('0' + (int)(v % 10u)); v /= 10u; }\n"
"    while (n < min_digits) tmp[n++] = '0';\n"
"    if (n == 0) tmp[n++] = '0';\n"
"    while (n > 0 && at < cap) dst[at++] = tmp[--n];\n"
"    return at;\n"
"}\n";
static const char *EC_FMT_UDEC64 =
"static int vm_fmt_udec64(unsigned char *dst, int cap, int at, unsigned long long v, int min_digits) {\n"
"    unsigned char tmp[20]; int n = 0;\n"
"    if (min_digits > 19) min_digits = 19;\n"
"    while (v) { tmp[n++] = (unsigned char)('0' + (int)(v % 10ull)); v /= 10ull; }\n"
"    while (n < min_digits) tmp[n++] = '0';\n"
"    if (n == 0) tmp[n++] = '0';\n"
"    while (n > 0 && at < cap) dst[at++] = tmp[--n];\n"
"    return at;\n"
"}\n";
static const char *EC_FMT_I32 =
"static int vm_fmt_i32(unsigned char *dst, int cap, int at, int v) {\n"
"    unsigned int u;\n"
"    if (at < 0) at = 0;\n"
"    if (v < 0) { if (at < cap) dst[at++] = '-'; u = (unsigned int)(-(long long)v); }\n"
"    else u = (unsigned int)v;\n"
"    return vm_fmt_udec32(dst, cap, at, u, 0);\n"
"}\n";
static const char *EC_FMT_I64 =
"static int vm_fmt_i64(unsigned char *dst, int cap, int at, long long v) {\n"
"    unsigned long long u;\n"
"    if (at < 0) at = 0;\n"
"    if (v < 0) { if (at < cap) dst[at++] = '-'; u = (unsigned long long)(~(unsigned long long)v) + 1ull; }\n"
"    else u = (unsigned long long)v;\n"
"    return vm_fmt_udec64(dst, cap, at, u, 0);\n"
"}\n";
static const char *EC_FMT_FX =
"static int vm_fmt_fx(unsigned char *dst, int cap, int at, int raw, int shift, int decimals) {\n"
"    long long r, one, ip, fp, scale = 1, fr; int i;\n"
"    if (at < 0) at = 0;\n"
"    if (shift <= 0) return vm_fmt_i32(dst, cap, at, raw);\n"
"    if (shift > 31) shift = 31;\n"
"    if (decimals < 0) decimals = 0;\n"
"    if (decimals > 9) decimals = 9;\n"
"    r = raw;\n"
"    if (r < 0) { if (at < cap) dst[at++] = '-'; r = -r; }\n"
"    one = 1LL << shift; ip = r >> shift; fp = r & (one - 1);\n"
"    for (i = 0; i < decimals; i++) scale *= 10;\n"
"    fr = (fp * scale + (one >> 1)) >> shift;\n"
"    if (fr >= scale) { fr -= scale; ip += 1; }\n"
"    at = vm_fmt_udec32(dst, cap, at, (unsigned int)ip, 0);\n"
"    if (decimals > 0) {\n"
"        if (at < cap) dst[at++] = '.';\n"
"        at = vm_fmt_udec32(dst, cap, at, (unsigned int)fr, decimals);\n"
"    }\n"
"    return at;\n"
"}\n";
static const char *EC_FMT_F64 =
"static int vm_fmt_f64(unsigned char *dst, int cap, int at, double v, int decimals) {\n"
"    long long scale = 1; int i; unsigned long long ip, fr; double frac;\n"
"    if (at < 0) at = 0;\n"
"    if (decimals < 0) decimals = 0;\n"
"    if (decimals > 9) decimals = 9;\n"
"    if (!(v == v)) return vm_fmt_bytes(dst, cap, at, (const unsigned char *)\"nan\", 3);\n"
"    if (v < 0) { if (at < cap) dst[at++] = '-'; v = -v; }\n"
"    if (!(v < 1.8e19)) return vm_fmt_bytes(dst, cap, at, (const unsigned char *)\"big\", 3);\n"
"    for (i = 0; i < decimals; i++) scale *= 10;\n"
"    ip = (unsigned long long)v; frac = v - (double)ip;\n"
"    fr = (unsigned long long)(frac * (double)scale + 0.5);\n"
"    if (fr >= (unsigned long long)scale) { fr -= (unsigned long long)scale; ip += 1ull; }\n"
"    at = vm_fmt_udec64(dst, cap, at, ip, 0);\n"
"    if (decimals > 0) {\n"
"        if (at < cap) dst[at++] = '.';\n"
"        at = vm_fmt_udec64(dst, cap, at, fr, decimals);\n"
"    }\n"
"    return at;\n"
"}\n";

// Clamping for slice(x, start, len), matching the interpreter's IR_SLICE.
// Emitted only when some slice needs it -- a string build's own subslice is
// in range by construction and skips both calls.
static const char *EC_SLICE_HELPERS =
"#ifndef VM_SLICE_HELPERS\n"
"#define VM_SLICE_HELPERS\n"
"static int vm_slice_at(int n, int start) { return start < 0 ? 0 : (start > n ? n : start); }\n"
"static int vm_slice_n(int n, int start, int len) {\n"
"    int s = vm_slice_at(n, start);\n"
"    if (len < 0) len = 0;\n"
"    return len > n - s ? n - s : len;\n"
"}\n"
"#endif\n";

static int ec_needs_slice_helpers(Func **list, int count) {
    for (int fi = 0; fi < count; fi++)
        for (int ni = 0; ni < list[fi]->n_nodes; ni++)
            if (list[fi]->nodes[ni].op == IR_SLICE && list[fi]->nodes[ni].sub_op == 0) return 1;
    return 0;
}

// Bit-preserving reinterpretation (IR_BITCAST). A union is the portable
// spelling -- punning through one is defined in C99, unlike a pointer cast --
// and needs no libc. `static inline` so the three a given script never calls
// cost nothing and warn about nothing.
static const char *EC_BITCAST_HELPERS =
"#ifndef VM_BITCAST_HELPERS\n"
"#define VM_BITCAST_HELPERS\n"
"static inline float     vm_bc_i32_f32(int v)       { union { int i; float f; } u; u.i = v; return u.f; }\n"
"static inline int       vm_bc_f32_i32(float v)     { union { int i; float f; } u; u.f = v; return u.i; }\n"
"static inline double    vm_bc_i64_f64(long long v) { union { long long i; double d; } u; u.i = v; return u.d; }\n"
"static inline long long vm_bc_f64_i64(double v)    { union { long long i; double d; } u; u.d = v; return u.i; }\n"
"#endif\n";

// Only a cross-kind bitcast needs a helper: i32 <-> fxN shares its storage
// with the source and emits as the bare operand.
static int ec_needs_bitcast_helpers(Func **list, int count) {
    for (int fi = 0; fi < count; fi++)
        for (int ni = 0; ni < list[fi]->n_nodes; ni++) {
            IRNode *n = &list[fi]->nodes[ni];
            if (n->op == IR_BITCAST && n->sub_op != (int)n->type.kind) return 1;
        }
    return 0;
}

// Guarded division and remainder, exactly as eval_binop's OP_DIV / OP_MOD: a zero
// divisor gives 0, and -1 negates in unsigned space rather than trapping on
// INT_MIN / -1. A call rather than `(b ? a / b : 0)` so each operand is evaluated
// once -- `x / (j++)` and `x / f()` must not run the divisor twice. Each is guarded
// on its own: chunks concatenated into one TU may each need a different one.
enum { DH_DIV_I32, DH_MOD_I32, DH_DIV_I64, DH_MOD_I64, DH_DIV_F32, DH_DIV_F64, DH_COUNT };
static const char *EC_DIV_FN[DH_COUNT] = {
    "vm_div_i32", "vm_mod_i32", "vm_div_i64", "vm_mod_i64", "vm_div_f32", "vm_div_f64",
};
static const char *EC_DIV_DEF[DH_COUNT] = {
"#ifndef VM_DIV_I32\n#define VM_DIV_I32\nstatic inline int vm_div_i32(int a, int b) { return b ? (b == -1 ? (int)(0u - (unsigned int)a) : a / b) : 0; }\n#endif\n",
"#ifndef VM_MOD_I32\n#define VM_MOD_I32\nstatic inline int vm_mod_i32(int a, int b) { return (b && b != -1) ? a % b : 0; }\n#endif\n",
"#ifndef VM_DIV_I64\n#define VM_DIV_I64\nstatic inline long long vm_div_i64(long long a, long long b) { return b ? (b == -1 ? (long long)(0ull - (unsigned long long)a) : a / b) : 0; }\n#endif\n",
"#ifndef VM_MOD_I64\n#define VM_MOD_I64\nstatic inline long long vm_mod_i64(long long a, long long b) { return (b && b != -1) ? a % b : 0; }\n#endif\n",
"#ifndef VM_DIV_F32\n#define VM_DIV_F32\nstatic inline float vm_div_f32(float a, float b) { return b ? a / b : 0.0f; }\n#endif\n",
"#ifndef VM_DIV_F64\n#define VM_DIV_F64\nstatic inline double vm_div_f64(double a, double b) { return b ? a / b : 0.0; }\n#endif\n",
};

static void ec_emit_div_helpers(StrBuf *b, unsigned used) {
    for (int i = 0; i < DH_COUNT; i++) if (used & (1u << i)) sb_str(b, EC_DIV_DEF[i]);
}

// Infinity and NaN have no C literal, and MSVC rejects a constant 0.0 / 0.0, so a
// non-finite constant reads a bit pattern through a union -- no libc needed.
static const char *EC_NONFINITE_HELPERS =
"#ifndef VM_NONFINITE_HELPERS\n"
"#define VM_NONFINITE_HELPERS\n"
"static const union { unsigned long long u; double d; } vm_inf_bits = { 0x7ff0000000000000ull };\n"
"static const union { unsigned long long u; double d; } vm_nan_bits = { 0x7ff8000000000000ull };\n"
"#endif\n";

static int ec_needs_nonfinite_helpers(Func **list, int count) {
    for (int fi = 0; fi < count; fi++)
        for (int ni = 0; ni < list[fi]->n_nodes; ni++) {
            IRNode *n = &list[fi]->nodes[ni];
            if (n->op == IR_CONST_F && (n->kf != n->kf || n->kf - n->kf != 0.0)) return 1;
        }
    return 0;
}

// The helper for one (source kind -> destination kind) pair. Only the four
// same-width pairs vm.c builds reach here.
static const char *ec_bitcast_fn(int from, int to) {
    if (from == VMT_I32 && to == VMT_F32) return "vm_bc_i32_f32";
    if (from == VMT_F32 && to == VMT_I32) return "vm_bc_f32_i32";
    if (from == VMT_I64 && to == VMT_F64) return "vm_bc_i64_f64";
    return "vm_bc_f64_i64";
}

// True when any collected function prints AND the VM asked for print calls to
// be emitted. Only then does the output carry the VM_PRINT macro -- under
// VM_PRINT_EMIT_DROP (the default) an IR_PRINT emits nothing at all, so a
// script that prints costs an export target exactly zero bytes.
static int ec_needs_print_macro(VM *vm, Func **list, int count) {
    if (vm->run.print_emit != VM_PRINT_EMIT_CALL) return 0;
    for (int fi = 0; fi < count; fi++)
        for (int ni = 0; ni < list[fi]->n_nodes; ni++)
            if (list[fi]->nodes[ni].op == IR_PRINT) return 1;
    return 0;
}

// Defined, not just declared, so the emitted file compiles and runs standalone
// on a target that has never heard of print. A target that WANTS the output
// defines VM_PRINT before including this.
static const char *EC_PRINT_MACRO =
    "#ifndef VM_PRINT\n"
    "#define VM_PRINT(s, n) ((void)(s), (void)(n))\n"
    "#endif\n";

// Which IR_FMT kinds the EMITTED code reaches, so only the helpers actually called
// get defined. Bit i = FMT_* kind i.
//
// A walk from each body rather than a sweep of the node array, because a dropped
// print() takes its whole subtree out of the output -- and its subtree is usually a
// string build. Sweeping the array would define vm_fmt_i32 for a call that is no
// longer there.
static void ec_fmt_kinds_walk(VM *vm, Func *f, IRNode *n, unsigned *used) {
    if (!n) return;
    if (n->op == IR_PRINT && vm->run.print_emit != VM_PRINT_EMIT_CALL) return;
    if (n->op == IR_FMT) *used |= 1u << n->sub_op;
    if (n->a >= 0) ec_fmt_kinds_walk(vm, f, ir_child(f, n->a), used);
    if (n->b >= 0) ec_fmt_kinds_walk(vm, f, ir_child(f, n->b), used);
    if (n->c >= 0) ec_fmt_kinds_walk(vm, f, ir_child(f, n->c), used);
    if (n->items_begin >= 0)
        for (int i = 0; i < n->n_items; i++)
            ec_fmt_kinds_walk(vm, f, ir_item(f, n, i), used);
}

static unsigned ec_fmt_kinds_used(VM *vm, Func **list, int count) {
    unsigned used = 0;
    for (int fi = 0; fi < count; fi++) {
        Func *f = list[fi];
        if (f->body < 0) continue;
        ec_fmt_kinds_walk(vm, f, &f->nodes[f->body], &used);
    }
    return used;
}

// Return the binary-operator C token string for a sub_op code.
static const char *ec_binop_str(int op) {
    switch (op) {
        case OP_ADD:  return " + ";
        case OP_SUB:  return " - ";
        case OP_MUL:  return " * ";
        case OP_DIV:  return " / ";
        case OP_MOD:  return " % ";
        case OP_DIV_NATIVE: return " / ";
        case OP_MOD_NATIVE: return " % ";
        case OP_LT:   return " < ";
        case OP_LE:   return " <= ";
        case OP_GT:   return " > ";
        case OP_GE:   return " >= ";
        case OP_EQ:   return " == ";
        case OP_NE:   return " != ";
        case OP_AND:  return " && ";
        case OP_OR:   return " || ";
        case OP_BAND: return " & ";
        case OP_BOR:  return " | ";
        case OP_BXOR: return " ^ ";
        case OP_BSHL:        return " << ";
        case OP_BSHL_NATIVE: return " << ";
        case OP_BSHR:        return " >> ";
        default:      return " /*?*/ ";
    }
}

// ============================================================================
// Dynamic output buffer (uses common/strbuf.h)
// ============================================================================

typedef StrBuf Buf;
#define buf_init(b, vm)  sb_init(b, (vm)->run.sys)
#define buf_str  sb_str
#define buf_char sb_char
#define buf_int  sb_int

// Return 1 when the from_number string is a bare integer (no '.' or 'e/E'),
// meaning we need to add a decimal point to make it a valid float literal.
static int ec_is_int_str(const char *s) {
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '.' || c == 'e' || c == 'E') return 0;
    }
    return 1;
}

// Emit a C floating-point literal with the correct form/suffix:
//   VMT_F64 -> 1.0   1.5   1e10
//   VMT_F32 -> 1.0f  1.5f  1e10f   (the dialect's f32_suffix)
static void buf_float_lit(StrBuf *b, VM *vm, double v, VTKind kind, const char *f32_suffix) {
    char tmp[S_FROM_NUMBER_MAX_CHARS];
    union { double d; unsigned long long u; } bits;
    bits.d = v;
    // -0.0 == 0.0, so the number formatter prints it unsigned.
    if (v == 0.0 && (bits.u >> 63)) sb_str(b, "-");
    // SHORTEST: the emitted literal must be the value the VM ran, not %g's
    // 6 significant digits (which turns 110566002.1 into 110566000).
    s_from_number_flags(v, tmp, S_FROM_NUMBER_FLAG_SHORTEST);
    sb_str(b, tmp);
    if (ec_is_int_str(tmp)) sb_str(b, ".0");
    if (kind == VMT_F32)     sb_str(b, f32_suffix);
}

// A ref to one struct record: a plain pointer in C.
static int ec_is_ref_type(VMType t) {
    return t.struct_id && ec_is_slice(t.kind) && !t.inner_len;
}

// Emit the C type name for a VMType into Buf (scalar, fixed array, or slice).
// Placed after Buf definition so buf_* functions are visible. `owner` is the Func
// the type belongs to, whose struct table names a struct: `my` for a record, `my_8`
// for eight of them, `my_slice` for a slice of them and `my*` for a ref.
static void ec_emit_type_name(StrBuf *b, VM *vm, Func *owner, VMType t) {
    if (t.struct_id) {
        buf_str(b, emit_struct_name(vm, emit_struct_of(owner, t)));
        if (ec_is_slice(t.kind)) { buf_str(b, t.inner_len ? "_slice" : "*"); return; }
        if (t.inner_len) { buf_char(b, '_'); buf_int(b, (long long)(t.len / t.inner_len)); }
        return;
    }
    if (ec_is_array(t.kind)) {
        // int_N, float_N, double_N, u8_N
        buf_str(b, ec_elem_name(t));
        buf_char(b, '_');
        buf_int(b, (long long)t.len);
    } else if (ec_is_slice(t.kind)) {
        buf_str(b, ec_elem_name(t));
        buf_str(b, "_slice");
    } else {
        buf_str(b, ec_scalar_type(t.kind));
    }
}

// Emit the struct body of an aggregate typedef: the `<type> data[N];` or
// `<type>* data; int len;` that goes between "typedef struct { " and " }".
static void ec_emit_agg_body(StrBuf *b, VM *vm, Func *owner, VMType t) {
    if (t.struct_id) {
        buf_str(b, emit_struct_name(vm, emit_struct_of(owner, t)));
        if (ec_is_array(t.kind)) {
            buf_str(b, " data[");
            buf_int(b, (long long)(t.len / t.inner_len));
            buf_str(b, "]; } ");
        } else {
            buf_str(b, "* data; int len; } ");
        }
        return;
    }
    if (ec_is_array(t.kind)) {
        if (t.pack_bits && !ec_pack_is_native(t.pack_bits)) {
            // u1/u2/u4: N elements packed into ceil(N*bits/32) int words.
            buf_str(b, "int data[");
            buf_int(b, (long long)ec_pack_words(t.len, t.pack_bits));
            buf_str(b, "]; } ");
            return;
        }
        buf_str(b, t.pack_bits ? ec_pack_type(t.pack_bits) : ec_elem_type(t.kind));
        buf_str(b, " data[");
        buf_int(b, (long long)t.len);
        buf_str(b, "]; } ");
        return;
    }
    if (t.pack_bits && !ec_pack_is_native(t.pack_bits)) {
        buf_str(b, "int* data; int len; } ");
        return;
    }
    buf_str(b, t.pack_bits ? ec_pack_type(t.pack_bits) : ec_elem_type(t.kind));
    buf_str(b, "* data; int len; } ");
}

static void ec_emit_fmt_helpers(StrBuf *b, unsigned used) {
    if (!used) return;
    buf_str(b, EC_FMT_HEAD);
    // vm_fmt_f64's out-of-range markers go through vm_fmt_bytes, and
    // vm_fmt_fx falls back to vm_fmt_i32 at shift 0, so those pull their
    // dependencies in here rather than at the call sites.
    if (used & ((1u << FMT_BYTES) | (1u << FMT_F64)))            buf_str(b, EC_FMT_BYTES);
    if (used & ((1u << FMT_I32) | (1u << FMT_FX)))               buf_str(b, EC_FMT_UDEC32);
    if (used & ((1u << FMT_I64) | (1u << FMT_F64)))              buf_str(b, EC_FMT_UDEC64);
    if (used & ((1u << FMT_I32) | (1u << FMT_FX)))               buf_str(b, EC_FMT_I32);
    if (used & (1u << FMT_I64))                                  buf_str(b, EC_FMT_I64);
    if (used & (1u << FMT_FX))                                   buf_str(b, EC_FMT_FX);
    if (used & (1u << FMT_F64))                                  buf_str(b, EC_FMT_F64);
    buf_str(b, "#endif\n");
}

// ============================================================================
// Aggregate type collection
// ============================================================================

#define MAX_AGG_TYPES 64

// `owner` is the Func whose struct table the type's struct_id indexes.
typedef struct { VMType type; Func *owner; } AggType;

// Return 1 if two aggregate types are identical (same kind + len + packing).
// pack_bits has to be part of this: [8]u4 and [8]i32 are both VMT_ARR_I32 with
// len 8 but need different typedefs. Two struct types are identical when they name
// the same struct -- by name, since the ids index different units' tables.
//
// A SLICE's len is compile-time knowledge the caller happened to have, not part
// of its C shape: every []i32 is the same `{int* data; int len;}` typedef under
// the same name whatever its len. Comparing it emitted `int_slice` twice -- a
// typedef redefinition, which is an error even in C11 -- for the ordinary case
// of one untyped function called with arrays of two different lengths, since
// each length gets its own specialisation (find_or_specialize_body).
// Const is NOT compared: it decides what the DSL may write through a view, not
// what the view looks like in C, and both spellings emit the same typedef under
// the same name. Comparing it would emit that typedef twice -- the same
// redefinition the slice `len` used to cause.
static int ec_agg_type_eq(VM *vm, AggType a, VMType b, Func *b_owner) {
    if (a.type.kind != b.kind || a.type.pack_bits != b.pack_bits) return 0;
    if (!ec_is_slice(b.kind) && a.type.len != b.len) return 0;
    if (!a.type.struct_id != !b.struct_id) return 0;
    if (!b.struct_id) return 1;
    if (a.type.inner_len != b.inner_len) return 0;
    return es_streq(emit_struct_name(vm, emit_struct_of(a.owner, a.type)),
                    emit_struct_name(vm, emit_struct_of(b_owner, b)));
}

static void ec_agg_add(VM *vm, AggType *agg, int *n, VMType t, Func *owner) {
    for (int j = 0; j < *n; j++) if (ec_agg_type_eq(vm, agg[j], t, owner)) return;
    if (*n < MAX_AGG_TYPES) { agg[*n].type = t; agg[*n].owner = owner; (*n)++; }
}

// Every aggregate a struct's layout spells, nested structs included: an array field
// is declared with its wrapper typedef (`float_4 buf;`, layout-identical to a bare
// `float buf[4]`), so that typedef is needed whether or not the body names the field.
static void ec_agg_add_struct_fields(VM *vm, AggType *agg, int *n, Func *owner, int struct_id, int depth) {
    const VMStruct *st = vm_struct_get(owner->structs, struct_id);
    if (!st || depth > 16) return;
    for (int i = 0; i < st->n_fields; i++) {
        VMType ft = st->fields[i].type;
        if (st->fields[i].width != FIELD_AGG) continue;
        ec_agg_add(vm, agg, n, ft, owner);
        if (ft.struct_id) ec_agg_add_struct_fields(vm, agg, n, owner, ft.struct_id, depth + 1);
    }
}

// True when any collected type needs the u1/u2/u4 shift/mask accessors.
static int ec_needs_pack_helpers(AggType *agg, int n) {
    for (int i = 0; i < n; i++)
        if (agg[i].type.pack_bits && !ec_pack_is_native(agg[i].type.pack_bits)) return 1;
    return 0;
}

// Scan all collected functions and build a deduplicated list of every
// distinct aggregate type (fixed array or slice) used in parameters,
// return types, or local variables.
// Returns the number of distinct types found.  Writes into arena.
static int collect_agg_types(VM *vm, Func **list, int count,
                              AggType **agg_out) {
    int n = 0;
    AggType *agg = (AggType *)mem_alloc(&vm->run.mem,
                                          sizeof(AggType) * MAX_AGG_TYPES);

    for (int fi = 0; fi < count; fi++) {
        Func *f = list[fi];

        // Return type
        if (ec_is_aggregate(f->ret_type.kind)) ec_agg_add(vm, agg, &n, f->ret_type, f);

        // All symbols (params + locals)
        for (int si = 0; si < f->n_syms; si++) {
            VMSym *s = &f->syms[si];
            if (!ec_is_aggregate(s->type.kind)) continue;
            // A host buffer is slice-TYPED in the VM but emits as a plain C
            // array, so it needs no struct -- counting it here would emit an
            // unused typedef. A slice()
            // OF one still lands in a local of its own and is picked up there.
            if (s->is_host_buf) continue;
            ec_agg_add(vm, agg, &n, s->type, f);
        }

        // Every IR node too: an aggregate can appear only as a temporary, with
        // no sym and no return type carrying it -- e.g. the array literal in
        // `f([x, y, z])` emits `(int_3){{x, y, z}}` at the call site, so int_3
        // needs a typedef even though nothing is declared with that type.
        for (int ni = 0; ni < f->n_nodes; ni++) {
            VMType t = f->nodes[ni].type;
            if (!ec_is_aggregate(t.kind)) continue;
            // Same for the node that READS a host buffer: it emits as the bare
            // pointer, never as a struct value.
            if (f->nodes[ni].op == IR_LOCAL
                && f->syms[(int)f->nodes[ni].ki].is_host_buf) continue;
            ec_agg_add(vm, agg, &n, t, f);
        }
    }
    for (int i = 0; i < n; i++)
        if (agg[i].type.struct_id)
            ec_agg_add_struct_fields(vm, agg, &n, agg[i].owner, agg[i].type.struct_id, 0);
    *agg_out = agg;
    return n;
}

// Emit every typedef the collected aggregates need, in an order C accepts: the
// plain array and slice wrappers first (a struct field may be one), then each
// struct after the structs it contains, then the wrappers around structs. Each is
// include-guarded when `guarded` is set -- callers concatenating several chunks
// into one translation unit -- and struct typedefs always are, since a struct can
// reach a chunk through more than one path.
static void ec_emit_one_agg_typedef(StrBuf *b, VM *vm, AggType a, int guarded) {
    if (guarded) {
        buf_str(b, "#ifndef VM_CTYPE_");
        ec_emit_type_name(b, vm, a.owner, a.type);
        buf_str(b, "\n#define VM_CTYPE_");
        ec_emit_type_name(b, vm, a.owner, a.type);
        buf_char(b, '\n');
    }
    buf_str(b, "typedef struct { ");
    ec_emit_agg_body(b, vm, a.owner, a.type);
    ec_emit_type_name(b, vm, a.owner, a.type);
    buf_str(b, guarded ? ";\n#endif\n" : ";\n");
}

static void ec_put_name(StrBuf *b, const char *name);

// One field of a struct typedef: `int x;`, `unsigned char a;`, `float_4 buf;`,
// `inner in;`, `inner_4 items;`.
static void ec_emit_struct_field(StrBuf *b, VM *vm, Func *owner, const VMStructField *fd) {
    buf_str(b, "    ");
    if (fd->width == FIELD_U8)       buf_str(b, "unsigned char");
    else if (fd->width == FIELD_U16) buf_str(b, "unsigned short");
    else if (fd->width == FIELD_AGG) ec_emit_type_name(b, vm, owner, fd->type);
    else                             buf_str(b, ec_scalar_type(fd->type.kind));
    buf_char(b, ' ');
    ec_put_name(b, intern_get_cstr(vm->intern, fd->name));
    buf_str(b, ";\n");
}

// The wrapper around an array or slice of records, once per chunk: a struct field
// can need it ahead of the struct holding it, and a local after.
static void ec_emit_struct_wrapper(StrBuf *b, VM *vm, Func *owner, VMType t,
                                   const char **done, int *n_done) {
    StrBuf nb;
    sb_init(&nb, vm->run.sys);
    ec_emit_type_name(&nb, vm, owner, t);
    const char *nm = nb.ok && nb.buf ? nb.buf : "";
    for (int i = 0; i < *n_done; i++)
        if (es_streq(done[i], nm)) { vm->run.sys->free(nb.buf); return; }
    int len = es_strlen(nm);
    char *keep = (char *)mem_alloc(&vm->run.mem, (size_t)len + 1);
    if (keep && *n_done < 2 * MAX_AGG_TYPES) {
        for (int i = 0; i <= len; i++) keep[i] = nm[i];
        done[(*n_done)++] = keep;
    }
    vm->run.sys->free(nb.buf);
    AggType a; a.type = t; a.owner = owner;
    ec_emit_one_agg_typedef(b, vm, a, 1);
}

static void ec_emit_struct_typedef(StrBuf *b, VM *vm, Func *owner, int struct_id,
                                   const char **done, int *n_done, int depth) {
    const VMStruct *st = vm_struct_get(owner->structs, struct_id);
    if (!st || depth > 16) return;
    const char *sn = emit_struct_name(vm, st);
    for (int i = 0; i < *n_done; i++) if (es_streq(done[i], sn)) return;
    for (int i = 0; i < st->n_fields; i++) {
        VMType ft = st->fields[i].type;
        if (st->fields[i].width != FIELD_AGG || !ft.struct_id) continue;
        ec_emit_struct_typedef(b, vm, owner, ft.struct_id, done, n_done, depth + 1);
        if (ft.inner_len) ec_emit_struct_wrapper(b, vm, owner, ft, done, n_done);
    }
    if (*n_done < 2 * MAX_AGG_TYPES) done[(*n_done)++] = sn;
    buf_str(b, "#ifndef VM_CTYPE_");
    buf_str(b, sn);
    buf_str(b, "\n#define VM_CTYPE_");
    buf_str(b, sn);
    buf_str(b, "\ntypedef struct ");
    buf_str(b, sn);
    buf_str(b, " {\n");
    for (int i = 0; i < st->n_fields; i++) ec_emit_struct_field(b, vm, owner, &st->fields[i]);
    buf_str(b, "} ");
    buf_str(b, sn);
    buf_str(b, ";\n#endif\n");
}

static void ec_emit_typedefs(StrBuf *b, VM *vm, AggType *agg, int n, int guarded) {
    for (int i = 0; i < n; i++) {
        // A plain wrapper; refs and records have none, being a pointer and the
        // struct itself.
        if (agg[i].type.struct_id) continue;
        ec_emit_one_agg_typedef(b, vm, agg[i], guarded);
    }
    const char *done[2 * MAX_AGG_TYPES];
    int n_done = 0;
    for (int i = 0; i < n; i++) {
        if (!agg[i].type.struct_id) continue;
        ec_emit_struct_typedef(b, vm, agg[i].owner, agg[i].type.struct_id, done, &n_done, 0);
    }
    for (int i = 0; i < n; i++) {
        VMType t = agg[i].type;
        if (!t.struct_id || !t.inner_len) continue;
        ec_emit_struct_wrapper(b, vm, agg[i].owner, t, done, &n_done);
    }

}



// Function names come from emit_build_cnames (vm_emit_shared.h) -- pure
// VMType -> identifier, so it is shared with the script backends. C needs no
// keyword escape: the DSL and C have near-identical keyword sets.
// C and C++ keywords, and names a host's headers already mean something by.
static const char *const EC_RESERVED[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool",
    "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
    "alignas", "alignof", "bool", "constexpr", "false", "nullptr", "static_assert",
    "thread_local", "true", "typeof", "typeof_unqual",
    "and", "and_eq", "asm", "bitand", "bitor", "catch", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const_cast", "consteval", "constinit", "co_await",
    "co_return", "co_yield", "decltype", "delete", "dynamic_cast", "explicit", "export",
    "friend", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "operator", "or",
    "or_eq", "private", "protected", "public", "reinterpret_cast", "requires", "static_cast",
    "template", "this", "throw", "try", "typeid", "typename", "using", "virtual", "wchar_t",
    "xor", "xor_eq",
    "main", "NULL", "EOF", "errno", "INFINITY", "NAN", "assert", "offsetof",
    0
};

// Whether a script name would mean something else in the emitted C: a keyword, or
// a name in the emitter's own namespaces -- hidden temps (__v<n>) and every other
// double-underscore name, the vm_ / VM_ helpers, math_pure's m_*, the fx natives
// (fx16_pow) and the aggregate typedefs (int_3, u8_slice). Such a name is emitted
// with '_' appended.
static int ec_name_clashes(const char *s) {
    if (es_is_reserved(EC_RESERVED, s)) return 1;
    if (s[0] == '_' && s[1] == '_') return 1;
    if ((s[0] == 'v' || s[0] == 'V') && (s[1] == 'm' || s[1] == 'M') && s[2] == '_') return 1;
    if (s[0] == 'm' && s[1] == '_') return 1;
    if (s[0] == 'f' && s[1] == 'x' && s[2] >= '0' && s[2] <= '9') return 1;
    int n = es_strlen(s), us = -1;
    for (int i = 0; i < n; i++) if (s[i] == '_') us = i;
    if (us <= 0 || us == n - 1) return 0;
    const char *tail = s + us + 1;
    int tail_ok = es_streq(tail, "slice");
    if (!tail_ok) { tail_ok = 1; for (const char *t = tail; *t; t++) if (*t < '0' || *t > '9') tail_ok = 0; }
    if (!tail_ok) return 0;
    static const char *const TYPES[] = { "int", "float", "double", "i64", "char", 0 };
    for (int i = 0; TYPES[i]; i++) {
        int k = 0;
        while (k < us && TYPES[i][k] == s[k]) k++;
        if (k == us && TYPES[i][k] == '\0') return 1;
    }
    if (s[0] != 'u' || us < 2) return 0;
    for (int i = 1; i < us; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

// A script name as the emitted C spells it (ec_name_clashes).
static void ec_put_name(StrBuf *b, const char *name) {
    sb_str(b, name);
    if (ec_name_clashes(name)) sb_char(b, '_');
}

static void build_cnames(VM *vm, Func **list, int count, const char **names_out) {
    emit_build_cnames(vm, list, count, names_out, 0);
    // The nameless __script__ and its lambdas keep the emitter's own spelling.
    for (int i = 0; i < count; i++) {
        if (list[i]->name == 0 || !ec_name_clashes(names_out[i])) continue;
        int n = es_strlen(names_out[i]);
        char *out = (char *)mem_alloc(&vm->run.mem, (size_t)n + 2);
        if (!out) continue;
        for (int k = 0; k < n; k++) out[k] = names_out[i][k];
        out[n] = '_';
        out[n + 1] = '\0';
        names_out[i] = out;
    }
}

// ============================================================================
// Emitter state
// ============================================================================

struct ECEmit {
    Buf         out;
    VM         *vm;
    Func       *cur_func;       // function currently being emitted
    int         indent;         // current indentation level (4 spaces each)
    Func      **func_list;      // collected function pointers
    const char **func_cnames;   // C names parallel to func_list
    int         func_count;
    int         strip_unused;   // 1: omit never-referenced params from the signature (see func_emit_c_named_stripped)
    // The one top-level statement whose assignment also DECLARES its target,
    // the prologue having skipped it. See emit_plan_decls.
    IRNode     *decl_stmt;
    const ECDialect *d;         // the target; &EC_DIALECT_C for C itself
    int         raw_bool;       // bool_is_type: the next boolean node is a condition, emit it bare
    int         n_guards;       // loop_guard counters named so far in this output
};
typedef struct ECEmit Emit;

// C: the walker's own behaviour, hooks all NULL.
static const ECDialect EC_DIALECT_C = {
    .name       = "c",
    .void_casts = 1,
    .f32_suffix = "f",
    .u32_type   = "unsigned int",
    .void_expr  = "(void)",
    .c_helpers  = 1,
};

static void ec_emit_init(Emit *e, VM *vm, const ECDialect *d) {
    e->vm           = vm;
    e->cur_func     = 0;
    e->indent       = 0;
    e->func_list    = 0;
    e->func_cnames  = 0;
    e->func_count   = 0;
    e->strip_unused = 0;
    e->decl_stmt    = 0;
    e->d            = d;
    e->raw_bool     = 0;
    e->n_guards     = 0;
    buf_init(&e->out, vm);
}

// A type's name in the target: the dialect's, or C's typedef name.
static void ec_type(Emit *e, Func *owner, VMType t) {
    if (e->d->type_name) e->d->type_name(e, owner, t);
    else                 ec_emit_type_name(&e->out, e->vm, owner, t);
}

static void emit_indent(Emit *e) {
    for (int i = 0; i < e->indent * 4; i++) buf_char(&e->out, ' ');
}

// Return the pre-computed C name for a Func* (linear scan; table is small).
static const char *ec_get_cname(Emit *e, Func *f) {
    for (int i = 0; i < e->func_count; i++)
        if (e->func_list[i] == f) return e->func_cnames[i];
    // Native function called from within a user function -- use the
    // appropriate variant name from the cfunc_table (resolved at call site).
    return "?";
}

// Which host buffer a sym names, or -1 when it is not one. The id is recovered from
// the sym's own offset, which addresses VmRun.host_bufs one fixed-size slot per id
// (see sym_bind_host_buf), so this needs no name lookup and still works on a
// deserialized func where the offset is all that survived.
static int ec_host_buf_id(Emit *e, int slot) {
    VMSym *s = &e->cur_func->syms[slot];
    if (!s->is_host_buf) return -1;
    int id = s->offset / (int)sizeof(VMHostBufSlot);
    if (id < 0 || id >= VM_MAX_HOST_BUFS) return -1;
    return e->vm->host_bufs[id].declared ? id : -1;
}

static const char *ec_host_buf_c_name(Emit *e, int slot) {
    int id = ec_host_buf_id(e, slot);
    return id < 0 ? 0 : e->vm->host_bufs[id].c_name;
}

// What `.len` emits for a host buffer. NULL c_len_expr means the documented
// default, "<c_name>_len", which the caller assembles.
static const char *ec_host_buf_len_expr(Emit *e, int slot) {
    int id = ec_host_buf_id(e, slot);
    return id < 0 ? 0 : e->vm->host_bufs[id].c_len_expr;
}

// Emit the name of a local variable (slot index into cur_func->syms).
//
// A shared variable is not a local at all: its storage is file-scope (see
// vm_emit_c_globals), so it emits under the accessor prefix the host set. Everything
// else about the expression is unchanged, which is why shared variables cost the
// emitter one branch here and one skip in the declaration prologue.
static void emit_local_name(Emit *e, int slot) {
    VMSym *s = &e->cur_func->syms[slot];
    if (s->is_global && s->name != 0) {
        buf_str(&e->out, e->vm->globals_c_prefix);
        ec_put_name(&e->out, intern_get_cstr(e->vm->intern, s->name));
        return;
    }
    // A host buffer emits under the C name the host registered, because the
    // identifier the target declares is the host's to choose. Falls back to the
    // script name if this VM has no entry for the id, which can only happen for a
    // func deserialized into a VM that never declared the buffer.
    if (s->is_host_buf) {
        const char *cn = ec_host_buf_c_name(e, slot);
        buf_str(&e->out, cn ? cn : intern_get_cstr(e->vm->intern, s->name));
        return;
    }
    if (e->d->sym_name) {
        e->d->sym_name(e, e->cur_func, slot);
    } else if (s->name != 0) {
        ec_put_name(&e->out, intern_get_cstr(e->vm->intern, s->name));
        if (s->name_dup) { buf_str(&e->out, "__"); buf_int(&e->out, (long long)slot); }
    } else {
        buf_str(&e->out, "__v");
        buf_int(&e->out, (long long)slot);
    }
}

// ============================================================================
// Expression emitter
// ============================================================================

// Forward declarations.
static void emit_expr(Emit *e, IRNode *n);
static void emit_stmt_as_expr(Emit *e, IRNode *n);
static void emit_cond(Emit *e, IRNode *n);
static void emit_cond_paren(Emit *e, IRNode *n);

// True when the node emits as a top-level multiply or shift. GCC's
// -Wint-in-bool-context flags those in a boolean position ("'*' in boolean
// context, suggest '&&' instead") even when the arithmetic is exactly what was
// meant -- and parenthesising doesn't silence it, only an explicit `!= 0` does.
// OP_BSHL counts because it emits as `u * (1u << n)`, not as a shift.
static int ec_needs_bool_coerce(IRNode *n) {
    if (n->op != IR_BINOP) return 0;
    switch (n->sub_op) {
        case OP_MUL:
        case OP_BSHL: case OP_BSHL_NATIVE:
        case OP_BSHR:
            return 1;
        default:
            return 0;
    }
}

// True when emit_expr already wraps this node's output in one outer pair of
// parentheses, so a caller that has to parenthesise a condition anyway can just
// let the node supply them. Doubling them is what makes clang warn on
// `if ((a == b))` ("equality comparison with extraneous parentheses").
static int ec_div_helper(Func *f, int flags, IRNode *n);

// A guarded divide that emits as a vm_div_* call is the one binop that doesn't.
static int ec_self_parenthesised(Emit *e, IRNode *n) {
    switch (n->op) {
        case IR_BINOP:
            return !(e->d->c_helpers && ec_div_helper(e->cur_func, emit_func_flags(e->vm, e->cur_func), n) >= 0);
        case IR_UNOP: case IR_SELECT: return 1;
        default: return 0;
    }
}

// The value VM_FLAG_CHECK_DIV_ZERO substitutes when the divisor turns out to be
// zero, or NULL when this op/type pair carries no guard at all.
static const char *ec_divzero_result(Emit *e, IRNode *n) {
    int div = (n->sub_op == OP_DIV), mod = (n->sub_op == OP_MOD);
    switch (n->type.kind) {
        case VMT_I32: return (div || mod) ? "0"    : 0;
        case VMT_I64: return (div || mod) ? "0LL"  : 0;
        case VMT_F32: return div          ? (e->d->f32_suffix[0] ? "0.0f" : "0.0") : 0;
        case VMT_F64: return div          ? "0.0"  : 0;
        default:      return 0;
    }
}

// A non-finite constant (EC_NONFINITE_HELPERS). The shader dialects have no union
// block and read 1.0 / 0.0 as infinity themselves.
static void ec_emit_nonfinite(Emit *e, double v, VTKind kind) {
    int f32 = (kind == VMT_F32);
    if (!e->d->c_helpers) {
        buf_str(&e->out, v != v ? "(0.0" : v > 0 ? "(1.0" : "(-1.0");
        if (f32) buf_str(&e->out, e->d->f32_suffix);
        buf_str(&e->out, " / 0.0");
        if (f32) buf_str(&e->out, e->d->f32_suffix);
        buf_char(&e->out, ')');
        return;
    }
    if (f32) buf_str(&e->out, "(float)");
    buf_str(&e->out, v != v ? "vm_nan_bits.d" : v > 0 ? "vm_inf_bits.d" : "(-vm_inf_bits.d)");
}

// Look past an IR_INSPECT wrapper to the node that actually produces the value.
//
// IR_INSPECT emits as its operand alone, so anything that INSPECTS a node -- rather
// than emitting it -- has to see the operand or the two disagree: `x / 2` emits a
// bare divide because the divisor is a literal, and `x / __ins(2, 0)` has to as
// well, or inspecting an expression changes it.
//
// Apply this wherever a child node is fetched to be examined. (Fetching one to EMIT
// it needs nothing: emit_expr has its own IR_INSPECT case.)
static IRNode *ec_thru_inspect(Func *f, IRNode *n) {
    while (n && n->op == IR_INSPECT && n->a >= 0) n = ir_child(f, n->a);
    return n;
}

// A checked shift's count, mod the width as eval_binop takes it: a literal is
// masked here, anything else in the output.
static void ec_emit_shift_count(Emit *e, IRNode *cnt, int mask) {
    IRNode *c = ec_thru_inspect(e->cur_func, cnt);
    if (c && c->op == IR_CONST_I) { buf_int(&e->out, c->ki & mask); return; }
    buf_char(&e->out, '(');
    emit_expr(e, cnt);
    buf_str(&e->out, " & ");
    buf_int(&e->out, mask);
    buf_char(&e->out, ')');
}

// Classify a divisor the C compiler can see through: 1 = literally non-zero,
// -1 = literally zero, 0 = unknown. A literal divisor makes the div-by-zero
// guard dead code, and emitting it anyway is what produces the float-literal-
// as-condition warning (-Wliteral-conversion) on `((4194304.0f) ? ... : 0.0f)`.
static int ec_const_divisor(IRNode *n) {
    if (!n) return 0;
    if (n->op == IR_CONST_I) return n->ki != 0 ? 1 : -1;
    if (n->op == IR_CONST_F) return n->kf != 0.0 ? 1 : -1;
    return 0;
}

// The EC_DIV_FN helper a guarded / or % calls, or -1 when it emits inline:
// unguarded, a literal divisor (other than an integer -1, which traps on INT_MIN),
// or a remainder of floats.
static int ec_div_helper(Func *f, int flags, IRNode *n) {
    if (n->op != IR_BINOP || (n->sub_op != OP_DIV && n->sub_op != OP_MOD)) return -1;
    if (!(flags & VM_FLAG_CHECK_DIV_ZERO)) return -1;
    IRNode *d = ec_thru_inspect(f, ir_child(f, n->b));
    if (ec_const_divisor(d) && !(d->op == IR_CONST_I && d->ki == -1)) return -1;
    int mod = (n->sub_op == OP_MOD);
    switch (n->type.kind) {
        case VMT_I32: return mod ? DH_MOD_I32 : DH_DIV_I32;
        case VMT_I64: return mod ? DH_MOD_I64 : DH_DIV_I64;
        case VMT_F32: return mod ? -1 : DH_DIV_F32;
        case VMT_F64: return mod ? -1 : DH_DIV_F64;
        default:      return -1;
    }
}

// A float converted to an integer saturates, nan giving 0, as the VM does it
// (cvt_to) -- unless the body asked for C's own cast (VM_FLAG_C_FLOAT_TO_INT).
// 0 for the i32 helper, 1 for i64, -1 when the cast is emitted as it stands.
static int ec_f2i_kind(Func *f, int flags, IRNode *n) {
    if (n->op != IR_CVT || (flags & VM_FLAG_C_FLOAT_TO_INT)) return -1;
    IRNode *a = ir_child(f, n->a);
    if (!a || (a->type.kind != VMT_F32 && a->type.kind != VMT_F64)) return -1;
    return n->type.kind == VMT_I32 ? 0 : n->type.kind == VMT_I64 ? 1 : -1;
}

static const char *EC_F2I_DEF[2] = {
"#ifndef VM_F2I\n#define VM_F2I\nstatic inline int vm_f2i(double v) { return v != v ? 0 : v >= 2147483647.0 ? 2147483647 : v <= -2147483648.0 ? (-2147483647 - 1) : (int)v; }\n#endif\n",
"#ifndef VM_F2L\n#define VM_F2L\nstatic inline long long vm_f2l(double v) { return v != v ? 0 : v >= 9223372036854775807.0 ? 9223372036854775807LL : v <= -9223372036854775807.0 ? (-9223372036854775807LL - 1) : (long long)v; }\n#endif\n",
};

static void ec_emit_f2i_helpers(VM *vm, StrBuf *b, Func **list, int count) {
    unsigned used = 0;
    for (int fi = 0; fi < count; fi++) {
        int flags = emit_func_flags(vm, list[fi]);
        for (int ni = 0; ni < list[fi]->n_nodes; ni++) {
            int k = ec_f2i_kind(list[fi], flags, &list[fi]->nodes[ni]);
            if (k >= 0) used |= 1u << k;
        }
    }
    for (int k = 0; k < 2; k++) if (used & (1u << k)) sb_str(b, EC_F2I_DEF[k]);
}

static unsigned ec_div_helpers_used(VM *vm, Func **list, int count) {
    unsigned used = 0;
    for (int fi = 0; fi < count; fi++) {
        int flags = emit_func_flags(vm, list[fi]);
        for (int ni = 0; ni < list[fi]->n_nodes; ni++) {
            int h = ec_div_helper(list[fi], flags, &list[fi]->nodes[ni]);
            if (h >= 0) used |= 1u << h;
        }
    }
    return used;
}

// Emit the raw C array literal backing an IR_DATA_SLICE node: (int[]){65, 66}.
// The elements live in VM-arena storage pointed to by ki (see
// compile_string_literal in vm.c), not as child IR nodes. An empty slice emits
// (int[]){0} -- a zero-length brace-init is not valid C -- and is paired with a
// len of 0 by the caller, so the dummy element is never read.
static void emit_dataslice_array(Emit *e, IRNode *n) {
    // A byte-materialised literal (see dataslice_to_bytes in vm.c) backs a
    // `const unsigned char*` native slot, so its storage is a byte array.
    int bytes = (n->type.pack_bits == 8);
    const int *data = bytes ? 0 : (const int *)(intptr_t)n->ki;
    const unsigned char *bdata = bytes ? (const unsigned char *)(intptr_t)n->ki : 0;
    buf_str(&e->out, bytes ? "(unsigned char[]){" : "(int[]){");
    if (n->n_items <= 0) {
        buf_char(&e->out, '0');
    } else {
        for (int i = 0; i < n->n_items; i++) {
            if (i) buf_str(&e->out, ", ");
            buf_int(&e->out, (long long)(bytes ? (int)bdata[i] : data[i]));
        }
    }
    buf_char(&e->out, '}');
}

// Emit a slice argument as a "ptr, len" pair for a register_c_func_sig call.
// An IR_DATA_SLICE (string literal) emits its inline int[] + count directly; a
// slice-typed expression emits `.data, .len`; a fixed array emits `.data, N`.
static void emit_slice_ptr_len(Emit *e, IRNode *arg) {
    if (arg->op == IR_DATA_SLICE) {
        emit_dataslice_array(e, arg);
        buf_str(&e->out, ", ");
        buf_int(&e->out, (long long)arg->n_items);
    } else if (ec_is_array(arg->type.kind)) {
        emit_expr(e, arg);
        buf_str(&e->out, ".data, ");
        buf_int(&e->out, (long long)arg->type.len);
    } else { // slice
        emit_expr(e, arg);
        buf_str(&e->out, ".data, ");
        emit_expr(e, arg);
        buf_str(&e->out, ".len");
    }
}

// The base of an IR_SLICE, ready for a `.data` / `.len` field access.
static void emit_slice_base(Emit *e, IRNode *base) {
    if (base->op == IR_LOCAL) { emit_local_name(e, (int)base->ki); return; }
    buf_char(&e->out, '(');
    emit_expr(e, base);
    buf_char(&e->out, ')');
}

// The LENGTH of an aggregate-valued node: a constant for a fixed array, a `.len`
// read for a slice. A host buffer has no struct to read it from, so the host
// supplies the C expression at declaration time -- see vm_declare_host_buffer's
// c_len_expr.
static void emit_agg_len(Emit *e, IRNode *base) {
    if (base->op == IR_LOCAL) {
        int slot = (int)base->ki;
        VMSym *s = &e->cur_func->syms[slot];
        if (s->is_host_buf) {
            const char *lx = ec_host_buf_len_expr(e, slot);
            if (lx) { buf_str(&e->out, lx); return; }
            // No expression given: the documented default is "<c_name>_len",
            // an int the target declares beside the buffer itself.
            const char *cn = ec_host_buf_c_name(e, slot);
            buf_str(&e->out, cn ? cn : intern_get_cstr(e->vm->intern, s->name));
            buf_str(&e->out, "_len");
            return;
        }
    }
    emit_slice_base(e, base);
    buf_str(&e->out, ".len");
}

static void emit_slice_base_len(Emit *e, IRNode *base) {
    if (ec_is_array(base->type.kind)) { buf_int(&e->out, (long long)base->type.len); return; }
    if (base->op == IR_DATA_SLICE)    { buf_int(&e->out, (long long)base->n_items);  return; }
    emit_agg_len(e, base);
}

// A slice argument whose value has to be *computed* -- today, a string build (an
// IR_COMMA of writes into a hidden buffer, ending in a read of the local holding the
// finished slice). The (ptr, len) split below names its argument twice and C leaves
// argument evaluation order unspecified, so such a value must be sequenced ahead of
// the call. Returns the final read to use in the argument slot, or 0 when the
// argument needs no prologue.
static IRNode *ec_slice_arg_prologue(Emit *e, IRNode *arg) {
    if (!arg || arg->op != IR_COMMA || arg->n_items < 1) return 0;
    return ir_item(e->cur_func, arg, arg->n_items - 1);
}

// Emit a call to a native (registered C) function.
static void emit_native_call(Emit *e, Func *callee, IRNode *n) {
    if (e->d->native_call) { e->d->native_call(e, callee, n); return; }
    int idx = callee->native_tok - TOK_MATHS_FIRST;
    CFuncEntry *ent = &e->vm->run.cfunc_table[idx];
    VTKind rk = n->type.kind;

    // Explicit mixed signature (register_c_func_sig): fixed C name, and each
    // VMT_SLICE_I32 argument expands to a (ptr, len) pair -- see CFuncEntry.
    if (ent->has_sig) {
        const char *sname = ent->sig_name ? ent->sig_name
                                           : intern_get_cstr(e->vm->intern, callee->name);
        // Sequence any computed slice argument first, with the comma operator
        // (whose order, unlike an argument list's, is defined):
        //     (__s0 = ..., text(x, y, __s0.data, __s0.len))
        int n_pre = 0;
        for (int i = 0; i < n->n_items; i++) {
            if (ent->arg_kinds[i] != VMT_SLICE_I32) continue;
            IRNode *arg = ir_item(e->cur_func, n, i);
            if (!ec_slice_arg_prologue(e, arg)) continue;
            // All the items *except* the trailing read: that read is what the
            // argument slot uses, and repeating it here would be a value with
            // no effect (-Wunused-value under -Wall).
            for (int k = 0; k < arg->n_items - 1; k++) {
                buf_str(&e->out, (n_pre || k) ? ", " : "(");
                IRNode *it = ir_item(e->cur_func, arg, k);
                if (it->op == IR_ASSIGN) emit_stmt_as_expr(e, it);
                else                     emit_expr(e, it);
            }
            n_pre++;
        }
        if (n_pre) buf_str(&e->out, ", ");

        buf_str(&e->out, sname);
        buf_char(&e->out, '(');
        for (int i = 0; i < n->n_items; i++) {
            if (i) buf_str(&e->out, ", ");
            IRNode *arg = ir_item(e->cur_func, n, i);
            if (ent->arg_kinds[i] == VMT_SLICE_I32) {
                IRNode *seq = ec_slice_arg_prologue(e, arg);
                emit_slice_ptr_len(e, seq ? seq : arg);
            } else {
                emit_expr(e, arg);
            }
        }
        buf_char(&e->out, ')');
        if (n_pre) buf_char(&e->out, ')');
        return;
    }

    // Pick the variant name that matches the call's resolved type. For i32,
    // n->sub_op records which slot the compiler chose (1 = fixed-point
    // fx_name, 0 = raw-int i32_name) -- see CFuncEntry in vm_types.h.
    const char *fname = 0;
    if      (rk == VMT_F32 && ent->f32_name) fname = ent->f32_name;
    else if (rk == VMT_F64 && ent->f64_name) fname = ent->f64_name;
    else if (rk == VMT_I32) fname = n->sub_op ? ent->fx_name : ent->i32_name;
    if (!fname) fname = intern_get_cstr(e->vm->intern, callee->name);

    buf_str(&e->out, fname);
    buf_char(&e->out, '(');
    for (int i = 0; i < n->n_items; i++) {
        if (i) buf_str(&e->out, ", ");
        emit_expr(e, ir_item(e->cur_func, n, i));
    }
    buf_char(&e->out, ')');
}

// Emit a compound literal for an aggregate type: (int_3){{...}}  or  (int_slice){ptr, len}
static void emit_compound_lit(Emit *e, IRNode *lit) {
    if (e->d->arr_lit) { e->d->arr_lit(e, lit); return; }
    buf_char(&e->out, '(');
    ec_emit_type_name(&e->out, e->vm, e->cur_func, lit->type);
    buf_str(&e->out, "){{");
    if (lit->type.pack_bits && !ec_pack_is_native(lit->type.pack_bits)) {
        // u1/u2/u4 have no C element type, so the initialiser is built as one
        // OR-expression per backing word. Each element is emitted exactly
        // once, so non-constant elements work too and constants fold away.
        int bits = lit->type.pack_bits;
        int epw  = 32 / bits;
        int words = ec_pack_words(lit->n_items, bits);
        for (int w = 0; w < words; w++) {
            if (w) buf_str(&e->out, ", ");
            int first = 1;
            for (int k = 0; k < epw; k++) {
                int i = w * epw + k;
                if (i >= lit->n_items) break;
                if (!first) buf_str(&e->out, " | ");
                first = 0;
                buf_str(&e->out, "((");
                emit_expr(e, ir_item(e->cur_func, lit, i));
                buf_str(&e->out, " & ");
                buf_int(&e->out, (long long)((1u << bits) - 1u));
                buf_str(&e->out, ") << ");
                buf_int(&e->out, (long long)(k * bits));
                buf_char(&e->out, ')');
            }
            if (first) buf_char(&e->out, '0');
        }
        buf_str(&e->out, "}}");
        return;
    }
    for (int i = 0; i < lit->n_items; i++) {
        if (i) buf_str(&e->out, ", ");
        emit_expr(e, ir_item(e->cur_func, lit, i));
    }
    buf_char(&e->out, '}');
    buf_char(&e->out, '}');
}

// Emit the ELEMENT POINTER of an aggregate-valued node -- what you index, what you
// do pointer arithmetic on. `name.data` for the emitter's own array and slice
// structs; the bare name for a host buffer, which is a plain C array in the target
// and has no struct to reach through (see vm.h, "Host buffers"). Every site that
// indexes or sub-slices goes through here rather than writing ".data" inline.
static void emit_agg_data(Emit *e, IRNode *base) {
    if (base->op == IR_LOCAL) {
        int slot = (int)base->ki;
        emit_local_name(e, slot);
        if (e->cur_func->syms[slot].is_host_buf) return;   // already a pointer
    } else if (base->op == IR_ARR_LIT) {
        buf_char(&e->out, '(');
        emit_compound_lit(e, base);
        buf_char(&e->out, ')');
    } else if (base->op == IR_FIELD && base->sub_op != FIELD_ELEM) {
        // A struct member already is a postfix expression: `o.items.data[i]`.
        emit_expr(e, base);
    } else {
        buf_char(&e->out, '(');
        emit_expr(e, base);
        buf_char(&e->out, ')');
    }
    if (!e->d->direct_index) buf_str(&e->out, ".data");
}

// Emit `<base>.data` for an index node's aggregate operand.
static void emit_index_base_data(Emit *e, IRNode *idx) {
    emit_agg_data(e, ir_child(e->cur_func, idx->a));
}

// `base.data[i]`, the element an IR_INDEX names. Indexing directly, a row of a
// nested array folds into its storage -- `a[(o) + (j)]` -- since the target has
// no view to index through.
static void emit_elem_access(Emit *e, IRNode *base, IRNode *idx) {
    if (e->d->direct_index && base->op == IR_SLICE) {
        emit_agg_data(e, ir_item(e->cur_func, base, 0));
        buf_str(&e->out, "[(");
        emit_expr(e, ir_item(e->cur_func, base, 1));
        buf_str(&e->out, ") + (");
        emit_expr(e, idx);
        buf_str(&e->out, ")]");
        return;
    }
    emit_agg_data(e, base);
    buf_char(&e->out, '[');
    emit_expr(e, idx);
    buf_char(&e->out, ']');
}

// ---- struct records (IR_FIELD) ----
//
// A record is a real `struct T` value, a ref a `T*`. The IR never folds nested
// offsets, so every IR_FIELD names exactly one member of the struct its base holds.

// The ref a FIELD_ELEM dereferences, or 0 when it indexes an array of records.
// vm.c only ever builds element 0 of a ref -- that IS the deref.
static IRNode *ec_deref_of(Emit *e, IRNode *n) {
    if (!n || n->op != IR_FIELD || n->sub_op != FIELD_ELEM) return 0;
    IRNode *base = ir_child(e->cur_func, n->a);
    return ec_is_ref_type(base->type) ? base : 0;
}

// The member an IR_FIELD reads: the field of its base's struct at its offset.
static const char *ec_field_name(Emit *e, IRNode *n) {
    IRNode *base = ir_child(e->cur_func, n->a);
    const VMStruct *st = emit_struct_of(e->cur_func, base->type);
    for (int i = 0; st && i < st->n_fields; i++)
        if (st->fields[i].offset == (int)n->ki) return intern_get_cstr(e->vm->intern, st->fields[i].name);
    return "__field";
}

// `p.x`, `a->x` through a ref, `arr.data[i].x`, `make(3).x`.
static void emit_field(Emit *e, IRNode *n) {
    IRNode *base = ir_child(e->cur_func, n->a);
    IRNode *ref  = ec_is_ref_type(base->type) ? base : ec_deref_of(e, base);
    if (ref) {
        if (ref->op == IR_LOCAL) emit_local_name(e, (int)ref->ki);
        else { buf_char(&e->out, '('); emit_expr(e, ref); buf_char(&e->out, ')'); }
        buf_str(&e->out, "->");
    } else {
        int bare = base->op == IR_LOCAL || base->op == IR_FIELD || base->op == IR_CALL;
        if (!bare) buf_char(&e->out, '(');
        emit_expr(e, base);
        if (!bare) buf_char(&e->out, ')');
        buf_char(&e->out, '.');
    }
    ec_put_name(&e->out, ec_field_name(e, n));
}

// One record of an array of them, `arr.data[i]`, or what a ref points at, `(*a)`.
static void emit_field_elem(Emit *e, IRNode *n) {
    IRNode *ref = ec_deref_of(e, n);
    if (ref) {
        buf_str(&e->out, "(*");
        if (ref->op == IR_LOCAL) emit_local_name(e, (int)ref->ki);
        else { buf_char(&e->out, '('); emit_expr(e, ref); buf_char(&e->out, ')'); }
        buf_char(&e->out, ')');
        return;
    }
    emit_agg_data(e, ir_child(e->cur_func, n->a));
    buf_char(&e->out, '[');
    emit_expr(e, ir_child(e->cur_func, n->b));
    buf_char(&e->out, ']');
}

// The address of a record-valued node, for a `T*` parameter or a hidden ref:
// `&p`, `&arr.data[i]`, and a ref passed on as itself rather than `&(*a)`.
static void emit_record_addr(Emit *e, IRNode *n) {
    if (ec_is_ref_type(n->type)) { emit_expr(e, n); return; }
    IRNode *ref = ec_deref_of(e, n);
    if (ref) { emit_expr(e, ref); return; }
    buf_char(&e->out, '&');
    if (n->op == IR_LOCAL || n->op == IR_FIELD) { emit_expr(e, n); return; }
    buf_char(&e->out, '(');
    emit_expr(e, n);
    buf_char(&e->out, ')');
}

// The `lvalue = ` of an assignment to a field or a record, including the
// truncating cast a u8 / u16 member takes.
static void emit_field_store_lhs(Emit *e, IRNode *lv) {
    if (lv->sub_op == FIELD_ELEM) emit_field_elem(e, lv);
    else                          emit_field(e, lv);
    buf_str(&e->out, " = ");
    if (lv->sub_op == FIELD_U8)  buf_str(&e->out, "(unsigned char)");
    if (lv->sub_op == FIELD_U16) buf_str(&e->out, "(unsigned short)");
}

// A node whose value is a bool in a bool_is_type dialect: comparisons, && / ||, !.
static int ec_is_boolish(IRNode *n) {
    if (n->op == IR_UNOP) return n->sub_op == OP_NOT;
    if (n->op != IR_BINOP) return 0;
    switch (n->sub_op) {
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE:
        case OP_AND: case OP_OR:
            return 1;
        default:
            return 0;
    }
}

static void emit_expr(Emit *e, IRNode *n) {
    if (!n) { buf_str(&e->out, "/*null*/"); return; }

    // A bool used as a value is 0 or 1, as it is in C and in the VM.
    if (e->d->bool_is_type && ec_is_boolish(n)) {
        if (!e->raw_bool) {
            buf_str(&e->out, "int(");
            e->raw_bool = 1;
            emit_expr(e, n);
            buf_char(&e->out, ')');
            return;
        }
        e->raw_bool = 0;
    }

    switch (n->op) {

        // ---- constants -------------------------------------------------------
        case IR_CONST_I:
            // C has no negative literals: `-2147483648` is `-(2147483648)`, whose
            // operand does not fit an int and so makes the whole expression 64-bit,
            // and 9223372036854775808LL does not fit anything.
            if (n->type.kind == VMT_I64 && n->ki == (-9223372036854775807LL - 1)) {
                buf_str(&e->out, "(-9223372036854775807LL - 1)");
            } else if (n->type.kind != VMT_I64 && n->ki == -2147483648LL) {
                buf_str(&e->out, "(-2147483647 - 1)");
            } else {
                buf_int(&e->out, n->ki);
                if (n->type.kind == VMT_I64) buf_str(&e->out, "LL");
            }
            break;

        case IR_CONST_F:
            if (n->kf != n->kf || n->kf - n->kf != 0.0) {
                ec_emit_nonfinite(e, n->kf, n->type.kind);
                break;
            }
            // Emit a proper C literal: 1.0 / 1.0f, 1.5 / 1.5f, etc.
            buf_float_lit(&e->out, e->vm, n->kf, n->type.kind, e->d->f32_suffix);
            break;

        // ---- locals / indexing -----------------------------------------------
        case IR_LOCAL:
            emit_local_name(e, (int)n->ki);
            break;

        case IR_INDEX: {
            // Base expression type tells us the element accessor: .data[i]
            // for both fixed arrays and slices.
            VTKind bkind = ir_child(e->cur_func, n->a)->type.kind;

            // Sub-word packed element (n->ki is the width, 0 = unpacked).
            // u8/u16 are declared as native C arrays, so they fall through to
            // the ordinary .data[i] path below and C's integer promotion does
            // the widening -- no masking in the generated code at all.
            if (n->ki && !ec_pack_is_native((int)n->ki)) {
                buf_str(&e->out, "vm_pack_get(");
                emit_index_base_data(e, n);
                buf_str(&e->out, ", ");
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_str(&e->out, ", ");
                buf_int(&e->out, n->ki);
                buf_char(&e->out, ')');
                break;
            }

            if (ir_child(e->cur_func, n->a)->op == IR_ARR_LIT) {
                // Inline array literal: ((int_3){{2, 4, 7}}).data[i]
                buf_str(&e->out, "(");
                emit_compound_lit(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, e->d->direct_index ? "[" : ".data[");
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_char(&e->out, ']');
                buf_char(&e->out, ')');
            } else {
                // Named local aggregate: name.data[i]. General expression:
                // (expr).data[i]. Host buffer: screen[i], no struct in sight.
                emit_elem_access(e, ir_child(e->cur_func, n->a), ir_child(e->cur_func, n->b));
            }
            break;
        }

        // ---- struct field / record element -----------------------------------
        case IR_FIELD:
            if (n->sub_op == FIELD_ELEM) emit_field_elem(e, n);
            else                         emit_field(e, n);
            break;

        // ---- slice length (.len) ---------------------------------------------
        case IR_LEN: {
            // Only slices reach here (see compile_field in vm.c) and a slice is
            // a struct carrying its own len -- so this is just a field read,
            // except for a host buffer, which has no struct and whose length
            // the host gave us as a C expression.
            emit_agg_len(e, ir_child(e->cur_func, n->a));
            break;
        }

        // ---- conversion ------------------------------------------------------
        case IR_CVT:
            if (e->d->c_helpers) {
                int fk = ec_f2i_kind(e->cur_func, emit_func_flags(e->vm, e->cur_func), n);
                if (fk >= 0) {
                    buf_str(&e->out, fk ? "vm_f2l(" : "vm_f2i(");
                    emit_expr(e, ir_child(e->cur_func, n->a));
                    buf_char(&e->out, ')');
                    break;
                }
            }
            if (e->d->ctor_casts) {
                ec_type(e, e->cur_func, n->type);
                buf_char(&e->out, '(');
            } else {
                buf_char(&e->out, '(');
                buf_str(&e->out, ec_scalar_type(n->type.kind));
                buf_str(&e->out, ")(");
            }
            emit_expr(e, ir_child(e->cur_func, n->a));
            buf_char(&e->out, ')');
            break;

        // ---- bit-preserving reinterpretation ---------------------------------
#if VM_HAS_INSPECT
        // Hover inspection is an editor-only facility: exported code is the
        // code the user wrote, so the node renders as its operand alone. No
        // dead-locals handling of the IR_PRINT kind is needed precisely
        // because nothing is dropped -- the operand is still emitted.
        case IR_INSPECT:
            emit_expr(e, ir_child(e->cur_func, n->a));
            break;
#endif

        case IR_BITCAST:
            // Same kind means i32 <-> fxN: the shift is compile-time only, so
            // the operand's word already *is* the result.
            if (n->sub_op != (int)n->type.kind)
                buf_str(&e->out, e->d->bitcast_fn ? e->d->bitcast_fn(n->sub_op, n->type.kind)
                                                  : ec_bitcast_fn(n->sub_op, n->type.kind));
            buf_char(&e->out, '(');
            emit_expr(e, ir_child(e->cur_func, n->a));
            buf_char(&e->out, ')');
            break;

        // ---- unary ops -------------------------------------------------------
        case IR_UNOP:
            if (n->sub_op == OP_NEG && e->d->c_helpers
                && (emit_func_flags(e->vm, e->cur_func) & VM_FLAG_C_INT_WRAP)
                && (n->type.kind == VMT_I32 || n->type.kind == VMT_I64)) {
                buf_str(&e->out, n->type.kind == VMT_I64 ? "((long long)(0ull - (unsigned long long)("
                                                         : "((int)(0u - (unsigned int)(");
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ")))");
            } else if (n->sub_op == OP_NEG) {
                buf_str(&e->out, "(-(");
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, "))");
            } else { // OP_NOT
                buf_str(&e->out, "(!(");
                emit_cond(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, "))");
            }
            break;

        // ---- binary ops ------------------------------------------------------
        case IR_BINOP: {
            // Divide/modulo under VM_FLAG_CHECK_DIV_ZERO: `(b ? a / b : 0)`.
            // A literal divisor decides the guard at compile time, so fold it
            // away rather than hand the C compiler a constant condition.
            const char *dz = (emit_func_flags(e->vm, e->cur_func) & VM_FLAG_CHECK_DIV_ZERO) ? ec_divzero_result(e, n) : 0;
            IRNode *divisor = ec_thru_inspect(e->cur_func, ir_child(e->cur_func, n->b));
            int dz_const = dz ? ec_const_divisor(divisor) : 0;
            // A literal integer -1 is no safer than a variable: INT_MIN / -1 traps.
            if (dz_const > 0 && divisor->op == IR_CONST_I && divisor->ki == -1) dz_const = 0;
            // An integer % the target spells as a call.
            int imod = e->d->imod_fn && n->type.kind == VMT_I32
                    && (n->sub_op == OP_MOD || n->sub_op == OP_MOD_NATIVE);
            int dh = e->d->c_helpers ? ec_div_helper(e->cur_func, emit_func_flags(e->vm, e->cur_func), n) : -1;

            if (dh >= 0) {
                buf_str(&e->out, EC_DIV_FN[dh]);
                buf_char(&e->out, '(');
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ", ");
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_char(&e->out, ')');
            } else if (dz && dz_const < 0) {
                // Literal zero divisor: always the else arm, but the numerator
                // still has to run for whatever side effects it carries.
                buf_char(&e->out, '(');
                buf_str(&e->out, e->d->void_expr);
                buf_char(&e->out, '(');
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, "), ");
                buf_str(&e->out, dz);
                buf_char(&e->out, ')');
            } else if (dz && dz_const == 0) {
                buf_char(&e->out, '(');
                emit_cond_paren(e, ir_child(e->cur_func, n->b));
                if (imod) {
                    buf_str(&e->out, " ? ");
                    buf_str(&e->out, e->d->imod_fn);
                    buf_char(&e->out, '(');
                    emit_expr(e, ir_child(e->cur_func, n->a));
                    buf_str(&e->out, ", ");
                    emit_expr(e, ir_child(e->cur_func, n->b));
                    buf_str(&e->out, ") : ");
                } else {
                    buf_str(&e->out, " ? (");
                    emit_expr(e, ir_child(e->cur_func, n->a));
                    buf_str(&e->out, n->sub_op == OP_DIV ? ") / (" : ") % (");
                    emit_expr(e, ir_child(e->cur_func, n->b));
                    buf_str(&e->out, ") : ");
                }
                buf_str(&e->out, dz);
                buf_char(&e->out, ')');
            } else if (imod) {
                buf_str(&e->out, e->d->imod_fn);
                buf_char(&e->out, '(');
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ", ");
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_char(&e->out, ')');
            } else if (n->sub_op == OP_BSHL && n->type.kind == VMT_I32) {
                // Shift in unsigned space for well-defined overflow. Emitted as
                // `u * (1u << n)` rather than `u << n` because the two are
                // identical mod 2^32 but the multiply form doesn't trip
                // -Wshift-negative-value: GCC constant-folds a negative operand
                // (e.g. (int)(-0.5*65536.0)) back through the unsigned cast and
                // warns on the fold even though the shift itself is unsigned.
                if (e->d->ctor_casts) {
                    buf_str(&e->out, "(int(");
                    buf_str(&e->out, e->d->u32_type);
                    buf_char(&e->out, '(');
                } else {
                    buf_str(&e->out, "((int)((unsigned int)(");
                }
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ") * (1u << ");
                ec_emit_shift_count(e, ir_child(e->cur_func, n->b), 31);
                buf_str(&e->out, ")))");
            } else if (n->sub_op == OP_BSHL && n->type.kind == VMT_I64) {
                // See the I32 case above -- `u * (1ull << n)` for the same reason.
                buf_str(&e->out, "((long long)((unsigned long long)(");
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ") * (1ull << ");
                ec_emit_shift_count(e, ir_child(e->cur_func, n->b), 63);
                buf_str(&e->out, ")))");
            } else if (n->sub_op == OP_BSHR && (n->type.kind == VMT_I32 || n->type.kind == VMT_I64)) {
                buf_char(&e->out, '(');
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, " >> ");
                ec_emit_shift_count(e, ir_child(e->cur_func, n->b), n->type.kind == VMT_I64 ? 63 : 31);
                buf_char(&e->out, ')');
            } else if (e->d->c_helpers && (emit_func_flags(e->vm, e->cur_func) & VM_FLAG_C_INT_WRAP)
                       && (n->type.kind == VMT_I32 || n->type.kind == VMT_I64)
                       && (n->sub_op == OP_ADD || n->sub_op == OP_SUB || n->sub_op == OP_MUL)) {
                // #enable c_int_wrap: through unsigned, so an overflow wraps as the
                // VM's does instead of being undefined.
                int w = n->type.kind == VMT_I64;
                buf_str(&e->out, w ? "((long long)((unsigned long long)(" : "((int)((unsigned int)(");
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ")");
                buf_str(&e->out, ec_binop_str(n->sub_op));
                buf_str(&e->out, w ? "(unsigned long long)(" : "(unsigned int)(");
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_str(&e->out, ")))");
            } else if (n->sub_op == OP_AND || n->sub_op == OP_OR) {
                buf_char(&e->out, '(');
                emit_cond(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ec_binop_str(n->sub_op));
                emit_cond(e, ir_child(e->cur_func, n->b));
                buf_char(&e->out, ')');
            } else {
                buf_char(&e->out, '(');
                emit_expr(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, ec_binop_str(n->sub_op));
                emit_expr(e, ir_child(e->cur_func, n->b));
                buf_char(&e->out, ')');
            }
            break;
        }

        // ---- ternary select (?:) ---------------------------------------------
        case IR_SELECT:
            buf_char(&e->out, '(');
            emit_cond(e, ir_child(e->cur_func, n->a));
            buf_str(&e->out, " ? ");
            emit_expr(e, ir_child(e->cur_func, n->b));
            buf_str(&e->out, " : ");
            emit_expr(e, ir_child(e->cur_func, n->c));
            buf_char(&e->out, ')');
            break;

        // ---- comma / sequence expression -------------------------------------
        // (item0, item1, ..., itemLast). Leading items are IR_ASSIGN statements
        // emitted in C expression form (local = rhs, or a[i] = rhs when an
        // update operator like `a[i] += 1` is used as a value); the last item is
        // the value.
        case IR_COMMA:
            buf_char(&e->out, '(');
            for (int i = 0; i < n->n_items; i++) {
                if (i) buf_str(&e->out, ", ");
                IRNode *it = ir_item(e->cur_func, n, i);
                if (it->op == IR_ASSIGN) emit_stmt_as_expr(e, it);
                else                     emit_expr(e, it);
            }
            buf_char(&e->out, ')');
            break;

        // ---- array literal in expression context (return / arg) --------------
        case IR_ARR_LIT:
            // Compound literal: (int_3){{1, 2, 3}}
            emit_compound_lit(e, n);
            break;

        // ---- string / constant-data slice literal ----------------------------
        case IR_DATA_SLICE:
            // Slice value: (int_slice){(int[]){65, 66, 67}, 3}. The register_
            // c_func_sig call path emits this as a bare (ptr, len) pair instead
            // (see emit_slice_ptr_len); this branch is for a data slice used as
            // a plain slice value, e.g. passed to a user-defined []i32 param.
            buf_char(&e->out, '(');
            ec_emit_type_name(&e->out, e->vm, e->cur_func, n->type);
            buf_str(&e->out, "){");
            emit_dataslice_array(e, n);
            buf_str(&e->out, ", ");
            buf_int(&e->out, (long long)n->n_items);
            buf_char(&e->out, '}');
            break;

        // ---- subslice: slice(x, start, len) ----------------------------------
        case IR_SLICE: {
            if (e->d->slice_value) { e->d->slice_value(e, n); break; }
            // (u8_slice){ x.data + start, len }. The pointer arithmetic is in
            // element units on both sides -- C scales by the element type, which
            // for a u8/u16 packed array is exactly the VM's stride.
            //
            // Unless sub_op says the operands are known in range, both go through
            // vm_slice_at/vm_slice_n so an out-of-range pair yields a shorter slice
            // here exactly as it does in the interpreter. Each operand is named
            // several times, which vm.c's stage_operand makes safe.
            IRNode *base = ir_item(e->cur_func, n, 0);
            IRNode *st   = ir_item(e->cur_func, n, 1);
            IRNode *ln   = ir_item(e->cur_func, n, 2);
            buf_char(&e->out, '(');
            ec_emit_type_name(&e->out, e->vm, e->cur_func, n->type);
            buf_str(&e->out, "){");
            emit_agg_data(e, base);
            buf_str(&e->out, " + ");
            if (n->sub_op) {
                emit_expr(e, st);
                buf_str(&e->out, ", ");
                emit_expr(e, ln);
            } else {
                buf_str(&e->out, "vm_slice_at(");
                emit_slice_base_len(e, base);
                buf_str(&e->out, ", ");
                emit_expr(e, st);
                buf_str(&e->out, "), vm_slice_n(");
                emit_slice_base_len(e, base);
                buf_str(&e->out, ", ");
                emit_expr(e, st);
                buf_str(&e->out, ", ");
                emit_expr(e, ln);
                buf_char(&e->out, ')');
            }
            buf_char(&e->out, '}');
            break;
        }

        // ---- formatted append ------------------------------------------------
        case IR_FMT: {
            // vm_fmt_KIND(dst.data, cap, cursor, value...) -- see EC_FMT_*.
            // emit_slice_ptr_len writes the "pointer, length" pair for both the
            // destination buffer and (for FMT_BYTES) the source string, which
            // is the same pair shape a register_c_func_sig slice argument uses.
            static const char *fn[] = { "vm_fmt_bytes(", "vm_fmt_i32(", "vm_fmt_i64(",
                                        "vm_fmt_fx(", "vm_fmt_f64(" };
            buf_str(&e->out, fn[n->sub_op]);
            emit_slice_ptr_len(e, ir_item(e->cur_func, n, 0));
            buf_str(&e->out, ", ");
            emit_expr(e, ir_item(e->cur_func, n, 1));
            buf_str(&e->out, ", ");
            if (n->sub_op == FMT_BYTES) {
                emit_slice_ptr_len(e, ir_item(e->cur_func, n, 2));
            } else {
                emit_expr(e, ir_item(e->cur_func, n, 2));
                if (n->sub_op == FMT_FX) {
                    buf_str(&e->out, ", ");
                    buf_int(&e->out, n->ki & 0xff);
                    buf_str(&e->out, ", ");
                    buf_int(&e->out, (n->ki >> 8) & 0xff);
                } else if (n->sub_op == FMT_F64) {
                    buf_str(&e->out, ", ");
                    buf_int(&e->out, n->ki & 0xff);
                }
            }
            buf_char(&e->out, ')');
            break;
        }

        // ---- function calls --------------------------------------------------
        case IR_CALL: {
            Func *callee = e->vm->run.funcs_by_id[(int)n->ki];
            if (callee->native_tok != 0) {
                emit_native_call(e, callee, n);
            } else if (e->d->user_call && e->d->user_call(e, callee, n)) {
                // the dialect spelled it (an intrinsic)
            } else {
                buf_str(&e->out, ec_get_cname(e, callee));
                buf_char(&e->out, '(');
                for (int i = 0; i < n->n_items; i++) {
                    if (i) buf_str(&e->out, ", ");
                    // When callee expects a slice but arg is a fixed array,
                    // wrap in a slice literal: (slice_type){arg.data, len}
                    IRNode *arg = ir_item(e->cur_func, n, i);
                    if (i < callee->n_params) {
                        VMType want = callee->syms[callee->param_slot[i]].type;
                        if (ec_is_ref_type(want)) {
                            emit_record_addr(e, arg);
                            continue;
                        }
                        if (ec_is_slice(want.kind) && ec_is_array(arg->type.kind) && !e->d->arrays_as_slices) {
                            buf_char(&e->out, '(');
                            ec_emit_type_name(&e->out, e->vm, callee, want);
                            buf_str(&e->out, "){");
                            emit_expr(e, arg);
                            buf_str(&e->out, ".data, ");
                            // A C slice counts elements -- records, for a struct.
                            buf_int(&e->out, (long long)(arg->type.struct_id && arg->type.inner_len
                                                         ? arg->type.len / arg->type.inner_len
                                                         : arg->type.len));
                            buf_char(&e->out, '}');
                            continue;
                        }
                    }
                    emit_expr(e, arg);
                }
                buf_char(&e->out, ')');
            }
            break;
        }

        default:
            buf_str(&e->out, "/*?expr?*/");
            break;
    }
}

// Emit an expression that lands in a boolean position (an if/while condition,
// a ?: selector, a && / || / ! operand, or the divisor guard below). Only the
// forms -Wint-in-bool-context objects to get an explicit `!= 0`; everything
// else is emitted as-is so the output stays readable.
static void emit_cond(Emit *e, IRNode *n) {
    if (e->d->bool_is_type) {
        if (ec_is_boolish(n)) { e->raw_bool = 1; emit_expr(e, n); return; }
        int fl = n->type.kind == VMT_F32 || n->type.kind == VMT_F64;
        buf_char(&e->out, '(');
        emit_expr(e, n);
        buf_str(&e->out, fl ? " != 0.0)" : " != 0)");
        return;
    }
    if (!ec_needs_bool_coerce(n)) {
        emit_expr(e, n);
        return;
    }
    int own = ec_self_parenthesised(e, n); // true for every coerced node today
    if (!own) buf_char(&e->out, '(');
    emit_expr(e, n);
    if (!own) buf_char(&e->out, ')');
    buf_str(&e->out, " != 0");
}

// Same, for the contexts whose syntax already demands surrounding parentheses
// (if/while/for conditions, the divisor guard's selector). A node that brings
// its own outer pair supplies them, so `a == b` comes out as `if (a == b)` and
// not `if ((a == b))` -- see ec_self_parenthesised.
static void emit_cond_paren(Emit *e, IRNode *n) {
    if (n && e->d->bool_is_type) {
        // A bare comparison brings its own parentheses; anything else gets them
        // from emit_cond's `(x != 0)`.
        emit_cond(e, n);
        return;
    }
    if (n && !ec_needs_bool_coerce(n) && ec_self_parenthesised(e, n)) {
        emit_expr(e, n);
        return;
    }
    buf_char(&e->out, '(');
    emit_cond(e, n);
    buf_char(&e->out, ')');
}

// ============================================================================
// Statement emitter
// ============================================================================

static void emit_stmt(Emit *e, IRNode *n);

// `<type> ` -- the prefix that turns an assignment into a declaration, for the
// statement emit_plan_decls picked to carry one.
static void ec_emit_decl_type(Emit *e, int slot) {
    ec_type(e, e->cur_func, e->cur_func->syms[slot].type);
    buf_char(&e->out, ' ');
}

// Emit the statement list inside a block node without extra braces.
static void emit_block_stmts(Emit *e, IRNode *blk) {
    if (!blk || blk->op != IR_BLOCK) return;
    for (int i = 0; i < blk->n_items; i++)
        emit_stmt(e, ir_item(e->cur_func, blk, i));
}

// Emit a statement node as a bare C expression -- no indent, no semicolon.
// Only the shapes a for-loop step clause can take (vm.c rejects the rest).
static void emit_stmt_as_expr(Emit *e, IRNode *n) {
    if (!n) return;
    if (n->op == IR_ASSIGN) {
        IRNode *lv = ir_child(e->cur_func, n->a);
        if (lv->op == IR_FIELD) {
            emit_field_store_lhs(e, lv);
            emit_expr(e, ir_child(e->cur_func, n->b));
            return;
        }
        if (lv->op == IR_LOCAL && ec_is_ref_type(e->cur_func->syms[(int)lv->ki].type)) {
            emit_local_name(e, (int)lv->ki);
            buf_str(&e->out, " = ");
            emit_record_addr(e, ir_child(e->cur_func, n->b));
            return;
        }
        if (lv->op == IR_INDEX) {
            emit_elem_access(e, ir_child(e->cur_func, lv->a), ir_child(e->cur_func, lv->b));
            buf_str(&e->out, " = ");
        } else {
            emit_local_name(e, (int)lv->ki);
            buf_str(&e->out, " = ");
        }
        emit_expr(e, ir_child(e->cur_func, n->b));
        return;
    }
    emit_expr(e, ir_child(e->cur_func, n->a));
}

static void emit_stmt(Emit *e, IRNode *n) {
    if (!n) return;

    switch (n->op) {

        // ---- block -----------------------------------------------------------
        case IR_BLOCK:
            emit_indent(e); buf_str(&e->out, "{\n");
            e->indent++;
            for (int i = 0; i < n->n_items; i++) emit_stmt(e, ir_item(e->cur_func, n, i));
            e->indent--;
            emit_indent(e); buf_str(&e->out, "}\n");
            break;

        // ---- print -----------------------------------------------------------
        // Handled here rather than in emit_expr because dropping it has to take the
        // whole STATEMENT: emitting nothing from an expression slot would leave a
        // bare `;`, and the string build feeding the call has to go with it. That is
        // what makes print free on a target that has none -- and why print's
        // arguments must be side-effect-free (see vm_set_print_emit in vm.h).
        case IR_PRINT: {
            if (e->vm->run.print_emit != VM_PRINT_EMIT_CALL || e->d->elide_print) {
                emit_indent(e);
                buf_str(&e->out, "/* print() elided */\n");
                break;
            }
            IRNode *arg = ir_child(e->cur_func, n->a);
            // A computed payload (a string build) is an IR_COMMA ending in a read
            // of the finished slice. VM_PRINT names its argument twice for the
            // (ptr, len) split, so sequence the build ahead of the call with the
            // comma operator -- exactly what emit_native_call does.
            IRNode *seq = ec_slice_arg_prologue(e, arg);
            emit_indent(e);
            if (seq) {
                buf_char(&e->out, '(');
                for (int k = 0; k < arg->n_items - 1; k++) {
                    if (k) buf_str(&e->out, ", ");
                    IRNode *it = ir_item(e->cur_func, arg, k);
                    if (it->op == IR_ASSIGN) emit_stmt_as_expr(e, it);
                    else                     emit_expr(e, it);
                }
                buf_str(&e->out, ", ");
            }
            buf_str(&e->out, "VM_PRINT(");
            emit_slice_ptr_len(e, seq ? seq : arg);
            buf_char(&e->out, ')');
            if (seq) buf_char(&e->out, ')');
            buf_str(&e->out, ";\n");
            break;
        }

        // ---- if / else -------------------------------------------------------
        case IR_IF:
            emit_indent(e); buf_str(&e->out, "if ");
            emit_cond_paren(e, ir_child(e->cur_func, n->a));
            buf_str(&e->out, " {\n");
            e->indent++;
            emit_block_stmts(e, ir_child(e->cur_func, n->b));
            e->indent--;
            emit_indent(e); buf_char(&e->out, '}');
            if (n->c >= 0) {
                buf_str(&e->out, " else {\n");
                e->indent++;
                emit_block_stmts(e, ir_child(e->cur_func, n->c));
                e->indent--;
                emit_indent(e); buf_char(&e->out, '}');
            }
            buf_char(&e->out, '\n');
            break;

        // ---- while / for ---------------------------------------------------
        // For `for`, the init clause is already a separate statement ahead of this
        // node; the step goes in the real increment slot so `continue` still runs it.
        //
        // With a loop_guard the loop sits in a block holding its own counter, and
        // breaks once the counter passes the cap -- counted at the top of the
        // body, so a `continue` counts too. A `for` with a fixed trip count
        // cannot run away and gets none.
        case IR_WHILE:
        case IR_FOR: {
            int guard = e->d->loop_guard > 0 && !(n->op == IR_FOR && n->ki) ? ++e->n_guards : 0;
            if (guard) {
                emit_indent(e); buf_str(&e->out, "{\n");
                e->indent++;
                emit_indent(e); buf_str(&e->out, "int vm_g"); buf_int(&e->out, guard); buf_str(&e->out, " = 0;\n");
            }
            if (n->op == IR_WHILE) {
                emit_indent(e); buf_str(&e->out, "while ");
                emit_cond_paren(e, ir_child(e->cur_func, n->a));
            } else {
                IRNode *step = ir_child(e->cur_func, n->c);
                emit_indent(e); buf_str(&e->out, "for (; ");
                emit_cond(e, ir_child(e->cur_func, n->a));
                buf_str(&e->out, "; ");
                if (step) {
                    for (int i = 0; i < step->n_items; i++) {
                        if (i) buf_str(&e->out, ", ");
                        emit_stmt_as_expr(e, ir_item(e->cur_func, step, i));
                    }
                }
                buf_char(&e->out, ')');
            }
            buf_str(&e->out, " {\n");
            e->indent++;
            if (guard) {
                emit_indent(e); buf_str(&e->out, "if (++vm_g"); buf_int(&e->out, guard);
                buf_str(&e->out, " > "); buf_int(&e->out, e->d->loop_guard); buf_str(&e->out, ") break;\n");
            }
            emit_block_stmts(e, ir_child(e->cur_func, n->b));
            e->indent--;
            emit_indent(e); buf_str(&e->out, "}\n");
            if (guard) {
                e->indent--;
                emit_indent(e); buf_str(&e->out, "}\n");
            }
            break;
        }

        // ---- loop controls ---------------------------------------------------
        case IR_BREAK:
            emit_indent(e); buf_str(&e->out, "break;\n");
            break;

        case IR_CONTINUE:
            emit_indent(e); buf_str(&e->out, "continue;\n");
            break;

        // ---- return ----------------------------------------------------------
        case IR_RETURN:
            emit_indent(e); buf_str(&e->out, "return");
            if (n->a >= 0) {
                buf_char(&e->out, ' ');
                emit_expr(e, ir_child(e->cur_func, n->a));
            }
            buf_str(&e->out, ";\n");
            break;

        // ---- assignment ------------------------------------------------------
        case IR_ASSIGN: {
            IRNode *lv = ir_child(e->cur_func, n->a);
            IRNode *rv = ir_child(e->cur_func, n->b);

            if (lv->op == IR_FIELD) {
                // A field, or a whole record: `p.x = v;`, `arr.data[i] = p;`, `*a = p;`.
                emit_indent(e);
                emit_field_store_lhs(e, lv);
                emit_expr(e, rv);
                buf_str(&e->out, ";\n");
            } else if (lv->op == IR_LOCAL && ec_is_ref_type(e->cur_func->syms[(int)lv->ki].type)) {
                // A hidden ref, bound to the record once so its base runs once.
                emit_indent(e);
                if (n == e->decl_stmt) ec_emit_decl_type(e, (int)lv->ki);
                emit_local_name(e, (int)lv->ki);
                buf_str(&e->out, " = ");
                emit_record_addr(e, rv);
                buf_str(&e->out, ";\n");
            } else if (rv->op == IR_ARR_LIT) {
                // Array literal on RHS: struct assignment or element-by-element.
                int slot  = (int)lv->ki;
                VMSym *s  = &e->cur_func->syms[slot];
                if (ec_is_aggregate(s->type.kind)) {
                    // Struct assignment:  name = (int_3){{...}};
                    emit_indent(e);
                    if (n == e->decl_stmt) ec_emit_decl_type(e, slot);
                    emit_local_name(e, slot);
                    buf_str(&e->out, " = ");
                    emit_compound_lit(e, rv);
                    buf_str(&e->out, ";\n");
                } else {
                    // Should not happen: array literal assigned to scalar.
                    buf_str(&e->out, "/* arr lit to scalar? */\n");
                }
            } else if (lv->op == IR_INDEX) {
                // Index assignment: arr.data[i] = expr
                emit_indent(e);
                // u1/u2/u4 are read-modify-write, and the index would appear
                // three times if written inline -- vm_pack_set evaluates it once.
                if (lv->ki && !ec_pack_is_native((int)lv->ki)) {
                    buf_str(&e->out, "vm_pack_set(");
                    emit_index_base_data(e, lv);
                    buf_str(&e->out, ", ");
                    emit_expr(e, ir_child(e->cur_func, lv->b));
                    buf_str(&e->out, ", ");
                    buf_int(&e->out, lv->ki);
                    buf_str(&e->out, ", ");
                    emit_expr(e, rv);
                    buf_str(&e->out, ");\n");
                    break;
                }
                emit_elem_access(e, ir_child(e->cur_func, lv->a), ir_child(e->cur_func, lv->b));
                buf_str(&e->out, " = ");
                // Narrowing into a native packed element: make the truncation
                // explicit so /W3 and -Wconversion stay quiet.
                if (lv->ki) {
                    buf_char(&e->out, '(');
                    buf_str(&e->out, ec_pack_type((int)lv->ki));
                    buf_char(&e->out, ')');
                }
                emit_expr(e, rv);
                buf_str(&e->out, ";\n");
            } else {
                // Scalar local assignment.
                emit_indent(e);
                // The declaration sank here from the prologue: this is the
                // first thing that touches the local and it writes the whole
                // value, so zero-initialising it up front would only have been
                // thrown away. See emit_plan_decls.
                if (n == e->decl_stmt) ec_emit_decl_type(e, (int)lv->ki);
                emit_local_name(e, (int)lv->ki);
                buf_str(&e->out, " = ");
                emit_expr(e, rv);
                buf_str(&e->out, ";\n");
            }
            break;
        }

        // ---- expression statement (call result discarded) --------------------
        case IR_EXPR_STMT:
        case IR_CALL_STMT: {
            IRNode *v = ir_child(e->cur_func, n->a);
            // print() compiles to a void expression, so it arrives wrapped
            // here. It has to be handled as a STATEMENT (dropping it takes the
            // trailing `;` with it), so hand it back to the case above.
            if (v && v->op == IR_PRINT) { emit_stmt(e, v); break; }
            emit_indent(e);
            emit_expr(e, v);
            buf_str(&e->out, ";\n");
            break;
        }

        default:
            emit_indent(e); buf_str(&e->out, "/* unknown stmt */\n");
            break;
    }
}

// ============================================================================
// Function emitter
// ============================================================================

// Emit the return-type + name + parameter list (no body) as a forward decl.
// When e->strip_unused is set, a param the body never referenced (see
// func_param_used(), vm.h) is left out entirely -- see
// func_emit_c_named_stripped's doc comment (vm_emit_c.h).
static void emit_func_signature(Emit *e, Func *f, const char *cname) {
    // Return type
    ec_type(e, f, f->ret_type);
    buf_char(&e->out, ' ');
    buf_str(&e->out, cname);
    buf_char(&e->out, '(');

    int n_emitted = 0;
    for (int i = 0; i < f->n_params; i++) {
        VMSym *s = &f->syms[f->param_slot[i]];
        if (e->strip_unused && !s->used) continue;
        if (n_emitted) buf_str(&e->out, ", ");
        if (e->d->param_decl) {
            e->d->param_decl(e, f, i);
        } else {
            ec_emit_type_name(&e->out, e->vm, f, s->type);
            buf_char(&e->out, ' ');
            if (s->name != 0) ec_put_name(&e->out, intern_get_cstr(e->vm->intern, s->name));
            else              buf_str(&e->out, "__p");
        }
        n_emitted++;
    }
    if (n_emitted == 0) buf_str(&e->out, "void");
    buf_char(&e->out, ')');
}

// Emit a complete function definition.
static void emit_func(Emit *e, Func *f, const char *cname) {
    Func *saved   = e->cur_func;
    e->cur_func   = f;
    e->indent     = 0;

    emit_func_signature(e, f, cname);
    buf_str(&e->out, "\n{\n");
    e->indent = 1;

    IRNode  *body    = f->body >= 0 ? &f->nodes[f->body] : 0;
    int      n_stmt  = body ? body->n_items : 0;
    char    *moved   = 0;
    IRNode **decl_of = 0;
    if (n_stmt > 0 && f->n_syms > 0) {
        moved   = (char *)mem_alloc(&e->vm->run.mem, (size_t)f->n_syms);
        decl_of = (IRNode **)mem_alloc(&e->vm->run.mem, sizeof(IRNode *) * (size_t)n_stmt);
        if (moved && decl_of) {
            for (int k = 0; k < f->n_syms; k++) moved[k] = 0;
            for (int k = 0; k < n_stmt; k++)    decl_of[k] = 0;
            emit_plan_decls(e->vm, f, body, n_stmt, 1, moved, decl_of);
        } else {
            moved = 0; decl_of = 0;    // no room to plan: every local declares here
        }
    }

    // Which symbols the EMITTED body still mentions. Not the same as "every symbol
    // the compiler made": a dropped print() takes its string build's hidden locals
    // out of the output, and those are created with VMSym.used already set.
    char *touched = 0;
    if (body && f->n_syms > 0) {
        touched = (char *)mem_alloc(&e->vm->run.mem, (size_t)f->n_syms);
        if (touched) {
            for (int k = 0; k < f->n_syms; k++) touched[k] = 0;
            emit_mark_slots(f, body, touched, f->n_syms,
                            e->vm->run.print_emit != VM_PRINT_EMIT_CALL || e->d->elide_print);
        }
    }

    // ---- local variable declarations ----------------------------------------
    // Emit one declaration per non-param, non-func symbol.  Variables are
    // zero-initialised to match the VM's zeroed frame -- except the ones whose
    // declaration sank onto their first assignment (emit_plan_decls), where the
    // zero would be dead.
    int had_decl = 0;
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        if (s->is_func)             continue;  // function reference, not a var
        if (s->is_global)           continue;  // file-scope; see vm_emit_c_globals
        if (s->is_host_buf)         continue;  // the target declares it; see vm.h
        if (emit_is_param(f, i))      continue;  // parameters declared in sig
        if (s->type.kind == VMT_VOID) continue; // unresolved / void
        if (moved && moved[i])      continue;  // declared at its first assignment
        if (touched && !touched[i]) continue;  // nothing emitted mentions it

        emit_indent(e);
        ec_type(e, e->cur_func, s->type);
        buf_char(&e->out, ' ');
        emit_local_name(e, i);
        if (e->d->zero_value) {
            buf_str(&e->out, " = ");
            e->d->zero_value(e, s->type);
            buf_str(&e->out, ";\n");
        } else {
            buf_str(&e->out, ec_is_ref_type(s->type) ? " = 0;\n" : " = {0};\n");
        }

        had_decl = 1;
    }

    // ---- (void) casts for never-read symbols --------------------------------
    // VMSym.used is only set when compile_ident resolves a *read* of the symbol, so
    // a local that is merely assigned -- or a param the body ignores -- would trip
    // -Wunused-variable in the generated C. Emit `(void)x;` for each. Params dropped
    // by e->strip_unused are skipped: they are not in the signature at all.
    //
    // A local the emitted body never mentions was skipped by the declaration loop
    // above, so there is no name here to cast. A PARAMETER is different: it is
    // declared by the signature whether the body mentions it or not.
    int had_void = 0;
    for (int i = 0; e->d->void_casts && i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        if (s->is_func)               continue;
        // A shared variable is file-scope storage, never an unused local, so
        // it cannot trip -Wunused-variable and naming it here would not even
        // compile (it is not declared in this scope).
        if (s->is_global)             continue;
        // Same for a host buffer: file-scope in the TARGET, not declared in
        // this scope at all, so `(void)screen;` would not compile either.
        if (s->is_host_buf)           continue;
        if (touched && !touched[i] && !emit_is_param(f, i)) continue;
        if (s->used && (!touched || touched[i])) continue;
        if (s->type.kind == VMT_VOID) continue;
        if (e->strip_unused && emit_is_param(f, i)) continue;

        emit_indent(e);
        buf_str(&e->out, "(void)");
        emit_local_name(e, i);
        buf_str(&e->out, ";\n");
        had_void = 1;
    }

    // blank line between the declaration prologue and the body
    if (had_decl || had_void) buf_char(&e->out, '\n');

    // ---- body ---------------------------------------------------------------
    for (int i = 0; i < n_stmt; i++) {
        e->decl_stmt = decl_of ? decl_of[i] : 0;
        emit_stmt(e, ir_item(f, body, i));
    }
    e->decl_stmt = 0;

    buf_str(&e->out, "}\n");
    e->cur_func = saved;
}

// ============================================================================
// Public API
// ============================================================================

// Whether some function in the list calls list[fi]. A nameless function nothing
// calls is a top-level __script__ and needs no prototype; a nameless one that IS
// called is a `=>` lambda (map's callback) and does.
static int ec_is_called(VM *vm, Func **list, int count, int fi) {
    for (int k = 0; k < count; k++)
        for (int i = 0; i < list[k]->n_nodes; i++) {
            IRNode *n = &list[k]->nodes[i];
            if (n->op == IR_CALL && vm->run.funcs_by_id[(int)n->ki] == list[fi]) return 1;
        }
    return 0;
}

char *vm_emit_c(VM *vm) {
    // Collect emittable functions.
    Func       *func_list [MAX_EMIT_FUNCS];
    const char *func_cnames[MAX_EMIT_FUNCS];

    int count = emit_collect_funcs(vm, func_list, MAX_EMIT_FUNCS);
    if (count < 0) return 0;
    build_cnames(vm, func_list, count, func_cnames);

    Emit e;
    ec_emit_init(&e, vm, &EC_DIALECT_C);
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = count;

    if (count == 0) {
        buf_str(&e.out, "/* No user-defined functions to emit. */\n");
        if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
        return e.out.buf;
    }

    // ---- typedefs for aggregate types ---------------------------------------
    AggType *agg_types = 0;
    int n_agg = collect_agg_types(vm, func_list, count, &agg_types);
    if (n_agg > 0) {
        ec_emit_typedefs(&e.out, vm, agg_types, n_agg, 0);
        buf_char(&e.out, '\n');
        if (ec_needs_pack_helpers(agg_types, n_agg)) {
            buf_str(&e.out, EC_PACK_HELPERS);
            buf_char(&e.out, '\n');
        }
    }
    if (ec_needs_slice_helpers(func_list, count)) buf_str(&e.out, EC_SLICE_HELPERS);
    if (ec_needs_bitcast_helpers(func_list, count)) buf_str(&e.out, EC_BITCAST_HELPERS);
    ec_emit_div_helpers(&e.out, ec_div_helpers_used(vm, func_list, count));
    ec_emit_f2i_helpers(vm, &e.out, func_list, count);
    if (ec_needs_nonfinite_helpers(func_list, count)) buf_str(&e.out, EC_NONFINITE_HELPERS);
    ec_emit_fmt_helpers(&e.out, ec_fmt_kinds_used(vm, func_list, count));
    if (ec_needs_print_macro(vm, func_list, count)) buf_str(&e.out, EC_PRINT_MACRO);

    // Forward declarations so mutually-recursive functions compile regardless
    // of order.  Skip the __script__ wrapper -- nothing calls it. A `=>` lambda
    // is nameless too, but IS called, so it is declared like any other function.
    for (int i = 0; i < count; i++) {
        if (func_list[i]->name == 0 && !ec_is_called(vm, func_list, count, i)) continue;
        emit_func_signature(&e, func_list[i], func_cnames[i]);
        buf_str(&e.out, ";\n");
    }
    buf_char(&e.out, '\n');

    // Full definitions.
    for (int i = 0; i < count; i++) {
        emit_func(&e, func_list[i], func_cnames[i]);
        if (i < count - 1) buf_char(&e.out, '\n');
    }

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}

char *func_emit_c(Func *f) {
    return func_emit_c_named(f, 0);
}

char *vm_emit_c_globals(VM *vm, const char *type_name, const char *var_name) {
    Emit e;
    ec_emit_init(&e, vm, &EC_DIALECT_C);

    if (vm->n_globals <= 0) {
        buf_str(&e.out, "/* No shared variables. */\n");
        if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
        return e.out.buf;
    }

    // An array-typed shared variable needs its aggregate typedef, same
    // include-guarded form the per-function emitter uses -- the struct field
    // below is spelled with it, and this output is concatenated into the same
    // translation unit as those functions.
    // The struct table the shared variables were declared against, as the Func
    // every type-naming helper expects. Set by vm_declare_globals; a VM that only
    // imported the layout has none, and cannot emit a struct-typed one.
    Func gowner;
    vm->run.sys->memset(&gowner, 0, sizeof(gowner));
    gowner.structs = vm->globals_structs;
    {
        AggType *agg = (AggType *)mem_alloc(&vm->run.mem, sizeof(AggType) * MAX_AGG_TYPES);
        int n_agg = 0;
        for (int i = 0; agg && i < vm->n_globals; i++) {
            VMType t = vm->globals[i].type;
            if (!ec_is_aggregate(t.kind)) continue;
            ec_agg_add(vm, agg, &n_agg, t, &gowner);
        }
        for (int i = 0; agg && i < n_agg; i++)
            if (agg[i].type.struct_id)
                ec_agg_add_struct_fields(vm, agg, &n_agg, &gowner, agg[i].type.struct_id, 0);
        if (agg) ec_emit_typedefs(&e.out, vm, agg, n_agg, 1);
    }

    // One struct rather than loose file-scope variables: it keeps the shared state
    // visibly one thing, and -- the reason that matters -- it lets a host declare
    // several independent INSTANCES of the same layout, with each body's
    // vm_set_globals_c_prefix deciding which one it compiles against.
    buf_str(&e.out, "typedef struct {\n");
    for (int i = 0; i < vm->n_globals; i++) {
        buf_str(&e.out, "    ");
        ec_emit_type_name(&e.out, vm, &gowner, vm->globals[i].type);
        buf_char(&e.out, ' ');
        ec_put_name(&e.out, intern_get_cstr(vm->intern, vm->globals[i].name));
        buf_str(&e.out, ";\n");
    }
    buf_str(&e.out, "} ");
    buf_str(&e.out, type_name);
    buf_str(&e.out, ";\n\n");

    buf_str(&e.out, "static ");
    buf_str(&e.out, type_name);
    buf_char(&e.out, ' ');
    buf_str(&e.out, var_name);
    buf_str(&e.out, ";\n");

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}

// Prefix a helper's C name with the top-level function's emitted name so two
// independently-compiled bodies that both define a helper called "rot" do not
// collide when their emitted C lands in the same translation unit. "rot" under
// top-level "kick" becomes "kick__rot".
static const char *ec_prefixed_name(VM *vm, const char *top, const char *name) {
    int tl = ec_strlen(top), nl = ec_strlen(name);
    char *out = (char *)mem_alloc(&vm->run.mem, (size_t)(tl + 2 + nl + 1));
    int p = 0;
    for (int i = 0; i < tl; i++) out[p++] = top[i];
    out[p++] = '_'; out[p++] = '_';
    for (int i = 0; i < nl; i++) out[p++] = name[i];
    out[p] = '\0';
    return out;
}

// Mark every user-defined function reachable from list[fi] via IR_CALL,
// recursively.  reached[] is parallel to list[]; entries already marked are
// not revisited, so recursion and mutual recursion terminate.
static void ec_mark_callees(VM *vm, Func **list, int count, char *reached, int fi) {
    Func *f = list[fi];
    for (int i = 0; i < f->n_nodes; i++) {
        IRNode *n = &f->nodes[i];
        if (n->op != IR_CALL) continue;
        Func *callee = vm->run.funcs_by_id[(int)n->ki];
        if (!callee || callee->native_tok != 0) continue;
        for (int j = 0; j < count; j++) {
            if (list[j] != callee || reached[j]) continue;
            reached[j] = 1;
            ec_mark_callees(vm, list, count, reached, j);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Content-addressed helper names (see EmitCDedup / func_emit_c_named_dedup)
// ---------------------------------------------------------------------------

// FNV-1a, 64 bits. Only ever compared against other hashes produced right
// here, so nothing depends on the constants beyond their staying put across
// the calls whose output lands in one translation unit.
static unsigned long long ec_hash_bytes(const char *s, int n, unsigned long long h) {
    for (int i = 0; i < n; i++) {
        h ^= (unsigned long long)(unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Deterministic depth-first walk of list[idx]'s call closure: the function itself
// first, then everything it calls, in IR-node order. Records an ORDER rather than
// just a reached-set, because ec_closure_hash needs each member's position to be a
// property of the closure alone. Already-visited entries are not revisited, so
// recursion terminates.
static void ec_closure_dfs(VM *vm, Func **list, int count, int idx,
                           int *order, int *n_order, char *seen) {
    seen[idx] = 1;
    order[(*n_order)++] = idx;
    Func *f = list[idx];
    for (int i = 0; i < f->n_nodes; i++) {
        IRNode *n = &f->nodes[i];
        if (n->op != IR_CALL) continue;
        Func *callee = vm->run.funcs_by_id[(int)n->ki];
        if (!callee || callee->native_tok != 0) continue;
        for (int j = 0; j < count; j++) {
            if (list[j] != callee) continue;
            if (!seen[j]) ec_closure_dfs(vm, list, count, j, order, n_order, seen);
            break;
        }
    }
}

// A 64-bit fingerprint of what list[idx] *is*: the C text it emits to, with every
// function in its call closure (itself included) renamed to "__cN" for its position
// in the walk above. Positional names take the script's own identifiers -- and the
// caller's "<top>__" prefixing -- out of the picture, so the same helper compiled
// into two different VMs fingerprints identically while two template
// specialisations of it do not. Native calls are left spelled by name, which is
// what makes two helpers calling different natives fingerprint apart.
//
// Falls back to a per-Func-unique value if the scratch emit fails to allocate: a
// collision here would silently drop a definition.
static unsigned long long ec_closure_hash(VM *vm, Func **list, const char **cnames,
                                          int count, int idx) {
    int  order[MAX_EMIT_FUNCS];
    char seen [MAX_EMIT_FUNCS];
    int  n_order = 0;
    for (int i = 0; i < count; i++) seen[i] = 0;
    ec_closure_dfs(vm, list, count, idx, order, &n_order, seen);

    // Scratch copy of the cname table, with the closure's members swapped for
    // their positional names, so emit_func resolves closure-internal call
    // sites to those. Non-members keep whatever they had; nothing in the
    // closure can reference them.
    const char *scratch[MAX_EMIT_FUNCS];
    char        pos    [MAX_EMIT_FUNCS][8];
    for (int i = 0; i < count; i++) scratch[i] = cnames[i];
    for (int k = 0; k < n_order; k++) {
        int p = 0, v = k, dn = 0;
        char d[6];
        pos[k][p++] = '_'; pos[k][p++] = '_'; pos[k][p++] = 'c';
        if (v == 0) d[dn++] = '0';
        while (v > 0) { d[dn++] = (char)('0' + v % 10); v /= 10; }
        while (dn > 0) pos[k][p++] = d[--dn];
        pos[k][p] = '\0';
        scratch[order[k]] = pos[k];
    }

    Emit e;
    ec_emit_init(&e, vm, &EC_DIALECT_C);   // helpers are never stripped -- see the emit loop below
    e.func_list    = list;
    e.func_cnames  = scratch;
    e.func_count   = count;
    for (int k = 0; k < n_order; k++)
        emit_func(&e, list[order[k]], scratch[order[k]]);

    unsigned long long h = 14695981039346656037ULL;
    if (e.out.ok && e.out.buf) h = ec_hash_bytes(e.out.buf, e.out.len, h);
    else                       h ^= (unsigned long long)(size_t)list[idx];
    vm->run.sys->free(e.out.buf);
    return h;
}

// "<script name>_<16 hex>" -- keeps the generated C readable while the hash
// carries the uniqueness. Two distinct helpers that share a script name get
// distinct suffixes; two identical ones get the same full name, which is what
// lets the second be skipped.
static const char *ec_hashed_name(VM *vm, const char *base, unsigned long long h) {
    static const char hexd[] = "0123456789abcdef";
    int bl = ec_strlen(base);
    char *out = (char *)mem_alloc(&vm->run.mem, (size_t)(bl + 1 + 16 + 1));
    int p = 0;
    for (int i = 0; i < bl; i++) out[p++] = base[i];
    out[p++] = '_';
    for (int i = 15; i >= 0; i--) out[p++] = hexd[(h >> (i * 4)) & 0xf];
    out[p] = '\0';
    return out;
}

// The name `h` was first emitted under, or NULL when `h` is new -- in which case
// `name` is recorded as the one every later chunk must call it by. The returned
// pointer lives in `dd`, which outlives the emitting VM, so callers may hold it.
//
// A full table -- or a full name pool -- stops deduping rather than evicting.
// Sized well past any plausible helper count instead.
static const char *ec_dedup_name_or_add(EmitCDedup *dd, unsigned long long h,
                                        const char *name) {
    for (int i = 0; i < dd->count; i++)
        if (dd->hash[i] == h) return dd->names + dd->name_off[i];
    int n = ec_strlen(name);
    if (dd->count < EMIT_C_DEDUP_MAX && dd->names_used + n + 1 <= EMIT_C_DEDUP_NAMES) {
        dd->hash[dd->count]     = h;
        dd->name_off[dd->count] = dd->names_used;
        for (int i = 0; i <= n; i++) dd->names[dd->names_used + i] = name[i];
        dd->names_used += n + 1;
        dd->count++;
    }
    return 0;
}

static char *func_emit_c_named_impl(Func *f, const char *name_override, int strip_unused,
                                    EmitCDedup *dd) {
    VM *vm = (VM*)f->run;

    const char *top_name = name_override ? name_override
        : ((f->name != 0)
            ? intern_get_cstr(vm->intern, f->name)
            : "__script__");
    if (!name_override && f->name != 0 && ec_name_clashes(top_name)) {
        int n = es_strlen(top_name);
        char *esc = (char *)mem_alloc(&vm->run.mem, (size_t)n + 2);
        if (esc) {
            for (int k = 0; k < n; k++) esc[k] = top_name[k];
            esc[n] = '_';
            esc[n + 1] = '\0';
            top_name = esc;
        }
    }
    // `f` may call other functions defined in the same script (helper lambdas,
    // say).  Collect every emittable function in the VM
    // so ec_get_cname can resolve those call sites, then keep the ones `f`
    // actually reaches -- emitting them alongside `f` under prefixed names.
    Func       *all_funcs [MAX_EMIT_FUNCS];
    const char *all_cnames[MAX_EMIT_FUNCS];
    char        reached   [MAX_EMIT_FUNCS];
    char        already   [MAX_EMIT_FUNCS]; // reached, but a previous call sharing `dd` already defined it
    int all_count = emit_collect_funcs(vm, all_funcs, MAX_EMIT_FUNCS);
    if (all_count < 0) return 0;
    build_cnames(vm, all_funcs, all_count, all_cnames);

    int self = -1;
    for (int i = 0; i < all_count; i++) {
        reached[i] = 0;
        already[i] = 0;
        if (all_funcs[i] == f) self = i;
    }

    // `f` itself may not be in the VM's list (a caller can hand us a Func it
    // built by other means); fall back to emitting it alone in that case.
    if (self < 0) {
        all_funcs[0]  = f;
        all_cnames[0] = top_name;
        all_count = 1;
        self = 0;
        reached[0] = 0;
        already[0] = 0;
    } else {
        all_cnames[self] = top_name;
        ec_mark_callees(vm, all_funcs, all_count, reached, self);
        reached[self] = 0;                    // emitted separately, last
        for (int i = 0; i < all_count; i++) {
            if (!reached[i]) continue;
            if (dd) {
                // Content-addressed name, so an identical helper reached from
                // another top-level function lands on the same C identifier and
                // only has to be defined once. ec_closure_hash rewrites every
                // closure member's name positionally, so it does not matter that
                // this loop has already renamed some of them. The NAME comes from
                // the table too, not from this VM's own base: a skipped definition
                // must leave the call sites pointing at the copy that WAS emitted.
                unsigned long long h = ec_closure_hash(vm, all_funcs, all_cnames, all_count, i);
                const char *hashed = ec_hashed_name(vm, all_cnames[i], h);
                const char *prev   = ec_dedup_name_or_add(dd, h, hashed);
                all_cnames[i] = prev ? prev : hashed;
                already[i]    = (char)(prev != 0);
            } else {
                all_cnames[i] = ec_prefixed_name(vm, top_name, all_cnames[i]);
            }
        }
    }

    Emit e;
    ec_emit_init(&e, vm, &EC_DIALECT_C);
    e.func_list   = all_funcs;
    e.func_cnames = all_cnames;
    e.func_count  = all_count;

    // typedefs for aggregate types used by f and its helpers
    Func *emit_list[MAX_EMIT_FUNCS];
    int n_emit = 0;
    for (int i = 0; i < all_count; i++) if (reached[i]) emit_list[n_emit++] = all_funcs[i];
    emit_list[n_emit++] = f;

    AggType *agg_types = 0;
    int n_agg = collect_agg_types(vm, emit_list, n_emit, &agg_types);
    if (n_agg > 0) {
        // Include-guard each typedef: callers emit one function per call and
        // concatenate the results into a single translation unit, so two
        // functions that both use int_3 would otherwise redefine the struct --
        // two distinct anonymous struct types under one name, an error even in
        // C11.
        ec_emit_typedefs(&e.out, vm, agg_types, n_agg, 1);
        buf_char(&e.out, '\n');
        if (ec_needs_pack_helpers(agg_types, n_agg)) {
            buf_str(&e.out, EC_PACK_HELPERS);
            buf_char(&e.out, '\n');
        }
    }
    if (ec_needs_slice_helpers(emit_list, n_emit)) buf_str(&e.out, EC_SLICE_HELPERS);
    if (ec_needs_bitcast_helpers(emit_list, n_emit)) buf_str(&e.out, EC_BITCAST_HELPERS);
    ec_emit_div_helpers(&e.out, ec_div_helpers_used(vm, emit_list, n_emit));
    ec_emit_f2i_helpers(vm, &e.out, emit_list, n_emit);
    if (ec_needs_nonfinite_helpers(emit_list, n_emit)) buf_str(&e.out, EC_NONFINITE_HELPERS);
    ec_emit_fmt_helpers(&e.out, ec_fmt_kinds_used(vm, emit_list, n_emit));
    if (ec_needs_print_macro(vm, emit_list, n_emit)) buf_str(&e.out, EC_PRINT_MACRO);

    // Forward-declare the helpers so their definition order (and mutual recursion)
    // does not matter, then define them.  strip_unused stays off for helpers: it
    // only applies to the top-level signature the caller controls. A helper
    // `already` carried by an earlier chunk is still declared here -- a repeated
    // declaration is legal C and keeps this chunk self-contained -- but not defined
    // again.
    if (n_emit > 1) {
        for (int i = 0; i < all_count; i++) {
            if (!reached[i]) continue;
            emit_func_signature(&e, all_funcs[i], all_cnames[i]);
            buf_str(&e.out, ";\n");
        }
        buf_char(&e.out, '\n');
        for (int i = 0; i < all_count; i++) {
            if (!reached[i] || already[i]) continue;
            emit_func(&e, all_funcs[i], all_cnames[i]);
            buf_char(&e.out, '\n');
        }
    }

    e.strip_unused = strip_unused;
    emit_func(&e, f, top_name);

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}

char *func_emit_c_named(Func *f, const char *name_override) {
    return func_emit_c_named_impl(f, name_override, 0, 0);
}

char *func_emit_c_named_stripped(Func *f, const char *name_override) {
    return func_emit_c_named_impl(f, name_override, 1, 0);
}

char *func_emit_c_named_dedup(Func *f, const char *name_override, int strip_unused,
                              EmitCDedup *dd) {
    return func_emit_c_named_impl(f, name_override, strip_unused, dd);
}

// ============================================================================
// Dialects (vm_emit_c_dialect.h)
// ============================================================================

StrBuf *ec_out(ECEmit *e)             { return &e->out; }
VM     *ec_vm(ECEmit *e)              { return e->vm; }
Func   *ec_func(ECEmit *e)            { return e->cur_func; }
const ECDialect *ec_dialect(ECEmit *e) { return e->d; }
void    ec_expr(ECEmit *e, IRNode *n) { emit_expr(e, n); }

int vm_emit_c_funcs(VM *vm, const ECDialect *d, Func **list, const char **names,
                    int count, StrBuf *out) {
    Emit e;
    ec_emit_init(&e, vm, d);
    e.out         = *out;
    e.func_list   = list;
    e.func_cnames = names;
    e.func_count  = count;
    for (int i = 0; i < count; i++) {
        if (list[i]->name == 0 && !ec_is_called(vm, list, count, i)) continue;   // __script__
        emit_func_signature(&e, list[i], names[i]);
        buf_str(&e.out, ";\n");
    }
    buf_char(&e.out, '\n');
    for (int i = 0; i < count; i++) {
        emit_func(&e, list[i], names[i]);
        buf_char(&e.out, '\n');
    }
    *out = e.out;
    return out->ok;
}
