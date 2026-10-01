// vm_emit_script.c
// One IR walker, two script dialects: JavaScript and Lua 5.5.
//
// Pipeline:  IRNode tree  -->  vm_emit_js() / vm_emit_lua()  -->  char* (source)
//
// The front end (vm.c) has already resolved every type by the time this sees
// anything, so what the two targets need is not a token table: it is code for
// the handful of places where their arithmetic disagrees with C's. Those are
// the ScriptDialect hooks. Everything else -- control flow, calls, the shape
// of a function, the aggregate copy rule -- is written once and shared.
//
// Five things are worth knowing before editing this file:
//
//   1. `&&`, `||`, `!` and the comparisons are NOT string substitutions. C and
//      the interpreter yield 0/1 from them and treat any nonzero as true; JS
//      and Lua yield an operand or a boolean, and Lua thinks 0 is TRUE. Every
//      such node therefore goes through emit_cond (boolean position, emitted
//      bare) or emit_expr (value position, wrapped in boolify). See
//      ss_emit_logical / ss_truthy.
//   2. f32 is a real, separate type that rounds at EVERY node. JS gets that
//      from Math.fround and Float32Array; Lua has no single precision at all
//      and refuses, pointing at `#rewire f32 -> f64`.
//   3. Lua has neither the comma operator nor a ternary, and neither
//      workaround is correct -- `(c and t or f)` fails on a false/nil true
//      arm, and a VM.sel() helper evaluates BOTH arms, which the div-by-zero
//      guard cannot survive. So IR_SELECT and IR_COMMA hoist to statements,
//      which is why expression emission can write into `e->pre` and every
//      statement flushes it first.
//   4. A fixed array is a VALUE in the VM and in C, and a reference here.
//      The copies go back in at exactly three sites, all keyed on the
//      DESTINATION type -- see ss_copy_wrap's callers.
//   5. Locals are escaped against the target's keyword list AND against each
//      other. `end` is a legal DSL identifier and a Lua keyword; a script
//      declaring both `end` and `end_` must not end up with two locals called
//      `end_`.

#include "vm_emit_script.h"
#include "vm_emit_shared.h"
#include "vm_emit_script_prelude.h"
#include "parser/tokens.h"
#include "common/string_pure.h"

#include <stdint.h>

// ============================================================================
// Small helpers
// ============================================================================

static int ss_is_array(VTKind k) {
    return k == VMT_ARR_I32 || k == VMT_ARR_F32 || k == VMT_ARR_F64 || k == VMT_ARR_I64;
}
static int ss_is_slice(VTKind k) {
    return k == VMT_SLICE_I32 || k == VMT_SLICE_F32 || k == VMT_SLICE_F64 || k == VMT_SLICE_I64;
}
static int ss_is_agg(VTKind k) { return ss_is_array(k) || ss_is_slice(k); }

static VTKind ss_agg_elem(VTKind k) {
    if (k == VMT_ARR_I32 || k == VMT_SLICE_I32) return VMT_I32;
    if (k == VMT_ARR_F32 || k == VMT_SLICE_F32) return VMT_F32;
    if (k == VMT_ARR_F64 || k == VMT_SLICE_F64) return VMT_F64;
    if (k == VMT_ARR_I64 || k == VMT_SLICE_I64) return VMT_I64;
    return VMT_VOID;
}

// u8 and u16 have exact typed-array counterparts and index directly; u1/u2/u4
// stay packed in i32 words and go through the shift/mask helpers.
static int ss_pack_is_native(int bits) { return bits == 8 || bits == 16; }
static int ss_pack_words(int n, int bits) { return (n * bits + 31) / 32; }

static int ss_is_compare(int op) {
    return op == OP_LT || op == OP_LE || op == OP_GT ||
           op == OP_GE || op == OP_EQ || op == OP_NE;
}

// A ref to one struct record: the object itself on both targets.
static int ss_is_ref_type(VMType t) {
    return t.struct_id && ss_is_slice(t.kind) && !t.inner_len;
}

// How many elements an aggregate type holds -- records, for an array of them.
static int ss_agg_count(VMType t) {
    return (t.struct_id && t.inner_len) ? t.len / t.inner_len : t.len;
}

// ============================================================================
// Dialect
// ============================================================================

typedef struct ScriptEmit ScriptEmit;
typedef struct ScriptDialect ScriptDialect;

struct ScriptDialect {
    const char *name;                    // "js" / "lua"

    // ---- pure spelling ------------------------------------------------
    const char *kw_if[2];                // {"if (", ") {"}   {"if ", " then"}
    const char *kw_elseif[2];
    const char *kw_else;
    const char *kw_end;                  // "}"  /  "end"
    const char *kw_while[2];
    const char *kw_local;                // "let " / "local "
    const char *kw_func[3];              // {"function ", "(", ") {"}
    const char *stmt_end;                // ";" / ""
    const char *comment;                 // "//" / "--"
    const char *op[OP_NOT + 1];          // indexed by the OP_* enum; NULL = the
                                         // dialect cannot spell it and a hook
                                         // owns it instead

    const char *const *reserved;         // NULL-terminated keyword list
    int has_comma_op;                    // JS 1, Lua 0
    int has_ternary;                     // JS 1, Lua 0
    int zero_is_falsy;                   // JS 1, Lua 0 (diagnostic: both emit
                                         // an explicit != 0 either way)

    const char *module_head;             // "use strict" + the VM namespace

    const char *f32_wrap[2];             // {"Math.fround(", ")"} / unused
    const char *truthy_wrap[2];          // {"(", " !== 0)"} / {"(", " ~= 0)"}
    const char *boolify_wrap[2];         // {"(", " ? 1 : 0)"} / {"(", " and 1 or 0)"}
    const char *not_wrap[2];             // {"!(", ")"} / {"not (", ")"}

    // ---- lowering hooks: where the dialects genuinely differ ----------
    void (*emit_arith)  (ScriptEmit *, IRNodeT *);
    void (*emit_shift)  (ScriptEmit *, IRNodeT *);
    void (*emit_convert)(ScriptEmit *, IRNodeT *);
    void (*emit_bitcast)(ScriptEmit *, IRNodeT *);
    void (*emit_zero)   (ScriptEmit *, VMType);        // local initialiser
    void (*emit_arr_lit)(ScriptEmit *, IRNodeT *);
    void (*emit_index)  (ScriptEmit *, IRNodeT *);     // element read
    void (*emit_store)  (ScriptEmit *, IRNodeT *lv, IRNodeT *rv);
    void (*emit_slice)  (ScriptEmit *, IRNodeT *);
    void (*emit_len)    (ScriptEmit *, IRNodeT *base);
    // The aggregate value in a context that wants a SLICE (a native argument,
    // an IR_FMT destination, a []T parameter). JS hands the typed array over
    // as-is; Lua has to build a view when the value is a bare table.
    void (*emit_as_slice)(ScriptEmit *, IRNodeT *);
    void (*emit_copy)   (ScriptEmit *, IRNodeT *, VMType dst);  // array value semantics
    int  (*supports)    (VTKind);
};

// ============================================================================
// Emitter state
// ============================================================================

#define SS_MAX_DATA  128
#define SS_MAX_LOOPS 32

struct ScriptEmit {
    VM         *vm;
    const ScriptDialect *d;
    StrBuf     *out;            // where expression / statement text goes
    StrBuf     *pre;            // hoisted statements for the enclosing statement
    StrBuf      root;           // the module text
    Func       *cur_func;
    int         indent;
    Func      **func_list;
    const char **func_cnames;
    int         func_count;
    const char **local_names;   // indexed by slot, for cur_func
    int         n_local_names;
    int         int_wrap;       // VM_FLAG_INT_WRAP for cur_func
    int         tmp_next;
    int         loop_label[SS_MAX_LOOPS];
    int         loop_depth;
    int         err;
    char        errmsg[200];

    // Which prelude helpers the walk reached. Set only through ss_call (and
    // ss_use for the handful of names that do not come from one), so a new
    // call site cannot emit a call the prelude then leaves out.
    char        helper_used[SS_N_HELPERS];

    // The one top-level statement whose assignment also DECLARES its target,
    // the prologue having skipped it. See emit_plan_decls.
    IRNodeT    *decl_stmt;

    // Module-level backing storage for IR_DATA_SLICE literals, deduplicated by
    // the arena pointer vm.c handed us. C gets this free from static storage;
    // a table constructor inside a loop body would allocate every iteration.
    const void *data_ptr[SS_MAX_DATA];
    int         data_n[SS_MAX_DATA];
    int         data_bytes[SS_MAX_DATA];
    int         n_data;
};

static void ss_str(ScriptEmit *e, const char *s) { sb_str(e->out, s); }
static void ss_ch (ScriptEmit *e, char c)        { sb_char(e->out, c); }
static void ss_int(ScriptEmit *e, long long v)   { sb_int(e->out, v); }

static void ss_use(ScriptEmit *e, int h) {
    if (h >= 0 && h < SS_N_HELPERS) e->helper_used[h] = 1;
}

// Write `VM.<helper>(` and record that the module needs it. THE ONLY WAY to
// spell a prelude call: writing "VM.foo(" by hand would emit a call to
// something ss_emit_prelude then has no reason to define.
static void ss_call(ScriptEmit *e, int h) {
    ss_use(e, h);
    ss_str(e, "VM.");
    ss_str(e, SCRIPT_HELPERS[h].name);
    ss_ch(e, '(');
}

// The helper a registered native resolves to, or -1 when the host supplies it
// (in which case nothing is emitted for it -- correctly, since the emitter has
// no body to emit). Matches the f32 variant name too: `m_sinf` and `m_sin`
// share one chunk.
static int ss_helper_by_name(const char *n) {
    for (int i = 0; i < SS_N_HELPERS; i++) {
        const ScriptHelper *h = &SCRIPT_HELPERS[i];
        if (h->name  && es_streq(h->name,  n)) return i;
        if (h->alias && es_streq(h->alias, n)) return i;
    }
    return -1;
}

static void ss_indent(ScriptEmit *e) {
    for (int i = 0; i < e->indent * 4; i++) sb_char(e->out, ' ');
}
static void ss_pre_indent(ScriptEmit *e) {
    for (int i = 0; i < e->indent * 4; i++) sb_char(e->pre, ' ');
}

// First failure wins: a later one is usually a consequence of the first, and
// the message that fixes the user's build is the one that came first.
static void ss_failf(ScriptEmit *e, const char *what, const char *detail) {
    if (e->err) return;
    e->err = 1;
    int p = 0;
    for (const char *s = what; *s && p < (int)sizeof(e->errmsg) - 1; s++) e->errmsg[p++] = *s;
    if (detail)
        for (const char *s = detail; *s && p < (int)sizeof(e->errmsg) - 1; s++) e->errmsg[p++] = *s;
    e->errmsg[p] = '\0';
}

// Redirect output, so a subexpression can be rendered into a scratch buffer
// with a hoist buffer of its own.
typedef struct { StrBuf *out, *pre; } SEChan;
static SEChan ss_redirect(ScriptEmit *e, StrBuf *out, StrBuf *pre) {
    SEChan old; old.out = e->out; old.pre = e->pre;
    e->out = out; e->pre = pre;
    return old;
}
static void ss_restore(ScriptEmit *e, SEChan c) { e->out = c.out; e->pre = c.pre; }

static void ss_take(StrBuf *dst, StrBuf *src) {
    if (src->len) sb_append(dst, src->buf, src->len);
    if (!src->ok) dst->ok = 0;
}

// ============================================================================
// Forward declarations
// ============================================================================

static void emit_expr(ScriptEmit *e, IRNodeT *n);
static void ss_struct_call(ScriptEmit *e, const char *what, VMType t);
static void ss_emit_field_access(ScriptEmit *e, IRNodeT *n);
static void ss_emit_elem(ScriptEmit *e, IRNodeT *n);
static void emit_cond(ScriptEmit *e, IRNodeT *n);
static void emit_stmt(ScriptEmit *e, IRNodeT *n, int is_last);
static void emit_block_stmts(ScriptEmit *e, IRNodeT *blk);
static void emit_local_name(ScriptEmit *e, int slot);
static void emit_slice_value(ScriptEmit *e, IRNodeT *n);

// ============================================================================
// Literals
// ============================================================================

static int ss_is_int_str(const char *s) {
    for (int i = 0; s[i]; i++)
        if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') return 0;
    return 1;
}

// A float literal that reads back as the value the VM ran -- SHORTEST, not
// %g's 6 significant digits, which silently rewrites 110566002.1. The trailing
// ".0" matters for more than looks in Lua, where `1` is an INTEGER and `1.0` a
// float, and the two behave differently under // and %.
static void ss_float_lit(ScriptEmit *e, double v, VTKind kind, int lua) {
    if (v != v)                    { ss_str(e, lua ? "(0.0/0.0)" : "NaN"); return; }
    if (v > 1.7976931348623157e308) { ss_str(e, lua ? "math.huge" : "Infinity"); return; }
    if (v < -1.7976931348623157e308){ ss_str(e, lua ? "-math.huge" : "-Infinity"); return; }
    // An f32 literal is pre-rounded here, so the rounding costs nothing at run
    // time and the printed digits are the ones the interpreter actually used.
    if (kind == VMT_F32) v = (double)(float)v;
    char tmp[S_FROM_NUMBER_MAX_CHARS];
    s_from_number_flags(v, tmp, S_FROM_NUMBER_FLAG_SHORTEST);
    ss_str(e, tmp);
    if (ss_is_int_str(tmp)) ss_str(e, ".0");
}

static void ss_int_lit(ScriptEmit *e, long long v, int lua) {
    // -9223372036854775808 has no literal spelling: Lua reads it as unary
    // minus applied to a value that has already overflowed to a float.
    if (lua && v == (-9223372036854775807LL - 1)) { ss_str(e, "math.mininteger"); return; }
    ss_int(e, v);
}

// ============================================================================
// Names
// ============================================================================

// A shared variable's field name, escaped once against the target's keywords
// so the declaration in vm_emit_*_globals and every use site agree. Escaping
// against the globals ALREADY assigned (not just the keyword list) is what
// keeps a table declaring both `end` and `end_` from collapsing them.
static const char *ss_global_name(VM *vm, const ScriptDialect *d, int idx) {
    const char *taken[VM_MAX_GLOBALS];
    for (int i = 0; i <= idx && i < VM_MAX_GLOBALS; i++) {
        const char *raw = intern_get_cstr(vm->intern, vm->globals[i].name);
        taken[i] = es_escape_name(vm, raw, d->reserved, taken, i);
    }
    return taken[idx];
}

