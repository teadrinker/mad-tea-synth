// vm_emit_curlywas.c
// Walk the VM's IR tree and generate CurlyWas source code.
//
// CurlyWas (https://github.com/exoticorn/curlywas) is a curly-braced,
// infix syntax for WebAssembly.  This emitter translates the VM's typed
// IR into CurlyWas source that can be compiled with the `curlywas` tool
// (microw8's `uw8 compile`).
//
// Pipeline:  IRNode tree  -->  vm_emit_curlywas()  -->  char* (CurlyWas source)
//
// Design notes:
//   - No libc; all allocation goes through vm->sys (malloc/realloc/free).
//   - Internal bookkeeping uses the VM's arena so it does not need to be
//     freed explicitly.
//   - The returned string is malloc'd; the caller frees it with sys->free.
//   - VM array types are not directly representable in CurlyWas (which uses
//     linear memory).  Whatever the emitter has no spelling for comes out as
//     a placeholder comment, so a program using it never compiles by accident.
//   - Nothing is exported: the caller decides which functions are entry points.

#include "vm_emit_curlywas.h"
#include "vm_emit_shared.h"
#include "parser/tokens.h"
#include "common/string_pure.h"

// ============================================================================
// Small helpers (no libc)
// ============================================================================

static int cw_is_array(VTKind k) {
    return k == VMT_ARR_I32 || k == VMT_ARR_F32 || k == VMT_ARR_F64 || k == VMT_ARR_I64;
}

static const char *cw_wasm_type(VTKind k) {
    switch (k) {
        case VMT_I32: return "i32";
        case VMT_I64: return "i64";
        case VMT_F32: return "f32";
        case VMT_F64: return "f64";
        default:      return "/*?type?*/";
    }
}

static const char *const CW_RESERVED_VAR[] = {
    "fn", "let", "loop", "block", "branch", "branch_if", "if", "else", "return",
    "select", "as", "global", "lazy", "mut", 0
};

static const char *const CW_RESERVED_FN[] = {
    "fn", "let", "loop", "block", "branch", "branch_if", "if", "else", "return",
    "select", "as", "global", "lazy", "mut",
    "sqrt", "abs", "min", "max", "floor", "ceil", "trunc", "nearest", 0
};