static int ss_global_index(ScriptEmit *e, InternID name) {
    for (int i = 0; i < e->vm->n_globals; i++)
        if (e->vm->globals[i].name == name) return i;
    return -1;
}

static int ss_host_buf_id(ScriptEmit *e, int slot) {
    VMSym *s = &e->cur_func->syms[slot];
    if (!s->is_host_buf) return -1;
    int id = s->offset / (int)sizeof(VMHostBufSlot);
    if (id < 0 || id >= VM_MAX_HOST_BUFS) return -1;
    return e->vm->host_bufs[id].declared ? id : -1;
}

// One name per slot, built on entry to each function. Assigned in slot order;
// on a collision with either the keyword list or an already-assigned name, a
// '_' is appended and the check re-runs -- the same conflict loop
// emit_build_cnames uses for function names.
static void ss_build_local_names(ScriptEmit *e, Func *f) {
    VM *vm = e->vm;
    const char **names = (const char **)mem_alloc(&vm->run.mem,
                                                  sizeof(char *) * (size_t)(f->n_syms + 1));
    if (!names) { ss_failf(e, "out of memory", 0); e->local_names = 0; e->n_local_names = 0; return; }

    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        const char *raw;

        if (s->is_global && s->name != 0) {
            // File-scope storage under the host's prefix -- not a local at all,
            // and not part of the local name space.
            int gi = ss_global_index(e, s->name);
            const char *field = gi >= 0 ? ss_global_name(vm, e->d, gi)
                                        : intern_get_cstr(vm->intern, s->name);
            const char *pfx = vm->globals_c_prefix;
            int pl = es_strlen(pfx), fl = es_strlen(field);
            char *out = (char *)mem_alloc(&vm->run.mem, (size_t)(pl + fl + 1));
            if (!out) { names[i] = field; continue; }
            for (int k = 0; k < pl; k++) out[k] = pfx[k];
            for (int k = 0; k <= fl; k++) out[pl + k] = field[k];
            names[i] = out;
            continue;
        }
        if (s->is_host_buf) {
            int id = ss_host_buf_id(e, i);
            const char *cn = id >= 0 ? vm->host_bufs[id].c_name : 0;
            raw = cn ? cn : intern_get_cstr(vm->intern, s->name);
            // The identifier is the HOST's to choose, so it gets the same
            // keyword escape -- `in`, `end` and `this` are all plausible
            // buffer names.
            names[i] = es_escape_name(vm, raw, e->d->reserved, 0, 0);
            continue;
        }
        if (s->name != 0) {
            raw = intern_get_cstr(vm->intern, s->name);
        } else {
            char *out = (char *)mem_alloc(&vm->run.mem, 16);
            if (!out) { names[i] = "__v"; continue; }
            int p = 0;
            out[p++] = '_'; out[p++] = '_'; out[p++] = 'v';
            int v = i, dn = 0; char dg[8];
            if (v == 0) dg[dn++] = '0';
            while (v > 0) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0) out[p++] = dg[--dn];
            out[p] = '\0';
            names[i] = out;
            continue;
        }
        names[i] = es_escape_name(e->vm, raw, e->d->reserved, names, i);
    }
    e->local_names  = names;
    e->n_local_names = f->n_syms;
}

static void emit_local_name(ScriptEmit *e, int slot) {
    if (slot < 0 || slot >= e->n_local_names || !e->local_names) { ss_str(e, "__bad"); return; }
    ss_str(e, e->local_names[slot]);
}

static const char *ss_get_cname(ScriptEmit *e, Func *f) {
    for (int i = 0; i < e->func_count; i++)
        if (e->func_list[i] == f) return e->func_cnames[i];
    return "__unknown";
}

// ============================================================================
// Hoisting (Lua): expressions that have to become statements
// ============================================================================

static int ss_hoists(ScriptEmit *e) { return !e->d->has_ternary || !e->d->has_comma_op; }

// A fresh temporary, declared in the hoist buffer. Callers own the returned
// name; it lives in the VM arena.
static const char *ss_new_temp(ScriptEmit *e) {
    char *out = (char *)mem_alloc(&e->vm->run.mem, 16);
    if (!out) { ss_failf(e, "out of memory", 0); return "__t"; }
    int p = 0, v = e->tmp_next++, dn = 0; char dg[8];
    out[p++] = '_'; out[p++] = '_'; out[p++] = 't';
    if (v == 0) dg[dn++] = '0';
    while (v > 0) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) out[p++] = dg[--dn];
    out[p] = '\0';
    return out;
}

// Render one node into `txt`, with any statements it hoists into `pre`. Both
// buffers are the caller's; neither is written to e->out.
static void ss_render(ScriptEmit *e, IRNodeT *n, int as_cond, StrBuf *pre, StrBuf *txt) {
    sb_init(pre, e->vm->run.sys);
    sb_init(txt, e->vm->run.sys);
    SEChan c = ss_redirect(e, txt, pre);
    if (as_cond) emit_cond(e, n); else emit_expr(e, n);
    ss_restore(e, c);
}

static void ss_render_free(ScriptEmit *e, StrBuf *pre, StrBuf *txt) {
    e->vm->run.sys->free(pre->buf);
    e->vm->run.sys->free(txt->buf);
}

// Evaluate `n` into a temporary, emitted into e->pre, and return the name.
// Used for the Lua slice base that would otherwise be named twice
// (`X.a[X.off + i]`) and for the operands of a hoisted select.
static const char *ss_stage(ScriptEmit *e, IRNodeT *n) {
    StrBuf pre, txt;
    ss_render(e, n, 0, &pre, &txt);
    ss_take(e->pre, &pre);
    const char *tmp = ss_new_temp(e);
    ss_pre_indent(e);
    sb_str(e->pre, e->d->kw_local);
    sb_str(e->pre, tmp);
    sb_str(e->pre, " = ");
    ss_take(e->pre, &txt);
    sb_str(e->pre, e->d->stmt_end);
    sb_char(e->pre, '\n');
    ss_render_free(e, &pre, &txt);
    return tmp;
}

// True when anything under `n` would have to hoist on this dialect. Used only
// to decide whether a short-circuit operand may be hoisted THROUGH -- doing so
// would make it evaluate unconditionally -- so it is deliberately conservative.
static int ss_subtree_hoists(ScriptEmit *e, IRNodeT *n) {
    if (!n || !ss_hoists(e)) return 0;
    Func *f = e->cur_func;
    int op = NODE_OP(n);
    if (op == IR_SELECT || op == IR_COMMA) return 1;
    if (op == IR_CALL || op == IR_ARR_LIT || op == IR_FMT || op == IR_SLICE) {
        for (int i = 0; i < NODE_N_ITEMS(n); i++)
            if (ss_subtree_hoists(e, ir_item(f, n, i))) return 1;
        return 0;
    }
    if (NODE_A(n) >= 0 && ss_subtree_hoists(e, ir_child(f, NODE_A(n)))) return 1;
    if (NODE_B(n) >= 0 && ss_subtree_hoists(e, ir_child(f, NODE_B(n)))) return 1;
    if (NODE_C(n) >= 0 && ss_subtree_hoists(e, ir_child(f, NODE_C(n)))) return 1;
    return 0;
}

// An `and` / `or` whose RIGHT operand has to hoist. Hoisting through the operator
// would lift the right side out and make it evaluate unconditionally -- the whole
// thing short-circuiting exists to prevent -- so the operator itself becomes an
// `if`, with the right side's statements inside the branch that actually runs it.
// Yields a temporary holding an i32 0 or 1, matching what the interpreter
// normalises `and`/`or` to.
static const char *ss_shortcircuit_temp(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    int is_and = (NODE_SUBOP(n) == OP_AND);
    StrBuf apre, atxt, bpre, btxt;

    ss_render(e, ir_child(f, NODE_A(n)), 1, &apre, &atxt);
    e->indent++;
    ss_render(e, ir_child(f, NODE_B(n)), 1, &bpre, &btxt);
    e->indent--;
    const char *tmp = ss_new_temp(e);

    ss_take(e->pre, &apre);
    ss_pre_indent(e); sb_str(e->pre, e->d->kw_local); sb_str(e->pre, tmp); sb_char(e->pre, '\n');
    ss_pre_indent(e); sb_str(e->pre, e->d->kw_if[0]);
    ss_take(e->pre, &atxt);
    sb_str(e->pre, e->d->kw_if[1]); sb_char(e->pre, '\n');

    // The arm that evaluates the right operand: `and` takes it when the left
    // was true, `or` when it was false. The other arm is the short-circuit
    // answer, and never touches the right side at all.
    for (int arm = 0; arm < 2; arm++) {
        if (arm) { ss_pre_indent(e); sb_str(e->pre, e->d->kw_else); sb_char(e->pre, '\n'); }
        e->indent++;
        if (arm == (is_and ? 1 : 0)) {
            ss_pre_indent(e); sb_str(e->pre, tmp);
            sb_str(e->pre, is_and ? " = 0" : " = 1");
            sb_str(e->pre, e->d->stmt_end); sb_char(e->pre, '\n');
        } else {
            ss_take(e->pre, &bpre);
            ss_pre_indent(e); sb_str(e->pre, tmp); sb_str(e->pre, " = ");
            sb_str(e->pre, e->d->boolify_wrap[0]);
            sb_append(e->pre, btxt.buf, btxt.len);
            sb_str(e->pre, e->d->boolify_wrap[1]);
            sb_str(e->pre, e->d->stmt_end); sb_char(e->pre, '\n');
        }
        e->indent--;
    }
    ss_pre_indent(e); sb_str(e->pre, e->d->kw_end); sb_char(e->pre, '\n');

    ss_render_free(e, &apre, &atxt);
    ss_render_free(e, &bpre, &btxt);
    return tmp;
}

// `continue` belonging to THIS loop (a nested loop owns its own), so a Lua
// loop only pays for the goto label when something jumps to it.
static int ss_has_continue(Func *f, IRNodeT *n) {
    if (!n) return 0;
    int op = NODE_OP(n);
    if (op == IR_CONTINUE) return 1;
    if (op == IR_WHILE || op == IR_FOR) return 0;
    switch (op) {
        case IR_BLOCK:
            for (int i = 0; i < NODE_N_ITEMS(n); i++)
                if (ss_has_continue(f, ir_item(f, n, i))) return 1;
            return 0;
        case IR_IF:
            return (NODE_A(n) >= 0 && ss_has_continue(f, ir_child(f, NODE_A(n))))
                || (NODE_B(n) >= 0 && ss_has_continue(f, ir_child(f, NODE_B(n))))
                || (NODE_C(n) >= 0 && ss_has_continue(f, ir_child(f, NODE_C(n))));
        default:
            return 0;
    }
}

// ============================================================================
// Data slices (string / constant-data literals)
// ============================================================================

static int ss_data_id(ScriptEmit *e, IRNodeT *n) {
    const void *p = (const void *)(intptr_t)NODE_KI(n);
    int bytes = (n->type.pack_bits == 8);
    for (int i = 0; i < e->n_data; i++)
        if (e->data_ptr[i] == p && e->data_n[i] == NODE_N_ITEMS(n) && e->data_bytes[i] == bytes)
            return i;
    if (e->n_data >= SS_MAX_DATA) { ss_failf(e, "too many string/data literals", 0); return 0; }
    e->data_ptr[e->n_data]   = p;
    e->data_n[e->n_data]     = NODE_N_ITEMS(n);
    e->data_bytes[e->n_data] = bytes;
    return e->n_data++;
}

static void ss_data_name(ScriptEmit *e, int id) {
    ss_str(e, "__d");
    ss_int(e, id);
}

// ============================================================================
// Truthiness / boolean results
// ============================================================================

static void ss_truthy_open(ScriptEmit *e)  { ss_str(e, e->d->truthy_wrap[0]); }
static void ss_truthy_close(ScriptEmit *e) { ss_str(e, e->d->truthy_wrap[1]); }

// A comparison or a logical op is already the boolean the target's `if` wants,
// so it is emitted bare in condition position; anything else gets an explicit
// `!= 0`. That last case is the one that matters most for Lua, where `if x`
// on x == 0 silently takes the wrong branch -- and it is invisible in JS
// testing, because JS agrees with C that 0 is falsy.
static void emit_cond(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    if (!n) { ss_str(e, e->d->has_ternary ? "false" : "false"); return; }

    if (NODE_OP(n) == IR_UNOP && NODE_SUBOP(n) == OP_NOT) {
        ss_str(e, e->d->not_wrap[0]);
        emit_cond(e, ir_child(f, NODE_A(n)));
        ss_str(e, e->d->not_wrap[1]);
        return;
    }
    if (NODE_OP(n) == IR_BINOP) {
        int s = NODE_SUBOP(n);
        if (s == OP_AND || s == OP_OR) {
            if (ss_hoists(e) && ss_subtree_hoists(e, ir_child(f, NODE_B(n)))) {
                // The temp is already an i32 0/1, so it comes back through the
                // ordinary "anything else" path.
                ss_truthy_open(e);
                ss_str(e, ss_shortcircuit_temp(e, n));
                ss_truthy_close(e);
                return;
            }
            ss_ch(e, '(');
            emit_cond(e, ir_child(f, NODE_A(n)));
            ss_str(e, e->d->op[s]);
            emit_cond(e, ir_child(f, NODE_B(n)));
            ss_ch(e, ')');
            return;
        }
        if (ss_is_compare(s)) {
            ss_ch(e, '(');
            emit_expr(e, ir_child(f, NODE_A(n)));
            ss_str(e, e->d->op[s]);
            emit_expr(e, ir_child(f, NODE_B(n)));
            ss_ch(e, ')');
            return;
        }
    }
    ss_truthy_open(e);
    emit_expr(e, n);
    ss_truthy_close(e);
}

// The same nodes in VALUE position: the interpreter normalises every one of
// them to a C int 0 or 1 (vm_run.c), which neither target does on its own.
static void ss_emit_boolified(ScriptEmit *e, IRNodeT *n) {
    ss_str(e, e->d->boolify_wrap[0]);
    emit_cond(e, n);
    ss_str(e, e->d->boolify_wrap[1]);
}

// ============================================================================
// f32 rounding
// ============================================================================

static int ss_f32(ScriptEmit *e, IRNodeT *n) {
    return n->type.kind == VMT_F32 && e->d->f32_wrap[0][0] != '\0';
}
static void ss_f32_open(ScriptEmit *e, IRNodeT *n)  { if (ss_f32(e, n)) ss_str(e, e->d->f32_wrap[0]); }
static void ss_f32_close(ScriptEmit *e, IRNodeT *n) { if (ss_f32(e, n)) ss_str(e, e->d->f32_wrap[1]); }

// ============================================================================
// Aggregates: the value-semantics copy
// ============================================================================

// The interpreter spells out exactly three memcpy sites (call argument,
// assignment, return), all keyed on is_array() of the DESTINATION type. A
// slice destination is deliberately NOT copied at those same sites -- it
// stores a borrowed pointer.
//
// Elided when the source is already fresh: an array literal constructs at the
// use site, and a call returning an array had its own return copy.
static int ss_needs_copy(VMType dst, IRNodeT *src) {
    if (!ss_is_array(dst.kind)) return 0;
    if (!src) return 0;
    int op = NODE_OP(src);
    if (op == IR_ARR_LIT || op == IR_CALL) return 0;
    return 1;
}

static void ss_emit_maybe_copy(ScriptEmit *e, IRNodeT *src, VMType dst) {
    if (ss_needs_copy(dst, src)) {
        if (dst.struct_id) {
            // A record, or records: a fresh object holding the same fields. The
            // struct is named from the source, whose type indexes this Func's
            // table -- the destination may be a callee's.
            VMType st = src->type;
            ss_struct_call(e, dst.inner_len ? "__clonen_" : "__clone_", st);
            emit_expr(e, src);
            if (dst.inner_len) { ss_str(e, ", "); ss_int(e, ss_agg_count(dst)); }
            ss_ch(e, ')');
            return;
        }
        e->d->emit_copy(e, src, dst);
        return;
    }
    emit_expr(e, src);
}

// ============================================================================
// Native calls
// ============================================================================

// The same variant name vm_emit_c.c would pick, under the VM namespace:
// VM.m_sin / VM.fx16_sin / VM.m_imin. Deliberately NOT new js_name/lua_name
// fields on CFuncEntry -- that struct is instantiated per registered builtin
// on targets where bytes matter.
static void emit_native_call(ScriptEmit *e, Func *callee, IRNodeT *n) {
    Func *f = e->cur_func;
    int idx = callee->native_tok - TOK_MATHS_FIRST;
    CFuncEntry *ent = &e->vm->run.cfunc_table[idx];
    VTKind rk = n->type.kind;
    const char *fname = 0;

    if (ent->has_sig) {
        fname = ent->sig_name ? ent->sig_name : intern_get_cstr(e->vm->intern, callee->name);
    } else {
        if      (rk == VMT_F32 && ent->f32_name) fname = ent->f32_name;
        else if (rk == VMT_F64 && ent->f64_name) fname = ent->f64_name;
        else if (rk == VMT_I32) fname = NODE_SUBOP(n) ? ent->fx_name : ent->i32_name;
        if (!fname) fname = intern_get_cstr(e->vm->intern, callee->name);
    }

    // Printed under the name the C backend would use, not the helper's
    // canonical one -- `m_sinf` stays `m_sinf`, and the chunk defines both.
    // A name the prelude does not know is a host-provided native: nothing to
    // mark and nothing to emit, which is the whole point of the VM namespace.
    ss_use(e, ss_helper_by_name(fname));

    ss_f32_open(e, n);
    ss_str(e, "VM.");
    ss_str(e, fname);
    ss_ch(e, '(');
    for (int i = 0; i < NODE_N_ITEMS(n); i++) {
        if (i) ss_str(e, ", ");
        IRNodeT *arg = ir_item(f, n, i);
        // A []T argument is ONE value here, not C's (ptr, len) pair: with no
        // pointer to fold the offset into, the pair shape cannot survive on
        // either target. This is host-facing -- see vm_emit_script.h.
        if (ent->has_sig && ent->arg_kinds[i] == VMT_SLICE_I32) emit_slice_value(e, arg);
        else                                                    emit_expr(e, arg);
    }
    ss_ch(e, ')');
    ss_f32_close(e, n);
}

// ============================================================================
// Expressions
// ============================================================================

// The value of an aggregate-typed node in a context that wants a slice.
static void emit_slice_value(ScriptEmit *e, IRNodeT *n) {
    e->d->emit_as_slice(e, n);
}

static void emit_call(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    Func *callee = e->vm->run.funcs_by_id[(int)NODE_KI(n)];
    if (callee->native_tok != 0) { emit_native_call(e, callee, n); return; }

    ss_str(e, ss_get_cname(e, callee));
    ss_ch(e, '(');
    for (int i = 0; i < NODE_N_ITEMS(n); i++) {
        if (i) ss_str(e, ", ");
        IRNodeT *arg = ir_item(f, n, i);
        if (i < callee->n_params) {
            VMType want = callee->syms[callee->param_slot[i]].type;
            // A ref takes the caller's object itself.
            if (ss_is_ref_type(want)) { emit_expr(e, arg); continue; }
            // Copy site 1: an ARRAY parameter takes its argument by value.
            // An unannotated parameter is a SLICE (template_param_type in
            // vm.c), so the ordinary untyped lambda costs nothing here.
            if (ss_is_array(want.kind)) { ss_emit_maybe_copy(e, arg, want); continue; }
            if (ss_is_slice(want.kind) && ss_is_agg(arg->type.kind)) { emit_slice_value(e, arg); continue; }
        }
        emit_expr(e, arg);
    }
    ss_ch(e, ')');
}

static void emit_select(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    if (e->d->has_ternary) {
        ss_ch(e, '(');
        emit_cond(e, ir_child(f, NODE_A(n)));
        ss_str(e, " ? ");
        emit_expr(e, ir_child(f, NODE_B(n)));
        ss_str(e, " : ");
        emit_expr(e, ir_child(f, NODE_C(n)));
        ss_ch(e, ')');
        return;
    }

    // Lua: statements, for the reason given at the top of this file -- neither
    // `(c and t or f)` nor a VM.sel() helper is correct here.
    StrBuf cpre, ctxt, bpre, btxt, epre, etxt;
    ss_render(e, ir_child(f, NODE_A(n)), 1, &cpre, &ctxt);
    const char *tmp = ss_new_temp(e);

    ss_take(e->pre, &cpre);
    ss_pre_indent(e); sb_str(e->pre, e->d->kw_local); sb_str(e->pre, tmp); sb_char(e->pre, '\n');
    ss_pre_indent(e); sb_str(e->pre, e->d->kw_if[0]);
    ss_take(e->pre, &ctxt);
    sb_str(e->pre, e->d->kw_if[1]); sb_char(e->pre, '\n');

    e->indent++;
    ss_render(e, ir_child(f, NODE_B(n)), 0, &bpre, &btxt);
    ss_take(e->pre, &bpre);
    ss_pre_indent(e); sb_str(e->pre, tmp); sb_str(e->pre, " = ");
    ss_take(e->pre, &btxt);
    sb_str(e->pre, e->d->stmt_end); sb_char(e->pre, '\n');
    e->indent--;

    ss_pre_indent(e); sb_str(e->pre, e->d->kw_else); sb_char(e->pre, '\n');

    e->indent++;
    ss_render(e, ir_child(f, NODE_C(n)), 0, &epre, &etxt);
    ss_take(e->pre, &epre);
    ss_pre_indent(e); sb_str(e->pre, tmp); sb_str(e->pre, " = ");
    ss_take(e->pre, &etxt);
    sb_str(e->pre, e->d->stmt_end); sb_char(e->pre, '\n');
    e->indent--;

    ss_pre_indent(e); sb_str(e->pre, e->d->kw_end); sb_char(e->pre, '\n');

    ss_render_free(e, &cpre, &ctxt);
    ss_render_free(e, &bpre, &btxt);
    ss_render_free(e, &epre, &etxt);
    ss_str(e, tmp);
}

// Emit one IR_COMMA item as a statement, into e->pre. Items 0..n-2 are the
// side effects; item n-1 is the value.
static void emit_assign_inline(ScriptEmit *e, IRNodeT *n);

static void emit_comma(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    if (e->d->has_comma_op) {
        ss_ch(e, '(');
        for (int i = 0; i < NODE_N_ITEMS(n); i++) {
            if (i) ss_str(e, ", ");
            IRNodeT *it = ir_item(f, n, i);
            if (NODE_OP(it) == IR_ASSIGN) emit_assign_inline(e, it);
            else                          emit_expr(e, it);
        }
        ss_ch(e, ')');
        return;
    }
    // Lua: the leading items become statements ahead of the enclosing one.
    for (int i = 0; i + 1 < NODE_N_ITEMS(n); i++) {
        IRNodeT *it = ir_item(f, n, i);
        StrBuf pre, txt;
        sb_init(&pre, e->vm->run.sys);
        sb_init(&txt, e->vm->run.sys);
        SEChan c = ss_redirect(e, &txt, &pre);
        if (NODE_OP(it) == IR_ASSIGN) emit_assign_inline(e, it);
        else                          emit_expr(e, it);
        ss_restore(e, c);
        ss_take(e->pre, &pre);
        ss_pre_indent(e);
        ss_take(e->pre, &txt);
        sb_char(e->pre, '\n');
        ss_render_free(e, &pre, &txt);
    }
    emit_expr(e, ir_item(f, n, NODE_N_ITEMS(n) - 1));
}

static void emit_expr(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    if (!n) { ss_str(e, "0"); return; }
    int lua = !e->d->has_ternary;

    switch (NODE_OP(n)) {
        case IR_CONST_I:
            ss_int_lit(e, NODE_KI(n), lua);
            break;

        case IR_CONST_F:
            ss_float_lit(e, NODE_KF(n), n->type.kind, lua);
            break;

        case IR_LOCAL:
            emit_local_name(e, (int)NODE_KI(n));
            break;

        case IR_INDEX:
            e->d->emit_index(e, n);
            break;

        case IR_LEN:
            e->d->emit_len(e, ir_child(f, NODE_A(n)));
            break;

        case IR_FIELD:
            if (NODE_SUBOP(n) == FIELD_ELEM) ss_emit_elem(e, n);
            else                             ss_emit_field_access(e, n);
            break;

        case IR_CVT:
            e->d->emit_convert(e, n);
            break;

#if VM_HAS_INSPECT
        // Editor-only: exported code is the code the user wrote, so this
        // renders as its operand alone. Nothing is dropped, so none of
        // IR_PRINT's dead-locals bookkeeping applies.
        case IR_INSPECT:
            emit_expr(e, ir_child(f, NODE_A(n)));
            break;
#endif

        case IR_BITCAST:
            e->d->emit_bitcast(e, n);
            break;

        case IR_UNOP:
            if (NODE_SUBOP(n) == OP_NEG) {
                ss_f32_open(e, n);
                if (n->type.kind == VMT_I32 && e->int_wrap && !e->d->has_ternary) {
                    // Lua: -INT_MIN has to come back down to 32 bits.
                    ss_call(e, H_W32); ss_str(e, "-(");
                    emit_expr(e, ir_child(f, NODE_A(n)));
                    ss_str(e, "))");
                } else if (n->type.kind == VMT_I32 && e->int_wrap) {
                    ss_str(e, "((-(");
                    emit_expr(e, ir_child(f, NODE_A(n)));
                    ss_str(e, ")) | 0)");
                } else {
                    ss_str(e, "(-(");
                    emit_expr(e, ir_child(f, NODE_A(n)));
                    ss_str(e, "))");
                }
                ss_f32_close(e, n);
            } else {
                ss_emit_boolified(e, n);
            }
            break;

        case IR_BINOP: {
            int s = NODE_SUBOP(n);
            if (ss_is_compare(s) || s == OP_AND || s == OP_OR) {
                // A short-circuit operand that would hoist must not be lifted
                // out of the `and`/`or` -- that would make it run
                // unconditionally. The operator becomes an `if` instead, whose
                // temporary is already the 0/1 this position wants.
                if ((s == OP_AND || s == OP_OR) && ss_hoists(e)
                    && ss_subtree_hoists(e, ir_child(f, NODE_B(n)))) {
                    ss_str(e, ss_shortcircuit_temp(e, n));
                    break;
                }
                ss_emit_boolified(e, n);
                break;
            }
            if (s == OP_BAND || s == OP_BOR || s == OP_BXOR) {
                ss_ch(e, '(');
                emit_expr(e, ir_child(f, NODE_A(n)));
                ss_str(e, e->d->op[s]);
                emit_expr(e, ir_child(f, NODE_B(n)));
                ss_ch(e, ')');
                break;
            }
            if (s == OP_BSHL || s == OP_BSHL_NATIVE || s == OP_BSHR) {
                e->d->emit_shift(e, n);
                break;
            }
            e->d->emit_arith(e, n);
            break;
        }

        case IR_SELECT:
            emit_select(e, n);
            break;

        case IR_COMMA:
            emit_comma(e, n);
            break;

        case IR_ARR_LIT:
            e->d->emit_arr_lit(e, n);
            break;

        case IR_DATA_SLICE:
            ss_data_name(e, ss_data_id(e, n));
            break;

        case IR_SLICE:
            e->d->emit_slice(e, n);
            break;

        case IR_FMT: {
            static const short fn[] = { H_FMT_BYTES, H_FMT_I32, H_FMT_I64,
                                        H_FMT_FX, H_FMT_F64 };
            // VM.str is the only helper nothing emitted calls -- it is how a
            // HOST reads the finished bytes back out. A module that formats
            // has someone waiting to do that, so it comes along here.
            ss_use(e, H_STR);
            ss_call(e, fn[NODE_SUBOP(n)]);
            emit_slice_value(e, ir_item(f, n, 0));
            ss_str(e, ", ");
            emit_expr(e, ir_item(f, n, 1));
            ss_str(e, ", ");
            if (NODE_SUBOP(n) == FMT_BYTES) {
                emit_slice_value(e, ir_item(f, n, 2));
            } else {
                emit_expr(e, ir_item(f, n, 2));
                if (NODE_SUBOP(n) == FMT_FX) {
                    ss_str(e, ", "); ss_int(e, NODE_KI(n) & 0xff);
                    ss_str(e, ", "); ss_int(e, (NODE_KI(n) >> 8) & 0xff);
                } else if (NODE_SUBOP(n) == FMT_F64) {
                    ss_str(e, ", "); ss_int(e, NODE_KI(n) & 0xff);
                }
            }
            ss_ch(e, ')');
            break;
        }

        case IR_CALL:
            emit_call(e, n);
            break;

        default:
            ss_failf(e, "unsupported expression node in the script backend", 0);
            ss_str(e, "0");
            break;
    }
}