// Return the binary-operator CurlyWas token string for a sub_op code.
static const char *cw_binop_str(int op) {
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
// Dynamic output buffer (uses common/string_pure.h StrBuf)
// ============================================================================

typedef StrBuf CWBuf;
#define cw_buf_init(b, vm)  sb_init(b, (vm)->run.sys)
#define cw_buf_str   sb_str
#define cw_buf_char  sb_char
#define cw_buf_int   sb_int

static void cw_buf_int_lit(CWBuf *b, long long v, VTKind k) {
    if (v < 0) cw_buf_char(b, '(');
    cw_buf_int(b, v);
    if (k == VMT_I64) cw_buf_str(b, "i64");
    if (v < 0) cw_buf_char(b, ')');
}

// CurlyWas float literals are f32 only and have no exponent form.
static int cw_plain_float_text(const char *s) {
    for (int i = 0; s[i]; i++)
        if (s[i] == 'e' || s[i] == 'E') return 0;
    return 1;
}

static void cw_buf_plain_f32(CWBuf *b, const char *s) {
    int has_dot = 0;
    for (int i = 0; s[i]; i++) if (s[i] == '.') has_dot = 1;
    cw_buf_char(b, '(');
    cw_buf_str(b, s);
    if (!has_dot) cw_buf_str(b, ".0");
    cw_buf_char(b, ')');
}

// An f32 literal is its decimal when it has one, and its bit pattern otherwise.
// An f64 is an f32 literal widened when that is exact, and its bit pattern
// otherwise: CurlyWas has no f64 literal.
static void cw_buf_float_lit(CWBuf *b, double v, VTKind k) {
    char tmp[S_FROM_NUMBER_MAX_CHARS];
    int finite = (v == v) && (v - v == 0.0);
    if (k == VMT_F32) {
        union { float f; int i; } u;
        u.f = (float)v;
        if (finite && u.i != (int)0x80000000) {
            s_from_number_flags((double)u.f, tmp, S_FROM_NUMBER_FLAG_SHORTEST);
            if (cw_plain_float_text(tmp)) { cw_buf_plain_f32(b, tmp); return; }
        }
        cw_buf_str(b, "f32.reinterpret_i32(");
        cw_buf_int_lit(b, (long long)u.i, VMT_I32);
        cw_buf_char(b, ')');
        return;
    }
    union { double d; long long i; } u;
    u.d = v;
    if (u.i == 0) { cw_buf_str(b, "(0.0 as f64)"); return; }
    if (finite && (double)(float)v == v) {
        s_from_number_flags(v, tmp, S_FROM_NUMBER_FLAG_SHORTEST);
        if (cw_plain_float_text(tmp)) {
            cw_buf_char(b, '(');
            cw_buf_plain_f32(b, tmp);
            cw_buf_str(b, " as f64)");
            return;
        }
    }
    cw_buf_str(b, "f64.reinterpret_i64(");
    cw_buf_int_lit(b, u.i, VMT_I64);
    cw_buf_char(b, ')');
}

// ============================================================================
// Guarded division, as the interpreter's eval_binop takes OP_DIV / OP_MOD: a zero
// divisor gives 0, and -1 negates rather than trapping on INT_MIN / -1.
// ============================================================================

enum { DH_DIV_I32, DH_MOD_I32, DH_DIV_I64, DH_MOD_I64, DH_DIV_F32, DH_DIV_F64, DH_COUNT };

static const char *CW_DIV_FN[DH_COUNT] = {
    "vm_div_i32", "vm_mod_i32", "vm_div_i64", "vm_mod_i64", "vm_div_f32", "vm_div_f64",
};

static const char *CW_DIV_DEF[DH_COUNT] = {
"fn vm_div_i32(a: i32, b: i32) -> i32 {\n  if (b == 0) { 0 } else { if (b == -1) { 0 - a } else { a / b } }\n}\n",
"fn vm_mod_i32(a: i32, b: i32) -> i32 {\n  if (b == 0) { 0 } else { if (b == -1) { 0 } else { a % b } }\n}\n",
"fn vm_div_i64(a: i64, b: i64) -> i64 {\n  if (b == 0i64) { 0i64 } else { if (b == -1i64) { 0i64 - a } else { a / b } }\n}\n",
"fn vm_mod_i64(a: i64, b: i64) -> i64 {\n  if (b == 0i64) { 0i64 } else { if (b == -1i64) { 0i64 } else { a % b } }\n}\n",
"fn vm_div_f32(a: f32, b: f32) -> f32 {\n  if (b == 0.0) { 0.0 } else { a / b }\n}\n",
"fn vm_div_f64(a: f64, b: f64) -> f64 {\n  if (b == (0.0 as f64)) { (0.0 as f64) } else { a / b }\n}\n",
};

// The CW_DIV_FN a node calls, or -1 when it emits inline: not a guarded op, or
// a literal divisor that is neither zero nor -1.
static int cw_div_helper(Func *f, IRNode *n) {
    if (n->op != IR_BINOP || (n->sub_op != OP_DIV && n->sub_op != OP_MOD)) return -1;
    IRNode *d = ir_child(f, n->b);
    if (d && d->op == IR_CONST_I && d->ki != 0 && d->ki != -1) return -1;
    if (d && d->op == IR_CONST_F && d->kf != 0.0) return -1;
    int mod = (n->sub_op == OP_MOD);
    switch (n->type.kind) {
        case VMT_I32: return mod ? DH_MOD_I32 : DH_DIV_I32;
        case VMT_I64: return mod ? DH_MOD_I64 : DH_DIV_I64;
        case VMT_F32: return mod ? -1 : DH_DIV_F32;
        case VMT_F64: return mod ? -1 : DH_DIV_F64;
        default:      return -1;
    }
}

static void cw_emit_div_helpers(CWBuf *b, Func **list, int count) {
    unsigned used = 0;
    for (int fi = 0; fi < count; fi++)
        for (int ni = 0; ni < list[fi]->n_nodes; ni++) {
            int h = cw_div_helper(list[fi], &list[fi]->nodes[ni]);
            if (h >= 0) used |= 1u << h;
        }
    for (int i = 0; i < DH_COUNT; i++) {
        if (!(used & (1u << i))) continue;
        cw_buf_str(b, CW_DIV_DEF[i]);
        cw_buf_char(b, '\n');
    }
}

// ============================================================================
// Emitter state
// ============================================================================

#define CW_MAX_LOOPS 64

typedef struct {
    CWBuf       out;
    VM         *vm;
    Func       *cur_func;       // function currently being emitted
    int         indent;         // current indentation level (2 spaces each)
    Func      **func_list;      // collected function pointers
    const char **func_cnames;   // CWA names parallel to func_list
    int         func_count;
    const char **local_names;   // indexed by slot, for cur_func
    int         n_local_names;
    int         loop_n;         // loops numbered so far in cur_func
    int         depth;          // loops open around the node being emitted
    int         loop_id[CW_MAX_LOOPS];
} CWEmitt;

static void cw_emit_indent(CWEmitt *e) {
    for (int i = 0; i < e->indent * 2; i++) cw_buf_char(&e->out, ' ');
}

// Return the pre-computed CWA name for a Func* (linear scan; table is small).
static const char *cw_get_cname(CWEmitt *e, Func *f) {
    for (int i = 0; i < e->func_count; i++)
        if (e->func_list[i] == f) return e->func_cnames[i];
    return "?";
}

// Return 1 if sym index `idx` is one of the declared parameters of f.
static int cw_is_param(Func *f, int idx) {
    for (int i = 0; i < f->n_params; i++)
        if (f->param_slot[i] == idx) return 1;
    return 0;
}

// One name per slot, escaped against CurlyWas's keywords and against the slots
// named before it, so two slots spelled alike never share a variable.
static void cw_build_local_names(CWEmitt *e, Func *f) {
    VM *vm = e->vm;
    const char **names = (const char **)mem_alloc(&vm->run.mem, sizeof(char *) * (size_t)(f->n_syms + 1));
    e->local_names = names;
    e->n_local_names = names ? f->n_syms : 0;
    if (!names) return;
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        if (s->name != 0) {
            names[i] = es_escape_name(vm, intern_get_cstr(vm->intern, s->name), CW_RESERVED_VAR, names, i);
            continue;
        }
        char *out = (char *)mem_alloc(&vm->run.mem, 16);
        if (!out) { names[i] = "__v"; continue; }
        int p = 0, v = i, dn = 0;
        char dg[8];
        out[p++] = '_'; out[p++] = '_'; out[p++] = 'v';
        if (v == 0) dg[dn++] = '0';
        while (v > 0) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
        while (dn > 0) out[p++] = dg[--dn];
        out[p] = '\0';
        names[i] = out;
    }
}