// ============================================================================
// Statements
// ============================================================================

// An assignment written as an expression: the shapes a for-loop step clause
// and an IR_COMMA item can take. No indent, no terminator.
static void emit_assign_inline(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *lv = ir_child(f, NODE_A(n));
    IRNodeT *rv = ir_child(f, NODE_B(n));
    if (NODE_OP(lv) == IR_INDEX) { e->d->emit_store(e, lv, rv); return; }
    if (NODE_OP(lv) == IR_FIELD) {
        int sub = NODE_SUBOP(lv);
        if (sub == FIELD_ELEM || lv->type.struct_id) {
            // Whole records are copied INTO the destination rather than replacing
            // it: the destination may be what a ref parameter points at.
            ss_struct_call(e, lv->type.inner_len ? "__copyn_" : "__copy_", lv->type);
            emit_expr(e, lv);
            ss_str(e, ", ");
            emit_expr(e, rv);
            if (lv->type.inner_len) { ss_str(e, ", "); ss_int(e, ss_agg_count(lv->type)); }
            ss_ch(e, ')');
            return;
        }
        emit_expr(e, lv);
        ss_str(e, " = ");
        if (sub >= FIELD_AGG) { ss_emit_maybe_copy(e, rv, lv->type); return; }
        // A u8 / u16 member is a plain number here, so the truncation C and the
        // interpreter get from the storage has to be written out.
        if (sub == FIELD_U8 || sub == FIELD_U16) {
            ss_ch(e, '(');
            emit_expr(e, rv);
            ss_str(e, sub == FIELD_U8 ? " & 0xFF)" : " & 0xFFFF)");
            return;
        }
        emit_expr(e, rv);
        return;
    }
    emit_local_name(e, (int)NODE_KI(lv));
    ss_str(e, " = ");
    // Copy site 2: assignment into an array-typed local.
    ss_emit_maybe_copy(e, rv, f->syms[(int)NODE_KI(lv)].type);
}

static void emit_stmt_inner(ScriptEmit *e, IRNodeT *n, int is_last);

// Every statement flushes the hoist buffer its own expressions filled, at the
// current indent, before writing its own line. Nested statements do the same
// into the enclosing statement's body buffer, so hoists never cross a
// statement boundary they should not.
static void emit_stmt(ScriptEmit *e, IRNodeT *n, int is_last) {
    if (!n) return;
    if (!ss_hoists(e)) { emit_stmt_inner(e, n, is_last); return; }

    StrBuf pre, body;
    sb_init(&pre, e->vm->run.sys);
    sb_init(&body, e->vm->run.sys);
    SEChan c = ss_redirect(e, &body, &pre);
    emit_stmt_inner(e, n, is_last);
    ss_restore(e, c);
    ss_take(e->out, &pre);
    ss_take(e->out, &body);
    e->vm->run.sys->free(pre.buf);
    e->vm->run.sys->free(body.buf);
}

static void emit_block_stmts(ScriptEmit *e, IRNodeT *blk) {
    if (!blk || NODE_OP(blk) != IR_BLOCK) return;
    int n = NODE_N_ITEMS(blk);
    for (int i = 0; i < n; i++)
        emit_stmt(e, ir_item(e->cur_func, blk, i), i == n - 1);
}

// Lua requires `return` to be the last statement of its block, and a jump
// (break / goto) reads better wrapped the same way. `do ... end` is the
// standard escape and is always valid.
static void ss_jump_open(ScriptEmit *e, int is_last) {
    if (!e->d->has_ternary && !is_last) ss_str(e, "do ");
}
static void ss_jump_close(ScriptEmit *e, int is_last) {
    if (!e->d->has_ternary && !is_last) ss_str(e, " end");
}

static void emit_loop_body(ScriptEmit *e, IRNodeT *body, int label) {
    if (label < 0) { emit_block_stmts(e, body); return; }
    // The `continue` target. The body goes inside its own do-block, and the
    // step after the label inside another, so the label is followed by no
    // local declaration in its own block -- which is the one thing Lua's goto
    // rule forbids.
    ss_indent(e); ss_str(e, "do\n");
    e->indent++;
    emit_block_stmts(e, body);
    e->indent--;
    ss_indent(e); ss_str(e, "end\n");
    ss_indent(e); ss_str(e, "::__cont"); ss_int(e, label); ss_str(e, "::\n");
}

static void emit_while(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *body = ir_child(f, NODE_B(n));
    int label = -1;
    if (!e->d->has_ternary && ss_has_continue(f, body)) label = e->tmp_next++;
    if (e->loop_depth < SS_MAX_LOOPS) e->loop_label[e->loop_depth] = label;
    e->loop_depth++;

    StrBuf cpre, ctxt;
    e->indent++;                      // the hoisted statements live INSIDE the loop
    ss_render(e, ir_child(f, NODE_A(n)), 1, &cpre, &ctxt);
    e->indent--;

    if (cpre.len == 0) {
        ss_indent(e); ss_str(e, e->d->kw_while[0]);
        ss_take(e->out, &ctxt);
        ss_str(e, e->d->kw_while[1]); ss_ch(e, '\n');
        e->indent++;
        emit_loop_body(e, body, label);
        e->indent--;
        ss_indent(e); ss_str(e, e->d->kw_end); ss_ch(e, '\n');
    } else {
        // The condition hoists, and hoisting it ABOVE the loop would evaluate
        // it once. Re-testing inside a `while true` is the only correct place
        // for it.
        ss_indent(e); ss_str(e, e->d->kw_while[0]); ss_str(e, "true");
        ss_str(e, e->d->kw_while[1]); ss_ch(e, '\n');
        e->indent++;
        ss_take(e->out, &cpre);
        ss_indent(e); ss_str(e, e->d->kw_if[0]);
        ss_str(e, e->d->not_wrap[0]);
        ss_take(e->out, &ctxt);
        ss_str(e, e->d->not_wrap[1]);
        ss_str(e, e->d->kw_if[1]);
        ss_str(e, " break "); ss_str(e, e->d->kw_end); ss_ch(e, '\n');
        emit_loop_body(e, body, label);
        e->indent--;
        ss_indent(e); ss_str(e, e->d->kw_end); ss_ch(e, '\n');
    }
    ss_render_free(e, &cpre, &ctxt);
    e->loop_depth--;
}

static void emit_for(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *body = ir_child(f, NODE_B(n));
    IRNodeT *step = NODE_C(n) >= 0 ? ir_child(f, NODE_C(n)) : 0;

    if (e->d->has_ternary) {
        // JS has C's for, so `continue` still runs the step with no help.
        e->loop_label[e->loop_depth < SS_MAX_LOOPS ? e->loop_depth : 0] = -1;
        e->loop_depth++;
        ss_indent(e); ss_str(e, "for (; ");
        emit_cond(e, ir_child(f, NODE_A(n)));
        ss_str(e, "; ");
        if (step)
            for (int i = 0; i < NODE_N_ITEMS(step); i++) {
                if (i) ss_str(e, ", ");
                IRNodeT *it = ir_item(f, step, i);
                if (NODE_OP(it) == IR_ASSIGN) emit_assign_inline(e, it);
                else                          emit_expr(e, ir_child(f, NODE_A(it)));
            }
        ss_str(e, ") {\n");
        e->indent++;
        emit_block_stmts(e, body);
        e->indent--;
        ss_indent(e); ss_str(e, "}\n");
        e->loop_depth--;
        return;
    }

    // Lua has no for-with-step over a general condition, so the loop is
    // open-coded. The step goes after the `continue` label, which is what
    // makes `continue` run it -- the reason IR_FOR is its own op rather than
    // sugar over IR_WHILE.
    int label = ss_has_continue(f, body) ? e->tmp_next++ : -1;
    if (e->loop_depth < SS_MAX_LOOPS) e->loop_label[e->loop_depth] = label;
    e->loop_depth++;

    StrBuf cpre, ctxt;
    e->indent++;
    ss_render(e, ir_child(f, NODE_A(n)), 1, &cpre, &ctxt);
    e->indent--;

    ss_indent(e); ss_str(e, "while true do\n");
    e->indent++;
    ss_take(e->out, &cpre);
    ss_indent(e); ss_str(e, "if not (");
    ss_take(e->out, &ctxt);
    ss_str(e, ") then break end\n");
    emit_loop_body(e, body, label);
    if (step)
        for (int i = 0; i < NODE_N_ITEMS(step); i++) {
            IRNodeT *it = ir_item(f, step, i);
            ss_indent(e); ss_str(e, "do ");
            if (NODE_OP(it) == IR_ASSIGN) emit_assign_inline(e, it);
            else                          emit_expr(e, ir_child(f, NODE_A(it)));
            ss_str(e, " end\n");
        }
    e->indent--;
    ss_indent(e); ss_str(e, "end\n");
    ss_render_free(e, &cpre, &ctxt);
    e->loop_depth--;
}

static void emit_stmt_inner(ScriptEmit *e, IRNodeT *n, int is_last) {
    Func *f = e->cur_func;
    const ScriptDialect *d = e->d;

    switch (NODE_OP(n)) {
        case IR_BLOCK:
            ss_indent(e); ss_str(e, d->has_ternary ? "{\n" : "do\n");
            e->indent++;
            emit_block_stmts(e, n);
            e->indent--;
            ss_indent(e); ss_str(e, d->has_ternary ? "}\n" : "end\n");
            break;

        case IR_IF:
            ss_indent(e); ss_str(e, d->kw_if[0]);
            emit_cond(e, ir_child(f, NODE_A(n)));
            ss_str(e, d->kw_if[1]); ss_ch(e, '\n');
            e->indent++;
            emit_block_stmts(e, ir_child(f, NODE_B(n)));
            e->indent--;
            if (NODE_C(n) >= 0) {
                ss_indent(e); ss_str(e, d->kw_else); ss_ch(e, '\n');
                e->indent++;
                emit_block_stmts(e, ir_child(f, NODE_C(n)));
                e->indent--;
            }
            ss_indent(e); ss_str(e, d->kw_end); ss_ch(e, '\n');
            break;

        // Dropped by default, which is what lets a script that prints export to a
        // target with no print of its own. Handled at STATEMENT level so the drop
        // takes the string build feeding it too. The slice reaches VM.print as ONE
        // value (the view / subarray), not as C's (ptr, len) pair.
        case IR_PRINT:
            if (e->vm->run.print_emit != VM_PRINT_EMIT_CALL) break;
            ss_indent(e);
            ss_call(e, H_PRINT);
            emit_expr(e, ir_child(f, NODE_A(n)));
            ss_ch(e, ')');
            ss_str(e, d->stmt_end);
            ss_ch(e, '\n');
            break;

        case IR_WHILE:
            emit_while(e, n);
            break;

        case IR_FOR:
            emit_for(e, n);
            break;

        case IR_BREAK:
            ss_indent(e);
            ss_jump_open(e, is_last);
            ss_str(e, "break"); ss_str(e, d->stmt_end);
            ss_jump_close(e, is_last);
            ss_ch(e, '\n');
            break;

        case IR_CONTINUE: {
            ss_indent(e);
            if (d->has_ternary) { ss_str(e, "continue;\n"); break; }
            int label = e->loop_depth > 0 && e->loop_depth <= SS_MAX_LOOPS
                        ? e->loop_label[e->loop_depth - 1] : -1;
            if (label < 0) { ss_failf(e, "continue outside a loop", 0); ss_ch(e, '\n'); break; }
            ss_jump_open(e, is_last);
            ss_str(e, "goto __cont"); ss_int(e, label);
            ss_jump_close(e, is_last);
            ss_ch(e, '\n');
            break;
        }

        case IR_RETURN:
            ss_indent(e);
            ss_jump_open(e, is_last);
            ss_str(e, "return");
            if (NODE_A(n) >= 0) {
                ss_ch(e, ' ');
                // Copy site 3: returning an array returns a value.
                ss_emit_maybe_copy(e, ir_child(f, NODE_A(n)), f->ret_type);
            }
            ss_str(e, d->stmt_end);
            ss_jump_close(e, is_last);
            ss_ch(e, '\n');
            break;

        case IR_ASSIGN:
            ss_indent(e);
            // The declaration sank here from the prologue: this is the first
            // thing that touches the local, and it writes the whole value, so
            // zero-initialising it up front would only have been thrown away.
            if (n == e->decl_stmt) ss_str(e, d->kw_local);
            emit_assign_inline(e, n);
            ss_str(e, d->stmt_end);
            ss_ch(e, '\n');
            break;

        case IR_EXPR_STMT:
        case IR_CALL_STMT: {
            IRNodeT *v = ir_child(f, NODE_A(n));
            // print() is a void expression and arrives wrapped here; it has to
            // be handled as a statement so dropping it takes the whole line.
            if (NODE_OP(v) == IR_PRINT) { emit_stmt_inner(e, v, is_last); break; }
            ss_indent(e);
            if (!d->has_ternary && NODE_OP(v) != IR_CALL) {
                // Lua has no expression statement: only a call can stand alone.
                // Anything else discarded is a value, so bind it and drop it.
                ss_str(e, "local _ = ");
            }
            emit_expr(e, v);
            ss_str(e, d->stmt_end);
            ss_ch(e, '\n');
            break;
        }

        default:
            ss_failf(e, "unsupported statement node in the script backend", 0);
            break;
    }
}

// ============================================================================
// JavaScript dialect
// ============================================================================

static const char *JS_RESERVED[] = {
    "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "enum", "export", "extends", "false",
    "finally", "for", "function", "if", "import", "in", "instanceof", "let",
    "new", "null", "return", "static", "super", "switch", "this", "throw",
    "true", "try", "typeof", "var", "void", "while", "with", "yield",
    "await", "arguments", "eval", "undefined", "NaN", "Infinity",
    "Math", "Number", "Object", "Array", "String", "VM", "module", 0
};

static void js_arith(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *a = ir_child(f, NODE_A(n)), *b = ir_child(f, NODE_B(n));
    VTKind k = n->type.kind;
    int s = NODE_SUBOP(n);
    int i32 = (k == VMT_I32);

    if (i32) {
        switch (s) {
            case OP_ADD: case OP_SUB:
                ss_str(e, e->int_wrap ? "((" : "(");
                emit_expr(e, a);
                ss_str(e, s == OP_ADD ? " + " : " - ");
                emit_expr(e, b);
                ss_str(e, e->int_wrap ? ") | 0)" : ")");
                return;
            case OP_MUL:
                if (e->int_wrap) {
                    ss_str(e, "Math.imul(");
                    emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b);
                    ss_ch(e, ')');
                } else {
                    ss_ch(e, '('); emit_expr(e, a); ss_str(e, " * "); emit_expr(e, b); ss_ch(e, ')');
                }
                return;
            case OP_DIV:
                ss_call(e, H_TDIV); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
                return;
            case OP_DIV_NATIVE:
                ss_str(e, "(("); emit_expr(e, a); ss_str(e, " / "); emit_expr(e, b); ss_str(e, ") | 0)");
                return;
            case OP_MOD:
                ss_call(e, H_TMOD); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
                return;
            case OP_MOD_NATIVE:
                ss_str(e, "(("); emit_expr(e, a); ss_str(e, " % "); emit_expr(e, b); ss_str(e, ") | 0)");
                return;
            default: break;
        }
    }

    // f32 / f64. Every f32 node rounds: operands are already rounded (they
    // came from f32 nodes or a Float32Array), the result is not.
    ss_f32_open(e, n);
    if (s == OP_DIV) {
        ss_call(e, H_FDIV); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
    } else if (s == OP_MOD || s == OP_MOD_NATIVE) {
        // Unreachable today: vm.c lowers float `%` to an fmod() call, and
        // eval_binop has no float MOD arm either. Loud rather than plausible,
        // because JS's `%` and Lua's disagree about negative operands and each
        // disagrees with fmod.
        ss_failf(e, "float modulo reached the script backend", 0);
    } else {
        ss_ch(e, '(');
        emit_expr(e, a);
        ss_str(e, e->d->op[s]);
        emit_expr(e, b);
        ss_ch(e, ')');
    }
    ss_f32_close(e, n);
}

static void js_shift(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    int s = NODE_SUBOP(n);
    ss_ch(e, '(');
    emit_expr(e, ir_child(f, NODE_A(n)));
    ss_str(e, (s == OP_BSHR) ? " >> " : " << ");
    emit_expr(e, ir_child(f, NODE_B(n)));
    ss_ch(e, ')');
}

static void js_convert(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VTKind dst = n->type.kind, src = (VTKind)NODE_SUBOP(n);
    IRNodeT *a = ir_child(f, NODE_A(n));
    if (dst == VMT_I32) {
        if (src == VMT_I32) { ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')'); return; }
        if ((src == VMT_F32 || src == VMT_F64) && !(emit_func_flags(e->vm, f) & VM_FLAG_C_FLOAT_TO_INT)) {
            ss_call(e, H_F2I); emit_expr(e, a); ss_ch(e, ')');
            return;
        }
        ss_ch(e, '('); emit_expr(e, a); ss_str(e, " | 0)");
        return;
    }
    if (dst == VMT_F32) { ss_str(e, "Math.fround("); emit_expr(e, a); ss_ch(e, ')'); return; }
    ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')');
}

static void js_bitcast(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VTKind dst = n->type.kind, src = (VTKind)NODE_SUBOP(n);
    IRNodeT *a = ir_child(f, NODE_A(n));
    // Same kind means i32 <-> fxN: the shift is compile-time only, so the
    // operand's word already IS the result.
    if ((int)src == (int)dst) { ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')'); return; }
    if (src == VMT_I32 && dst == VMT_F32) { ss_call(e, H_BC_I32_F32); emit_expr(e, a); ss_ch(e, ')'); return; }
    if (src == VMT_F32 && dst == VMT_I32) { ss_call(e, H_BC_F32_I32); emit_expr(e, a); ss_ch(e, ')'); return; }
    ss_failf(e, "bitcast between i64 and f64 needs 64-bit integers; "
                "the js backend has none -- try '#rewire i64 -> f64'", 0);
}

static const char *js_array_ctor(VMType t) {
    if (t.pack_bits == 8)  return "Uint8Array";
    if (t.pack_bits == 16) return "Uint16Array";
    if (t.pack_bits)       return "Int32Array";      // u1/u2/u4 packed in words
    switch (ss_agg_elem(t.kind)) {
        case VMT_F32: return "Float32Array";
        case VMT_F64: return "Float64Array";
        default:      return "Int32Array";
    }
}

static void js_zero(ScriptEmit *e, VMType t) {
    if (ss_is_array(t.kind)) {
        int n = (t.pack_bits && !ss_pack_is_native(t.pack_bits))
                ? ss_pack_words(t.len, t.pack_bits) : t.len;
        ss_str(e, "new "); ss_str(e, js_array_ctor(t));
        ss_ch(e, '('); ss_int(e, n); ss_ch(e, ')');
        return;
    }
    if (ss_is_slice(t.kind)) {
        ss_str(e, "new "); ss_str(e, js_array_ctor(t)); ss_str(e, "(0)");
        return;
    }
    ss_ch(e, '0');
}

static void js_arr_lit(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VMType t = n->type;
    ss_str(e, js_array_ctor(t));
    ss_str(e, ".of(");
    if (t.pack_bits && !ss_pack_is_native(t.pack_bits)) {
        // u1/u2/u4: one OR-expression per backing word, each element emitted
        // exactly once so non-constant elements work too.
        int bits = t.pack_bits, epw = 32 / bits;
        int words = ss_pack_words(NODE_N_ITEMS(n), bits);
        for (int w = 0; w < words; w++) {
            if (w) ss_str(e, ", ");
            int first = 1;
            for (int k = 0; k < epw; k++) {
                int i = w * epw + k;
                if (i >= NODE_N_ITEMS(n)) break;
                if (!first) ss_str(e, " | ");
                first = 0;
                ss_str(e, "((");
                emit_expr(e, ir_item(f, n, i));
                ss_str(e, " & "); ss_int(e, (1 << bits) - 1);
                ss_str(e, ") << "); ss_int(e, k * bits);
                ss_ch(e, ')');
            }
            if (first) ss_ch(e, '0');
        }
    } else {
        for (int i = 0; i < NODE_N_ITEMS(n); i++) {
            if (i) ss_str(e, ", ");
            emit_expr(e, ir_item(f, n, i));
        }
    }
    ss_ch(e, ')');
}

// Every JS aggregate is a typed array, so array, slice and host buffer all
// index the same way and a subarray composes with a subarray.
static void js_index(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_child(f, NODE_A(n));
    IRNodeT *idx  = ir_child(f, NODE_B(n));
    int bits = (int)NODE_KI(n);
    if (bits && !ss_pack_is_native(bits)) {
        ss_call(e, H_PACK_GET);
        emit_expr(e, base);
        ss_str(e, ", "); emit_expr(e, idx);
        ss_str(e, ", "); ss_int(e, bits);
        ss_ch(e, ')');
        return;
    }
    if (NODE_OP(base) == IR_LOCAL) emit_local_name(e, (int)NODE_KI(base));
    else { ss_ch(e, '('); emit_expr(e, base); ss_ch(e, ')'); }
    ss_ch(e, '[');
    emit_expr(e, idx);
    ss_ch(e, ']');
}

static void js_store(ScriptEmit *e, IRNodeT *lv, IRNodeT *rv) {
    Func *f = e->cur_func;
    int bits = (int)NODE_KI(lv);
    IRNodeT *base = ir_child(f, NODE_A(lv));
    if (bits && !ss_pack_is_native(bits)) {
        ss_call(e, H_PACK_SET);
        emit_expr(e, base);
        ss_str(e, ", "); emit_expr(e, ir_child(f, NODE_B(lv)));
        ss_str(e, ", "); ss_int(e, bits);
        ss_str(e, ", "); emit_expr(e, rv);
        ss_ch(e, ')');
        return;
    }
    if (NODE_OP(base) == IR_LOCAL) emit_local_name(e, (int)NODE_KI(base));
    else { ss_ch(e, '('); emit_expr(e, base); ss_ch(e, ')'); }
    ss_ch(e, '[');
    emit_expr(e, ir_child(f, NODE_B(lv)));
    ss_str(e, "] = ");
    emit_expr(e, rv);
}

// A host buffer has no `.length` the emitter can trust -- it is whatever the
// target declared -- so the host names the expression, exactly as in C.
static void js_len(ScriptEmit *e, IRNodeT *base) {
    if (base->type.struct_id && ss_is_array(base->type.kind)) { ss_int(e, ss_agg_count(base->type)); return; }
    Func *f = e->cur_func;
    if (NODE_OP(base) == IR_LOCAL) {
        int slot = (int)NODE_KI(base);
        if (f->syms[slot].is_host_buf) {
            int id = ss_host_buf_id(e, slot);
            const char *lx = id >= 0 ? e->vm->host_bufs[id].c_len_expr : 0;
            if (lx) { ss_str(e, lx); return; }
            emit_local_name(e, slot);
            ss_str(e, "_len");
            return;
        }
        emit_local_name(e, slot);
        ss_str(e, ".length");
        return;
    }
    if (NODE_OP(base) == IR_DATA_SLICE) { ss_int(e, NODE_N_ITEMS(base)); return; }
    if (ss_is_array(base->type.kind))   { ss_int(e, base->type.len); return; }
    ss_ch(e, '('); emit_expr(e, base); ss_str(e, ").length");
}

static void js_slice(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_item(f, n, 0);
    IRNodeT *st   = ir_item(f, n, 1);
    IRNodeT *ln   = ir_item(f, n, 2);
    if (n->type.pack_bits && !ss_pack_is_native(n->type.pack_bits)) {
        ss_failf(e, "a u1/u2/u4 slice has no offset a script view can carry; "
                    "index the array directly, or use u8", 0);
    }
    ss_call(e, NODE_SUBOP(n) ? H_SUBU : H_SUB);
    emit_expr(e, base);
    ss_str(e, ", "); emit_expr(e, st);
    ss_str(e, ", "); emit_expr(e, ln);
    ss_ch(e, ')');
}

static void js_as_slice(ScriptEmit *e, IRNodeT *n) { emit_expr(e, n); }

static void js_copy(ScriptEmit *e, IRNodeT *n, VMType dst) {
    (void)dst;
    ss_call(e, H_ACOPY);
    emit_expr(e, n);
    ss_ch(e, ')');
}

static int js_supports(VTKind k) {
    // No BigInt: it is viral, it does not mix with Number, and every
    // arithmetic node would have to know which it is holding.
    return !(k == VMT_I64 || k == VMT_ARR_I64 || k == VMT_SLICE_I64);
}

static const ScriptDialect DIALECT_JS = {
    "js",
    { "if (", ") {" }, { "} else if (", ") {" }, "} else {", "}",
    { "while (", ") {" },
    "let ",
    { "function ", "(", ") {" },
    ";",
    "//",
    {
        [OP_ADD] = " + ", [OP_SUB] = " - ", [OP_MUL] = " * ", [OP_DIV] = " / ",
        [OP_MOD] = " % ", [OP_DIV_NATIVE] = " / ", [OP_MOD_NATIVE] = " % ",
        [OP_LT] = " < ", [OP_LE] = " <= ", [OP_GT] = " > ", [OP_GE] = " >= ",
        [OP_EQ] = " === ", [OP_NE] = " !== ",
        [OP_AND] = " && ", [OP_OR] = " || ",
        [OP_BAND] = " & ", [OP_BOR] = " | ", [OP_BXOR] = " ^ ",
        [OP_BSHL] = " << ", [OP_BSHL_NATIVE] = " << ", [OP_BSHR] = " >> ",
        [OP_NEG] = "-", [OP_NOT] = "!"
    },
    JS_RESERVED, 1, 1, 1,
    "\"use strict\";\nconst VM = {};\n",
    { "Math.fround(", ")" },
    { "(", " !== 0)" },
    { "(", " ? 1 : 0)" },
    { "!(", ")" },
    js_arith, js_shift, js_convert, js_bitcast, js_zero, js_arr_lit,
    js_index, js_store, js_slice, js_len, js_as_slice, js_copy, js_supports
};

// ============================================================================
// Lua dialect
// ============================================================================

static const char *LUA_RESERVED[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "global", "if", "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
    "math", "string", "table", "ipairs", "pairs", "select", "type", "VM", 0
};

static void lua_arith(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *a = ir_child(f, NODE_A(n)), *b = ir_child(f, NODE_B(n));
    VTKind k = n->type.kind;
    int s = NODE_SUBOP(n);

    if (k == VMT_I32 || k == VMT_I64) {
        int narrow = (k == VMT_I32) && e->int_wrap;
        switch (s) {
            case OP_ADD: case OP_SUB: case OP_MUL:
                if (narrow) ss_call(e, H_W32); else ss_ch(e, '(');
                emit_expr(e, a);
                ss_str(e, s == OP_ADD ? " + " : (s == OP_SUB ? " - " : " * "));
                emit_expr(e, b);
                ss_ch(e, ')');
                return;
            // Lua's // floors where C truncates, and integer //0 raises. Both
            // guarded and unguarded div go through the helper for that second
            // reason: a raise is not a wrong number, it is a dead process.
            case OP_DIV: case OP_DIV_NATIVE:
                ss_call(e, H_TDIV); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
                return;
            case OP_MOD: case OP_MOD_NATIVE:
                ss_call(e, H_TMOD); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
                return;
            default: break;
        }
    }

    if (s == OP_DIV) {
        ss_call(e, H_FDIV); emit_expr(e, a); ss_str(e, ", "); emit_expr(e, b); ss_ch(e, ')');
        return;
    }
    if (s == OP_MOD || s == OP_MOD_NATIVE) {
        // See the matching arm in js_arith: unreachable, and Lua's `%` floors.
        ss_failf(e, "float modulo reached the script backend", 0);
        return;
    }
    ss_ch(e, '(');
    emit_expr(e, a);
    ss_str(e, e->d->op[s]);
    emit_expr(e, b);
    ss_ch(e, ')');
}

static void lua_shift(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    int s = NODE_SUBOP(n);
    int i32 = (n->type.kind == VMT_I32);
    if (s == OP_BSHR) ss_call(e, i32 ? H_SAR32 : H_SAR64);
    else              ss_call(e, i32 ? H_SHL32 : H_SHL64);
    emit_expr(e, ir_child(f, NODE_A(n)));
    ss_str(e, ", ");
    emit_expr(e, ir_child(f, NODE_B(n)));
    ss_ch(e, ')');
}