static void cw_emit_local_name(CWEmitt *e, int slot) {
    if (slot < 0 || slot >= e->n_local_names) { cw_buf_str(&e->out, "__bad"); return; }
    cw_buf_str(&e->out, e->local_names[slot]);
}

// ============================================================================
// Expression emitter
// ============================================================================

// Forward declaration.
static void cw_emit_expr(CWEmitt *e, IRNode *n);

// ============================================================================
// Host buffers placed in linear memory (vm_place_host_buffer)
// ============================================================================

// The load/store operator for one element, and its size: u8 `?`, i32 `!`, f32 `$`.
static char cw_buf_access(VMType t, int *size) {
    *size = 4;
    if (t.kind == VMT_SLICE_I32 && t.pack_bits == 8) { *size = 1; return '?'; }
    if (t.kind == VMT_SLICE_I32 && t.pack_bits == 0) return '!';
    if (t.kind == VMT_SLICE_F32 && t.pack_bits == 0) return '$';
    return 0;
}

// The host-buffer id `base` reads, when that buffer has a place in memory and an
// element CurlyWas can load; -1 otherwise.
static int cw_placed_buf(CWEmitt *e, IRNode *base) {
    if (!base || base->op != IR_LOCAL) return -1;
    VMSym *s = &e->cur_func->syms[(int)base->ki];
    if (!s->is_host_buf) return -1;
    int id = s->offset / (int)sizeof(VMHostBufSlot), size;
    if (id < 0 || id >= VM_MAX_HOST_BUFS) return -1;
    if (!e->vm->host_bufs[id].declared || !e->vm->host_bufs[id].mem_placed) return -1;
    return cw_buf_access(s->type, &size) ? id : -1;
}

// `(i)?addr` / `(i * 4)!addr`: the index is the base, the buffer's address the offset.
static void cw_emit_buf_elem(CWEmitt *e, int id, IRNode *idx) {
    int size;
    char op = cw_buf_access(e->vm->host_bufs[id].type, &size);
    cw_buf_char(&e->out, '(');
    cw_emit_expr(e, idx);
    if (size != 1) { cw_buf_str(&e->out, " * "); cw_buf_int(&e->out, size); }
    cw_buf_char(&e->out, ')');
    cw_buf_char(&e->out, op);
    unsigned addr = (unsigned)e->vm->host_bufs[id].mem_addr;
    char hex[12];
    int n = 0;
    do { hex[n++] = "0123456789abcdef"[addr & 15]; addr >>= 4; } while (addr);
    cw_buf_str(&e->out, "0x");
    while (n > 0) cw_buf_char(&e->out, hex[--n]);
}

// Emit `n` as an i32 truth value: an i32 as it stands, anything else != 0.
static void cw_emit_cond(CWEmitt *e, IRNode *n) {
    VTKind k = n->type.kind;
    if (k != VMT_I64 && k != VMT_F32 && k != VMT_F64) { cw_emit_expr(e, n); return; }
    cw_buf_char(&e->out, '(');
    cw_emit_expr(e, n);
    cw_buf_str(&e->out, " != ");
    if (k == VMT_I64) cw_buf_str(&e->out, "0i64");
    else              cw_buf_float_lit(&e->out, 0.0, k);
    cw_buf_char(&e->out, ')');
}

// Emit a call to a native (registered C) function.
// CurlyWas doesn't have native C functions; we emit the name and args.
static void cw_emit_native_call(CWEmitt *e, Func *callee, IRNode *n) {
    int idx = callee->native_tok - TOK_MATHS_FIRST;
    CFuncEntry *ent = &e->vm->run.cfunc_table[idx];
    VTKind rk = n->type.kind;

    // Pick the variant name. For i32, n->sub_op records which slot the
    // compiler chose (1 = fixed-point fx_name, 0 = raw-int i32_name) -- see
    // CFuncEntry in vm_types.h.
    const char *fname = 0;
    if      (rk == VMT_F32 && ent->f32_name) fname = ent->f32_name;
    else if (rk == VMT_F64 && ent->f64_name) fname = ent->f64_name;
    else if (rk == VMT_I32) fname = n->sub_op ? ent->fx_name : ent->i32_name;
    if (!fname) fname = intern_get_cstr(e->vm->intern, callee->name);

    cw_buf_str(&e->out, fname);
    cw_buf_char(&e->out, '(');
    for (int i = 0; i < n->n_items; i++) {
        if (i) cw_buf_str(&e->out, ", ");
        cw_emit_expr(e, ir_item(e->cur_func, n, i));
    }
    cw_buf_char(&e->out, ')');
}

// Forward declaration for statement emitter.
static void cw_emit_stmt(CWEmitt *e, IRNode *n);

static void cw_emit_binop(CWEmitt *e, IRNode *n) {
    Func *f = e->cur_func;
    IRNode *a = ir_child(f, n->a);
    IRNode *b = ir_child(f, n->b);

    if (n->sub_op == OP_AND || n->sub_op == OP_OR) {
        int is_and = (n->sub_op == OP_AND);
        cw_buf_str(&e->out, "(if (");
        cw_emit_cond(e, a);
        cw_buf_str(&e->out, is_and ? ") { (" : ") { 1 } else { (");
        if (is_and) {
            cw_emit_cond(e, b);
            cw_buf_str(&e->out, " != 0) } else { 0 })");
        } else {
            cw_emit_cond(e, b);
            cw_buf_str(&e->out, " != 0) })");
        }
        return;
    }

    int dh = cw_div_helper(f, n);
    if (dh >= 0) {
        cw_buf_str(&e->out, CW_DIV_FN[dh]);
        cw_buf_char(&e->out, '(');
        cw_emit_expr(e, a);
        cw_buf_str(&e->out, ", ");
        cw_emit_expr(e, b);
        cw_buf_char(&e->out, ')');
        return;
    }

    cw_buf_char(&e->out, '(');
    cw_emit_expr(e, a);
    cw_buf_str(&e->out, cw_binop_str(n->sub_op));
    // A 64-bit shift takes a 64-bit count in wasm; the VM's count is any integer.
    int wide_shift = n->type.kind == VMT_I64
                  && (n->sub_op == OP_BSHL || n->sub_op == OP_BSHL_NATIVE || n->sub_op == OP_BSHR)
                  && b->type.kind == VMT_I32;
    if (wide_shift) cw_buf_char(&e->out, '(');
    cw_emit_expr(e, b);
    if (wide_shift) cw_buf_str(&e->out, " as i64)");
    cw_buf_char(&e->out, ')');
}