static void lua_convert(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VTKind dst = n->type.kind, src = (VTKind)NODE_SUBOP(n);
    IRNodeT *a = ir_child(f, NODE_A(n));
    if (dst == VMT_I32) {
        if (src == VMT_I32)                       { ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')'); return; }
        if (src == VMT_I64)                       { ss_call(e, H_W32); emit_expr(e, a); ss_ch(e, ')'); return; }
        if (!(emit_func_flags(e->vm, f) & VM_FLAG_C_FLOAT_TO_INT)) { ss_call(e, H_F2I); emit_expr(e, a); ss_ch(e, ')'); return; }
        ss_call(e, H_W32); ss_call(e, H_TRUNC); emit_expr(e, a); ss_str(e, "))");
        return;
    }
    if (dst == VMT_I64) {
        if (src == VMT_I32 || src == VMT_I64)     { ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')'); return; }
        if (!(emit_func_flags(e->vm, f) & VM_FLAG_C_FLOAT_TO_INT)) { ss_call(e, H_F2L); emit_expr(e, a); ss_ch(e, ')'); return; }
        ss_call(e, H_TRUNC); emit_expr(e, a); ss_ch(e, ')');
        return;
    }
    // to f64. Lua tells integers from floats, and an integer leaking into
    // float code changes what // and % do to it later, so the widening is
    // spelled out rather than left to coercion.
    if (src == VMT_I32 || src == VMT_I64) { ss_ch(e, '('); emit_expr(e, a); ss_str(e, " + 0.0)"); return; }
    ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')');
}

static void lua_bitcast(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VTKind dst = n->type.kind, src = (VTKind)NODE_SUBOP(n);
    IRNodeT *a = ir_child(f, NODE_A(n));
    if ((int)src == (int)dst) { ss_ch(e, '('); emit_expr(e, a); ss_ch(e, ')'); return; }
    if (src == VMT_I64 && dst == VMT_F64) { ss_call(e, H_BC_I64_F64); emit_expr(e, a); ss_ch(e, ')'); return; }
    if (src == VMT_F64 && dst == VMT_I64) { ss_call(e, H_BC_F64_I64); emit_expr(e, a); ss_ch(e, ')'); return; }
    ss_failf(e, "bitcast to or from f32 is not supported by the lua backend; "
                "try '#rewire f32 -> f64'", 0);
}

static void lua_zero(ScriptEmit *e, VMType t) {
    if (ss_is_array(t.kind)) {
        int n = (t.pack_bits && !ss_pack_is_native(t.pack_bits))
                ? ss_pack_words(t.len, t.pack_bits) : t.len;
        int isf = !t.pack_bits && ss_agg_elem(t.kind) == VMT_F64;
        ss_call(e, isf ? H_ZEROSF : H_ZEROS);
        ss_int(e, n);
        ss_ch(e, ')');
        return;
    }
    if (ss_is_slice(t.kind)) { ss_call(e, H_EMPTY); ss_ch(e, ')'); return; }
    ss_str(e, (t.kind == VMT_F64 || t.kind == VMT_F32) ? "0.0" : "0");
}

// `{[0]=a, b, c}` puts a at index 0 and the positional entries at 1 and 2 --
// the whole 0-based literal in two tokens.
static void lua_arr_lit(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    VMType t = n->type;
    int packed = t.pack_bits && !ss_pack_is_native(t.pack_bits);
    int count = packed ? ss_pack_words(NODE_N_ITEMS(n), t.pack_bits) : NODE_N_ITEMS(n);
    if (count <= 0) { ss_str(e, "{}"); return; }

    ss_str(e, "{[0]=");
    if (packed) {
        int bits = t.pack_bits, epw = 32 / bits;
        for (int w = 0; w < count; w++) {
            if (w) ss_str(e, ", ");
            int first = 1;
            for (int k = 0; k < epw; k++) {
                int i = w * epw + k;
                if (i >= NODE_N_ITEMS(n)) break;
                if (!first) ss_str(e, " | ");
                first = 0;
                ss_str(e, "((");
                emit_expr(e, ir_item(f, n, i));
                ss_str(e, " & "); ss_int(e, (1 << bits) - 1);
                ss_str(e, ") << "); ss_int(e, k * bits);
                ss_ch(e, ')');
            }
            if (first) ss_ch(e, '0');
        }
    } else {
        for (int i = 0; i < NODE_N_ITEMS(n); i++) {
            if (i) ss_str(e, ", ");
            emit_expr(e, ir_item(f, n, i));
        }
    }
    ss_ch(e, '}');
}

// True when the value is a slice VIEW rather than a bare 0-based table. A host
// buffer is slice-TYPED in the VM but a plain table in the target, exactly as
// it is a plain array in emitted C.
static int lua_is_view(ScriptEmit *e, IRNodeT *n) {
    if (!ss_is_slice(n->type.kind)) return 0;
    if (NODE_OP(n) == IR_LOCAL && e->cur_func->syms[(int)NODE_KI(n)].is_host_buf) return 0;
    return 1;
}

// A name the emitted Lua may repeat. `X.a[X.off + i]` names the base twice, so
// anything that is not already a plain local is staged into a temporary first.
static const char *lua_base_ref(ScriptEmit *e, IRNodeT *base) {
    if (NODE_OP(base) == IR_LOCAL) {
        int slot = (int)NODE_KI(base);
        if (slot >= 0 && slot < e->n_local_names && e->local_names) return e->local_names[slot];
    }
    if (NODE_OP(base) == IR_DATA_SLICE) {
        int id = ss_data_id(e, base);
        char *out = (char *)mem_alloc(&e->vm->run.mem, 16);
        if (!out) return "__d0";
        int p = 0, v = id, dn = 0; char dg[8];
        out[p++] = '_'; out[p++] = '_'; out[p++] = 'd';
        if (v == 0) dg[dn++] = '0';
        while (v > 0) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
        while (dn > 0) out[p++] = dg[--dn];
        out[p] = '\0';
        return out;
    }
    return ss_stage(e, base);
}

static void lua_index(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_child(f, NODE_A(n));
    IRNodeT *idx  = ir_child(f, NODE_B(n));
    int bits = (int)NODE_KI(n);

    if (bits && !ss_pack_is_native(bits)) {
        ss_call(e, H_PACK_GET);
        ss_str(e, lua_base_ref(e, base));
        ss_str(e, ", "); emit_expr(e, idx);
        ss_str(e, ", "); ss_int(e, bits);
        ss_ch(e, ')');
        return;
    }
    const char *r = lua_base_ref(e, base);
    if (lua_is_view(e, base)) {
        ss_str(e, r); ss_str(e, ".a[");
        ss_str(e, r); ss_str(e, ".off + (");
        emit_expr(e, idx);
        ss_str(e, ")]");
        return;
    }
    ss_str(e, r); ss_ch(e, '[');
    emit_expr(e, idx);
    ss_ch(e, ']');
}

static void lua_store(ScriptEmit *e, IRNodeT *lv, IRNodeT *rv) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_child(f, NODE_A(lv));
    IRNodeT *idx  = ir_child(f, NODE_B(lv));
    int bits = (int)NODE_KI(lv);

    if (bits && !ss_pack_is_native(bits)) {
        ss_call(e, H_PACK_SET);
        ss_str(e, lua_base_ref(e, base));
        ss_str(e, ", "); emit_expr(e, idx);
        ss_str(e, ", "); ss_int(e, bits);
        ss_str(e, ", "); emit_expr(e, rv);
        ss_ch(e, ')');
        return;
    }
    const char *r = lua_base_ref(e, base);
    if (lua_is_view(e, base)) {
        ss_str(e, r); ss_str(e, ".a["); ss_str(e, r); ss_str(e, ".off + (");
        emit_expr(e, idx);
        ss_str(e, ")] = ");
    } else {
        ss_str(e, r); ss_ch(e, '[');
        emit_expr(e, idx);
        ss_str(e, "] = ");
    }
    // u8 / u16 are typed arrays in JS and plain table slots here, so the
    // narrowing that C and JS get for free has to be written out.
    if (bits == 8 || bits == 16) {
        ss_ch(e, '(');
        emit_expr(e, rv);
        ss_str(e, bits == 8 ? " & 0xFF)" : " & 0xFFFF)");
        return;
    }
    emit_expr(e, rv);
}

static void lua_len(ScriptEmit *e, IRNodeT *base) {
    if (base->type.struct_id && ss_is_array(base->type.kind)) { ss_int(e, ss_agg_count(base->type)); return; }
    Func *f = e->cur_func;
    if (NODE_OP(base) == IR_LOCAL) {
        int slot = (int)NODE_KI(base);
        if (f->syms[slot].is_host_buf) {
            int id = ss_host_buf_id(e, slot);
            const char *lx = id >= 0 ? e->vm->host_bufs[id].c_len_expr : 0;
            if (lx) { ss_str(e, lx); return; }
            emit_local_name(e, slot);
            ss_str(e, "_len");
            return;
        }
    }
    if (NODE_OP(base) == IR_DATA_SLICE) { ss_int(e, NODE_N_ITEMS(base)); return; }
    if (ss_is_array(base->type.kind))   { ss_int(e, base->type.len); return; }
    // `#t` is never emitted: on a 0-based table it is off by one, it is only
    // defined for hole-free 1..n sequences, and it is a border search rather
    // than a field read.
    ss_str(e, lua_base_ref(e, base));
    ss_str(e, ".len");
}

static void lua_slice(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_item(f, n, 0);
    IRNodeT *st   = ir_item(f, n, 1);
    IRNodeT *ln   = ir_item(f, n, 2);
    int known = NODE_SUBOP(n);
    if (n->type.pack_bits && !ss_pack_is_native(n->type.pack_bits)) {
        ss_failf(e, "a u1/u2/u4 slice has no offset a script view can carry; "
                    "index the array directly, or use u8", 0);
    }
    if (lua_is_view(e, base)) {
        ss_call(e, known ? H_SUBSU : H_SUBS);
        ss_str(e, lua_base_ref(e, base));
        ss_str(e, ", "); emit_expr(e, st);
        ss_str(e, ", "); emit_expr(e, ln);
        ss_ch(e, ')');
        return;
    }
    if (known) {
        ss_call(e, H_SUBAU);
        ss_str(e, lua_base_ref(e, base));
        ss_str(e, ", "); emit_expr(e, st);
        ss_str(e, ", "); emit_expr(e, ln);
        ss_ch(e, ')');
        return;
    }
    ss_call(e, H_SUBA);
    ss_str(e, lua_base_ref(e, base));
    ss_str(e, ", ");
    lua_len(e, base);
    ss_str(e, ", "); emit_expr(e, st);
    ss_str(e, ", "); emit_expr(e, ln);
    ss_ch(e, ')');
}

// A bare table has to become a view before anything that reads `.len` or
// `.off` touches it.
static void lua_as_slice(ScriptEmit *e, IRNodeT *n) {
    if (lua_is_view(e, n)) { emit_expr(e, n); return; }
    ss_call(e, H_VIEW);
    ss_str(e, lua_base_ref(e, n));
    ss_str(e, ", ");
    lua_len(e, n);
    ss_ch(e, ')');
}

static void lua_copy(ScriptEmit *e, IRNodeT *n, VMType dst) {
    int len = (dst.pack_bits && !ss_pack_is_native(dst.pack_bits))
              ? ss_pack_words(dst.len, dst.pack_bits) : dst.len;
    ss_call(e, H_ACOPY);
    emit_expr(e, n);
    ss_str(e, ", "); ss_int(e, len);
    ss_ch(e, ')');
}

static int lua_supports(VTKind k) {
    // Lua numbers are doubles at every level (lib/lua/src/luaconf.h); the only
    // faithful f32 emulation is a string.pack round-trip per node, which
    // allocates two strings per arithmetic op.
    return !(k == VMT_F32 || k == VMT_ARR_F32 || k == VMT_SLICE_F32);
}

static const ScriptDialect DIALECT_LUA = {
    "lua",
    { "if ", " then" }, { "elseif ", " then" }, "else", "end",
    { "while ", " do" },
    "local ",
    { "function ", "(", ")" },
    "",
    "--",
    {
        [OP_ADD] = " + ", [OP_SUB] = " - ", [OP_MUL] = " * ", [OP_DIV] = " / ",
        [OP_MOD] = " % ", [OP_DIV_NATIVE] = " / ", [OP_MOD_NATIVE] = " % ",
        [OP_LT] = " < ", [OP_LE] = " <= ", [OP_GT] = " > ", [OP_GE] = " >= ",
        [OP_EQ] = " == ", [OP_NE] = " ~= ",
        [OP_AND] = " and ", [OP_OR] = " or ",
        [OP_BAND] = " & ", [OP_BOR] = " | ", [OP_BXOR] = " ~ ",
        [OP_BSHL] = " << ", [OP_BSHL_NATIVE] = " << ", [OP_BSHR] = " >> ",
        [OP_NEG] = "-", [OP_NOT] = "not "
    },
    LUA_RESERVED, 0, 0, 0,
    "local VM = {}\n",
    { "", "" },                       // no single precision to round to
    { "(", " ~= 0)" },
    { "(", " and 1 or 0)" },
    { "not (", ")" },
    lua_arith, lua_shift, lua_convert, lua_bitcast, lua_zero, lua_arr_lit,
    lua_index, lua_store, lua_slice, lua_len, lua_as_slice, lua_copy, lua_supports
};

// ============================================================================
// Structs: an object (JS) or a table (Lua) per record
// ============================================================================
//
// A struct's bytes are never observable from script code (vm.c's guard list), so
// nothing here emulates them. Each struct the module uses gets six helpers, named
// after it: __new_T() builds a zeroed record, __copy_T(d, s) copies s into d in
// place and returns d, __clone_T(s) is a fresh copy, and the __newn_T / __copyn_T /
// __clonen_T trio does the same for n of them.

static void ss_struct_call(ScriptEmit *e, const char *what, VMType t) {
    ss_str(e, what);
    ss_str(e, emit_struct_name(e->vm, emit_struct_of(e->cur_func, t)));
    ss_ch(e, '(');
}

static int ss_is_lua(ScriptEmit *e) { return !e->d->has_ternary; }

// `.name`, or `["end"]` for a member Lua reserves.
static void ss_member(ScriptEmit *e, const char *name) {
    if (ss_is_lua(e) && es_is_reserved(e->d->reserved, name)) {
        ss_str(e, "[\""); ss_str(e, name); ss_str(e, "\"]");
        return;
    }
    ss_ch(e, '.');
    ss_str(e, name);
}

// An expression yielding the record a field is read from.
static void ss_emit_record_base(ScriptEmit *e, IRNodeT *base) {
    int op = NODE_OP(base);
    if (op == IR_LOCAL)                     { emit_local_name(e, (int)NODE_KI(base)); return; }
    if (op == IR_FIELD || op == IR_CALL)    { emit_expr(e, base); return; }
    ss_ch(e, '('); emit_expr(e, base); ss_ch(e, ')');
}