static void cw_emit_expr(CWEmitt *e, IRNode *n) {
    if (!n) { cw_buf_str(&e->out, "/*null*/"); return; }

    switch (n->op) {

        // ---- constants -------------------------------------------------------
        case IR_CONST_I:
            cw_buf_int_lit(&e->out, n->ki, n->type.kind);
            break;

        case IR_CONST_F:
            cw_buf_float_lit(&e->out, n->kf, n->type.kind);
            break;

        // ---- locals / indexing -----------------------------------------------
        case IR_LOCAL:
            cw_emit_local_name(e, (int)n->ki);
            break;

        case IR_INDEX: {
            // Array index: name[index]
            // CurlyWas uses linear memory for arrays, not typed arrays.
            // Emit as a memory access comment for now.
            IRNode *base = ir_child(e->cur_func, n->a);
            int hb = cw_placed_buf(e, base);
            if (hb >= 0) {
                cw_buf_char(&e->out, '(');
                cw_emit_buf_elem(e, hb, ir_child(e->cur_func, n->b));
                cw_buf_char(&e->out, ')');
                break;
            }
            cw_buf_str(&e->out, "/* array: ");
            if (base->op == IR_LOCAL) {
                cw_emit_local_name(e, (int)base->ki);
            } else {
                cw_buf_str(&e->out, "(");
                cw_emit_expr(e, base);
                cw_buf_str(&e->out, ")");
            }
            cw_buf_char(&e->out, '[');
            cw_emit_expr(e, ir_child(e->cur_func, n->b));
            cw_buf_str(&e->out, "] */ 0");
            break;
        }

        case IR_LEN: {
            int hb = cw_placed_buf(e, ir_child(e->cur_func, n->a));
            if (hb >= 0 && n->ki <= 0) cw_buf_int(&e->out, e->vm->host_bufs[hb].mem_len);
            else                       cw_buf_str(&e->out, "/*?len?*/");
            break;
        }

        // Struct records are arrays underneath, and arrays
        // are not implemented here, so a field gets the same placeholder an element
        // does.
        case IR_FIELD:
            cw_buf_str(&e->out, "/* struct field */ 0");
            break;

        // ---- conversion ------------------------------------------------------
        // A float to an integer saturates, nan giving 0, as the VM does it, unless
        // the body asked for the host's own cast.
        case IR_CVT: {
            IRNode *a = ir_child(e->cur_func, n->a);
            VTKind from = a->type.kind, to = n->type.kind;
            if (from == to) { cw_emit_expr(e, a); break; }
            int f2i = (from == VMT_F32 || from == VMT_F64) && (to == VMT_I32 || to == VMT_I64);
            if (f2i && !(emit_func_flags(e->vm, e->cur_func) & VM_FLAG_C_FLOAT_TO_INT)) {
                cw_buf_str(&e->out, to == VMT_I64 ? "i64.trunc_sat_" : "i32.trunc_sat_");
                cw_buf_str(&e->out, from == VMT_F64 ? "f64_s(" : "f32_s(");
                cw_emit_expr(e, a);
                cw_buf_char(&e->out, ')');
                break;
            }
            cw_buf_char(&e->out, '(');
            cw_emit_expr(e, a);
            cw_buf_str(&e->out, " as ");
            cw_buf_str(&e->out, cw_wasm_type(to));
            cw_buf_char(&e->out, ')');
            break;
        }

#if VM_HAS_INSPECT
        // Editor-only: renders as its operand alone (see vm_emit_c.c).
        case IR_INSPECT:
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            break;
#endif

        // ---- bit-preserving reinterpretation ---------------------------------
        case IR_BITCAST: {
            VTKind from = (VTKind)n->sub_op, to = n->type.kind;
            // Same kind means i32 <-> fxN: the shift only ever existed at
            // compile time, so the operand is already the result.
            if (from == to) {
                cw_buf_char(&e->out, '(');
                cw_emit_expr(e, ir_child(e->cur_func, n->a));
                cw_buf_char(&e->out, ')');
                break;
            }
            int ok = (from == VMT_I32 && to == VMT_F32) || (from == VMT_F32 && to == VMT_I32)
                  || (from == VMT_I64 && to == VMT_F64) || (from == VMT_F64 && to == VMT_I64);
            if (!ok) { cw_buf_str(&e->out, "/*?bitcast unsupported?*/"); break; }
            cw_buf_str(&e->out, cw_wasm_type(to));
            cw_buf_str(&e->out, ".reinterpret_");
            cw_buf_str(&e->out, cw_wasm_type(from));
            cw_buf_char(&e->out, '(');
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_char(&e->out, ')');
            break;
        }

        // ---- unary ops -------------------------------------------------------
        case IR_UNOP:
            if (n->sub_op == OP_NEG) {
                cw_buf_str(&e->out, "(-(");
                cw_emit_expr(e, ir_child(e->cur_func, n->a));
                cw_buf_str(&e->out, "))");
            } else { // OP_NOT
                cw_buf_str(&e->out, "(!(");
                cw_emit_cond(e, ir_child(e->cur_func, n->a));
                cw_buf_str(&e->out, "))");
            }
            break;

        // ---- binary ops ------------------------------------------------------
        case IR_BINOP:
            cw_emit_binop(e, n);
            break;

        // Lazy, as ?: is in the VM: wasm's select would run both arms.
        case IR_SELECT:
            cw_buf_str(&e->out, "(if (");
            cw_emit_cond(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ") { ");
            cw_emit_expr(e, ir_child(e->cur_func, n->b));
            cw_buf_str(&e->out, " } else { ");
            cw_emit_expr(e, ir_child(e->cur_func, n->c));
            cw_buf_str(&e->out, " })");
            break;

        // ---- comma / sequence expression -------------------------------------
        // Emitted as an inline block whose last expression is the value:
        // { t = x; t = t * t; t * t }. Leading items are IR_ASSIGN staging steps.
        case IR_COMMA: {
            cw_buf_str(&e->out, "{ ");
            for (int i = 0; i < n->n_items; i++) {
                IRNode *it = ir_item(e->cur_func, n, i);
                int last = (i == n->n_items - 1);
                if (it->op == IR_ASSIGN) {
                    IRNode *lv = ir_child(e->cur_func, it->a);
                    int hb = lv->op == IR_INDEX ? cw_placed_buf(e, ir_child(e->cur_func, lv->a)) : -1;
                    if (lv->op == IR_LOCAL) {
                        cw_emit_local_name(e, (int)lv->ki);
                        cw_buf_str(&e->out, " = ");
                        cw_emit_expr(e, ir_child(e->cur_func, it->b));
                    } else if (hb >= 0) {
                        cw_emit_buf_elem(e, hb, ir_child(e->cur_func, lv->b));
                        cw_buf_str(&e->out, " = ");
                        cw_emit_expr(e, ir_child(e->cur_func, it->b));
                    } else {
                        // Non-local store, e.g. `a[i] += 1` used as a value.
                        // This backend has no arrays at all (reads degrade to
                        // "/* array: ... */ 0"), so degrade the store the same
                        // way instead of emitting a bogus local name.
                        cw_buf_str(&e->out, "/* unsupported store */ 0");
                    }
                    cw_buf_str(&e->out, "; ");
                } else {
                    cw_emit_expr(e, it);
                    if (!last) cw_buf_str(&e->out, "; ");
                }
            }
            cw_buf_str(&e->out, " }");
            break;
        }

        // ---- function calls --------------------------------------------------
        case IR_CALL: {
            Func *callee = e->vm->run.funcs_by_id[(int)n->ki];
            if (callee->native_tok != 0) {
                cw_emit_native_call(e, callee, n);
            } else {
                cw_buf_str(&e->out, cw_get_cname(e, callee));
                cw_buf_char(&e->out, '(');
                for (int i = 0; i < n->n_items; i++) {
                    if (i) cw_buf_str(&e->out, ", ");
                    cw_emit_expr(e, ir_item(e->cur_func, n, i));
                }
                cw_buf_char(&e->out, ')');
            }
            break;
        }

        default:
            cw_buf_str(&e->out, "/*?expr?*/");
            break;
    }
}

// ============================================================================
// Statement emitter
// ============================================================================

// Forward declaration with is_last parameter.
static void cw_emit_stmt_last(CWEmitt *e, IRNode *n, int is_last);

// Emit an item as the last element in a block (is_last=1 means no semicolon
// needed if it's a return-with-value -- it becomes the block's implicit return).
// In CurlyWas the last expression in a block is the return value and has no
// semicolon; early returns use the `return` keyword explicitly.
static void cw_emit_stmt(CWEmitt *e, IRNode *n) {
    cw_emit_stmt_last(e, n, 0);
}

static void cw_emit_block_items(CWEmitt *e, IRNode *blk, int is_last) {
    if (!blk) return;
    for (int i = 0; i < blk->n_items; i++)
        cw_emit_stmt_last(e, ir_item(e->cur_func, blk, i), (i == blk->n_items - 1) ? is_last : 0);
}

// block brk_N { loop top_N { branch_if !cond: brk_N; block cont_N { body } step; branch top_N; } }
// `continue` leaves cont_N, so the step still runs; `break` leaves brk_N.
static void cw_emit_loop(CWEmitt *e, IRNode *n) {
    if (e->depth >= CW_MAX_LOOPS) {
        cw_emit_indent(e); cw_buf_str(&e->out, "/* loops nested too deep */\n");
        return;
    }
    int id = ++e->loop_n;
    e->loop_id[e->depth++] = id;

    cw_emit_indent(e); cw_buf_str(&e->out, "block brk_"); cw_buf_int(&e->out, id); cw_buf_str(&e->out, " {\n");
    e->indent++;
    cw_emit_indent(e); cw_buf_str(&e->out, "loop top_"); cw_buf_int(&e->out, id); cw_buf_str(&e->out, " {\n");
    e->indent++;
    if (n->a >= 0) {
        cw_emit_indent(e); cw_buf_str(&e->out, "branch_if (!(");
        cw_emit_cond(e, ir_child(e->cur_func, n->a));
        cw_buf_str(&e->out, ")): brk_"); cw_buf_int(&e->out, id); cw_buf_str(&e->out, ";\n");
    }
    cw_emit_indent(e); cw_buf_str(&e->out, "block cont_"); cw_buf_int(&e->out, id); cw_buf_str(&e->out, " {\n");
    e->indent++;
    cw_emit_block_items(e, ir_child(e->cur_func, n->b), 0);
    e->indent--;
    cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
    if (n->op == IR_FOR && n->c >= 0) cw_emit_block_items(e, ir_child(e->cur_func, n->c), 0);
    cw_emit_indent(e); cw_buf_str(&e->out, "branch top_"); cw_buf_int(&e->out, id); cw_buf_str(&e->out, ";\n");
    e->indent--;
    cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
    e->indent--;
    cw_emit_indent(e); cw_buf_str(&e->out, "}\n");

    e->depth--;
}