static void ss_emit_field_access(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_child(f, NODE_A(n));
    const VMStruct *st = emit_struct_of(f, base->type);
    const char *name = "__field";
    for (int i = 0; st && i < st->n_fields; i++)
        if (st->fields[i].offset == (int)NODE_KI(n)) { name = intern_get_cstr(e->vm->intern, st->fields[i].name); break; }
    ss_emit_record_base(e, base);
    ss_member(e, name);
}

// One record of an array of them. Element 0 of a ref is the ref.
static void ss_emit_elem(ScriptEmit *e, IRNodeT *n) {
    Func *f = e->cur_func;
    IRNodeT *base = ir_child(f, NODE_A(n));
    IRNodeT *idx  = ir_child(f, NODE_B(n));
    if (ss_is_ref_type(base->type)) { ss_emit_record_base(e, base); return; }
    if (!ss_is_lua(e)) {
        ss_emit_record_base(e, base);
        ss_ch(e, '[');
        emit_expr(e, idx);
        ss_ch(e, ']');
        return;
    }
    const char *r = lua_base_ref(e, base);
    if (lua_is_view(e, base)) {
        ss_str(e, r); ss_str(e, ".a["); ss_str(e, r); ss_str(e, ".off + (");
        emit_expr(e, idx);
        ss_str(e, ")]");
        return;
    }
    ss_str(e, r); ss_ch(e, '[');
    emit_expr(e, idx);
    ss_ch(e, ']');
}

// A struct-typed local's initial value.
static void ss_emit_struct_zero(ScriptEmit *e, VMType t) {
    if (ss_is_slice(t.kind)) { ss_str(e, ss_is_lua(e) ? "nil" : "null"); return; }
    ss_struct_call(e, t.inner_len ? "__newn_" : "__new_", t);
    if (t.inner_len) ss_int(e, ss_agg_count(t));
    ss_ch(e, ')');
}

#define SS_MAX_STRUCTS 64

typedef struct {
    Func       *owner[SS_MAX_STRUCTS];
    int         id[SS_MAX_STRUCTS];
    const char *name[SS_MAX_STRUCTS];
    int         n;
} SSStructs;

// Add a struct and, first, every struct its fields hold: helpers call each other,
// and a Lua `local function` has to be declared before it is named.
static void ss_structs_add(ScriptEmit *e, SSStructs *set, Func *owner, int id, int depth) {
    const VMStruct *st = vm_struct_get(owner->structs, id);
    if (!st || depth > 16) return;
    const char *nm = emit_struct_name(e->vm, st);
    for (int i = 0; i < set->n; i++) if (es_streq(set->name[i], nm)) return;
    for (int i = 0; i < st->n_fields; i++)
        if (st->fields[i].type.struct_id) ss_structs_add(e, set, owner, st->fields[i].type.struct_id, depth + 1);
    if (set->n >= SS_MAX_STRUCTS) { ss_failf(e, "too many structs for the script backend", 0); return; }
    set->owner[set->n] = owner;
    set->id[set->n]    = id;
    set->name[set->n]  = nm;
    set->n++;
}

static void ss_collect_structs(ScriptEmit *e, Func **list, int count, SSStructs *set) {
    set->n = 0;
    for (int fi = 0; fi < count; fi++) {
        Func *f = list[fi];
        if (f->ret_type.struct_id) ss_structs_add(e, set, f, f->ret_type.struct_id, 0);
        for (int si = 0; si < f->n_syms; si++)
            if (f->syms[si].type.struct_id) ss_structs_add(e, set, f, f->syms[si].type.struct_id, 0);
        for (int ni = 0; ni < f->n_nodes; ni++)
            if (f->nodes[ni].type.struct_id) ss_structs_add(e, set, f, f->nodes[ni].type.struct_id, 0);
    }
}

// `<head> <name>(<args>)<open>` in the dialect's function spelling.
static void ss_helper_open(ScriptEmit *e, const char *what, const char *sname, const char *args) {
    ss_str(e, ss_is_lua(e) ? "local function " : "function ");
    ss_str(e, what); ss_str(e, sname);
    ss_ch(e, '('); ss_str(e, args); ss_str(e, ss_is_lua(e) ? ")" : ") {");
}

// A zeroed record as a table / object literal. Nested records call their own
// __new_T when `use_helpers` is set; the shared-variable module has no helpers to
// call, so there they are spelled out too.
static void ss_emit_record_zero_lit(ScriptEmit *e, Func *owner, int id, int use_helpers) {
    const VMStruct *st = vm_struct_get(owner->structs, id);
    int lua = ss_is_lua(e);
    if (!st) { ss_str(e, "{}"); return; }
    ss_ch(e, '{');
    for (int i = 0; i < st->n_fields; i++) {
        const VMStructField *fd = &st->fields[i];
        const char *nm = intern_get_cstr(e->vm->intern, fd->name);
        ss_str(e, i ? ", " : " ");
        if (lua && es_is_reserved(e->d->reserved, nm)) { ss_str(e, "[\""); ss_str(e, nm); ss_str(e, "\"]"); }
        else ss_str(e, nm);
        ss_str(e, lua ? " = " : ": ");
        if (fd->type.struct_id && use_helpers) {
            ss_emit_struct_zero(e, fd->type);
        } else if (fd->type.struct_id && !fd->type.inner_len) {
            ss_emit_record_zero_lit(e, owner, fd->type.struct_id, 0);
        } else if (fd->type.struct_id) {
            int n = ss_agg_count(fd->type);
            if (lua) {
                ss_str(e, "(function() local a = {} for i = 0, "); ss_int(e, n - 1); ss_str(e, " do a[i] = ");
                ss_emit_record_zero_lit(e, owner, fd->type.struct_id, 0);
                ss_str(e, " end return a end)()");
            } else {
                ss_str(e, "Array.from({ length: "); ss_int(e, n); ss_str(e, " }, () => (");
                ss_emit_record_zero_lit(e, owner, fd->type.struct_id, 0);
                ss_str(e, "))");
            }
        }
        else if (fd->width == FIELD_AGG)      e->d->emit_zero(e, fd->type);
        else if (lua && (fd->type.kind == VMT_F32 || fd->type.kind == VMT_F64)) ss_str(e, "0.0");
        else                                  ss_ch(e, '0');
    }
    ss_str(e, st->n_fields ? " }" : "}");
}

static void ss_emit_struct_helpers(ScriptEmit *e, Func *owner, int id) {
    const VMStruct *st = vm_struct_get(owner->structs, id);
    const char *sn = emit_struct_name(e->vm, st);
    int lua = ss_is_lua(e);
    Func *saved = e->cur_func;
    e->cur_func = owner;

    // __new_T: every member at zero.
    ss_helper_open(e, "__new_", sn, "");
    ss_str(e, " return ");
    ss_emit_record_zero_lit(e, owner, id, 1);
    ss_str(e, lua ? " end\n" : "; }\n");

    // __copy_T: member by member, arrays element by element, nested in place.
    ss_helper_open(e, "__copy_", sn, "d, s");
    ss_ch(e, '\n');
    for (int i = 0; i < st->n_fields; i++) {
        const VMStructField *fd = &st->fields[i];
        const char *nm = intern_get_cstr(e->vm->intern, fd->name);
        ss_str(e, "    ");
        if (fd->type.struct_id) {
            ss_struct_call(e, fd->type.inner_len ? "__copyn_" : "__copy_", fd->type);
            ss_str(e, "d"); ss_member(e, nm); ss_str(e, ", s"); ss_member(e, nm);
            if (fd->type.inner_len) { ss_str(e, ", "); ss_int(e, ss_agg_count(fd->type)); }
            ss_ch(e, ')');
        } else if (fd->width == FIELD_AGG) {
            int n = (fd->type.pack_bits && !ss_pack_is_native(fd->type.pack_bits))
                    ? ss_pack_words(fd->type.len, fd->type.pack_bits) : fd->type.len;
            if (lua) {
                ss_str(e, "for i = 0, "); ss_int(e, n - 1); ss_str(e, " do d");
                ss_member(e, nm); ss_str(e, "[i] = s"); ss_member(e, nm); ss_str(e, "[i] end");
            } else {
                ss_str(e, "d"); ss_member(e, nm); ss_str(e, ".set(s"); ss_member(e, nm); ss_ch(e, ')');
            }
        } else {
            ss_str(e, "d"); ss_member(e, nm); ss_str(e, " = s"); ss_member(e, nm);
        }
        ss_str(e, lua ? "\n" : ";\n");
    }
    ss_str(e, lua ? "    return d\nend\n" : "    return d;\n}\n");

    ss_helper_open(e, "__clone_", sn, "s");
    ss_str(e, " return __copy_"); ss_str(e, sn); ss_str(e, "(__new_"); ss_str(e, sn); ss_str(e, "(), s)");
    ss_str(e, lua ? " end\n" : "; }\n");

    // The n-record trio: 0-based on both targets, as every array here is.
    ss_helper_open(e, "__newn_", sn, "n");
    if (lua) { ss_str(e, " local a = {} for i = 0, n - 1 do a[i] = __new_"); ss_str(e, sn); ss_str(e, "() end return a end\n"); }
    else     { ss_str(e, " const a = new Array(n); for (let i = 0; i < n; i++) a[i] = __new_"); ss_str(e, sn); ss_str(e, "(); return a; }\n"); }
    ss_helper_open(e, "__copyn_", sn, "d, s, n");
    if (lua) { ss_str(e, " for i = 0, n - 1 do __copy_"); ss_str(e, sn); ss_str(e, "(d[i], s[i]) end return d end\n"); }
    else     { ss_str(e, " for (let i = 0; i < n; i++) __copy_"); ss_str(e, sn); ss_str(e, "(d[i], s[i]); return d; }\n"); }
    ss_helper_open(e, "__clonen_", sn, "s, n");
    ss_str(e, " return __copyn_"); ss_str(e, sn); ss_str(e, "(__newn_"); ss_str(e, sn); ss_str(e, "(n), s, n)");
    ss_str(e, lua ? " end\n" : "; }\n");

    e->cur_func = saved;
}

// ============================================================================
// Unsupported-type pre-pass
// ============================================================================

// i64 arrives in the IR with no i64 anywhere in any signature: vm.c lowers
// fixed-point * and / through an i64 intermediate. So this cannot be a collect-time
// filter over signatures -- it has to walk NODES.
//
// Sets *declared when the offending kind was NAMED (a sym or a return type) rather
// than only appearing on an interior node: that is the difference between
// suggesting `#rewire i64 -> f64` and `#rewire i64 -> i32`.
static VTKind ss_scan_unsupported(Func **list, int count,
                                  const ScriptDialect *d,
                                  Func **which, int *declared, int *packed_slice) {
    *declared = 0;
    *packed_slice = 0;
    for (int fi = 0; fi < count; fi++) {
        Func *f = list[fi];
        if (!d->supports(f->ret_type.kind)) { *which = f; *declared = 1; return f->ret_type.kind; }
        for (int si = 0; si < f->n_syms; si++) {
            if (f->syms[si].is_func) continue;
            VMType t = f->syms[si].type;
            if (!d->supports(t.kind)) { *which = f; *declared = 1; return t.kind; }
            if (ss_is_slice(t.kind) && t.pack_bits && !ss_pack_is_native(t.pack_bits)) {
                *which = f; *packed_slice = 1; return t.kind;
            }
        }
        for (int ni = 0; ni < f->n_nodes; ni++) {
            IRNodeT *n = &f->nodes[ni];
            VTKind k = NODE_TYPE_KIND(n);
            if (!d->supports(k)) { *which = f; return k; }
            // IR_CVT and IR_BITCAST carry a SOURCE kind the node's own type
            // does not.
            int op = NODE_OP(n);
            if ((op == IR_CVT || op == IR_BITCAST) && !d->supports((VTKind)NODE_SUBOP(n))) {
                *which = f;
                return (VTKind)NODE_SUBOP(n);
            }
            if (op == IR_SLICE && n->type.pack_bits && !ss_pack_is_native(n->type.pack_bits)) {
                *which = f; *packed_slice = 1; return n->type.kind;
            }
        }
    }
    return VMT_VOID;
}

static const char *ss_kind_name(VTKind k) {
    switch (k) {
        case VMT_I32: return "i32";
        case VMT_I64: return "i64";
        case VMT_F32: return "f32";
        case VMT_F64: return "f64";
        case VMT_ARR_I64: case VMT_SLICE_I64: return "i64";
        case VMT_ARR_F32: case VMT_SLICE_F32: return "f32";
        default: return "that type";
    }
}

static void ss_set_error(VM *vm, const char *a, const char *b, const char *c) {
    int la = a ? es_strlen(a) : 0, lb = b ? es_strlen(b) : 0, lc = c ? es_strlen(c) : 0;
    char *out = (char *)mem_alloc(&vm->run.mem, (size_t)(la + lb + lc + 1));
    if (!out) { vm->run.last_error = "out of memory"; return; }
    int p = 0;
    for (int i = 0; i < la; i++) out[p++] = a[i];
    for (int i = 0; i < lb; i++) out[p++] = b[i];
    for (int i = 0; i < lc; i++) out[p++] = c[i];
    out[p] = '\0';
    vm->run.last_error = out;
}

// ============================================================================
// Function emitter
// ============================================================================

static void emit_func_header(ScriptEmit *e, Func *f, const char *cname) {
    ss_str(e, e->d->kw_func[0]);
    ss_str(e, cname);
    ss_str(e, e->d->kw_func[1]);
    for (int i = 0; i < f->n_params; i++) {
        if (i) ss_str(e, ", ");
        emit_local_name(e, f->param_slot[i]);
    }
    ss_str(e, e->d->kw_func[2]);
    ss_ch(e, '\n');
}