static void cw_emit_stmt_last(CWEmitt *e, IRNode *n, int is_last) {
    if (!n) return;

    switch (n->op) {

        // ---- block -----------------------------------------------------------
        case IR_BLOCK:
            cw_emit_indent(e); cw_buf_str(&e->out, "{\n");
            e->indent++;
            cw_emit_block_items(e, n, is_last);
            e->indent--;
            cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
            break;

        // ---- if / else -------------------------------------------------------
        // Both arms carry the value when this is the function's tail; an if with
        // no else is a statement and returns early with `return`.
        case IR_IF: {
            int has_else = n->c >= 0;
            cw_emit_indent(e); cw_buf_str(&e->out, "if (");
            cw_emit_cond(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ") {\n");
            e->indent++;
            cw_emit_block_items(e, ir_child(e->cur_func, n->b), is_last && has_else);
            e->indent--;
            cw_emit_indent(e); cw_buf_char(&e->out, '}');
            if (has_else) {
                cw_buf_str(&e->out, " else {\n");
                e->indent++;
                cw_emit_block_items(e, ir_child(e->cur_func, n->c), is_last);
                e->indent--;
                cw_emit_indent(e); cw_buf_char(&e->out, '}');
            }
            cw_buf_char(&e->out, '\n');
            break;
        }

        // ---- loops -----------------------------------------------------------
        case IR_WHILE:
        case IR_FOR:
            cw_emit_loop(e, n);
            break;

        // ---- loop controls ---------------------------------------------------
        case IR_BREAK:
        case IR_CONTINUE:
            cw_emit_indent(e);
            if (e->depth == 0) {
                cw_buf_str(&e->out, "/* break or continue outside a loop */\n");
                break;
            }
            cw_buf_str(&e->out, n->op == IR_BREAK ? "branch brk_" : "branch cont_");
            cw_buf_int(&e->out, e->loop_id[e->depth - 1]);
            cw_buf_str(&e->out, ";\n");
            break;

        // ---- return ----------------------------------------------------------
        case IR_RETURN:
            if (n->a >= 0) {
                if (is_last) {
                    // Last expression in block = implicit return, no `return` keyword,
                    // no semicolon.
                    cw_emit_indent(e);
                    cw_emit_expr(e, ir_child(e->cur_func, n->a));
                    cw_buf_str(&e->out, "\n");
                } else {
                    // Early return -- use explicit `return` keyword
                    cw_emit_indent(e); cw_buf_str(&e->out, "return ");
                    cw_emit_expr(e, ir_child(e->cur_func, n->a));
                    cw_buf_str(&e->out, ";\n");
                }
            } else {
                // Void return
                if (!is_last) {
                    cw_emit_indent(e); cw_buf_str(&e->out, "return;\n");
                }
                // At end of void function: emit nothing
            }
            break;

        // ---- assignment ------------------------------------------------------
        case IR_ASSIGN: {
            IRNode *lv = ir_child(e->cur_func, n->a);
            IRNode *rv = ir_child(e->cur_func, n->b);

            if (lv->op == IR_FIELD) {
                cw_emit_indent(e);
                cw_buf_str(&e->out, "/* struct field set */\n");
            } else if (rv->op == IR_ARR_LIT) {
                // Array literal initialization: comment it out
                cw_emit_indent(e);
                cw_buf_str(&e->out, "/* array init: ");
                cw_emit_local_name(e, (int)lv->ki);
                cw_buf_str(&e->out, " = {");
                for (int i = 0; i < rv->n_items; i++) {
                    if (i) cw_buf_str(&e->out, ", ");
                    cw_emit_expr(e, ir_item(e->cur_func, rv, i));
                }
                cw_buf_str(&e->out, "} */\n");
            } else if (lv->op == IR_INDEX && cw_placed_buf(e, ir_child(e->cur_func, lv->a)) >= 0) {
                cw_emit_indent(e);
                cw_emit_buf_elem(e, cw_placed_buf(e, ir_child(e->cur_func, lv->a)), ir_child(e->cur_func, lv->b));
                cw_buf_str(&e->out, " = ");
                cw_emit_expr(e, rv);
                cw_buf_str(&e->out, ";\n");
            } else if (lv->op == IR_INDEX) {
                // Index assignment: comment it out
                IRNode *ilv_base = ir_child(e->cur_func, lv->a);
                cw_emit_indent(e);
                cw_buf_str(&e->out, "/* arr set: ");
                if (ilv_base->op == IR_LOCAL) {
                    cw_emit_local_name(e, (int)ilv_base->ki);
                } else {
                    cw_emit_expr(e, ilv_base);
                }
                cw_buf_char(&e->out, '[');
                cw_emit_expr(e, ir_child(e->cur_func, lv->b));
                cw_buf_str(&e->out, "] = ");
                cw_emit_expr(e, rv);
                cw_buf_str(&e->out, " */\n");
            } else {
                cw_emit_indent(e);
                cw_emit_local_name(e, (int)lv->ki);
                cw_buf_str(&e->out, " = ");
                cw_emit_expr(e, rv);
                // Assignments always have semicolon (even as last statement)
                cw_buf_str(&e->out, ";\n");
            }
            break;
        }

        // ---- expression statement (call result discarded) --------------------
        case IR_EXPR_STMT:
        case IR_CALL_STMT: {
            IRNode *v = ir_child(e->cur_func, n->a);
            // print() is a void expression and arrives wrapped here.
            if (v && v->op == IR_PRINT) { cw_emit_stmt_last(e, v, is_last); break; }
            cw_emit_indent(e);
            cw_emit_expr(e, v);
            cw_buf_str(&e->out, ";\n");
            break;
        }

        // ---- print -----------------------------------------------------------
        // Always dropped, with no VM_PRINT_EMIT_CALL escape: CurlyWas has no
        // array or slice support at all, so there is no way to hand the
        // payload over even if a target wanted it.
        case IR_PRINT:
            cw_emit_indent(e); cw_buf_str(&e->out, "// print() elided\n");
            break;

        default:
            cw_emit_indent(e); cw_buf_str(&e->out, "/* unknown stmt */\n");
            break;
    }
}

// ============================================================================
// Function emitter
// ============================================================================

// Emit the function signature (name + parameter list, no body).
static void cw_emit_func_signature(CWEmitt *e, Func *f, const char *cname) {
    cw_buf_str(&e->out, cname);
    cw_buf_char(&e->out, '(');

    for (int i = 0; i < f->n_params; i++) {
        if (i) cw_buf_str(&e->out, ", ");
        cw_emit_local_name(e, f->param_slot[i]);
        cw_buf_str(&e->out, ": ");
        cw_buf_str(&e->out, cw_wasm_type(f->syms[f->param_slot[i]].type.kind));
    }
    cw_buf_char(&e->out, ')');
}

// Emit a complete function definition.
static void cw_emit_func(CWEmitt *e, Func *f, const char *cname) {
    Func *saved_func = e->cur_func;
    const char **saved_names = e->local_names;
    int saved_n_names = e->n_local_names;
    e->cur_func   = f;
    e->indent     = 0;
    e->loop_n     = 0;
    e->depth      = 0;
    cw_build_local_names(e, f);

    cw_buf_str(&e->out, "fn ");
    cw_emit_func_signature(e, f, cname);
    if (f->ret_type.kind != VMT_VOID) {
        cw_buf_str(&e->out, " -> ");
        cw_buf_str(&e->out, cw_wasm_type(f->ret_type.kind));
    }
    cw_buf_str(&e->out, " {\n");
    e->indent = 1;

    // ---- local variable declarations ----------------------------------------
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        if (s->is_func)             continue;
        if (s->is_host_buf)         continue;
        if (cw_is_param(f, i))      continue;
        if (s->type.kind == VMT_VOID) continue;

        if (cw_is_array(s->type.kind)) {
            // Arrays not natively supported in CurlyWas; emit as comment
            cw_emit_indent(e);
            cw_buf_str(&e->out, "/* let ");
            cw_emit_local_name(e, i);
            cw_buf_str(&e->out, ": ");
            cw_buf_str(&e->out, cw_wasm_type(s->type.kind));
            cw_buf_str(&e->out, "[");  // type info
            cw_buf_int(&e->out, (long long)s->type.len);
            cw_buf_str(&e->out, "] = ... (array, not representable in CWA) */\n");
        } else {
            cw_emit_indent(e);
            cw_buf_str(&e->out, "let ");
            cw_emit_local_name(e, i);
            cw_buf_str(&e->out, ": ");
            cw_buf_str(&e->out, cw_wasm_type(s->type.kind));
            cw_buf_str(&e->out, ";\n");
        }
    }

    // ---- body (last item is the implicit return in CurlyWas) ---------------
    if (f->body >= 0) {
        IRNode *body = &f->nodes[f->body];
        int n = body->n_items;
        for (int i = 0; i < n; i++)
            cw_emit_stmt_last(e, ir_item(f, body, i), (i == n - 1) ? 1 : 0);
    }

    cw_buf_str(&e->out, "}\n");
    e->cur_func = saved_func;
    e->local_names = saved_names;
    e->n_local_names = saved_n_names;
}

// ============================================================================
// Public API
// ============================================================================

static void cw_emit_prelude(CWEmitt *e, Func **list, int count) {
    cw_buf_str(&e->out, "import \"env.memory\" memory(4);\n\n");
    cw_emit_div_helpers(&e->out, list, count);
}

char *vm_emit_curlywas(VM *vm) {
    // Collect emittable functions.
    Func       *func_list [MAX_EMIT_FUNCS];
    const char *func_cnames[MAX_EMIT_FUNCS];

    int count = emit_collect_funcs(vm, func_list, MAX_EMIT_FUNCS);
    if (count < 0) return 0;
    emit_build_cnames(vm, func_list, count, func_cnames, CW_RESERVED_FN);

    CWEmitt e;
    e.vm          = vm;
    e.cur_func    = 0;
    e.indent      = 0;
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = count;
    e.local_names = 0;
    e.n_local_names = 0;
    e.loop_n      = 0;
    e.depth       = 0;
    cw_buf_init(&e.out, vm);

    cw_emit_prelude(&e, func_list, count);

    if (count == 0) {
        cw_buf_str(&e.out, "// No user-defined functions to emit.\n");
        if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
        return e.out.buf;
    }

    // Full definitions.
    for (int i = 0; i < count; i++) {
        cw_emit_func(&e, func_list[i], func_cnames[i]);
        if (i < count - 1) cw_buf_char(&e.out, '\n');
    }

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}

char *func_emit_curlywas(Func *f) {
    VM *vm = (VM*)f->run;

    Func       *func_list [1];
    const char *func_cnames[1];
    func_list[0] = f;
    func_cnames[0] = es_escape_name(vm, (f->name != 0) ? intern_get_cstr(vm->intern, f->name) : "__script__",
                                    CW_RESERVED_FN, 0, 0);

    CWEmitt e;
    e.vm          = vm;
    e.cur_func    = 0;
    e.indent      = 0;
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = 1;
    e.local_names = 0;
    e.n_local_names = 0;
    e.loop_n      = 0;
    e.depth       = 0;
    cw_buf_init(&e.out, vm);

    cw_emit_prelude(&e, func_list, 1);
    cw_emit_func(&e, f, func_cnames[0]);

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}