static void emit_func(ScriptEmit *e, Func *f, const char *cname) {
    Func *saved = e->cur_func;
    const char **saved_names = e->local_names;
    int saved_count = e->n_local_names;
    int saved_tmp = e->tmp_next;

    e->cur_func = f;
    e->indent   = 0;
    e->tmp_next = 0;
    e->int_wrap = (emit_func_flags(e->vm, f) & VM_FLAG_INT_WRAP) != 0;
    ss_build_local_names(e, f);

    emit_func_header(e, f, cname);
    e->indent = 1;

    IRNodeT  *body    = f->body >= 0 ? &f->nodes[f->body] : 0;
    int       n_stmt  = body ? NODE_N_ITEMS(body) : 0;
    char     *moved   = 0;
    IRNodeT **decl_of = 0;
    if (n_stmt > 0 && f->n_syms > 0) {
        moved   = (char *)mem_alloc(&e->vm->run.mem, (size_t)f->n_syms);
        decl_of = (IRNodeT **)mem_alloc(&e->vm->run.mem, sizeof(IRNodeT *) * (size_t)n_stmt);
        if (moved && decl_of) {
            for (int i = 0; i < f->n_syms; i++) moved[i] = 0;
            for (int i = 0; i < n_stmt; i++)    decl_of[i] = 0;
            emit_plan_decls(e->vm, f, body, n_stmt, 0, moved, decl_of);
        } else {
            moved = 0; decl_of = 0;    // no room to plan: every local declares here
        }
    }

    // Which symbols the EMITTED body still mentions -- the hidden locals of a
    // string build feeding a dropped print() are not among them.
    char *touched = 0;
    if (body && f->n_syms > 0) {
        touched = (char *)mem_alloc(&e->vm->run.mem, (size_t)f->n_syms);
        if (touched) {
            for (int k = 0; k < f->n_syms; k++) touched[k] = 0;
            emit_mark_slots(f, body, touched, f->n_syms, e->vm->run.print_emit != VM_PRINT_EMIT_CALL);
        }
    }

    // Locals are declared and zero-initialised up front, matching the VM's
    // zeroed frame -- except the ones whose declaration sank onto their first
    // assignment (emit_plan_decls), where the zero would be dead.
    int had_decl = 0;
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        if (s->is_func)               continue;
        if (s->is_global)             continue;   // module scope; see vm_emit_*_globals
        if (s->is_host_buf)           continue;   // the target declares it
        if (emit_is_param(f, i))        continue;
        if (s->type.kind == VMT_VOID) continue;
        if (moved && moved[i])        continue;
        if (touched && !touched[i])   continue;   // nothing emitted mentions it
        ss_indent(e);
        ss_str(e, e->d->kw_local);
        emit_local_name(e, i);
        ss_str(e, " = ");
        if (s->type.struct_id) ss_emit_struct_zero(e, s->type);
        else                   e->d->emit_zero(e, s->type);
        ss_str(e, e->d->stmt_end);
        ss_ch(e, '\n');
        had_decl = 1;
    }
    if (had_decl) ss_ch(e, '\n');

    for (int i = 0; i < n_stmt; i++) {
        e->decl_stmt = decl_of ? decl_of[i] : 0;
        emit_stmt(e, ir_item(f, body, i), i == n_stmt - 1);
    }
    e->decl_stmt = 0;

    e->indent = 0;
    ss_str(e, e->d->kw_end);
    ss_ch(e, '\n');

    e->cur_func     = saved;
    e->local_names  = saved_names;
    e->n_local_names = saved_count;
    e->tmp_next     = saved_tmp;
}

// The runtime half of the module: the `VM` namespace, then only those helpers the
// walk actually reached. Run AFTER the bodies are rendered -- the same reason the
// data-slice table is -- because until the walk is done there is no usage set to
// filter on. Deps always point at a LOWER id (SCRIPT_HELPERS' standing rule), so
// one reverse sweep closes the set and one forward sweep prints it in order.
static void ss_emit_prelude(ScriptEmit *e) {
    int lua = !e->d->has_ternary;

    for (int i = SS_N_HELPERS - 1; i >= 0; i--) {
        if (!e->helper_used[i]) continue;
        const short *deps = SCRIPT_HELPERS[i].deps;
        for (int k = 0; k < SS_MAX_HELPER_DEPS && deps[k] >= 0; k++)
            e->helper_used[deps[k]] = 1;
    }

    // The namespace itself is unconditional: a host attaches its own natives
    // to it, and a body calling one emits `VM.<name>(` with no helper behind
    // it.
    ss_str(e, e->d->module_head);
    for (int i = 0; i < SS_N_HELPERS; i++) {
        if (!e->helper_used[i]) continue;
        const char *text = lua ? SCRIPT_HELPERS[i].lua : SCRIPT_HELPERS[i].js;
        if (text) ss_str(e, text);
    }
}

// The module-level backing storage for every string / constant-data literal
// the walk collected. Emitted AFTER the bodies are rendered (the ids are only
// known then) and spliced in ahead of them by the caller.
static void emit_data_defs(ScriptEmit *e) {
    int lua = !e->d->has_ternary;
    for (int i = 0; i < e->n_data; i++) {
        const int *w = e->data_bytes[i] ? 0 : (const int *)e->data_ptr[i];
        const unsigned char *b = e->data_bytes[i] ? (const unsigned char *)e->data_ptr[i] : 0;
        int n = e->data_n[i];
        ss_str(e, e->d->kw_local);
        ss_str(e, "__d"); ss_int(e, i);
        ss_str(e, " = ");
        if (lua) {
            ss_str(e, "{ a = ");
            if (n <= 0) ss_str(e, "{}");
            else {
                ss_str(e, "{[0]=");
                for (int k = 0; k < n; k++) {
                    if (k) ss_str(e, ", ");
                    ss_int(e, b ? (long long)b[k] : (long long)w[k]);
                }
                ss_ch(e, '}');
            }
            ss_str(e, ", off = 0, len = "); ss_int(e, n); ss_str(e, " }");
        } else {
            ss_str(e, e->data_bytes[i] ? "new Uint8Array([" : "new Int32Array([");
            for (int k = 0; k < n; k++) {
                if (k) ss_str(e, ", ");
                ss_int(e, b ? (long long)b[k] : (long long)w[k]);
            }
            ss_str(e, "])");
        }
        ss_str(e, e->d->stmt_end);
        ss_ch(e, '\n');
    }
    if (e->n_data) ss_ch(e, '\n');
}

// ============================================================================
// Public API
// ============================================================================

static char *ss_emit(VM *vm, const ScriptDialect *d) {
    Func       *func_list  [MAX_EMIT_FUNCS];
    const char *func_cnames[MAX_EMIT_FUNCS];
    int lua = !d->has_ternary;

    int count = emit_collect_funcs(vm, func_list, MAX_EMIT_FUNCS);
    if (count < 0) return 0;
    emit_build_cnames(vm, func_list, count, func_cnames, d->reserved);

    // Refuse before writing anything: a mid-walk refusal leaves exactly the
    // half-emitted output this pass exists to prevent.
    if (count > 0) {
        Func *bad = 0; int declared = 0, packed_slice = 0;
        VTKind k = ss_scan_unsupported(func_list, count, d, &bad, &declared, &packed_slice);
        if (k != VMT_VOID) {
            if (packed_slice) {
                ss_set_error(vm, "a u1/u2/u4 slice is not supported by the ",
                             d->name, " backend; index the array directly, or use u8");
                return 0;
            }
            const char *kn = ss_kind_name(k);
            const char *fix;
            if (lua)            fix = "; try '#rewire f32 -> f64'";
            else if (declared)  fix = "; try '#rewire i64 -> f64' (exact only to 2^53)";
            else                fix = " -- it comes from this body's fixed-point multiply or "
                                      "divide, which vm.c lowers through i64; try "
                                      "'#rewire i64 -> i32' (or '#disable fx_i64_widening')";
            char msg[192];
            int p = 0;
            for (const char *s = kn; *s; s++) msg[p++] = *s;
            for (const char *s = " is not supported by the "; *s; s++) msg[p++] = *s;
            for (const char *s = d->name; *s; s++) msg[p++] = *s;
            for (const char *s = " backend"; *s; s++) msg[p++] = *s;
            msg[p] = '\0';
            ss_set_error(vm, msg, fix, 0);
            return 0;
        }
    }

    ScriptEmit e;
    vm->run.sys->memset(&e, 0, sizeof(e));
    e.vm          = vm;
    e.d           = d;
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = count;
    sb_init(&e.root, vm->run.sys);

    if (count == 0) {
        e.out = &e.root;
        ss_str(&e, d->comment);
        ss_str(&e, " No user-defined functions to emit.\n");
        if (!e.root.ok) { vm->run.sys->free(e.root.buf); return 0; }
        return e.root.buf;
    }

    // Bodies first, into a scratch buffer: the data-slice table is only
    // complete once the walk is done, and it has to be declared ahead of them.
    StrBuf bodies;
    sb_init(&bodies, vm->run.sys);
    e.out = &bodies;
    e.pre = &bodies;
    for (int i = 0; i < count; i++) {
        emit_func(&e, func_list[i], func_cnames[i]);
        if (i < count - 1) sb_char(&bodies, '\n');
    }

    if (e.err) {
        ss_set_error(vm, e.errmsg, 0, 0);
        vm->run.sys->free(bodies.buf);
        vm->run.sys->free(e.root.buf);
        return 0;
    }

    // The struct helpers, rendered before the prelude so a helper they call
    // (VM.zeros for an array member, say) is counted as used.
    StrBuf shelp;
    sb_init(&shelp, vm->run.sys);
    {
        SSStructs set;
        ss_collect_structs(&e, func_list, count, &set);
        e.out = &shelp;
        e.pre = &shelp;
        for (int i = 0; i < set.n; i++) ss_emit_struct_helpers(&e, set.owner[i], set.id[i]);
    }

    e.out = &e.root;
    ss_emit_prelude(&e);
    ss_ch(&e, '\n');
    if (shelp.len) {
        ss_take(&e.root, &shelp);
        ss_ch(&e, '\n');
    }
    vm->run.sys->free(shelp.buf);

    if (lua) {
        // Lua locals must exist before the functions that call each other
        // mention them, so every name is declared up front; `function f(...)`
        // then assigns to the local rather than creating a global.
        ss_str(&e, "local ");
        for (int i = 0; i < count; i++) {
            if (i) ss_str(&e, ", ");
            ss_str(&e, func_cnames[i]);
        }
        ss_ch(&e, '\n');
    }

    emit_data_defs(&e);
    ss_take(&e.root, &bodies);
    vm->run.sys->free(bodies.buf);

    ss_ch(&e, '\n');
    if (lua) {
        ss_str(&e, "return {");
        for (int i = 0; i < count; i++) {
            if (i) ss_str(&e, ",");
            ss_str(&e, "\n    ");
            ss_str(&e, func_cnames[i]);
            ss_str(&e, " = ");
            ss_str(&e, func_cnames[i]);
        }
        ss_str(&e, "\n}\n");
    } else {
        ss_str(&e, "const EXPORTS = {");
        for (int i = 0; i < count; i++) {
            if (i) ss_str(&e, ",");
            ss_str(&e, "\n    ");
            ss_str(&e, func_cnames[i]);
            ss_str(&e, ": ");
            ss_str(&e, func_cnames[i]);
        }
        ss_str(&e, "\n};\n");
        ss_str(&e, "if (typeof module !== \"undefined\") module.exports = EXPORTS;\n");
    }

    if (!e.root.ok) { vm->run.sys->free(e.root.buf); return 0; }
    return e.root.buf;
}

char *vm_emit_js (VM *vm) { return ss_emit(vm, &DIALECT_JS); }
char *vm_emit_lua(VM *vm) { return ss_emit(vm, &DIALECT_LUA); }

static char *ss_emit_globals(VM *vm, const ScriptDialect *d, const char *var_name) {
    ScriptEmit e;
    vm->run.sys->memset(&e, 0, sizeof(e));
    e.vm = vm;
    e.d  = d;
    sb_init(&e.root, vm->run.sys);
    e.out = &e.root;
    e.pre = &e.root;

    if (vm->n_globals <= 0) {
        ss_str(&e, d->comment);
        ss_str(&e, " No shared variables.\n");
        if (!e.root.ok) { vm->run.sys->free(e.root.buf); return 0; }
        return e.root.buf;
    }

    for (int i = 0; i < vm->n_globals; i++)
        if (!d->supports(vm->globals[i].type.kind)) {
            ss_set_error(vm, ss_kind_name(vm->globals[i].type.kind),
                         " is not supported by the ", d->name);
            vm->run.sys->free(e.root.buf);
            return 0;
        }

    // One object rather than loose module-level variables, for the reason
    // vm_emit_c_globals uses one struct: it lets a host declare several
    // independent INSTANCES of the same layout and point each VM at one with
    // vm_set_globals_c_prefix.
    // The table the struct-typed variables' ids index; see VM.globals_structs.
    Func gowner;
    vm->run.sys->memset(&gowner, 0, sizeof(gowner));
    gowner.structs = vm->globals_structs;
    e.cur_func = &gowner;

    ss_str(&e, d->kw_local);
    ss_str(&e, var_name);
    ss_str(&e, " = {\n");
    for (int i = 0; i < vm->n_globals; i++) {
        VMType gt = vm->globals[i].type;
        ss_str(&e, "    ");
        ss_str(&e, ss_global_name(vm, d, i));
        ss_str(&e, d->has_ternary ? ": " : " = ");
        if (vm->globals[i].struct_name && !gt.struct_id) {
            ss_set_error(vm, "shared variable '", ss_global_name(vm, d, i),
                         "' is a struct; emit the shared module from the VM that declared it");
            vm->run.sys->free(e.root.buf);
            return 0;
        }
        if (gt.struct_id && !gt.inner_len) {
            ss_emit_record_zero_lit(&e, &gowner, gt.struct_id, 0);
        } else if (gt.struct_id) {
            int n = ss_agg_count(gt);
            if (!d->has_ternary) {
                ss_str(&e, "(function() local a = {} for i = 0, "); ss_int(&e, n - 1); ss_str(&e, " do a[i] = ");
                ss_emit_record_zero_lit(&e, &gowner, gt.struct_id, 0);
                ss_str(&e, " end return a end)()");
            } else {
                ss_str(&e, "Array.from({ length: "); ss_int(&e, n); ss_str(&e, " }, () => (");
                ss_emit_record_zero_lit(&e, &gowner, gt.struct_id, 0);
                ss_str(&e, "))");
            }
        } else {
            d->emit_zero(&e, gt);
        }
        if (i < vm->n_globals - 1) ss_ch(&e, ',');
        ss_ch(&e, '\n');
    }
    ss_str(&e, "}");
    ss_str(&e, d->stmt_end);
    ss_ch(&e, '\n');

    if (!e.root.ok) { vm->run.sys->free(e.root.buf); return 0; }
    return e.root.buf;
}

char *vm_emit_js_globals (VM *vm, const char *var_name) {
    return ss_emit_globals(vm, &DIALECT_JS, var_name);
}
char *vm_emit_lua_globals(VM *vm, const char *var_name) {
    return ss_emit_globals(vm, &DIALECT_LUA, var_name);
}
