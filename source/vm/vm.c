// VM: typed IR tree-interpreter built from a parser ASTNode tree.
// Supports vars, fixed arrays, if/while/break/continue, and functions with
// typed args, return and recursion. There is no bytecode lowering step.

#include "vm.h"
#include "vm_types.h"
#include "vm_lib.h"
#include "vm_lib_accel.h"
#define VM_HAS_FUNC_ALLOC  // provided by this file, don't duplicate in vm_run.c
#include "parser/tokens.h"
#include "common/math_pure.h"
#include "common/string_pure.h"

// Forward declarations for functions defined in vm_run.c (included below).
static void    vm_set_error(VmRun *run, const char *msg);
static int     is_scalar(VTKind k);
static int     is_array(VTKind k);
static int     is_slice(VTKind k);
static VTKind  arr_elem(VTKind k);
static VTKind  arr_of(VTKind elem);
static VTKind  slice_of(VTKind elem);
static int     vt_size_of(VTKind k);
static int     vt_align_of(VTKind k);
static int     vt_slot_bytes(VMType t);
static inline int pack_bytes_for(int n_elems, int bits);
static int     op_is_compare(int op);
// Defined at the bottom of vm_run.c (included below): the compile-time fold
// runs through the interpreter's own eval_binop / cvt_to so a folded value
// cannot disagree with an evaluated one. See VM_FLAG_CONST_FOLD.
int vm_fold_binop(struct Func *f, int sub_op, VTKind k,
                  long long ai, double af, long long bi, double bf,
                  long long *oi, double *of);
int vm_fold_cvt(VTKind from, VTKind to, long long ii, double fi,
                long long *oi, double *of);
int vm_fold_unop(int sub_op, VTKind k, long long ii, double fi,
                 VTKind *out_kind, long long *oi, double *of);
int vm_fold_node(struct Func *f, int node, VTKind *out_kind,
                 long long *oi, double *of);
#include <stdint.h>
#include <stdarg.h>

// Forward declarations for directive handling (defined below).
static int  unpack_directive(ASTNode *node, ASTNode **out, int max);
static int  stmt_is_directive(ASTNode *node);
static ASTNode *directive_indent_block(ASTNode *node);
static int  compile_directive(VM *vm, ASTNode **chain, int n, ASTNode *stmt);

// Defined further down; used by the string-building code above its body.
static void stmt_push_idx(VM *vm, int **slot, int *count, int *cap, int n);
// str()/slice(): compiler intrinsics, dispatched from compile_call (above the
// definition, hence this declaration). *handled stays 0 for any other name.
static int compile_str_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
// print(...): the same arrangement, and it reuses str()'s string builder.
static int compile_print_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
// bitcast(T, x): the same arrangement, for the same reason.
static int compile_bitcast(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
// __ins(x, id) and inspect(x): the live editor's inspection. Same arrangement
// again -- and deliberately NOT under VM_HAS_INSPECT, because inspect(x) lives
// in the user's buffer and so must compile on every target the buffer reaches.
static int compile_inspect_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
// map(xs, f): unrolled at compile time, like every other vector expression.
static int compile_map_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
static int compile_vec_ctor(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled);
// Binds an operand to a hidden local so emitted C may name it twice; defined
// with the string builder, used by compile_row_slice above it.
static int stage_operand(VM *vm, Func *f, int ir, int **items, int *n, int *cap);
#if VM_REACTIVE
// A component call's argument list, split the way compile_call splits any call's.
static int expand_call_args(VM *vm, ASTNode *node, ASTNode *par, ASTNode **out, int max);
#endif


// ---------- small utils ----------

// Is `n` the node `anc`, or somewhere under it? Depth-capped: a malformed
// parent chain must not hang the compiler on an error path.
static int ast_is_within(ASTNode *n, ASTNode *anc) {
    for (int d = 0; n && d < 256; d++, n = n->parent)
        if (n == anc) return 1;
    return 0;
}

// First character offset the unparser attributed to `node`, or -1. A node that
// owns no text falls back to the first offset belonging to anything under it.
static long vm_node_offset(VM *vm, ASTNode *node) {
    if (!node) return -1;
    for (size_t off = 0; off < vm->num_txt_to_ref; off++)
        if (vm->txt_to_ref[off] == node) return (long)off;
    for (size_t off = 0; off < vm->num_txt_to_ref; off++)
        if (ast_is_within(vm->txt_to_ref[off], node)) return (long)off;
    return -1;
}

// Look up the source position of an ASTNode from the txt_to_ref / line_offsets
// stored in the VM.  Sets *row/*col (0-based).  Leaves them at -1 if unknown.
static void vm_node_position(VM *vm, ASTNode *node, int *row, int *col) {
    *row = -1; *col = -1;
    if (!node || !vm->txt_to_ref || vm->num_txt_to_ref == 0) return;
    // Walk up as far as needed: a node the compiler synthesised (a desugared
    // `+=`, a #define expansion) has no text of its own or below it, but the
    // statement it hangs off does.
    long off = -1;
    for (int d = 0; node && off < 0 && d < 256; d++, node = node->parent)
        off = vm_node_offset(vm, node);
    if (off < 0) return;
    if (vm->line_offsets && vm->num_lines_offsets > 0) {
        int l;
        for (l = vm->num_lines_offsets - 1; l >= 0; l--) {
            if (vm->line_offsets[l] <= (size_t)off) break;
        }
        if (l < 0) { *row = 0; *col = (int)off; }
        else       { *row = l + 1; *col = (int)((size_t)off - vm->line_offsets[l]); }
    } else { *row = 0; *col = (int)off; }
}

// Decimal digits of a non-negative int, no terminator. Returns the count.
static int vm_fmt_uint(char *buf, int n) {
    char tmp[12];
    int t = 0;
    do { tmp[t++] = (char)('0' + n % 10); n /= 10; } while (n && t < 11);
    for (int i = 0; i < t; i++) buf[i] = tmp[t - 1 - i];
    return t;
}

// Set an error and record the source position of the given ASTNode, falling back
// to vm->err_ctx -- the innermost statement being compiled -- when it maps nowhere.
static void vm_set_error_at(VM *vm, ASTNode *node, const char *msg) {
    // A failed shared-struct binding already said what is wrong; whatever the
    // compile reports on its way out is a consequence of it. See VM.bind_failed.
    if (vm->bind_failed) return;
    // Inside a built-in written in the language (vm_lib.h), the node belongs to
    // text the user cannot see, and txt_to_ref maps THEIR parse -- so a position
    // lookup would land on an unrelated line. Report the call site instead, and
    // say which built-in failed. See VM.lib_depth.
    char libbuf[256];
    if (vm->lib_depth > 0) {
        const char *nm = vm->lib_name ? intern_get_cstr(vm->intern, vm->lib_name) : 0;
        int p = 0;
        if (nm) {
            const char *pre = "in built-in ";
            while (*pre && p < (int)sizeof(libbuf) - 4) libbuf[p++] = *pre++;
            while (*nm  && p < (int)sizeof(libbuf) - 4) libbuf[p++] = *nm++;
            libbuf[p++] = ':'; libbuf[p++] = ' ';
        }
        int i = 0;
        while (msg[i] && p < (int)sizeof(libbuf) - 1) libbuf[p++] = msg[i++];
        libbuf[p] = 0;
        msg  = libbuf;
        node = vm->lib_at;
    }
    int r, c;
    vm_node_position(vm, node, &r, &c);
    if (r < 0 && vm->err_ctx && vm->err_ctx != node)
        vm_node_position(vm, vm->err_ctx, &r, &c);
    vm->run.error_row = r;
    vm->run.error_col = c;
    if (r >= 0 && c >= 0) {
        // Format as "row:col: message" so the position is always in the string
        // for callers that only print it. Row and col are 1-based in messages.
        char buf[256];
        int p = 0;
        p += vm_fmt_uint(buf + p, r + 1);
        buf[p++] = ':';
        p += vm_fmt_uint(buf + p, c + 1);
        buf[p++] = ':';
        buf[p++] = ' ';
        int i = 0;
        while (msg[i] && p < (int)sizeof(buf) - 1) buf[p++] = msg[i++];
        buf[p] = 0;
        vm_set_error(&vm->run, buf);
    } else {
        vm_set_error(&vm->run, msg);
    }
}

static void vm_errorf_at(VM *vm, ASTNode *node, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    s_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    vm_set_error_at(vm, node, buf);
}

// ---------- directive state ----------

typedef enum { REWIRE_BOTH = 0, REWIRE_TYPE, REWIRE_LITERAL } RewireScope;

static VTKind rewire_apply(VM *vm, VTKind k, RewireScope where) {
    if ((int)k < VMT_I32 || (int)k > VMT_I64) return k;
    VTKind m = (VTKind)k;
    if (where == REWIRE_TYPE)        m = vm->type_map[(int)k];
    if (where == REWIRE_LITERAL || where == REWIRE_BOTH)
                                      m = vm->literal_map[(int)k];
    return m;
}

static int rewire_shift(VM *vm, VTKind k, RewireScope where) {
    if ((int)k < VMT_I32 || (int)k > VMT_I64) return 0;
    if (where == REWIRE_TYPE) return vm->type_shift[(int)k];
    if (where == REWIRE_LITERAL || where == REWIRE_BOTH) return vm->literal_shift[(int)k];
    return 0;
}

// True when fixed-point arithmetic may use an i64 intermediate: the target
// still has i64 to widen into, and the extra precision has not been switched
// off. The two are separate questions -- see VM_FLAG_FX_I64_WIDE.
static int fx_wide_ok(VM *vm) {
    return (vm->flags & VM_FLAG_FX_I64_WIDE)
        && rewire_apply(vm, VMT_I64, REWIRE_TYPE) == VMT_I64;
}

static VTKind ident_to_vtkind(VM *vm, ASTNode *n, int *shift_out);

static void set_rewire(VM *vm, VTKind from, VTKind to, int shift, RewireScope scope) {
    if (scope == REWIRE_TYPE || scope == REWIRE_BOTH) {
        vm->type_map[(int)from] = to;
        vm->type_shift[(int)from] = shift;
    }
    if (scope == REWIRE_LITERAL || scope == REWIRE_BOTH) {
        vm->literal_map[(int)from] = to;
        vm->literal_shift[(int)from] = shift;
    }
}

static void snapshot_save(VM *vm) {
    if (vm->dir_stack_depth >= VM_MAX_DIRECTIVE_STACK) return;
    DirectiveSnapshot *snap = &vm->dir_stack[vm->dir_stack_depth++];
    vm->run.sys->memcpy(snap->type_map,    vm->type_map,    sizeof(vm->type_map));
    vm->run.sys->memcpy(snap->literal_map, vm->literal_map, sizeof(vm->literal_map));
    vm->run.sys->memcpy(snap->type_shift, vm->type_shift, sizeof(vm->type_shift));
    vm->run.sys->memcpy(snap->literal_shift, vm->literal_shift, sizeof(vm->literal_shift));
    snap->flags = vm->flags;
    snap->defines = vm->defines;
}

static void snapshot_restore(VM *vm) {
    if (vm->dir_stack_depth == 0) return;
    DirectiveSnapshot *snap = &vm->dir_stack[--vm->dir_stack_depth];
    vm->run.sys->memcpy(vm->type_map,    snap->type_map,    sizeof(vm->type_map));
    vm->run.sys->memcpy(vm->literal_map, snap->literal_map, sizeof(vm->literal_map));
    vm->run.sys->memcpy(vm->type_shift, snap->type_shift, sizeof(vm->type_shift));
    vm->run.sys->memcpy(vm->literal_shift, snap->literal_shift, sizeof(vm->literal_shift));
    vm->flags = snap->flags;
    vm->defines = snap->defines;
}

// C's promotion ladder, before the rewire. Split out so promote_t can ask what
// type the rewire is mapping FROM -- the shift lives under that key, not under
// the mapped-to kind.
static VTKind promote_raw(VTKind a, VTKind b) {
    if (a == VMT_F64 || b == VMT_F64) return VMT_F64;
    if (a == VMT_F32 || b == VMT_F32) return VMT_F32;
    if (a == VMT_I64 || b == VMT_I64) return VMT_I64;
    return VMT_I32;
}

static VTKind promote(VM *vm, VTKind a, VTKind b) {
    return rewire_apply(vm, promote_raw(a, b), REWIRE_TYPE);
}

// promote(), as a whole type. A type rewire to fxN names a FIXED-POINT type, so
// the Q-shift has to come across with the kind; callers that build a result type
// from a bare VTKind get a RAW i32 instead and truncate both operands. Operands
// that already carry a shift keep deciding it themselves -- this only fills in
// the case where the shift could otherwise come from nowhere.
static VMType promote_t(VM *vm, VTKind a, VTKind b) {
    VTKind base = promote_raw(a, b);
    VMType t = {0};
    t.kind = rewire_apply(vm, base, REWIRE_TYPE);
    if (t.kind == VMT_I32) t.len = rewire_shift(vm, base, REWIRE_TYPE);
    return t;
}

// Fixed-point shift helpers. The shift is carried in VMType.len for VMT_I32
// scalars and in .elem_shift for array/slice elements (whose .len is the element
// count); these hide that split.
static int ct_shift(VMType t) {
    if (t.kind == VMT_F32) return -1;
    if (t.kind == VMT_F64) return -2;
    if (t.kind == VMT_I32) return t.len;
    if (is_array(t.kind) || is_slice(t.kind)) {
        VTKind e = arr_elem(t.kind);
        if (e == VMT_F32) return -1;
        if (e == VMT_F64) return -2;
        if (e == VMT_I32) return t.elem_shift;
    }
    return 0;
}
static VMType ct_set_shift(VMType t, int shift) {
    if (t.kind == VMT_I32) t.len = shift;
    else if ((is_array(t.kind) || is_slice(t.kind)) && arr_elem(t.kind) == VMT_I32)
        t.elem_shift = shift;
    return t;
}
static VMType ct_vmtype_clear_shift(VMType t) {
    if (t.kind == VMT_I32 || t.kind == VMT_F32 || t.kind == VMT_F64 || t.kind == VMT_I64) t.len = 0;
    t.elem_shift = 0;
    return t;
}

// The integer type a bitwise or shift operand lands in.  64-bit sources (i64,
// f64) keep their width; everything else -- i32, fxN, f32 -- becomes raw i32.
// fxN and the floats *rescale* on the way (fx16 1.5 becomes 1), they are not
// reinterpreted -- bitcast() is the operator for that.
static VTKind ct_bitwise_int_kind(VMType t) {
    return (t.kind == VMT_I64 || t.kind == VMT_F64) ? VMT_I64 : VMT_I32;
}

// Storage width in bits of a scalar type, or 0 for anything that isn't one
// (void, arrays, slices). fxN shares i32's storage, so the shift plays no
// part -- which is exactly why a raw-i32 <-> fxN bitcast costs nothing.
static int ct_storage_bits(VMType t) {
    if (t.kind == VMT_I32 || t.kind == VMT_F32) return 32;
    if (t.kind == VMT_I64 || t.kind == VMT_F64) return 64;
    return 0;
}

// Widening ladder for reassignment checks: raw i32 < i64 < f32 < f64. A
// fixed-point i32 (shift != 0) sits off the ladder entirely -- it's only
// "safe" against its own exact (kind, shift) pair, since a shift change
// rescales the represented value rather than just extending its range.
// Returns -1 for anything off the ladder (fixed-point, or non-scalar).
static int scalar_rank(VTKind kind, int shift) {
    if (kind == VMT_I32) return shift == 0 ? 0 : -1;
    if (kind == VMT_I64) return 1;
    if (kind == VMT_F32) return 2;
    if (kind == VMT_F64) return 3;
    return -1;
}

// True if reassigning a variable already typed `to` (kind/shift) with a
// value of type `from` (kind/shift) can lose information -- i.e. `from` is
// not on a widening path into `to`. Same (kind, shift) is never lossy.
static int is_lossy_assign(VTKind from_kind, int from_shift, VTKind to_kind, int to_shift) {
    if (from_kind == to_kind && from_shift == to_shift) return 0;
    int fr = scalar_rank(from_kind, from_shift), tr = scalar_rank(to_kind, to_shift);
    if (fr < 0 || tr < 0) return 1;
    return fr > tr;
}

// Short label for a scalar (kind, shift) pair used in lossy-assignment error
// messages: "i32", "i64", "f32", "f64", or "fx<N>" for fixed-point.
//
// Scalars only -- a whole VMType belongs in type_label. An aggregate kind that
// reached the fixed-point branch below would print as a bare "fx": its shift is
// ct_shift's f32/f64 sentinel (-1/-2), and the digit loop writes nothing for a
// negative one. Say so instead of inventing a type; no caller should get here.
static const char *scalar_type_label(VTKind kind, int shift, char *buf, int buf_sz) {
    if (kind == VMT_F64) return "f64";
    if (kind == VMT_F32) return "f32";
    if (kind == VMT_I64) return "i64";
    if (kind != VMT_I32) return "?";
    if (shift <= 0) return "i32";
    int p = 0;
    if (p < buf_sz - 1) buf[p++] = 'f';
    if (p < buf_sz - 1) buf[p++] = 'x';
    char tmp[12]; int t = 0; int v = shift;
    if (v == 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0 && p < buf_sz - 1) buf[p++] = tmp[--t];
    buf[p] = 0;
    return buf;
}

// Append the decimal spelling of `v` (>= 0) to buf at *p.
static void label_uint(char *buf, int buf_sz, int *p, int v) {
    char tmp[12]; int t = 0;
    if (v <= 0) tmp[t++] = '0';
    while (v > 0 && t < (int)sizeof(tmp)) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0 && *p < buf_sz - 1) buf[(*p)++] = tmp[--t];
}

// Short label for ANY type in a diagnostic: "i32", "fx16", "[6]u4", "[]i32". An
// aggregate prints its element the way the source spells it.
static const char *type_label(VMType t, char *buf, int buf_sz) {
    if (!is_array(t.kind) && !is_slice(t.kind))
        return scalar_type_label(t.kind, ct_shift(t), buf, buf_sz);
    int p = 0;
    // Spelled the way it is written: `const[]i32`, no space.
    if (t.is_const) for (const char *c = "const"; *c && p < buf_sz - 1; c++) buf[p++] = *c;
    if (p < buf_sz - 1) buf[p++] = '[';
    if (is_array(t.kind)) label_uint(buf, buf_sz, &p, t.len);
    if (p < buf_sz - 1) buf[p++] = ']';
    char ebuf[16];
    const char *el;
    if (t.pack_bits) {
        int q = 0;
        ebuf[q++] = 'u';
        label_uint(ebuf, (int)sizeof(ebuf), &q, t.pack_bits);
        ebuf[q] = 0;
        el = ebuf;
    } else {
        el = scalar_type_label(arr_elem(t.kind), t.elem_shift, ebuf, sizeof(ebuf));
    }
    for (int i = 0; el[i] && p < buf_sz - 1; i++) buf[p++] = el[i];
    buf[p] = 0;
    return buf;
}

// ---------- AST helpers ----------

static int is_ident(ASTNode *n) {
    return n && n->token != 0 && !n->number_flags && !n->left && !n->right
        && n->left_bracket == 0 && n->items.size == 0;
}
static int is_number(ASTNode *n) {
    return n && n->number_flags != NUM_NONE;
}
static int is_paren(ASTNode *n) {
    return n && n->left_bracket == TOK_LPAREN;
}
static int is_brace(ASTNode *n) {
    return n && n->left_bracket == TOK_LBRACE;
}
static int is_bracket(ASTNode *n) {
    return n && n->left_bracket == TOK_LBRACKET;
}
// `...x`, the spread. A prefix unary, so the operand hangs off ->right and
// ->left is empty -- which is also what tells it apart from the rest-parameter
// marker's annotated form, where the `:` is the top node (see param_name_of).
static int is_splat_node(ASTNode *n) {
    return n && n->token == TOK_DOTDOTDOT && !n->left && n->right;
}
// Is `n` the bare identifier `name`?  The parser has no keywords, so every
// construct the statement compiler recognizes is matched this way.
static int ident_is(VM *vm, ASTNode *n, const char *name) {
    if (!is_ident(n)) return 0;
    const char *s = intern_get_cstr(vm->intern, n->token);
    return s && s_strcmp(s, name) == 0;
}

// A sub-scope attachment: `head { body }` or `head` + an indented body, which the
// parser marks with the NULL token (see is_c_style_call in parser.c). Returns the
// block, or 0.
//
// The block sits inside the injected-bracket wrapper the implicit-call arg was
// parsed into, and is either a real `{...}` or an indent block. Requiring one on
// the right is what keeps this apart from the other NULL-token pair, `[N]type`.
static int is_block_node(ASTNode *n) {
    if (!n) return 0;
    // TOK_THEN / TOK_DO / TOK_ELSE are the one-line block openers (tokens.h):
    // brackets that the end of the statement closes, so they sit in the same
    // slot as `{` and reach here as ordinary blocks.
    return n->left_bracket == TOK_LBRACE || n->left_bracket == TOK_EMPTYSTRING
        || n->left_bracket == TOK_THEN || n->left_bracket == TOK_DO
        || n->left_bracket == TOK_ELSE;
}

// A block written with indentation rather than braces. Deliberately not the
// one-line openers: an indent block can be mere alignment, which flatten_stmt
// dissolves, while `then` and `do` were written on purpose.
static int is_indent_block(ASTNode *n) {
    return n && n->left_bracket == TOK_EMPTYSTRING;
}

static ASTNode *subscope_block(ASTNode *n) {
    if (!n || n->token != 0 || !n->left || !n->right) return 0;
    ASTNode *R = n->right;
    if (is_block_node(R)) return R;
    if (R->left_bracket != 0 || R->left || R->items.size == 0) return 0;
    ASTNode *first = *(ASTNode**)array_get(&R->items, 0);
    return is_block_node(first) ? first : 0;
}

static int is_subscope(ASTNode *n) { return subscope_block(n) != 0; }

// Unwrap parser's "chain continuation" wrappers: an (impl-call) with no
// left/right/brackets/number, holding a single item.
static ASTNode *unwrap_chain(ASTNode *n) {
    while (n && n->token == 0 && !n->number_flags
        && !n->left && !n->right && n->left_bracket == 0
        && n->items.size == 1) {
        n = *(ASTNode**)array_get(&n->items, 0);
    }
    return n;
}

// Strip single-item () paren wrapping to expose the inner expression.
static ASTNode *paren_inner(ASTNode *n) {
    while (is_paren(n) && n->items.size == 1)
        n = *(ASTNode**)array_get(&n->items, 0);
    return n;
}

// `i = i / 2` (or `i /= 2`) on an integer: the fix is nearly always `/~`.
static const char *int_div_hint(ASTNode *rhs, VTKind to_kind, int to_shift) {
    rhs = paren_inner(rhs);
    if (!rhs || rhs->token != TOK_SLASH || !rhs->left || !rhs->right) return "";
    if (!((to_kind == VMT_I32 && to_shift == 0) || to_kind == VMT_I64)) return "";
    return ". For integer division use '/~'";
}

// `...[a, b]` -- a spread whose operand is written out as a flat array literal
// right there. Returns the bracket node, or 0.
//
// This one case is a purely SYNTACTIC expansion: `f(...[a, b])` is `f(a, b)`
// for any callee, because the elements are already argument-shaped AST nodes of
// the current parse. Worth recognizing separately from the type-driven
// expansion, for two reasons. It emits `f(a, b)` instead of building a hidden
// array and reading three constant subscripts back out of it; and it is the
// only spread a REACTIVE component call can take, because there each argument
// becomes a separately compiled live statement built from an AST node, and
// there is no AST node for `x[0]`.
static ASTNode *splat_literal(ASTNode *n) {
    if (!is_splat_node(n)) return 0;
    ASTNode *lit = paren_inner(n->right);
    if (!is_bracket(lit) || lit->items.size == 0) return 0;
    ASTNode *first = paren_inner(*(ASTNode**)array_get(&lit->items, 0));
    // A nested literal's rows are not arguments; leave those to the typed path.
    if (is_bracket(first)) return 0;
    // `[]` arrives as one blank item, exactly as `()` does.
    if (lit->items.size == 1 && !first->left && !first->right && !first->number_flags
        && first->token == 0 && first->string == 0 && first->items.size == 0
        && first->left_bracket == 0)
        return 0;
    return lit;
}

// A "text" / 'text' literal node. The content id sits in ->string, but that
// field is 0 for identifiers and the quote id shares a union with
// number_as_token, so both halves have to be checked.
static int is_string_literal(ASTNode *n) {
    return n && n->string != 0
        && TOKEN_IS_QUOTE(n->string_quoting);
}

// Adjacent string literals concatenate into one, C-style:
//     "hi t" "here"  ==  "hi there"
// Quoting is per fragment, so `"hi t" 'here'` is the same string.
//
// Adjacency reaches here as an implicit application: TOK_EMPTYSTRING with the
// literal on the left and the rest on the right, nested to the RIGHT for three or
// more fragments. Merged in the compiler rather than the parser so the AST keeps
// one node per source literal and unparse round-trips it byte for byte.
static int is_string_chain(ASTNode *n);
static int is_string_concat(ASTNode *n) {
    n = paren_inner(n);
    if (!n || n->token != TOK_EMPTYSTRING || !n->left || !n->right) return 0;
    return is_string_chain(n->left) && is_string_chain(unwrap_chain(n->right));
}
static int is_string_chain(ASTNode *n) {
    n = paren_inner(n);
    return is_string_literal(n) || is_string_concat(n);
}

// Strip a `= default` wrapper off a parameter node: `(a = 1)` parses as
// TOK_EQ{left=a, right=1} and `(a:i32 = 1)` as TOK_EQ{left=COLON{a,i32}, ...}
// since ':' binds tighter than '='. *out_def gets the default expression (0
// when the param has none); the return value is the bare `a` / `a:i32` node.
static ASTNode *param_split_default(ASTNode *p, ASTNode **out_def) {
    *out_def = 0;
    if (p && p->token == TOK_EQ && p->left && p->right) {
        *out_def = p->right;
        return paren_inner(p->left);
    }
    return p;
}

// `...` peeled off a rest parameter, marking that it was there. `...rest`
// arrives as the unary node itself; `...rest: []i32` does NOT, because `:`
// (prec 7) binds looser than the unary `...` (21) and ends up the top node with
// the marker inside its left. So the peel has to run on both sides of the
// colon, which is what param_name_of below does.
static ASTNode *param_peel_rest(ASTNode *p, int *is_rest) {
    if (p && p->token == TOK_DOTDOTDOT && !p->left && p->right) {
        if (is_rest) *is_rest = 1;
        return paren_inner(p->right);
    }
    return p;
}

// The identifier naming a parameter, with any rest marker peeled, or 0 when the
// node is not a parameter shape at all. `*is_rest` is set when the marker was
// there. Takes the node with its `= default` wrapper already split off.
static ASTNode *param_name_of(ASTNode *p, int *is_rest) {
    *is_rest = 0;
    p = param_peel_rest(p, is_rest);
    if (p && p->token == TOK_COLON && p->left && p->right) {
        ASTNode *l = param_peel_rest(paren_inner(p->left), is_rest);
        return is_ident(l) ? l : 0;
    }
    return is_ident(p) ? p : 0;
}

// The type-annotation node of a parameter, or 0 when it is unannotated.
static ASTNode *param_type_of(ASTNode *p) {
    int rest = 0;
    p = param_peel_rest(p, &rest);
    return (p && p->token == TOK_COLON && p->left && p->right) ? p->right : 0;
}

// The head of a type written as an implicit call, i.e. the part that swallows
// the comma after it: a bare ident (`ref my`) or `const[]` (which reads as a
// call on `const[]` for the same reason). A bracket-headed `[]i32` / `[2]i32` is
// not a call, so its comma stays a sibling and never reaches here.
static int type_head_is_call(ASTNode *h) {
    if (!h) return 0;
    if (is_ident(h)) return 1;
    return (h->token == TOK_EMPTYSTRING || h->token == 0)
        && is_ident(h->left) && is_bracket(h->right);
}

// `q: const[]i32 = a` -- a call-headed type swallows the INITIALISER as its
// argument, the same way it swallows a following parameter, so the element type
// arrives as the `=`'s left and the initialiser as its right. Both sides only
// read this shape; nothing rebuilds the node.
static ASTNode *type_arg_split_init(ASTNode *arg, ASTNode **init_out) {
    if (init_out) *init_out = 0;
    if (arg && arg->token == TOK_EQ && arg->left && arg->right) {
        if (init_out) *init_out = arg->right;
        return arg->left;
    }
    return arg;
}

// The initialiser a call-headed type annotation swallowed, or 0 when it did not.
static ASTNode *annotation_swallowed_init(ASTNode *ty) {
    if (!ty || !(ty->token == TOK_EMPTYSTRING || ty->token == 0)) return 0;
    if (!type_head_is_call(ty->left) || !ty->right) return 0;
    ASTNode *w    = unwrap_chain(ty->right);
    ASTNode *init = 0;
    type_arg_split_init(w, &init);
    if (init) return init;
    if (!w || w->token != 0 || w->left || w->right || w->left_bracket != 0 || w->items.size < 1) return 0;
    type_arg_split_init(unwrap_chain(*(ASTNode**)array_get(&w->items, 0)), &init);
    return init;
}

// Append parameter node `p` to out[] -- and whatever else it swallowed. A type
// written as an implicit call takes the comma after it as another argument:
// `(a: ref my, b: int)` parses as `a: ref(my, b: int)`. The first argument is the
// type (parse_type reads only that one), the rest are the parameters that
// followed. Returns the new count, or max + 1 when there are too many.
static int param_list_add(ASTNode *p, ASTNode **out, int n, int max) {
    if (n >= max) return max + 1;
    out[n++] = p;
    ASTNode *def;
    ASTNode *bare = param_split_default(paren_inner(p), &def);
    if (!def && bare && bare->token == TOK_COLON && bare->right
        && bare->right->token == TOK_EMPTYSTRING && type_head_is_call(bare->right->left) && bare->right->right) {
        ASTNode *w = bare->right->right;
        if (w->token == 0 && !w->left && !w->right && w->left_bracket == 0 && w->items.size > 1)
            for (size_t k = 1; k < w->items.size && n <= max; k++)
                n = param_list_add(*(ASTNode**)array_get(&w->items, k), out, n, max);
    }
    return n;
}

// The parameter nodes of a `(...)` list, `= default` wrappers still on.
static int param_list(ASTNode *params, ASTNode **out, int max) {
    int n = 0;
    for (size_t i = 0; i < params->items.size && n <= max; i++)
        n = param_list_add(*(ASTNode**)array_get(&params->items, i), out, n, max);
    return n;
}

// The default expression for callee param `i`, or 0. Recovered from the
// stored param-list AST rather than a per-Func array.
static ASTNode *param_default_of(Func *f, int i) {
    ASTNode *ps = (ASTNode*)f->params_ast;
    if (!ps || !is_paren(ps) || i < 0) return 0;
    ASTNode *list[17];
    int n = param_list(ps, list, 16);
    if (i >= n || i >= 16) return 0;
    ASTNode *def;
    param_split_default(paren_inner(list[i]), &def);
    return def;
}

// Defined below; needed here to let a `const` name stand in a default value.
static VMConst  *const_find(VM *vm, InternID name);
static VMDefine *define_find(VM *vm, InternID name);

// A default value is compiled into the *caller*, so an identifier in it would
// silently capture a caller local rather than mean anything in the callee.
// Restrict defaults to identifier-free expressions.
//
// A `const` name is the exception, safe for the reason the rule exists: consts
// resolve ahead of the symbol table, and a default is compiled in the scope the
// `=>` was written in (compile_call), so the name cannot be captured. A `#define` is NOT allowed -- its body is re-compiled at
// each use, which is precisely the late binding this rejects.
static int default_is_const(VM *vm, ASTNode *n) {
    n = paren_inner(n);
    if (!n) return 0;
    if (n->left_bracket != 0) return 0;      // array/brace literal or multi-item paren
    if (n->number_flags != NUM_NONE) return 1;
    if (n->string) return 1;
    if (is_string_concat(n)) return 1;
    if (is_ident(n) && !define_find(vm, n->token) && const_find(vm, n->token)) return 1;
    if (!n->left && n->right && (n->token == TOK_MINUS || n->token == TOK_PLUS || n->token == TOK_BANG
                                 || n->token == TOK_SLASH))
        return default_is_const(vm, n->right);
    if (n->left && n->right && (TOKEN_IS_OPERATOR_EXCEPT_EQ(n->token) || n->token == TOK_TILDE))
        return default_is_const(vm, n->left) && default_is_const(vm, n->right);
    return 0;
}

// Collect the chain of arguments following a head identifier (e.g. after
// "if" or "while"). Writes up to `max` args into `out`, returns count.
// A node whose right child is an injected-bracket wrapper is already a
// complete no-paren call and is not split further -- `if sin a` is one
// condition, not the two args `sin` and `a`.
// kw_context == 1 when the head is a known keyword (if/while): there the one
// wrapper that IS still a step is one holding a block, which is the spaced
// `if (c) { b }` form -- see the comment on that test below.
static int collect_chain_args(ASTNode *rest, ASTNode **out, int max, int kw_context) {
    int n = 0;
    while (rest && n < max) {
        ASTNode *u = unwrap_chain(rest);
        if (!u) break;
        // A chain-step has shape: (impl-call) left=THIS_ARG right=NEXT.
        // Its NEXT is null or another chain-wrapper (no brackets). A call
        // like add(3,4) has the same left/right pair but `right` is a
        // paren node -- we must not split that.
        int right_is_injected = (u->right && u->right->left_bracket == 0
                                 && u->right->items.size > 0);
        // A keyword's wrapper holding a BLOCK is still a step: that is the
        // spaced `if (c) { b }` form, which has to flatten to [cond, body] for
        // compile_if to recognise and report it.  A wrapper holding an
        // expression is a no-paren call's argument list and must stay whole.
        if (kw_context && right_is_injected) {
            ASTNode *w0 = *(ASTNode**)array_get(&u->right->items, 0);
            if (w0 && is_block_node(w0)) right_is_injected = 0;
        }
        int is_step = (u->token == TOK_EMPTYSTRING && !right_is_injected
                       && u->right->left_bracket == 0);
        if (is_step) {
            out[n++] = u->left;
            rest = u->right;
            if (!rest) break;
        } else {
            out[n++] = u;
            break;
        }
    }
    return n;
}

// Returns 1 if tok names one of the built-in keywords handled by the VM's
// statement compiler (if / while / return).  Used to select chain-flattening
// vs. no-paren-call behaviour in find_kw_head.
static int is_vm_keyword(VM *vm, InternID tok) {
    const char *s = intern_get_cstr(vm->intern, tok);
    if (!s) return 0;
    // Only if/while split a condition from a block written beside it, which is
    // the spaced `if (c) { b }` form they exist to reject.  `return` takes a
    // single expression and must NOT split, so it is intentionally absent from
    // this list -- and so is `else`, whose args are already complete no-paren
    // calls (`if(c) {...}`) that must be kept whole.
    if (s[0]=='i' && s[1]=='f' && !s[2]) return 1;
    if (s[0]=='w' && s[1]=='h' && s[2]=='i' && s[3]=='l' && s[4]=='e' && !s[5]) return 1;
    return 0;
}

// Recognize a keyword-headed statement and flatten its arguments. Handles
// both `if (cond) { body }` (space-separated, parser builds a right-leaning
// implicit-call chain via items wrappers) and `if(cond) { body }` (no
// space, parser builds a left-leaning call-form). Returns the head ident
// node or NULL; on match, fills `out_args` with up to `max` args.
//
// A sub-scope body is peeled off first and appended as the last arg, so
// `if(c) {b}` yields [cond, body] whatever form the source used.
static ASTNode *find_kw_head(VM *vm, ASTNode *node, ASTNode **out_args, int *out_n, int max) {
    ASTNode *stack[16]; int sp = 0;
    ASTNode *els = 0;
    ASTNode *blk = subscope_block(node);
    if (blk) {
        node = node->left;
        // `if c then a else b` is two chained one-line blocks on one statement:
        // the outer is the else body, and the then body is on the head below it.
        // Only an `if` can be carrying one, so it is safe to append blindly.
        if (blk->left_bracket == TOK_ELSE) {
            els = blk;
            blk = subscope_block(node);
            if (blk) node = node->left;
        }
    }
    ASTNode *cur = node;
    // Walk down lefts; each step pushes the right as a positional arg.
    while (cur && cur->token == TOK_EMPTYSTRING) {
        if (sp >= 16) break;
        stack[sp++] = cur->right;
        cur = cur->left;
    }
    if (!cur || !is_ident(cur)) return 0;
    int kw = is_vm_keyword(vm, cur->token);
    // Reverse stack into a temp list, then expand chain-wrapper args inline.
    int n = 0;
    for (int i = sp - 1; i >= 0; i--) {
        ASTNode *a = stack[i];
        // Chain-wrapper: (impl) with items only, no l/r/brackets.
        if (a && a->token == 0 && !a->number_flags && !a->left && !a->right
            && a->left_bracket == 0 && a->items.size >= 1) {
            ASTNode *exp[16];
            int ne = collect_chain_args(a, exp, 16, kw);
            for (int j = 0; j < ne && n < max; j++) out_args[n++] = exp[j];
        } else if (n < max) {
            out_args[n++] = a;
        }
    }
    if (blk && n < max) out_args[n++] = blk;
    if (els && n < max) out_args[n++] = els;   // `if`'s one-line else body
    *out_n = n;
    return cur;
}

// `else` never reaches us attached to its `if`. The parser ends a statement at the
// `}` of a C-style `x(...) { ... }` call -- the general rule that lets
// `while(a) {} if(b) {}` be two statements without the parser knowing any keywords
// -- so `if(c) {a} else {b}` arrives as two SIBLING statements. Pairing them back
// up is this layer's job.
//
// Returns the `else` head node (or 0 when `stmt` is not an `else`) and fills
// out_args with:
//    else { b }              -> [ {b} ]
//    else if(c) {b} else {d} -> [ if(c) {b}, else {d} ]
static ASTNode *find_else_head(VM *vm, ASTNode *stmt, ASTNode **out_args, int *out_n, int max) {
    ASTNode *head = find_kw_head(vm, stmt, out_args, out_n, max);
    if (!head || !ident_is(vm, head, "else")) return 0;
    return head;
}

// Write a short human-readable description of `n` into buf (at most buf_sz bytes).
// For identifiers it writes 'name'; for literals/operators a short type string.
// Returns the number of characters written (not including null terminator).
static int node_describe(VM *vm, ASTNode *n, char *buf, int buf_sz) {
    int p = 0;
    if (!n) {
        const char *s = "<null>";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
        buf[p] = 0;
        return p;
    }
    if (is_ident(n)) {
        const char *nm = intern_get_cstr(vm->intern, n->token);
        if (!nm) nm = "?";
        buf[p++] = '\'';
        for (int i = 0; nm[i] && p < buf_sz - 2; i++) buf[p++] = nm[i];
        buf[p++] = '\'';
    } else if (is_number(n)) {
        int nt = n->number_flags & NUM_TYPE;
        const char *t = (nt == NUM_FLOAT)  ? "float literal" :
                        (nt == NUM_DOUBLE) ? "double literal" :
                        "integer literal";
        for (int i = 0; t[i] && p < buf_sz - 1; i++) buf[p++] = t[i];
    } else if (is_paren(n)) {
        const char *s = "parenthesized expression";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (is_block_node(n)) {
        const char *s = "block";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (is_bracket(n)) {
        const char *s = "array literal";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (n->token == TOK_ARROW_F) {
        const char *s = "arrow function";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (n->token == TOK_EMPTYSTRING) {
        const char *s = "implicit call";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (is_subscope(n)) {
        const char *s = "block";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    } else if (n->token != 0) {
        const char *nm = intern_get_cstr(vm->intern, n->token);
        if (nm) {
            buf[p++] = '\'';
            for (int i = 0; nm[i] && p < buf_sz - 2; i++) buf[p++] = nm[i];
            buf[p++] = '\'';
        } else {
            const char *s = "<unknown>";
            for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
        }
    } else {
        const char *s = "<unknown>";
        for (int i = 0; s[i] && p < buf_sz - 1; i++) buf[p++] = s[i];
    }
    buf[p] = 0;
    return p;
}

// ---------- IR construction ----------

// Room for `need` items in an array allocated from `mem`: a doubled copy (at
// least `first`), the old block left to the arena. Returns 0 when out of memory.
static int mem_grow(VM *vm, MemBackend *mem, void **items, int n, int *cap, int need, size_t elem, int first) {
    if (need <= *cap) return 1;
    int nc = *cap ? *cap * 2 : first;
    if (nc < need) nc = need;
    void *ns = mem_alloc(mem, elem * (size_t)nc);
    if (!ns) return 0;
    if (n) vm->run.sys->memcpy(ns, *items, elem * (size_t)n);
    *items = ns;
    *cap = nc;
    return 1;
}
#define MEM_GROW(vm, mem, arr, n, cap, need, first) \
    mem_grow((vm), (mem), (void **)&(arr), (n), &(cap), (need), sizeof(*(arr)), (first))

static int ir_new(VM *vm, struct Func *f, int op) {
    MEM_GROW(vm, &vm->run.mem, f->nodes, f->n_nodes, f->cap_nodes, f->n_nodes + 1, 16);
    int idx = f->n_nodes++;
    vm->run.sys->memset(&f->nodes[idx], 0, sizeof(IRNode));
    f->nodes[idx].op = op;
    f->nodes[idx].a = -1;
    f->nodes[idx].b = -1;
    f->nodes[idx].c = -1;
    f->nodes[idx].items_begin = -1;
    return idx;
}

// ---------- constant folding (VM_FLAG_CONST_FOLD / VM_FLAG_IDENTITY_ELIM) ----

static int ir_is_const(Func *f, int ir) {
    if (ir < 0) return 0;
    return f->nodes[ir].op == IR_CONST_I || f->nodes[ir].op == IR_CONST_F;
}

// The constant's value as a double, whichever node kind holds it. Fixed-point
// nodes keep a raw integer here, not a scaled real: that is what the operators
// act on, and what a folded result must be written back as.
static double ir_const_num(Func *f, int ir) {
    return f->nodes[ir].op == IR_CONST_F ? f->nodes[ir].kf : (double)f->nodes[ir].ki;
}

// A float operand's integer view is only read by integer ops; one that does not
// fit (inf, nan, 1e300) would be undefined to convert, so it reads as 0.
static long long ir_const_int(Func *f, int ir) {
    if (f->nodes[ir].op != IR_CONST_F) return f->nodes[ir].ki;
    double v = f->nodes[ir].kf;
    return (v > -9.2e18 && v < 9.2e18) ? (long long)v : 0;
}

static int ir_const_is_zero(Func *f, int ir) {
    if (!ir_is_const(f, ir)) return 0;
    return ir_const_num(f, ir) == 0.0;
}

static int ir_const_is(Func *f, int ir, double v) {
    if (!ir_is_const(f, ir)) return 0;
    return ir_const_num(f, ir) == v;
}

static int ir_const_i(VM *vm, Func *f, VMType t, long long v) {
    int c = ir_new(vm, f, IR_CONST_I);
    f->nodes[c].type = t;
    f->nodes[c].ki = v;
    return c;
}

static int ir_i32(VM *vm, Func *f, long long v) {
    VMType t = {0};
    t.kind = VMT_I32;
    return ir_const_i(vm, f, t, v);
}

// Build a constant node of type `t` holding a value the interpreter produced.
static int ir_const_of(VM *vm, Func *f, VMType t, long long vi, double vf) {
    int is_float = (t.kind == VMT_F32 || t.kind == VMT_F64);
    int c = ir_new(vm, f, is_float ? IR_CONST_F : IR_CONST_I);
    f->nodes[c].type = t;
    if (is_float) f->nodes[c].kf = (t.kind == VMT_F32) ? (float)vf : vf;
    else          f->nodes[c].ki = vi;
    return c;
}

// Can this op, on these constants, be folded without deciding an answer the target
// would have been free to decide differently? Division or modulo by a zero constant
// and out-of-range shift counts are refused: under `#disable safe_div_by_zero` those
// are C undefined behaviour, and picking a value for UB is worse than leaving the op
// where the target defines it.
static int fold_is_safe(Func *f, int sub_op, VTKind k, int r) {
    // A native integer divide by -1 is left alone too: folding INT_MIN / -1
    // would trap in the compiler itself.
    if ((sub_op == OP_DIV_NATIVE || sub_op == OP_MOD_NATIVE)
     && (k == VMT_I32 || k == VMT_I64) && ir_const_is(f, r, -1.0))
        return 0;
    if (sub_op == OP_DIV || sub_op == OP_MOD
     || sub_op == OP_DIV_NATIVE || sub_op == OP_MOD_NATIVE)
        return !ir_const_is_zero(f, r);
    // The checked shifts take the count mod the width (eval_binop), so any count
    // folds; only the native one is undefined past the width.
    if (sub_op == OP_BSHL_NATIVE) {
        double s = ir_const_num(f, r);
        int width = (k == VMT_I64) ? 64 : 32;
        return s >= 0 && s < width;
    }
    return 1;
}

// Fold `l <sub_op> r` when both are constants. Returns the new node, or -1 to
// say "emit the op". `t` is the result type the caller was going to give the
// IR_BINOP; comparisons pass their own i32 result type, which is why the
// operand kind is passed separately.
static int try_fold_binop(VM *vm, Func *f, int sub_op, int l, int r,
                          VTKind operand_kind, VMType t) {
    if (!(vm->flags & VM_FLAG_CONST_FOLD)) return -1;
    if (!ir_is_const(f, l) || !ir_is_const(f, r)) return -1;
    if (!fold_is_safe(f, sub_op, operand_kind, r)) return -1;
    long long oi = 0; double of = 0;
    if (!vm_fold_binop(f, sub_op, operand_kind,
                       ir_const_int(f, l), ir_const_num(f, l),
                       ir_const_int(f, r), ir_const_num(f, r), &oi, &of))
        return -1;
    return ir_const_of(vm, f, t, oi, of);
}

// Would dropping this subtree lose anything the program can observe? Needed because
// annihilation (`0 * x`) discards its other operand, unlike the identities.
// Conservative: every call counts, and so does a native (unchecked) divide, which
// can trap.
static int ir_has_side_effects(Func *f, int ir) {
    if (ir < 0) return 0;
    IRNode *n = &f->nodes[ir];
    switch (n->op) {
        case IR_CONST_I: case IR_CONST_F: case IR_LOCAL: case IR_DATA_SLICE:
            return 0;
        case IR_BINOP:
            if (n->sub_op == OP_DIV_NATIVE || n->sub_op == OP_MOD_NATIVE) return 1;
            break;
        case IR_UNOP: case IR_CVT: case IR_INDEX: case IR_SELECT:
        case IR_ARR_LIT: case IR_LEN: case IR_FIELD:
            break;
        default:
            // Calls, assignments, and anything statement-shaped that reached an
            // operand position.
            return 1;
    }
    if (ir_has_side_effects(f, n->a)) return 1;
    if (ir_has_side_effects(f, n->b)) return 1;
    if (ir_has_side_effects(f, n->c)) return 1;
    for (int i = 0; i < n->n_items; i++)
        if (ir_has_side_effects(f, f->child_indices[n->items_begin + i])) return 1;
    return 0;
}

// May `ir` stand in for a result of type `t` without a conversion? Guards the
// identity rewrites below, which return an operand in place of the whole op.
static int ir_type_matches(Func *f, int ir, VMType t) {
    VMType it = f->nodes[ir].type;
    return it.kind == t.kind && ct_shift(it) == ct_shift(t);
}

// The IR half of VM_FLAG_IDENTITY_ELIM: identities whose literal only became a
// constant once compiled, plus the annihilators, which the AST half cannot do
// at all because it never compiles the operand it would need to discard.
// Returns a node, or -1 for "emit the op".
static int try_simplify_binop(VM *vm, Func *f, int sub_op, int l, int r, VMType t) {
    if (!(vm->flags & VM_FLAG_IDENTITY_ELIM)) return -1;
    int lz = ir_const_is_zero(f, l), rz = ir_const_is_zero(f, r);
    int l1 = ir_const_is(f, l, 1.0),  r1 = ir_const_is(f, r, 1.0);

    // Annihilators: the surviving value is a constant and the other operand
    // goes away, so it has to be droppable. Floats are assumed finite, as a
    // deliberate choice: `0 * x` on a variable is the fold worth having, so
    // `0.0 * inf` is 0 rather than nan, and a zero's sign is not kept.
    switch (sub_op) {
        case OP_MUL:
            if ((lz && !ir_has_side_effects(f, r)) || (rz && !ir_has_side_effects(f, l)))
                return ir_const_of(vm, f, t, 0, 0.0);
            break;
        case OP_BAND:
            if ((lz && !ir_has_side_effects(f, r)) || (rz && !ir_has_side_effects(f, l)))
                return ir_const_of(vm, f, t, 0, 0.0);
            break;
        case OP_DIV:
            // Only the CHECKED op: 0/0 is already 0 there, so this decides
            // nothing. OP_DIV_NATIVE is undefined behaviour on a zero divisor
            // and is left where the target can define it.
            if (lz && !ir_has_side_effects(f, r)) return ir_const_of(vm, f, t, 0, 0.0);
            break;
        case OP_MOD:
            if (lz && !ir_has_side_effects(f, r)) return ir_const_of(vm, f, t, 0, 0.0);
            if (r1 && !ir_has_side_effects(f, l)) return ir_const_of(vm, f, t, 0, 0.0);
            break;
        case OP_BSHL: case OP_BSHL_NATIVE: case OP_BSHR:
            if (lz && !ir_has_side_effects(f, r)) return ir_const_of(vm, f, t, 0, 0.0);
            break;
        default: break;
    }

    // Identities: the other operand survives, so nothing is dropped and no
    // side-effect check is needed -- only a type check, since the op may have
    // been carrying a conversion.
    switch (sub_op) {
        case OP_ADD: case OP_BOR: case OP_BXOR:
            if (rz && ir_type_matches(f, l, t)) return l;
            if (lz && ir_type_matches(f, r, t)) return r;
            break;
        case OP_SUB:
            if (rz && ir_type_matches(f, l, t)) return l;
            break;
        case OP_MUL:
            if (r1 && ir_type_matches(f, l, t)) return l;
            if (l1 && ir_type_matches(f, r, t)) return r;
            break;
        case OP_DIV:
            if (r1 && ir_type_matches(f, l, t)) return l;
            break;
        case OP_BSHL: case OP_BSHL_NATIVE: case OP_BSHR:
            if (rz && ir_type_matches(f, l, t)) return l;
            break;
        default: break;
    }
    return -1;
}

// Build `l <sub_op> r`, folding or simplifying it away where that is possible.
// Every IR_BINOP that can carry a constant operand should be built through here
// rather than with a bare ir_new.
static int ir_binop(VM *vm, Func *f, int sub_op, int l, int r, VMType t) {
    if (l >= 0 && r >= 0) {
        int c = try_fold_binop(vm, f, sub_op, l, r, f->nodes[l].type.kind, t);
        if (c >= 0) return c;
        c = try_simplify_binop(vm, f, sub_op, l, r, t);
        if (c >= 0) return c;
    }
    int b = ir_new(vm, f, IR_BINOP);
    f->nodes[b].sub_op = sub_op;
    f->nodes[b].a = l;
    f->nodes[b].b = r;
    f->nodes[b].type = t;
    return b;
}

// Build `<sub_op> a`, folding a constant operand away.
static int ir_unop(VM *vm, Func *f, int sub_op, int a, VMType t) {
    if ((vm->flags & VM_FLAG_CONST_FOLD) && ir_is_const(f, a)) {
        VTKind ok = t.kind;
        long long oi = 0; double of = 0;
        if (vm_fold_unop(sub_op, f->nodes[a].type.kind,
                         ir_const_int(f, a), ir_const_num(f, a), &ok, &oi, &of))
            return ir_const_of(vm, f, t, oi, of);
    }
    int u = ir_new(vm, f, IR_UNOP);
    f->nodes[u].sub_op = sub_op;
    f->nodes[u].a = a;
    f->nodes[u].type = t;
    return u;
}

// Build `cond ? tv : fv`. A constant condition picks an arm outright: the other was
// never going to run, in the interpreter or in C, so no side-effect check is needed.
static int ir_select(VM *vm, Func *f, int cond, int tv, int fv, VMType t) {
    if ((vm->flags & VM_FLAG_CONST_FOLD) && ir_is_const(f, cond)) {
        int taken = ir_const_num(f, cond) != 0.0 ? tv : fv;
        if (ir_type_matches(f, taken, t)) return taken;
    }
    int sel = ir_new(vm, f, IR_SELECT);
    f->nodes[sel].a = cond;
    f->nodes[sel].b = tv;
    f->nodes[sel].c = fv;
    f->nodes[sel].type = t;
    return sel;
}

// A finished IR_CALL to a pure native whose arguments are all constants is a
// constant. Call it now and keep the answer -- the f32/f64/i32/fx variant was
// already chosen by the same code the call site would have used, so this is exact.
// Returns -1 to keep the call.
static int try_fold_native_call(VM *vm, Func *f, int ir) {
    if (!(vm->flags & VM_FLAG_CONST_FOLD)) return -1;
    if (f->nodes[ir].op != IR_CALL) return -1;
    VMType t = f->nodes[ir].type;
    if (t.kind != VMT_I32 && t.kind != VMT_I64 && t.kind != VMT_F32 && t.kind != VMT_F64)
        return -1;
    int fid = (int)f->nodes[ir].ki;
    if (fid < 0 || fid >= vm->run.n_func_ids) return -1;
    Func *callee = vm->run.funcs_by_id[fid];
    if (!callee || callee->native_tok == 0) return -1;
    int idx = callee->native_tok - TOK_MATHS_FIRST;
    if (idx < 0 || idx >= vm->run.cfunc_table_cap) return -1;
    CFuncEntry *e = &vm->run.cfunc_table[idx];
    if (!e->is_pure || e->wants_ctx || e->has_sig) return -1;
    for (int i = 0; i < f->nodes[ir].n_items; i++)
        if (!ir_is_const(f, f->child_indices[f->nodes[ir].items_begin + i])) return -1;

    VTKind ok = t.kind;
    long long oi = 0; double of = 0;
    if (!vm_fold_node(f, ir, &ok, &oi, &of)) return -1;
    return ir_const_of(vm, f, t, oi, of);
}

static int ir_alloc_items(VM *vm, struct Func *f, int n) {
    if (n <= 0) return -1;
    int need = f->n_child_indices + n;
    MEM_GROW(vm, &vm->run.mem, f->child_indices, f->n_child_indices, f->cap_child_indices, need, 16);
    int start = f->n_child_indices;
    f->n_child_indices = need;
    for (int i = start; i < need; i++) f->child_indices[i] = -1;
    return start;
}

// A comma over items[0..n) and then `tail` (none when < 0), valued as its last item.
static int ir_comma(VM *vm, Func *f, const int *items, int n, int tail, VMType t) {
    int total = n + (tail >= 0);
    int begin = ir_alloc_items(vm, f, total);
    if (n) vm->run.sys->memcpy(&f->child_indices[begin], items, sizeof(int) * n);
    if (tail >= 0) f->child_indices[begin + n] = tail;
    int comma = ir_new(vm, f, IR_COMMA);
    f->nodes[comma].items_begin = begin;
    f->nodes[comma].n_items     = total;
    f->nodes[comma].type        = t;
    return comma;
}

// An inline expansion whose operands had to be staged: the assignments have to
// run before the expression that reads them, so both hang off a comma whose
// value is the expansion. Returns `result` untouched when nothing was staged.
static int inline_comma(VM *vm, Func *f, int result, VMType t, int *items, int n_items) {
    return n_items ? ir_comma(vm, f, items, n_items, result, t) : result;
}

// What must outlive the statement being compiled: under a host's per-statement
// arena (vm_rx_use_arena), the backend from before it.
static MemBackend *long_mem(VM *vm) {
#if VM_REACTIVE
    if (vm->rx_arena_depth > 0 && vm->rx_home_mem.arena) return &vm->rx_home_mem;
#endif
    return &vm->run.mem;
}

// ---------- Symbol table ----------

#define SYM_INDEX_MIN 16

static VMSym *sym_find(Func *f, InternID name) {
    if (!f->sym_index || name == 0) {
        for (int i = 0; i < f->n_syms; i++)
            if (f->syms[i].name == name) return &f->syms[i];
        return 0;
    }
    for (unsigned h = ((unsigned)name * 2654435761u) & (unsigned)f->sym_index_mask; f->sym_index[h];
         h = (h + 1) & (unsigned)f->sym_index_mask) {
        VMSym *s = &f->syms[f->sym_index[h] - 1];
        if (s->name == name) return s;
    }
    return 0;
}

// First one in wins, matching the scan: a later sym of the same name is shadowed.
static void sym_index_put(Func *f, int i) {
    InternID name = f->syms[i].name;
    if (name == 0) return;
    unsigned h = ((unsigned)name * 2654435761u) & (unsigned)f->sym_index_mask;
    for (; f->sym_index[h]; h = (h + 1) & (unsigned)f->sym_index_mask)
        if (f->syms[f->sym_index[h] - 1].name == name) return;
    f->sym_index[h] = i + 1;
}

static VMSym *sym_add(VM *vm, Func *f, InternID name, VMType type) {
    int old_cap = f->cap_syms;
    MEM_GROW(vm, &vm->run.mem, f->syms, f->n_syms, f->cap_syms, f->n_syms + 1, 8);
    // Twice the sym capacity, so the table is never more than half full.
    if (f->cap_syms != old_cap && f->cap_syms >= SYM_INDEX_MIN) {
        int n = SYM_INDEX_MIN;
        while (n < f->cap_syms * 2) n *= 2;
        f->sym_index = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)n);
        if (f->sym_index) {
            vm->run.sys->memset(f->sym_index, 0, sizeof(int) * (size_t)n);
            f->sym_index_mask = n - 1;
            for (int i = 0; i < f->n_syms; i++) sym_index_put(f, i);
        }
    }
    VMSym *s = &f->syms[f->n_syms++];
    vm->run.sys->memset(s, 0, sizeof(*s));
    s->name = name;
    s->type = type;
    s->offset = -1;
    if (f->sym_index) sym_index_put(f, f->n_syms - 1);
    return s;
}

// An IR_LOCAL read of `slot`, carrying the symbol's fixed-point shift on the
// node the way compile_ident does. strb_local below reads the slot type raw,
// which is right for the byte buffers it builds but would drop an fx array's
// element shift.
static int sym_read(VM *vm, Func *f, int slot) {
    int lv = ir_new(vm, f, IR_LOCAL);
    f->nodes[lv].type = ct_set_shift(f->syms[slot].type, f->syms[slot].shift);
    f->nodes[lv].ki   = slot;
    return lv;
}

static int struct_find(VM *vm, InternID name);
static unsigned long long struct_layout_hash(VM *vm, const VMStructTable *tab, int id, int depth);

// Materialise one of the VM's shared variables (vm_import_globals) as an ordinary
// sym in THIS func's table, carrying is_global and the imported layout's offset.
// Returns NULL if `name` is not one. Looking like any other sym is what keeps the
// rest of the compiler unaware of the distinction.
//
// A struct-typed one takes its struct from THIS unit, by name, and only when the
// layout hashes agree: the bytes are shared, so both programs must read them the
// same way. Otherwise it reports, sets VM.bind_failed, and returns NULL.
static VMSym *sym_bind_global(VM *vm, Func *f, InternID name) {
    if (!vm->globals || name == 0) return 0;
    for (int i = 0; i < vm->n_globals; i++) {
        if (vm->globals[i].name != name) continue;
        VMType t = vm->globals[i].type;
        if (vm->globals[i].struct_name) {
            const char *vn = intern_get_cstr(vm->intern, name);
            const char *sn = intern_get_cstr(vm->intern, vm->globals[i].struct_name);
            int sid = struct_find(vm, vm->globals[i].struct_name);
            if (!sid) {
                vm_errorf_at(vm, vm->err_ctx, "shared '%s' is a struct '%s'; declare struct %s in this program too",
                             vn, sn, sn);
                vm->bind_failed = 1;
                return 0;
            }
            if (struct_layout_hash(vm, vm->structs, sid, 0) != vm->globals[i].struct_hash) {
                vm_errorf_at(vm, vm->err_ctx, "shared '%s' was declared as struct '%s' with a different layout",
                             vn, sn);
                vm->bind_failed = 1;
                return 0;
            }
            t.struct_id = sid;
        }
        VMSym *ns = sym_add(vm, f, name, t);
        ns->offset    = vm->globals[i].offset;
        ns->shift     = vm->globals[i].shift;
        ns->is_global = 1;
        return ns;
    }
    return 0;
}

// Materialise one of the VM's host buffers (vm_declare_host_buffer) as an ordinary
// sym, like sym_bind_global above, but with the offset addressing VmRun.host_bufs.
// The shift is always 0: a host buffer's elements are raw storage.
static VMSym *sym_bind_host_buf(VM *vm, Func *f, InternID name) {
    if (name == 0) return 0;
    for (int i = 0; i < VM_MAX_HOST_BUFS; i++) {
        if (!vm->host_bufs[i].declared || vm->host_bufs[i].name != name) continue;
        VMSym *ns = sym_add(vm, f, name, vm->host_bufs[i].type);
        if (!ns) return 0;
        ns->offset      = i * (int)sizeof(VMHostBufSlot);
        ns->shift       = 0;
        ns->is_host_buf = 1;
        return ns;
    }
    return 0;
}

// Lookup name across this func's syms; if not found, the VM's shared
// variables, then its host buffers; if still not found, VM-level funcs.
// A built-in written in the language (vm_lib.h): attaches the source to this
// unit the first time a name resolves to nothing. Defined below, with the rest
// of the library machinery.
static VMSym *lib_attach(VM *vm, Func *f, InternID name, ASTNode *at);

static VMSym *resolve_name(VM *vm, Func *f, InternID name) {
#if VM_REACTIVE
    vm->rx_name_reported = 0;
#endif
    VMSym *s = sym_find(f, name);
    if (s) return s;
    s = sym_bind_global(vm, f, name);
    if (s) return s;
    s = sym_bind_host_buf(vm, f, name);
    if (s) return s;
    int session_owned = 0;
#if VM_REACTIVE
    // Ahead of the function list: a reactive session owns which `f` a call at
    // this point of the program means, and the list only knows every `f` ever
    // compiled into this VM.
    if (vm->rx_name_fn && name != 0) {
        VmExtern ex;
        vm->run.sys->memset(&ex, 0, sizeof(ex));
        int rc = vm->rx_name_fn(vm->rx_user, name, vm->rx_func_depth, &ex);
        if (rc < 0) { vm->rx_name_reported = 1; return 0; }
        if (rc) {
            VMType vt = { VMT_VOID, 0 };
            if (ex.is_func) {
                VMSym *ns = sym_add(vm, f, name, vt);
                ns->is_func = 1;
                ns->fn = ex.fn;
                return ns;
            }
            if (ex.is_comp) {
                VMSym *ns = sym_add(vm, f, name, vt);
                ns->is_comp = 1;
                return ns;
            }
            VMSym *ns = sym_add(vm, f, name, ex.type);
            ns->shift     = ex.shift;
            ns->is_extern = 1;
            ns->ext_id    = ex.ext_id;
            return ns;
        }
        // Script functions are the session's alone: the list would hand out an
        // `f` defined later in the program, or by an earlier text. It still
        // serves natives, and a body calling the function it is compiling.
        session_owned = 1;
    }
#endif
    for (Func *g = vm->run.funcs; g; g = g->next) {
        if (g->name == name && name != 0) {
            // A library function is reachable only through lib_attach, which
            // owns the per-unit rebuild: found here it would be a template
            // compiled under some earlier unit's #rewire.
            if (g->is_lib) continue;
            if (session_owned && g->native_tok == 0 && g->ct_purity != CT_PURITY_COMPILING) continue;
            // Create a synthetic sym pointing to that function.
            VMType vt = { VMT_VOID, 0 };
            VMSym *ns = sym_add(vm, f, name, vt);
            ns->is_func = 1;
            ns->fn = g;
            return ns;
        }
    }
    // Nothing in the unit, nothing registered: a built-in written in the
    // language may still own this name.
    return lib_attach(vm, f, name, vm->err_ctx);
}

// ---------- forward decls ----------

static int  compile_expr(VM *vm, Func *f, ASTNode *node, VMType *out_t);
static int  is_staged_arr_lit(Func *f, int ir);
static int  emit_slice_ir(VM *vm, Func *f, int base, VMType bt, int start, int len, VMType *out_t);
static ASTNode *template_param_node(Func *tmpl, int i);
static int  compile_binop_ir(VM *vm, Func *f, ASTNode *at, int sub,
                             int l, VMType lt, int r, VMType rt, VMType *out_t);
// Elementwise builtin call: defined with the rest of the lane machinery,
// dispatched from compile_call above it.
static int  compile_vec_call(VM *vm, Func *f, ASTNode *node, Func *callee,
                             int args_begin, const VMType *arg_types, int n_args,
                             VMType *out_t);
static int  compile_vec_cast(VM *vm, Func *f, ASTNode *at, int e, VMType et,
                             VMType target, VMType *out_t);
static int  compile_vec_pow(VM *vm, Func *f, ASTNode *node, int l, VMType lt,
                            int lit_exp, VMType *out_t);
static int  compile_pow_ir(VM *vm, Func *f, ASTNode *node, int l, VMType lt,
                           int lit_exp, VMType *out_t);
// A read of a local by slot; defined with the string builder.
static int  strb_local(VM *vm, Func *f, int slot);
static int  compile_block(VM *vm, Func *f, ASTNode *block_brace_or_items);
static int  compile_stmt_into(VM *vm, Func *f, ASTNode *node, int **slot, int *count, int *cap);
static int  compile_if(VM *vm, Func *f, ASTNode *node, ASTNode **args, int n,
                       ASTNode **else_args, int else_n);
static int  compile_else_tail(VM *vm, Func *f, ASTNode **args, int n);
static int  compile_annotated_decl(VM *vm, Func *f, ASTNode *colon, ASTNode *init,
                                   int **slot, int *count, int *cap);
static int  dataslice_to_i32(VM *vm, Func *f, int idx);
static Func   *find_or_specialize(VM *vm, Func *tmpl, int n_args, const VMType *atypes);
static Func   *compile_func_def(VM *vm, InternID name, ASTNode *arrow_node);
static int     slice_escapes_frame(Func *f, int ir);
static void    layout_frame(Func *f);
static int     insert_fx_convert(VM *vm, Func *f, int e, VMType from, VMType to);
static int     ref_to_value(VM *vm, Func *f, int e, VMType *t);
static int     struct_ids_match(const VMStructTable *ta, int ida, const VMStructTable *tb, int idb);
static int     struct_fill_slot(VM *vm, Func *f, int slot, int id, ASTNode *brace, int zero_omitted,
                                ASTNode *at, int **items, int *count, int *cap);
static int     compile_update_field_staged(VM *vm, Func *f, ASTNode *target, InternID op,
                                           ASTNode *operand, int **slot, int *count, int *cap);

// ---------- built-ins written in the language itself ----------
//
// The source lives in vm/vm_lib.h and is registered by vm_register_lib_defaults
// (or by a host, through vm_declare_lib_func). Nothing is parsed or compiled
// until a unit actually names one.

// The entry for `name`, or NULL. An entry the current unit defines itself is
// masked: the user's definition wins wherever it sits in the file.
static struct VMLibFunc *lib_find(VM *vm, InternID name) {
    if (name == 0) return 0;
    for (int i = 0; i < vm->n_lib; i++) {
        struct VMLibFunc *e = &vm->lib[i];
        if (e->name != name) continue;
        if (vm->unit_id != 0 && e->masked == vm->unit_id) return 0;
        return e;
    }
    return 0;
}

// The `name = (params) => body` statement in a parsed library snippet.
static ASTNode *lib_find_arrow(VM *vm, ParseResult *pres, InternID name) {
    ASTNode *tree = pres->code_tree;
    if (!tree) return 0;
    for (size_t i = 0; i < tree->items.size; i++) {
        ASTNode *it = paren_inner(*(ASTNode**)array_get(&tree->items, i));
        if (!it || it->token != TOK_EQ || !it->left || !it->right) continue;
        if (!is_ident(it->left) || it->left->token != name) continue;
        if (it->right->token != TOK_ARROW_F) continue;
        (void)vm;
        return it->right;
    }
    return 0;
}

// Parse once per VM, compile once per unit. The template is NOT kept across
// units: it snapshots the compile flags (compile_func_def) and its
// specialisations are keyed on parameter types alone, so one held over would
// hand the next unit a body compiled under the previous unit's #rewire.
static Func *lib_template(VM *vm, struct VMLibFunc *e, ASTNode *at) {
    if (e->tmpl && e->unit == vm->unit_id) return e->tmpl;
    if (e->busy) {
        vm->lib_name = e->name;
        vm_errorf_at(vm, at, "built-in calls itself before it is defined");
        return 0;
    }
    if (!vm->parser) return 0;

    int saved_depth = vm->lib_depth;
    ASTNode *saved_at = vm->lib_at;
    InternID saved_nm = vm->lib_name;
    vm->lib_depth++;
    vm->lib_at   = at;
    vm->lib_name = e->name;
    e->busy = 1;

    Func *out = 0;
    // The parse and the template outlive the statement being compiled.
    MemBackend stmt_mem = vm->run.mem;
    vm->run.mem = *long_mem(vm);
    if (!e->pres) {
        ParseResult *pr = (ParseResult*)mem_alloc(&vm->run.mem, sizeof(ParseResult));
        if (!pr) { vm_set_error(&vm->run, "out of memory"); goto done; }
        *pr = parse_to_asts(vm->parser, e->src, 0);
        if (parser_get_error(pr)) {
            vm_errorf_at(vm, at, "%s", parser_get_error(pr));
            goto done;
        }
        e->pres  = pr;
        e->arrow = lib_find_arrow(vm, pr, e->name);
        if (!e->arrow) { vm_set_error_at(vm, at, "source defines no function of this name"); goto done; }
    }
    // Compiled HERE, under this unit's live directive state, which is the whole
    // point: the same text becomes f64, f32 or fx<N> code depending on where it
    // is used.
    out = compile_func_def(vm, e->name, e->arrow);
    if (out) {
        out->is_lib = 1;
        out->accel  = e->accel;
        e->tmpl = out;
        e->unit = vm->unit_id;
    }
done:
    vm->run.mem = stmt_mem;
    e->busy = 0;
    vm->lib_depth = saved_depth;
    vm->lib_at    = saved_at;
    vm->lib_name  = saved_nm;
    return out;
}

static VMSym *lib_attach(VM *vm, Func *f, InternID name, ASTNode *at) {
    struct VMLibFunc *e = lib_find(vm, name);
    if (!e) return 0;
    Func *g = lib_template(vm, e, at);
    if (!g) return 0;
    VMType vt = { VMT_VOID, 0 };
    VMSym *ns = sym_add(vm, f, name, vt);
    ns->is_func = 1;
    ns->fn      = g;
    return ns;
}

// A unit's own `name = ...` at top level takes the name back from the library,
// wherever in the file it sits -- so `sum = (xs) => ...` followed by a call
// means the user's, and so does a call that comes first.
static void lib_mask_unit_names(VM *vm, ASTNode *tree) {
    if (!tree || vm->n_lib == 0) return;
    for (size_t i = 0; i < tree->items.size; i++) {
        ASTNode *it = paren_inner(*(ASTNode**)array_get(&tree->items, i));
        if (!it || it->token != TOK_EQ || !it->left) continue;
        ASTNode *lhs = it->left;
        // `name : T = ...` declares `name` too.
        if (lhs->token == TOK_COLON && lhs->left) lhs = lhs->left;
        if (!is_ident(lhs)) continue;
        for (int k = 0; k < vm->n_lib; k++)
            if (vm->lib[k].name == lhs->token) vm->lib[k].masked = vm->unit_id;
    }
}

void vm_declare_lib_func_accel(VM *vm, const char *name, const char *src,
                               int (*accel)(struct Func *spec, unsigned char *frame)) {
    if (!vm || !name || !src || !vm->intern) return;
    InternID id = intern_c_string(&vm->globals_owner, name);
    for (int i = 0; i < vm->n_lib; i++)
        if (vm->lib[i].name == id) {
            // Replacing an entry drops whatever was parsed from the old text.
            vm->lib[i].src   = src;
            vm->lib[i].pres  = 0;
            vm->lib[i].arrow = 0;
            vm->lib[i].tmpl  = 0;
            vm->lib[i].unit  = 0;
            vm->lib[i].accel = accel;
            return;
        }
    // Heap, not the VM arena: the table lives as long as the VM, and an entry is
    // only ever added by the host, never while a pointer into it is held.
    if (vm->n_lib == vm->cap_lib) {
        int nc = vm->cap_lib ? vm->cap_lib * 2 : 32;
        struct VMLibFunc *nl = (struct VMLibFunc*)S_REALLOC(vm->run.sys, vm->lib, sizeof(*nl) * (size_t)nc);
        if (!nl) return;
        vm->lib = nl;
        vm->cap_lib = nc;
    }
    struct VMLibFunc *e = &vm->lib[vm->n_lib++];
    vm->run.sys->memset(e, 0, sizeof(*e));
    e->name  = id;
    e->src   = src;
    e->accel = accel;
}

void vm_declare_lib_func(VM *vm, const char *name, const char *src) {
    vm_declare_lib_func_accel(vm, name, src, 0);
}

// The one float kind a specialisation's parameters and result all share, or
// VMT_VOID. Only such a spec takes an accelerator: the C versions are written
// per float width, and anything mixed or fixed-point runs its body.
static VTKind lib_accel_kind(Func *sp) {
    VTKind k = VMT_VOID;
    for (int i = 0; i <= sp->n_params; i++) {
        VMType t = i < sp->n_params ? sp->syms[sp->param_slot[i]].type : sp->ret_type;
        VTKind e = t.kind;
        if (is_array(t.kind) || is_slice(t.kind)) {
            if (t.pack_bits || t.struct_id || t.inner_len) return VMT_VOID;
            e = arr_elem(t.kind);
        }
        if (e != VMT_F32 && e != VMT_F64) return VMT_VOID;
        if (k != VMT_VOID && e != k) return VMT_VOID;
        k = e;
    }
    return k;
}

void vm_register_lib_defaults(VM *vm) {
    vm_declare_lib_func(vm, "__ipow64",      VM_LIB_SRC_IPOW64);
    vm_declare_lib_func(vm, "sum",           VM_LIB_SRC_SUM);
    vm_declare_lib_func(vm, "mean",          VM_LIB_SRC_MEAN);
    vm_declare_lib_func(vm, "fromPolar",     VM_LIB_SRC_FROMPOLAR);
    vm_declare_lib_func(vm, "toPolar",       VM_LIB_SRC_TOPOLAR);
    vm_declare_lib_func(vm, "rotate",        VM_LIB_SRC_ROTATE);
    vm_declare_lib_func(vm, "rotateX",       VM_LIB_SRC_ROTATEX);
    vm_declare_lib_func(vm, "rotateY",       VM_LIB_SRC_ROTATEY);
    vm_declare_lib_func(vm, "rotateZ",       VM_LIB_SRC_ROTATEZ);
    vm_declare_lib_func(vm, "euler",         VM_LIB_SRC_EULER);
    vm_declare_lib_func(vm, "project",       VM_LIB_SRC_PROJECT);
    vm_declare_lib_func_accel(vm, "dot",         VM_LIB_SRC_DOT,         VM_LIB_ACCEL(acc_dot));
    vm_declare_lib_func_accel(vm, "length",      VM_LIB_SRC_LENGTH,      VM_LIB_ACCEL(acc_length));
    vm_declare_lib_func_accel(vm, "distance",    VM_LIB_SRC_DISTANCE,    VM_LIB_ACCEL(acc_distance));
    vm_declare_lib_func_accel(vm, "normalize",   VM_LIB_SRC_NORMALIZE,   VM_LIB_ACCEL(acc_normalize));
    vm_declare_lib_func_accel(vm, "reflect",     VM_LIB_SRC_REFLECT,     VM_LIB_ACCEL(acc_reflect));
    vm_declare_lib_func_accel(vm, "refract",     VM_LIB_SRC_REFRACT,     VM_LIB_ACCEL(acc_refract));
    vm_declare_lib_func_accel(vm, "cross",       VM_LIB_SRC_CROSS,       VM_LIB_ACCEL(acc_cross));
    vm_declare_lib_func_accel(vm, "faceforward", VM_LIB_SRC_FACEFORWARD, VM_LIB_ACCEL(acc_faceforward));
    // Shadow a registered native, so only #enable source_builtins reaches them.
    vm_declare_lib_func(vm, "linearstep",    VM_LIB_SRC_LINEARSTEP);
    vm_declare_lib_func(vm, "smoothstep",    VM_LIB_SRC_SMOOTHSTEP);
    vm_declare_lib_func(vm, "smootherstep",  VM_LIB_SRC_SMOOTHERSTEP);
    vm_declare_lib_func(vm, "linearstepa",   VM_LIB_SRC_LINEARSTEPA);
    vm_declare_lib_func(vm, "smoothstepa",   VM_LIB_SRC_SMOOTHSTEPA);
    vm_declare_lib_func(vm, "smootherstepa", VM_LIB_SRC_SMOOTHERSTEPA);
}

// Each parsed snippet owns an arena of its own; the VM outlives them all, so
// they are released together when it is destroyed.
void vm_lib_free(VM *vm) {
    if (!vm) return;
    for (int i = 0; vm->parser && i < vm->n_lib; i++)
        if (vm->lib[i].pres) {
            free_parse_result(vm->parser, (ParseResult*)vm->lib[i].pres);
            vm->lib[i].pres = 0;
        }
    if (vm->lib) S_FREE(vm->run.sys, vm->lib);
    vm->lib = 0;
    vm->n_lib = vm->cap_lib = 0;
}


// Does the body contain a `return` with no value anywhere -- including nested
// in an `if` or a loop?  The nodes array is flat per function, so one linear
// scan sees every block.  A valueless return is how a function says "I return
// nothing", which both the implicit-return pass and `return expr` need to know.
static int has_valueless_return(Func *f) {
    for (int i = 0; i < f->n_nodes; i++)
        if (f->nodes[i].op == IR_RETURN && f->nodes[i].a < 0) return 1;
    return 0;
}

// Does control leaving this statement always pass through a `return`? Lets a
// body that ends in an if/else whose arms both return be told apart from one
// that can fall off the end. Only the shapes that can terminate are walked: a
// loop may run zero times, so it never counts, and an `if` without an else has
// a path around it.
static int block_always_returns(Func *f, int blk);
static int stmt_always_returns(Func *f, int idx) {
    if (idx < 0) return 0;
    int op = f->nodes[idx].op;
    if (op == IR_RETURN) return 1;
    if (op == IR_BLOCK)  return block_always_returns(f, idx);
    if (op == IR_IF)
        return f->nodes[idx].c >= 0
            && stmt_always_returns(f, f->nodes[idx].b)
            && stmt_always_returns(f, f->nodes[idx].c);
    return 0;
}
static int block_always_returns(Func *f, int blk) {
    if (blk < 0 || f->nodes[blk].op != IR_BLOCK) return 0;
    for (int i = 0; i < f->nodes[blk].n_items; i++)
        if (stmt_always_returns(f, f->child_indices[f->nodes[blk].items_begin + i])) return 1;
    return 0;
}

// Does converting a return value from `from` to `to` keep the value? Only
// widenings do. Every narrowing -- f64 -> i32 drops the fraction, i64 -> i32
// drops the high word, a fixed-point rescale rounds off bits -- would silently
// change what the function hands back.
static int ret_convert_is_lossless(VMType from, VMType to) {
    int fs = ct_shift(from), ts = ct_shift(to);
    if (from.kind == to.kind && fs == ts) return 1;   // identical: no conversion at all
    if (from.kind == VMT_I32) {
        // Within i32 storage a raw int is just fx with shift 0, so one rule covers
        // both: keeping at least as many fractional bits is a left shift and exact,
        // dropping any of them rounds.
        if (to.kind == VMT_I32) return ts >= fs;
        // fx -> f64 is exact: a 32-bit word over a power of two, both of which f64
        // holds exactly. f32's 24-bit mantissa cannot promise that.
        if (to.kind == VMT_F64) return 1;
        return fs == 0 && to.kind == VMT_I64;
    }
    if (from.kind == VMT_F32) return to.kind == VMT_F64;
    return 0;                                        // f64/i64 narrowing, aggregates
}

// The one type that holds both return values without losing anything: the wider
// of the two when one widens into the other, and f64 when an int and an f32 meet
// -- neither of those holds the other, but f64 holds both. 0 if there is none.
static int ret_type_join(VMType a, VMType b, VMType *out) {
    // Const joins toward const: if either return hands back a read-only view,
    // the caller must treat the result as one. Taken before the lossless checks
    // because those pick whichever operand they were handed, which would drop
    // the bit half the time depending on the order the returns were seen in.
    int cst = a.is_const || b.is_const;
    if (ret_convert_is_lossless(a, b)) { *out = b; out->is_const = cst; return 1; }
    if (ret_convert_is_lossless(b, a)) { *out = a; out->is_const = cst; return 1; }
    // Zero-initialised whole, not field by field: VMType has grown fields before.
    VMType wide = {0};
    wide.kind = VMT_F64;
    if (ret_convert_is_lossless(a, wide) && ret_convert_is_lossless(b, wide)) {
        *out = wide;
        return 1;
    }
    return 0;
}

// Widen the function's return type so it also holds `et`. Order-independent: no
// conversion is inserted here, because a return still to come may widen the type
// again -- finalize_return_types does that once the whole body is in.
static int note_return_type(VM *vm, Func *f, VMType et, ASTNode *at) {
    if (!f->has_return) { f->ret_type = et; f->has_return = 1; return 1; }
    // A struct has no conversion to widen into: every return hands back the same one.
    if (vmt_is_struct(et) || vmt_is_struct(f->ret_type)) {
        if (et.kind == f->ret_type.kind && et.len == f->ret_type.len && et.inner_len == f->ret_type.inner_len
            && struct_ids_match(f->structs, et.struct_id, f->structs, f->ret_type.struct_id)) {
            // This comparison enumerates fields, so const has to be decided
            // explicitly: it joins toward const, as in ret_type_join.
            if (et.is_const) f->ret_type.is_const = 1;
            return 1;
        }
        vm_set_error_at(vm, at, "this function returns two different types, and one is a struct; "
                                "every return has to hand back the same struct");
        return 0;
    }
    VMType joined;
    if (!ret_type_join(f->ret_type, et, &joined)) {
        vm_set_error_at(vm, at,
            "this return value and another one in the same function have no common type that "
            "holds both without losing part of the value. Make them agree, or convert one on "
            "purpose with `as`");
        return 0;
    }
    f->ret_type = joined;
    return 1;
}

// 1 (having set the error) when a value is needed and `t` is a call to a
// function that returns nothing. The interpreter read it as 0; C has no value
// to read at all.
static int reject_void(VM *vm, ASTNode *at, VMType t) {
    if (t.kind != VMT_VOID) return 0;
    vm_set_error_at(vm, at, "this returns nothing, so it has no value to use here");
    return 1;
}

// The break or continue in statement `ir` that no loop encloses, or -1. A loop's
// own are not looked into; a lambda's body is a Func of its own, checked there.
static int stray_break(Func *f, int ir) {
    if (ir < 0) return -1;
    IRNode *n = &f->nodes[ir];
    switch (n->op) {
        case IR_BREAK: case IR_CONTINUE:
            return ir;
        case IR_BLOCK:
            for (int i = 0; i < n->n_items; i++) {
                int s = stray_break(f, f->child_indices[n->items_begin + i]);
                if (s >= 0) return s;
            }
            return -1;
        case IR_IF: {
            int s = stray_break(f, n->b);
            return s >= 0 ? s : stray_break(f, n->c);
        }
        default:
            return -1;
    }
}

// The interpreter only finds out at run time, and C refuses to compile it.
static int check_stray_break(VM *vm, Func *f) {
    int s = stray_break(f, f->body);
    if (s < 0) return 1;
    vm_set_error(&vm->run, f->nodes[s].op == IR_BREAK ? "'break' outside a loop" : "'continue' outside a loop");
    return 0;
}

// Convert every return in the body to the type they joined to. Runs once the body
// is complete: until then f->ret_type is still provisional, so converting at each
// return would convert to a type that is not the final one.
static int finalize_return_types(VM *vm, Func *f) {
    if (!f->has_return || f->ret_type.kind == VMT_VOID) return 1;

    // A recursive call was typed from f->ret_type as it stood mid-body, so a later
    // return that widened the type leaves that call node -- and everything computed
    // from it -- stale. Nothing here can retype it: the expression built on top of
    // it has already been folded and typed. Report it instead of miscompiling, and
    // the fix is to make the returns agree outright. A self-call reached before any
    // return at all is typed VOID and is left alone: that shape predates the join
    // (the ternary in `f = n => n < 2 ? 1 : f(n-1)` is where it shows up) and takes
    // its type from elsewhere in the expression.
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op != IR_CALL || f->nodes[i].ki != f->func_id) continue;
        if (f->nodes[i].type.kind == VMT_VOID) continue;
        if (f->nodes[i].type.kind == f->ret_type.kind
            && ct_shift(f->nodes[i].type) == ct_shift(f->ret_type)) continue;
        vm_set_error_at(vm, 0,
            "this function calls itself, and a later return widened its type after that call "
            "was already compiled. Give every return the same type outright -- write the "
            "narrower ones in the wider form (0.0 rather than 0)");
        return 0;
    }

    // ir_new appends, so the conversions land past here; the count is captured
    // first rather than walking into the nodes this loop is creating.
    int n = f->n_nodes;
    for (int i = 0; i < n; i++) {
        if (f->nodes[i].op != IR_RETURN) continue;
        int a = f->nodes[i].a;
        if (a < 0) continue;
        VMType at = f->nodes[a].type;
        if (at.kind == f->ret_type.kind && ct_shift(at) == ct_shift(f->ret_type)) continue;
        // Through a temporary: insert_fx_convert can reallocate f->nodes, so the
        // destination must be addressed after it returns, not before.
        int cvt = insert_fx_convert(vm, f, a, at, f->ret_type);
        f->nodes[i].a = cvt;
    }
    return 1;
}

// The last statement of a body is its value: `x` at the end means `return x`,
// exactly as if it had been written. Runs after the body is compiled, so an
// earlier `return` has already pinned f->ret_type and this one converts into it.
// Returns 0 (with last_error set) for a trailing value that cannot be returned.
static int apply_implicit_return(VM *vm, Func *f, int blk) {
    if (blk < 0) return 1;
    // A bare `return` already declared this function void, so a trailing value
    // would give it a type that the early `return;` cannot satisfy.
    if (has_valueless_return(f)) return 1;

    int n_items  = f->nodes[blk].n_items;
    int last_pos = f->nodes[blk].items_begin + n_items - 1;
    int last_idx = n_items ? f->child_indices[last_pos] : -1;

    // The tail is a value only if it is an expression statement whose expression
    // has one. A trailing assignment, loop or void call leaves the function void.
    int tail = -1;
    if (last_idx >= 0 && f->nodes[last_idx].op == IR_EXPR_STMT
        && f->nodes[f->nodes[last_idx].a].type.kind != VMT_VOID)
        tail = f->nodes[last_idx].a;

    if (tail < 0) {
        // No trailing value. Fine for a void function, but if an earlier return
        // gave this one a type, control can now reach the end of a body that owes
        // the caller a value -- unless every path already returned.
        if (f->has_return && f->ret_type.kind != VMT_VOID && !block_always_returns(f, blk)) {
            vm_set_error_at(vm, 0,
                "this function returns a value in one place but ends without one. Finish it "
                "with the value it should hand back, or an explicit 'return'");
            return 0;
        }
        return 1;
    }

    if (slice_escapes_frame(f, tail)) {
        vm_set_error_at(vm, 0,
            "cannot return a string or slice built here: it points into this call's frame. "
            "Take a []u8 buffer as a parameter and fill that instead");
        return 0;
    }
    {
        // A ref returns the record it points at, by value.
        VMType tt = f->nodes[tail].type;
        tail = ref_to_value(vm, f, tail, &tt);
    }
    if (!note_return_type(vm, f, f->nodes[tail].type, 0)) return 0;
    int ret_idx = ir_new(vm, f, IR_RETURN);
    f->nodes[ret_idx].a = tail;
    f->child_indices[last_pos] = ret_idx;
    return 1;
}

// ---------- helpers for compile ----------

// Is `e` an i32 seen as f64: a CVT from a raw i32, or an f64 constant that is
// one exactly?
static int is_widened_i32(Func *f, int e) {
    if (e < 0) return 0;
    if (f->nodes[e].op == IR_CVT && f->nodes[e].sub_op == VMT_I32 && f->nodes[e].type.kind == VMT_F64) {
        VMType at = f->nodes[f->nodes[e].a].type;
        return at.kind == VMT_I32 && ct_shift(at) == 0;
    }
    if (f->nodes[e].op == IR_CONST_F && f->nodes[e].type.kind == VMT_F64) {
        double v = f->nodes[e].kf;
        return v >= -2147483648.0 && v <= 2147483647.0 && v == (double)(long long)v;
    }
    return 0;
}

static int unwiden_i32(VM *vm, Func *f, int e) {
    if (f->nodes[e].op == IR_CVT) return f->nodes[e].a;
    VMType it = {0}; it.kind = VMT_I32;
    return ir_const_of(vm, f, it, (long long)f->nodes[e].kf, f->nodes[e].kf);
}

// `a[i/2]`: the f64 quotient of two i32s, truncated, is exactly C's integer
// quotient -- f64 holds every i32 and its rounding cannot carry a quotient
// across an integer -- so the double divide is dropped. Only speed changes:
// wherever this does not fire the value is the same. i32 operands only; an i64
// or f32 quotient would come out more exact than the one it replaces.
static int int_quotient_of_widened(VM *vm, Func *f, int e) {
    if (f->nodes[e].op != IR_BINOP || f->nodes[e].type.kind != VMT_F64) return -1;
    int sub = f->nodes[e].sub_op;
    if (sub != OP_DIV && sub != OP_DIV_NATIVE) return -1;
    int a = f->nodes[e].a, b = f->nodes[e].b;
    if (!is_widened_i32(f, a) || !is_widened_i32(f, b)) return -1;
    VMType it = {0}; it.kind = VMT_I32;
    int x = unwiden_i32(vm, f, a);
    int y = unwiden_i32(vm, f, b);
    return ir_binop(vm, f, sub, x, y, it);
}

// A constant under zero or more inspect wrappers, none of them fixed-point.
static int is_inspected_const(Func *f, int e) {
    while (e >= 0 && f->nodes[e].op == IR_INSPECT && f->nodes[e].sub_op == 0) e = f->nodes[e].a;
    return ir_is_const(f, e);
}

// Folding a float constant to an integer gives the saturated value (cvt_to).
// Under #enable c_float_to_int an out-of-range or nan one is C's to decide, so
// it is left for the target rather than folded to an answer C never promised.
static int cvt_fold_ok(VM *vm, Func *f, int e, VTKind from, VTKind to) {
    if (!(vm->flags & VM_FLAG_C_FLOAT_TO_INT)) return 1;
    if ((from != VMT_F32 && from != VMT_F64) || (to != VMT_I32 && to != VMT_I64)) return 1;
    double v = ir_const_num(f, e);
    double lim = to == VMT_I32 ? 2147483648.0 : 9223372036854775808.0;
    return v == v && v > -lim - 1.0 && v < lim;
}

static int insert_cvt(VM *vm, Func *f, int e, VMType from, VTKind to) {
    if (from.kind == to) return e;
    if (from.kind == VMT_F64 && (to == VMT_I32 || to == VMT_I64)) {
        int q = int_quotient_of_widened(vm, f, e);
        if (q >= 0) {
            if (to == VMT_I32) return q;
            VMType it = {0}; it.kind = VMT_I32;
            return insert_cvt(vm, f, q, it, VMT_I64);
        }
    }
    // A converted constant is still a constant. This is the other half of the
    // fold: without it a fixed-point literal reaches every use as
    // CVT(CONST) >> k rather than as the shifted word itself.
    if ((vm->flags & VM_FLAG_CONST_FOLD) && ir_is_const(f, e) && cvt_fold_ok(vm, f, e, from.kind, to)) {
        long long oi = 0; double of = 0;
        if (vm_fold_cvt(from.kind, to, ir_const_int(f, e), ir_const_num(f, e), &oi, &of)) {
            // Zero-initialised whole, not field by field: VMType has grown
            // fields before, and a partial init is how that bit people.
            VMType ct = {0};
            ct.kind = to;
            return ir_const_of(vm, f, ct, oi, of);
        }
    }
    // `x / __ins(2, 0)` converts the hovered literal: fold it under the wrapper,
    // or inspecting it changes what the program emits. The label then reports
    // the converted value, which is the value the operator actually used.
    if ((vm->flags & VM_FLAG_CONST_FOLD) && f->nodes[e].op == IR_INSPECT && is_inspected_const(f, e)) {
        int k = insert_cvt(vm, f, f->nodes[e].a, from, to);
        if (is_inspected_const(f, k)) {
            int ins = ir_new(vm, f, IR_INSPECT);
            f->nodes[ins] = f->nodes[e];
            f->nodes[ins].a = k;
            f->nodes[ins].type = f->nodes[k].type;
            return ins;
        }
    }
    int c = ir_new(vm, f, IR_CVT);
    f->nodes[c].a = e;
    f->nodes[c].sub_op = from.kind;
    f->nodes[c].type.kind = to;
    return c;
}

// insert_cvt where the destination carries a fixed-point shift. Narrowing an i64
// Q<shift> accumulator back to i32 Q<shift> reinterprets nothing about the binary
// point, so the shift rides along -- which insert_cvt's VTKind destination cannot
// express. Every fixed-point site that used to hand-build this node lost the
// constant fold with it.
static int insert_cvt_t(VM *vm, Func *f, int e, VMType from, VMType to) {
    if (from.kind == to.kind && ct_shift(from) == ct_shift(to)) return e;
    if ((vm->flags & VM_FLAG_CONST_FOLD) && ir_is_const(f, e) && cvt_fold_ok(vm, f, e, from.kind, to.kind)) {
        long long oi = 0; double of = 0;
        if (vm_fold_cvt(from.kind, to.kind, ir_const_int(f, e), ir_const_num(f, e), &oi, &of))
            return ir_const_of(vm, f, to, oi, of);
    }
    int c = ir_new(vm, f, IR_CVT);
    f->nodes[c].a = e;
    f->nodes[c].sub_op = from.kind;
    f->nodes[c].type = to;
    return c;
}

// Emit conversion IR between two types that may carry fixed-point shifts.
// Returns the node index of the converted value.
static int insert_fx_convert(VM *vm, Func *f, int e, VMType from, VMType to) {
    int fs = ct_shift(from), ts = ct_shift(to);
    if (from.kind == to.kind && fs == ts) return e;
    // fxA -> fxB (both i32, different shift): BSHL or BSHR
    if (from.kind == VMT_I32 && to.kind == VMT_I32 && fs != ts) {
        int d = ts - fs;
        int ci = ir_i32(vm, f, d > 0 ? d : -d);
        int sub = d > 0 ? ((vm->flags & VM_FLAG_C_SHIFTS) ? OP_BSHL_NATIVE : OP_BSHL) : OP_BSHR;
        return ir_binop(vm, f, sub, e, ci, ct_set_shift(ct_vmtype_clear_shift(to), ts));
    }
    // fxN -> float: CVT to float, then DIV by 2^N
    if (from.kind == VMT_I32 && fs > 0 && (to.kind == VMT_F32 || to.kind == VMT_F64)) {
        VMType fc = ct_vmtype_clear_shift(from);
        int cv = insert_cvt(vm, f, e, fc, to.kind);
        int sc = ir_new(vm, f, IR_CONST_F);
        f->nodes[sc].type.kind = to.kind;
        f->nodes[sc].kf = (double)(1LL << fs);
        if (to.kind == VMT_F32) f->nodes[sc].kf = (float)f->nodes[sc].kf;
        VMType dt = {0}; dt.kind = to.kind;
        return ir_binop(vm, f, OP_DIV, cv, sc, dt);
    }
    // float -> fxN: MUL by 2^N, then CVT to i32
    if ((from.kind == VMT_F32 || from.kind == VMT_F64) && to.kind == VMT_I32 && ts > 0) {
        int sc = ir_new(vm, f, IR_CONST_F);
        f->nodes[sc].type.kind = from.kind;
        f->nodes[sc].kf = (double)(1LL << ts);
        if (from.kind == VMT_F32) f->nodes[sc].kf = (float)f->nodes[sc].kf;
        VMType mt = {0}; mt.kind = from.kind;
        int ml = ir_binop(vm, f, OP_MUL, e, sc, mt);
        return insert_cvt_t(vm, f, ml, mt, ct_set_shift(ct_vmtype_clear_shift(to), ts));
    }
    // raw i32 -> fxN: BSHL
    if (from.kind == VMT_I32 && fs == 0 && to.kind == VMT_I32 && ts > 0) {
        int ci = ir_i32(vm, f, ts);
        int sub = (vm->flags & VM_FLAG_C_SHIFTS) ? OP_BSHL_NATIVE : OP_BSHL;
        return ir_binop(vm, f, sub, e, ci, ct_set_shift(ct_vmtype_clear_shift(to), ts));
    }
    // fxN -> raw i32: BSHR
    if (from.kind == VMT_I32 && fs > 0 && to.kind == VMT_I32 && ts == 0) {
        int ci = ir_i32(vm, f, fs);
        VMType rt = {0}; rt.kind = VMT_I32;
        return ir_binop(vm, f, OP_BSHR, e, ci, rt);
    }
    // i64 has no fixed-point form, so a conversion with fixed point on the
    // other side is done in two steps via a raw i32 -- otherwise the fallback
    // below would reinterpret the word and silently drop the scaling.
    if (from.kind == VMT_I64 && to.kind == VMT_I32 && ts > 0) {
        VMType raw = ct_vmtype_clear_shift(to);
        return insert_fx_convert(vm, f, insert_cvt(vm, f, e, from, VMT_I32), raw, to);
    }
    if (from.kind == VMT_I32 && fs > 0 && to.kind == VMT_I64) {
        VMType raw = ct_vmtype_clear_shift(from);
        return insert_cvt(vm, f, insert_fx_convert(vm, f, e, from, raw), raw, VMT_I64);
    }
    // Standard type cast (i32<->f32, etc.)
    return insert_cvt(vm, f, e, ct_vmtype_clear_shift(from), to.kind);
}

// C's rule for a truth value: the operand is compared against zero at its own
// width, never narrowed to an int first.  `if((1 as i64) << 40)` is true even
// though its low word is zero, and `if(0.5)` is true even though (int)0.5 is 0.
// i32 is already its own truth value and passes straight through -- fxN
// included, since a nonzero raw word is exactly a nonzero value.  Anything
// that isn't a scalar keeps the narrowing conversion.
static int emit_truthy(VM *vm, Func *f, int e, VMType t) {
    if (t.kind == VMT_I32) return e;
    // An array has no truth value -- the narrowing below made one up (0) -- and a
    // call that returns nothing has no value at all.
    if (is_array(t.kind) || is_slice(t.kind) || t.kind == VMT_VOID) {
        vm_set_error_at(vm, vm->err_ctx, t.kind == VMT_VOID
            ? "this returns nothing, so it cannot be a condition"
            : "an array cannot be a condition; test its elements, or its .len");
        return -1;
    }
    if (t.kind != VMT_I64 && t.kind != VMT_F32 && t.kind != VMT_F64)
        return insert_cvt(vm, f, e, t, VMT_I32);
    int z;
    if (t.kind == VMT_I64) { z = ir_new(vm, f, IR_CONST_I); f->nodes[z].ki = 0; }
    else                   { z = ir_new(vm, f, IR_CONST_F); f->nodes[z].kf = 0.0; }
    f->nodes[z].type.kind = t.kind;
    // Comparisons carry an i32 result and let the operands' own kind drive the
    // runtime op -- the same convention compile_expr's op_is_compare case uses.
    VMType bt = {0}; bt.kind = VMT_I32;
    return ir_binop(vm, f, OP_NE, e, z, bt);
}

// Emit a numeric literal of value `v` whose source spelling was `num_type`
// (NUM_INTEGER / NUM_DOUBLE / NUM_FLOAT), applying the literal rewire table. Split
// out of compile_number so a `const` can re-emit its folded value at each use site
// under the rules the literal itself would have met there.
//
// An integer outside i32 becomes an i64 literal rather than a truncated i32. `exact`
// is the parser's exact value for one past 2^53 (NUM_EXACT_I64), NULL otherwise.
static int emit_number_value(VM *vm, Func *f, double v, int num_type, const long long *exact,
                             ASTNode *at, VMType *out_t) {
    int ir;
    switch (num_type & NUM_TYPE) {
        case NUM_FLOAT: {
            VTKind tk = rewire_apply(vm, VMT_F32, REWIRE_LITERAL);
            int shift = rewire_shift(vm, VMT_F32, REWIRE_LITERAL);
            if (tk == VMT_I32 || tk == VMT_I64) {
                ir = ir_new(vm, f, IR_CONST_I);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].type.len = shift;
                f->nodes[ir].ki = shift > 0 ? (long long)((float)v * (float)(1LL << shift)) : (long long)(float)v;
            } else {
                ir = ir_new(vm, f, IR_CONST_F);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].kf = (tk == VMT_F32) ? (float)v : v;
            }
            break;
        }
        case NUM_DOUBLE: {
            VTKind tk = rewire_apply(vm, VMT_F64, REWIRE_LITERAL);
            int shift = rewire_shift(vm, VMT_F64, REWIRE_LITERAL);
            if (tk == VMT_I32 || tk == VMT_I64) {
                ir = ir_new(vm, f, IR_CONST_I);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].type.len = shift;
                f->nodes[ir].ki = shift > 0 ? (long long)(v * (double)(1LL << shift)) : (long long)v;
            } else {
                ir = ir_new(vm, f, IR_CONST_F);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].kf = (tk == VMT_F32) ? (float)v : v;
            }
            break;
        }
        case NUM_INTEGER:
        default: {
            VTKind tk = rewire_apply(vm, VMT_I32, REWIRE_LITERAL);
            int shift = rewire_shift(vm, VMT_I32, REWIRE_LITERAL);
            if (tk == VMT_F32 || tk == VMT_F64) {
                ir = ir_new(vm, f, IR_CONST_F);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].kf = (tk == VMT_F32) ? (float)v : v;
            } else {
                // 2^63 as a double: the first magnitude a long long cannot take.
                if (!exact && (v >= 9223372036854775808.0 || v < -9223372036854775808.0)) {
                    vm_set_error_at(vm, at, "integer literal does not fit in 64 bits");
                    return -1;
                }
                long long iv = exact ? *exact : (long long)v;
                if (tk == VMT_I32 && shift == 0 && (iv > 2147483647LL || iv < -2147483647LL - 1)) {
#if VM_HAS_I64
                    tk = VMT_I64;
#else
                    vm_set_error_at(vm, at, "integer literal does not fit in 32 bits");
                    return -1;
#endif
                }
                ir = ir_new(vm, f, IR_CONST_I);
                f->nodes[ir].type.kind = tk;
                f->nodes[ir].type.len = shift;
                f->nodes[ir].ki = shift > 0 ? iv << shift : iv;
            }
            break;
        }
    }
    *out_t = f->nodes[ir].type;
    return ir;
}

#if VM_REACTIVE
// Literal provenance: remember which AST node an IR constant was spelled by, so
// the reactive engine can write a value that flowed backwards into the text.
static void rx_note_lit(VM *vm, Func *f, int ir, ASTNode *n) {
    if (!vm->rx_active || ir < 0 || !n) return;
    if (!MEM_GROW(vm, &vm->run.mem, f->lits, f->n_lits, f->cap_lits, f->n_lits + 1, 8)) return;
    f->lits[f->n_lits].ir  = ir;
    f->lits[f->n_lits].ast = n;
    f->n_lits++;
}
// A component call inside an expression. The host instantiates the component
// (compiling its arguments through vm_compile_expr, reentrantly) and answers
// with the output's type and a key; the call compiles to a read of an extern
// slot the host fills, exactly as a named extern does. A void component leaves
// a typeless constant behind, so the statement it sits in still compiles.
static int compile_component_call(VM *vm, Func *f, ASTNode *node, InternID name, VMType *out_t) {
    ASTNode *argv[VM_MAX_PARAMS];
    int n_args = node->right ? expand_call_args(vm, node, node->right, argv, VM_MAX_PARAMS) : 0;
    if (n_args < 0) return -1;
    // A `...` argument reaches the host as the spread node itself. Expanding it
    // is the host's job, not this function's: these arguments are never
    // compiled here -- the host takes them as AST and compiles each one
    // reentrantly, so there is no argument run to expand into. The reactive
    // engine fans the operand's elements into one live value per lane
    // (rx_splat_create); a host that does not, reports its own error.
    // `...[a, b]` never gets this far -- expand_call_args spliced it already.
    VmExtern ex;
    vm->run.sys->memset(&ex, 0, sizeof(ex));
    if (!vm->rx_call_fn || !vm->rx_call_fn(vm->rx_user, node, name, argv, n_args, &ex)) {
        if (!vm->run.last_error)
            vm_errorf_at(vm, node, "cannot use component '%s' here", intern_get_cstr(vm->intern, name));
        return -1;
    }
    if (ex.type.kind == VMT_VOID) {
        int c = ir_new(vm, f, IR_CONST_I);
        f->nodes[c].type.kind = VMT_VOID;
        *out_t = f->nodes[c].type;
        return c;
    }
    VMSym *es = sym_add(vm, f, 0, ex.type);
    es->shift     = ex.shift;
    es->is_extern = 1;
    es->ext_id    = ex.ext_id;
    es->used      = 1;
    int slot = (int)(es - f->syms);
    int ir = sym_read(vm, f, slot);
    *out_t = ct_set_shift(es->type, es->shift);
    return ir;
}
#endif

static int compile_number(VM *vm, Func *f, ASTNode *n, VMType *out_t) {
    int ir = emit_number_value(vm, f, n->number, n->number_flags,
                               ast_has_exact_i64(n) ? &n->number_i : NULL, n, out_t);
#if VM_REACTIVE
    rx_note_lit(vm, f, ir, n);
#endif
    return ir;
}

// ---------- `#define NAME body` substitutions ----------

static VMDefine *define_find(VM *vm, InternID name) {
    if (name == 0) return 0;
    // Not inside a built-in written in the language: the unit's `#define`s are
    // the unit's, and a body the user cannot see must not be rewritten by them.
    if (vm->lib_depth > 0) return 0;
    for (VMDefine *d = vm->defines; d; d = d->next)
        if (d->name == name) return d;
    return 0;
}

static int define_arity(VMDefine *d) {
    return d->n_values;
}

static ASTNode *define_value(VMDefine *d, int i) {
    return d->values[i];
}

// ---------- compile-time constants (`const NAME = expr`) ----------

static Func *scope_of(Func *f) {
    return f && f->scope ? f->scope : f;
}

// A `const` is seen by the body that declared it and every body written inside
// that one. An auto-const only by its own body: it stands in for a local, and
// folding it must not make a name visible that the local would not be --
// otherwise `k = 10` / `f = (x) => x + k` compiles until `k` is written twice.
static int const_visible(VM *vm, VMConst *c) {
    Func *s = vm->cur_scope;
    if (c->is_auto) return c->scope == s;
    for (int guard = 0; guard < 256; guard++) {
        if (c->scope == s) return 1;
        if (!s) return 0;
        s = s->lex_parent;
    }
    return 0;
}

static VMConst *const_find(VM *vm, InternID name) {
    if (name == 0) return 0;
    // A built-in written in the language is not part of the unit, so the unit's
    // names must not reach into it. Without this a script's `x = 0.25` -- folded
    // to an auto-const -- would resolve ahead of smoothstep's own `x` parameter
    // and the built-in would silently compute with the caller's number. The same
    // isolation `#define` gets just below, and for the same reason.
    if (vm->lib_depth > 0) return 0;
    for (VMConst *c = vm->consts; c; c = c->next)
        if (c->name == name && const_visible(vm, c)) return c;
    // Host-declared constants come second, so a unit-level const of the same name
    // would win -- except declare_const rejects that outright.
    for (VMConst *c = vm->host_consts; c; c = c->next)
        if (c->name == name) return c;
    return 0;
}

// A const and a variable may not share a name. Consts resolve ahead of the
// symbol table (compile_ident), so a local that shadowed one would be
// unreadable -- every mention of it would still fold to the constant. Called
// from every site that declares or writes a name.
static int const_reject_write(VM *vm, ASTNode *at, InternID name, const char *what) {
    if (define_find(vm, name)) {
        vm_errorf_at(vm, at, "'%s' is a #define and cannot be %s",
                     intern_get_cstr(vm->intern, name), what);
        return 0;
    }
    if (!const_find(vm, name)) return 1;
    vm_errorf_at(vm, at, "'%s' is a constant and cannot be %s",
                 intern_get_cstr(vm->intern, name), what);
    return 0;
}

// Language-level constants. Consulted only once every other lookup has failed,
// so any declaration of the same name -- const, variable, parameter, #define --
// shadows one rather than colliding with it, and scripts that already spell
// their own `const pi` keep compiling.
static int builtin_const(VM *vm, InternID name, double *out) {
    static const struct { const char *name; double value; } k[] = {
        { "pi",  3.14159265358979323846 },
        { "tau", 6.28318530717958647692 },
    };
    if (name == 0) return 0;
    const char *s = intern_get_cstr(vm->intern, name);
    if (!s) return 0;
    for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++)
        if (s_strcmp(s, k[i].name) == 0) { *out = k[i].value; return 1; }
    return 0;
}

// ---------- auto_const: which names are only ever written once ----------
//
// `x = 5` at the top of a body, never written again, is a constant that happens
// to be spelled as a variable. VM_FLAG_AUTO_CONST turns it into one -- but only
// once the whole unit has been searched for another write, because a const
// resolves ahead of the symbol table and a second write would then have nowhere
// to go. That search happens ONCE per unit, here, off the AST, rather than per
// declaration: the per-declaration form would re-walk the tree on every
// constant in it, on every keystroke of the live-coding loop.
//
// Counting is deliberately crude and deliberately over-eager. Every identifier
// anywhere on the left of an `=` or an `op=`, every `++`/`--` operand, every
// `for ... in` variable, every parameter name and every `:` declaration counts
// as a write, whether or not the position really is one -- a ternary's `:` and
// an index base both get counted, and both only cost the fold. Missing a write
// is the failure that matters, so shapes are matched loosely and the walk
// covers the entire tree, nested function literals included.

static void ac_note_write(VM *vm, InternID name) {
    if (name == 0 || vm->ac_off) return;
    for (int i = 0; i < vm->ac_count; i++)
        if (vm->ac_names[i].name == name) {
            vm->ac_names[i].writes++;
            if (vm->ac_names[i].scope != vm->ac_scope) vm->ac_names[i].scopes = 2;
            return;
        }
    if (vm->ac_count >= VM_AUTOCONST_MAX) { vm->ac_off = 1; return; }
    vm->ac_names[vm->ac_count].name   = name;
    vm->ac_names[vm->ac_count].writes = 1;
    vm->ac_names[vm->ac_count].scope  = vm->ac_scope;
    vm->ac_names[vm->ac_count].scopes = 1;
    vm->ac_count++;
}

// Every identifier in `n`, as a write. Used for a whole assignment target, so
// that `a[i].x = v` counts `a` (and `i`, harmlessly) without this having to
// know what shapes a target can take.
static void ac_note_writes_in(VM *vm, ASTNode *n, int depth) {
    if (!n || depth > 64 || vm->ac_off) return;
    if (is_ident(n)) ac_note_write(vm, n->token);
    ac_note_writes_in(vm, n->left,  depth + 1);
    ac_note_writes_in(vm, n->right, depth + 1);
    for (size_t i = 0; i < n->items.size; i++)
        ac_note_writes_in(vm, *(ASTNode**)array_get(&n->items, i), depth + 1);
}

static void ac_scan(VM *vm, ASTNode *n, int depth) {
    if (!n || depth > 256 || vm->ac_off) return;

    struct ASTNode *outer = vm->ac_scope;
    if (n->token == TOK_ARROW_F) vm->ac_scope = n;

    if ((n->token == TOK_EQ || TOKEN_IS_COMPOUND_ASSIGN(n->token)) && n->left && n->right)
        ac_note_writes_in(vm, n->left, 0);
    // `a++` keeps its operand on the left, `++a` on the right (update_op_split).
    else if (TOKEN_IS_INC_DEC(n->token) && (!n->left != !n->right))
        ac_note_writes_in(vm, n->left ? n->left : n->right, 0);
    // `name : T`, with or without an `=` above it -- either way it declares.
    else if (n->token == TOK_COLON && is_ident(n->left))
        ac_note_write(vm, n->left->token);
    // `for x in xs`. The index of `for i, x in xs` arrives BESIDE the `in` node
    // rather than under it (see the for branch in compile_stmt_into), so it is
    // picked up from the two-item control clause instead.
    else if (n->token == TOK_IN && n->left && n->right)
        ac_note_writes_in(vm, n->left, 0);
    else if (n->token == 0 && n->items.size == 2) {
        ASTNode *a1 = paren_inner(unwrap_chain(*(ASTNode**)array_get(&n->items, 1)));
        if (a1 && a1->token == TOK_IN)
            ac_note_writes_in(vm, paren_inner(unwrap_chain(*(ASTNode**)array_get(&n->items, 0))), 0);
    }
    // A parameter binds its name to whatever the caller passed, which is a write
    // in every sense that matters here -- and a const of that name would make the
    // parameter unreadable (const_reject_write says so at the declaration).
    else if (n->token == TOK_ARROW_F && n->left)
        ac_note_writes_in(vm, n->left, 0);

    ac_scan(vm, n->left,  depth + 1);
    ac_scan(vm, n->right, depth + 1);
    for (size_t i = 0; i < n->items.size; i++)
        ac_scan(vm, *(ASTNode**)array_get(&n->items, i), depth + 1);
    vm->ac_scope = outer;
}

// Start of a compilation unit: forget the previous unit's counts and re-count.
// Runs whether or not the flag is set -- the census is one allocation-free walk,
// and a `#push`ed `#enable auto_const` partway down a unit that opened disabled
// must find the counts already there. Whether a given declaration may use them is
// autoconst_eligible's question, asked with the flags in force where it stands.
static void autoconst_scan(VM *vm, ASTNode *root) {
    vm->ac_count  = 0;
    vm->ac_off    = 0;
    vm->ac_scope  = 0;
    vm->blk_depth = 0;
    ac_scan(vm, root, 0);
}

// May the declaration of `name` being compiled right now become a constant?
// The caller has already established that this is a declaration -- not a shared
// variable, not a host buffer, not an existing local -- and that the initialiser
// compiled to a single constant node.
static int autoconst_eligible(VM *vm, InternID name) {
    if (vm->ac_off || vm->ac_suspend || !(vm->flags & VM_FLAG_AUTO_CONST)) return 0;
    if (vm->blk_depth != 1) return 0;   // runs conditionally; the local is real
    for (int i = 0; i < vm->ac_count; i++)
        if (vm->ac_names[i].name == name) return vm->ac_names[i].writes == 1;
    return 0;
}

// Emit a const's value pinned to its declared type, exactly (rather than
// emitting the literal and converting, which would round an fx16 value
// through whatever the literal rewire happened to pick first).
static int emit_const_typed(VM *vm, Func *f, double v, VMType t, VMType *out_t) {
    int ir;
    if (t.kind == VMT_F32 || t.kind == VMT_F64) {
        ir = ir_new(vm, f, IR_CONST_F);
        f->nodes[ir].type.kind = t.kind;
        f->nodes[ir].kf = (t.kind == VMT_F32) ? (float)v : v;
    } else {
        int shift = ct_shift(t);
        ir = ir_new(vm, f, IR_CONST_I);
        f->nodes[ir].type.kind = t.kind;
        f->nodes[ir].type.len  = shift;
        f->nodes[ir].ki = shift > 0 ? (long long)(v * (double)(1LL << shift)) : (long long)v;
    }
    *out_t = f->nodes[ir].type;
    return ir;
}

// Re-emit an auto-const's value: the IR constant node the initialiser folded
// to, rebuilt verbatim. Unlike emit_const_typed this does not re-scale -- the
// stored word IS what the node held, fixed-point raw and all -- which is what
// makes the fold value-preserving where a round trip through a double would not
// be (an i64 past 2^53).
static int emit_const_auto(VM *vm, Func *f, VMConst *c, VMType *out_t) {
    int is_float = (c->type.kind == VMT_F32 || c->type.kind == VMT_F64);
    int ir = ir_new(vm, f, is_float ? IR_CONST_F : IR_CONST_I);
    f->nodes[ir].type = c->type;
    if (is_float) f->nodes[ir].kf = c->value;
    else          f->nodes[ir].ki = c->ivalue;
    *out_t = c->type;
    return ir;
}

// Record a declaration that turned out to be constant. `ir` is the single
// IR_CONST_* node its initialiser compiled to and `rt` the type compile_expr
// reported for it. Returns 0 only when the allocation fails, and the caller
// then declares an ordinary local -- nothing is lost but the optimisation.
//
// Both halves of a VMConst are filled, and they say different things:
//
//   ivalue/type  the node VERBATIM -- the raw word, fixed-point and all. This
//                is what emit_const_auto rebuilds at each use, and why the fold
//                cannot change a value or lose an i64's high bits.
//   value/num_flags  the same number as a plain REAL, which is what every other
//                reader of a VMConst expects: `const k = x * 2` folds through
//                const_eval, which knows nothing about shifts or node kinds.
//
// `rt` -- what compile_expr HANDED BACK -- rather than the node's own type: a
// node's type.len does not reliably carry the fixed-point shift, which is why an
// ordinary declaration takes the shift from `rt` too (ct_shift(rt), at the
// sym_add this replaces). Off the node instead, `x = 2.5 as fx16` loses its
// shift and re-emits as its raw word.
static int autoconst_declare(VM *vm, Func *f, InternID name, int ir, VMType rt) {
    VMConst *c = (VMConst*)mem_alloc(&vm->run.mem, sizeof(VMConst));
    if (!c) return 0;
    vm->run.sys->memset(c, 0, sizeof(*c));
    c->name     = name;
    c->type     = rt;
    c->has_type = 1;
    c->is_auto  = 1;
    c->scope    = vm->cur_scope;
    if (f->nodes[ir].op == IR_CONST_F) {
        c->value     = f->nodes[ir].kf;
        c->num_flags = (rt.kind == VMT_F32) ? NUM_FLOAT : NUM_DOUBLE;
    } else {
        int shift    = ct_shift(rt);
        c->ivalue    = f->nodes[ir].ki;
        c->value     = shift > 0 ? (double)c->ivalue / (double)(1LL << shift)
                                 : (double)c->ivalue;
        c->num_flags = shift > 0 ? NUM_DOUBLE : NUM_INTEGER;
    }
    c->next     = vm->consts;
    vm->consts  = c;
    return 1;
}

// Fold a `const` initialiser to a single value at the declaration.
//
// Deliberately narrow: number literals, the arithmetic over them, and other consts.
// Anything else would have to be re-evaluated at every use site, which is the cost
// a const exists to avoid; those go to comptime_eval instead.
//
// `num_type` is the literal KIND the fold came out as, not merely "is it an
// integer": an unannotated const is re-emitted as a literal of that kind at each
// use, so `3f` must not collapse to a double. Integerness follows the operators:
// `const half = 1/2` is 0.5 and `1 /~ 2` is 0.
static int const_num_rank(int nt) {
    return (nt & NUM_TYPE) == NUM_INTEGER ? 0 : (nt & NUM_TYPE) == NUM_FLOAT ? 1 : 2;
}

// The kind a binary operator's result carries: the wider operand's, which is
// what the compiler gives the same expression written inline (f32*i32 is f32,
// f32*f64 is f64).
static int const_num_join(int a, int b) {
    return const_num_rank(a) >= const_num_rank(b) ? (a & NUM_TYPE) : (b & NUM_TYPE);
}

static int const_eval_is_binop(InternID tok) {
    return tok == TOK_PLUS || tok == TOK_MINUS || tok == TOK_MUL || tok == TOK_SLASH
        || tok == TOK_PERCENT || tok == TOK_STARSTAR || tok == TOK_LSHIFT || tok == TOK_RSHIFT
        || tok == TOK_SLASH_TILDE || tok == TOK_SLASH_PERCENT || tok == TOK_DOUBLE_PERCENT;
}

// `hard` distinguishes the two ways this returns 0. A SHAPE refusal leaves it clear
// and compile_const_decl falls back to comptime_eval. A genuine ERROR in arithmetic
// this function does understand -- a zero divisor, a self-reference -- sets it, and
// is reported as-is rather than quietly producing the interpreter's answer for it.
static int const_eval(VM *vm, ASTNode *n, double *out, int *num_type, int depth, int *hard) {
    n = paren_inner(n);
    if (!n) return 0;
    if (depth > 32) { vm_set_error_at(vm, n, "constant expression nests too deeply"); *hard = 1; return 0; }

    if (n->number_flags != NUM_NONE && !n->left && !n->right) {
        *out = n->number;
        *num_type = n->number_flags & NUM_TYPE;
        return 1;
    }
    if (is_ident(n)) {
        VMConst *c = const_find(vm, n->token);
        if (!c && builtin_const(vm, n->token, out)) { *num_type = NUM_DOUBLE; return 1; }
        if (!c) {
            vm_errorf_at(vm, n, "'%s' is not a constant -- a const initialiser can only use "
                                "literals and other consts",
                         intern_get_cstr(vm->intern, n->token));
            *hard = 1;
            return 0;
        }
        if (c->evaluating || c->str) {
            vm_errorf_at(vm, n, "constant '%s' is defined in terms of itself",
                         intern_get_cstr(vm->intern, n->token));
            *hard = 1;
            return 0;
        }
        *out = c->value;
        *num_type = c->num_flags & NUM_TYPE;
        return 1;
    }
    // Unary +/- (the parser leaves the operand on the right).
    if (!n->left && n->right && (n->token == TOK_MINUS || n->token == TOK_PLUS)) {
        if (!const_eval(vm, n->right, out, num_type, depth + 1, hard)) return 0;
        if (n->token == TOK_MINUS) *out = -*out;
        return 1;
    }
    if (!n->left && n->right && n->token == TOK_SLASH) {
        if (!const_eval(vm, n->right, out, num_type, depth + 1, hard)) return 0;
        if (*out == 0) { vm_set_error_at(vm, n, "divide by zero in constant expression"); *hard = 1; return 0; }
        int both_int = (*num_type & NUM_TYPE) == NUM_INTEGER;
        if (both_int && (vm->flags & VM_FLAG_C_DIVISION)) {
            *out = (double)(1LL / (long long)*out);
        } else {
            *out = 1.0 / *out;
            if (both_int) *num_type = NUM_DOUBLE;
        }
        return 1;
    }
    // Binary arithmetic. The token is checked BEFORE the operands are: a call
    // like `sqrt(2)` is also a left/right pair, and recursing into it first
    // would blame `sqrt` for not being a constant rather than saying what a
    // const initialiser may contain.
    if (n->left && n->right && const_eval_is_binop(n->token)) {
        double a, b; int at, bt;
        if (!const_eval(vm, n->left,  &a, &at, depth + 1, hard)) return 0;
        if (!const_eval(vm, n->right, &b, &bt, depth + 1, hard)) return 0;
        int join = const_num_join(at, bt);
        int both_int = (join == NUM_INTEGER);
        switch (n->token) {
            case TOK_PLUS:  *out = a + b; *num_type = join; return 1;
            case TOK_MINUS: *out = a - b; *num_type = join; return 1;
            case TOK_MUL:   *out = a * b; *num_type = join; return 1;
            case TOK_SLASH:
                if (b == 0) { vm_set_error_at(vm, n, "divide by zero in constant expression"); *hard = 1; return 0; }
                if (both_int && (vm->flags & VM_FLAG_C_DIVISION)) {
                    *out = (double)((long long)a / (long long)b);
                    *num_type = join;
                } else {
                    *out = a / b;
                    *num_type = both_int ? NUM_DOUBLE : join;
                }
                return 1;
            case TOK_SLASH_TILDE:
            case TOK_SLASH_PERCENT: {
                if (b == 0) { vm_set_error_at(vm, n, "divide by zero in constant expression"); *hard = 1; return 0; }
                double q = a / b;
                *out = (n->token == TOK_SLASH_PERCENT) ? m_floor(q) : (q < 0 ? -m_floor(-q) : m_floor(q));
                *num_type = NUM_INTEGER;
                return 1;
            }
            case TOK_DOUBLE_PERCENT: {
                if (b == 0) { vm_set_error_at(vm, n, "modulo by zero in constant expression"); *hard = 1; return 0; }
                if (!both_int) { vm_set_error_at(vm, n, "'%%' in a constant expression needs integers"); *hard = 1; return 0; }
                long long m = (long long)a % (long long)b;
                if (m != 0 && ((m < 0) != (b < 0))) m += (long long)b;
                *out = (double)m;
                *num_type = NUM_INTEGER;
                return 1;
            }
            case TOK_PERCENT:
                if (b == 0) { vm_set_error_at(vm, n, "modulo by zero in constant expression"); *hard = 1; return 0; }
                if (!both_int) { vm_set_error_at(vm, n, "'%' in a constant expression needs integers"); *hard = 1; return 0; }
                *out = (double)((long long)a % (long long)b);
                *num_type = NUM_INTEGER;
                return 1;
            case TOK_STARSTAR: {
                if (both_int && b >= 0) {
                    long long r = 1, base = (long long)a;
                    for (long long k = 0; k < (long long)b; k++) r *= base;
                    *out = (double)r; *num_type = NUM_INTEGER;
                } else {
                    // Integer operands with a negative exponent leave the
                    // integers behind (2 ** -1 is 0.5), so the result is a
                    // double; float operands stay float.
                    *out = m_pow(a, b);
                    *num_type = both_int ? NUM_DOUBLE : join;
                }
                return 1;
            }
            case TOK_LSHIFT:
            case TOK_RSHIFT:
                if (!both_int) { vm_set_error_at(vm, n, "a shift in a constant expression needs integers"); *hard = 1; return 0; }
                *out = (double)(n->token == TOK_LSHIFT ? ((long long)a << (long long)b)
                                                       : ((long long)a >> (long long)b));
                *num_type = NUM_INTEGER;
                return 1;
            default: break;
        }
    }
    vm_set_error_at(vm, n, "a const initialiser must be literal arithmetic "
                           "(numbers, other consts, + - * / /~ /% % %% ** << >>)");
    return 0;
}

// ---------- comptime evaluation ----------
//
// A const initialiser is compiled into a throwaway zero-parameter Func and RUN,
// which is what lets it contain calls, loops and tables rather than only the
// arithmetic const_eval above can fold by hand.
//
// The scratch Func is deliberately NOT linked into vm->run.funcs and gets no func
// id: emit_collect_funcs walks that whole list, so a scratch func left on it would
// be written into the generated C. Calls OUT of it still resolve, via funcs_by_id.

// How many interpreter steps a single const initialiser may take. Generous
// enough for a computed table, small enough that a runaway recursion in a
// half-typed line returns an error instead of hanging the editor -- which
// matters because this runs on every keystroke in the live-coding loop.
#define VM_COMPTIME_BUDGET 2000000

// Why an initialiser cannot be evaluated, carried out of the walk so the error
// can name the thing rather than just the const.
typedef enum {
    CT_IMPURE_GLOBAL,      // a shared variable
    CT_IMPURE_HOST_BUF,    // a host-bound buffer
    CT_IMPURE_NATIVE,      // a native that is not a pure function of its args
    CT_IMPURE_UNFINISHED   // a function still being compiled
} CtImpureKind;

typedef struct {
    CtImpureKind kind;
    InternID     name;     // the offending symbol or function
    InternID     via;      // the called function it was reached through, or 0
} CtImpure;

static int comptime_ir_pure(VM *vm, Func *f, int ir, CtImpure *why);

// Is calling `g` at compile time honest? Derived from its body: a script function is
// pure when it touches no shared variable and no host buffer and calls nothing
// impure. See Func.ct_purity.
//
// Interprocedural, because walking only the initialiser catches `const k = g` but
// not `peek = () => g` + `const k = peek()` -- and the second would bake whatever
// the host's globals block happens to hold into the program.
static int func_comptime_pure(VM *vm, Func *g, CtImpure *why) {
    if (!g) return 0;
    if (g->ct_purity == CT_PURITY_PURE || g->ct_purity == CT_PURITY_WALKING) return 1;
    if (g->ct_purity == CT_PURITY_IMPURE) return 0;
    if (g->is_template) return 1;   // no body of its own; a call reaches a spec
    if (g->ct_purity == CT_PURITY_COMPILING) {
        why->kind = CT_IMPURE_UNFINISHED;
        why->name = g->name;
        return 0;
    }
    g->ct_purity = CT_PURITY_WALKING;
    int pure = comptime_ir_pure(vm, g, g->body, why);
    g->ct_purity = pure ? CT_PURITY_PURE : CT_PURITY_IMPURE;
    return pure;
}

// Walk one function's IR. Done over the compiled IR rather than the AST: a
// global read and a native call are unambiguous here, and a #define expands
// late (see VMDefine), so an AST-level check would be inspecting a tree that is
// not the one compiled.
static int comptime_ir_pure(VM *vm, Func *f, int ir, CtImpure *why) {
    if (ir < 0) return 1;
    IRNode *n = &f->nodes[ir];
    if (n->op == IR_LOCAL) {
        int slot = (int)n->ki;
        if (slot >= 0 && slot < f->n_syms) {
            VMSym *s = &f->syms[slot];
            // Covers writes too: an assignment's lvalue is an IR_LOCAL, so a
            // function that STORES to a shared variable is refused here rather
            // than being allowed to mutate host state from the compiler.
            if (s->is_global || s->is_host_buf) {
                why->kind = s->is_global ? CT_IMPURE_GLOBAL : CT_IMPURE_HOST_BUF;
                why->name = s->name;
                return 0;
            }
        }
    }
    if (n->op == IR_CALL) {
        int fid = (int)n->ki;
        Func *callee = (fid >= 0 && fid < vm->run.n_func_ids) ? vm->run.funcs_by_id[fid] : 0;
        if (!callee) return 1;
        if (callee->native_tok != 0) {
            int idx = callee->native_tok - TOK_MATHS_FIRST;
            CFuncEntry *e = (idx >= 0 && idx < vm->run.cfunc_table_cap)
                          ? &vm->run.cfunc_table[idx] : 0;
            if (!e || !e->is_pure || e->wants_ctx) {
                why->kind = CT_IMPURE_NATIVE;
                why->name = callee->name;
                return 0;
            }
        } else if (!func_comptime_pure(vm, callee, why)) {
            if (!why->via) why->via = callee->name;
            return 0;
        }
    }
    if (!comptime_ir_pure(vm, f, n->a, why)) return 0;
    if (!comptime_ir_pure(vm, f, n->b, why)) return 0;
    if (!comptime_ir_pure(vm, f, n->c, why)) return 0;
    if (n->items_begin >= 0)
        for (int i = 0; i < n->n_items; i++)
            if (!comptime_ir_pure(vm, f, f->child_indices[n->items_begin + i], why)) return 0;
    return 1;
}

// Refuse anything the compile-time run cannot honestly evaluate, and say what.
static int comptime_purity_check(VM *vm, Func *f, int ir, ASTNode *at) {
    CtImpure why;
    why.kind = CT_IMPURE_GLOBAL; why.name = 0; why.via = 0;
    if (comptime_ir_pure(vm, f, ir, &why)) return 1;

    const char *what, *tail;
    switch (why.kind) {
        case CT_IMPURE_HOST_BUF:
            what = "the host buffer";
            tail = "the host binds it after this is compiled";
            break;
        case CT_IMPURE_NATIVE:
            what = "the native";
            tail = "it is not a pure function of its arguments";
            break;
        case CT_IMPURE_UNFINISHED:
            what = "the function";
            tail = "it is still being compiled here";
            break;
        default:
            what = "the shared variable";
            tail = "its value at compile time is not its value at run time";
            break;
    }
    const char *nm = why.name ? intern_get_cstr(vm->intern, why.name) : "?";
    if (why.via)
        vm_errorf_at(vm, at,
            "this const cannot be evaluated at compile time: '%s' reaches %s '%s' -- %s",
            intern_get_cstr(vm->intern, why.via), what, nm, tail);
    else
        vm_errorf_at(vm, at,
            "this const cannot be evaluated at compile time: it uses %s '%s' -- %s",
            what, nm, tail);
    return 0;
}

// Put the rewire into the state the initialiser should be compiled in.
//
// `declared` is the const's annotation, or VMT_VOID for an untyped const. Untyped,
// and annotated under #enable const_precise, both mean full precision: identity
// rewire, integer literals widened to i64. Annotated under #disable const_precise
// points the literal map at the declared type, so natives inside resolve to that
// type's variant. See VM_FLAG_CONST_PRECISE.
static void comptime_set_rewire(VM *vm, VMType declared, int has_type) {
    VTKind all[] = { VMT_I32, VMT_I64, VMT_F32, VMT_F64 };
    // Read before the maps move: this asks whether the TARGET still has i64,
    // which the identity reset below would otherwise make trivially true.
    int have_i64 = (rewire_apply(vm, VMT_I64, REWIRE_TYPE) == VMT_I64);
    for (int i = 0; i < 4; i++) set_rewire(vm, all[i], all[i], 0, REWIRE_LITERAL);
    if (has_type && !(vm->flags & VM_FLAG_CONST_PRECISE)) {
        for (int i = 0; i < 4; i++)
            set_rewire(vm, all[i], declared.kind, ct_shift(declared), REWIRE_LITERAL);
        return;
    }
    // Precise means precise, so the TYPE rewire stands down alongside the literal
    // one: with `#rewire f64 -> fxN` live, promote() would make `f64 + f64` inside
    // the initialiser a fixed-point add and truncate both operands before the const
    // ever has a value. The fx conversion belongs at the use site instead, where
    // compile_const_use re-emits the value as a literal.
    for (int i = 0; i < 4; i++) set_rewire(vm, all[i], all[i], 0, REWIRE_TYPE);
    if (VM_HAS_I64 && have_i64)
        set_rewire(vm, VMT_I32, VMT_I64, 0, REWIRE_LITERAL);
}

// Compile `init` into a scratch Func, run it, hand back the value.
// Returns 0 having set an error.
static int comptime_eval(VM *vm, ASTNode *init, VMType declared, int has_type,
                         double *out, int *num_type) {
    snapshot_save(vm);
    comptime_set_rewire(vm, declared, has_type);

    Func *cf = func_alloc(&vm->run);
    if (!cf) { snapshot_restore(vm); vm_set_error_at(vm, init, "out of memory"); return 0; }
    cf->structs = vm->structs;
    cf->flags = vm->flags | VM_FLAG_SNAPSHOT;

    VMType rt;
    int e = compile_expr(vm, cf, init, &rt);
    if (e < 0) { snapshot_restore(vm); return 0; }
    // Purity first: a call to a function still being compiled has no return
    // type yet, so the shape check below would blame the value for not being a
    // number rather than naming the actual problem.
    if (!comptime_purity_check(vm, cf, e, init)) { snapshot_restore(vm); return 0; }
    if (rt.kind != VMT_I32 && rt.kind != VMT_I64
     && rt.kind != VMT_F32 && rt.kind != VMT_F64) {
        snapshot_restore(vm);
        vm_errorf_at(vm, init, "a const must be a number: this initialiser is not");
        return 0;
    }

    int ret = ir_new(vm, cf, IR_RETURN);
    cf->nodes[ret].a = e;
    cf->ret_type  = rt;
    cf->has_return = 1;
    int blk = ir_new(vm, cf, IR_BLOCK);
    cf->nodes[blk].items_begin = ir_alloc_items(vm, cf, 1);
    cf->child_indices[cf->nodes[blk].items_begin] = ret;
    cf->nodes[blk].n_items = 1;
    cf->body = blk;
    layout_frame(cf);

    unsigned char *frame = (unsigned char*)mem_alloc(&vm->run.mem, cf->frame_size);
    if (!frame) { snapshot_restore(vm); vm_set_error_at(vm, init, "out of memory"); return 0; }
    vm->run.sys->memset(frame, 0, cf->frame_size);

    // The interpreter writes these; the compile around us is mid-flight and
    // must not see them move.
    const char *saved_err = vm->run.last_error;
    int saved_row = vm->run.error_row, saved_col = vm->run.error_col;

    Args a;
    args_bind(&a, cf, frame, cf->frame_size);
    VMStatus st = func_run(cf, &a, VM_COMPTIME_BUDGET);

    if (st != VM_OK) {
        const char *why = (st == VM_BUDGET)
            ? "it did not finish -- an endless loop, or too much work for one compile"
            : (vm->run.last_error ? vm->run.last_error : "it failed to run");
        vm->run.last_error = saved_err;
        vm->run.error_row = saved_row; vm->run.error_col = saved_col;
        snapshot_restore(vm);
        vm_errorf_at(vm, init, "this const could not be evaluated at compile time: %s", why);
        return 0;
    }
    vm->run.last_error = saved_err;
    vm->run.error_row = saved_row; vm->run.error_col = saved_col;

    // The type the initialiser actually evaluated at becomes the literal kind
    // the const re-emits as, so `const a = sqrt(2f)` stays a float literal the
    // way `sqrt(2f)` written inline is a float expression.
    int shift = ct_shift(rt);
    switch (rt.kind) {
        case VMT_I32: *out = shift ? (double)args_get_i32_result(&a) / (double)(1LL << shift)
                                   : (double)args_get_i32_result(&a);
                      *num_type = shift ? NUM_DOUBLE : NUM_INTEGER; break;
#if VM_HAS_I64
        case VMT_I64: *out = (double)args_get_i64_result(&a); *num_type = NUM_INTEGER; break;
#endif
#if VM_HAS_F32
        case VMT_F32: *out = (double)args_get_f32_result(&a); *num_type = NUM_FLOAT; break;
#endif
        default:      *out = args_get_f64_result(&a); *num_type = NUM_DOUBLE; break;
    }
    snapshot_restore(vm);
    return 1;
}

// A function body reading a name that only some other body writes -- the
// enclosing one, a sibling, the unit's top level.
static int written_in_other_scope(VM *vm, InternID name) {
    Func *s = vm->cur_scope;
    if (!s || vm->ac_off || vm->lib_depth > 0) return 0;
    ASTNode *here = s->template_ast;
    for (int i = 0; i < vm->ac_count; i++)
        if (vm->ac_names[i].name == name)
            return vm->ac_names[i].scopes > 1 || vm->ac_names[i].scope != here;
    return 0;
}

// Resolve an identifier that names a const, emitting its value here.
static int compile_const_use(VM *vm, Func *f, ASTNode *n, VMConst *c, VMType *out_t) {
    if (c->str) return compile_expr(vm, f, c->str, out_t);
    if (c->is_auto) return emit_const_auto(vm, f, c, out_t);
    if (c->has_type) return emit_const_typed(vm, f, c->value, c->type, out_t);
    return emit_number_value(vm, f, c->value, c->num_flags, NULL, n, out_t);
}

// Substitute a define in a position that wants exactly one value. A list
// define has nowhere to put its extra values here; the only place it fits is a
// call's argument list, which splices it before this is ever reached.
static int compile_define_use(VM *vm, Func *f, ASTNode *n, VMDefine *d, VMType *out_t) {
    const char *nm = intern_get_cstr(vm->intern, d->name);
    if (define_arity(d) != 1) {
        vm_errorf_at(vm, n, "'%s' expands to %d values, so it can only be used as "
                            "call arguments -- not here", nm, define_arity(d));
        return -1;
    }
    if (d->expanding) {
        vm_errorf_at(vm, n, "'%s' expands into itself", nm);
        return -1;
    }
    d->expanding = 1;
    int ir = compile_expr(vm, f, define_value(d, 0), out_t);
    d->expanding = 0;
    return ir;
}

static int compile_ident(VM *vm, Func *f, ASTNode *n, VMType *out_t) {
    for (int u = vm->n_unroll - 1; u >= 0; u--)
        if (vm->unroll[u].name == n->token && vm->unroll[u].f == f && vm->unroll[u].lib_depth == vm->lib_depth) {
            *out_t = vm->unroll[u].type;
            return ir_const_of(vm, f, vm->unroll[u].type, vm->unroll[u].ki, vm->unroll[u].kf);
        }
    VMDefine *d = define_find(vm, n->token);
    if (d) return compile_define_use(vm, f, n, d, out_t);
    VMConst *k = const_find(vm, n->token);
    if (k) return compile_const_use(vm, f, n, k, out_t);
    VMSym *s = resolve_name(vm, f, n->token);
    if (!s) {
#if VM_REACTIVE
        if (vm->rx_name_reported) return -1;
#endif
        double bv;
        if (builtin_const(vm, n->token, &bv)) return emit_number_value(vm, f, bv, NUM_DOUBLE, NULL, n, out_t);
        if (written_in_other_scope(vm, n->token)) {
            const char *nm = intern_get_cstr(vm->intern, n->token);
            vm_errorf_at(vm, n, "'%s' is defined outside this function; pass it in as a parameter, "
                                "or make it a const: const %s = ...", nm, nm);
            return -1;
        }
        vm_errorf_at(vm, n, "unknown identifier: %s", intern_get_cstr(vm->intern, n->token));
        return -1;
    }
    if (s->is_func) {
        vm_errorf_at(vm, n, "function '%s' used as value", intern_get_cstr(vm->intern, n->token));
        return -1;
    }
#if VM_REACTIVE
    if (s->is_comp) {
        vm_errorf_at(vm, n, "'%s' is a component; call it: %s(...)",
                     intern_get_cstr(vm->intern, n->token), intern_get_cstr(vm->intern, n->token));
        return -1;
    }
#endif
    s->used = 1;
    int ir = ir_new(vm, f, IR_LOCAL);
    // Carry the symbol's fixed-point shift on the node too, so consumers that
    // inspect node types (array-literal element unification, index reads) see
    // the same shift compile_expr hands back in *out_t.  The interpreter never
    // reads type.len off an IRNode, only off VMSym/Func.ret_type.
    f->nodes[ir].type = ct_set_shift(s->type, s->shift);
    f->nodes[ir].ki = (s - f->syms);
    *out_t = ct_set_shift(s->type, s->shift);
    return ir;
}

// ---------- structs: the unit's table ----------
//
// A struct instance is one contiguous record laid out as C lays out `struct NAME`,
// stored as an ordinary [S]u8 aggregate; a ref to one is a []u8 slice over it.
// VMType.struct_id is what keeps script code from using either as bytes -- the
// invariant is that a struct's bytes are never observable from script code, which
// is what lets the C emitter use a real `struct` and the script backends a table.

#define VM_STRUCT_MAX_SIZE 65535

// A named struct's id in the unit's table, or 0.
static int struct_find(VM *vm, InternID name) {
    VMStructTable *tab = vm->structs;
    if (!tab || name == 0) return 0;
    for (int i = 0; i < tab->count; i++)
        if (tab->items[i].name == name) return i + 1;
    return 0;
}

static const VMStruct *struct_get(VM *vm, int id) {
    return vm_struct_get(vm->structs, id);
}

// FNV-1a over what two programs sharing a record must agree on: the struct's
// name and size, and each field's name (as text -- ids are per parser), storage,
// offset and nested layout.
static unsigned long long struct_hash_bytes(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long struct_layout_hash(VM *vm, const VMStructTable *tab, int id, int depth) {
    const VMStruct *st = vm_struct_get(tab, id);
    unsigned long long h = 14695981039346656037ULL;
    if (!st || depth > 16) return h;
    const char *nm = st->name ? intern_get_cstr(vm->intern, st->name) : "";
    h = struct_hash_bytes(h, nm, s_strlen(nm));
    h = struct_hash_bytes(h, &st->size, sizeof(st->size));
    for (int i = 0; i < st->n_fields; i++) {
        const VMStructField *fd = &st->fields[i];
        const char *fn = intern_get_cstr(vm->intern, fd->name);
        int w[8] = { (int)fd->type.kind, fd->type.len, fd->type.pack_bits, fd->type.inner_len,
                     fd->offset, fd->width, fd->shift, fd->type.elem_shift };
        h = struct_hash_bytes(h, fn, s_strlen(fn));
        h = struct_hash_bytes(h, w, sizeof(w));
        if (fd->type.struct_id) {
            unsigned long long in = struct_layout_hash(vm, tab, fd->type.struct_id, depth + 1);
            h = struct_hash_bytes(h, &in, sizeof(in));
        }
    }
    return h;
}

// The four shapes a struct_id can ride on. inner_len holds the stride of an
// array or slice of records, so it is what tells one record from many.
static int vmt_is_record(VMType t)        { return t.struct_id && is_array(t.kind) && !t.inner_len; }
static int vmt_is_record_array(VMType t)  { return t.struct_id && is_array(t.kind) &&  t.inner_len; }
static int vmt_is_ref(VMType t)           { return t.struct_id && is_slice(t.kind) && !t.inner_len; }
static int vmt_is_struct_slice(VMType t)  { return t.struct_id && is_slice(t.kind) &&  t.inner_len; }

// The element count of an aggregate when it is a compile-time fact, or -1.
//
// `kind` says how storage is passed -- an array is owned, a slice is a view --
// and `len` says how many elements there are; the two are independent. A slice
// keeps its count whenever the compiler put one there: a row of a nested array
// (compile_row_slice), and a parameter that took a fixed array
// (template_view_of, which changes only the kind). `len` 0 is the genuinely
// run-time-length slice.
//
// Every consumer of that fact goes through here, so `.len` folding and `...`
// spreading cannot drift apart -- they were separately written once, and the
// spread's copy was the stricter of the two for no reason.
static int vmt_known_count(VMType t) {
    if (!is_array(t.kind) && !is_slice(t.kind)) return -1;
    if (is_slice(t.kind) && t.len <= 0) return -1;
    return t.inner_len ? t.len / t.inner_len : t.len;
}

// One record by value: an [S]u8.
static VMType struct_record_type(VM *vm, int id) {
    VMType t = {0};
    const VMStruct *st = struct_get(vm, id);
    t.kind = VMT_ARR_I32;
    t.len = st ? st->size : 0;
    t.pack_bits = 8;
    t.struct_id = id;
    return t;
}

// The name a diagnostic uses for a struct: its own, or a description of an
// inferred shape.
static const char *struct_name_s(VM *vm, int id) {
    const VMStruct *st = struct_get(vm, id);
    if (!st) return "?";
    return st->name ? intern_get_cstr(vm->intern, st->name) : "{...}";
}

// Do two struct ids -- each against its own table, since a callee may come from
// another unit -- name the same shape? Named structs match by name and layout; an
// inferred one matches any struct with the same fields.
static int struct_layout_eq(const VMStruct *a, const VMStruct *b) {
    if (a->size != b->size || a->n_fields != b->n_fields) return 0;
    for (int i = 0; i < a->n_fields; i++) {
        const VMStructField *fa = &a->fields[i], *fb = &b->fields[i];
        if (fa->name != fb->name || fa->offset != fb->offset || fa->width != fb->width
            || fa->shift != fb->shift || fa->type.kind != fb->type.kind
            || fa->type.len != fb->type.len || fa->type.pack_bits != fb->type.pack_bits
            || fa->type.inner_len != fb->type.inner_len
            || (fa->type.struct_id != 0) != (fb->type.struct_id != 0))
            return 0;
    }
    return 1;
}

static int struct_ids_match(const VMStructTable *ta, int ida, const VMStructTable *tb, int idb) {
    if (ida == 0 || idb == 0) return ida == idb;
    if (ta == tb && ida == idb) return 1;
    const VMStruct *a = vm_struct_get(ta, ida), *b = vm_struct_get(tb, idb);
    if (!a || !b) return 0;
    if (a->name && b->name && a->name != b->name) return 0;
    return struct_layout_eq(a, b);
}

// Same storage shape and, when either carries one, the same struct. Both types
// are against the unit being compiled.
static int struct_types_same(VM *vm, VMType a, VMType b) {
    if (!a.struct_id != !b.struct_id) return 0;
    if (a.kind != b.kind || a.len != b.len || a.inner_len != b.inner_len) return 0;
    return struct_ids_match(vm->structs, a.struct_id, vm->structs, b.struct_id);
}

static int struct_field_index(const VMStruct *st, InternID name) {
    for (int i = 0; i < st->n_fields; i++)
        if (st->fields[i].name == name) return i;
    return -1;
}

// "x, y, buf" -- what an unknown-field error lists.
static const char *struct_field_list(VM *vm, const VMStruct *st, char *buf, int buf_sz) {
    int p = 0;
    for (int i = 0; i < st->n_fields && p < buf_sz - 1; i++) {
        const char *nm = intern_get_cstr(vm->intern, st->fields[i].name);
        if (i && p < buf_sz - 2) { buf[p++] = ','; buf[p++] = ' '; }
        for (int k = 0; nm && nm[k] && p < buf_sz - 1; k++) buf[p++] = nm[k];
    }
    buf[p] = 0;
    return buf;
}

// An IR_FIELD. Children go in as indices, so nothing here holds a pointer into
// f->nodes across ir_new.
static int ir_field(VM *vm, Func *f, int base, int sub, long long ki, int b, VMType t) {
    int ir = ir_new(vm, f, IR_FIELD);
    f->nodes[ir].a = base;
    f->nodes[ir].b = b;
    f->nodes[ir].sub_op = sub;
    f->nodes[ir].ki = ki;
    f->nodes[ir].type = t;
    return ir;
}

// Field `fi` of the record `base` yields: a load for a scalar, the field in place
// for an array or a nested record.
static int struct_field_node(VM *vm, Func *f, int base, const VMStruct *st, int fi, VMType *out_t) {
    VMStructField fd = st->fields[fi];
    if (fd.width != FIELD_AGG) {
        *out_t = fd.type;
        return ir_field(vm, f, base, fd.width, fd.offset, -1, fd.type);
    }
    int c = ir_i32(vm, f, fd.type.len);
    *out_t = fd.type;
    return ir_field(vm, f, base, field_agg_sub(fd.type.pack_bits), fd.offset, c, fd.type);
}

// A ref used as a value is the record it points at: element 0 of it. That is also
// how emitted C spells it, `(*a)`.
static int ref_to_value(VM *vm, Func *f, int e, VMType *t) {
    if (e < 0 || !vmt_is_ref(*t)) return e;
    VMType rec = struct_record_type(vm, t->struct_id);
    int z = ir_i32(vm, f, 0);
    *t = rec;
    return ir_field(vm, f, e, FIELD_ELEM, rec.len, z, rec);
}

// May this record-yielding node be written through, or passed by ref? A local
// may, and so may anything reached through a slice (it points at someone's
// storage) -- but not a record a call returned by value, which is gone by the
// next statement.
static int struct_lvalue_ok(Func *f, int ir) {
    for (int guard = 0; ir >= 0 && guard < 64; guard++) {
        IRNode *n = &f->nodes[ir];
        if (n->op == IR_LOCAL) return 1;
        if (n->op == IR_FIELD) {
            if (is_slice(f->nodes[n->a].type.kind)) return 1;
            ir = n->a;
            continue;
        }
        return 0;
    }
    return 0;
}

// Parse a type annotation node.  Handles:
//   i32 / f32 / f64                      -> scalar
//   [N]i32 / [N]f32 / [N]f64            -> fixed array   (N = integer literal)
//   []i32 / []f32 / []f64               -> slice
// The type-annotation AST can be:
//   - a plain ident (scalar)
//   - an impl-call whose left is a bracket and right is an ident  ([N]i32 no space)
//   - an items-wrapper whose first item is a bracket and second is an ident  ([N] i32)
// Sub-word element widths: "u1"/"u2"/"u4"/"u8"/"u16" name a packed i32 array
// element, never a scalar. Returns the bit width, or 0 if `s` is not one.
// Only widths dividing 32 are accepted so no element straddles a word.
static int parse_pack_width(const char *s) {
    if (s[0] != 'u') return 0;
    int w = 0, digits = 0;
    for (int i = 1; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        w = w * 10 + (s[i] - '0');
        digits++;
    }
    if (!digits) return 0;
    if (w == 1 || w == 2 || w == 4 || w == 8 || w == 16) return w;
    return 0;
}

// The plain scalar type names. `int`, `float` and `double` are aliases for i32,
// f32 and f64 -- the same type under a C spelling, not a distinct kind -- so they
// are interchangeable everywhere a type name is written. Returns 0 if `s` names
// none of them (fxN and the packed uN widths have their own spellings).
static int parse_base_type_name(const char *s, VTKind *out) {
    if (s_strcmp(s, "i32") == 0 || s_strcmp(s, "int")    == 0) { *out = VMT_I32; return 1; }
    if (s_strcmp(s, "f32") == 0 || s_strcmp(s, "float")  == 0) { *out = VMT_F32; return 1; }
    if (s_strcmp(s, "f64") == 0 || s_strcmp(s, "double") == 0) { *out = VMT_F64; return 1; }
    if (s_strcmp(s, "i64") == 0)                               { *out = VMT_I64; return 1; }
    return 0;
}

// The GLSL vector names: vec2..vec4 are [N]f32, ivec2..ivec4 are [N]i32.
static int parse_vec_type_name(const char *s, VTKind *elem, int *n) {
    VTKind k = VMT_F32;
    if (s[0] == 'i') { k = VMT_I32; s++; }
    if (s[0] != 'v' || s[1] != 'e' || s[2] != 'c' || s[3] < '2' || s[3] > '4' || s[4]) return 0;
    *elem = k;
    *n = s[3] - '0';
    return 1;
}

// `open_len`, when given, lets the length be `_` ([_]u1): the array comes back
// with len 0 and *open_len set, for the caller to size from an initialiser.
static int parse_type_ex(VM *vm, ASTNode *n, VMType *out, int apply_rewire, int *open_len) {
    // Whole, not field by field: a VMType field added later would otherwise
    // keep whatever the caller's uninitialised local held.
    VMType zero = {0};
    *out = zero;
    if (open_len) *open_len = 0;
    if (!n) return 0;
    // Plain ident: i32 / f32 / f64 (or int / float / double)
    if (is_ident(n)) {
        const char *s = intern_get_cstr(vm->intern, n->token);
        VTKind base;
        if (parse_base_type_name(s, &base)) {
            out->kind = apply_rewire ? rewire_apply(vm, base, REWIRE_TYPE) : base;
            out->len  = apply_rewire ? rewire_shift(vm, base, REWIRE_TYPE) : 0;
            return 1;
        }
        int vn;
        if (parse_vec_type_name(s, &base, &vn)) {
            VTKind ek = apply_rewire ? rewire_apply(vm, base, REWIRE_TYPE) : base;
            out->kind = arr_of(ek);
            out->len  = vn;
            if (apply_rewire && ek == VMT_I32) out->elem_shift = rewire_shift(vm, base, REWIRE_TYPE);
            return 1;
        }
        {
            int sid = struct_find(vm, n->token);
            if (sid) { *out = struct_record_type(vm, sid); return 1; }
        }
        // uN is storage, not a value type -- it only means anything as an
        // array element, where it selects the packing width.
        if (parse_pack_width(s)) {
            vm_errorf_at(vm, n, "%s is only valid as an array element type (e.g. [16]%s)", s, s);
            return 0;
        }
        // fxN: fixed-point i32 with N fractional bits
        if (s[0]=='f' && s[1]=='x') {
            int shift = 0;
            for (int i = 2; s[i] >= '0' && s[i] <= '9'; i++) {
                shift = shift * 10 + (s[i] - '0');
                if (s[i+1] != 0) continue;
                if (shift <= 32) { out->kind = apply_rewire ? rewire_apply(vm, VMT_I32, REWIRE_TYPE) : VMT_I32; int rs = apply_rewire ? rewire_shift(vm, VMT_I32, REWIRE_TYPE) : 0; out->len = (out->kind == VMT_I32 && rs > 0) ? rs : (out->kind == VMT_I32 ? shift : 0); return 1; }
                return 0;
            }
            return 0;
        }
        return 0;
    }
    // Extract bracket + element-ident from the AST.
    // The parser produces: (impl left=bracket right=wrapper) where wrapper
    // is either a plain ident or a chain-wrapper containing the ident.
    // When the right side is an injected-bracket scope with multiple items
    // (e.g. the comma in `(a: [3]i32, s:i32)` is consumed as a second
    // argument to the implicit call), take only the first item as the
    // element type -- the rest belong to the next parameter.
    // `ref my` -- one record, by reference. A comma after it is folded into the
    // implicit call (`(a: ref my, b: int)` hands `ref` two arguments), so only the
    // first is the type; see param_list for where the rest go.
    if ((n->token == TOK_EMPTYSTRING || n->token == 0) && ident_is(vm, n->left, "ref") && n->right) {
        ASTNode *r = unwrap_chain(n->right);
        if (r && !is_ident(r) && r->left_bracket == 0 && !r->left && r->items.size > 0)
            r = unwrap_chain(*(ASTNode**)array_get(&r->items, 0));
        int sid = is_ident(r) ? struct_find(vm, r->token) : 0;
        if (!sid) {
            vm_set_error_at(vm, n, "ref needs a struct type, as ref my");
            return 0;
        }
        out->kind = VMT_SLICE_I32;
        out->pack_bits = 8;
        out->struct_id = sid;
        return 1;
    }
    ASTNode *brack = 0;
    ASTNode *elem  = 0;
    // `const[]i32` -- a read-only view. Written WITHOUT a space, which is what
    // tells it apart from the `const NAME = expr` statement: bound tight, the
    // parser hands back impl(impl(const, []), i32), so the bracket hangs off
    // n->left->right and the element is where it always is, in n->right. That
    // makes this a prefix on the ordinary []T extraction below rather than a
    // shape of its own.
    //
    // `const` is not a parser keyword -- it is matched by string compare at
    // statement head only (see compile_stmt_into) -- so inside an annotation it is an
    // ordinary identifier and nothing had to change in the parser.
    int want_const = 0;
    if ((n->token == TOK_EMPTYSTRING || n->token == 0) && n->left
        && (n->left->token == TOK_EMPTYSTRING || n->left->token == 0)
        && ident_is(vm, n->left->left, "const")
        && is_bracket(n->left->right)) {
        // `const[N]T` has no meaning: a fixed array is the callee's own copy, so
        // there is no one else's storage to protect. Say that rather than falling
        // through to "expected [N]type or []type".
        if (n->left->right->items.size != 0) {
            vm_set_error_at(vm, n, "const applies to a view; a [N] array is already a copy of its own");
            return 0;
        }
        want_const = 1;
        // Re-point at the ordinary []T shape: bracket on the left, element right.
        ASTNode *inner = n->left->right;
        // `q: const[]i32 = a` swallows the initialiser too, so the element can
        // arrive as the `=`'s left -- directly when it was the only argument
        // (unwrap_chain collapses the one-item wrapper onto it), or as item[0]
        // when a parameter followed. annotation_swallowed_init reads the other
        // half of the same shape.
        ASTNode *rhs   = type_arg_split_init(unwrap_chain(n->right), 0);
        if (is_ident(rhs)) { brack = inner; elem = rhs; }
        else if (rhs && rhs->left_bracket == 0 && rhs->items.size > 0) {
            ASTNode *a0 = type_arg_split_init(unwrap_chain(*(ASTNode**)array_get(&rhs->items, 0)), 0);
            if (is_ident(a0)) { brack = inner; elem = a0; }
        }
        if (!brack) {
            vm_set_error_at(vm, n, "const[] needs an element type, as const[]i32");
            return 0;
        }
    }
    // `const[]` with no element: the element type is inferred, which only a
    // parameter can do -- template_param_type handles it and never gets here.
    // Anywhere else there is nothing to infer from.
    else if ((n->token == TOK_EMPTYSTRING || n->token == 0)
             && ident_is(vm, n->left, "const")
             && is_bracket(n->right) && n->right->items.size == 0) {
        vm_set_error_at(vm, n, "const[] infers its element type from the caller, so it only works "
                               "as a parameter; write const[]i32 here");
        return 0;
    }
    // A `const` that did NOT bind tight to a bracket. One spelling while this
    // settles, and it keeps `const` unambiguously the statement keyword whenever
    // whitespace follows it.
    else if ((n->token == TOK_EMPTYSTRING || n->token == 0) && ident_is(vm, n->left, "const")) {
        vm_set_error_at(vm, n, "write the const slice type without a space, as const[]i32");
        return 0;
    }
    // [N][M]i32 -- a nested fixed array, which is one flat [N*M] aggregate with
    // the row width in inner_len, exactly as a nested array literal builds it.
    // The parser groups the dimensions to the left, ([N][M])i32, so both
    // brackets hang off n->left and only the inner one names the storage.
    int outer_len = 0;
    if ((n->token == TOK_EMPTYSTRING || n->token == 0) && n->left
        && (n->left->token == TOK_EMPTYSTRING || n->left->token == 0)
        && is_bracket(n->left->left) && is_bracket(n->left->right)
        && n->left->left->items.size == 1) {
        ASTNode *sz  = paren_inner(*(ASTNode**)array_get(&n->left->left->items, 0));
        ASTNode *rhs = unwrap_chain(n->right);
        if (!is_ident(rhs) && rhs->left_bracket == 0 && rhs->items.size > 0) {
            // Two brackets bind tighter than the comma, so anything written
            // after this parameter has been folded into the type and would
            // otherwise vanish without a word. Say so instead.
            if (rhs->items.size > 1) {
                vm_set_error_at(vm, n, "a [N][M] parameter must be the last one in the list");
                return 0;
            }
            rhs = *(ASTNode**)array_get(&rhs->items, 0);
        }
        if (!is_number(sz) || (sz->number_flags & NUM_TYPE) != NUM_INTEGER) {
            vm_set_error_at(vm, n->left->left, "outer array length must be an integer literal");
            return 0;
        }
        if (is_ident(rhs)) { outer_len = (int)sz->number; brack = n->left->right; elem = rhs; }
    }
    // [N]i32  (no space: impl-call or null-token pair with left=bracket)
    if (!brack
        && (n->token == TOK_EMPTYSTRING || n->token == 0) && is_bracket(n->left) && is_ident(n->right)) {
        brack = n->left; elem = n->right;
    }
    else if (!brack
             && (n->token == TOK_EMPTYSTRING || n->token == 0) && is_bracket(n->left)) {
        ASTNode *rhs = n->right;
        rhs = unwrap_chain(rhs);
        if (is_ident(rhs)) { brack = n->left; elem = rhs; }
        else if (rhs->left_bracket == 0 && rhs->items.size > 0) {
            ASTNode *a0 = *(ASTNode**)array_get(&rhs->items, 0);
            if (is_ident(a0)) { brack = n->left; elem = a0; }
        }
    }
    // [N] i32  (space: items wrapper with two items)
    if (!brack && n->token == 0 && !n->number_flags && !n->left && !n->right
        && n->left_bracket == 0 && n->items.size == 2) {
        ASTNode *a0 = *(ASTNode**)array_get(&n->items, 0);
        ASTNode *a1 = unwrap_chain(*(ASTNode**)array_get(&n->items, 1));
        if (is_bracket(a0) && is_ident(a1)) { brack = a0; elem = a1; }
    }
    if (!brack || !elem) { vm_set_error_at(vm, n, "expected [N]type or []type"); return 0; }
    if (!elem->token)    { vm_set_error_at(vm, n, "bad element type name");   return 0; }
    const char *elem_s = intern_get_cstr(vm->intern, elem->token);
    if (!elem_s)         { vm_set_error_at(vm, n, "unknown element type");     return 0; }
    VTKind ek = VMT_VOID;
    VTKind elem_base;
    int elem_shift = 0;   // fixed-point shift of an i32 element (0 = raw int)
    int pack_bits  = 0;   // sub-word packing width (0 = one element per slot)
    // uN elements stay VMT_I32 aggregates -- packing is storage only, so no
    // new VTKind and no rewire (a packed element has no fixed-point meaning).
    if (struct_find(vm, elem->token)) {
        // [N]my / []my: records laid end to end, inner_len holding the stride --
        // the same slot a nested array keeps its row width in, which is why an
        // [N][M]my has nowhere to go.
        int sid = struct_find(vm, elem->token);
        const VMStruct *st = struct_get(vm, sid);
        if (outer_len) {
            vm_errorf_at(vm, n, "[N][M]%s is not supported; use [N*M]%s", elem_s, elem_s);
            return 0;
        }
        if (brack->items.size == 0) {
            out->kind = VMT_SLICE_I32;
        } else {
            ASTNode *sz = paren_inner(*(ASTNode**)array_get(&brack->items, 0));
            if (brack->items.size != 1 || !is_number(sz) || (sz->number_flags & NUM_TYPE) != NUM_INTEGER
                || sz->number < 1) {
                vm_set_error_at(vm, brack, "array length must be a positive integer literal");
                return 0;
            }
            if ((long long)sz->number * st->size > 0x00FFFFFF) {
                vm_errorf_at(vm, brack, "[%d]%s is too large", (int)sz->number, elem_s);
                return 0;
            }
            out->kind = VMT_ARR_I32;
            out->len  = (int)sz->number * st->size;
        }
        out->pack_bits = 8;
        out->inner_len = st->size;
        out->struct_id = sid;
        out->is_const  = want_const;
        return 1;
    }
    else if ((pack_bits = parse_pack_width(elem_s)) != 0) {
        ek = VMT_I32;
    }
    else if (parse_base_type_name(elem_s, &elem_base)) {
        ek = apply_rewire ? rewire_apply(vm, elem_base, REWIRE_TYPE) : elem_base;
        if (apply_rewire && ek == VMT_I32) elem_shift = rewire_shift(vm, elem_base, REWIRE_TYPE);
    }
    else if (elem_s[0]=='f' && elem_s[1]=='x') {
        // [N]fxM / []fxM -- fixed-point i32 elements with M fractional bits.
        int shift = 0, digits = 0;
        for (int i = 2; elem_s[i]; i++) {
            if (elem_s[i] < '0' || elem_s[i] > '9') return 0;
            shift = shift * 10 + (elem_s[i] - '0');
            digits++;
        }
        if (!digits || shift > 32) return 0;
        ek = apply_rewire ? rewire_apply(vm, VMT_I32, REWIRE_TYPE) : VMT_I32;
        if (ek == VMT_I32) {
            int rs = apply_rewire ? rewire_shift(vm, VMT_I32, REWIRE_TYPE) : 0;
            elem_shift = rs > 0 ? rs : shift;
        }
    }
    else if (elem_s[0] == 'u' && elem_s[1] >= '0' && elem_s[1] <= '9') {
        // A uN that parse_pack_width rejected: only widths dividing 32 work,
        // otherwise an element would straddle a word boundary.
        vm_errorf_at(vm, elem, "unsupported packed element type %s (use u1, u2, u4, u8 or u16)", elem_s);
        return 0;
    }
    else return 0;
    if (brack->items.size == 0) {
        // []i32  -> slice
        if (outer_len) {
            vm_set_error_at(vm, n, "a nested array needs both lengths, e.g. [8][3]i32");
            return 0;
        }
        out->kind = slice_of(ek);
        out->elem_shift = elem_shift;
        out->pack_bits = pack_bits;
        out->is_const  = want_const;
        return 1;
    }
    if (brack->items.size == 1) {
        ASTNode *sz = *(ASTNode**)array_get(&brack->items, 0);
        sz = paren_inner(sz);
        if (is_number(sz) && (sz->number_flags & NUM_TYPE) == NUM_INTEGER) {
            out->kind = arr_of(ek);
            out->len  = (int)sz->number;
            out->elem_shift = elem_shift;
            out->pack_bits = pack_bits;
            if (outer_len) { out->inner_len = out->len; out->len *= outer_len; }
            return 1;
        }
        if (ident_is(vm, sz, "_") && !outer_len) {
            if (!open_len) {
                vm_set_error_at(vm, brack, "[_] takes its length from an initialiser; give one, or write the length");
                return 0;
            }
            out->kind = arr_of(ek);
            out->elem_shift = elem_shift;
            out->pack_bits = pack_bits;
            *open_len = 1;
            return 1;
        }
    }
    return 0;
}

static int parse_type(VM *vm, ASTNode *n, VMType *out, int apply_rewire) {
    return parse_type_ex(vm, n, out, apply_rewire, 0);
}

// ---------- expression compilation ----------

// Coerce an already-compiled array-literal element list to one element type.
// Besides the kind, this unifies the *fixed-point shift*: elements widen to
// the widest shift present, the same rule ADD uses for mixed fx/raw operands.
// Without it `[0.0, 0, 0]` under `#rewire f64 -> fx12` would store one fx12
// value next to two raw ints. Returns the common shift for the element type.
static int unify_array_elems(VM *vm, Func *f, int elems_begin, int n, VTKind elem) {
    int shift = 0;
    if (elem == VMT_I32) {
        for (int i = 0; i < n; i++) {
            int s = ct_shift(f->nodes[f->child_indices[elems_begin + i]].type);
            if (s > shift) shift = s;
        }
    }
    VMType want = { VMT_VOID, 0, 0 };
    want.kind = elem;
    want = ct_set_shift(want, shift);
    for (int i = 0; i < n; i++) {
        int ei = f->child_indices[elems_begin + i];
        VMType have = f->nodes[ei].type;
        if (have.kind != elem || ct_shift(have) != shift)
            f->child_indices[elems_begin + i] = insert_fx_convert(vm, f, ei, have, want);
    }
    return shift;
}

// Row width of a nested array literal, or 0 when it is a plain flat one.
// The literal is nested when its FIRST element is a bracket; every other
// element then has to be a row of the same length, because all the rows
// share one flat block of storage and a ragged one has no layout.
// Sets *ok to 0 (and reports) when the shape is wrong.
//
// A row can also be written as an expression -- `[[1,2], a]`, `[a, b]` -- and
// only the compiled type says how long that one is, so those are passed over
// here and checked against the width in compile_array_literal.
static int arr_lit_row_width(VM *vm, ASTNode *node, int *ok) {
    *ok = 1;
    int n = (int)node->items.size;
    if (n == 0) return 0;
    ASTNode *first = paren_inner(*(ASTNode**)array_get(&node->items, 0));
    if (!is_bracket(first)) return 0;
    int w = (int)first->items.size;
    if (w == 0) {
        vm_set_error_at(vm, first, "empty row in nested array literal");
        *ok = 0; return 0;
    }
    for (int i = 1; i < n; i++) {
        ASTNode *row = paren_inner(*(ASTNode**)array_get(&node->items, i));
        if (!is_bracket(row)) continue;   // an expression row; checked once compiled
        if ((int)row->items.size != w) {
            vm_errorf_at(vm, row, "nested array rows must all be %d long, this one is %d",
                         w, (int)row->items.size);
            *ok = 0; return 0;
        }
    }
    return w;
}

// `a[i]` where `a` is a nested array: the i-th row, as a slice of inner_len
// elements over the flat storage. Making a row a plain slice is the point of
// the whole design -- a second `[j]`, `.len`, and passing the row to a []i32
// parameter are then all ordinary slice handling with nothing new behind them.
static int compile_row_slice(VM *vm, Func *f, ASTNode *node, int base, VMType bt,
                             int ie, VMType *out_t) {
    int cols = bt.inner_len;
    if (f->nodes[base].op == IR_ARR_LIT) {
        vm_errorf_at(vm, node, "cannot index a nested array literal directly; assign it to a variable first");
        return -1;
    }
    if (bt.pack_bits && bt.pack_bits < 8) {
        vm_errorf_at(vm, node, "cannot take a row of a u%d array: its elements have no byte address",
                     bt.pack_bits);
        return -1;
    }
    // A constant row index folds to a constant offset, and is then known in
    // range, so the emitted C keeps the clamp out of the common case.
    int in_range = 0, start;
    if (f->nodes[ie].op == IR_CONST_I) {
        long long v = f->nodes[ie].ki;
        in_range = (v >= 0 && v * cols + cols <= bt.len);
        start = ir_i32(vm, f, v * cols);
    } else {
        int k = ir_i32(vm, f, cols);
        start = ir_new(vm, f, IR_BINOP);
        f->nodes[start].sub_op = OP_MUL;
        f->nodes[start].a = ie;
        f->nodes[start].b = k;
        f->nodes[start].type.kind = VMT_I32;
    }
    int len = ir_i32(vm, f, cols);

    int *items = 0, n_items = 0, cap_items = 0;
    base  = stage_operand(vm, f, base,  &items, &n_items, &cap_items);
    start = stage_operand(vm, f, start, &items, &n_items, &cap_items);

    int items_begin = ir_alloc_items(vm, f, 3);
    f->child_indices[items_begin + 0] = base;
    f->child_indices[items_begin + 1] = start;
    f->child_indices[items_begin + 2] = len;
    int sl = ir_new(vm, f, IR_SLICE);
    f->nodes[sl].ki          = bt.pack_bits;
    f->nodes[sl].sub_op      = in_range;
    f->nodes[sl].items_begin = items_begin;
    f->nodes[sl].n_items     = 3;
    f->nodes[sl].type.kind       = slice_of(arr_elem(bt.kind));
    // The row width is a compile-time constant, so a row is a SIZED slice:
    // `.len` folds to it and `a[i] + a[j]` vectorizes, exactly as the whole
    // array does. This is compile-time knowledge only -- the storage and the
    // run-time slice header are untouched, and an out-of-range row still
    // clamps to empty and reports the bounds error on the inner index.
    f->nodes[sl].type.len        = cols;
    f->nodes[sl].type.elem_shift = 0;
    f->nodes[sl].type.pack_bits  = bt.pack_bits;
    // A row of a read-only array is read-only too -- see VMType.is_const on the
    // rule that a field-by-field build has to carry it explicitly.
    f->nodes[sl].type.is_const   = bt.is_const;
    f->nodes[sl].type = ct_set_shift(f->nodes[sl].type, ct_shift(bt));
    *out_t = f->nodes[sl].type;
    return inline_comma(vm, f, sl, *out_t, items, n_items);
}

// The range node inside a subscript, or 0: `x[a..b]` and `x[a..=b]` are
// slicing, not indexing. Written against the already-unwrapped index
// expression, so both callers (read and assignment) ask the same question.
static int is_range_node(ASTNode *n) {
    return n && (n->token == TOK_DOTDOT || n->token == TOK_DOTDOT_EQ)
        && n->left && n->right;
}


// The subscript expression of `x[i]`, unwrapped, or 0 if there isn't exactly
// one. compile_index does the real work; this exists so the assignment path can
// look at the same node without compiling anything.
static ASTNode *index_expr(ASTNode *node) {
    if (!node->right || node->right->items.size != 1) return 0;
    ASTNode *iexpr = paren_inner(*(ASTNode**)array_get(&node->right->items, 0));
    if (iexpr && iexpr->items.size == 1 && iexpr->token == 0 && !iexpr->number_flags
        && iexpr->left_bracket == 0 && !iexpr->left)
        iexpr = *(ASTNode**)array_get(&iexpr->items, 0);
    return iexpr;
}

// Defined next to the slice() intrinsic it shares its guts with.
static int compile_slice_range(VM *vm, Func *f, ASTNode *at, ASTNode *base_ast,
                               ASTNode *range, VMType *out_t);

// The element read `base[ie]`, given an already-compiled base of array or slice
// type and an already-compiled i32 index.  Split out of compile_index because
// `for x in a` has no AST node for its index -- the counter is a hidden local --
// so it cannot go through the AST path at all.
//
// `at` is the base's type; `at_node` only positions errors.
static int emit_index_ir(VM *vm, Func *f, ASTNode *at_node, int aref, VMType at,
                         int ie, VMType *out_t) {
    if (at.inner_len > 0)
        return compile_row_slice(vm, f, at_node, aref, at, ie, out_t);

    int ir = ir_new(vm, f, IR_INDEX);
    f->nodes[ir].a = aref;
    f->nodes[ir].b = ie;
    f->nodes[ir].type.kind = arr_elem(at.kind);
    // Sub-word packing width, read back by the interpreter and the C emitter.
    // 0 (the default) is the ordinary one-element-per-slot path. A packed
    // element is always a plain i32 in 0 .. (1<<bits)-1, so it never carries a
    // fixed-point shift.
    f->nodes[ir].ki = at.pack_bits;
    if (!at.pack_bits)
        // The element inherits the array's fixed-point shift; without this an
        // fx12 array reads back as a raw int and every later op is off by 2^12.
        f->nodes[ir].type = ct_set_shift(f->nodes[ir].type, ct_shift(at));
    *out_t = f->nodes[ir].type;
    return ir;
}

// How many arguments `...e` stands for, given the operand's type -- or -1 with
// an error set. IR_CALL's arity is baked into the node, so the count has to be
// a compile-time fact: exactly what vmt_known_count answers, and the same
// question compile_field's `.len` asks.
static int splat_len(VM *vm, ASTNode *at, VMType t) {
    if (vmt_is_struct(t)) {
        vm_set_error_at(vm, at, "cannot spread a struct; name its fields");
        return -1;
    }
    if (!is_array(t.kind) && !is_slice(t.kind)) {
        vm_set_error_at(vm, at, "... can only spread an array");
        return -1;
    }
    int n = vmt_known_count(t);
    if (n < 0) {
        // A sized slice -- a row, or a parameter that took a fixed array -- is
        // spreadable; only one whose length arrives at run time is not.
        vm_set_error_at(vm, at, "cannot spread this slice: its length is not known at compile time. "
                                "Spread a fixed-size array, or index it: f(s[0], s[1])");
        return -1;
    }
    return n;
}

// A spread whose operand has been compiled: the base, its type, and how many
// elements it stands for.
typedef struct { int base; VMType bt; int n; } SplatSrc;

// Compile `...operand`, check it and stage the base. Returns the element count,
// or -1.
//
// Split from the lane emission below because an array literal has to learn
// every spread's length before it can allocate its element run, and only then
// emit the lanes into it.
//
// The base is named once per lane, so an impure one is staged into a hidden
// local and the assignment pushed onto `pre`, which the caller has to run
// before the expression it builds. A pure base (a local read, a constant) is
// shared between the lanes unchanged, exactly as compile_vec_call shares one
// operand node across its lanes.
static int splat_prepare(VM *vm, Func *f, ASTNode *at, SplatSrc *out,
                         int **pre, int *n_pre, int *cap_pre) {
    out->base = compile_expr(vm, f, at->right, &out->bt);
    if (out->base < 0) return -1;
    out->n = splat_len(vm, at, out->bt);
    if (out->n < 0) return -1;
    out->base = stage_operand(vm, f, out->base, pre, n_pre, cap_pre);
    return out->n;
}

// Element `k` of a prepared spread, and the whole of what a spread compiles to:
// an ordinary IR_INDEX at a constant subscript, placed where the argument a
// caller wrote out by hand would go. Nothing downstream -- coercion,
// specialisation, the interpreter, any emitter -- can tell the two apart, which
// is why the feature needs no new IR op.
static int row_lane(VM *vm, Func *f, ASTNode *at, const SplatSrc *s, int k,
                    VMType *out_t) {
    int ke = ir_i32(vm, f, k);
    return emit_index_ir(vm, f, at, s->base, s->bt, ke, out_t);
}

static int splat_lane(VM *vm, Func *f, ASTNode *at, const SplatSrc *s, int k,
                      VMType *out_t) {
    return row_lane(vm, f, at->right, s, k, out_t);
}

// How many elements an array literal's element stands for when it is a ROW
// written as an expression -- `p = [a, b]`. 0 means "a single value", which is
// what a flat literal is made of; -1 means the shape cannot be a row at all.
// The rule is splat_len's: the flat block behind a nested literal is laid out
// at compile time, so only a fixed-size array has a usable length.
static int arr_row_len(VM *vm, ASTNode *at, VMType t) {
    if (vmt_is_struct(t)) {
        vm_set_error_at(vm, at, "a struct cannot be an element of an array literal; "
                                "declare the array as [N]Name and fill it with { .. } rows");
        return -1;
    }
    if (is_slice(t.kind)) {
        vm_set_error_at(vm, at, "cannot use a slice as a row: its length is not known at "
                                "compile time. Use a fixed-size array, or write the row out");
        return -1;
    }
    if (!is_array(t.kind)) return 0;
    if (t.inner_len) {
        vm_set_error_at(vm, at, "arrays nest at most two levels deep");
        return -1;
    }
    return t.len;
}

// Compile one expression row and stage it, exactly as splat_prepare does for a
// spread: the row is named once per lane below, so an impure one has to be
// evaluated into a hidden local first. Returns its element count, 0 for a
// single value, or -1.
static int arr_row_prepare(VM *vm, Func *f, ASTNode *e, SplatSrc *out,
                           int **pre, int *n_pre, int *cap_pre) {
    out->base = compile_expr(vm, f, e, &out->bt);
    if (out->base < 0) return -1;
    out->n = arr_row_len(vm, e, out->bt);
    if (out->n <= 0) return out->n;
    out->base = stage_operand(vm, f, out->base, pre, n_pre, cap_pre);
    return out->n;
}

// Both halves at once, for an argument list -- which knows its own room and can
// emit each lane the moment it is counted.
static int expand_splat(VM *vm, Func *f, ASTNode *at, int *out_ir, VMType *out_t,
                        int room, int **pre, int *n_pre, int *cap_pre) {
    SplatSrc s;
    int n = splat_prepare(vm, f, at, &s, pre, n_pre, cap_pre);
    if (n < 0) return -1;
    if (n > room) {
        vm_errorf_at(vm, at, "spreading this array needs %d slots, only %d left", n, room);
        return -1;
    }
    for (int k = 0; k < n; k++) {
        VMType et;
        int e = splat_lane(vm, f, at, &s, k, &et);
        if (e < 0) return -1;
        out_ir[k] = e;
        if (out_t) out_t[k] = et;
    }
    return n;
}

static int compile_index(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    // node: (impl) left=array_expr right=(impl lb='[' items=[idx])

    // --- Extract and compile the index expression (shared) ---
    if (node->right->items.size != 1) {
        vm_errorf_at(vm, node->right, "expected 1 index, got %d", (int)node->right->items.size);
        return -1;
    }
    ASTNode *iexpr = index_expr(node);

    // A range subscript is a slice, not an index -- and it takes the base as
    // an AST node, so it has to branch before the index is compiled.
    if (is_range_node(iexpr))
        return compile_slice_range(vm, f, node, node->left, iexpr, out_t);

    VMType it;
    int ie = compile_expr(vm, f, iexpr, &it);
    if (ie < 0) return -1;
    // An fx index counts in whole elements: under `#rewire number -> fx16` the
    // 1 in `xs[1]` is stored as 65536.
    VMType raw = { VMT_I32, 0 };
    if (it.kind != VMT_I32 || ct_shift(it) != 0) ie = insert_fx_convert(vm, f, ie, it, raw);

    // --- Inline array literal: [e0, e1, ...][idx] ---
    if (is_bracket(node->left)) {
        ASTNode *arr = node->left;
        int n = (int)arr->items.size;
        if (n == 0) { vm_set_error_at(vm, node->left, "empty array literal"); return -1; }
        // A nested literal indexed in place would need a slice of an rvalue
        // that has no storage anywhere; naming it first gives it some.
        int shape_ok = 1;
        int nested = arr_lit_row_width(vm, arr, &shape_ok);
        if (!shape_ok) return -1;
        if (nested) {
            vm_set_error_at(vm, node->left, "cannot index a nested array literal directly; assign it to a variable first");
            return -1;
        }
        // Compile elements to discover common type.
        int elems_begin = ir_alloc_items(vm, f, n);
        VTKind elem = VMT_I32;
        for (int i = 0; i < n; i++) {
            ASTNode *e = *(ASTNode**)array_get(&arr->items, i);
            e = paren_inner(e);
            VMType et;
            int ee = compile_expr(vm, f, e, &et);
            if (ee < 0) return -1;
            if (vmt_is_struct(et) || is_array(et.kind) || is_slice(et.kind)) {
                vm_set_error_at(vm, node->left, "cannot index a nested array literal directly; "
                                                "assign it to a variable first");
                return -1;
            }
            if (i == 0) elem = et.kind;
            else elem = promote(vm, elem, et.kind);
            f->child_indices[elems_begin + i] = ee;
        }
        // Cast all elements to the common type (kind + fixed-point shift).
        int eshift = unify_array_elems(vm, f, elems_begin, n, elem);
        int arr_lit = ir_new(vm, f, IR_ARR_LIT);
        f->nodes[arr_lit].items_begin = elems_begin;
        f->nodes[arr_lit].n_items = n;
        f->nodes[arr_lit].type.kind = arr_of(elem);
        f->nodes[arr_lit].type.len = n;
        f->nodes[arr_lit].type.elem_shift = eshift;
        int ir = ir_new(vm, f, IR_INDEX);
        f->nodes[ir].a = arr_lit;
        f->nodes[ir].b = ie;
        f->nodes[ir].type.kind = elem;
        f->nodes[ir].type = ct_set_shift(f->nodes[ir].type, eshift);
        *out_t = f->nodes[ir].type;
        return ir;
    }

    // --- General expression index: expr[idx] ---
    // Compile the array/slice expression (may be ident, call, etc.).
    VMType at;
    int aref = compile_expr(vm, f, node->left, &at);
    if (aref < 0) return -1;
    if (!is_array(at.kind) && !is_slice(at.kind)) {
        char desc[128];
        node_describe(vm, node->left, desc, sizeof(desc));
        vm_errorf_at(vm, node->left, "index target must be array or slice, got %s", desc);
        return -1;
    }
    if (vmt_is_struct(at)) {
        char desc[128];
        node_describe(vm, node->left, desc, sizeof(desc));
        if (vmt_is_record(at) || vmt_is_ref(at)) {
            vm_errorf_at(vm, node->left, "%s is a struct; name a field, as p.x", desc);
            return -1;
        }
        // One element of an array or slice of records: the record itself, as the
        // {ptr, len} a field access or a by-ref argument takes.
        VMType rec = struct_record_type(vm, at.struct_id);
        // One record out of a read-only array of them is read-only, which is what
        // makes `s[0].x = 9` an error rather than a hole: the const rides on the
        // element type, so the field store sees it one step later.
        rec.is_const = at.is_const;
        *out_t = rec;
        return ir_field(vm, f, aref, FIELD_ELEM, at.inner_len, ie, rec);
    }
    return emit_index_ir(vm, f, node, aref, at, ie, out_t);
}

// Zig-style field access: `x.len` (or `.length`) gives the element count of an array
// or slice as a raw i32. Those are the only fields the VM knows.
//
// A fixed array carries its length in its type and a string literal carries it on
// the IR_DATA_SLICE node, so both fold to a constant here and the operand is not
// evaluated at run time. A real slice keeps its length beside its pointer, so that
// case compiles to IR_LEN.
// `xy`, `zyx`, `rgba`: 1-4 letters, all from xyzw or all from rgba. Returns the
// component count and fills comp[] with element indices, or 0. *mixed is set
// when the letters are all swizzle letters but from both sets.
static int swizzle_parse(const char *name, int *comp, int *mixed) {
    static const char sets[2][5] = { "xyzw", "rgba" };
    *mixed = 0;
    if (!name) return 0;
    int n = (int)s_strlen(name);
    if (n < 1 || n > 4) return 0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        int k = -1;
        for (int s = 0; s < 2 && k < 0; s++)
            for (int j = 0; j < 4; j++)
                if (name[i] == sets[s][j]) { k = j; used |= 1 << s; break; }
        if (k < 0) return 0;
        comp[i] = k;
    }
    if (used == 3) { *mixed = 1; return 0; }
    return n;
}

// Shared by the read and the write form: the checks a swizzle's base has to pass.
static int swizzle_check_base(VM *vm, ASTNode *node, VMType at, const char *name,
                              const int *comp, int n) {
    if (!is_array(at.kind) && !is_slice(at.kind)) {
        char buf[24];
        vm_errorf_at(vm, node->left, ".%s needs an array, got %s", name, type_label(at, buf, sizeof(buf)));
        return 0;
    }
    if (at.inner_len > 0) {
        vm_errorf_at(vm, node->left, ".%s on a nested array; index a row first, as a[i].%s", name, name);
        return 0;
    }
    int known = vmt_known_count(at);
    for (int i = 0; i < n && known >= 0; i++) {
        if (comp[i] >= known) {
            char desc[128];
            node_describe(vm, node->left, desc, sizeof(desc));
            vm_errorf_at(vm, node->right, ".%c needs %d elements; %s has %d", name[i], comp[i] + 1, desc, known);
            return 0;
        }
    }
    return 1;
}

static int vec_materialise(VM *vm, Func *f, int *lane, VMType *lane_t, int n,
                           int inner, int *pre, int n_pre, int cap_pre, VMType *out_t);

// `v.x` is `v[0]`; `v.zy` is the vector expression [v[2], v[1]]. The base is
// named once per component, so anything but a local read is staged first.
static int compile_swizzle(VM *vm, Func *f, ASTNode *node, int aref, VMType at,
                           const char *name, const int *comp, int n, VMType *out_t) {
    if (!swizzle_check_base(vm, node, at, name, comp, n)) return -1;
    if (n == 1) return emit_index_ir(vm, f, node, aref, at, ir_i32(vm, f, comp[0]), out_t);
    int *pre = 0, n_pre = 0, cap_pre = 0;
    int base = stage_operand(vm, f, aref, &pre, &n_pre, &cap_pre);
    if (f->nodes[base].op != IR_LOCAL) {
        vm_errorf_at(vm, node->left, "cannot swizzle this operand; assign it to a variable first");
        return -1;
    }
    int slot = (int)f->nodes[base].ki;
    int    lane[4];
    VMType lane_t[4];
    for (int i = 0; i < n; i++) {
        lane[i] = emit_index_ir(vm, f, node, strb_local(vm, f, slot), at, ir_i32(vm, f, comp[i]), &lane_t[i]);
        if (lane[i] < 0) return -1;
    }
    return vec_materialise(vm, f, lane, lane_t, n, 0, pre, n_pre, cap_pre, out_t);
}

static int compile_field(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    ASTNode *fld = paren_inner(node->right);
    const char *name = is_ident(fld) ? intern_get_cstr(vm->intern, fld->token) : 0;
    int is_len = name && (s_strcmp(name, "len") == 0 || s_strcmp(name, "length") == 0);
    int swz_comp[4], swz_mixed = 0;
    int swz_n = swizzle_parse(name, swz_comp, &swz_mixed);
    int is_swz = swz_n > 0 && (vm->flags & VM_FLAG_SWIZZLE);
    ASTNode *lft = paren_inner(node->left);
    if (is_ident(lft) && struct_find(vm, lft->token) && !sym_find(f, lft->token)) {
        const char *sn = intern_get_cstr(vm->intern, lft->token);
        vm_errorf_at(vm, node->left, "'%s' is a struct type; declare an instance, as p: %s", sn, sn);
        return -1;
    }
    // Only a struct can have a field besides .len, so anything else is refused
    // before its base is compiled, as it always was.
    int any_struct = vm->structs && vm->structs->count > 0;
    for (int i = 0; !any_struct && i < vm->n_globals; i++)
        if (vm->globals[i].struct_name) any_struct = 1;
    if (swz_mixed && (vm->flags & VM_FLAG_SWIZZLE) && !any_struct) {
        vm_errorf_at(vm, node->right, "swizzle .%s mixes xyzw and rgba", name);
        return -1;
    }
    if (!is_len && !is_swz && !(name && any_struct)) {
        char desc[128];
        node_describe(vm, node->right, desc, sizeof(desc));
        vm_errorf_at(vm, node->right, "unknown field %s (only .len/.length is supported)%s", desc,
                     swz_n > 0 ? " (swizzles are disabled here)" : "");
        return -1;
    }
    VMType at;
    int aref = compile_expr(vm, f, node->left, &at);
    if (aref < 0) return -1;
    // Struct fields resolve BEFORE .len, so a struct may have a field named `len`.
    if (vmt_is_record(at) || vmt_is_ref(at)) {
        const VMStruct *st = struct_get(vm, at.struct_id);
        int fi = st ? struct_field_index(st, fld->token) : -1;
        if (fi < 0) {
            char list[160];
            vm_errorf_at(vm, node->right, "struct '%s' has no field '%s' (fields: %s)",
                         struct_name_s(vm, at.struct_id), name, st ? struct_field_list(vm, st, list, sizeof(list)) : "");
            return -1;
        }
        return struct_field_node(vm, f, aref, st, fi, out_t);
    }
    if (vmt_is_struct(at) && !is_len) {
        char desc[128];
        node_describe(vm, node->left, desc, sizeof(desc));
        vm_errorf_at(vm, node->left, "%s is an array of structs; index it first, as a[i].%s", desc, name);
        return -1;
    }
    if (is_swz) return compile_swizzle(vm, f, node, aref, at, name, swz_comp, swz_n, out_t);
    if (!is_len) {
        char desc[128];
        node_describe(vm, node->right, desc, sizeof(desc));
        vm_errorf_at(vm, node->right, "unknown field %s (only .len/.length is supported)", desc);
        return -1;
    }
    if (vmt_is_struct(at)) {
        // The record count: a fixed array folds it, a slice divides its byte
        // length by the stride at run time.
        int ir;
        if (is_array(at.kind)) {
            ir = ir_i32(vm, f, at.len / at.inner_len);
        } else {
            ir = ir_new(vm, f, IR_LEN);
            f->nodes[ir].a  = aref;
            f->nodes[ir].ki = at.inner_len;
            f->nodes[ir].type.kind = VMT_I32;
        }
        *out_t = f->nodes[ir].type;
        return ir;
    }
    if (!is_array(at.kind) && !is_slice(at.kind)) {
        char desc[128];
        node_describe(vm, node->left, desc, sizeof(desc));
        vm_errorf_at(vm, node->left, ".%s needs an array or slice, got %s", name, desc);
        return -1;
    }
    // A nested array's length is its ROW count -- `a[i]` names a row, so `.len`
    // has to agree with what `for i in 0..a.len` then indexes. A sized slice
    // (a row, or a parameter that took a fixed array) answers from its type the
    // same way; only a genuinely run-time-length slice reads IR_LEN, unless the
    // node in front of us is a literal whose element run is right there.
    int known = vmt_known_count(at);
    if (known < 0 && f->nodes[aref].op == IR_DATA_SLICE) known = f->nodes[aref].n_items;
    int ir;
    if (known >= 0) {
        ir = ir_new(vm, f, IR_CONST_I);
        f->nodes[ir].ki = known;
    } else {
        ir = ir_new(vm, f, IR_LEN);
        f->nodes[ir].a = aref;
    }
    f->nodes[ir].type.kind = VMT_I32;
    f->nodes[ir].type.len  = 0;
    *out_t = f->nodes[ir].type;
    return ir;
}

// Adopt a packed parameter's element width onto an array literal sitting DIRECTLY in
// argument position -- the same move compile_annotated_decl makes for
// `a : [6]u1 = [1,0,1,0,0,1]`.
//
// Only a BARE literal qualifies: it has no packing and no layout of its own yet, so
// there is nothing to lose, and without this a packed parameter is unreachable from
// a literal argument. Once a literal is bound to a variable the variable owns a
// laid-out slot, and handing that to a differently packed parameter is a real
// mismatch -- reported as one by the callers, same restriction as dataslice_to_i32
// above. Constant elements are range-checked as the annotated declaration does;
// non-constant ones pass, since both backends store them through the packed
// accessors.
//
// Returns 1 if the literal adopted the packing, 0 if this is not a literal it
// applies to (the caller then reports the mismatch), -1 if an element does not fit.
static int arr_lit_adopt_pack(VM *vm, Func *f, ASTNode *at, int idx, VMType want) {
    if (idx < 0 || f->nodes[idx].op != IR_ARR_LIT) return 0;
    VMType have = f->nodes[idx].type;
    if (have.pack_bits != 0 || want.pack_bits == 0) return 0;
    if (arr_elem(have.kind) != VMT_I32 || arr_elem(want.kind) != VMT_I32) return 0;
    // A fixed-point element is a scaled quantity, not a small integer, and a
    // nested literal's rows would have to pack per row -- neither is what a
    // uN parameter is asking for, so leave both to the mismatch error.
    if (have.elem_shift != 0 || have.inner_len != 0 || want.inner_len != 0) return 0;
    if (!(vm->flags & VM_FLAG_LOSSY_ASSIGNMENT)) {
        long long lim = 1LL << want.pack_bits;
        for (int i = 0; i < f->nodes[idx].n_items; i++) {
            IRNode *el = &f->nodes[f->child_indices[f->nodes[idx].items_begin + i]];
            if (el->op != IR_CONST_I) continue;
            if (el->ki < 0 || el->ki >= lim) {
                vm_errorf_at(vm, at,
                    "%lld does not fit in u%d (0..%lld); #enable lossy_assignment to allow",
                    el->ki, want.pack_bits, lim - 1);
                return -1;
            }
        }
    }
    // Carry the packing onto the literal so both backends lay the elements out
    // the same way -- the C emitter's compound literal packs per backing word
    // off this field, and the interpreter reads the width from the parameter.
    f->nodes[idx].type.pack_bits = want.pack_bits;
    return 1;
}

// "argument 1 is [6]i32 but the parameter is []u1 ..." -- packing is invisible
// in the kind (both are VMT_ARR_I32/VMT_SLICE_I32), so the plain names are the
// only way the message can show what actually differs.
static void pack_mismatch_error(VM *vm, ASTNode *node, int argi, VMType have, VMType want) {
    char hb[24], wb[24];
    type_label(have, hb, sizeof(hb));
    type_label(want, wb, sizeof(wb));
    if (is_array(have.kind))
        vm_errorf_at(vm, node,
            "argument %d is %s but the parameter is %s: element packing is part of the "
            "storage layout. Declare the value packed, e.g. 'v : [%d]u%d = [...]'",
            argi + 1, hb, wb, have.len, want.pack_bits);
    else
        vm_errorf_at(vm, node,
            "argument %d is %s but the parameter is %s: element packing is part of the "
            "storage layout",
            argi + 1, hb, wb);
}

// One argument where the parameter or the argument is a struct. A `my` parameter
// takes a copy of a record (a ref is dereferenced to one); `ref my` and `[]my` take
// the caller's own storage, so the argument has to be something that outlives the
// call.
static int coerce_struct_arg(VM *vm, Func *f, ASTNode *node, Func *callee, int args_begin, int i) {
    VMType want = callee->syms[callee->param_slot[i]].type;
    int ai = f->child_indices[args_begin + i];
    VMType have = f->nodes[ai].type;
    const char *cn = callee->name ? intern_get_cstr(vm->intern, callee->name) : "function";
    if (vmt_is_record(want) && vmt_is_ref(have)) {
        ai = ref_to_value(vm, f, ai, &have);
        f->child_indices[args_begin + i] = ai;
    }
    int ok;
    if (vmt_is_record(want))            ok = vmt_is_record(have);
    else if (vmt_is_record_array(want)) ok = vmt_is_record_array(have) && have.len == want.len;
    else if (vmt_is_ref(want))          ok = vmt_is_record(have) || vmt_is_ref(have);
    else if (vmt_is_struct_slice(want)) ok = vmt_is_record_array(have) || vmt_is_struct_slice(have);
    else                                ok = 0;
    if (ok) ok = struct_ids_match(callee->structs, want.struct_id, vm->structs, have.struct_id);
    if (!ok) {
        if (!want.struct_id)
            vm_errorf_at(vm, node, "argument %d of '%s' is a struct; the parameter is not", i + 1, cn);
        else {
            const VMStruct *ws = vm_struct_get(callee->structs, want.struct_id);
            const char *wn = ws && ws->name ? intern_get_cstr(vm->intern, ws->name) : "{...}";
            vm_errorf_at(vm, node, "argument %d of '%s' must be %s'%s'", i + 1, cn,
                         vmt_is_struct_slice(want) ? "an array of " : vmt_is_record_array(want) ? "a fixed array of " : "a ",
                         wn);
        }
        return -1;
    }
    if ((vmt_is_ref(want) || vmt_is_struct_slice(want)) && !struct_lvalue_ok(f, ai)) {
        vm_errorf_at(vm, node, "argument %d of '%s': cannot pass a temporary by ref; assign it to a variable first",
                     i + 1, cn);
        return -1;
    }
    return 0;
}

// Coerce already-compiled call arguments to the callee's declared param types.
// Shared by the plain user-function path and the template path (a template
// specialisation's params can carry declared types too, when the function
// mixes annotated and unannotated params).
static int coerce_call_args(VM *vm, Func *f, ASTNode *node, Func *callee,
                            int args_begin, int n_args) {
    for (int i = 0; i < n_args; i++) {
        VMType want = callee->syms[callee->param_slot[i]].type;
        VMType have = f->nodes[f->child_indices[args_begin + i]].type;
        // A read-only view cannot be handed to a parameter that may write through
        // it -- this is the rule the whole guarantee rests on. The reverse, a
        // mutable []T into a const[]T, is the point and costs nothing: the storage
        // is identical and only the permission differs. A by-value [N]T parameter
        // copies, so const never stands in ITS way -- though a slice argument
        // cannot reach one at all, const or not, for want of a copy path.
        if (have.is_const && !want.is_const && is_slice(want.kind)) {
            char hb[32], wb[32];
            const char *cn = callee->name ? intern_get_cstr(vm->intern, callee->name) : "function";
            vm_errorf_at(vm, node,
                "argument %d of '%s' is %s and the parameter is %s, which may write to it. "
                "Make the parameter const, or pass a copy",
                i + 1, cn, type_label(have, hb, sizeof(hb)), type_label(want, wb, sizeof(wb)));
            return -1;
        }
        if (vmt_is_struct(want) || vmt_is_struct(have)) {
            if (coerce_struct_arg(vm, f, node, callee, args_begin, i) < 0) return -1;
            continue;
        }
        if (is_scalar(want.kind) && is_scalar(have.kind)) {
            if (have.kind != want.kind || ct_shift(have) != ct_shift(want))
                f->child_indices[args_begin + i] = insert_fx_convert(vm, f, f->child_indices[args_begin + i], have, want);
        } else if (is_array(want.kind) && is_array(have.kind)) {
            if (arr_elem(want.kind) != arr_elem(have.kind)) {
                vm_set_error_at(vm, node, "array element type mismatch"); return -1;
            }
            if (want.len != have.len) {
                vm_set_error_at(vm, node, "fixed array length mismatch"); return -1;
            }
            // Packing is part of the storage layout, so [8]u4 and [8]i32 are
            // not interchangeable even though both are VMT_ARR_I32 -- unless
            // the argument is a bare literal, which has no layout yet and can
            // simply adopt the parameter's width.
            if (want.pack_bits != have.pack_bits) {
                int ad = arr_lit_adopt_pack(vm, f, node, f->child_indices[args_begin + i], want);
                if (ad < 0) return -1;
                if (!ad) { pack_mismatch_error(vm, node, i, have, want); return -1; }
            }
            if (want.inner_len != have.inner_len) {
                vm_set_error_at(vm, node, "nested array shape mismatch"); return -1;
            }
        } else if (is_slice(want.kind) && is_array(have.kind)) {
            // Array -> Slice: allowed (runtime creates slice ptr)
            if (arr_elem(want.kind) != arr_elem(have.kind)) {
                vm_set_error_at(vm, node, "array element type mismatch for slice"); return -1;
            }
            if (want.pack_bits != have.pack_bits) {
                int ad = arr_lit_adopt_pack(vm, f, node, f->child_indices[args_begin + i], want);
                if (ad < 0) return -1;
                if (!ad) { pack_mismatch_error(vm, node, i, have, want); return -1; }
            }
        } else if (is_slice(want.kind) && is_slice(have.kind)) {
            if (arr_elem(want.kind) != arr_elem(have.kind)) {
                vm_set_error_at(vm, node, "slice element type mismatch"); return -1;
            }
            // A string literal is []u8; a `(s: []i32)` param still accepts one
            // directly, widened at compile time.
            if (want.pack_bits != have.pack_bits
                && !(want.pack_bits == 0
                     && dataslice_to_i32(vm, f, f->child_indices[args_begin + i]))) {
                pack_mismatch_error(vm, node, i, have, want); return -1;
            }
        } else {
            vm_set_error_at(vm, node, "incompatible argument type"); return -1;
        }
    }
    return 0;
}

// Flatten a call's argument list, splicing any argument that is a bare define into
// the values its body holds -- `#define pos (x,y)` makes `at(pos)` a two-argument
// call. The one place a multi-value define fits, so the one place that expands it;
// everywhere else compile_define_use rejects it. Only a bare identifier splices --
// `f((pos))` does not, and neither does a define reached through an expression.
static int expand_call_args(VM *vm, ASTNode *node, ASTNode *par, ASTNode **out, int max) {
    int n = 0;
    for (size_t i = 0; i < par->items.size; i++) {
        ASTNode *a = *(ASTNode**)array_get(&par->items, i);
        // Empty () arrives as a single blank item. A string literal has token 0
        // too but carries ->string, so `f("x")` is not mistaken for `f()`.
        if (par->items.size == 1 && !a->left && !a->right && !a->number_flags
            && a->token == 0 && a->string == 0 && a->items.size == 0 && a->left_bracket == 0)
            return 0;
        // `...[a, b]`: splice the literal's elements in as arguments. The
        // type-driven spread in compile_call handles every other operand, but
        // it cannot help a component call, which takes its arguments as AST.
        ASTNode *spl = splat_literal(paren_inner(a));
        if (spl) {
            int ne = (int)spl->items.size;
            if (n + ne > max) {
                vm_errorf_at(vm, node, "too many arguments after spreading (max %d)", max);
                return -1;
            }
            for (int k = 0; k < ne; k++)
                out[n++] = *(ASTNode**)array_get(&spl->items, k);
            continue;
        }
        VMDefine *d = is_ident(a) ? define_find(vm, a->token) : 0;
        int count = d ? define_arity(d) : 1;
        if (n + count > max) {
            vm_errorf_at(vm, node, "too many arguments (max %d)", max);
            return -1;
        }
        if (d) for (int k = 0; k < count; k++) out[n++] = define_value(d, k);
        else   out[n++] = a;
    }
    return n;
}

// Does `n` name a scalar type usable as a cast target -- i32 / i64 / f32 / f64 /
// fxN? The pack widths are filtered out first so `u8(x)` gets the ordinary
// unknown-identifier error instead of parse_type's advice about array element types
// (parse_type *sets* that error, so it can't be called blind). The type rewire is
// applied, exactly as it is for a declaration.
static int cast_target_type(VM *vm, ASTNode *n, VMType *out) {
    if (!is_ident(n)) return 0;
    const char *s = intern_get_cstr(vm->intern, n->token);
    if (!s || parse_pack_width(s)) return 0;
    if (!parse_type(vm, n, out, 1)) return 0;
    return out->kind == VMT_I32 || out->kind == VMT_I64
        || out->kind == VMT_F32 || out->kind == VMT_F64;
}

// A cast spelling only wins if the name isn't bound to something else. The
// builtin i32/f32/f64/i64 Funcs exist purely so resolve_name() has something
// to find, so they don't count as a binding -- but a script's own `i32` does,
// and keeps its meaning.
static int cast_name_is_free(VM *vm, Func *f, ASTNode *n) {
    VMSym *s = resolve_name(vm, f, n->token);
    if (!s) return 1;
    if (!s->is_func || !s->fn) return 0;
    int nt = s->fn->native_tok;
    return TOKEN_IS_CAST(nt);
}

// mix(a, b, t) expanded inline at fixed-point shift `shift` (the largest the three
// arguments carry). The float variants use a + t*(b-a); at fixed point that form
// rounds (b-a) into the product and loses low bits whenever a and b are close, so
// this emits the algebraically equal
//
//     a*(1.0 - t) + t*b        with 1.0 == 1 << shift
//
// Expanding it here rather than calling one fixed fx16 helper keeps whatever shift
// the call site already uses. With i64 available both products accumulate full-width
// and share a single >>shift; without it each pre-shifts its operands by half the
// shift, the same trade the fixed-point OP_MUL path makes.

// `a >>> n`, the logical shift: the arithmetic one with the sign bits it drags in
// masked off, (a >> n) & ~((-1 << (W - 1 - n)) << 1). It needs no op of its own,
// so no backend has to learn one. The count is taken mod the width like every
// shift's, and staged in a temp because the mask names it again.
static int compile_urshift(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    VMType lt, rt;
    int l = compile_expr(vm, f, node->left, &lt);
    if (l < 0 || reject_void(vm, node->left, lt)) return -1;
    int r = compile_expr(vm, f, node->right, &rt);
    if (r < 0 || reject_void(vm, node->right, rt)) return -1;
    if ((lt.kind != VMT_I32 && lt.kind != VMT_I64) || ct_shift(lt) != 0
        || (rt.kind != VMT_I32 && rt.kind != VMT_I64) || ct_shift(rt) != 0) {
        vm_set_error_at(vm, node, "'>>>' needs integer operands");
        return -1;
    }
    VMType t = ct_vmtype_clear_shift(lt);
    VMType i32t = { VMT_I32, 0 };
    int w1 = lt.kind == VMT_I64 ? 63 : 31;
    if (rt.kind != VMT_I32) r = insert_cvt(vm, f, r, rt, VMT_I32);
    r = ir_binop(vm, f, OP_BAND, r, ir_const_of(vm, f, i32t, w1, 0.0), i32t);
    VMSym *s = sym_add(vm, f, 0, i32t);
    s->used = 1;
    int ts = (int)(s - f->syms);
    int asn = ir_new(vm, f, IR_ASSIGN);
    f->nodes[asn].a = sym_read(vm, f, ts);
    f->nodes[asn].b = r;
    int shr   = ir_binop(vm, f, OP_BSHR, l, sym_read(vm, f, ts), t);
    int cnt   = ir_binop(vm, f, OP_SUB, ir_const_of(vm, f, i32t, w1, 0.0), sym_read(vm, f, ts), i32t);
    int hi    = ir_binop(vm, f, OP_BSHL, ir_const_of(vm, f, t, -1, 0.0), cnt, t);
    hi        = ir_binop(vm, f, OP_BSHL, hi, ir_const_of(vm, f, i32t, 1, 0.0), t);
    int keep  = ir_binop(vm, f, OP_BXOR, hi, ir_const_of(vm, f, t, -1, 0.0), t);
    int value = ir_binop(vm, f, OP_BAND, shr, keep, t);
    *out_t = t;
    return inline_comma(vm, f, value, t, &asn, 1);
}

static int compile_mix_fixed(VM *vm, Func *f, int args_begin, const VMType *arg_types,
                             int shift, VMType *out_t)
{
    VMType fx = { VMT_I32, 0 };
    fx.len = shift;
    int a = insert_fx_convert(vm, f, f->child_indices[args_begin + 0], arg_types[0], fx);
    int b = insert_fx_convert(vm, f, f->child_indices[args_begin + 1], arg_types[1], fx);
    int t = insert_fx_convert(vm, f, f->child_indices[args_begin + 2], arg_types[2], fx);

    // t is named twice below (once in 1.0-t, once in t*b), so bind it to a
    // hidden local unless it is already a constant or a plain local read.
    int *items = 0, n_items = 0, cap_items = 0;
    t = stage_operand(vm, f, t, &items, &n_items, &cap_items);

    // u = 1.0 - t, at t's own shift.
    int one = ir_const_i(vm, f, fx, 1LL << shift);
    int u = ir_binop(vm, f, OP_SUB, one, t, fx);

    int result;
    if (fx_wide_ok(vm)) {
        VMType i64t = { VMT_I64, 0 };
        VMType raw  = ct_vmtype_clear_shift(fx);
        int a64 = insert_cvt(vm, f, a, raw, VMT_I64);
        int u64 = insert_cvt(vm, f, u, raw, VMT_I64);
        int t64 = insert_cvt(vm, f, t, raw, VMT_I64);
        int b64 = insert_cvt(vm, f, b, raw, VMT_I64);
        int p1 = ir_binop(vm, f, OP_MUL, a64, u64, i64t);
        int p2 = ir_binop(vm, f, OP_MUL, t64, b64, i64t);
        int sum = ir_binop(vm, f, OP_ADD, p1, p2, i64t);
        int shc = ir_const_i(vm, f, i64t, shift);
        int shr = ir_binop(vm, f, OP_BSHR, sum, shc, i64t);
        result = insert_cvt_t(vm, f, shr, i64t, fx);
    } else {
        // No i64: (x >> half) * (y >> rest) keeps the product in i32 range
        // and lands back on `shift` without an intermediate widen.
        int half = shift >> 1, rest = shift - half;
        int p[2];
        int lhs[2], rhs[2];
        lhs[0] = a; rhs[0] = u;
        lhs[1] = t; rhs[1] = b;
        VMType raw32 = {0}; raw32.kind = VMT_I32;
        for (int i = 0; i < 2; i++) {
            int ch = ir_i32(vm, f, half);
            int sl = ir_binop(vm, f, OP_BSHR, lhs[i], ch, raw32);
            int cr = ir_i32(vm, f, rest);
            int sr = ir_binop(vm, f, OP_BSHR, rhs[i], cr, raw32);
            p[i] = ir_binop(vm, f, OP_MUL, sl, sr, fx);
        }
        result = ir_binop(vm, f, OP_ADD, p[0], p[1], fx);
    }

    *out_t = fx;
    return inline_comma(vm, f, result, fx, items, n_items);
}

static int compile_user_call_ir(VM *vm, Func *f, ASTNode *at, Func *callee,
                                int args_begin, const VMType *arg_types,
                                int n_args, VMType *out_t);

// Integer operands whose power is i64: natives have no i64 slot, so this is the
// library's __ipow64 (vm_lib.h), exact like m_ipow, where it used to be an i32
// or f64 variant reading the wrong type.
static int ipow_is_i64(VMType a, VMType b) {
    int ints = (a.kind == VMT_I32 || a.kind == VMT_I64) && (b.kind == VMT_I32 || b.kind == VMT_I64);
    return ints && ct_shift(a) == 0 && ct_shift(b) == 0 && (a.kind == VMT_I64 || b.kind == VMT_I64);
}

static int emit_ipow64_call(VM *vm, Func *f, ASTNode *at, int l, VMType lt, int r, VMType rt, VMType *out_t) {
    struct VMLibFunc *e = lib_find(vm, intern_c_string(&vm->globals_owner, "__ipow64"));
    Func *g = e ? lib_template(vm, e, at) : 0;
    if (!g) {
        if (!vm->run.last_error) vm_set_error_at(vm, at, "an i64 power needs the built-in library (vm_register_lib_defaults)");
        return -1;
    }
    VMType i64t = { VMT_I64, 0 };
    l = insert_fx_convert(vm, f, l, lt, i64t);
    int args = ir_alloc_items(vm, f, 2);
    f->child_indices[args + 0] = l;
    f->child_indices[args + 1] = r;
    VMType types[2] = { i64t, rt };
    return compile_user_call_ir(vm, f, at, g, args, types, 2, out_t);
}

// Which native call, given arguments that are already compiled, and their
// types. `at` is only for error positions. Allocates its own IR_CALL, may
// rewrite entries of `args_begin` in place (insert_fx_convert does), and
// returns the IR for the whole call -- which for the compiler-inlined cases is
// not a call at all.
//
// Split out of compile_call so that ONE lane of a vectorized builtin goes
// through the identical type policy -- #rewire, the fx variant, the float-slot
// fallback, the i32 inline blocks, the constant fold -- as the scalar call it
// would have been. See compile_vec_call.
static int compile_native_call_ir(VM *vm, Func *f, ASTNode *at, Func *callee,
                                  int args_begin, const VMType *arg_types,
                                  int n_args, VMType *out_t) {
    if (callee->native_tok == TOK_IPOW && n_args == 2 && ipow_is_i64(arg_types[0], arg_types[1]))
        return emit_ipow64_call(vm, f, at, f->child_indices[args_begin], arg_types[0],
                                f->child_indices[args_begin + 1], arg_types[1], out_t);

    // Does the host need this call to survive as a call? Only in a reactive
    // unit: the inverse pass dispatches on IR_CALL.ki -> native_tok, so an
    // inlined `mix` or a folded `floor` is a drag that silently stops working.
    // An export build is unaffected -- floor(v) on fx16 still becomes AND masks.
    // See CFuncEntry.keep_call and set_c_func_keep_call.
    int keep_call = 0;
#if VM_REACTIVE
    if (callee->native_tok != 0 && vm->rx_active) {
        int kidx = callee->native_tok - TOK_MATHS_FIRST;
        if (kidx >= 0 && kidx < vm->run.cfunc_table_cap)
            keep_call = vm->run.cfunc_table[kidx].keep_call;
    }
#endif

    // ---- floor / frac / min / max at i32 kind: compiler-inlined ----
    // These reduce to a bitmask (floor/frac) or a raw comparison (min/max) at *any*
    // shift -- shift 0 degenerates to the trivial case automatically -- so there is
    // no need to dispatch through cfunc_table: doing it here keeps full precision at
    // whatever shift the arguments carry, instead of round-tripping through one
    // fixed fx16 implementation. Float args fall through to the native-call path.
    if (!keep_call &&
       (callee->native_tok == TOK_FLOOR || callee->native_tok == TOK_FRAC ||
        callee->native_tok == TOK_MIN   || callee->native_tok == TOK_MAX
#ifndef VM_DONT_INLINE_INTEGER_ABS
        || callee->native_tok == TOK_ABS
#endif
        )) {
        VTKind eff0 = n_args > 0 ? arg_types[0].kind : VMT_F64;
        for (int i = 1; i < n_args; i++) eff0 = promote(vm, eff0, arg_types[i].kind);
        if (eff0 == VMT_I32) {
            int c_shift = 0;
            for (int i = 0; i < n_args; i++)
                if (arg_types[i].kind == VMT_I32 && arg_types[i].len > c_shift) c_shift = arg_types[i].len;
            VMType want = { VMT_I32, c_shift };
            int a0 = f->child_indices[args_begin + 0];
            if (arg_types[0].kind != VMT_I32 || arg_types[0].len != c_shift)
                a0 = insert_fx_convert(vm, f, a0, arg_types[0], want);

            if (callee->native_tok == TOK_FLOOR || callee->native_tok == TOK_FRAC) {
                // mask = the low c_shift bits (the fractional part in this
                // representation). floor clears them (rounds toward
                // -infinity via two's-complement AND); frac keeps only them
                // (always non-negative, exactly x - floor(x) at this shift).
                long long mask = c_shift > 0 ? ((1LL << c_shift) - 1) : 0;
                int mc = ir_const_i(vm, f, want, (callee->native_tok == TOK_FLOOR) ? ~mask : mask);
                *out_t = want;
                return ir_binop(vm, f, OP_BAND, a0, mc, want);
            } else if (callee->native_tok == TOK_ABS) {
                // abs(x) = x < 0 ? -x : x, at whatever shift x already carries.
                // x is named three times, so it is bound to a hidden local first
                // -- otherwise the emitted C repeats the whole argument
                // expression, which costs code size on every target that has a
                // budget for it.
                int *items = 0, n_items = 0, cap_items = 0;
                a0 = stage_operand(vm, f, a0, &items, &n_items, &cap_items);
                int zero = ir_const_i(vm, f, want, 0);
                VMType i32t = {0}; i32t.kind = VMT_I32;
                int cmp = ir_binop(vm, f, OP_LT, a0, zero, i32t);
                int neg = ir_unop(vm, f, OP_NEG, a0, want);
                int sel = ir_select(vm, f, cmp, neg, a0, want);
                *out_t = want;
                return inline_comma(vm, f, sel, want, items, n_items);
            } else {
                // min/max: fixed-point ordering matches raw int ordering,
                // so a plain comparison + select needs no special handling
                // once both operands share a shift. Both operands are named
                // twice (the comparison and the select), so both are staged.
                int a1 = f->child_indices[args_begin + 1];
                if (arg_types[1].kind != VMT_I32 || arg_types[1].len != c_shift)
                    a1 = insert_fx_convert(vm, f, a1, arg_types[1], want);
                int *items = 0, n_items = 0, cap_items = 0;
                a0 = stage_operand(vm, f, a0, &items, &n_items, &cap_items);
                a1 = stage_operand(vm, f, a1, &items, &n_items, &cap_items);
                VMType i32t = {0}; i32t.kind = VMT_I32;
                int cmp = ir_binop(vm, f, (callee->native_tok == TOK_MIN) ? OP_LT : OP_GT,
                                   a0, a1, i32t);
                int sel = ir_select(vm, f, cmp, a0, a1, want);
                *out_t = want;
                return inline_comma(vm, f, sel, want, items, n_items);
            }
        }
    }

    // ---- mix at fixed-point kind: compiler-inlined ----
    // Same motivation as the block above; mix registers neither an i32 nor an fx
    // variant. Plain ints with no shift anywhere are NOT handled here -- they
    // promote to f64 through the native path below, the rule sqrt(4) follows.
    if (!keep_call && callee->native_tok == TOK_MIX && n_args == 3) {
        VTKind eff0 = arg_types[0].kind;
        for (int i = 1; i < n_args; i++) eff0 = promote(vm, eff0, arg_types[i].kind);
        if (eff0 == VMT_I32) {
            int c_shift = 0;
            for (int i = 0; i < n_args; i++)
                if (arg_types[i].kind == VMT_I32 && arg_types[i].len > c_shift) c_shift = arg_types[i].len;
            if (c_shift == 0) {
                // All-int arguments: this call would fall back to the float
                // slot below. Under `#rewire f64 -> fxN` that slot *means*
                // fxN, so expand at its shift rather than dragging a double
                // back into code the rewire exists to keep float-free. Same
                // slot-preference order as the fallback itself.
                CFuncEntry *me = &vm->run.cfunc_table[callee->native_tok - TOK_MATHS_FIRST];
                VTKind fb = me->f64_fn ? VMT_F64 : (me->f32_fn ? VMT_F32 : VMT_I32);
                if (fb != VMT_I32 && rewire_apply(vm, fb, REWIRE_TYPE) == VMT_I32)
                    c_shift = rewire_shift(vm, fb, REWIRE_TYPE);
            }
            if (c_shift > 0)
                return compile_mix_fixed(vm, f, args_begin, arg_types, c_shift, out_t);
        }
    }

    int ir   = ir_new(vm, f, IR_CALL);
    f->nodes[ir].items_begin = args_begin;
    f->nodes[ir].n_items = n_args;

    // ---- Native C-function built-in: type dispatch ----
    // Use standard promote() across args (i32+i32->i32, f32+f64->f64, ...).
    // Functions with no i32 variant are float-only: i32 args are promoted to f64.
    if (callee->native_tok != 0) {
        int idx = callee->native_tok - TOK_MATHS_FIRST;
        CFuncEntry *e = &vm->run.cfunc_table[idx];
        // A native sees bytes; a struct's are not script-visible.
        for (int i = 0; i < n_args; i++)
            if (vmt_is_struct(arg_types[i])) {
                vm_errorf_at(vm, at, "argument %d of '%s' is a struct; pass its fields",
                             i + 1, intern_get_cstr(vm->intern, callee->name));
                return -1;
            }

        // ---- Explicit mixed signature (register_c_func_sig): coerce each arg
        // to its declared kind rather than a single promoted type. Scalars
        // (VMT_I32) convert normally; a VMT_SLICE_I32 slot accepts an i32
        // slice (e.g. a string literal) or i32 array as-is. See CFuncEntry. ----
        if (e->has_sig) {
            for (int i = 0; i < n_args; i++) {
                VTKind want = e->arg_kinds[i];
                if (want == VMT_SLICE_I32) {
                    if (!(is_slice(arg_types[i].kind) || is_array(arg_types[i].kind))
                        || arr_elem(arg_types[i].kind) != VMT_I32) {
                        vm_errorf_at(vm, at, "argument %d expects an i32 slice/string", i + 1);
                        return -1;
                    }
                    // Element width: a byte-flagged slot
                    // (register_c_func_arg_bytes) takes const unsigned char*,
                    // which is what a []u8 -- including a string literal --
                    // already is. A slot left at the i32 default takes a
                    // literal too, widened here at compile time.
                    int want_bits = e->arg_pack_bits[i];
                    if (want_bits != arg_types[i].pack_bits
                        && !(want_bits == 0
                             && dataslice_to_i32(vm, f, f->child_indices[args_begin + i]))) {
                        vm_errorf_at(vm, at, "argument %d expects %s", i + 1,
                                     want_bits == 8 ? "a byte slice ([]u8 or a string literal)"
                                                    : "an i32 slice, not a byte slice");
                        return -1;
                    }
                } else { // scalar i32 slot: coerce to the sig's Q-format
                    // insert_fx_convert (not insert_cvt) so a call-site fx value
                    // is rescaled to sig_scalar_shift (BSHR/BSHL), not
                    // reinterpreted as a huge raw int. With shift 0 this scales
                    // down to a whole integer; with a nonzero one a matching fx
                    // argument passes through unchanged. f32/f64 args scale to
                    // that Q-format as usual.
                    VMType want_t = { want, e->sig_scalar_shift };
                    f->child_indices[args_begin + i] = insert_fx_convert(vm, f,
                        f->child_indices[args_begin + i], arg_types[i], want_t);
                }
            }
            f->nodes[ir].ki        = callee->func_id;
            f->nodes[ir].type.kind = e->ret_kind;
            f->nodes[ir].type.len  = 0;
            *out_t = f->nodes[ir].type;
            return ir;
        }

        VTKind eff = n_args > 0 ? arg_types[0].kind : VMT_F64;
        for (int i = 1; i < n_args; i++)
            eff = promote(vm, eff, arg_types[i].kind);

        // Does any i32 argument actually carry a fixed-point shift (e.g. a
        // value declared/cast `fx16`)? A plain untyped int (shift 0) does
        // NOT count -- so `sqrt(4)` still promotes to f64 exactly as if
        // the function had no i32 variant, while `sqrt(x)` with `x: fx16`
        // opts into the fixed-point path below.
        int any_fx_shift = 0;
        if (eff == VMT_I32)
            for (int i = 0; i < n_args; i++)
                if (arg_types[i].kind == VMT_I32 && arg_types[i].len > 0) { any_fx_shift = 1; break; }

        // Resolve which registered variant this call uses. Natural promotion (`eff`)
        // first; if that slot isn't registered, fall back to whichever one IS --
        // preferring float, then fixed -- and coerce every argument to it, so a
        // function may register just ONE numeric variant and still take every
        // argument combination consistently. Functions registering all their
        // variants are unaffected: sqrt(4) still resolves to f64 here.
        int use_fx = (eff == VMT_I32 && e->fx_fn && e->fx_shift > 0 && any_fx_shift);
        if (!use_fx) {
            int have_eff = (eff == VMT_I32 && e->i32_fn) ||
                           (eff == VMT_F32 && e->f32_fn) ||
                           (eff == VMT_F64 && e->f64_fn);
            if (!have_eff) {
                // Route the fallback through the active #rewire first: the slot we
                // would land on is a *type*, and under `#rewire f64 -> fx16` that
                // type means fx16. If the rewired target has no registered variant
                // the plain order still holds.
                VTKind fb = e->f64_fn ? VMT_F64 : (e->f32_fn ? VMT_F32 : VMT_I32);
                VTKind rk = rewire_apply(vm, fb, REWIRE_TYPE);
                int rk_fx = (rk == VMT_I32 && rewire_shift(vm, fb, REWIRE_TYPE) > 0);

                if      (rk_fx && e->fx_fn && e->fx_shift > 0) use_fx = 1;
                else if (rk == VMT_F64 && e->f64_fn)  eff = VMT_F64;
                else if (rk == VMT_F32 && e->f32_fn)  eff = VMT_F32;
                else if (e->f64_fn)                   eff = VMT_F64;
                else if (e->f32_fn)                   eff = VMT_F32;
                else if (e->fx_fn && e->fx_shift > 0) use_fx = 1;
                else if (e->i32_fn)                   eff = VMT_I32;
                // else: no numeric variant registered -> runtime dispatch errors.
            }
        }

        if (use_fx) {
            // Fixed-point variant: align every arg to the function's native shift --
            // raw ints, floats and other fxN shifts alike -- and tag the result with
            // that shift so it composes with other fxN code. sub_op=1 records that
            // this resolved to CFuncEntry.fx_fn rather than i32_fn.
            VMType want = { VMT_I32, 0 }; want.len = e->fx_shift;
            for (int i = 0; i < n_args; i++)
                f->child_indices[args_begin + i] = insert_fx_convert(vm, f, f->child_indices[args_begin + i],
                                          arg_types[i], want);
            f->nodes[ir].ki     = callee->func_id;
            f->nodes[ir].sub_op = 1;
            f->nodes[ir].type   = want;
            *out_t    = f->nodes[ir].type;
            // Folding a fixed-point native is exact in a way the float fold is not:
            // the fold calls the very fx function the emitted C will call, and it is
            // integer-only, so the answer is bit-identical on every target.
            int fx_folded = keep_call ? -1 : try_fold_native_call(vm, f, ir);
            return fx_folded >= 0 ? fx_folded : ir;
        }

        if (eff == VMT_I32) {
            // Raw-int slot: insert_fx_convert (not insert_cvt) so an fx-shifted arg
            // is rescaled to a whole int (>>shift), not reinterpreted as a huge raw
            // value. Plain ints and floats coerce as before.
            VMType want = { VMT_I32, 0 };
            for (int i = 0; i < n_args; i++)
                f->child_indices[args_begin + i] = insert_fx_convert(vm, f, f->child_indices[args_begin + i],
                                          arg_types[i], want);
        } else {
            // Float slot (f32/f64): insert_fx_convert (not insert_cvt) so an
            // fx-shifted i32 arg is rescaled to its real value (divided by
            // 2^shift), not reinterpreted as a huge raw integer. Plain ints and
            // other floats coerce exactly as insert_cvt did before.
            VMType want = { VMT_VOID, 0 }; want.kind = eff;
            for (int i = 0; i < n_args; i++)
                if (arg_types[i].kind != eff)
                    f->child_indices[args_begin + i] = insert_fx_convert(vm, f, f->child_indices[args_begin + i],
                                          arg_types[i], want);
        }
        f->nodes[ir].ki        = callee->func_id;
        f->nodes[ir].type.kind = eff;
        *out_t        = f->nodes[ir].type;
        int folded = keep_call ? -1 : try_fold_native_call(vm, f, ir);
        return folded >= 0 ? folded : ir;
    }

    // Not reached: every caller checks native_tok first.
    vm_errorf_at(vm, at, "internal: not a native call");
    return -1;
}


// ---------- inlining a library body into its caller (#enable inline_builtins) ----------
//
// The call is replaced by the specialisation's own IR, grafted into the caller:
// its locals become fresh caller locals, its parameter reads become the
// argument expressions, and its statements become the items of a comma whose
// value is what it returned. The point is the interpreter, where a call costs a
// frame and a dispatch that an expression does not -- an export compiles to C,
// where the C compiler was already inlining both.
//
// Deliberately narrow. A body qualifies only when it is a straight line ending
// in one return: assignments and expression statements, then IR_RETURN. Control
// flow does not graft into a comma expression (a `for` is not an expression in
// C), and neither does a second return, so those keep the call.
static int inline_ops_ok(Func *sp, int ir, int depth) {
    if (ir < 0) return 1;
    if (depth > 64) return 0;
    const IRNode *n = &sp->nodes[ir];
    switch (n->op) {
        case IR_CONST_I: case IR_CONST_F: case IR_LOCAL: case IR_INDEX:
        case IR_BINOP:   case IR_UNOP:    case IR_CVT:   case IR_CALL:
        case IR_ARR_LIT: case IR_SELECT:  case IR_COMMA: case IR_LEN:
        case IR_SLICE:   case IR_DATA_SLICE: case IR_ASSIGN: case IR_EXPR_STMT:
            break;
        default: return 0;      // IR_IF / IR_FOR / IR_WHILE / a print / an inspect
    }
    if (!inline_ops_ok(sp, n->a, depth + 1)) return 0;
    if (!inline_ops_ok(sp, n->b, depth + 1)) return 0;
    if (!inline_ops_ok(sp, n->c, depth + 1)) return 0;
    if (n->items_begin >= 0)
        for (int i = 0; i < n->n_items; i++)
            if (!inline_ops_ok(sp, sp->child_indices[n->items_begin + i], depth + 1)) return 0;
    return 1;
}

static int inline_spec_ok(Func *sp) {
    if (!sp || sp->body < 0 || sp->n_specs) return 0;
    // Scalars only, in and out. An aggregate parameter is passed by writing the
    // caller's storage into the callee's frame slot -- a graft would have to
    // reproduce that, and assigning an array literal to a slice local does not.
    // The flag exists for the step family, which is scalar throughout.
    for (int i = 0; i < sp->n_params; i++)
        if (!is_scalar(sp->syms[sp->param_slot[i]].type.kind)) return 0;
    if (!is_scalar(sp->ret_type.kind)) return 0;
    // A shared variable or a host buffer is addressed by a slot that means
    // something in ITS function, so a graft would have to re-bind it. Not worth
    // it for a built-in, which reads neither.
    for (int i = 0; i < sp->n_syms; i++)
        if (sp->syms[i].is_global || sp->syms[i].is_host_buf) return 0;
    const IRNode *b = &sp->nodes[sp->body];
    if (b->op != IR_BLOCK || b->n_items < 1) return 0;
    for (int i = 0; i < b->n_items; i++) {
        int st = sp->child_indices[b->items_begin + i];
        int last = (i == b->n_items - 1);
        int op = sp->nodes[st].op;
        if (last) { if (op != IR_RETURN || sp->nodes[st].a < 0) return 0; }
        else if (op != IR_ASSIGN && op != IR_EXPR_STMT) return 0;
        if (!inline_ops_ok(sp, last ? sp->nodes[st].a : st, 0)) return 0;
    }
    return 1;
}

// Copy one node of `sp` into `caller`, children first. `map` holds the caller
// index of each spec node, `slots` the caller slot of each spec sym.
static int inline_copy(VM *vm, Func *caller, Func *sp, int ir, int *map, const int *slots) {
    if (ir < 0) return -1;
    if (map[ir] >= 0) return map[ir];
    const IRNode src = sp->nodes[ir];
    int a = inline_copy(vm, caller, sp, src.a, map, slots);
    int b = inline_copy(vm, caller, sp, src.b, map, slots);
    int c = inline_copy(vm, caller, sp, src.c, map, slots);
    int items = -1;
    if (src.items_begin >= 0 && src.n_items > 0) {
        int *idx = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)src.n_items);
        if (!idx) return -1;
        for (int i = 0; i < src.n_items; i++)
            idx[i] = inline_copy(vm, caller, sp, sp->child_indices[src.items_begin + i], map, slots);
        items = ir_alloc_items(vm, caller, src.n_items);
        for (int i = 0; i < src.n_items; i++) caller->child_indices[items + i] = idx[i];
    }
    int out = ir_new(vm, caller, src.op);
    IRNode *d = &caller->nodes[out];
    d->sub_op = src.sub_op;
    d->type   = src.type;
    d->ki     = src.ki;
    d->kf     = src.kf;
    d->a = a; d->b = b; d->c = c;
    d->items_begin = items;
    d->n_items     = src.n_items;
    // The one field that names a position rather than a value: a slot index.
    if (src.op == IR_LOCAL) d->ki = slots[(int)src.ki];
    map[ir] = out;
    return out;
}

// The grafted expression, or -1 meaning "not inlined, emit the call". Never
// reports an error: every rejection is a fallback, not a failure.
static int inline_spec_body(VM *vm, Func *caller, Func *sp,
                            int args_begin, int n_args, VMType *out_t) {
    if (sp->n_nodes <= 0) return -1;
    int *slots = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)(sp->n_syms ? sp->n_syms : 1));
    int *map = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)sp->n_nodes);
    if (!slots || !map) return -1;
    for (int i = 0; i < sp->n_nodes; i++) map[i] = -1;

    // Every local of the callee becomes a local of the caller, parameters
    // included: the argument is assigned into it, which is what the call would
    // have done, and it keeps an argument named twice in the body from being
    // evaluated twice.
    for (int i = 0; i < sp->n_syms; i++) {
        if (sp->syms[i].is_func) { slots[i] = -1; continue; }
        VMSym *ns = sym_add(vm, caller, 0, sp->syms[i].type);
        if (!ns) return -1;
        ns->shift = sp->syms[i].shift;
        ns->used  = 1;
        slots[i]  = (int)(ns - caller->syms);
    }

    int *pre = 0, n_pre = 0, cap_pre = 0;
    for (int i = 0; i < n_args; i++) {
        int dst = strb_local(vm, caller, slots[sp->param_slot[i]]);
        int asn = ir_new(vm, caller, IR_ASSIGN);
        caller->nodes[asn].a = dst;
        caller->nodes[asn].b = caller->child_indices[args_begin + i];
        stmt_push_idx(vm, &pre, &n_pre, &cap_pre, asn);
    }

    const IRNode *blk = &sp->nodes[sp->body];
    int value = -1;
    for (int i = 0; i < blk->n_items; i++) {
        int st = sp->child_indices[blk->items_begin + i];
        if (i == blk->n_items - 1) {
            value = inline_copy(vm, caller, sp, sp->nodes[st].a, map, slots);
            if (value < 0) return -1;
        } else {
            int cp = inline_copy(vm, caller, sp, st, map, slots);
            if (cp < 0) return -1;
            stmt_push_idx(vm, &pre, &n_pre, &cap_pre, cp);
        }
    }
    *out_t = caller->nodes[value].type;
    return inline_comma(vm, caller, value, *out_t, pre, n_pre);
}

// A call to a script function -- a template specialisation or a plain one --
// given arguments that are already compiled. `at` is for error positions.
// Allocates its own IR_CALL and may rewrite entries of `args_begin` in place.
//
// Split out of compile_call for the reason compile_native_call_ir was: map()
// builds one of these per element, and a lane has to go through exactly the
// same specialisation and coercion as the call a user could have written out.
static int compile_user_call_ir(VM *vm, Func *f, ASTNode *at, Func *callee,
                                int args_begin, const VMType *arg_types,
                                int n_args, VMType *out_t) {
    int ir   = ir_new(vm, f, IR_CALL);
    f->nodes[ir].items_begin = args_begin;
    f->nodes[ir].n_items = n_args;

    // ---- Polymorphic template: find or compile a type-specialised version ----
    if (callee->is_template) {
        // No automatic promotion: sq(2) stays i32, sq(2.0) stays f64.
        Func *spec = find_or_specialize(vm, callee, n_args, arg_types);
        if (!spec) return -1;
        // Unannotated params took their type from the call site, so those args
        // already match; annotated ones (a partly-typed signature) still need
        // the usual coercion to the declared type.
        if (coerce_call_args(vm, f, at, spec, args_begin, n_args) < 0) return -1;
        // #enable inline_builtins: graft a built-in's body in rather than
        // calling it. Library functions only -- the flag is about what writing
        // a built-in in the language costs, not a general inliner.
        if (callee->is_lib && (vm->flags & VM_FLAG_INLINE_BUILTINS) && inline_spec_ok(spec)) {
            VMType it;
            int grafted = inline_spec_body(vm, f, spec, args_begin, n_args, &it);
            if (grafted >= 0) { *out_t = it; return grafted; }
        }
        f->nodes[ir].ki   = spec->func_id;
        f->nodes[ir].type = spec->ret_type;
        *out_t   = f->nodes[ir].type;
        return ir;
    }

    // ---- Regular user-defined function: coerce to declared param types ----
    f->nodes[ir].ki   = callee->func_id;
    f->nodes[ir].type = callee->ret_type;
    if (coerce_call_args(vm, f, at, callee, args_begin, n_args) < 0) return -1;
    *out_t = f->nodes[ir].type;
    return ir;
}

// The declared type of a callee's rest parameter, or 0 when it was left bare
// (`...rest`) and the gather has to infer the element type from what was
// passed. A template has no syms yet, so its annotation is read back off the
// parameter AST -- compile_func_def_body already validated it.
static int rest_elem_type(VM *vm, Func *callee, int slot, VMType *out) {
    if (!callee->is_template) {
        *out = callee->syms[callee->param_slot[slot]].type;
        return 1;
    }
    ASTNode *p  = template_param_node(callee, slot);
    ASTNode *ty = p ? param_type_of(p) : 0;
    if (!ty) return 0;
    return parse_type(vm, ty, out, 1) && out->kind != VMT_VOID;
}

// Collect arg_ir[first .. n-1] into a hidden array local and leave a read of it
// in slot `first`, which is the rest parameter's.
//
// The array is a plain IR_ARR_LIT assigned to an anonymous sym -- exactly what
// `r = [a, b, c]` compiles to -- so the interpreter needs nothing new and the
// emitted C is one local array plus the `(slice){r.data, N}` literal the C
// backend already writes for an array argument in a slice slot.
//
// Two deliberate asymmetries:
//   - The NODE left behind keeps the array type, so coerce_call_args performs
//     the array-to-slice conversion at the boundary as it does for any other
//     array argument. The TYPE handed to the specialiser is the slice, with
//     len 0, so one specialisation serves every arity instead of one per call.
//   - Elements are converted to the DECLARED element type here when there is
//     one. coerce_call_args' array-to-slice path checks element kinds and does
//     not convert them, so without this `sum(1, 2)` could not reach a
//     `(...rest: []f64)`.
static int gather_rest_args(VM *vm, Func *f, ASTNode *at, Func *callee,
                            int *arg_ir, VMType *arg_types, int first, int n,
                            int **pre, int *n_pre, int *cap_pre) {
    int k = n - first;
    VMType want;
    int have_want = rest_elem_type(vm, callee, first, &want);
    if (have_want && (!is_slice(want.kind) || want.struct_id || want.pack_bits)) {
        vm_set_error_at(vm, at, "a ...rest parameter must be annotated []T, or left bare");
        return -1;
    }
    if (!have_want && k == 0) {
        vm_set_error_at(vm, at,
            "cannot tell what a ...rest parameter holds with no arguments to gather; "
            "annotate it, as (...rest: []i32)");
        return -1;
    }
    for (int i = first; i < n; i++)
        if (is_array(arg_types[i].kind) || is_slice(arg_types[i].kind)) {
            vm_errorf_at(vm, at, "argument %d lands in a ...rest parameter, which gathers numbers", i + 1);
            return -1;
        }

    // k == 0 still needs storage to point at: a zero-length C array is not a
    // thing, so one element is declared and a zero-length view of it passed.
    int n_el  = k > 0 ? k : 1;
    int elems = ir_alloc_items(vm, f, n_el);
    for (int i = 0; i < k; i++) f->child_indices[elems + i] = arg_ir[first + i];
    if (k == 0) {
        f->child_indices[elems] = ir_i32(vm, f, 0);
    }

    VTKind elem;
    int    eshift;
    if (have_want) {
        elem   = arr_elem(want.kind);
        eshift = ct_shift(want);
        VMType w = { VMT_VOID, 0, 0 };
        w.kind = elem;
        w = ct_set_shift(w, eshift);
        for (int i = 0; i < n_el; i++) {
            int    ei   = f->child_indices[elems + i];
            VMType have = f->nodes[ei].type;
            if (have.kind == elem && ct_shift(have) == eshift) continue;
            int cv = insert_fx_convert(vm, f, ei, have, w);
            f->child_indices[elems + i] = cv;
        }
    } else {
        elem = arg_types[first].kind;
        for (int i = 1; i < k; i++) elem = promote(vm, elem, arg_types[first + i].kind);
        eshift = unify_array_elems(vm, f, elems, n_el, elem);
    }

    int lit = ir_new(vm, f, IR_ARR_LIT);
    f->nodes[lit].items_begin    = elems;
    f->nodes[lit].n_items        = n_el;
    f->nodes[lit].type.kind      = arr_of(elem);
    f->nodes[lit].type.len       = n_el;
    f->nodes[lit].type.elem_shift = (elem == VMT_I32) ? eshift : 0;
    VMType lt = f->nodes[lit].type;

    VMSym *s = sym_add(vm, f, 0, ct_vmtype_clear_shift(lt));
    s->shift = ct_shift(lt);
    s->used  = 1;
    int slot = (int)(s - f->syms);
    // Both operands are built BEFORE the node that holds them: ir_new reallocs
    // f->nodes, so `f->nodes[asn].a = strb_local(...)` would be free to compute
    // the destination address first and then store it into the old buffer.
    int lhs  = strb_local(vm, f, slot);
    int asn  = ir_new(vm, f, IR_ASSIGN);
    f->nodes[asn].a = lhs;
    f->nodes[asn].b = lit;
    stmt_push_idx(vm, pre, n_pre, cap_pre, asn);

    VMType sl = { VMT_VOID, 0, 0 };
    sl.kind = slice_of(elem);
    sl = ct_set_shift(sl, eshift);

    int base = sym_read(vm, f, slot);
    if (k > 0) {
        arg_ir[first] = base;
    } else {
        int z0 = ir_i32(vm, f, 0);
        int z1 = ir_i32(vm, f, 0);
        VMType st;
        int sr = emit_slice_ir(vm, f, base, lt, z0, z1, &st);
        if (sr < 0) return -1;
        arg_ir[first] = sr;
    }
    arg_types[first] = sl;
    return 0;
}

static int compile_call(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    // ---- str(x[, decimals]) / slice(x, start, len) / print(...) /
    //      bitcast(T, x) ----
    // Compiler intrinsics rather than registered natives: str and slice *produce* a
    // slice, which the native ABI cannot return; print's C name would be emitted
    // verbatim by every backend, and no export target should have to provide one;
    // bitcast's first argument is a *type*, which must not be compiled as an
    // expression. Looked up only when the name resolves to nothing, so a script
    // that defines its own keeps it.
    if (is_ident(node->left) && !resolve_name(vm, f, node->left->token)) {
        int handled = 0;
        int ir = compile_str_intrinsic(vm, f, node, out_t, &handled);
        if (handled) return ir;
        ir = compile_print_intrinsic(vm, f, node, out_t, &handled);
        if (handled) return ir;
        ir = compile_bitcast(vm, f, node, out_t, &handled);
        if (handled) return ir;
        ir = compile_inspect_intrinsic(vm, f, node, out_t, &handled);
        if (handled) return ir;
        ir = compile_map_intrinsic(vm, f, node, out_t, &handled);
        if (handled) return ir;
        ir = compile_vec_ctor(vm, f, node, out_t, &handled);
        if (handled) return ir;
    }

    // ---- Type casts written as a call: i32(x), fx16(x), and the C spelling
    // (i32)(x) ----
    // Every type name parse_type understands works here, and the conversion
    // goes through insert_fx_convert -- the same path `x as i32` takes -- so a
    // fixed-point source is rescaled rather than handed over as its raw word.
    // Checked before the callee is resolved so fxN, which has no Func behind
    // it, is reached at all.
    {
        ASTNode *tn = node->left;
        if (is_paren(tn)) tn = paren_inner(tn);
        VMType target;
        if (cast_target_type(vm, tn, &target) && cast_name_is_free(vm, f, tn)) {
            ASTNode *argv[VM_MAX_PARAMS];
            int n_args = expand_call_args(vm, node, node->right, argv, VM_MAX_PARAMS);
            if (n_args < 0) return -1;
            if (n_args != 1) {
                vm_errorf_at(vm, node, "expected 1 arg, got %d", n_args);
                return -1;
            }
            VMType at;
            int ae = compile_expr(vm, f, paren_inner(argv[0]), &at);
            if (ae < 0) return -1;
            // Constant-fold i64(constant): just change the IR_CONST_I type.
            // Only a constant with no fixed-point shift: an fx one has to be
            // RESCALED, not retyped -- `i64(2.5 as fx16)` is 2, not the raw
            // 163840 -- and its shift would ride along onto an i64 node, where
            // type.len means something else entirely. insert_fx_convert below
            // folds the constant too, so nothing is lost by going that way.
            if (target.kind == VMT_I64 && f->nodes[ae].op == IR_CONST_I
                && ct_shift(at) == 0) {
                f->nodes[ae].type.kind = VMT_I64;
                *out_t = f->nodes[ae].type;
                return ae;
            }
            // An aggregate converts elementwise, like every other operation on
            // one; a scalar takes the ordinary path.
            if ((is_array(at.kind) || is_slice(at.kind)) && !vmt_is_struct(at)) {
                if (!(vm->flags & VM_FLAG_AUTO_VEC)) {
                    vm_errorf_at(vm, node, "elementwise operators are disabled here (#enable auto_vec)");
                    return -1;
                }
                return compile_vec_cast(vm, f, node, ae, at, target, out_t);
            }
            int cvt = insert_fx_convert(vm, f, ae, at, target);
            *out_t = target;
            return cvt;
        }
    }

    // node->left = callee (ident or inline arrow); node->right = paren args
    Func *callee = 0;
    if (is_ident(node->left)) {
        VMSym *s = resolve_name(vm, f, node->left->token);
        if (!s) {
#if VM_REACTIVE
            if (vm->rx_name_reported) return -1;
#endif
            char desc[128];
            node_describe(vm, node->left, desc, sizeof(desc));
            vm_errorf_at(vm, node->left, "unknown identifier %s", desc);
            return -1;
        }
#if VM_REACTIVE
        if (s->is_comp) return compile_component_call(vm, f, node, node->left->token, out_t);
#endif
        if (!s->is_func) {
            char desc[128];
            node_describe(vm, node->left, desc, sizeof(desc));
            vm_errorf_at(vm, node->left, "%s is not a function", desc);
            return -1;
        }
        callee = s->fn;
        // `#enable source_builtins`: this name has a registered C function AND a
        // definition in the language, and the source one was asked for. Checked
        // HERE rather than in resolve_name so that a scoped #enable / #disable
        // means what it looks like, and so one unit can hold both forms -- two
        // Funcs, two mangled C names, no collision.
        if (callee->native_tok != 0 && (vm->flags & VM_FLAG_SOURCE_BUILTINS)) {
            struct VMLibFunc *le = lib_find(vm, node->left->token);
            if (le) {
                Func *lf = lib_template(vm, le, node->left);
                if (!lf) return -1;
                callee = lf;
            }
        }
    } else {
        // Inline arrow function: ((x) => body)(args)  or  (=> body)(args)
        ASTNode *inner = node->left;
        if (is_paren(inner)) inner = paren_inner(inner);
        if (!inner || inner->token != TOK_ARROW_F) {
            char desc[128];
            node_describe(vm, node->left, desc, sizeof(desc));
            vm_errorf_at(vm, node->left, "trying to call %s (not symbol or func)", desc);
            return -1;
        }
        callee = compile_func_def(vm, 0, inner);
        if (!callee) return -1;
    }

    ASTNode *par = node->right;
    ASTNode *argv[VM_MAX_PARAMS];
    int n_ast = expand_call_args(vm, node, par, argv, VM_MAX_PARAMS);
    if (n_ast < 0) return -1;

    // Compile all args up-front to discover their types -- needed for templates
    // (choose/create a specialisation) and for native poly dispatch. They land in
    // locals rather than straight into the IR_CALL's item run because a `...`
    // spread only reveals how many slots it fills once its operand has been
    // compiled: the arity check and the item allocation both have to wait for it.
    // Omitted trailing args are filled from the callee's defaults below, so the
    // IR_CALL always carries the full arity and no later stage can tell a
    // defaulted arg -- or a spread one -- from one the caller wrote out.
    int     arg_ir[VM_MAX_PARAMS];
    VMType  arg_types[VM_MAX_PARAMS];
    // Statements that have to run before the call: a `{ ... }` argument fills a
    // hidden instance of the parameter's struct and passes that, a spread of an
    // impure expression is staged into a hidden local, and a variadic callee's
    // gathered arguments are built into one.
    int *pre = 0, n_pre = 0, cap_pre = 0;
    // Destination parameter slot. Not the loop counter once a spread has run,
    // which is why everything below indexes by `dst` rather than by `i`.
    int dst = 0;
    int n_spread = 0;
    for (int i = 0; i < n_ast; i++) {
        ASTNode *a = paren_inner(argv[i]);
        if (dst >= VM_MAX_PARAMS) {
            vm_errorf_at(vm, node, "too many arguments%s (max %d)",
                         n_spread ? " after spreading" : "", VM_MAX_PARAMS);
            return -1;
        }
        if (is_splat_node(a)) {
            int n = expand_splat(vm, f, a, arg_ir + dst, arg_types + dst,
                                 VM_MAX_PARAMS - dst, &pre, &n_pre, &cap_pre);
            if (n < 0) return -1;
            dst      += n;
            n_spread += n;
            continue;
        }
        VMType at;
        int ae;
        if (is_brace(a)) {
            VMType pt = {0};
            if (callee->native_tok == 0 && !callee->is_template && dst < callee->n_params)
                pt = callee->syms[callee->param_slot[dst]].type;
            if (!vmt_is_record(pt) || callee->structs != vm->structs) {
                vm_set_error_at(vm, a, "a { ... } argument needs a struct parameter to take its type from; "
                                       "annotate it, as (p: T)");
                return -1;
            }
            VMSym *hs = sym_add(vm, f, 0, pt);
            hs->used = 1;
            int hslot = (int)(hs - f->syms);
            // Omitted fields are cleared, not left over from the last call.
            if (struct_fill_slot(vm, f, hslot, pt.struct_id, a, 1, a, &pre, &n_pre, &cap_pre) < 0) return -1;
            ae = sym_read(vm, f, hslot);
            at = pt;
        } else {
            ae = compile_expr(vm, f, a, &at);
        }
        if (ae < 0 || reject_void(vm, a, at)) return -1;
        // A staged vector expression, IR_COMMA(t = ..., lit): the argument paths
        // evaluate a bare IR_ARR_LIT straight into the callee's slot, so the
        // staged assignments join the statements run ahead of the call.
        if (is_staged_arr_lit(f, ae)) {
            IRNode *cn = &f->nodes[ae];
            int last = cn->items_begin + cn->n_items - 1;
            for (int k = cn->items_begin; k < last; k++)
                stmt_push_idx(vm, &pre, &n_pre, &cap_pre, f->child_indices[k]);
            ae = f->child_indices[last];
        }
        arg_types[dst] = at;
        arg_ir[dst]    = ae;
        dst++;
    }
    int n_args  = dst;
    int n_slots = callee->n_params;

    if (callee->has_rest) {
        // Everything past the fixed parameters is gathered into one hidden array
        // local, which the rest parameter takes as an ordinary slice.
        int n_fixed = n_slots - 1;
        if (n_args < n_fixed) {
            vm_errorf_at(vm, node, "expected at least %d arg(s), got %d%s",
                         n_fixed, n_args, n_spread ? " after spreading" : "");
            return -1;
        }
        if (gather_rest_args(vm, f, node, callee, arg_ir, arg_types, n_fixed, n_args,
                             &pre, &n_pre, &cap_pre) < 0) return -1;
        n_args = n_slots;
    } else {
        int n_required = n_slots - callee->n_defaults;
        if (n_args < n_required || n_args > n_slots) {
            const char *how = n_spread ? " after spreading" : "";
            if (callee->n_defaults)
                vm_errorf_at(vm, node, "expected %d..%d arg(s), got %d%s", n_required, n_slots, n_args, how);
            else
                vm_errorf_at(vm, node, "expected %d arg(s), got %d%s", n_slots, n_args, how);
            return -1;
        }
    }

    int args_begin = n_slots > 0 ? ir_alloc_items(vm, f, n_slots) : 0;
    for (int i = 0; i < n_args; i++) f->child_indices[args_begin + i] = arg_ir[i];
    for (int i = n_args; i < n_slots; i++) {
        if (callee->native_tok != 0) {
            // Native built-ins carry no param-list AST (params_ast is 0), so the
            // generic param_default_of path can't serve them. Their supported
            // defaults are a literal 0 (register_c_func_defaults) and, per slot,
            // 1.0 (register_c_func_default_one). 0 is 0 at every shift, so it goes
            // out untyped; the 1.0 goes out TYPED as a Q16.16 word, so
            // insert_fx_convert rescales it instead of reinterpreting a raw 1.
            int want_one = (callee->native_default_one >> i) & 1u;
            int de = ir_new(vm, f, IR_CONST_I);
            f->nodes[de].type.kind = VMT_I32;
            f->nodes[de].type.len = want_one ? 16 : 0;
            f->nodes[de].type.elem_shift = 0;
            f->nodes[de].ki = want_one ? 65536 : 0;
            arg_types[i] = f->nodes[de].type;
            f->child_indices[args_begin + i] = de;
            continue;
        }
        ASTNode *d = param_default_of(callee, i);
        if (!d) {
            vm_errorf_at(vm, node, "missing argument %d", i + 1);
            return -1;
        }
        // Compiled into the caller, but it names consts where the `=>` was written.
        VMType dt;
        Func *saved_scope = vm->cur_scope;
        vm->cur_scope = callee->lex_parent;
        int de = compile_expr(vm, f, paren_inner(d), &dt);
        vm->cur_scope = saved_scope;
        if (de < 0) return -1;
        arg_types[i] = dt;
        f->child_indices[args_begin + i] = de;
    }
    n_args = n_slots;

    int ir;
    if (callee->native_tok != 0) {
        CFuncEntry *ce = &vm->run.cfunc_table[callee->native_tok - TOK_MATHS_FIRST];
        int any_agg = 0;
        for (int i = 0; i < n_args; i++)
            if (is_array(arg_types[i].kind) || is_slice(arg_types[i].kind)) { any_agg = 1; break; }
        // An aggregate argument to a numeric native: unroll it into one call per
        // lane when that is safe, rather than promoting it to a scalar kind and
        // reading it as one.
        //
        // Safe means pure and signature-free. C11 6.7.9p23 leaves the elements of
        // an initializer list indeterminately sequenced, so N calls inside one
        // would have unspecified order -- fine for a pure function, a silent trap
        // for a host native that draws something. A has_sig native
        // (register_c_func_sig) takes its slices deliberately and must keep them
        // whole.
        int vec = 0;
        if (any_agg && !ce->has_sig) {
            if (ce->is_pure && (vm->flags & VM_FLAG_AUTO_VEC)) vec = 1;
            else for (int i = 0; i < n_args; i++)
                if (is_array(arg_types[i].kind) || is_slice(arg_types[i].kind)) {
                    const char *nm = intern_get_cstr(vm->intern, callee->name);
                    if (ce->is_pure)
                        vm_errorf_at(vm, node, "elementwise operators are disabled here (#enable auto_vec)");
                    else
                        vm_errorf_at(vm, node, "argument %d of '%s' is an array; '%s' takes numbers, call it per element", i + 1, nm, nm);
                    return -1;
                }
        }
        ir = vec ? compile_vec_call(vm, f, node, callee, args_begin, arg_types, n_args, out_t)
                 : compile_native_call_ir(vm, f, node, callee, args_begin, arg_types, n_args, out_t);
    } else {
        ir = compile_user_call_ir(vm, f, node, callee, args_begin, arg_types, n_args, out_t);
    }
    if (ir < 0) return -1;

    // The staged statements have to run before the call reads what they wrote.
    // This used to sit on the user-function path alone, where a `{ ... }`
    // argument was the one thing that produced any; a spread base and a variadic
    // gather both produce them for a native too.
    return inline_comma(vm, f, ir, *out_t, pre, n_pre);
}

// A literal's spreads are prepared before its element run is allocated, so they
// need somewhere to sit. Sixteen is far past anything sane and keeps this off
// the arena.
#define ARR_LIT_MAX_SPREADS 16

static int compile_array_literal(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    int ok = 1;
    int cols = arr_lit_row_width(vm, node, &ok);
    if (!ok) return -1;
    int rows = (int)node->items.size;

    // `[1, ...x, 4]`: how long the literal is only becomes known once every
    // spread's operand has been compiled and typed, so that happens first and
    // the element run is allocated after. The lanes themselves are still
    // emitted in written order below -- what moves is only the evaluation of
    // the spread operands, which for anything impure is a staged assignment
    // that has to run ahead of the literal anyway.
    //
    // Flat literals only. A spread row would have to be a slice of a slice,
    // which the nested representation (one flat block plus inner_len) has no
    // room for.
    SplatSrc sp[ARR_LIT_MAX_SPREADS];
    int      sp_item[ARR_LIT_MAX_SPREADS];
    int      n_sp = 0;
    int *pre = 0, n_pre = 0, cap_pre = 0;

    // `p = [a, b]`, where a and b are arrays: a nested literal whose rows are
    // written as expressions rather than as `[..]` lists. Nothing in the AST
    // tells those apart from scalars, so the first element is compiled here,
    // ahead of everything else, and its type decides the literal's shape --
    // the same job a leading `[..]` row does above. The compiled node is kept
    // and reused as lane 0 below, so it is never compiled twice.
    SplatSrc probe;
    int have_probe = 0;
    if (!cols && rows) {
        ASTNode *first = paren_inner(*(ASTNode**)array_get(&node->items, 0));
        if (!is_splat_node(first) && !is_bracket(first)) {
            if (arr_row_prepare(vm, f, first, &probe, &pre, &n_pre, &cap_pre) < 0) return -1;
            have_probe = 1;
            cols = probe.n;   // 0 = a single value, so still a flat literal
        }
    }

    int n = cols ? rows * cols : rows;
    if (!cols) {
        n = 0;
        for (int r = 0; r < rows; r++) {
            ASTNode *e = paren_inner(*(ASTNode**)array_get(&node->items, r));
            if (!is_splat_node(e)) { n++; continue; }
            if (n_sp == ARR_LIT_MAX_SPREADS) {
                vm_errorf_at(vm, e, "at most %d spreads in one array literal", ARR_LIT_MAX_SPREADS);
                return -1;
            }
            int k = splat_prepare(vm, f, e, &sp[n_sp], &pre, &n_pre, &cap_pre);
            if (k < 0) return -1;
            sp_item[n_sp++] = r;
            n += k;
        }
    }

    int ir = ir_new(vm, f, IR_ARR_LIT);
    f->nodes[ir].items_begin = ir_alloc_items(vm, f, n);
    f->nodes[ir].n_items = n;
    VTKind elem = VMT_I32;
    int at = 0;
    int si = 0;
    for (int r = 0; r < rows; r++) {
        ASTNode *row = paren_inner(*(ASTNode**)array_get(&node->items, r));
        if (si < n_sp && sp_item[si] == r) {
            for (int k = 0; k < sp[si].n; k++) {
                VMType et;
                int ee = splat_lane(vm, f, row, &sp[si], k, &et);
                if (ee < 0) return -1;
                f->child_indices[f->nodes[ir].items_begin + at] = ee;
                if (at == 0) elem = et.kind;
                else elem = promote(vm, elem, et.kind);
                at++;
            }
            si++;
            continue;
        }
        // A row written as an expression. Its lanes are constant-subscript
        // indexes into the compiled row -- the same thing a spread emits, and
        // the same thing a written-out row stores below, so the flat block
        // comes out identical either way.
        if (cols && !is_bracket(row)) {
            SplatSrc s;
            if (r == 0 && have_probe) {
                s = probe;
            } else if (arr_row_prepare(vm, f, row, &s, &pre, &n_pre, &cap_pre) < 0) {
                return -1;
            }
            if (s.n != cols) {
                if (s.n == 0)
                    vm_errorf_at(vm, row, "every element of this array literal must be a row of %d, "
                                          "this one is a single value", cols);
                else
                    vm_errorf_at(vm, row, "nested array rows must all be %d long, this one is %d",
                                 cols, s.n);
                return -1;
            }
            for (int c = 0; c < cols; c++) {
                VMType et;
                int ee = row_lane(vm, f, row, &s, c, &et);
                if (ee < 0) return -1;
                f->child_indices[f->nodes[ir].items_begin + at] = ee;
                if (at == 0) elem = et.kind;
                else elem = promote(vm, elem, et.kind);
                at++;
            }
            continue;
        }
        for (int c = 0; c < (cols ? cols : 1); c++) {
            ASTNode *e = cols ? paren_inner(*(ASTNode**)array_get(&row->items, c)) : row;
            if (is_splat_node(e)) {
                vm_set_error_at(vm, e, "cannot spread into a row of a nested array literal");
                return -1;
            }
            if (is_bracket(e)) {
                vm_set_error_at(vm, e, "arrays nest at most two levels deep");
                return -1;
            }
            VMType et;
            int ee;
            if (r == 0 && c == 0 && have_probe) { ee = probe.base; et = probe.bt; }
            else if ((ee = compile_expr(vm, f, e, &et)) < 0) return -1;
            // An aggregate in a lane would be cut down to a scalar by the
            // unify below -- silently, and to nothing useful. It is either a
            // row in a literal whose shape says otherwise, or a level too deep.
            if (vmt_is_struct(et) || is_array(et.kind) || is_slice(et.kind)) {
                if (cols)
                    vm_set_error_at(vm, e, "arrays nest at most two levels deep");
                else
                    vm_set_error_at(vm, e, "this element is an array but the first one is a single "
                                           "value; make every element a row -- [[..], [..]] -- or "
                                           "spread it with ...x");
                return -1;
            }
            f->child_indices[f->nodes[ir].items_begin + at] = ee;
            if (at == 0) elem = et.kind;
            else elem = promote(vm, elem, et.kind);
            at++;
        }
    }
    // Cast all elements to the common type (kind + fixed-point shift).
    int eshift = unify_array_elems(vm, f, f->nodes[ir].items_begin, n, elem);
    f->nodes[ir].type.kind = arr_of(elem);
    f->nodes[ir].type.len = n;
    f->nodes[ir].type.elem_shift = eshift;
    f->nodes[ir].type.inner_len = cols;
    *out_t = f->nodes[ir].type;
    // A staged spread base leaves IR_COMMA(t = ..., lit) -- the shape
    // is_staged_arr_lit names, which assignment and `return` unwrap and the
    // positions that special-case a bare literal refuse.
    return inline_comma(vm, f, ir, f->nodes[ir].type, pre, n_pre);
}

// Re-materialise an IR_DATA_SLICE's storage as i32 elements.
//
// A string literal's natural type is []u8 (see compile_string_literal), which is
// what a byte-taking native or a []u8 parameter wants. A slot that still wants one
// i32 per character gets a widened copy instead -- a compile-time choice with no
// runtime conversion, since the wanted width is known at the call site.
//
// Only ever fires on a literal sitting *directly* in an argument position: once a
// literal is bound to a variable it has a type, and that type is []u8.
//
// Returns 1 if the node was converted (or already was i32), 0 if it is not a data
// slice.
static int dataslice_to_i32(VM *vm, Func *f, int idx) {
    if (idx < 0 || f->nodes[idx].op != IR_DATA_SLICE) return 0;
    if (f->nodes[idx].type.pack_bits == 0) return 1;
    int n = f->nodes[idx].n_items;
    const unsigned char *src = (const unsigned char*)(intptr_t)f->nodes[idx].ki;
    int *data = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (n > 0 ? n : 1));
    for (int i = 0; i < n; i++) data[i] = src[i];
    f->nodes[idx].ki = (long long)(intptr_t)data;
    f->nodes[idx].type.pack_bits = 0;
    f->nodes[idx].sub_op = 0;
    return 1;
}

// Value of one hex digit, -1 if `c` is not one.
static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode the backslash escapes in one literal fragment. The parser stores a
// fragment's source bytes verbatim -- unparse writes ->string straight back out
// between the quote characters and that round-trip has to be byte exact -- so the
// decoding happens here instead, once, on the way into the data slice.
//
// Recognised: \a \b \e \f \n \r \t \v \0 \\ \' \" and \xHH (one or two
// hex digits). Anything else keeps its backslash, so a lone `\` that was never
// meant as an escape still survives intact.
//
// Writes to `dst` when it is non-NULL and returns the decoded byte count either
// way, so the measure pass and the copy pass cannot disagree.
static int str_unescape(const char *s, unsigned char *dst) {
    int n = 0;
    for (int i = 0; s[i]; ) {
        unsigned char c = (unsigned char)s[i++];
        // A trailing lone backslash has nothing to escape: keep it as data.
        if (c != '\\' || !s[i]) { if (dst) dst[n] = c; n++; continue; }
        unsigned char e = (unsigned char)s[i++];
        int v;
        switch (e) {
            case 'a': v = 0x07; break;
            case 'b': v = 0x08; break;
            case 'e': v = 0x1b; break;   // ESC -- not standard C, handy for textmode
            case 'f': v = 0x0c; break;
            case 'n': v = 0x0a; break;
            case 'r': v = 0x0d; break;
            case 't': v = 0x09; break;
            case 'v': v = 0x0b; break;
            case '0': v = 0x00; break;
            case '\\': case '\'': case '"': v = e; break;
            case 'x': {
                int h = 0, nd = 0;
                while (nd < 2 && hex_val((unsigned char)s[i]) >= 0)
                    { h = h * 16 + hex_val((unsigned char)s[i]); i++; nd++; }
                if (!nd) {  // `\x` with no digits after it is not an escape
                    if (dst) { dst[n] = '\\'; dst[n + 1] = 'x'; }
                    n += 2; continue;
                }
                v = h & 0xff;
                break;
            }
            default:
                if (dst) { dst[n] = '\\'; dst[n + 1] = e; }
                n += 2; continue;
        }
        if (dst) dst[n] = (unsigned char)v;
        n++;
    }
    return n;
}

// Total decoded byte length of a literal (or a chain of adjacent ones), -1 if a
// fragment's content id does not resolve.
static int str_chain_len(VM *vm, ASTNode *n) {
    n = paren_inner(n);
    if (is_string_literal(n)) {
        const char *s = intern_get_cstr(vm->intern, n->string);
        if (!s) return -1;
        return str_unescape(s, 0);
    }
    int a = str_chain_len(vm, n->left);
    int b = str_chain_len(vm, unwrap_chain(n->right));
    if (a < 0 || b < 0) return -1;
    return a + b;
}

// Append the chain's fragments to `dst` in source order. Sized by
// str_chain_len, which walks the same nodes in the same order.
static void str_chain_copy(VM *vm, ASTNode *n, unsigned char *dst, int *at) {
    n = paren_inner(n);
    if (is_string_literal(n)) {
        const char *s = intern_get_cstr(vm->intern, n->string);
        *at += str_unescape(s, dst + *at);
        return;
    }
    str_chain_copy(vm, n->left, dst, at);
    str_chain_copy(vm, unwrap_chain(n->right), dst, at);
}

// Compile a string literal ("hello" / 'hello') -- or a run of adjacent ones, which
// concatenate (see is_string_concat) -- into an IR_DATA_SLICE of bytes. The decoded
// content lives in the parser's intern table, which the host frees with the
// ParseResult right after compile, so it is *copied* into the VM's memory backend.
// One byte per element (no UTF-8 decoding); escapes are decoded by str_unescape.
// Not NUL-terminated: .len is the only terminator.
//
// The result type is []u8 -- VMT_SLICE_I32 with pack_bits 8, since packing is
// storage and consumes no VTKind. A slot wanting i32 elements gets a widened copy
// via dataslice_to_i32 above.
static int compile_string_literal(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    int n = str_chain_len(vm, node);
    if (n < 0) { vm_set_error_at(vm, node, "bad string literal"); return -1; }
    // Borrow VM-lifetime storage for the bytes. A zero-length string yields a
    // valid empty slice (ptr non-NULL, len 0).
    unsigned char *data = (unsigned char*)mem_alloc(&vm->run.mem, (size_t)(n > 0 ? n : 1));
    int at = 0;
    str_chain_copy(vm, node, data, &at);
    int ir = ir_new(vm, f, IR_DATA_SLICE);
    f->nodes[ir].ki        = (long long)(intptr_t)data;
    f->nodes[ir].n_items   = n;
    f->nodes[ir].sub_op    = 8;
    f->nodes[ir].type.kind = VMT_SLICE_I32;
    f->nodes[ir].type.len  = 0;
    f->nodes[ir].type.pack_bits = 8;
    *out_t = f->nodes[ir].type;
#if VM_REACTIVE
    rx_note_lit(vm, f, ir, node);
#endif
    return ir;
}

// ---------- string building (str(), `~` concatenation, slice()) ----------
//
// There is no allocator in the VM: `mem_alloc` is the compile-time arena and a
// frame is a buffer the caller owns. So string building follows zig's
// bufPrint rather than rust's format! -- the bytes go into a buffer and the
// value is a subslice of it -- with the one twist that the compiler supplies
// the buffer instead of the script:
//
//     "score: " ~ n          compiles to
//     (t = 0,
//      t = fmt_bytes(__s, cap, t, "score: "),
//      t = fmt_i32  (__s, cap, t, n),
//      slice(__s, 0, t))
//
// where `__s` is a hidden [cap]u8 frame local and `t` a hidden i32. `cap` is
// the sum of each part's worst-case width, so overflow is impossible for a
// build made only of literals and numbers; a part whose length is only known
// at run time (another byte slice) contributes STRB_CAP_DYNAMIC and can
// truncate, which is the single failure mode and needs no error path.
//
// So the result lives as long as the frame and must not be stored in a shared
// variable (rejected in compile_assign), and `s = s ~ "x"` in a loop cannot work:
// the capacity is fixed at compile time. Write an explicit buffer + cursor and
// slice() it instead.
//
// A left-nested chain (`a ~ b ~ c`) reopens the IR_COMMA the left operand
// already built instead of copying into a second buffer, so a chain of N
// parts costs one buffer and N appends, not N buffers and O(N^2) copies.

#define STRB_MARK          1     // IR_COMMA sub_op: "this is a string build"
#define STRB_CAP_INT      12     // "-2147483648"
#define STRB_CAP_I64      21
#define STRB_CAP_FLOAT    28     // sign + 19 integer digits + '.' + 9 decimals
#define STRB_CAP_DYNAMIC  32     // a byte slice whose length isn't known yet
#define STRB_CAP_MIN       8
#define STRB_CAP_MAX    1024
#define STRB_DEFAULT_DECIMALS 3

// True for the VM's string type: a byte-element ([]u8) slice or fixed array.
// A string literal is one, and so is every value str()/`+` produces.
static int is_byte_string(VMType t) {
    return (is_slice(t.kind) || is_array(t.kind))
        && arr_elem(t.kind) == VMT_I32 && t.pack_bits == 8 && !t.struct_id;
}

// The []u8 slice type every string-valued expression carries.
static VMType byte_slice_type(void) {
    VMType t = { VMT_SLICE_I32, 0 };
    t.pack_bits = 8;
    return t;
}

// Defined below; strb_part needs it to splice a nested build.
static int is_strbuild(Func *f, int ir);

typedef struct {
    int  tslot;        // hidden [cap]u8 buffer sym
    int  cslot;        // hidden i32 cursor sym
    int  rslot;        // hidden []u8 result sym (-1 until strb_finish makes one)
    int *items;        // the IR_COMMA's leading statement items
    int  n_items, cap_items;
    int  cap;          // running worst-case byte count
    int  slice_idx;    // IR_SLICE node to reuse when reopening, else -1
} StrBuild;

// An IR_LOCAL read of a slot, typed from the symbol (which is what the
// interpreter reads anyway -- see do_store_local/eval IR_LOCAL).
static int strb_local(VM *vm, Func *f, int slot) {
    int lv = ir_new(vm, f, IR_LOCAL);
    f->nodes[lv].type = f->syms[slot].type;
    f->nodes[lv].ki   = slot;
    return lv;
}

// Start a fresh build: allocate the buffer and cursor symbols and emit
// `cursor = 0`. The buffer's length is patched to the final capacity by
// strb_finish, so nothing here needs to know how many parts follow.
static void strb_begin(VM *vm, Func *f, StrBuild *b) {
    VMType bt = { VMT_VOID, 0 };
    bt.kind = arr_of(VMT_I32); bt.len = STRB_CAP_MIN; bt.pack_bits = 8;
    VMSym *t = sym_add(vm, f, 0, bt);
    t->shift = 0; t->used = 1;
    b->tslot = (int)(t - f->syms);

    VMType ct = { VMT_I32, 0 };
    VMSym *c = sym_add(vm, f, 0, ct);
    c->shift = 0; c->used = 1;
    b->cslot = (int)(c - f->syms);

    b->items = 0; b->n_items = 0; b->cap_items = 0;
    b->cap = 0; b->slice_idx = -1; b->rslot = -1;

    int zero = ir_i32(vm, f, 0);
    // Take every child index into a local *before* the store. ir_new can
    // reallocate f->nodes, and in `f->nodes[x].a = ir_new(...)` C leaves the
    // compiler free to compute the destination address first and then write it
    // through the freed array -- which is exactly what gcc does. Same rule for
    // f->child_indices and ir_alloc_items throughout this file.
    int cur = strb_local(vm, f, b->cslot);
    int asn = ir_new(vm, f, IR_ASSIGN);
    f->nodes[asn].a = cur;
    f->nodes[asn].b = zero;
    stmt_push_idx(vm, &b->items, &b->n_items, &b->cap_items, asn);
}

// Reopen the build a nested `a ~ b` already produced, so `(a ~ b) ~ c` appends
// to the same buffer. The shape strb_finish builds is fixed, which is what
// makes the hidden slots recoverable: N `cursor = IR_FMT(...)` assignments,
// then `result = IR_SLICE(buffer, 0, cursor)`, then a read of `result`.
static void strb_reopen(VM *vm, Func *f, int comma, StrBuild *b) {
    int base  = f->nodes[comma].items_begin;
    int nkeep = f->nodes[comma].n_items - 2;          // drop the store and the read
    int store = f->child_indices[base + nkeep];
    int sl    = f->nodes[store].b;
    b->slice_idx = sl;
    b->rslot = (int)f->nodes[f->nodes[store].a].ki;
    b->tslot = (int)f->nodes[f->child_indices[f->nodes[sl].items_begin + 0]].ki;
    b->cslot = (int)f->nodes[f->child_indices[f->nodes[sl].items_begin + 2]].ki;
    b->items = 0; b->n_items = 0; b->cap_items = 0;
    b->cap   = f->syms[b->tslot].type.len;
    for (int i = 0; i < nkeep; i++)
        stmt_push_idx(vm, &b->items, &b->n_items, &b->cap_items, f->child_indices[base + i]);
}

// Append one part: `cursor = IR_FMT(kind, buffer, cursor, value)`.
// `decimals` is only read by the fixed-point and float kinds.
static int strb_part(VM *vm, Func *f, StrBuild *b, ASTNode *at,
                     int ir, VMType t, int decimals) {
    int kind, extra = 0, width;
    // A part that is itself a string build -- `"fx=" + str(x, 3)` -- gets its
    // statements spliced in ahead of ours, and contributes the read of its
    // result local as the value. Left nested, it would be an IR_COMMA sitting
    // where a plain operand is expected, and the emitted (ptr, len) split
    // would run the inner build twice with unsequenced writes to its cursor.
    if (is_strbuild(f, ir)) {
        int base = f->nodes[ir].items_begin, n = f->nodes[ir].n_items;
        for (int i = 0; i < n - 1; i++)
            stmt_push_idx(vm, &b->items, &b->n_items, &b->cap_items, f->child_indices[base + i]);
        ir = f->child_indices[base + n - 1];
        t  = f->nodes[ir].type;
    }
    if (vmt_is_struct(t)) {
        char desc[128];
        node_describe(vm, at, desc, sizeof(desc));
        vm_errorf_at(vm, at, "%s is a struct; name a field, as p.x", desc);
        return -1;
    }
    if (is_byte_string(t)) {
        kind = FMT_BYTES;
        // A literal or a fixed array has a length the compiler knows; a real
        // slice does not, and gets a fixed budget it may truncate against.
        width = is_array(t.kind) ? t.len
              : (f->nodes[ir].op == IR_DATA_SLICE ? f->nodes[ir].n_items
                                                  : STRB_CAP_DYNAMIC);
    } else if (t.kind == VMT_I32) {
        int shift = ct_shift(t);
        if (shift > 0) {
            kind = FMT_FX; extra = shift | (decimals << 8); width = STRB_CAP_FLOAT;
        } else {
            kind = FMT_I32; width = STRB_CAP_INT;
        }
    } else if (t.kind == VMT_I64) {
        kind = FMT_I64; width = STRB_CAP_I64;
    } else if (t.kind == VMT_F32 || t.kind == VMT_F64) {
        // One float path: f32 widens so the emitted C and the interpreter
        // agree on a single helper.
        if (t.kind == VMT_F32) ir = insert_cvt(vm, f, ir, t, VMT_F64);
        kind = FMT_F64; extra = decimals; width = STRB_CAP_FLOAT;
    } else {
        vm_set_error_at(vm, at, "cannot convert this value to a string");
        return -1;
    }

    int dst = strb_local(vm, f, b->tslot);
    int cur = strb_local(vm, f, b->cslot);
    int items_begin = ir_alloc_items(vm, f, 3);
    f->child_indices[items_begin + 0] = dst;
    f->child_indices[items_begin + 1] = cur;
    f->child_indices[items_begin + 2] = ir;
    int fmt = ir_new(vm, f, IR_FMT);
    f->nodes[fmt].sub_op      = kind;
    f->nodes[fmt].ki          = extra;
    f->nodes[fmt].items_begin = items_begin;
    f->nodes[fmt].n_items     = 3;
    f->nodes[fmt].type.kind   = VMT_I32;

    int lhs = strb_local(vm, f, b->cslot);
    int asn = ir_new(vm, f, IR_ASSIGN);
    f->nodes[asn].a = lhs;
    f->nodes[asn].b = fmt;
    stmt_push_idx(vm, &b->items, &b->n_items, &b->cap_items, asn);

    b->cap += width;
    return 0;
}

// Close the build: size the buffer, retype every node that names it, and wrap
// everything in an IR_COMMA yielding slice(buffer, 0, cursor).
static int strb_finish(VM *vm, Func *f, StrBuild *b, VMType *out_t) {
    int cap = b->cap < STRB_CAP_MIN ? STRB_CAP_MIN
            : (b->cap > STRB_CAP_MAX ? STRB_CAP_MAX : b->cap);
    f->syms[b->tslot].type.len = cap;
    // Every IR_LOCAL naming the buffer copied the symbol's type when it was
    // built, and the emitter reads an array's length off the node (a fixed
    // array's length is compile-time data), so they all have to be refreshed
    // now that the final capacity is known.
    for (int i = 0; i < f->n_nodes; i++)
        if (f->nodes[i].op == IR_LOCAL && (int)f->nodes[i].ki == b->tslot)
            f->nodes[i].type = f->syms[b->tslot].type;

    int zero = ir_i32(vm, f, 0);
    int sbuf = strb_local(vm, f, b->tslot);
    int scur = strb_local(vm, f, b->cslot);
    int items_begin = ir_alloc_items(vm, f, 3);
    f->child_indices[items_begin + 0] = sbuf;
    f->child_indices[items_begin + 1] = zero;
    f->child_indices[items_begin + 2] = scur;
    // Reuse the node a reopened build already had, so the abandoned one
    // doesn't linger with a stale (smaller) buffer type for the emitter's
    // aggregate-type scan to trip over.
    int sl = b->slice_idx >= 0 ? b->slice_idx : ir_new(vm, f, IR_SLICE);
    f->nodes[sl].op          = IR_SLICE;
    f->nodes[sl].sub_op      = 1;             // 0..cursor is in range by construction
    f->nodes[sl].ki          = 8;             // byte elements
    f->nodes[sl].items_begin = items_begin;
    f->nodes[sl].n_items     = 3;
    f->nodes[sl].type        = byte_slice_type();

    // The comma's *value* is a read of a third hidden local holding the
    // finished slice, not the IR_SLICE itself. That gives every consumer a
    // plain lvalue to name: emitted C has to split a slice argument into a
    // (ptr, len) pair, and C leaves argument evaluation order unspecified, so
    // a value that has to be computed twice would run the whole build twice.
    // See emit_native_call's prologue handling in vm_emit_c.c.
    int rslot = b->rslot;
    if (rslot < 0) {
        VMSym *rs = sym_add(vm, f, 0, byte_slice_type());
        rs->shift = 0; rs->used = 1;
        rslot = (int)(rs - f->syms);
    }
    int rlhs  = strb_local(vm, f, rslot);
    int store = ir_new(vm, f, IR_ASSIGN);
    f->nodes[store].a = rlhs;
    f->nodes[store].b = sl;
    int rread = strb_local(vm, f, rslot);

    int comma_items = ir_alloc_items(vm, f, b->n_items + 2);
    for (int i = 0; i < b->n_items; i++) f->child_indices[comma_items + i] = b->items[i];
    f->child_indices[comma_items + b->n_items]     = store;
    f->child_indices[comma_items + b->n_items + 1] = rread;

    int comma = ir_new(vm, f, IR_COMMA);
    f->nodes[comma].sub_op      = STRB_MARK;
    f->nodes[comma].items_begin = comma_items;
    f->nodes[comma].n_items     = b->n_items + 2;
    f->nodes[comma].type        = byte_slice_type();
    *out_t = f->nodes[comma].type;
    return comma;
}

// Can `ir` be named more than once in emitted C without repeating work or a
// side effect? Constants and plain local reads can; everything else is staged
// into a temp by stage_operand below.
//
// An inspection wrapper is looked through: it is type-transparent AND effect-
// transparent, so `__ins(xs, 0)` is exactly as pure as `xs`. Staging it instead
// would change what the program does -- `for x in xs` indexes a fixed array where
// it stands, so a write inside the body is seen by the later passes, and a hidden
// copy of it would not see them.
static int ir_is_pure_operand(Func *f, int ir) {
    while (f->nodes[ir].op == IR_INSPECT && f->nodes[ir].a >= 0) ir = f->nodes[ir].a;
    int op = f->nodes[ir].op;
    return op == IR_CONST_I || op == IR_CONST_F || op == IR_LOCAL || op == IR_DATA_SLICE;
}

// Bind `ir` to a hidden local (pushing the assignment onto `items`) and return
// a read of it. Returns `ir` unchanged when there is nothing to gain.
static int stage_operand(VM *vm, Func *f, int ir, int **items, int *n, int *cap) {
    if (ir < 0 || ir_is_pure_operand(f, ir)) return ir;
    VMType t = f->nodes[ir].type;
    VMSym *s = sym_add(vm, f, 0, ct_vmtype_clear_shift(t));
    s->shift = ct_shift(t);
    s->used  = 1;
    int slot = (int)(s - f->syms);
    int lhs  = strb_local(vm, f, slot);
    int asn  = ir_new(vm, f, IR_ASSIGN);
    f->nodes[asn].a = lhs;
    f->nodes[asn].b = ir;
    stmt_push_idx(vm, items, n, cap, asn);
    // sym_read, not strb_local: the read must carry the fixed-point shift, or a
    // staged fx operand is used as a raw int (`v / length(v)` divided by 2^16).
    return sym_read(vm, f, slot);
}

// A vector expression whose operands needed staging: IR_COMMA(t = ..., lit).
// Assignment and `return` unwrap it (vm_run.c arr_lit_unwrap, and emitted C
// gets a sequenced comma expression for free) and a call argument hoists the
// assignments ahead of the call; a ternary arm does neither, and says so.
static int is_staged_arr_lit(Func *f, int ir) {
    if (ir < 0 || f->nodes[ir].op != IR_COMMA || f->nodes[ir].n_items <= 0) return 0;
    int last = f->child_indices[f->nodes[ir].items_begin + f->nodes[ir].n_items - 1];
    return f->nodes[last].op == IR_ARR_LIT;
}

// A previously compiled node that is a string build this file produced, and so
// can be appended to rather than copied.
static int is_strbuild(Func *f, int ir) {
    return ir >= 0 && f->nodes[ir].op == IR_COMMA && f->nodes[ir].sub_op == STRB_MARK;
}

// Does this slice's storage live in the current frame? A local fixed array
// does; a string literal (VM arena), a shared variable and a slice parameter
// (the caller's memory) do not.
static int slice_base_is_frame(Func *f, int ir) {
    if (ir < 0) return 0;
    if (f->nodes[ir].op == IR_SLICE)
        return slice_base_is_frame(f, f->child_indices[f->nodes[ir].items_begin + 0]);
    if (f->nodes[ir].op == IR_LOCAL) {
        VMSym *s = &f->syms[(int)f->nodes[ir].ki];
        return is_array(s->type.kind) && !s->is_global;
    }
    return 0;
}

// True when `ir` obviously evaluates to a *slice* over this frame's own storage: a
// string build, or a slice() of a local array. Used to reject `return` of such a
// value. Returning a fixed array is fine -- an array is a value type here and is
// copied into the return slot. A slice stored in a variable first is not tracked;
// the interpreter's own run-time check backstops it.
static int slice_escapes_frame(Func *f, int ir) {
    if (ir < 0) return 0;
    if (is_strbuild(f, ir)) return 1;
    if (f->nodes[ir].op == IR_SLICE)
        return slice_base_is_frame(f, f->child_indices[f->nodes[ir].items_begin + 0]);
    return 0;
}

// `a .. b`. Either side may be a string or a number; a number is formatted
// with str()'s default rules, so `1 .. 2` is "12", exactly as in Lua. An
// operand that is neither (an array, a function) is rejected by strb_part.
static int compile_str_concat(VM *vm, Func *f, ASTNode *node,
                              int l, VMType lt, int r, VMType rt, VMType *out_t);

static int compile_concat_op(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    VMType lt, rt;
    int l = compile_expr(vm, f, node->left, &lt);
    if (l < 0) return -1;
    int r = compile_expr(vm, f, node->right, &rt);
    if (r < 0) return -1;
    return compile_str_concat(vm, f, node, l, lt, r, rt, out_t);
}

static int compile_str_concat(VM *vm, Func *f, ASTNode *node,
                              int l, VMType lt, int r, VMType rt, VMType *out_t) {
    StrBuild b;
    if (is_strbuild(f, l)) {
        strb_reopen(vm, f, l, &b);
    } else {
        strb_begin(vm, f, &b);
        if (strb_part(vm, f, &b, node->left, l, lt, STRB_DEFAULT_DECIMALS) < 0) return -1;
    }
    if (strb_part(vm, f, &b, node->right, r, rt, STRB_DEFAULT_DECIMALS) < 0) return -1;
    return strb_finish(vm, f, &b, out_t);
}

// Argument count of a call's paren node, with the same empty-`()` correction
// compile_call applies (an empty argument list arrives as one blank item).
static int call_argc(ASTNode *par) {
    int n = (int)par->items.size;
    if (n == 1) {
        ASTNode *only = *(ASTNode**)array_get(&par->items, 0);
        if (!only->left && !only->right && !only->number_flags && only->token == 0
            && only->string == 0 && only->items.size == 0 && only->left_bracket == 0)
            return 0;
    }
    return n;
}

static ASTNode *call_arg(ASTNode *par, int i) {
    return paren_inner(*(ASTNode**)array_get(&par->items, i));
}

// A non-negative integer literal argument, or -1.
static int call_arg_const_int(ASTNode *a) {
    if (!is_number(a) || (a->number_flags & NUM_TYPE) != NUM_INTEGER) return -1;
    if (a->number < 0 || a->number > 9) return -1;
    return (int)a->number;
}

// The operand checks shared by `slice(x, start, len)` and `x[start..end]`.
// `what` names the spelling the user actually wrote, so the message points at
// the form in front of them.
static int check_sliceable(VM *vm, Func *f, ASTNode *at, int base, VMType bt, const char *what) {
    if (!is_array(bt.kind) && !is_slice(bt.kind)) {
        vm_errorf_at(vm, at, "%s needs an array or slice", what);
        return 0;
    }
    if (vmt_is_record(bt) || vmt_is_ref(bt)) {
        vm_errorf_at(vm, at, "%s of a struct: name a field, as p.x", what);
        return 0;
    }
    if (vmt_is_struct(bt)) {
        vm_errorf_at(vm, at, "slicing an array of structs is not supported yet");
        return 0;
    }
    if (bt.pack_bits && bt.pack_bits < 8) {
        vm_errorf_at(vm, at, "cannot slice a u%d array: its elements have no byte address",
                     bt.pack_bits);
        return 0;
    }
    if (f->nodes[base].op == IR_ARR_LIT) {
        vm_errorf_at(vm, at, "%s of an array literal; assign it to a variable first", what);
        return 0;
    }
    return 1;
}

// The IR_SLICE itself, given operands that are already compiled and already
// raw i32. Split out so the call form and the subscript form share the staging
// rule and the result type, which are the parts that are easy to get subtly
// different.
static int emit_slice_ir(VM *vm, Func *f, int base, VMType bt, int start, int len, VMType *out_t) {
    // Constant bounds inside a length known here: the clamp cannot change them,
    // so the view is SIZED like a row, and `a[1..3] * 2` vectorizes.
    int sized = 0;
    if (f->nodes[start].op == IR_CONST_I && f->nodes[len].op == IR_CONST_I && !bt.inner_len
        && bt.len > 0 && (is_array(bt.kind) || is_slice(bt.kind))) {
        long long s0 = f->nodes[start].ki, n0 = f->nodes[len].ki;
        if (s0 >= 0 && n0 > 0 && s0 + n0 <= bt.len) sized = (int)n0;
    }
    // The emitted C names each operand more than once (the clamp mentions the
    // base length, the start and the length), so anything that isn't already a
    // constant or a plain local read is staged into a temp first.
    int *items = 0, n_items = 0, cap_items = 0;
    base  = stage_operand(vm, f, base,  &items, &n_items, &cap_items);
    start = stage_operand(vm, f, start, &items, &n_items, &cap_items);
    len   = stage_operand(vm, f, len,   &items, &n_items, &cap_items);

    int items_begin = ir_alloc_items(vm, f, 3);
    f->child_indices[items_begin + 0] = base;
    f->child_indices[items_begin + 1] = start;
    f->child_indices[items_begin + 2] = len;
    int sl = ir_new(vm, f, IR_SLICE);
    f->nodes[sl].ki          = bt.pack_bits;
    f->nodes[sl].items_begin = items_begin;
    f->nodes[sl].n_items     = 3;
    f->nodes[sl].type.kind       = slice_of(arr_elem(bt.kind));
    f->nodes[sl].type.len        = sized;
    f->nodes[sl].type.elem_shift = 0;
    f->nodes[sl].type.pack_bits  = bt.pack_bits;
    // `p[a..b]` of a read-only view is another view of the same storage, so it
    // is read-only too -- without this line a sub-slice launders const away.
    f->nodes[sl].type.is_const   = bt.is_const;
    *out_t = f->nodes[sl].type;
    return inline_comma(vm, f, sl, *out_t, items, n_items);
}

// slice(x, start, len): a view into an array or slice, no copy. The subscript
// spelling `x[start..end]` (compile_slice_range) is the same thing with the
// length worked out from the two endpoints.
static int compile_slice_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    ASTNode *par = node->right;
    if (call_argc(par) != 3) {
        vm_errorf_at(vm, node, "slice(x, start, len) expects 3 args, got %d", call_argc(par));
        return -1;
    }
    VMType bt;
    int base = compile_expr(vm, f, call_arg(par, 0), &bt);
    if (base < 0) return -1;
    if (!check_sliceable(vm, f, node, base, bt, "slice()")) return -1;

    VMType raw = { VMT_I32, 0 };
    VMType st, lt;
    int start = compile_expr(vm, f, call_arg(par, 1), &st);
    if (start < 0) return -1;
    if (st.kind != VMT_I32 || ct_shift(st) != 0) start = insert_fx_convert(vm, f, start, st, raw);
    int len = compile_expr(vm, f, call_arg(par, 2), &lt);
    if (len < 0) return -1;
    if (lt.kind != VMT_I32 || ct_shift(lt) != 0) len = insert_fx_convert(vm, f, len, lt, raw);
    return emit_slice_ir(vm, f, base, bt, start, len, out_t);
}

// x[start..end] / x[start..=end]: the same view slice() builds, with the length
// worked out from the endpoints -- `end - start`, or one more for `..=`. The `+ 1`
// is folded onto the end expression as a synthetic AST node rather than added to the
// compiled length, so `end` is still compiled exactly once.
//
// Out-of-range endpoints clamp, exactly as slice()'s do, and a reversed pair gives
// an empty slice rather than a negative length.
static int compile_slice_range(VM *vm, Func *f, ASTNode *at, ASTNode *base_ast,
                               ASTNode *range, VMType *out_t) {
    VMType bt;
    int base = compile_expr(vm, f, base_ast, &bt);
    if (base < 0) return -1;
    if (!check_sliceable(vm, f, at, base, bt, "a range subscript")) return -1;
    // On a nested array `a[i]` is a *row*, so `a[x..y]` would have to be a
    // range of rows -- a slice whose element is itself a slice, which there is
    // no type for. Rejected rather than silently meaning "flat elements x..y",
    // which is what falling through would give (and what .len would then
    // disagree with).
    if (bt.inner_len) {
        vm_errorf_at(vm, at, "cannot take a range of rows from a nested array; "
                             "take one row first, e.g. a[i][x..y]");
        return -1;
    }

    VMType raw = { VMT_I32, 0 };
    VMType st, et;
    int start = compile_expr(vm, f, range->left, &st);
    if (start < 0) return -1;
    if (st.kind != VMT_I32 || ct_shift(st) != 0) start = insert_fx_convert(vm, f, start, st, raw);

    int end = compile_expr(vm, f, range->right, &et);
    if (end < 0) return -1;
    if (et.kind != VMT_I32 || ct_shift(et) != 0) end = insert_fx_convert(vm, f, end, et, raw);
    // `..=` includes the endpoint. A constant one moves here rather than through
    // an add, so the length stays constant with #disable const_folding too.
    if (range->token == TOK_DOTDOT_EQ) {
        VMType lt;
        if (f->nodes[end].op == IR_CONST_I) end = ir_const_of(vm, f, raw, f->nodes[end].ki + 1, 0.0);
        else end = compile_binop_ir(vm, f, range, OP_ADD, end, raw, ir_const_of(vm, f, raw, 1, 0.0), raw, &lt);
        if (end < 0) return -1;
    }

    // start is named twice from here (the length, and the slice's own start),
    // so stage it before it can be compiled into two different temporaries.
    int *pre = 0, n_pre = 0, cap_pre = 0;
    start = stage_operand(vm, f, start, &pre, &n_pre, &cap_pre);

    // Two constant endpoints fold to a constant length, so `x[1..3]` emits
    // exactly what `slice(x, 1, 2)` does -- no staged temp, and the emitter's
    // clamp collapses.
    int len;
    if (f->nodes[end].op == IR_CONST_I && f->nodes[start].op == IR_CONST_I) {
        long long n = f->nodes[end].ki - f->nodes[start].ki;
        len = ir_i32(vm, f, n < 0 ? 0 : n);
    } else {
        VMType lt;
        len = compile_binop_ir(vm, f, range, OP_SUB, end, raw, start, raw, &lt);
        if (len < 0) return -1;
    }

    int sl = emit_slice_ir(vm, f, base, bt, start, len, out_t);
    if (sl < 0 || !n_pre) return sl;

    // The staged start has to run ahead of the slice, so splice it in front.
    return inline_comma(vm, f, sl, *out_t, pre, n_pre);
}

static int compile_str_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    if (!node->right || !is_paren(node->right)) return -1;
    if (ident_is(vm, node->left, "slice")) {
        *handled = 1;
        return compile_slice_intrinsic(vm, f, node, out_t);
    }
    if (!ident_is(vm, node->left, "str")) return -1;
    *handled = 1;

    ASTNode *par = node->right;
    int argc = call_argc(par);
    if (argc < 1 || argc > 2) {
        vm_errorf_at(vm, node, "str(x[, decimals]) expects 1 or 2 args, got %d", argc);
        return -1;
    }
    // The decimal count is baked into the IR node, so it has to be a literal.
    int decimals = STRB_DEFAULT_DECIMALS;
    if (argc == 2) {
        decimals = call_arg_const_int(call_arg(par, 1));
        if (decimals < 0) {
            vm_errorf_at(vm, node, "str()'s decimals argument must be a literal 0..9");
            return -1;
        }
    }
    VMType at;
    int a = compile_expr(vm, f, call_arg(par, 0), &at);
    if (a < 0) return -1;

    StrBuild b;
    strb_begin(vm, f, &b);
    if (strb_part(vm, f, &b, call_arg(par, 0), a, at, decimals) < 0) return -1;
    return strb_finish(vm, f, &b, out_t);
}

// A []u8 IR_DATA_SLICE over `n` bytes copied from `s` -- the node
// compile_string_literal builds, for text the COMPILER supplies rather than
// the script (print's argument separator, and its empty payload).
static int compiler_bytes_literal(VM *vm, Func *f, const char *s, int n) {
    unsigned char *data = (unsigned char*)mem_alloc(&vm->run.mem, (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) data[i] = (unsigned char)s[i];
    int ir = ir_new(vm, f, IR_DATA_SLICE);
    f->nodes[ir].ki      = (long long)(intptr_t)data;
    f->nodes[ir].n_items = n;
    f->nodes[ir].sub_op  = 8;
    f->nodes[ir].type    = byte_slice_type();
    return ir;
}

// print(x, ...) -- hand a formatted line to the host's print sink. An intrinsic
// rather than a registered native for the reason spelled out on IR_PRINT in
// vm_types.h, sitting beside str/slice/bitcast so a script that defines its own
// `print` keeps it. One argument that is ALREADY text is handed to the node
// uncopied; anything else is formatted as `~` does it, joined with single spaces.
static int compile_print_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    // A bare `print` with nothing attached is not a call at all: leave it
    // unhandled so it gets the ordinary "unknown identifier" rather than a
    // print-specific complaint.
    if (!ident_is(vm, node->left, "print") || !node->right) return -1;
    *handled = 1;

    // expand_call_args rather than a paren-only walk: it reads the argument list off
    // ->items, the same field whether the call was written `print(x)`, `print (x)`
    // or `print x`, and it splices a multi-value #define.
    ASTNode *argv[VM_MAX_PARAMS];
    int argc = expand_call_args(vm, node, node->right, argv, VM_MAX_PARAMS);
    if (argc < 0) return -1;

    int    payload;
    VMType pt;
    if (argc == 0) {
        // A blank line: an empty payload, and no build at all.
        payload = compiler_bytes_literal(vm, f, "", 0);
        pt      = byte_slice_type();
    } else if (argc == 1) {
        payload = compile_expr(vm, f, argv[0], &pt);
        if (payload < 0) return -1;
        if (!is_byte_string(pt)) {
            StrBuild b;
            strb_begin(vm, f, &b);
            if (strb_part(vm, f, &b, argv[0], payload, pt,
                          STRB_DEFAULT_DECIMALS) < 0) return -1;
            payload = strb_finish(vm, f, &b, &pt);
            if (payload < 0) return -1;
        }
    } else {
        StrBuild b;
        strb_begin(vm, f, &b);
        for (int i = 0; i < argc; i++) {
            ASTNode *an = argv[i];
            if (i > 0) {
                int sp = compiler_bytes_literal(vm, f, " ", 1);
                if (strb_part(vm, f, &b, an, sp, byte_slice_type(),
                              STRB_DEFAULT_DECIMALS) < 0) return -1;
            }
            VMType at;
            int a = compile_expr(vm, f, an, &at);
            if (a < 0) return -1;
            if (strb_part(vm, f, &b, an, a, at, STRB_DEFAULT_DECIMALS) < 0) return -1;
        }
        payload = strb_finish(vm, f, &b, &pt);
        if (payload < 0) return -1;
    }

    // Take the child index into a local first: ir_new below can reallocate
    // f->nodes, and in `f->nodes[x].a = ir_new(...)` C is free to compute the
    // destination address before the call. (It already is one here -- said
    // out loud because the surrounding code has been bitten by it.)
    int arg = payload;
    int pr  = ir_new(vm, f, IR_PRINT);
    f->nodes[pr].a         = arg;
    f->nodes[pr].type.kind = VMT_VOID;
    *out_t = f->nodes[pr].type;
    return pr;
}

// __ins(x, id) and inspect(x) -- inspection: report x's value, yield x untouched.
//
// The contract is that splicing either around any subexpression cannot change what
// the program computes: the node's type IS the operand's type, so `7 / __ins(2, 0)`
// stays integer division. That is why this is a compiler intrinsic and not a
// registered native -- a native's parameter type is fixed at registration, and an
// i32 argument would coerce into an f64 parameter. See vm.h.
//
//   __ins(x, id)   spliced by the host into a throwaway copy of the source; `id`
//                  names a slot so several inspections can be told apart.
//   inspect(x)     written into the USER'S BUFFER by a pin gesture, and therefore
//                  ordinary source that reaches every export target. Reports under
//                  id -1, so a host reads a pin's values by WRAPPING it as
//                  __ins(inspect(x), N), keeping every instrumenting edit a pure
//                  insertion.
//
// Not under VM_HAS_INSPECT, unlike everything else about inspection: a profile with
// no host to report to still has to COMPILE a buffer containing inspect(x), or a
// pinned body stops building for a target the pin is supposed to be invisible to.
// There it degrades to the operand alone, with no node and no runtime trace.

static int compile_inspect_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    int is_slotted = ident_is(vm, node->left, "__ins");
    int is_pin     = !is_slotted && ident_is(vm, node->left, "inspect");
    if ((!is_slotted && !is_pin) || !node->right) return -1;
    *handled = 1;

    // expand_call_args, not a paren-only walk: the language accepts f(1),
    // f (1) and f 1 for every other call, and requiring parens here was a
    // real bug in print's first version (see compile_print_intrinsic).
    ASTNode *argv[VM_MAX_PARAMS];
    int argc = expand_call_args(vm, node, node->right, argv, VM_MAX_PARAMS);
    if (argc != (is_slotted ? 2 : 1)) {
        vm_set_error_at(vm, node, is_slotted ? "__ins is written __ins(x, id)"
                                             : "inspect is written inspect(x)");
        return -1;
    }

    int slot = VM_INSPECT_ID_NONE;
    if (is_slotted) {
        if (argv[1]->number_flags == NUM_NONE) {
            vm_set_error_at(vm, node, "__ins's second argument must be a literal slot id");
            return -1;
        }
        slot = (int)argv[1]->number;
    }

    VMType at;
    int arg = compile_expr(vm, f, argv[0], &at);
    if (arg < 0) return -1;

#if !VM_HAS_INSPECT
    // No sink can exist on this target, so the wrapper is nothing but its
    // operand. Building no node keeps the emitted code and the serialized blob
    // byte-identical to the same source without the wrapper.
    *out_t = at;
    return arg;
#else
    // Unsupported operand type -> the operand ALONE, with no node built. Hovering a
    // string, an array, a slice or a void call must not fail the compile; it must
    // produce a program that still runs and simply reports nothing.
    //
    // fx (VMT_I32 with len > 0) IS supported, but not as itself: its raw word is not
    // the number it stands for. The shift rides on sub_op -- free on IR_INSPECT,
    // since only BINOP/UNOP/CVT use it -- and the interpreter divides it back out.
    // The kind stays VMT_I32 and the shift goes to the sink beside it.
    //
    // An array or slice of them is supported the same way, with the ELEMENT's shift
    // on sub_op (an aggregate's own `len` is its element count). A packed one --
    // a string, a []u8 buffer, a struct record -- stays unsupported and compiles to
    // the operand alone: it is a byte view, not a list of numbers.
    //
    // An array LITERAL (bare, or staged like `[1,2] + [3,4]`) is different. It
    // cannot simply be wrapped: assignment, return, call arguments and the ternary
    // each special-case that node, so a wrapper around it would change what they
    // emit. The slotted form therefore evaluates it into a hidden local first and
    // reports THAT -- `(t = [3, 4], __ins(t, id))` -- which every consumer already
    // handles, being an ordinary array-valued expression. That costs a copy, which
    // is why only __ins does it: it is the editor's private form and never reaches
    // an export. `inspect(x)` lives in the user's buffer and does reach one, so it
    // leaves a literal alone and reports nothing.
    int is_agg = at.kind == VMT_ARR_I32 || at.kind == VMT_SLICE_I32
#if VM_HAS_F32
              || at.kind == VMT_ARR_F32 || at.kind == VMT_SLICE_F32
#endif
#if VM_HAS_F64
              || at.kind == VMT_ARR_F64 || at.kind == VMT_SLICE_F64
#endif
#if VM_HAS_I64
              || at.kind == VMT_ARR_I64 || at.kind == VMT_SLICE_I64
#endif
                 ;
    int hoist = 0;
    if (is_agg) {
        if (at.pack_bits || at.struct_id) { *out_t = at; return arg; }
        if (f->nodes[arg].op == IR_ARR_LIT || is_staged_arr_lit(f, arg)) {
            if (!is_slotted) { *out_t = at; return arg; }
            hoist = 1;
        }
    }
    int fx_shift = is_agg ? at.elem_shift
                 : (at.kind == VMT_I32 && at.len > 0) ? at.len : 0;
    // A shift the interpreter's divisor cannot represent stays unsupported
    // rather than being reported wrong. Every fx type in use is far under this.
    if (fx_shift >= 31 || fx_shift < 0) { *out_t = at; return arg; }
    int supported = is_agg || at.kind == VMT_I32
#if VM_HAS_F32
                 || at.kind == VMT_F32
#endif
#if VM_HAS_F64
                 || at.kind == VMT_F64
#endif
#if VM_HAS_I64
                 || at.kind == VMT_I64
#endif
                 ;
    if (!supported) {
        *out_t = at;
        return arg;
    }

    // A literal is bound to a hidden local, and what gets reported is a read of it.
    int *items = 0, n_items = 0, cap_items = 0;
    if (hoist) {
        arg = stage_operand(vm, f, arg, &items, &n_items, &cap_items);
        if (arg < 0) return -1;
    }

    // Take the child index into a local first: ir_new below can reallocate
    // f->nodes, and in `f->nodes[x].a = ir_new(...)` C is free to compute the
    // destination address before the call. (Same note as
    // compile_print_intrinsic -- the surrounding code has been bitten by it.)
    int a  = arg;
    int ir = ir_new(vm, f, IR_INSPECT);
    f->nodes[ir].a      = a;
    f->nodes[ir].ki     = (long long)slot;
    f->nodes[ir].sub_op = fx_shift;  // 0 for everything that is not fixed-point
    f->nodes[ir].type   = at;        // pass-through: the operand's type verbatim
    *out_t = at;
    // (t = literal, __ins(t, id)): the assignment runs, the report is the value.
    return inline_comma(vm, f, ir, at, items, n_items);
#endif // VM_HAS_INSPECT
}

// bitcast(T, x) -- reinterpret x's bits as T, with no numeric conversion. The
// counterpart to `x as T` / `T(x)`, which rescale: `bitcast(fx16, 1000)` is the fx16
// whose raw word is 1000 (about 0.0153), where `fx16(1000)` is 1000 << 16.
//
// Only same-width pairs are legal, since there are no bits to invent or drop: i32
// (and every fxN, which shares its storage) with f32, i64 with f64. Inside i32
// storage it is a pure compile-time relabel; the two float pairs become an
// IR_BITCAST node.
//
// The type is the *first* argument, matching WGSL's bitcast<f32>(x) and C++'s
// bit_cast<T>(x). It is never compiled as an expression, so the bare `fx16` reaches
// parse_type intact.
static int compile_bitcast(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    if (!ident_is(vm, node->left, "bitcast")) return -1;
    *handled = 1;
    // `bitcast fx16 (x)` (no comma) parses as ONE argument -- and that
    // argument is the *converting* cast fx16(x), which would rescale before
    // this ever saw it. Arity alone would catch it, but not readably.
    if (!node->right || !is_paren(node->right)) {
        vm_set_error_at(vm, node, "bitcast is written bitcast(T, x), with a comma");
        return -1;
    }
    ASTNode *par = node->right;
    int argc = call_argc(par);
    if (argc != 2) {
        vm_errorf_at(vm, node, "bitcast(T, x) expects 2 args, got %d", argc);
        return -1;
    }

    // The type rewire applies exactly as it does to `x as T` and to a declaration.
    //
    // Unlike the call spelling `i32(x)`, this slot needs no cast_name_is_free check:
    // bitcast's first argument is only ever a type, so a script's own binding of
    // that name has nothing to say here.
    ASTNode *tn = call_arg(par, 0);
    VMType target;
    if (!cast_target_type(vm, tn, &target)) {
        char desc[128];
        node_describe(vm, tn, desc, sizeof(desc));
        vm_errorf_at(vm, tn, "bitcast's first argument must be a scalar type, got %s", desc);
        return -1;
    }

    VMType at;
    int ae = compile_expr(vm, f, call_arg(par, 1), &at);
    if (ae < 0) return -1;

    int from_bits = ct_storage_bits(at), to_bits = ct_storage_bits(target);
    if (from_bits == 0 || to_bits == 0 || from_bits != to_bits) {
        // type_label, not scalar_type_label: the "not a scalar" half of this
        // message is reached precisely BY an aggregate, which has to print as
        // one ("[2]i32", not the "i32" of its element).
        char fb[24], tb[24];
        vm_errorf_at(vm, node, "cannot bitcast %s to %s: %s",
            type_label(at, fb, sizeof(fb)),
            type_label(target, tb, sizeof(tb)),
            (from_bits && to_bits) ? "different storage widths"
                                   : "bitcast only applies to scalars");
        return -1;
    }

    *out_t = target;

    // Constants fold outright, in either direction. A *new* node carries the
    // result rather than the operand being retyped in place, so this stays
    // correct even where an operand node is shared.
    if (f->nodes[ae].op == IR_CONST_I || f->nodes[ae].op == IR_CONST_F) {
        long long ki = f->nodes[ae].ki;
        double    kf = f->nodes[ae].kf;
        int src_is_float = (at.kind == VMT_F32 || at.kind == VMT_F64);
        int dst_is_float = (target.kind == VMT_F32 || target.kind == VMT_F64);
        if (src_is_float == dst_is_float) {
            // Same storage class: only the compile-time type differs.
            int c = ir_new(vm, f, f->nodes[ae].op);
            f->nodes[c].ki = ki; f->nodes[c].kf = kf;
            f->nodes[c].type = target;
            return c;
        }
        if (from_bits == 32) {
            union { int32_t i; float fl; } u;
            if (src_is_float) {
                u.fl = (float)kf;
                int c = ir_const_i(vm, f, target, (long long)u.i);
                return c;
            }
            u.i = (int32_t)ki;
            int c = ir_new(vm, f, IR_CONST_F);
            f->nodes[c].kf = (double)u.fl;
            f->nodes[c].type = target;
            return c;
        }
        union { int64_t i; double d; } u;
        if (src_is_float) {
            u.d = kf;
            int c = ir_const_i(vm, f, target, (long long)u.i);
            return c;
        }
        u.i = (int64_t)ki;
        int c = ir_new(vm, f, IR_CONST_F);
        f->nodes[c].kf = u.d;
        f->nodes[c].type = target;
        return c;
    }

    int bc = ir_new(vm, f, IR_BITCAST);
    f->nodes[bc].a      = ae;
    f->nodes[bc].sub_op = at.kind;   // source kind, exactly as IR_CVT records it
    f->nodes[bc].type   = target;
    return bc;
}

// `/~`, `/%` and `%%`. Compile-only: compile_binop_ir lowers each onto ordinary
// IR, so none of them ever reaches an IR_BINOP, an emitter or the interpreter.
enum { SUBOP_TDIV = 1000, SUBOP_FLOORDIV, SUBOP_FLOORMOD };

static int op_from_token(InternID t, int *out) {
    switch (t) {
        case TOK_PLUS:  *out = OP_ADD; return 1;
        case TOK_MINUS: *out = OP_SUB; return 1;
        case TOK_MUL:   *out = OP_MUL; return 1;
        case TOK_SLASH: *out = OP_DIV; return 1;
        case TOK_PERCENT: *out = OP_MOD; return 1;
        case TOK_SLASH_TILDE:   *out = SUBOP_TDIV;     return 1;
        case TOK_SLASH_PERCENT: *out = SUBOP_FLOORDIV; return 1;
        case TOK_DOUBLE_PERCENT: *out = SUBOP_FLOORMOD; return 1;
        case TOK_LT:    *out = OP_LT;  return 1;
        case TOK_LT_EQ: *out = OP_LE;  return 1;
        case TOK_GT:    *out = OP_GT;  return 1;
        case TOK_GT_EQ: *out = OP_GE;  return 1;
        case TOK_EQ_EQ: case TOK_EQ_EQ_EQ: *out = OP_EQ; return 1;
        case TOK_NOT_EQ: case TOK_NOT_EQ_EQ: *out = OP_NE; return 1;
        case TOK_AND_AND: *out = OP_AND; return 1;
        case TOK_OR_OR:   *out = OP_OR;  return 1;
        case TOK_AMP:   *out = OP_BAND; return 1;
        case TOK_PIPE:  *out = OP_BOR;  return 1;
        case TOK_CARET: *out = OP_BXOR; return 1;
        case TOK_LSHIFT:*out = OP_BSHL; return 1;
        case TOK_RSHIFT:*out = OP_BSHR; return 1;
    }
    return 0;
}

// ---- update operators: compound assignment and ++/-- ---------------------
// All of them mean the same thing as a statement -- `target = target <op>
// operand` -- so they are rewritten into exactly that AST shape and fed back
// through the ordinary assignment path.  That reuses declaration-on-first-
// assign, fixed-point conversion, the lossy-assignment check and `a[i] = ...`
// targets instead of growing a second copy of all of it.

// The binary operator a compound assignment stands for, or 0 if `t` isn't one.
// The two token runs are laid out in parallel (see tokens.h), so this is a
// subtraction rather than a table.
static InternID compound_assign_binop(InternID t) {
    return TOKEN_IS_COMPOUND_ASSIGN(t) ? (InternID)TOKEN_COMPOUND_ASSIGN_BINOP(t) : 0;
}

typedef struct {
    ASTNode *target;   // assignment target; also the left operand of `op`
    ASTNode *operand;  // right operand of `op`
    InternID op;       // TOK_PLUS / TOK_MUL / ...; 0 when the node isn't an update
    int      postfix;  // 1 for `a++` / `a--` -- changes only the value the
                       // expression form yields, never what gets stored
} UpdateOp;

// Recognise `a op= b` (left+right), `a++` (left only) and `++a` (right only).
// `one` is caller-owned scratch for the implicit 1 that ++/-- add; it must stay
// alive as long as the returned UpdateOp is used.
static UpdateOp update_op_split(VM *vm, ASTNode *node, ASTNode *one) {
    UpdateOp u; u.target = 0; u.operand = 0; u.op = 0; u.postfix = 0;
    if (!node) return u;
    InternID bop = compound_assign_binop(node->token);
    if (bop && node->left && node->right) {
        u.op = bop; u.target = node->left; u.operand = node->right;
        return u;
    }
    // Exactly one side present: postfix keeps its operand on the left, prefix
    // on the right (see parser.c's operator loop / get_unary_op).
    if (TOKEN_IS_INC_DEC(node->token) && (!node->left != !node->right)) {
        vm->run.sys->memset(one, 0, sizeof(*one));
        one->number = 1;
        one->number_flags = NUM_INTEGER;
        u.op      = (node->token == TOK_PLUS_PLUS) ? TOK_PLUS : TOK_MINUS;
        u.operand = one;
        u.postfix = node->left ? 1 : 0;
        u.target  = node->left ? node->left : node->right;
    }
    return u;
}

// Compile the assignment an update operator stands for, pushing it onto the
// given statement list.  The two synthetic nodes are the caller's locals: no
// statement AST is retained past compilation (only function-definition bodies
// are, and a function literal is never an update-operator operand).
static int compile_update_stmt(VM *vm, Func *f, const UpdateOp *u,
                               int **slot, int *count, int *cap) {
    {
        int st = compile_update_field_staged(vm, f, u->target, u->op, u->operand, slot, count, cap);
        if (st != 0) return st;
    }
    ASTNode bin, asn;
    vm->run.sys->memset(&bin, 0, sizeof(bin));
    vm->run.sys->memset(&asn, 0, sizeof(asn));
    bin.token = u->op;   bin.left = u->target; bin.right = u->operand;
    asn.token = TOK_EQ;  asn.left = u->target; asn.right = &bin;
    // Synthetic, so auto_const has no census entry matching its shape. It could
    // only ever fire on an update to a name nothing declared, which is an error
    // anyway -- but the rule is the shape, not the reachability. See VM.ac_suspend.
    vm->ac_suspend++;
    int r = compile_stmt_into(vm, f, &asn, slot, count, cap);
    vm->ac_suspend--;
    return r;
}

// The same operators used as a *value*.  Compiles to an IR_COMMA:
//     [ (postfix only) tmp = target,  target = target op operand,  value ]
// where `value` re-reads the target -- except for the postfix form, which must yield
// the pre-update value and so reads the staged temp instead.
//
// The target is compiled two or three times over, so keep index expressions in an
// update target pure: `a[i++] += 1` would run that inner update more than once.
static int compile_update_expr(VM *vm, Func *f, const UpdateOp *u, VMType *out_t) {
    // The statement form goes through compile_stmt_into's assignment branch and
    // is caught there; the value form assembles its own assignment, so it needs
    // the same guard here.
    if (is_ident(u->target) && !const_reject_write(vm, u->target, u->target->token, "assigned to"))
        return -1;
    // Stage the pre-update value first, before the assignment can clobber it.
    int stage = -1, tslot = -1;
    VMType tty = { VMT_VOID, 0, 0 };
    if (u->postfix) {
        VMType ot;
        int old = compile_expr(vm, f, u->target, &ot);
        if (old < 0) return -1;
        VMSym *t = sym_add(vm, f, 0, ct_vmtype_clear_shift(ot));
        t->shift = ct_shift(ot);
        t->used  = 1;
        tslot = (int)(t - f->syms);
        tty   = ot;
        int lv = ir_new(vm, f, IR_LOCAL);
        f->nodes[lv].type = t->type;
        f->nodes[lv].ki   = tslot;
        stage = ir_new(vm, f, IR_ASSIGN);
        f->nodes[stage].a = lv; f->nodes[stage].b = old;
    }

    int *slot = 0, scount = 0, scap = 0;
    if (compile_update_stmt(vm, f, u, &slot, &scount, &scap) < 0) return -1;
    // compile_stmt_into's assignment path always pushes exactly one IR_ASSIGN;
    // a function-declaration RHS (its other outcome) can't occur here.
    if (scount != 1 || f->nodes[slot[0]].op != IR_ASSIGN) {
        vm_set_error_at(vm, u->target, scount == 2
        ? "this update evaluates its target through a call; use it as a statement"
        : "invalid target for update operator");
        return -1;
    }

    int val;
    VMType vt;
    if (u->postfix) {
        val = ir_new(vm, f, IR_LOCAL);
        f->nodes[val].type = tty;   // shift carried on the node, as compile_ident does
        f->nodes[val].ki   = tslot;
        vt  = tty;
    } else {
        val = compile_expr(vm, f, u->target, &vt);
        if (val < 0) return -1;
    }

    int nitems = (stage >= 0 ? 3 : 2);
    int items_begin = ir_alloc_items(vm, f, nitems);
    int k = 0;
    if (stage >= 0) f->child_indices[items_begin + k++] = stage;
    f->child_indices[items_begin + k++] = slot[0];
    f->child_indices[items_begin + k++] = val;

    int comma = ir_new(vm, f, IR_COMMA);
    f->nodes[comma].items_begin = items_begin;
    f->nodes[comma].n_items     = nitems;
    f->nodes[comma].type        = vt;
    *out_t = vt;
    return comma;
}

// True if `n` is an integer literal equal to `v` that compiles as a plain
// (un-rewired) raw i32 -- i.e. folding it away changes neither the value nor
// the result type of the surrounding op. See #disable identity_elim.
static int is_identity_literal(VM *vm, ASTNode *n, double v) {
    if (!n || !is_number(n) || (n->number_flags & NUM_TYPE) != NUM_INTEGER) return 0;
    if (n->number != v) return 0;
    return rewire_apply(vm, VMT_I32, REWIRE_LITERAL) == VMT_I32
        && rewire_shift(vm, VMT_I32, REWIRE_LITERAL) == 0;
}

// Find a registered native (C) function by its token, e.g. TOK_POW/TOK_FMOD.
static Func *find_native_func(VM *vm, int tok) {
    for (Func *g = vm->run.funcs; g; g = g->next)
        if (g->native_tok == tok) return g;
    return 0;
}

// Emit a call to a 2-arg native function (e.g. pow/fmod). Operand coercion
// mirrors compile_call's built-in dispatch so `**`/`%%`-style operators reach
// the same variant a spelled-out call would:
//   - if an operand carries a fixed-point shift and the function has an fx
//     variant (e.g. fx16_pow), align both args to its native shift and tag the
//     call fx (sub_op=1), yielding a fixed-point result -- NOT a raw-int one;
//   - otherwise convert to the promoted float/int type, but if a fixed-point
//     operand is present with no fx variant, promote to f64 so the fx value is
//     rescaled to its real magnitude (>> by 2^shift via DIV), not truncated.
static int emit_native_binop_call(VM *vm, Func *f, int tok, int l, VMType lt, int r, VMType rt, VMType *out_t) {
    if (tok == TOK_IPOW && ipow_is_i64(lt, rt))
        return emit_ipow64_call(vm, f, vm->err_ctx, l, lt, r, rt, out_t);
    Func *callee = find_native_func(vm, tok);
    CFuncEntry *e = &vm->run.cfunc_table[tok - TOK_MATHS_FIRST];

    int any_fx = (lt.kind == VMT_I32 && ct_shift(lt) > 0)
              || (rt.kind == VMT_I32 && ct_shift(rt) > 0);

    VMType ft = {0};
    int fx_call = 0;
    if (any_fx && e->fx_fn && e->fx_shift > 0) {
        // Fixed-point variant: align both args to fx_shift, result is fxN.
        ft.kind = VMT_I32; ft.len = e->fx_shift; ft.elem_shift = 0;
        fx_call = 1;
    } else {
        VTKind eff = promote(vm, lt.kind, rt.kind);
        // A fixed-point operand with no fx variant must go through float, or
        // insert_fx_convert would shift its raw word down to a whole integer.
        if (any_fx && eff != VMT_F32 && eff != VMT_F64)
            eff = rewire_apply(vm, VMT_F64, REWIRE_TYPE);
        ft.kind = eff; ft.len = 0; ft.elem_shift = 0;
    }
    l = insert_fx_convert(vm, f, l, lt, ft);
    r = insert_fx_convert(vm, f, r, rt, ft);
    int args_begin = ir_alloc_items(vm, f, 2);
    int ir = ir_new(vm, f, IR_CALL);
    f->child_indices[args_begin + 0] = l;
    f->child_indices[args_begin + 1] = r;
    f->nodes[ir].items_begin = args_begin;
    f->nodes[ir].n_items = 2;
    f->nodes[ir].ki = callee->func_id;
    f->nodes[ir].sub_op = fx_call;   // 1 => dispatch to CFuncEntry.fx_fn
    f->nodes[ir].type = ft;
    *out_t = ft;
    int folded = try_fold_native_call(vm, f, ir);
    return folded >= 0 ? folded : ir;
}

// ---- ** inline-power expansion ------------------------------------------
// Fast integer exponents get expanded to repeated multiplication with the base
// staged once into a scratch temp (square-and-multiply, minimizing the multiply
// count). Everything else falls back to ipow/pow. See #disable inline_powers.
//
// The plan for each exponent: stage the base into t, apply a sequence of
// "t = t^step" self-multiplies, then yield "t^final".
static int pow_inline_plan(int exp, int steps[3], int *final) {
    switch (exp) {
        case 2:  *final = 2; return 0;
        case 3:  *final = 3; return 0;
        case 4:  steps[0] = 2; *final = 2; return 1;
        case 6:  steps[0] = 3; *final = 2; return 1;
        case 8:  steps[0] = 2; steps[1] = 2; *final = 2; return 2;
        case 9:  steps[0] = 3; *final = 3; return 1;
        case 12: steps[0] = 3; *final = 4; return 1;
        case 16: steps[0] = 2; steps[1] = 2; steps[2] = 2; *final = 2; return 3;
    }
    return -1;
}
static int pow_is_inlinable_exp(int exp) {
    int steps[3], final;
    return pow_inline_plan(exp, steps, &final) >= 0;
}

// A fresh IR_LOCAL read of scratch slot `tslot` (type `ty`).
static int pow_read_temp(VM *vm, Func *f, int tslot, VMType ty) {
    int lv = ir_new(vm, f, IR_LOCAL);
    f->nodes[lv].type = ty;
    f->nodes[lv].ki = tslot;
    return lv;
}
// t * t * ... (k factors), left-associative, reading the scratch temp.
// shift==0: plain OP_MUL of type `ty` (float, i64 or raw i32). shift>0: `ty`
// must be i64 and each pairwise product is shift-corrected -- (a*b)>>shift --
// so the accumulator stays in Q<shift> throughout (fixed-point self-multiply).
static int pow_temp_product(VM *vm, Func *f, int tslot, VMType ty, int k, int shift) {
    int acc = pow_read_temp(vm, f, tslot, ty);
    for (int i = 1; i < k; i++) {
        int r = pow_read_temp(vm, f, tslot, ty);
        int b = ir_new(vm, f, IR_BINOP);
        f->nodes[b].sub_op = OP_MUL;
        f->nodes[b].a = acc; f->nodes[b].b = r;
        f->nodes[b].type = ty;
        if (shift > 0) {
            int shc = ir_const_i(vm, f, ty, shift);
            int shr = ir_new(vm, f, IR_BINOP);
            f->nodes[shr].sub_op = OP_BSHR;
            f->nodes[shr].a = b; f->nodes[shr].b = shc;
            f->nodes[shr].type = ty;   // i64
            b = shr;
        }
        acc = b;
    }
    return acc;
}
// Build (t = base, t = t^step..., t^final) as an IR_COMMA. `bt` is either a
// mul-safe scalar (float, i64, or raw i32 -- worked in-place) or a fixed-point
// i32 (shift>0), in which case the temp is widened to i64, every self-multiply
// is shift-corrected (see pow_temp_product), and the final value is narrowed
// back to the base's fx type. Callers gate the fx case on i64 availability.
static int build_pow_inline(VM *vm, Func *f, int base_ir, VMType bt, int exp, VMType *out_t) {
    int steps[3], final;
    int nsteps = pow_inline_plan(exp, steps, &final);
    if (nsteps < 0) return -1;

    int fx_shift = (bt.kind == VMT_I32) ? ct_shift(bt) : 0;
    VMType ty = {0};
    int base_staged;
    if (fx_shift > 0) {
        // Fixed-point: accumulate raw Q<shift> bits in i64.
        ty.kind = VMT_I64; ty.len = 0; ty.elem_shift = 0;
        base_staged = insert_cvt(vm, f, base_ir, ct_vmtype_clear_shift(bt), VMT_I64);
    } else {
        ty = ct_vmtype_clear_shift(bt);
        base_staged = base_ir;
    }

    VMSym *t = sym_add(vm, f, 0, ty);
    t->shift = 0;
    t->used  = 1;
    int tslot = (int)(t - f->syms);

    int nitems = 1 + nsteps + 1;  // init + step assigns + final value
    int items_begin = ir_alloc_items(vm, f, nitems);

    // init: t = base
    {
        int lv = pow_read_temp(vm, f, tslot, ty);
        int asn = ir_new(vm, f, IR_ASSIGN);
        f->nodes[asn].a = lv; f->nodes[asn].b = base_staged;
        f->child_indices[items_begin + 0] = asn;
    }
    // steps: t = t^step
    for (int i = 0; i < nsteps; i++) {
        int prod = pow_temp_product(vm, f, tslot, ty, steps[i], fx_shift);
        int lv = pow_read_temp(vm, f, tslot, ty);
        int asn = ir_new(vm, f, IR_ASSIGN);
        f->nodes[asn].a = lv; f->nodes[asn].b = prod;
        f->child_indices[items_begin + 1 + i] = asn;
    }
    // final value: t^final
    int fin = pow_temp_product(vm, f, tslot, ty, final, fx_shift);

    VMType result_ty = {0};
    if (fx_shift > 0) {
        // Narrow the i64 Q<shift> accumulator back to the base's fx type.
        result_ty.kind = VMT_I32; result_ty.len = fx_shift; result_ty.elem_shift = 0;
        fin = insert_cvt_t(vm, f, fin, ty, result_ty);
    } else {
        result_ty = ty;
    }
    f->child_indices[items_begin + 1 + nsteps] = fin;

    int comma = ir_new(vm, f, IR_COMMA);
    f->nodes[comma].items_begin = items_begin;
    f->nodes[comma].n_items     = nitems;
    f->nodes[comma].type        = result_ty;
    *out_t = result_ty;
    return comma;
}

// Compile `base ** exp`. Integer literal exponents in the inlinable set expand
// to repeated multiplication (unless #disable inline_powers); otherwise emit a
// call to ipow (integer operands) or pow (float / fixed-point operands).
// `base ** exp` given an already-compiled base. Split out of compile_pow_op for
// the reason compile_native_call_ir was split out of compile_call: one LANE of
// `v ** 2` has to go through the identical policy -- inline powers, the fx
// widening rule, the pow/ipow choice -- as the scalar it would have been.
static int compile_pow_ir(VM *vm, Func *f, ASTNode *node, int l, VMType lt,
                          int lit_exp, VMType *out_t) {
    // x**1 == x exactly for any operand type (no squaring, so no shift/scale
    // concerns) -- drop it entirely rather than emitting a pow/ipow call.
    if ((vm->flags & VM_FLAG_INLINE_POWERS) && lit_exp == 1) {
        *out_t = lt;
        return l;
    }

    int base_mul_safe = (lt.kind == VMT_F32 || lt.kind == VMT_F64 || lt.kind == VMT_I64
                         || (lt.kind == VMT_I32 && ct_shift(lt) == 0));
    // Fixed-point bases inline too, but only when i64 is available (the temp
    // accumulates raw Q-bits in i64); otherwise fall through to the fx pow call.
    int base_fx = (lt.kind == VMT_I32 && ct_shift(lt) > 0);
    int i64_ok  = fx_wide_ok(vm);

    if ((vm->flags & VM_FLAG_INLINE_POWERS) && lit_exp >= 0
        && pow_is_inlinable_exp(lit_exp)
        && (base_mul_safe || (base_fx && i64_ok)))
        return build_pow_inline(vm, f, l, lt, lit_exp, out_t);

    // Fallback: compile the exponent normally and emit a native call.
    VMType rt;
    int r = compile_expr(vm, f, node->right, &rt);
    if (r < 0) return -1;
    int is_float = lt.kind == VMT_F32 || lt.kind == VMT_F64
                 || rt.kind == VMT_F32 || rt.kind == VMT_F64;
    // Float or fixed-point operands -> pow; pure integers -> exact ipow.
    if (is_float || ct_shift(lt) > 0 || ct_shift(rt) > 0)
        return emit_native_binop_call(vm, f, TOK_POW, l, lt, r, rt, out_t);
    return emit_native_binop_call(vm, f, TOK_IPOW, l, lt, r, rt, out_t);
}

static int compile_pow_op(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    VMType lt;
    int l = compile_expr(vm, f, node->left, &lt);
    if (l < 0) return -1;

    // Compile-time non-negative integer literal exponent?
    ASTNode *exp_ast = paren_inner(node->right);
    int lit_exp = -1;
    if (is_number(exp_ast) && (exp_ast->number_flags & NUM_TYPE) == NUM_INTEGER) {
        double ev = exp_ast->number;
        if (ev >= 0 && ev == (double)(long long)ev && ev <= 1024)
            lit_exp = (int)ev;
    }

    // An aggregate base unrolls, exactly as `v * v` does -- compile_pow_op runs
    // ahead of compile_expr's binop branch, so without this `v ** 2` never
    // reached the lane machinery and typed itself scalar in silence.
    if ((is_array(lt.kind) || is_slice(lt.kind)) && !vmt_is_struct(lt)) {
        if (!(vm->flags & VM_FLAG_AUTO_VEC)) {
            vm_errorf_at(vm, node, "elementwise operators are disabled here (#enable auto_vec)");
            return -1;
        }
        return compile_vec_pow(vm, f, node, l, lt, lit_exp, out_t);
    }

    return compile_pow_ir(vm, f, node, l, lt, lit_exp, out_t);
}

// ---------- elementwise operators on arrays -------------------------------
//
// A vector expression is not a value, it is a compile-time list of lanes.
// `[1,2] + [3,4]` produces an IR_ARR_LIT whose two elements are the scalar
// expressions `1+3` and `2+4`, so there is no new IROp, no new VTKind, no
// interpreter arm and no serialization change -- it is loop unrolling done in the
// compiler. The cost is code size, which is why the lane count is capped.

// One operand, expanded into lanes.
typedef struct {
    int     n;          // lane count (1 and is_scalar for a broadcast operand)
    int     is_scalar;  // reuse lane 0 against every lane of the other side
    int     inner_len;  // nested-array row width, carried to the result
    int     ir[VM_VEC_MAX_LANES];
    VMType  t[VM_VEC_MAX_LANES];
} VecLanes;

// An aggregate of known length. []u8 is included: with concatenation moved to
// `..`, a string has no separate meaning under `+`, and adding two of them
// elementwise is the same thing it is for any other array.
//
// A SLICE qualifies when its length is known -- a nested array's row, or a
// parameter that took a fixed array. What matters for unrolling is the lane
// count, not whether the bytes are owned here or pointed at.
static int vec_is_vectorizable(VMType t) {
    return (is_array(t.kind) || is_slice(t.kind)) && t.len > 0;
}

// Expand one compiled operand into lanes. Only shapes whose lane `i` can be read
// without re-evaluating the operand or repeating a side effect qualify:
//
//   IR_ARR_LIT                          each element expression, used once
//   IR_LOCAL of fixed-array type        a fresh IR_INDEX(local, i) -- pure
//   IR_COMMA ending in an IR_ARR_LIT    that literal's lanes, with the comma's
//                                       leading items spliced into `pre`
//
// The third is what makes a chain compose: `(a + b) + c` reuses the inner result's
// lanes instead of materialising it. Anything else would have to be evaluated once
// per lane, so it is rejected rather than duplicated.
static int vec_lane_source(VM *vm, Func *f, ASTNode *at, int ir, VMType t,
                           VecLanes *out, int **pre, int *n_pre, int *cap_pre) {
    int lit = ir;
    if (f->nodes[ir].op == IR_COMMA && f->nodes[ir].n_items > 0) {
        int last = f->child_indices[f->nodes[ir].items_begin + f->nodes[ir].n_items - 1];
        // A trailing IR_SLICE of known length is as good as a literal: that is
        // the shape compile_row_slice returns once it has staged its base and
        // offset, so `a[i] + a[j]` composes rather than being rejected.
        int slice_tail = is_slice(f->nodes[last].type.kind) && f->nodes[last].type.len > 0;
        if (f->nodes[last].op != IR_ARR_LIT && !slice_tail) {
            vm_errorf_at(vm, at, "cannot vectorize this operand; assign it to a variable first");
            return 0;
        }
        // Splice the leading items in ahead of our own lanes, in order.
        for (int i = 0; i < f->nodes[ir].n_items - 1; i++)
            stmt_push_idx(vm, pre, n_pre, cap_pre,
                          f->child_indices[f->nodes[ir].items_begin + i]);
        lit = last;
    }

    out->is_scalar = 0;
    out->inner_len = t.inner_len;
    if (f->nodes[lit].op == IR_ARR_LIT) {
        int n = f->nodes[lit].n_items;
        if (n > VM_VEC_MAX_LANES) {
            vm_errorf_at(vm, at, "vectorizing this would emit %d lanes (limit %d); write a loop",
                         n, VM_VEC_MAX_LANES);
            return 0;
        }
        out->n = n;
        for (int i = 0; i < n; i++) {
            int e = f->child_indices[f->nodes[lit].items_begin + i];
            out->ir[i] = e;
            out->t[i]  = f->nodes[e].type;
        }
        return 1;
    }
    if (vec_is_vectorizable(t)) {
        int n = t.len;
        if (n > VM_VEC_MAX_LANES) {
            vm_errorf_at(vm, at, "vectorizing this would emit %d lanes (limit %d); write a loop",
                         n, VM_VEC_MAX_LANES);
            return 0;
        }
        // A local is already re-readable once per lane; anything else (a row's
        // IR_SLICE) is bound to a hidden local first, so the slice header is
        // built once rather than per lane.
        int based = stage_operand(vm, f, lit, pre, n_pre, cap_pre);
        if (f->nodes[based].op != IR_LOCAL) {
            vm_errorf_at(vm, at, "cannot vectorize this operand; assign it to a variable first");
            return 0;
        }
        int slot = (int)f->nodes[based].ki;
        out->n = n;
        for (int i = 0; i < n; i++) {
            // Element type follows compile_index exactly: a packed element is a
            // plain i32 in range, an unpacked one inherits the array's shift.
            int base = strb_local(vm, f, slot);
            int ci = ir_i32(vm, f, i);
            int ix = ir_new(vm, f, IR_INDEX);
            f->nodes[ix].a = base;
            f->nodes[ix].b = ci;
            f->nodes[ix].type.kind = arr_elem(t.kind);
            f->nodes[ix].ki = t.pack_bits;
            if (!t.pack_bits)
                f->nodes[ix].type = ct_set_shift(f->nodes[ix].type, ct_shift(t));
            out->ir[i] = ix;
            out->t[i]  = f->nodes[ix].type;
        }
        return 1;
    }
    vm_errorf_at(vm, at, "cannot vectorize this operand; assign it to a variable first");
    return 0;
}

// The broadcast case: one lane, reused. An impure scalar is bound to a hidden
// local first, because it is named once per lane.
static void vec_scalar_lanes(VM *vm, Func *f, int ir, VecLanes *out,
                             int **pre, int *n_pre, int *cap_pre) {
    out->n = 1;
    out->is_scalar = 1;
    out->inner_len = 0;
    out->ir[0] = stage_operand(vm, f, ir, pre, n_pre, cap_pre);
    out->t[0]  = f->nodes[out->ir[0]].type;
}

// Turn the finished lanes into a genuine array literal, so that assignment,
// `.len`, indexing and `return` all work with nothing added downstream.
static int vec_materialise(VM *vm, Func *f, int *lane, VMType *lane_t, int n,
                           int inner, int *pre, int n_pre, int cap_pre,
                           VMType *out_t) {
    int begin = ir_alloc_items(vm, f, n);
    VTKind elem = lane_t[0].kind;
    for (int i = 0; i < n; i++) {
        f->child_indices[begin + i] = lane[i];
        if (i) elem = promote(vm, elem, lane_t[i].kind);
    }
    int eshift = unify_array_elems(vm, f, begin, n, elem);
    int lit = ir_new(vm, f, IR_ARR_LIT);
    f->nodes[lit].items_begin = begin;
    f->nodes[lit].n_items     = n;
    f->nodes[lit].type.kind   = arr_of(elem);
    f->nodes[lit].type.len    = n;
    f->nodes[lit].type.elem_shift = eshift;
    f->nodes[lit].type.inner_len  = inner;
    if (!n_pre) { *out_t = f->nodes[lit].type; return lit; }

    // Staged operands are hoisted *ahead* of the literal rather than folded
    // into lane 0: C11 6.7.9p23 leaves the evaluations of an initializer list
    // indeterminately sequenced with respect to one another, so a write and a
    // read of the same temp inside one compound literal would be undefined
    // behaviour in emitted C. A comma expression is sequenced, and is already
    // what IR_COMMA emits.
    *out_t = f->nodes[lit].type;
    return ir_comma(vm, f, pre, n_pre, lit, *out_t);
}

// The diagnostics every aggregate operand shares. Returns 0 (having reported)
// when this operand cannot be unrolled.
static int vec_check_operand(VM *vm, Func *f, ASTNode *node, VMType t, int ok) {
    (void)f;
    // A string is a []u8, so it lands here like any other aggregate. It is
    // almost always a literal or a slice -- no compile-time lane count -- and
    // the useful thing to say is which operator the writer meant.
    if (is_byte_string(t) && !ok) {
        vm_errorf_at(vm, node, "cannot do arithmetic on a string; use '~' to join strings");
        return 0;
    }
    // A slice's length is a run-time value, so there is no lane count. Say so
    // rather than falling through to promote(), which would quietly type the
    // whole expression i32 and fail much later.
    if (!ok) {
        vm_errorf_at(vm, node, "cannot vectorize a slice: its length is not known at compile time");
        return 0;
    }
    if (!(vm->flags & VM_FLAG_AUTO_VEC)) {
        vm_errorf_at(vm, node, "elementwise operators are disabled here (#enable auto_vec)");
        return 0;
    }
    return 1;
}

// `a <op> b` where at least one side is an aggregate.
// Expand `n_ops` already-compiled operands into lanes: an aggregate of known
// length unrolls, a scalar broadcasts. Every aggregate must agree on lane count
// and row width. Returns the lane count and the shared row width in *out_inner,
// or -1 having reported. Staged scalars and staged bases land in `pre`.
//
// One copy of the shape rules for every elementwise form -- binary operators,
// and a builtin call of any arity (compile_vec_call).
static int vec_lanes_n(VM *vm, Func *f, ASTNode *at,
                       const int *ir, const VMType *t, int n_ops,
                       VecLanes *out, int *out_inner,
                       int **pre, int *n_pre, int *cap_pre) {
    for (int i = 0; i < n_ops; i++) {
        int vec = vec_is_vectorizable(t[i]);
        if (is_array(t[i].kind) || is_slice(t[i].kind)) {
            if (!vec_check_operand(vm, f, at, t[i], vec)) return -1;
        }
        if (vec) {
            if (!vec_lane_source(vm, f, at, ir[i], t[i], &out[i], pre, n_pre, cap_pre)) return -1;
        } else {
            vec_scalar_lanes(vm, f, ir[i], &out[i], pre, n_pre, cap_pre);
        }
    }
    int n = -1, inner = 0, first = -1;
    for (int i = 0; i < n_ops; i++) {
        if (out[i].is_scalar) continue;
        if (n < 0) { n = out[i].n; inner = out[i].inner_len; first = i; continue; }
        if (out[i].n != n) {
            vm_errorf_at(vm, at, "arrays are [%d] and [%d]; elementwise operators need equal lengths",
                         n, out[i].n);
            return -1;
        }
        if (out[i].inner_len != inner) {
            vm_errorf_at(vm, at, "nested array shapes differ: rows are %d and %d wide",
                         inner, out[i].inner_len);
            return -1;
        }
    }
    (void)first;
    // Every operand broadcast: one lane, which is the scalar case and is the
    // caller's to reject if it wanted an aggregate.
    if (n < 0) n = 1;
    *out_inner = inner;
    return n;
}

static int compile_vec_binop(VM *vm, Func *f, ASTNode *node, int sub,
                             int l, VMType lt, int r, VMType rt, VMType *out_t) {
    if (vmt_is_struct(lt) || vmt_is_struct(rt)) {
        vm_set_error_at(vm, node, "operators do not apply to a struct; use its fields, as p.x");
        return -1;
    }
    int lv = vec_is_vectorizable(lt), rv = vec_is_vectorizable(rt);
    if (is_array(lt.kind) || is_slice(lt.kind)) {
        if (!vec_check_operand(vm, f, node, lt, lv)) return -1;
    }
    if (is_array(rt.kind) || is_slice(rt.kind)) {
        if (!vec_check_operand(vm, f, node, rt, rv)) return -1;
    }
    // Elementwise these produce a mask nobody asked for; reduced they have no
    // single defensible meaning.
    if (sub == OP_LT || sub == OP_LE || sub == OP_GT || sub == OP_GE) {
        vm_errorf_at(vm, node, "ordering comparisons have no meaning between arrays; compare element by element");
        return -1;
    }
    if (sub == OP_AND || sub == OP_OR) {
        vm_errorf_at(vm, node, "'&&' and '||' have no meaning between arrays");
        return -1;
    }

    int *pre = 0, n_pre = 0, cap_pre = 0;
    VecLanes ops[2];
    int      ir2[2] = { l, r };
    VMType   t2[2]  = { lt, rt };
    int inner = 0;
    int n = vec_lanes_n(vm, f, node, ir2, t2, 2, ops, &inner, &pre, &n_pre, &cap_pre);
    if (n < 0) return -1;
    VecLanes a = ops[0], b = ops[1];
    (void)lv; (void)rv;

    // One scalar binop per lane, each through the full type policy: fixed
    // point, #rewire, i64 widening and div-by-zero guarding are all inherited
    // because a lane is compiled by the same function that compiles `a + b`.
    int lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    for (int i = 0; i < n; i++) {
        int ai = a.is_scalar ? 0 : i, bi = b.is_scalar ? 0 : i;
        lane[i] = compile_binop_ir(vm, f, node, sub, a.ir[ai], a.t[ai],
                                   b.ir[bi], b.t[bi], &lane_t[i]);
        if (lane[i] < 0) return -1;
    }

    // `==` / `!=` reduce to one i32 -- that is what a reader means by `a == b`.
    if (sub == OP_EQ || sub == OP_NE) {
        int acc = lane[0];
        for (int i = 1; i < n; i++) {
            int rhs = lane[i];
            int j = ir_new(vm, f, IR_BINOP);
            f->nodes[j].sub_op = (sub == OP_EQ) ? OP_AND : OP_OR;
            f->nodes[j].a = acc; f->nodes[j].b = rhs;
            f->nodes[j].type.kind = VMT_I32;
            acc = j;
        }
        *out_t = f->nodes[acc].type;
        return acc;
    }

    return vec_materialise(vm, f, lane, lane_t, n, inner,
                           pre, n_pre, cap_pre, out_t);
}

// `-v`. The lanes are the operand's, one IR_UNOP each -- the same shape as the
// binary case with nothing on the other side, so it needs no lane matching.
static int compile_vec_unop(VM *vm, Func *f, ASTNode *node, int sub,
                            int e, VMType t, VMType *out_t) {
    if (vmt_is_struct(t)) {
        vm_set_error_at(vm, node, "operators do not apply to a struct; use its fields, as p.x");
        return -1;
    }
    if (!vec_check_operand(vm, f, node, t, vec_is_vectorizable(t))) return -1;
    int *pre = 0, n_pre = 0, cap_pre = 0;
    VecLanes a;
    if (!vec_lane_source(vm, f, node, e, t, &a, &pre, &n_pre, &cap_pre)) return -1;
    int lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    for (int i = 0; i < a.n; i++) {
        int u = ir_new(vm, f, IR_UNOP);
        f->nodes[u].sub_op = sub;
        f->nodes[u].a      = a.ir[i];
        f->nodes[u].type   = a.t[i];
        lane[i]   = u;
        lane_t[i] = a.t[i];
    }
    return vec_materialise(vm, f, lane, lane_t, a.n, a.inner_len,
                           pre, n_pre, cap_pre, out_t);
}

// `sin(v)`, `mix(a, b, t)`, `clamp(v, 0, 1)`: one native call per lane, each
// compiled by compile_native_call_ir, so a lane inherits the whole type policy
// -- the fx variant, the i32 inline expansions, #rewire, the constant fold --
// from the scalar call it would otherwise have been.
//
// Type dispatch lands on the same answer in every lane by construction: an
// aggregate's element types are unified by compile_array_literal and
// vec_materialise, so lane i and lane j of one operand always share a kind and
// a shift, and a broadcast operand is literally the same node each time.
static int compile_vec_call(VM *vm, Func *f, ASTNode *node, Func *callee,
                            int args_begin, const VMType *arg_types, int n_args,
                            VMType *out_t)
{
    // A call-shaped version of vec_check_operand's diagnosis: name the argument
    // and the function, which is what a reader needs here.
    for (int i = 0; i < n_args; i++) {
        if (!is_array(arg_types[i].kind) && !is_slice(arg_types[i].kind)) continue;
        if (vec_is_vectorizable(arg_types[i])) continue;
        const char *nm = intern_get_cstr(vm->intern, callee->name);
        if (is_byte_string(arg_types[i]))
            vm_errorf_at(vm, node, "argument %d of '%s' is a string; '%s' takes numbers", i + 1, nm, nm);
        else
            vm_errorf_at(vm, node, "argument %d of '%s' is a slice: its length is not known at compile time",
                         i + 1, nm);
        return -1;
    }

    int *pre = 0, n_pre = 0, cap_pre = 0, inner = 0;
    VecLanes ops[VM_MAX_PARAMS];
    int      arg_ir[VM_MAX_PARAMS];
    for (int i = 0; i < n_args; i++) arg_ir[i] = f->child_indices[args_begin + i];

    int n = vec_lanes_n(vm, f, node, arg_ir, arg_types, n_args, ops, &inner,
                        &pre, &n_pre, &cap_pre);
    if (n < 0) return -1;

    int    lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    for (int k = 0; k < n; k++) {
        // A fresh argument block per lane: insert_fx_convert rewrites entries in
        // place, so the lanes must not share one.
        int    ab = n_args > 0 ? ir_alloc_items(vm, f, n_args) : 0;
        VMType at[VM_MAX_PARAMS];
        for (int i = 0; i < n_args; i++) {
            int li = ops[i].is_scalar ? 0 : k;
            f->child_indices[ab + i] = ops[i].ir[li];
            at[i] = ops[i].t[li];
        }
        lane[k] = compile_native_call_ir(vm, f, node, callee, ab, at, n_args, &lane_t[k]);
        if (lane[k] < 0) return -1;
    }
    return vec_materialise(vm, f, lane, lane_t, n, inner, pre, n_pre, cap_pre, out_t);
}

// `v ** 2`: one compile_pow_ir per lane, so inline powers, the fixed-point
// widening rule and the pow/ipow choice are all the scalar ones.
static int compile_vec_pow(VM *vm, Func *f, ASTNode *node, int l, VMType lt,
                           int lit_exp, VMType *out_t)
{
    int *pre = 0, n_pre = 0, cap_pre = 0, inner = 0;
    VecLanes ops[1];
    int      ir1[1]; VMType t1[1];
    ir1[0] = l; t1[0] = lt;
    int n = vec_lanes_n(vm, f, node, ir1, t1, 1, ops, &inner, &pre, &n_pre, &cap_pre);
    if (n < 0) return -1;
    int    lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    for (int k = 0; k < n; k++) {
        lane[k] = compile_pow_ir(vm, f, node, ops[0].ir[k], ops[0].t[k], lit_exp, &lane_t[k]);
        if (lane[k] < 0) return -1;
    }
    return vec_materialise(vm, f, lane, lane_t, n, inner, pre, n_pre, cap_pre, out_t);
}

// `f32(v)` / `v as fx16` on an aggregate: one convert per lane, so a
// fixed-point source is RESCALED per element rather than reinterpreted -- the
// same rule the scalar cast follows. Without this the convert produced a
// scalar-typed node over array storage and the next `[i]` reported something
// baffling about indexing.
static int compile_vec_cast(VM *vm, Func *f, ASTNode *at, int e, VMType et,
                            VMType target, VMType *out_t)
{
    int *pre = 0, n_pre = 0, cap_pre = 0, inner = 0;
    VecLanes ops[1];
    int      ir1[1]; VMType t1[1];
    ir1[0] = e; t1[0] = et;
    int n = vec_lanes_n(vm, f, at, ir1, t1, 1, ops, &inner, &pre, &n_pre, &cap_pre);
    if (n < 0) return -1;
    int    lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    for (int k = 0; k < n; k++) {
        lane[k]   = insert_fx_convert(vm, f, ops[0].ir[k], ops[0].t[k], target);
        if (lane[k] < 0) return -1;
        lane_t[k] = target;
    }
    return vec_materialise(vm, f, lane, lane_t, n, inner, pre, n_pre, cap_pre, out_t);
}

// vec2/vec3/vec4 (f32 lanes) and ivec2/3/4 (i32 lanes), GLSL-style: the
// arguments' components are laid end to end -- vec4(v.xyz, 1.0) -- and a single
// scalar fills every lane. The result is the array literal [N]f32 / [N]i32,
// rewired like any declared f32 / i32.
static int compile_vec_ctor(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    VTKind ek; int want;
    const char *nm = intern_get_cstr(vm->intern, node->left->token);
    if (!parse_vec_type_name(nm, &ek, &want)) return -1;
    *handled = 1;
    VMType vt;
    if (!parse_type(vm, node->left, &vt, 1)) return -1;
    VMType et = { arr_elem(vt.kind), 0, 0 };
    et = ct_set_shift(et, vt.elem_shift);

    ASTNode *argv[VM_MAX_PARAMS];
    int n_args = expand_call_args(vm, node, node->right, argv, VM_MAX_PARAMS);
    if (n_args < 0) return -1;
    if (n_args == 0) {
        vm_errorf_at(vm, node, "%s needs %d components, got none", nm, want);
        return -1;
    }
    int *pre = 0, n_pre = 0, cap_pre = 0;
    int    lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    int n = 0;
    for (int i = 0; i < n_args; i++) {
        ASTNode *a = paren_inner(argv[i]);
        if (a && a->token == TOK_DOTDOTDOT && !a->left && a->right) a = a->right;
        VMType at;
        int e = compile_expr(vm, f, a, &at);
        if (e < 0) return -1;
        VecLanes l;
        if (is_array(at.kind) || is_slice(at.kind)) {
            if (vmt_is_struct(at) || at.inner_len || !vec_is_vectorizable(at)) {
                vm_errorf_at(vm, a, "argument %d of %s must be a number or an array of known length", i + 1, nm);
                return -1;
            }
            if (!vec_lane_source(vm, f, a, e, at, &l, &pre, &n_pre, &cap_pre)) return -1;
        } else if (n_args == 1) {
            vec_scalar_lanes(vm, f, e, &l, &pre, &n_pre, &cap_pre);
            // A fresh leaf per lane, so the literal stays a tree.
            for (int k = 1; k < want; k++) {
                int c = ir_new(vm, f, f->nodes[l.ir[0]].op);
                f->nodes[c] = f->nodes[l.ir[0]];
                l.ir[k] = c; l.t[k] = l.t[0];
            }
            l.n = want;
        } else {
            l.n = 1; l.ir[0] = e; l.t[0] = at;
        }
        for (int k = 0; k < l.n; k++) {
            if (n >= want) { n++; continue; }
            int v = l.ir[k];
            if (l.t[k].kind != et.kind || ct_shift(l.t[k]) != ct_shift(et))
                v = insert_fx_convert(vm, f, v, l.t[k], et);
            if (v < 0) return -1;
            lane[n] = v; lane_t[n] = et; n++;
        }
    }
    if (n != want) {
        vm_errorf_at(vm, node, "%s needs %d components, got %d", nm, want, n);
        return -1;
    }
    return vec_materialise(vm, f, lane, lane_t, n, 0, pre, n_pre, cap_pre, out_t);
}

// A range bound that folds to a constant: the expression is compiled and
// discarded, so a literal, a const and an auto-const all answer the same way.
static int map_const_bound(VM *vm, Func *f, ASTNode *n, long long *out) {
    VMType t;
    int e = compile_expr(vm, f, n, &t);
    if (e < 0) return 0;
    if (f->nodes[e].op != IR_CONST_I) return 0;
    *out = f->nodes[e].ki;
    return 1;
}

// map(xs, f) / map(a..b, f): one call of `f` per element, and the array of
// their values. A compiler intrinsic rather than a library function, for the
// reason auto-vec exists -- the result's length is the input's, which the
// language has no way to write down in a signature, but which the lane
// machinery already knows at compile time.
//
// `f` is an identifier bound to a function, or an inline `(x) => ...`, and it
// is bound HERE, at compile time: every lane is a direct call, so the emitted C
// has no function pointers and the reactive engine sees ordinary calls.
static int compile_map_intrinsic(VM *vm, Func *f, ASTNode *node, VMType *out_t, int *handled) {
    if (!is_ident(node->left) || !ident_is(vm, node->left, "map")) return -1;
    ASTNode *par = node->right;
    if (!par || call_argc(par) != 2) return -1;
    *handled = 1;

    if (!(vm->flags & VM_FLAG_AUTO_VEC)) {
        vm_errorf_at(vm, node, "map unrolls at compile time (#enable auto_vec)");
        return -1;
    }

    // ---- the function ----
    ASTNode *fn_ast = call_arg(par, 1);
    Func *callee = 0;
    if (fn_ast && fn_ast->token == TOK_ARROW_F) {
        callee = compile_func_def(vm, 0, fn_ast);
        if (!callee) return -1;
    } else if (is_ident(fn_ast)) {
        VMSym *fs = resolve_name(vm, f, fn_ast->token);
        if (fs && fs->is_func) callee = fs->fn;
    }
    if (!callee) {
        vm_set_error_at(vm, fn_ast ? fn_ast : node,
            "map's second argument is a function: map(xs, (x) => ...) or map(xs, name)");
        return -1;
    }
    if (callee->n_params - callee->n_defaults > 1 || callee->n_params < 1) {
        vm_errorf_at(vm, node, "map's function takes one element, but this one takes %d", callee->n_params);
        return -1;
    }

    int *pre = 0, n_pre = 0, cap_pre = 0;
    int    lane[VM_VEC_MAX_LANES];
    VMType lane_t[VM_VEC_MAX_LANES];
    int n = 0, inner = 0;

    // ---- the sequence: a range with constant bounds, or a sized aggregate ----
    ASTNode *seq = call_arg(par, 0);
    if (is_range_node(seq)) {
        long long lo, hi;
        if (!map_const_bound(vm, f, paren_inner(seq->left), &lo)
            || !map_const_bound(vm, f, paren_inner(seq->right), &hi)) {
            vm_set_error_at(vm, seq, "map over a range needs constant bounds; write a loop instead");
            return -1;
        }
        if (seq->token == TOK_DOTDOT_EQ) hi++;
        long long count = hi - lo;
        if (count < 0) count = 0;
        if (count > VM_VEC_MAX_LANES) {
            vm_errorf_at(vm, node, "map would emit %lld lanes (limit %d); write a loop",
                         count, VM_VEC_MAX_LANES);
            return -1;
        }
        n = (int)count;
        for (int i = 0; i < n; i++) {
            int e = ir_i32(vm, f, lo + i);
            int ab = ir_alloc_items(vm, f, 1);
            f->child_indices[ab] = e;
            VMType at0 = f->nodes[e].type;
            lane[i] = compile_user_call_ir(vm, f, node, callee, ab, &at0, 1, &lane_t[i]);
            if (lane[i] < 0) return -1;
        }
    } else {
        VMType st;
        int se = compile_expr(vm, f, seq, &st);
        if (se < 0) return -1;
        if (!vec_is_vectorizable(st)) {
            vm_set_error_at(vm, seq,
                "map needs an array whose length is known at compile time, or a constant range");
            return -1;
        }
        VecLanes src;
        int ir1[1]; VMType t1[1];
        ir1[0] = se; t1[0] = st;
        if (vec_lanes_n(vm, f, seq, ir1, t1, 1, &src, &inner, &pre, &n_pre, &cap_pre) < 0) return -1;
        // Over rows the element is a ROW: `inner` lanes of the flat block,
        // regathered into a row literal of their own.
        int step = inner ? inner : 1;
        n = src.n / step;
        for (int i = 0; i < n; i++) {
            int arg = src.ir[i];
            VMType at0 = src.t[i];
            if (inner) {
                arg = vec_materialise(vm, f, src.ir + i * inner, src.t + i * inner, inner, 0, 0, 0, 0, &at0);
                if (arg < 0) return -1;
            }
            int ab = ir_alloc_items(vm, f, 1);
            f->child_indices[ab] = arg;
            lane[i] = compile_user_call_ir(vm, f, node, callee, ab, &at0, 1, &lane_t[i]);
            if (lane[i] < 0) return -1;
        }
    }

    if (n == 0) {
        vm_set_error_at(vm, node, "map over an empty sequence has no type to give back");
        return -1;
    }
    // A function with no value (a draw) is run for its effect: the calls in
    // order, and map has no value either.
    int n_void = 0;
    for (int i = 0; i < n; i++) n_void += lane_t[i].kind == VMT_VOID;
    if (n_void) {
        if (n_void != n) {
            vm_set_error_at(vm, node, "map's function returns a value for some elements and none for others");
            return -1;
        }
        int citems = ir_alloc_items(vm, f, n_pre + n);
        for (int i = 0; i < n_pre; i++) f->child_indices[citems + i] = pre[i];
        for (int i = 0; i < n; i++) f->child_indices[citems + n_pre + i] = lane[i];
        int comma = ir_new(vm, f, IR_COMMA);
        f->nodes[comma].items_begin = citems;
        f->nodes[comma].n_items     = n_pre + n;
        f->nodes[comma].type.kind   = VMT_VOID;
        *out_t = f->nodes[comma].type;
        return comma;
    }
    // A function returning an array gives map a nested result: the lanes are
    // the rows, flattened, with the row width carried as inner_len -- the same
    // shape `[[1,2],[3,4]]` has.
    if (is_array(lane_t[0].kind) || is_slice(lane_t[0].kind)) {
        int row = lane_t[0].len;
        if (row <= 0) {
            vm_set_error_at(vm, node, "map's function returns a slice of unknown length");
            return -1;
        }
        if (n * row > VM_VEC_MAX_LANES) {
            vm_errorf_at(vm, node, "map would emit %d lanes (limit %d); write a loop",
                         n * row, VM_VEC_MAX_LANES);
            return -1;
        }
        int    flat[VM_VEC_MAX_LANES];
        VMType flat_t[VM_VEC_MAX_LANES];
        int fn_ = 0;
        for (int i = 0; i < n; i++) {
            VecLanes rl;
            int ir1[1]; VMType t1[1];
            int dummy = 0;
            ir1[0] = lane[i]; t1[0] = lane_t[i];
            if (lane_t[i].len != row) {
                vm_set_error_at(vm, node, "map's calls return different lengths");
                return -1;
            }
            if (vec_lanes_n(vm, f, node, ir1, t1, 1, &rl, &dummy, &pre, &n_pre, &cap_pre) < 0) return -1;
            for (int k = 0; k < rl.n; k++) { flat[fn_] = rl.ir[k]; flat_t[fn_] = rl.t[k]; fn_++; }
        }
        return vec_materialise(vm, f, flat, flat_t, fn_, row, pre, n_pre, cap_pre, out_t);
    }
    return vec_materialise(vm, f, lane, lane_t, n, 0, pre, n_pre, cap_pre, out_t);
}

static int compile_expr(VM *vm, Func *f, ASTNode *node, VMType *out_t) {
    if (!node) { vm_set_error_at(vm, 0, "null expr"); return -1; }
#if VM_MAX_COMPILE_STACK
    {
        uintptr_t here = (uintptr_t)&node, base = vm->compile_stack_base;
        if (base && (base > here ? base - here : here - base) > (uintptr_t)VM_MAX_COMPILE_STACK) {
            vm_set_error_at(vm, node, "nested too deeply to compile (expressions, blocks or calls)");
            return -1;
        }
    }
#endif
    node = paren_inner(node);
    vm->err_ctx = node;

    // A string-literal node carries an interned content id in ->string and satisfies
    // is_ident() below (token set, no children), so it must be detected first --
    // ->string is 0 for ordinary identifiers. A run of adjacent literals is one
    // string too, and has to be recognized ahead of the TOK_EMPTYSTRING call/index
    // branch that would otherwise claim it.
    if (is_string_literal(node) || is_string_concat(node))
        return compile_string_literal(vm, f, node, out_t);

    if (is_number(node)) return compile_number(vm, f, node, out_t);
    if (is_ident(node))  return compile_ident(vm, f, node, out_t);

    if (is_bracket(node))
        return compile_array_literal(vm, f, node, out_t);

    if (is_brace(node)) {
        vm_set_error_at(vm, node, "a { ... } literal needs a struct type here; declare a variable first, "
                                  "as p: T = { ... }");
        return -1;
    }

    // A spread is expanded by the argument list or the array literal that holds
    // it; anywhere else there is nothing for it to expand into.
    if (is_splat_node(node)) {
        vm_set_error_at(vm, node, "... can only spread an argument list or an array literal");
        return -1;
    }

    if (is_subscope(node)) {
        vm_set_error_at(vm, subscope_block(node), "a block is not a value here");
        return -1;
    }

    // Call/index: implicit application (TOK_EMPTYSTRING nodes always have left+right)
    if (node->token == TOK_EMPTYSTRING) {
        if (is_bracket(node->right))
            return compile_index(vm, f, node, out_t);
        return compile_call(vm, f, node, out_t);
    }

    // Field access: x.len
    if (node->token == TOK_DOT && node->left && node->right)
        return compile_field(vm, f, node, out_t);

    // Unary minus
    if (node->token == TOK_MINUS && !node->left && node->right) {
        VMType t;
        int e = compile_expr(vm, f, node->right, &t);
        if (e < 0 || reject_void(vm, node->right, t)) return -1;
        if (is_array(t.kind) || is_slice(t.kind))
            return compile_vec_unop(vm, f, node, OP_NEG, e, t, out_t);
        int u = ir_unop(vm, f, OP_NEG, e, t);
        *out_t = t;
        return u;
    }
    // `/x`, the reciprocal: `1 / x` by the binary operator's rules.
    if (node->token == TOK_SLASH && !node->left && node->right) {
        VMType t;
        int e = compile_expr(vm, f, node->right, &t);
        if (e < 0) return -1;
        VMType it = {0}; it.kind = VMT_I32;
        int one = ir_const_of(vm, f, it, 1, 1.0);
        if (is_array(t.kind) || is_slice(t.kind))
            return compile_vec_binop(vm, f, node, OP_DIV, one, it, e, t, out_t);
        return compile_binop_ir(vm, f, node, OP_DIV, one, it, e, t, out_t);
    }
    if (node->token == TOK_BANG && !node->left && node->right) {
        VMType t;
        int e = compile_expr(vm, f, node->right, &t);
        if (e < 0) return -1;
        // Elementwise it is a mask nobody asked for, and reduced it has no
        // single defensible meaning -- the same reason `&&` is rejected.
        if (is_array(t.kind) || is_slice(t.kind)) {
            vm_errorf_at(vm, node, "'!' has no meaning for an array");
            return -1;
        }
        e = emit_truthy(vm, f, e, t);
        if (e < 0) return -1;
        VMType nt = {0}; nt.kind = VMT_I32;
        int u = ir_unop(vm, f, OP_NOT, e, nt);
        *out_t = nt;
        return u;
    }

    // Ternary: a ? b : c  (parser gives ?(cond, :(true, false)))
    if (node->token == TOK_QMARK && node->left && node->right) {
        ASTNode *colon_node = node->right;
        if (colon_node->token != TOK_COLON || !colon_node->left || !colon_node->right) {
            vm_set_error_at(vm, node, "expected ':' in ternary expression"); return -1;
        }
        VMType ct;
        int cond = compile_expr(vm, f, node->left, &ct);
        if (cond < 0) return -1;
        cond = emit_truthy(vm, f, cond, ct);
        if (cond < 0) return -1;
        VMType tt, ft;
        int tv = compile_expr(vm, f, colon_node->left, &tt);
        if (tv < 0) return -1;
        int fv = compile_expr(vm, f, colon_node->right, &ft);
        if (fv < 0) return -1;
        // Aggregate arms.  promote() only knows the scalar kinds, so two arrays
        // (or two slices) fall through its final `else` to VMT_I32 and the
        // result silently loses its arrayness -- the first `r[i]` on it then
        // fails with "index target must be array or slice".  There is no
        // conversion to insert for an aggregate, so both arms have to agree
        // exactly and the type is carried through unchanged.
        if (is_array(tt.kind) || is_slice(tt.kind) || is_array(ft.kind) || is_slice(ft.kind)) {
            if (tt.kind != ft.kind || ct_shift(tt) != ct_shift(ft)
                || tt.pack_bits != ft.pack_bits
                || (is_array(tt.kind) && tt.len != ft.len)
                || !struct_types_same(vm, tt, ft)) {
                vm_set_error_at(vm, colon_node,
                    "ternary branches have different types; both must be the same array or slice type");
                return -1;
            }
            // An array literal has no storage of its own until it is assigned
            // (the interpreter rejects it outside an assign), so it cannot be
            // one arm of a select.
            if (is_staged_arr_lit(f, tv) || is_staged_arr_lit(f, fv)) {
                vm_set_error_at(vm, colon_node,
                    "vector expression here needs a temporary; assign it to a variable first");
                return -1;
            }
            if (f->nodes[tv].op == IR_ARR_LIT || f->nodes[fv].op == IR_ARR_LIT) {
                vm_set_error_at(vm, colon_node,
                    "array literal in a ternary branch; assign it to a variable first");
                return -1;
            }
            // The two arms agree on everything the check above enumerates, but
            // const is not one of those and either arm may carry it: the result
            // is a view onto whichever storage the condition picks, so it is
            // read-only if either could be.
            if (ft.is_const) tt.is_const = 1;
            int sel = ir_select(vm, f, cond, tv, fv, tt);
            *out_t = tt;
            return sel;
        }
        // promote_t so a rewired fxN branch type keeps its shift -- see the same
        // choice in compile_binop_ir.
        int ts = ct_shift(tt), fs = ct_shift(ft);
        VMType common = promote_t(vm, tt.kind, ft.kind);
        if (tt.kind == VMT_I32 && ft.kind == VMT_I32)
            common.len = ts > fs ? ts : fs;
        if (tt.kind != common.kind || ts != ct_shift(common))
            tv = insert_fx_convert(vm, f, tv, tt, common);
        if (ft.kind != common.kind || fs != ct_shift(common))
            fv = insert_fx_convert(vm, f, fv, ft, common);
        int ir = ir_select(vm, f, cond, tv, fv, common);
        *out_t = common;
        return ir;
    }

    // Rust-style cast: a as i32 / f32 / f64 / fxN
    if (node->token == TOK_AS && node->left && node->right) {
        ASTNode *ty = paren_inner(node->right);
        if (!is_ident(ty)) { vm_set_error_at(vm, node, "expected type after 'as'"); return -1; }
        // Try parse_type first (handles fxN, i32, f32, f64, i64). The type
        // rewire applies here just as it does to a declaration of the same
        // name -- see cast_target_type, which does this for the call spelling.
        VMType target;
        if (parse_type(vm, ty, &target, 1) && target.kind != VMT_VOID) {
            if (!is_scalar(target.kind)) {
                vm_errorf_at(vm, node->right, "cannot cast to '%s'; 'as' converts between number types",
                             intern_get_cstr(vm->intern, ty->token));
                return -1;
            }
            VMType at;
            int ae = compile_expr(vm, f, node->left, &at);
            if (ae < 0) return -1;
            if ((is_array(at.kind) || is_slice(at.kind)) && !vmt_is_struct(at)) {
                if (!(vm->flags & VM_FLAG_AUTO_VEC)) {
                    vm_errorf_at(vm, node, "elementwise operators are disabled here (#enable auto_vec)");
                    return -1;
                }
                return compile_vec_cast(vm, f, node, ae, at, target, out_t);
            }
            int cvt = insert_fx_convert(vm, f, ae, at, target);
            *out_t = target;
            return cvt;
        }
        // Fallback: resolve as registered cast function (legacy path)
        VMSym *s = resolve_name(vm, f, ty->token);
        int nt = (s && s->is_func && s->fn) ? s->fn->native_tok : 0;
        VTKind target_kind;
        if      (nt == TOK_I32) target_kind = VMT_I32;
        else if (nt == TOK_F32) target_kind = VMT_F32;
        else if (nt == TOK_F64) target_kind = VMT_F64;
        else if (nt == TOK_I64) target_kind = VMT_I64;
        else { vm_set_error_at(vm, node->right, "unknown cast target type"); return -1; }
        VMType at;
        int ae = compile_expr(vm, f, node->left, &at);
        if (ae < 0) return -1;
        int cvt = insert_cvt(vm, f, ae, at, target_kind);
        *out_t = f->nodes[cvt].type;
        return cvt;
    }

    // Exponentiation: base ** exp  (JS-style pow)
    if (node->token == TOK_STARSTAR && node->left && node->right)
        return compile_pow_op(vm, f, node, out_t);

    // Concatenation: "x = " ~ n.  Its own token rather than an overload of
    // `+`, so that `+` can mean elementwise addition on every array including
    // []u8 (see compile_vec_binop) and so that identity elimination never has
    // to guess whether an operand is a string.
    if (node->token == TOK_TILDE && node->left && node->right)
        return compile_concat_op(vm, f, node, out_t);

    // A range outside a `for ... in` header. Caught here rather than left to
    // fall through to the call path, which would report something unrelated.
    if ((node->token == TOK_DOTDOT || node->token == TOK_DOTDOT_EQ) && node->left && node->right) {
        vm_set_error_at(vm, node, "a range is only valid as 'for i in a..b'");
        return -1;
    }

    // Update operator used as a value: `x = a++`, `while(i++ < n)`, `a[i] += 1`
    // inside a larger expression.
    {
        ASTNode one;
        UpdateOp u = update_op_split(vm, node, &one);
        if (u.op) return compile_update_expr(vm, f, &u, out_t);
    }

    // Binary op
    if (node->token == TOK_URSHIFT && node->left && node->right)
        return compile_urshift(vm, f, node, out_t);
    int sub;
    if (node->left && node->right && op_from_token(node->token, &sub)) {
        // Identity elimination: x+0, 0+x, x-0, x*1, 1*x, x/1 all compile to
        // just the surviving operand, skipping the op (and its type
        // promotion/shift-alignment machinery) entirely.
        // Not `x/1` unless it truncates: an integer x has to come out f64.
        // The IR half (try_simplify_binop) still drops the op after the widen.
        if ((vm->flags & VM_FLAG_IDENTITY_ELIM)
            && (sub == OP_ADD || sub == OP_SUB || sub == OP_MUL
                || (sub == OP_DIV && (vm->flags & VM_FLAG_C_DIVISION)))) {
            double ident = (sub == OP_MUL || sub == OP_DIV) ? 1.0 : 0.0;
            if (is_identity_literal(vm, paren_inner(node->right), ident))
                return compile_expr(vm, f, node->left, out_t);
            if ((sub == OP_ADD || sub == OP_MUL)
                && is_identity_literal(vm, paren_inner(node->left), ident))
                return compile_expr(vm, f, node->right, out_t);
        }
        if (sub == OP_BSHL && (vm->flags & VM_FLAG_C_SHIFTS)) sub = OP_BSHL_NATIVE;
        VMType lt, rt;
        int l = compile_expr(vm, f, node->left, &lt);
        if (l < 0 || reject_void(vm, node->left, lt)) return -1;
        int r = compile_expr(vm, f, node->right, &rt);
        if (r < 0 || reject_void(vm, node->right, rt)) return -1;
        // Aggregate operands go elementwise. This sits
        // after both operands are compiled, so any array-valued expression --
        // a literal, a variable, the result of another vector op -- works.
        if (is_array(lt.kind) || is_slice(lt.kind)
            || is_array(rt.kind) || is_slice(rt.kind))
            return compile_vec_binop(vm, f, node, sub, l, lt, r, rt, out_t);
        return compile_binop_ir(vm, f, node, sub, l, lt, r, rt, out_t);
    }

    char desc[128];
    node_describe(vm, node, desc, sizeof(desc));
    vm_errorf_at(vm, node, "unsupported expression %s", desc);
    return -1;
}

static int vmt_is_raw_int(VMType t) {
    return (t.kind == VMT_I32 && ct_shift(t) == 0) || t.kind == VMT_I64;
}

static int vmt_is_float(VMType t) {
    return t.kind == VMT_F32 || t.kind == VMT_F64;
}

// The integer type `/~`, `/%` and `%%` work at when neither side is a float:
// i64 if either is, else i32 at the wider shift. Two words at one shift divide
// to the quotient of their values, and leave the remainder at that shift.
static VMType int_divmod_type(VMType lt, VMType rt) {
    VMType t = {0};
    if (lt.kind == VMT_I64 || rt.kind == VMT_I64) { t.kind = VMT_I64; return t; }
    int ls = ct_shift(lt), rs = ct_shift(rt);
    t.kind = VMT_I32;
    t.len = ls > rs ? ls : rs;
    return t;
}

// `m != 0 && (m < 0) != (b < 0)` as a 0/1 i32: a C remainder that has to move
// one divisor over to take the divisor's sign.
static int floor_adjust(VM *vm, Func *f, int m, VMType mt, int b, VMType bt) {
    VMType it = {0}; it.kind = VMT_I32;
    int nz = ir_binop(vm, f, OP_NE, m, ir_const_of(vm, f, mt, 0, 0.0), it);
    int mn = ir_binop(vm, f, OP_LT, m, ir_const_of(vm, f, mt, 0, 0.0), it);
    int bn = ir_binop(vm, f, OP_LT, b, ir_const_of(vm, f, bt, 0, 0.0), it);
    return ir_binop(vm, f, OP_BAND, nz, ir_binop(vm, f, OP_NE, mn, bn, it), it);
}

// `a /~ b` truncates toward zero, `a /% b` floors. Both answer an integer
// whatever the operands were -- i64 if either is -- so `x /~ w` indexes.
static int compile_int_div(VM *vm, Func *f, int sub, int l, VMType lt, int r, VMType rt, VMType *out_t) {
    int dv = (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE;
    VMType it = {0}, bt = {0};
    it.kind = (lt.kind == VMT_I64 || rt.kind == VMT_I64) ? VMT_I64 : VMT_I32;
    bt.kind = VMT_I32;
    int *items = 0, n_items = 0, cap_items = 0;
    int res;
    *out_t = it;
    if (vmt_is_float(lt) || vmt_is_float(rt)) {
        VMType ft = {0};
        ft.kind = (lt.kind == VMT_F64 || rt.kind == VMT_F64) ? VMT_F64 : VMT_F32;
        l = insert_fx_convert(vm, f, l, lt, ft);
        r = insert_fx_convert(vm, f, r, rt, ft);
        int q = ir_binop(vm, f, dv, l, r, ft);
        if (sub == SUBOP_TDIV) return insert_cvt(vm, f, q, ft, it.kind);
        // floor(q) = trunc(q) - (q < trunc(q))
        q = stage_operand(vm, f, q, &items, &n_items, &cap_items);
        int t = stage_operand(vm, f, insert_cvt(vm, f, q, ft, it.kind), &items, &n_items, &cap_items);
        int below = ir_binop(vm, f, OP_LT, q, insert_cvt(vm, f, t, it, ft.kind), bt);
        res = ir_binop(vm, f, OP_SUB, t, insert_cvt(vm, f, below, bt, it.kind), it);
    } else {
        VMType at = int_divmod_type(lt, rt);
        l = insert_fx_convert(vm, f, l, lt, at);
        r = insert_fx_convert(vm, f, r, rt, at);
        if (sub == SUBOP_TDIV) return ir_binop(vm, f, dv, l, r, it);
        l = stage_operand(vm, f, l, &items, &n_items, &cap_items);
        r = stage_operand(vm, f, r, &items, &n_items, &cap_items);
        int mv = (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_MOD : OP_MOD_NATIVE;
        int m = stage_operand(vm, f, ir_binop(vm, f, mv, l, r, at), &items, &n_items, &cap_items);
        int q = ir_binop(vm, f, dv, l, r, it);
        int adj = floor_adjust(vm, f, m, at, r, at);
        res = ir_binop(vm, f, OP_SUB, q, insert_cvt(vm, f, adj, bt, it.kind), it);
    }
    return inline_comma(vm, f, res, it, items, n_items);
}

// `a %% b`: the remainder that takes the divisor's sign, so that
// a == (a /% b)*b + a %% b the way a == (a /~ b)*b + a % b.
static int compile_floor_mod(VM *vm, Func *f, int l, VMType lt, int r, VMType rt, VMType *out_t) {
    int *items = 0, n_items = 0, cap_items = 0;
    VMType mt;
    int m;
    r = stage_operand(vm, f, r, &items, &n_items, &cap_items);
    if (vmt_is_float(lt) || vmt_is_float(rt)) {
        m = emit_native_binop_call(vm, f, TOK_FMOD, l, lt, r, rt, &mt);
    } else {
        mt = int_divmod_type(lt, rt);
        l = insert_fx_convert(vm, f, l, lt, mt);
        int mv = (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_MOD : OP_MOD_NATIVE;
        m = ir_binop(vm, f, mv, l, insert_fx_convert(vm, f, r, rt, mt), mt);
    }
    m = stage_operand(vm, f, m, &items, &n_items, &cap_items);
    int adj = floor_adjust(vm, f, m, mt, r, rt);
    int sum = ir_binop(vm, f, OP_ADD, m, insert_fx_convert(vm, f, r, rt, mt), mt);
    *out_t = mt;
    return inline_comma(vm, f, ir_select(vm, f, adj, sum, m, mt), mt, items, n_items);
}

// Everything from "which op, given these two already-compiled operands and their
// types" onward: string-free, AST-free type policy. `at` is only for error
// positions. Split out of compile_expr so vectorization (compile_vec_binop) can call
// it once per lane and inherit all of it -- #rewire, fixed-point shift alignment,
// the i64 widening paths, div-by-zero guarding, `^`-on-floats to pow, `%`-on-floats
// to fmod.
static int compile_binop_ir(VM *vm, Func *f, ASTNode *at, int sub,
                            int l, VMType lt, int r, VMType rt, VMType *out_t) {
    (void)at;
    if (sub == SUBOP_TDIV || sub == SUBOP_FLOORDIV)
        return compile_int_div(vm, f, sub, l, lt, r, rt, out_t);
    if (sub == SUBOP_FLOORMOD)
        return compile_floor_mod(vm, f, l, lt, r, rt, out_t);
    // `/` between two integers is a real division: 1/3 is not 0. Widened to what
    // f64 means here, so under `#rewire f64 -> fxN` it is fixed-point. `/~` is
    // the truncating form, or #enable c_division for C's rule.
    if (sub == OP_DIV && !(vm->flags & VM_FLAG_C_DIVISION) && vmt_is_raw_int(lt) && vmt_is_raw_int(rt)) {
        VMType wt = promote_t(vm, VMT_F64, VMT_F64);
        l = insert_fx_convert(vm, f, l, lt, wt);
        r = insert_fx_convert(vm, f, r, rt, wt);
        lt = wt;
        rt = wt;
    }
    int ls = ct_shift(lt), rs = ct_shift(rt);
    int is_float_operand = lt.kind == VMT_F32 || lt.kind == VMT_F64
                         || rt.kind == VMT_F32 || rt.kind == VMT_F64;
    // '^' on floats means exponentiation, not bitwise xor: emit powf/pow.
    if (sub == OP_BXOR && is_float_operand)
        return emit_native_binop_call(vm, f, TOK_POW, l, lt, r, rt, out_t);
    // Logical: C tests each operand against zero at its own width rather
    // than narrowing it, so `0.5 && 1` is true and so is a 64-bit value
    // whose low word happens to be 0.  The 0/1 answer is always i32.
    if (sub == OP_AND || sub == OP_OR) {
        l = emit_truthy(vm, f, l, lt);
        if (l < 0) return -1;
        r = emit_truthy(vm, f, r, rt);
        if (r < 0) return -1;
        VMType bt = {0}; bt.kind = VMT_I32;
        int b = ir_binop(vm, f, sub, l, r, bt);
        *out_t = bt;
        return b;
    }
    // Bitwise: both operands convert to an integer, and mixed widths meet
    // at i64.  Forcing raw i32 here instead would silently drop the high
    // word -- `(x as i64) << 40 & mask` came out 0.
    if (sub == OP_BAND || sub == OP_BOR || sub == OP_BXOR) {
        VMType it = { VMT_I32, 0 };
        if (ct_bitwise_int_kind(lt) == VMT_I64 || ct_bitwise_int_kind(rt) == VMT_I64)
            it.kind = VMT_I64;
        if (lt.kind != it.kind || ls != 0) l = insert_fx_convert(vm, f, l, lt, it);
        if (rt.kind != it.kind || rs != 0) r = insert_fx_convert(vm, f, r, rt, it);
        int b = ir_binop(vm, f, sub, l, r, it);
        *out_t = it;
        return b;
    }
    // '%' on floats means fmod, not truncated int remainder: emit fmodf/fmod.
    if (sub == OP_MOD && is_float_operand)
        return emit_native_binop_call(vm, f, TOK_FMOD, l, lt, r, rt, out_t);
    // Mod: fixed-point i32 operands align to a common shift (raw_a % raw_b
    // at equal shift equals the fixed-point remainder), instead of being
    // truncated to raw ints.  Non-fixed operands (float/i64) still force
    // raw i32, since the runtime MOD op only exists for i32/i64.
    if (sub == OP_MOD) {
        VMType mt = { VMT_VOID, 0, 0 };
        if (lt.kind == VMT_I32 && rt.kind == VMT_I32) {
            mt.kind = VMT_I32;
            mt.len = ls > rs ? ls : rs;
        } else {
            mt.kind = VMT_I32;
            mt.len = 0;
        }
        if (lt.kind != mt.kind || ls != mt.len) l = insert_fx_convert(vm, f, l, lt, mt);
        if (rt.kind != mt.kind || rs != mt.len) r = insert_fx_convert(vm, f, r, rt, mt);
        int b = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_MOD : OP_MOD_NATIVE,
                         l, r, mt);
        *out_t = mt;
        return b;
    }
    // Shift: the left operand keeps its width either way.  << also keeps
    // its fixed-point shift; >> drops it (the raw word is what shifts).
    // Width matters: forcing the result to i32 made `x >> n` on an i64
    // read only the low word, so `(1 as i64) << 40 >> 1` came out 0.
    // A non-integer left operand converts to an integer first, by the same
    // rule the bitwise ops use -- so the shift count never widens the
    // result, only the value being shifted does.
    if (sub == OP_BSHL || sub == OP_BSHL_NATIVE || sub == OP_BSHR) {
        VMType raw = { VMT_I32, 0 };
        if (rt.kind != VMT_I32 || rs != 0) r = insert_fx_convert(vm, f, r, rt, raw);
        VTKind lk = ct_bitwise_int_kind(lt);
        VMType res;
        if (lt.kind == lk) {
            res = (sub == OP_BSHR) ? ct_vmtype_clear_shift(lt) : lt;
        } else {
            res = raw; res.kind = lk;
            l = insert_fx_convert(vm, f, l, lt, res);
        }
        int b = ir_binop(vm, f, sub, l, r, res);
        *out_t = res;
        return b;
    }
    // Determine common type considering fixed-point shifts
    VMType common = { VMT_VOID, 0, 0 };
    int c_shift = 0;
    if (lt.kind == VMT_I32 && rt.kind == VMT_I32) {
        c_shift = ls > rs ? ls : rs;
        common.kind = VMT_I32;
        common.len = c_shift;
    } else {
        // promote_t, not promote: under `#rewire f64 -> fxN` the promoted type IS
        // fxN, and taking only its kind would make this a raw-int add with both
        // operands truncated on the way in.
        common = promote_t(vm, lt.kind, rt.kind);
        c_shift = ct_shift(common) > 0 ? ct_shift(common) : 0;
    }
    // Fixed-point MUL/DIV: handle rewired types before operand conversion.
    // Mixed raw-i32/fixed-point operands need no shift alignment at all --
    // the fixed side already carries the correct output shift.  Aligning
    // the raw side up to c_shift first (as the generic "convert to common
    // type" step below would) can overflow i32 long before an i64 widen
    // ever happens, so these cases are special-cased regardless of
    // whether i64 is available.
    int mixed = (ls == 0 && rs > 0) || (ls > 0 && rs == 0);
    if (common.kind == VMT_I32 && c_shift > 0 && sub == OP_MUL && mixed) {
        if (fx_wide_ok(vm)) {
            // Widen both (unshifted) operands to i64 before multiplying so
            // a large raw operand can't overflow i32 first.
            int l64 = insert_cvt(vm, f, l, ct_vmtype_clear_shift(lt), VMT_I64);
            int r64 = insert_cvt(vm, f, r, ct_vmtype_clear_shift(rt), VMT_I64);
            VMType m64t = {0}; m64t.kind = VMT_I64;
            int mul64 = ir_binop(vm, f, OP_MUL, l64, r64, m64t);
            *out_t = common;
            return insert_cvt_t(vm, f, mul64, m64t, common);
        }
        // No i64: keep operands as-is (no conversion).  Result naturally
        // has the fixed-point operand's shift (= c_shift).
        int b = ir_binop(vm, f, OP_MUL, l, r, common);
        *out_t = common;
        return b;
    }
    if (common.kind == VMT_I32 && c_shift > 0 && sub == OP_DIV && mixed) {
        if (rs == 0) {
            // Divisor is raw i32: plain div, result inherits dividend's
            // shift exactly -- no alignment needed, no overflow risk
            // beyond an ordinary i32 divide, regardless of i64 availability.
            int b = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE,
                             l, r, common);
            *out_t = common;
            return b;
        }
        // Dividend is raw i32, divisor is fixed-point (shift rs == c_shift).
        // out_raw = (l << (rs + os)) / r_raw at output shift os; the shift
        // amount must include rs (the divisor's own scale), not just os,
        // or the result is off by a factor of 2^rs.
        int os = c_shift >> 1;
        int shift_amt = rs + os;
        if (fx_wide_ok(vm)) {
            // Pre-shift the dividend in i64 so a large raw dividend can't
            // overflow i32 before the divide.
            VMType i64t = {VMT_I64, 0};
            int l64 = insert_cvt(vm, f, l, ct_vmtype_clear_shift(lt), VMT_I64);
            int r64 = insert_cvt(vm, f, r, ct_vmtype_clear_shift(rt), VMT_I64);
            int shc = ir_const_i(vm, f, i64t, shift_amt);
            int shl = ir_binop(vm, f, (vm->flags & VM_FLAG_C_SHIFTS) ? OP_BSHL_NATIVE : OP_BSHL,
                               l64, shc, i64t);
            int div64 = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE,
                                 shl, r64, i64t);
            VMType st = {VMT_I32, os};
            *out_t = st;
            return insert_cvt_t(vm, f, div64, i64t, st);
        }
        VMType st = {VMT_I32, os};
        int ci = ir_i32(vm, f, shift_amt);
        int sl = ir_binop(vm, f, OP_BSHL, l, ci, st);
        int b = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE,
                         sl, r, st);
        *out_t = st;
        return b;
    }
    if (common.kind == VMT_I32 && c_shift > 0 && sub == OP_DIV && !mixed) {
        if (!fx_wide_ok(vm)) {
            // Both fixed-point (before conversion): pre-shift dividend by
            // ((ls+rs)>>2), output shift compensates for diff in original shifts.
            int k = (ls + rs) >> 2;
            int out_shift = k + (ls - rs);
            VMType st = {VMT_I32, out_shift};
            int ci = ir_i32(vm, f, k);
            VMType slt = {0}; slt.kind = VMT_I32;
            int sl = ir_binop(vm, f, OP_BSHL, l, ci, slt);
            int b = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE,
                             sl, r, st);
            *out_t = st;
            return b;
        }
    }
    // Convert operands to common type
    if (lt.kind != common.kind || ls != c_shift)
        l = insert_fx_convert(vm, f, l, lt, common);
    if (rt.kind != common.kind || rs != c_shift)
        r = insert_fx_convert(vm, f, r, rt, common);
    if (op_is_compare(sub)) {
        VMType cmpt = {0}; cmpt.kind = VMT_I32;
        int b = ir_binop(vm, f, sub, l, r, cmpt);
        *out_t = cmpt;
        return b;
    }
    // Fixed-point MUL: i64 or pre-shift both.
    if (common.kind == VMT_I32 && c_shift > 0 && sub == OP_MUL) {
        if (!fx_wide_ok(vm)) {
            // Both fixed-point: pre-shift each operand by half the total
            // shift to avoid precision loss without using i64.
            int half = c_shift >> 1;
            int rest = c_shift - half;
            int ci_a = ir_i32(vm, f, half);
            VMType i32t = {0}; i32t.kind = VMT_I32;
            int sa = ir_binop(vm, f, OP_BSHR, l, ci_a, i32t);
            int ci_b = ir_i32(vm, f, rest);
            int sb = ir_binop(vm, f, OP_BSHR, r, ci_b, i32t);
            int b = ir_binop(vm, f, OP_MUL, sa, sb, common);
            *out_t = common;
            return b;
        }
        // i64 is available: widen to i64 for full-precision multiply.
        VMType i64t = {VMT_I64, 0};
        int l64 = insert_cvt(vm, f, l, ct_vmtype_clear_shift(common), VMT_I64);
        int r64 = insert_cvt(vm, f, r, ct_vmtype_clear_shift(common), VMT_I64);
        int mul64 = ir_binop(vm, f, OP_MUL, l64, r64, i64t);
        int shc = ir_const_i(vm, f, i64t, c_shift);
        int shr = ir_binop(vm, f, OP_BSHR, mul64, shc, i64t);
        *out_t = common;
        return insert_cvt_t(vm, f, shr, i64t, common);
    }
    if (common.kind == VMT_I32 && c_shift > 0 && sub == OP_DIV) {
        // i64 is available: widen to i64 for full-precision divide.
        VMType i64t = {VMT_I64, 0};
        int l64 = insert_cvt(vm, f, l, ct_vmtype_clear_shift(common), VMT_I64);
        int shc = ir_const_i(vm, f, i64t, c_shift);
        int shl = ir_binop(vm, f, (vm->flags & VM_FLAG_C_SHIFTS) ? OP_BSHL_NATIVE : OP_BSHL,
                           l64, shc, i64t);
        int r64 = insert_cvt(vm, f, r, ct_vmtype_clear_shift(common), VMT_I64);
        int div64 = ir_binop(vm, f, (vm->flags & VM_FLAG_CHECK_DIV_ZERO) ? OP_DIV : OP_DIV_NATIVE,
                             shl, r64, i64t);
        *out_t = common;
        return insert_cvt_t(vm, f, div64, i64t, common);
    }
    {
        int sub_op = sub;
        if (!(vm->flags & VM_FLAG_CHECK_DIV_ZERO)) {
            if (sub == OP_DIV) sub_op = OP_DIV_NATIVE;
        }
        int b = ir_binop(vm, f, sub_op, l, r, common);
        *out_t = common;
        return b;
    }
}


// ---------- statement compilation ----------

static void stmt_push_idx(VM *vm, int **slot, int *count, int *cap, int n) {
    MEM_GROW(vm, &vm->run.mem, *slot, *count, *cap, *count + 1, 4);
    (*slot)[(*count)++] = n;
}

// A one-line block must be opened by the word that goes with its head: `then`
// for `if`, `do` for the loops. The parser accepts either on anything, since it
// knows no keywords -- so the pairing is checked here, the layer that does.
// Braced and indented bodies carry neither opener and pass straight through.
static int check_block_opener(VM *vm, ASTNode *blk, InternID want, const char *head) {
    InternID got = blk ? blk->left_bracket : 0;
    if (got != TOK_THEN && got != TOK_DO) return 1;
    if (got == want) return 1;
    vm_errorf_at(vm, blk, "'%s' takes a '%s' body, not '%s'", head,
                 want == TOK_THEN ? "then" : "do",
                 got == TOK_THEN ? "then" : "do");
    return 0;
}

// Compile `if(cond) { body }` plus an optional else chain (the args of the
// `else` statement that followed it -- see find_else_head).  Returns the IR_IF
// index, or -1 on error.
static int compile_if(VM *vm, Func *f, ASTNode *node, ASTNode **args, int n,
                      ASTNode **else_args, int else_n) {
    if (n < 2) { vm_set_error_at(vm, node, "if requires (cond) { body }"); return -1; }
    if (!is_subscope(node))
        { vm_set_error_at(vm, node, "invalid syntax: use if(cond) {...} or if cond then stmt, not if (cond) {...}"); return -1; }
    if (!check_block_opener(vm, args[1], TOK_THEN, "if")) return -1;
    VMType ct;
    int cond = compile_expr(vm, f, args[0], &ct);
    if (cond < 0) return -1;
    cond = emit_truthy(vm, f, cond, ct);
    if (cond < 0) return -1;
    int thenb = compile_block(vm, f, args[1]);
    if (thenb < 0) return -1;
    int elseb = -1;
    if (else_n > 0) {
        elseb = compile_else_tail(vm, f, else_args, else_n);
        if (elseb < 0) return -1;
    }
    int ir = ir_new(vm, f, IR_IF);
    f->nodes[ir].a = cond; f->nodes[ir].b = thenb; f->nodes[ir].c = elseb;
    return ir;
}

// Compile an else chain into the IR_BLOCK that goes in IR_IF's else slot: for a
// plain `else` that is just its `{...}` body, for `else if(c) {...} [else ...]`
// it is a one-statement block holding the nested IR_IF.  Both backends and the
// interpreter require a block there, hence the wrapper.
static int compile_else_tail(VM *vm, Func *f, ASTNode **args, int n) {
    if (n <= 0) return -1;
    ASTNode *first = unwrap_chain(args[0]);
    ASTNode *kw_args[8];
    int kw_n = 0;
    ASTNode *head = first ? find_kw_head(vm, first, kw_args, &kw_n, 8) : 0;
    if (head && ident_is(vm, head, "if")) {
        // `else if(c) {...}`: the rest of the chain is that nested if's else.
        int inner = compile_if(vm, f, first, kw_args, kw_n, args + 1, n - 1);
        if (inner < 0) return -1;
        int blk = ir_new(vm, f, IR_BLOCK);
        int begin = ir_alloc_items(vm, f, 1);
        f->child_indices[begin] = inner;
        f->nodes[blk].items_begin = begin;
        f->nodes[blk].n_items = 1;
        return blk;
    }
    if (head && ident_is(vm, head, "else")) {
        // The `else` after an `else if` chains as an *argument* of the outer
        // `else` rather than as a sibling, so unwrap one level and retry.
        if (kw_n == 0) { vm_set_error_at(vm, head, "else requires a { body }"); return -1; }
        // Indentation splits the chain into siblings instead, so the rest of the
        // run arrives alongside this else rather than inside it. Splice it back
        // on so both forms reach the recursion the same way.
        if (n > 1) {
            ASTNode *merged[8];
            int m = 0;
            for (int k = 0; k < kw_n && m < 8; k++) merged[m++] = kw_args[k];
            for (int k = 1; k < n && m < 8; k++) merged[m++] = args[k];
            return compile_else_tail(vm, f, merged, m);
        }
        return compile_else_tail(vm, f, kw_args, kw_n);
    }
    if (n > 1) { vm_set_error_at(vm, args[1], "unexpected expression after else body"); return -1; }
    return compile_block(vm, f, args[0]);
}

// ---------- structs: declarations, literals, defaults ----------

// A name the C emitter would collide with: a type it spells itself, or the
// `T_slice` / `T_<N>` typedefs it generates.
static int struct_name_reserved(const char *s) {
    VTKind k;
    if (parse_base_type_name(s, &k) || parse_pack_width(s)) return 1;
    if (s[0] == 'f' && s[1] == 'x' && s[2] >= '0' && s[2] <= '9') return 1;
    static const char *const kw[] = {
        "void", "char", "short", "long", "signed", "unsigned", "struct", "union", "enum",
        "typedef", "const", "static", "extern", "register", "volatile", "inline", "sizeof",
        "return", "if", "else", "while", "for", "do", "switch", "case", "default", "break",
        "continue", "goto", "auto", "restrict", "ref", 0
    };
    for (int i = 0; kw[i]; i++) if (s_strcmp(s, kw[i]) == 0) return 1;
    int last = -1;
    for (int i = 0; s[i]; i++) if (s[i] == '_') last = i;
    if (last < 0) return 0;
    const char *tail = s + last + 1;
    if (s_strcmp(tail, "slice") == 0) return 1;
    if (!tail[0]) return 0;
    for (int i = 0; tail[i]; i++) if (tail[i] < '0' || tail[i] > '9') return 0;
    return 1;
}

// The type, width, size and alignment of one field, from its annotation.
static int struct_parse_field(VM *vm, ASTNode *tnode, const char *fname, VMStructField *fd, int *align) {
    VMType t = {0};
    fd->width = FIELD_NAT;
    fd->shift = 0;
    if (is_ident(tnode)) {
        const char *s = intern_get_cstr(vm->intern, tnode->token);
        int pw = s ? parse_pack_width(s) : 0;
        if (pw == 8 || pw == 16) {
            // A byte or a 16-bit word, zero-extended on the way out and truncated
            // on the way in. It reads as a plain i32 everywhere else.
            t.kind = VMT_I32;
            fd->type  = t;
            fd->width = pw == 8 ? FIELD_U8 : FIELD_U16;
            fd->size  = pw / 8;
            *align    = fd->size;
            return 1;
        }
        if (pw) {
            vm_errorf_at(vm, tnode, "field '%s' is u%d; a struct field needs a whole number of bytes (u8, u16)",
                         fname, pw);
            return 0;
        }
    }
    if (!parse_type(vm, tnode, &t, 1) || t.kind == VMT_VOID) {
        if (!vm_last_error(vm)) vm_errorf_at(vm, tnode, "bad type for field '%s'", fname);
        return 0;
    }
    if (is_slice(t.kind)) {
        vm_errorf_at(vm, tnode, "field '%s' is a %s; it has no storage of its own",
                     fname, t.struct_id ? "ref" : "slice");
        return 0;
    }
    if (is_scalar(t.kind)) {
        fd->type  = t;
        fd->shift = ct_shift(t);
        fd->size  = vt_size_of(t.kind);
        *align    = fd->size;
        return 1;
    }
    fd->width = FIELD_AGG;
    fd->type  = t;
    if (t.struct_id) {
        fd->size = t.len;
        *align   = struct_get(vm, t.struct_id)->align;
        return 1;
    }
    if (t.inner_len) {
        vm_errorf_at(vm, tnode, "field '%s' is a nested array; a struct field has one dimension, as [N*M]T",
                     fname);
        return 0;
    }
    if (t.pack_bits == 8)       { fd->size = t.len;     *align = 1; }
    else if (t.pack_bits == 16) { fd->size = t.len * 2; *align = 2; }
    else if (t.pack_bits)       { fd->size = pack_bytes_for(t.len, t.pack_bits); *align = 4; }
    else {
        int es = vt_size_of(arr_elem(t.kind));
        fd->size = t.len * es;
        *align   = es;
    }
    return 1;
}

// Lay the fields out under C rules and add the struct to the unit's table.
// Returns its id, or 0 having reported.
static int struct_add(VM *vm, InternID name, VMStructField *fields, int *aligns, int n, ASTNode *at) {
    const char *nm = name ? intern_get_cstr(vm->intern, name) : "{...}";
    if (n <= 0) {
        vm_errorf_at(vm, at, "struct '%s' has no fields", nm);
        return 0;
    }
    long long off = 0;
    int al = 1;
    for (int i = 0; i < n; i++) {
        int a = aligns[i] > 0 ? aligns[i] : 1;
        off = (off + a - 1) & ~(long long)(a - 1);
        fields[i].offset = (int)off;
        off += fields[i].size;
        if (a > al) al = a;
        if (off > VM_STRUCT_MAX_SIZE) break;
    }
    long long size = (off + al - 1) & ~(long long)(al - 1);
    if (size > VM_STRUCT_MAX_SIZE) {
        vm_errorf_at(vm, at, "struct '%s' is %lld bytes; the limit is %d", nm, size, VM_STRUCT_MAX_SIZE);
        return 0;
    }
    VMStructTable *tab = vm->structs;
    if (!MEM_GROW(vm, &vm->run.mem, tab->items, tab->count, tab->cap, tab->count + 1, 4)) {
        vm_set_error_at(vm, at, "out of memory declaring a struct"); return 0;
    }
    VMStruct *st = &tab->items[tab->count++];
    st->name     = name;
    st->fields   = fields;
    st->n_fields = n;
    st->size     = (int)size;
    st->align    = al;
    return tab->count;
}

// A field default is folded into every instance, so it follows the parameter
// default rule: constants only. An array field takes a [ ... ] of constants.
static int struct_default_ok(VM *vm, const VMStructField *fd, ASTNode *def) {
    def = paren_inner(def);
    if (fd->width != FIELD_AGG) return default_is_const(vm, def);
    if (fd->type.struct_id || !is_bracket(def)) return 0;
    for (size_t i = 0; i < def->items.size; i++)
        if (!default_is_const(vm, *(ASTNode**)array_get(&def->items, i))) return 0;
    return 1;
}

// `struct NAME { field: T [= default], ... }`, and `NAME = struct { ... }`.
// Declares a type, not storage: nothing is emitted.
static int compile_struct_decl(VM *vm, Func *f, ASTNode *name_node, ASTNode *brace, ASTNode *at) {
    if (!is_array(VMT_ARR_I32)) {
        vm_set_error_at(vm, at, "structs need array support");
        return -1;
    }
    if (!is_ident(name_node)) {
        vm_set_error_at(vm, at, "struct needs a name: struct NAME { field: T, ... }");
        return -1;
    }
    InternID nm = name_node->token;
    const char *nm_s = intern_get_cstr(vm->intern, nm);
    brace = paren_inner(unwrap_chain(brace));
    if (!is_brace(brace)) {
        vm_errorf_at(vm, at, "struct '%s' needs a body: struct %s { field: T, ... }", nm_s, nm_s);
        return -1;
    }
    if (struct_find(vm, nm)) {
        vm_errorf_at(vm, name_node, "struct '%s' is already declared", nm_s);
        return -1;
    }
    if (const_find(vm, nm) || define_find(vm, nm)) {
        vm_errorf_at(vm, name_node, "'%s' is already declared as a %s", nm_s,
                     define_find(vm, nm) ? "#define" : "constant");
        return -1;
    }
    if (resolve_name(vm, f, nm)) {
        vm_errorf_at(vm, name_node, "'%s' is already declared as a variable or function", nm_s);
        return -1;
    }
    if (struct_name_reserved(nm_s)) {
        vm_errorf_at(vm, name_node, "struct '%s' collides with a generated C type name", nm_s);
        return -1;
    }

    int n = (int)brace->items.size;
    VMStructField *fields = (VMStructField*)mem_alloc(&vm->run.mem, sizeof(VMStructField) * (size_t)(n > 0 ? n : 1));
    int *aligns = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!fields || !aligns) { vm_set_error_at(vm, at, "out of memory declaring a struct"); return -1; }
    vm->run.sys->memset(fields, 0, sizeof(VMStructField) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        ASTNode *it = paren_inner(unwrap_chain(*(ASTNode**)array_get(&brace->items, i)));
        ASTNode *def = 0;
        ASTNode *decl = param_split_default(it, &def);
        decl = paren_inner(decl);
        if (!decl || decl->token != TOK_COLON || !is_ident(decl->left) || !decl->right) {
            vm_errorf_at(vm, it, "struct '%s': each field is written name: T", nm_s);
            return -1;
        }
        const char *fname = intern_get_cstr(vm->intern, decl->left->token);
        for (int k = 0; k < i; k++)
            if (fields[k].name == decl->left->token) {
                vm_errorf_at(vm, decl->left, "duplicate field '%s' in struct '%s'", fname, nm_s);
                return -1;
            }
        fields[i].name = decl->left->token;
        if (!struct_parse_field(vm, decl->right, fname, &fields[i], &aligns[i])) return -1;
        if (def) {
            if (!struct_default_ok(vm, &fields[i], def)) {
                vm_errorf_at(vm, def, fields[i].type.struct_id
                    ? "field '%s' is a struct and cannot have a default; give its fields defaults instead"
                    : "default for field '%s' must be a constant expression", fname);
                return -1;
            }
            fields[i].def = def;
        }
    }
    return struct_add(vm, nm, fields, aligns, n, at) ? 1 : -1;
}

// Where a struct literal writes. A chain of these rebuilds the destination's IR
// once per field, so no record-valued node is ever shared between two parents.
typedef struct StructDest {
    int kind;
    int slot;                         // SD_SLOT: the record or ref local; SD_ELEM_SLOT: the index local
    ASTNode *ast;                     // SD_AST: a side-effect-free lvalue, compiled afresh per use
    const struct StructDest *parent;  // SD_FIELD, SD_ELEM_*
    int off;                          // SD_FIELD: byte offset; SD_ELEM_CONST: the index
    int count;                        // SD_FIELD: byte count
    int stride;                       // SD_ELEM_*: the record size
    VMType type;                      // what the destination yields
} StructDest;

enum { SD_SLOT, SD_AST, SD_FIELD, SD_ELEM_CONST, SD_ELEM_SLOT };

static int sd_node(VM *vm, Func *f, const StructDest *d) {
    switch (d->kind) {
        case SD_SLOT: return sym_read(vm, f, d->slot);
        case SD_AST: { VMType t; return compile_expr(vm, f, d->ast, &t); }
        case SD_FIELD: {
            int base = sd_node(vm, f, d->parent);
            if (base < 0) return -1;
            int c = ir_i32(vm, f, d->count);
            return ir_field(vm, f, base, field_agg_sub(d->type.pack_bits), d->off, c, d->type);
        }
        default: {
            int base = sd_node(vm, f, d->parent);
            if (base < 0) return -1;
            int idx = d->kind == SD_ELEM_CONST ? ir_i32(vm, f, d->off) : sym_read(vm, f, d->slot);
            return ir_field(vm, f, base, FIELD_ELEM, d->stride, idx, d->type);
        }
    }
}

static void push_assign(VM *vm, Func *f, int lv, int rv, int **slot, int *count, int *cap) {
    int ir = ir_new(vm, f, IR_ASSIGN);
    f->nodes[ir].a = lv;
    f->nodes[ir].b = rv;
    stmt_push_idx(vm, slot, count, cap, ir);
}

static int ir_block_of(VM *vm, Func *f, int *items, int n) {
    int blk = ir_new(vm, f, IR_BLOCK);
    if (n > 0) {
        int b = ir_alloc_items(vm, f, n);
        for (int i = 0; i < n; i++) f->child_indices[b + i] = items[i];
        f->nodes[blk].items_begin = b;
    }
    f->nodes[blk].n_items = n;
    return blk;
}

// Does filling an instance with nothing given write anything? It does when some
// field, or some field of a nested record, has a default.
static int struct_has_defaults(VM *vm, int id) {
    const VMStruct *st = struct_get(vm, id);
    for (int i = 0; st && i < st->n_fields; i++) {
        if (st->fields[i].def) return 1;
        if (st->fields[i].type.struct_id && struct_has_defaults(vm, st->fields[i].type.struct_id)) return 1;
    }
    return 0;
}

// A record-valued expression, as the value of `what`: a ref is dereferenced, and
// the struct has to be the one wanted.
static int struct_record_value(VM *vm, Func *f, ASTNode *val, int want_id, const char *what) {
    VMType rt;
    int r = compile_expr(vm, f, val, &rt);
    if (r < 0) return -1;
    r = ref_to_value(vm, f, r, &rt);
    if (!vmt_is_record(rt) || !struct_ids_match(vm->structs, want_id, vm->structs, rt.struct_id)) {
        char desc[128];
        node_describe(vm, val, desc, sizeof(desc));
        vm_errorf_at(vm, val, "%s is a '%s'; got %s", what, struct_name_s(vm, want_id), desc);
        return -1;
    }
    return r;
}

static int struct_scalar_value(VM *vm, Func *f, ASTNode *val, VMType want, const char *fname) {
    ASTNode *v = paren_inner(val);
    if (is_brace(v) || is_bracket(v)) {
        vm_errorf_at(vm, val, "field '%s' is a number, not a %s", fname, is_brace(v) ? "struct" : "array");
        return -1;
    }
    VMType rt;
    int r = compile_expr(vm, f, val, &rt);
    if (r < 0) return -1;
    if (!is_scalar(rt.kind)) {
        char desc[128];
        node_describe(vm, val, desc, sizeof(desc));
        vm_errorf_at(vm, val, "field '%s' is a number; got %s", fname, desc);
        return -1;
    }
    if (rt.kind != want.kind || ct_shift(rt) != ct_shift(want))
        r = insert_fx_convert(vm, f, r, rt, want);
    return r;
}

// The value of an array field: a [ ... ] literal is converted element by element
// to the field's type, any other array has to match it exactly.
static int struct_array_value(VM *vm, Func *f, ASTNode *val, VMType want, const char *fname) {
    VMType rt;
    int r = compile_expr(vm, f, val, &rt);
    if (r < 0) return -1;
    char hb[24], wb[24];
    if (f->nodes[r].op == IR_ARR_LIT && !rt.struct_id && !rt.inner_len) {
        if (f->nodes[r].n_items != want.len) {
            vm_errorf_at(vm, val, "field '%s' is %s but given %d elements",
                         fname, type_label(want, wb, sizeof(wb)), f->nodes[r].n_items);
            return -1;
        }
        VMType et = {0};
        et.kind = arr_elem(want.kind);
        if (!want.pack_bits) et = ct_set_shift(et, want.elem_shift);
        for (int i = 0; i < want.len; i++) {
            int ei = f->child_indices[f->nodes[r].items_begin + i];
            VMType have = f->nodes[ei].type;
            if (have.kind != et.kind || ct_shift(have) != ct_shift(et)) {
                int cv = insert_fx_convert(vm, f, ei, have, et);
                f->child_indices[f->nodes[r].items_begin + i] = cv;
            }
        }
        f->nodes[r].type = want;
        return r;
    }
    if (!is_array(rt.kind) || rt.struct_id || arr_elem(rt.kind) != arr_elem(want.kind)
        || rt.len != want.len || rt.pack_bits != want.pack_bits || rt.inner_len != want.inner_len) {
        vm_errorf_at(vm, val, "field '%s' is %s; got %s", fname,
                     type_label(want, wb, sizeof(wb)), type_label(rt, hb, sizeof(hb)));
        return -1;
    }
    return r;
}

static int struct_fill(VM *vm, Func *f, const StructDest *d, int id, ASTNode *brace,
                       int zero_omitted, ASTNode *at, int **slot, int *count, int *cap);
static int struct_fill_array(VM *vm, Func *f, const StructDest *arr, int id, int n,
                             ASTNode *bracket, int zero_omitted, ASTNode *at,
                             int **slot, int *count, int *cap);

// Write field `fi` of the record at `d`. `val` is the value's AST, or 0: then a
// nested record still gets its own defaults, and with `zero_omitted` everything
// else gets zero.
static int struct_fill_field(VM *vm, Func *f, const StructDest *d, int id, int fi, ASTNode *val,
                             int zero_omitted, ASTNode *at, int **slot, int *count, int *cap) {
    VMStructField fd = struct_get(vm, id)->fields[fi];
    const char *fname = intern_get_cstr(vm->intern, fd.name);
    if (fd.width != FIELD_AGG) {
        int r;
        if (val) {
            r = struct_scalar_value(vm, f, val, fd.type, fname);
        } else {
            r = ir_new(vm, f, (fd.type.kind == VMT_F32 || fd.type.kind == VMT_F64) ? IR_CONST_F : IR_CONST_I);
            f->nodes[r].type = fd.type;
        }
        if (r < 0) return -1;
        int base = sd_node(vm, f, d);
        if (base < 0) return -1;
        int lv = ir_field(vm, f, base, fd.width, fd.offset, -1, fd.type);
        push_assign(vm, f, lv, r, slot, count, cap);
        return 1;
    }
    StructDest child;
    vm->run.sys->memset(&child, 0, sizeof(child));
    child.kind   = SD_FIELD;
    child.parent = d;
    child.off    = fd.offset;
    child.count  = fd.type.len;
    child.type   = fd.type;
    ASTNode *v = val ? paren_inner(val) : 0;
    if (vmt_is_record(fd.type) && (!v || is_brace(v)))
        return struct_fill(vm, f, &child, fd.type.struct_id, v, zero_omitted, at, slot, count, cap);
    if (vmt_is_record_array(fd.type) && (!v || is_bracket(v)))
        return struct_fill_array(vm, f, &child, fd.type.struct_id, fd.type.len / fd.type.inner_len,
                                 v, zero_omitted, at, slot, count, cap);
    int r;
    if (vmt_is_record(fd.type)) {
        r = struct_record_value(vm, f, val, fd.type.struct_id, "this field");
    } else if (vmt_is_record_array(fd.type)) {
        VMType rt;
        r = compile_expr(vm, f, val, &rt);
        if (r >= 0 && !struct_types_same(vm, rt, fd.type)) {
            vm_errorf_at(vm, val, "field '%s' is [%d]%s; got a different array", fname,
                         fd.type.len / fd.type.inner_len, struct_name_s(vm, fd.type.struct_id));
            return -1;
        }
    } else if (val) {
        r = struct_array_value(vm, f, val, fd.type, fname);
    } else {
        // Zero every element, which for an array field means a literal of zeros.
        if (fd.type.len > 1024) {
            vm_errorf_at(vm, at, "field '%s' has %d elements to clear; assign it explicitly", fname, fd.type.len);
            return -1;
        }
        r = ir_new(vm, f, IR_ARR_LIT);
        int b = ir_alloc_items(vm, f, fd.type.len);
        VMType et = {0};
        et.kind = arr_elem(fd.type.kind);
        for (int i = 0; i < fd.type.len; i++) {
            int z = ir_new(vm, f, (et.kind == VMT_F32 || et.kind == VMT_F64) ? IR_CONST_F : IR_CONST_I);
            f->nodes[z].type = et;
            f->child_indices[b + i] = z;
        }
        f->nodes[r].items_begin = b;
        f->nodes[r].n_items     = fd.type.len;
        f->nodes[r].type        = fd.type;
    }
    if (r < 0) return -1;
    int lv = sd_node(vm, f, &child);
    if (lv < 0) return -1;
    push_assign(vm, f, lv, r, slot, count, cap);
    return 1;
}

// Fill the record at `d` from a `{ field: value, ... }` literal (or from nothing):
// given fields get their value, the rest their default, and with `zero_omitted`
// zero otherwise -- a re-assignment has to clear what a fresh frame would.
static int struct_fill(VM *vm, Func *f, const StructDest *d, int id, ASTNode *brace,
                       int zero_omitted, ASTNode *at, int **slot, int *count, int *cap) {
    const VMStruct *st = struct_get(vm, id);
    int nf = st->n_fields;
    ASTNode **given = (ASTNode**)mem_alloc(&vm->run.mem, sizeof(ASTNode*) * (size_t)nf);
    if (!given) { vm_set_error_at(vm, at, "out of memory"); return -1; }
    for (int i = 0; i < nf; i++) given[i] = 0;
    if (brace) {
        for (size_t i = 0; i < brace->items.size; i++) {
            ASTNode *it = paren_inner(unwrap_chain(*(ASTNode**)array_get(&brace->items, i)));
            if (it && it->token == TOK_EQ && it->left && paren_inner(it->left)->token == TOK_COLON) {
                vm_errorf_at(vm, it, "a '%s' literal takes its field types from the struct; write name: value",
                             struct_name_s(vm, id));
                return -1;
            }
            if (!it || it->token != TOK_COLON || !is_ident(it->left) || !it->right) {
                vm_errorf_at(vm, it ? it : brace, "a '%s' literal is written { field: value, ... }",
                             struct_name_s(vm, id));
                return -1;
            }
            int fi = struct_field_index(st, it->left->token);
            if (fi < 0) {
                char list[160];
                vm_errorf_at(vm, it->left, "struct '%s' has no field '%s' (fields: %s)",
                             struct_name_s(vm, id), intern_get_cstr(vm->intern, it->left->token),
                             struct_field_list(vm, st, list, sizeof(list)));
                return -1;
            }
            if (given[fi]) {
                vm_errorf_at(vm, it->left, "field '%s' is given twice",
                             intern_get_cstr(vm->intern, it->left->token));
                return -1;
            }
            given[fi] = it->right;
        }
    }
    for (int i = 0; i < nf; i++) {
        const VMStructField *fd = &struct_get(vm, id)->fields[i];
        ASTNode *val = given[i] ? given[i] : fd->def;
        if (!val && !zero_omitted && !fd->type.struct_id) continue;
        if (struct_fill_field(vm, f, d, id, i, val, zero_omitted, at, slot, count, cap) < 0) return -1;
    }
    return 1;
}

// Fill an array of `n` records at `arr`: from a `[ {...}, ... ]` literal, one element
// per item, or -- with no literal -- by looping over every element with the same
// fill an omitted instance gets. The loop is skipped when that fill writes nothing.
static int struct_fill_array(VM *vm, Func *f, const StructDest *arr, int id, int n,
                             ASTNode *bracket, int zero_omitted, ASTNode *at,
                             int **slot, int *count, int *cap) {
    int stride = struct_get(vm, id)->size;
    VMType rec = struct_record_type(vm, id);
    StructDest el;
    vm->run.sys->memset(&el, 0, sizeof(el));
    el.parent = arr;
    el.stride = stride;
    el.type   = rec;
    if (bracket) {
        if ((int)bracket->items.size != n) {
            vm_errorf_at(vm, bracket, "an array of %d '%s' is initialised with %d elements",
                         n, struct_name_s(vm, id), (int)bracket->items.size);
            return -1;
        }
        el.kind = SD_ELEM_CONST;
        for (int k = 0; k < n; k++) {
            ASTNode *item = paren_inner(unwrap_chain(*(ASTNode**)array_get(&bracket->items, k)));
            el.off = k;
            if (is_brace(item)) {
                if (struct_fill(vm, f, &el, id, item, zero_omitted, at, slot, count, cap) < 0) return -1;
                continue;
            }
            int r = struct_record_value(vm, f, item, id, "each element");
            if (r < 0) return -1;
            int lv = sd_node(vm, f, &el);
            if (lv < 0) return -1;
            push_assign(vm, f, lv, r, slot, count, cap);
        }
        return 1;
    }
    if (!zero_omitted && !struct_has_defaults(vm, id)) return 1;

    //   __k = 0
    //   for (; __k < n; __k = __k + 1) { <fill arr[__k]> }
    VMType i32t = {0};
    i32t.kind = VMT_I32;
    VMSym *ks = sym_add(vm, f, 0, i32t);
    ks->used = 1;
    int kslot = (int)(ks - f->syms);
    push_assign(vm, f, sym_read(vm, f, kslot), ir_i32(vm, f, 0), slot, count, cap);
    int cond = ir_binop(vm, f, OP_LT, sym_read(vm, f, kslot), ir_i32(vm, f, n), i32t);

    int *bitems = 0, bn = 0, bcap = 0;
    el.kind = SD_ELEM_SLOT;
    el.slot = kslot;
    if (struct_fill(vm, f, &el, id, 0, zero_omitted, at, &bitems, &bn, &bcap) < 0) return -1;
    int body = ir_block_of(vm, f, bitems, bn);

    int add = ir_binop(vm, f, OP_ADD, sym_read(vm, f, kslot), ir_i32(vm, f, 1), i32t);
    int *sitems = 0, sn = 0, scap = 0;
    push_assign(vm, f, sym_read(vm, f, kslot), add, &sitems, &sn, &scap);
    int step = ir_block_of(vm, f, sitems, sn);

    int loop = ir_new(vm, f, IR_FOR);
    f->nodes[loop].a = cond;
    f->nodes[loop].b = body;
    f->nodes[loop].c = step;
    stmt_push_idx(vm, slot, count, cap, loop);
    return 1;
}

// `p: my`, `p: my = {...}`, `p: my = q`, `arr: [N]my`, `arr: [N]my = [{...}, ...]`.
// One sym either way; a missing initialiser still writes the defaults.
static int compile_struct_instance_decl(VM *vm, Func *f, ASTNode *colon, VMType declared,
                                        ASTNode *init, int **slot, int *count, int *cap) {
    InternID nm = colon->left->token;
    const char *nm_s = intern_get_cstr(vm->intern, nm);
    int id = declared.struct_id;
    if (vmt_is_ref(declared) || vmt_is_struct_slice(declared)) {
        vm_errorf_at(vm, colon, "'%s': a %s local is not supported; take it as a parameter",
                     nm_s, vmt_is_ref(declared) ? "ref" : "[]struct");
        return -1;
    }
    VMSym *s = sym_add(vm, f, nm, declared);
    StructDest d;
    vm->run.sys->memset(&d, 0, sizeof(d));
    d.kind = SD_SLOT;
    d.slot = (int)(s - f->syms);
    d.type = declared;
    ASTNode *in = init ? paren_inner(init) : 0;
    if (vmt_is_record(declared)) {
        if (!in || is_brace(in)) return struct_fill(vm, f, &d, id, in, 0, colon, slot, count, cap);
        int r = struct_record_value(vm, f, init, id, nm_s);
        if (r < 0) return -1;
        push_assign(vm, f, sym_read(vm, f, d.slot), r, slot, count, cap);
        return 1;
    }
    if (!in || is_bracket(in))
        return struct_fill_array(vm, f, &d, id, declared.len / declared.inner_len, in, 0, colon,
                                 slot, count, cap);
    VMType rt;
    int r = compile_expr(vm, f, init, &rt);
    if (r < 0) return -1;
    if (!struct_types_same(vm, rt, declared)) {
        vm_errorf_at(vm, init, "'%s' is [%d]%s; initialise it with [ {...}, ... ] or an array of the same type",
                     nm_s, declared.len / declared.inner_len, struct_name_s(vm, id));
        return -1;
    }
    push_assign(vm, f, sym_read(vm, f, d.slot), r, slot, count, cap);
    return 1;
}

// `p = { a: 1, b: 2.5 }` where `p` is new: the struct is inferred from the values,
// or from an annotation on an item (`a: i32 = 1`). Its fields are then written like
// any declared struct's.
static int compile_inferred_struct(VM *vm, Func *f, ASTNode *name_node, ASTNode *brace,
                                   int **slot, int *count, int *cap) {
    int n = (int)brace->items.size;
    if (n == 0) {
        vm_set_error_at(vm, brace, "an empty { } has no fields to infer; declare a struct");
        return -1;
    }
    VMStructField *fields = (VMStructField*)mem_alloc(&vm->run.mem, sizeof(VMStructField) * (size_t)n);
    int *aligns = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)n);
    int *vals   = (int*)mem_alloc(&vm->run.mem, sizeof(int) * (size_t)n);
    ASTNode **typed = (ASTNode**)mem_alloc(&vm->run.mem, sizeof(ASTNode*) * (size_t)n);
    if (!fields || !aligns || !vals || !typed) { vm_set_error_at(vm, brace, "out of memory"); return -1; }
    vm->run.sys->memset(fields, 0, sizeof(VMStructField) * (size_t)n);
    for (int i = 0; i < n; i++) {
        ASTNode *it = paren_inner(unwrap_chain(*(ASTNode**)array_get(&brace->items, i)));
        ASTNode *key = 0, *val = 0, *tnode = 0;
        if (it && it->token == TOK_EQ && it->left && it->right && paren_inner(it->left)->token == TOK_COLON) {
            key = paren_inner(it->left)->left;
            tnode = paren_inner(it->left)->right;
            val = it->right;
        } else if (it && it->token == TOK_COLON && it->right) {
            key = it->left;
            val = it->right;
        }
        if (!is_ident(key)) {
            vm_set_error_at(vm, it ? it : brace, "a { ... } literal is written { name: value, ... }");
            return -1;
        }
        const char *fname = intern_get_cstr(vm->intern, key->token);
        for (int k = 0; k < i; k++)
            if (fields[k].name == key->token) {
                vm_errorf_at(vm, key, "field '%s' is given twice", fname);
                return -1;
            }
        fields[i].name = key->token;
        typed[i] = tnode ? val : 0;
        vals[i]  = -1;
        if (tnode) {
            if (!struct_parse_field(vm, tnode, fname, &fields[i], &aligns[i])) return -1;
            continue;
        }
        ASTNode *v = paren_inner(val);
        if (is_string_literal(v) || is_string_concat(v)) {
            vm_errorf_at(vm, val, "field '%s' is a string; declare it, as %s: [16]u8", fname, fname);
            return -1;
        }
        if (is_brace(v)) {
            vm_errorf_at(vm, val, "a nested { } has no type to infer; declare the inner struct and write %s: T = {...}",
                         fname);
            return -1;
        }
        VMType rt;
        int r = compile_expr(vm, f, val, &rt);
        if (r < 0) return -1;
        r = ref_to_value(vm, f, r, &rt);
        vals[i] = r;
        if (is_byte_string(rt) && !is_array(rt.kind)) {
            vm_errorf_at(vm, val, "field '%s' is a string; declare it, as %s: [16]u8", fname, fname);
            return -1;
        }
        if (is_slice(rt.kind) || rt.kind == VMT_VOID || rt.inner_len && !rt.struct_id) {
            vm_errorf_at(vm, val, "field '%s' cannot hold this value; a field is a number, an array or a struct",
                         fname);
            return -1;
        }
        fields[i].type = rt;
        if (is_scalar(rt.kind)) {
            fields[i].width = FIELD_NAT;
            fields[i].shift = ct_shift(rt);
            fields[i].size  = vt_size_of(rt.kind);
            aligns[i]       = fields[i].size;
            continue;
        }
        fields[i].width = FIELD_AGG;
        if (rt.struct_id) {
            fields[i].size = rt.len;
            aligns[i] = struct_get(vm, rt.struct_id)->align;
        } else if (rt.pack_bits == 8)  { fields[i].size = rt.len;     aligns[i] = 1; }
        else if (rt.pack_bits == 16)   { fields[i].size = rt.len * 2; aligns[i] = 2; }
        else if (rt.pack_bits)         { fields[i].size = pack_bytes_for(rt.len, rt.pack_bits); aligns[i] = 4; }
        else {
            int es = vt_size_of(arr_elem(rt.kind));
            fields[i].size = rt.len * es;
            aligns[i] = es;
        }
    }
    int id = struct_add(vm, 0, fields, aligns, n, brace);
    if (!id) return -1;
    VMSym *s = sym_add(vm, f, name_node->token, struct_record_type(vm, id));
    StructDest d;
    vm->run.sys->memset(&d, 0, sizeof(d));
    d.kind = SD_SLOT;
    d.slot = (int)(s - f->syms);
    d.type = struct_record_type(vm, id);
    for (int i = 0; i < n; i++) {
        if (typed[i]) {
            if (struct_fill_field(vm, f, &d, id, i, typed[i], 0, brace, slot, count, cap) < 0) return -1;
            continue;
        }
        VMType ft;
        int base = sym_read(vm, f, d.slot);
        int lv = struct_field_node(vm, f, base, struct_get(vm, id), i, &ft);
        push_assign(vm, f, lv, vals[i], slot, count, cap);
    }
    return 1;
}

static int struct_fill_slot(VM *vm, Func *f, int slot, int id, ASTNode *brace, int zero_omitted,
                            ASTNode *at, int **items, int *count, int *cap) {
    StructDest d;
    vm->run.sys->memset(&d, 0, sizeof(d));
    d.kind = SD_SLOT;
    d.slot = slot;
    d.type = f->syms[slot].type;
    return struct_fill(vm, f, &d, id, brace, zero_omitted, at, items, count, cap);
}

// `base.x op= v` where evaluating `base` has a side effect -- `g(s)[i].x += 1` --
// would run it twice if desugared to `base.x = base.x op v`. The record is bound
// to a hidden ref first and both halves go through that. Returns 0 when the
// target is not such a field, so the ordinary desugaring applies.
static int compile_update_field_staged(VM *vm, Func *f, ASTNode *target, InternID op,
                                       ASTNode *operand, int **slot, int *count, int *cap) {
    ASTNode *tgt = paren_inner(target);
    if (!tgt || tgt->token != TOK_DOT || !tgt->left || !is_ident(paren_inner(tgt->right))) return 0;
    // Nothing but a struct can have a field besides .len, so skip the trial
    // compile entirely when the unit declares none.
    int any_struct = vm->structs && vm->structs->count > 0;
    for (int i = 0; !any_struct && i < vm->n_globals; i++)
        if (vm->globals[i].struct_name) any_struct = 1;
    if (!any_struct) return 0;
    int mark = f->n_nodes;
    VMType bt;
    int base = compile_expr(vm, f, tgt->left, &bt);
    if (base < 0) return -1;
    if (!(vmt_is_record(bt) || vmt_is_ref(bt)) || !ir_has_side_effects(f, base)) {
        // A trial compile only: drop the nodes it made, so no stray IR is left
        // for a whole-body scan to trip over.
        f->n_nodes = mark;
        return 0;
    }
    if (!struct_lvalue_ok(f, base)) {
        vm_set_error_at(vm, tgt, "cannot assign to a field of a temporary; assign it to a variable first");
        return -1;
    }
    const VMStruct *st = struct_get(vm, bt.struct_id);
    InternID fname = paren_inner(tgt->right)->token;
    int fi = struct_field_index(st, fname);
    if (fi < 0) {
        char list[160];
        vm_errorf_at(vm, tgt->right, "struct '%s' has no field '%s' (fields: %s)",
                     struct_name_s(vm, bt.struct_id), intern_get_cstr(vm->intern, fname),
                     struct_field_list(vm, st, list, sizeof(list)));
        return -1;
    }
    if (st->fields[fi].width == FIELD_AGG) {
        vm_errorf_at(vm, tgt->right, "an update operator needs a number; field '%s' is not one",
                     intern_get_cstr(vm->intern, fname));
        return -1;
    }
    VMType ref = {0};
    ref.kind = VMT_SLICE_I32;
    ref.pack_bits = 8;
    ref.struct_id = bt.struct_id;
    VMSym *hs = sym_add(vm, f, 0, ref);
    hs->used = 1;
    int hslot = (int)(hs - f->syms);
    push_assign(vm, f, sym_read(vm, f, hslot), base, slot, count, cap);

    VMType ft;
    int rd = struct_field_node(vm, f, sym_read(vm, f, hslot), struct_get(vm, bt.struct_id), fi, &ft);
    VMType ot;
    int opnd = compile_expr(vm, f, operand, &ot);
    if (opnd < 0) return -1;
    int sub;
    if (!op_from_token(op, &sub)) { vm_set_error_at(vm, tgt, "unsupported update operator"); return -1; }
    if (sub == OP_BSHL && (vm->flags & VM_FLAG_C_SHIFTS)) sub = OP_BSHL_NATIVE;
    VMType vt;
    int val = compile_binop_ir(vm, f, tgt, sub, rd, ft, opnd, ot, &vt);
    if (val < 0) return -1;
    if (vt.kind != ft.kind || ct_shift(vt) != ct_shift(ft)) val = insert_fx_convert(vm, f, val, vt, ft);
    int lv = struct_field_node(vm, f, sym_read(vm, f, hslot), struct_get(vm, bt.struct_id), fi, &ft);
    push_assign(vm, f, lv, val, slot, count, cap);
    return 1;
}

// Compile `name: T` or `name: T = init` into a declaration.  `colon` is the
// TOK_COLON node (left = the name, right = the type annotation); `init` is the
// initialiser expression, or NULL for a bare declaration -- which is how a
// writable packed buffer is created, since the frame is zeroed by args_bind.
static int compile_annotated_decl(VM *vm, Func *f, ASTNode *colon, ASTNode *init,
                                  int **slot, int *count, int *cap) {
    InternID nm = colon->left->token;
    const char *nm_s = intern_get_cstr(vm->intern, nm);
    if (!const_reject_write(vm, colon, nm, "redeclared")) return -1;
    VMType declared;
    int open_len = 0;
    if (!parse_type_ex(vm, colon->right, &declared, 1, &open_len) || declared.kind == VMT_VOID) {
        // An instance named where a type belongs gets told apart from a typo.
        ASTNode *tn = paren_inner(colon->right);
        VMSym *ts = is_ident(tn) ? sym_find(f, tn->token) : 0;
        if (ts && vmt_is_struct(ts->type)) {
            vm->run.last_error = 0;
            vm_errorf_at(vm, colon->right, "'%s' is a struct instance, not a type; declare the type with "
                                           "name = struct { ... }", intern_get_cstr(vm->intern, tn->token));
            return -1;
        }
        // parse_type has already reported the specific problem in most cases.
        if (!vm_last_error(vm))
            vm_errorf_at(vm, colon->right, "bad type annotation for '%s'", nm_s);
        return -1;
    }
    if (sym_find(f, nm)) {
        vm_errorf_at(vm, colon, "'%s' is already declared", nm_s);
        return -1;
    }
    if (struct_find(vm, nm)) {
        vm_errorf_at(vm, colon, "'%s' is a struct type; name the instance something else", nm_s);
        return -1;
    }
    if (vmt_is_struct(declared))
        return compile_struct_instance_decl(vm, f, colon, declared, init, slot, count, cap);
    if (is_slice(declared.kind) && !init) {
        vm_errorf_at(vm, colon, "slice '%s' needs an initialiser (a slice has no storage of its own)", nm_s);
        return -1;
    }
    if (open_len && !init) {
        vm_errorf_at(vm, colon, "'%s' is [_], which takes its length from an initialiser", nm_s);
        return -1;
    }

    VMSym *s = sym_add(vm, f, nm, ct_vmtype_clear_shift(declared));
    s->shift = ct_shift(declared);
    if (!init) return 1;   // zeroed by the frame; nothing to emit

    // Keep the INDEX, not the pointer: compile_expr below can add symbols (a
    // staging temp for **, a shared-variable binding), and sym_add grows
    // f->syms by allocating a new array and copying -- which leaves any
    // VMSym* taken beforehand pointing into the old one. Reading through it
    // gives a stale type, and `s - f->syms` then yields a wild slot index that
    // only blows up much later, in the interpreter or the emitter.
    int si = (int)(s - f->syms);
    s = 0;

    VMType rt;
    int r = compile_expr(vm, f, init, &rt);
    if (r < 0) return -1;

    if (is_array(declared.kind) || is_slice(declared.kind)) {
        if (!is_array(rt.kind) && !is_slice(rt.kind)) {
            vm_errorf_at(vm, init, "'%s' is declared as an array but initialised with a scalar", nm_s);
            return -1;
        }
        if (open_len) {
            if (!is_array(rt.kind)) {
                vm_errorf_at(vm, init, "'%s' is [_], a copy, and a slice has no length to copy; write the length", nm_s);
                return -1;
            }
            declared.len = f->nodes[r].op == IR_ARR_LIT ? f->nodes[r].n_items : rt.len;
            declared.inner_len = rt.inner_len;
            f->syms[si].type = ct_vmtype_clear_shift(declared);
        }
        if (arr_elem(declared.kind) != arr_elem(rt.kind)) {
            vm_errorf_at(vm, init, "array element type mismatch initialising '%s'", nm_s);
            return -1;
        }
        if (is_array(declared.kind) && declared.inner_len != rt.inner_len) {
            vm_errorf_at(vm, init, "nested array shape mismatch initialising '%s'", nm_s);
            return -1;
        }
        // A view onto read-only storage cannot be declared writable. A by-value
        // [N]T is exempt: it copies, so what it copied from does not constrain it.
        if (rt.is_const && !declared.is_const && is_slice(declared.kind)) {
            char rbuf[24];
            vm_errorf_at(vm, init,
                "'%s' is declared writable but initialised from %s; declare it const[]T too, "
                "or copy into a [N]T", nm_s, type_label(rt, rbuf, sizeof(rbuf)));
            return -1;
        }
        // `s: []i32 = "hi"` -- widen the literal to one i32 per character.
        if (declared.pack_bits == 0 && rt.pack_bits != 0) dataslice_to_i32(vm, f, r);
        if (is_array(declared.kind) && f->nodes[r].op == IR_ARR_LIT) {
            if (f->nodes[r].n_items != declared.len) {
                vm_errorf_at(vm, init, "'%s' is declared [%d] but initialised with %d elements",
                             nm_s, declared.len, f->nodes[r].n_items);
                return -1;
            }
            // Range-check what fits: a packed element silently losing its top
            // bits is the kind of thing you only notice much later.
            if (declared.pack_bits && !(vm->flags & VM_FLAG_LOSSY_ASSIGNMENT)) {
                long long lim = 1LL << declared.pack_bits;
                for (int i = 0; i < f->nodes[r].n_items; i++) {
                    IRNode *el = &f->nodes[f->child_indices[f->nodes[r].items_begin + i]];
                    if (el->op != IR_CONST_I) continue;
                    if (el->ki < 0 || el->ki >= lim) {
                        vm_errorf_at(vm, init,
                            "%lld does not fit in u%d (0..%lld); #enable lossy_assignment to allow",
                            el->ki, declared.pack_bits, lim - 1);
                        return -1;
                    }
                }
            }
            // Carry the packing onto the literal so the C emitter lays the
            // initialiser out the same way the interpreter stores it. An array
            // literal has no packing of its own -- it adopts the declared
            // type -- so this is the one case that skips the check below.
            f->nodes[r].type = declared;
        } else if (declared.pack_bits != f->nodes[r].type.pack_bits) {
            vm_errorf_at(vm, init, "element packing mismatch initialising '%s'", nm_s);
            return -1;
        }
    } else {
        if (rt.kind != declared.kind || ct_shift(rt) != ct_shift(declared))
            r = insert_fx_convert(vm, f, r, rt, declared);
    }

    int lv = ir_new(vm, f, IR_LOCAL);
    f->nodes[lv].type = f->syms[si].type;   // re-read: see `si` above
    f->nodes[lv].ki = si;
    int ir = ir_new(vm, f, IR_ASSIGN);
    f->nodes[ir].a = lv; f->nodes[ir].b = r;
    stmt_push_idx(vm, slot, count, cap, ir);
    return 1;
}

// `const NAME = expr` / `const NAME : T = expr`.
//
// Declares nothing at run time: the value is folded here and re-emitted at
// each use (see VMConst), so nothing is allocated in the frame, nothing is
// added to the shared-variable table, and nothing appears in emitted C except
// the literal itself. `decl` is the `=` node the keyword was applied to.
static int compile_const_decl(VM *vm, Func *f, ASTNode *head, ASTNode *decl) {
    (void)f;
    decl = paren_inner(decl);
    if (!decl || decl->token != TOK_EQ || !decl->left || !decl->right) {
        vm_set_error_at(vm, decl ? decl : head, "const needs a value: const NAME = expr");
        return -1;
    }

    ASTNode *lhs = paren_inner(decl->left);
    ASTNode *name_node = lhs;
    ASTNode *type_node = 0;
    if (lhs && lhs->token == TOK_COLON && is_ident(lhs->left) && lhs->right) {
        name_node = lhs->left;
        type_node = lhs->right;
    }
    if (!is_ident(name_node)) {
        vm_set_error_at(vm, decl->left, "const needs a name: const NAME = expr");
        return -1;
    }
    InternID nm = name_node->token;
    const char *nm_s = intern_get_cstr(vm->intern, nm);

    VMConst *prev = const_find(vm, nm);
    // The same declaration again: a specialisation re-compiling this body.
    if (prev && prev->decl == decl && prev->scope == vm->cur_scope) return 1;
    if (prev) {
        // Naming the host's list explicitly: "already declared" reads as a typo
        // when the other declaration is nowhere in the source (see
        // vm_declare_const_num).
        int from_host = 0;
        for (VMConst *h = vm->host_consts; h; h = h->next)
            if (h->name == nm) { from_host = 1; break; }
        vm_errorf_at(vm, name_node, from_host
            ? "'%s' is a built-in constant and cannot be redeclared"
            : "constant '%s' is already declared", nm_s);
        return -1;
    }
    if (define_find(vm, nm)) {
        vm_errorf_at(vm, name_node, "'%s' is already declared as a #define", nm_s);
        return -1;
    }
    if (sym_find(f, nm)) {
        vm_errorf_at(vm, name_node, "'%s' is already declared as a variable", nm_s);
        return -1;
    }

    VMType declared = { VMT_VOID, 0, 0 };
    if (type_node) {
        if (!parse_type(vm, type_node, &declared, 1) || declared.kind == VMT_VOID) {
            if (!vm_last_error(vm))
                vm_errorf_at(vm, type_node, "bad type annotation for constant '%s'", nm_s);
            return -1;
        }
        if (is_array(declared.kind) || is_slice(declared.kind)) {
            vm_errorf_at(vm, type_node,
                "constant '%s' cannot be an array: an array needs storage. Declare it in the "
                "shared-variable block instead", nm_s);
            return -1;
        }
    }

    ASTNode *init = paren_inner(decl->right);
    if (init && init->left_bracket != 0) {
        vm_errorf_at(vm, init,
            "constant '%s' cannot be an array or block literal: it needs storage. Declare it in "
            "the shared-variable block instead", nm_s);
        return -1;
    }

    VMConst *c = (VMConst*)mem_alloc(&vm->run.mem, sizeof(VMConst));
    if (!c) { vm_set_error_at(vm, decl, "out of memory declaring a constant"); return -1; }
    vm->run.sys->memset(c, 0, sizeof(*c));
    c->name     = nm;
    c->type     = ct_vmtype_clear_shift(declared);
    c->has_type = type_node ? 1 : 0;
    c->scope    = vm->cur_scope;
    c->decl     = decl;
    if (type_node) c->type = declared;

    // A string const keeps its literal node and re-compiles it at each use: a
    // string is already a borrowed pointer to constant data, so this costs the
    // same nothing a number const does.
    if (init && init->string != 0 && !init->left && !init->right) {
        if (type_node) {
            vm_errorf_at(vm, type_node, "constant '%s' is a string; drop the type annotation", nm_s);
            return -1;
        }
        c->str = init;
    } else {
        int num_type = NUM_INTEGER;
        c->evaluating = 1;
        // Literal arithmetic folds without building a Func -- the common case,
        // and the one whose error messages name what a const may contain.
        // Anything else is compiled and run (see comptime_eval); const_eval's
        // refusal is what decides which, so it must not report the failure.
        const char *saved_err = vm->run.last_error;
        int saved_row = vm->run.error_row, saved_col = vm->run.error_col;
        int hard = 0;
        // const_eval folds in f64/long long, which IS precise mode. An
        // annotated const under #disable const_precise has to be compiled at
        // its declared type instead, so the fast path does not apply to it.
        int precise = !(c->has_type && !(vm->flags & VM_FLAG_CONST_PRECISE));
        int ok = precise && const_eval(vm, decl->right, &c->value, &num_type, 0, &hard);
        if (!ok && !hard) {
            vm->run.last_error = saved_err;
            vm->run.error_row = saved_row; vm->run.error_col = saved_col;
            ok = comptime_eval(vm, paren_inner(decl->right), c->type, c->has_type,
                               &c->value, &num_type);
        }
        c->evaluating = 0;
        if (!ok) return -1;
        c->num_flags = num_type;
    }

    c->next = vm->consts;
    vm->consts = c;
    return 1;
}

// Does anything under IR node `idx` assign to local slot `slot`?
//
// IR_ASSIGN is the only node that writes a scalar local -- `++`/`+=` desugar
// onto it, and a call cannot reach a caller's frame -- so one walk looking for
// an IR_ASSIGN whose lvalue names the slot answers the whole question.
// Used by compile_for_range to decide whether a plain-variable loop endpoint
// has to be copied into a hidden local first.
static int ir_writes_slot(Func *f, int idx, int slot) {
    if (idx < 0) return 0;
    IRNode *n = &f->nodes[idx];
    if (n->op == IR_ASSIGN && n->a >= 0) {
        IRNode *lv = &f->nodes[n->a];
        if (lv->op == IR_LOCAL && (int)lv->ki == slot) return 1;
    }
    if (ir_writes_slot(f, n->a, slot)) return 1;
    if (ir_writes_slot(f, n->b, slot)) return 1;
    if (ir_writes_slot(f, n->c, slot)) return 1;
    if (n->items_begin >= 0)
        for (int i = 0; i < n->n_items; i++)
            if (ir_writes_slot(f, f->child_indices[n->items_begin + i], slot)) return 1;
    return 0;
}

// Is `n`, the target of a write, the name itself? `i`, `i: T`, `i, x`.
static int unroll_names(ASTNode *n, InternID name) {
    while (n && n->token == 0 && !n->left && !n->right && n->items.size == 1)
        n = *(ASTNode**)array_get(&n->items, 0);
    if (!n) return 0;
    if (is_ident(n)) return n->token == name;
    if (n->token == TOK_COLON || n->token == TOK_COMMA)
        return unroll_names(n->left, name) || unroll_names(n->right, name);
    return 0;
}

// Does a loop body need its loop? A break or continue does, a function defined in
// it would be defined once per pass, and a write to `name` steers the counter.
// Only the innermost loop unrolls, so a nested one keeps it too: unrolled at every
// level the passes would multiply.
// A declaration by annotation, `v : T` or `v : T = e`: compiled once per pass it
// would declare `v` again. A ternary's `:` sits under its `?`, never here.
static int unroll_is_decl(ASTNode *n) {
    n = paren_inner(n);
    if (n && n->token == TOK_EQ) n = paren_inner(n->left);
    return n && n->token == TOK_COLON && is_ident(paren_inner(n->left));
}

static int unroll_blocked(VM *vm, ASTNode *n, InternID name) {
    if (!n) return 0;
    if (n->token == TOK_ARROW_F) return 1;
    if (unroll_is_decl(n)) return 1;
    if (n->token == TOK_QMARK && n->right && n->right->token == TOK_COLON)
        return unroll_blocked(vm, n->left, name) || unroll_blocked(vm, n->right->left, name)
            || unroll_blocked(vm, n->right->right, name);
    if (ident_is(vm, n, "break") || ident_is(vm, n, "continue")) return 1;
    if (ident_is(vm, n, "for") || ident_is(vm, n, "while")) return 1;
    if ((n->token == TOK_EQ || n->token == TOK_IN || TOKEN_IS_COMPOUND_ASSIGN(n->token))
        && unroll_names(n->left, name)) return 1;
    if (TOKEN_IS_INC_DEC(n->token) && (unroll_names(n->left, name) || unroll_names(n->right, name))) return 1;
    if (n->token == TOK_COMMA && n->right && n->right->token == TOK_IN && unroll_names(n->left, name)) return 1;
    if (unroll_blocked(vm, n->left, name) || unroll_blocked(vm, n->right, name)) return 1;
    for (int i = 0; i < (int)n->items.size; i++)
        if (unroll_blocked(vm, *(ASTNode**)array_get(&n->items, i), name)) return 1;
    return 0;
}

// The number a constant node stands for, an fx one decoded.
static double unroll_const_value(Func *f, int c) {
    if (f->nodes[c].op == IR_CONST_F) return f->nodes[c].kf;
    double v = (double)f->nodes[c].ki;
    for (int s = f->nodes[c].type.kind == VMT_I32 ? f->nodes[c].type.len : 0; s > 0; s--) v *= 0.5;
    return v;
}

// A loop counter's `i = <start>` -- when it is the one statement its compile pushed
// (stmts[from..to)), a plain numeric local given a constant. Else -1.
static int unroll_counter_init(Func *f, const int *stmts, int from, int to) {
    if (to - from != 1) return -1;
    int s = stmts[from];
    if (f->nodes[s].op != IR_ASSIGN) return -1;
    int lv = f->nodes[s].a, rv = f->nodes[s].b;
    if (lv < 0 || rv < 0 || f->nodes[lv].op != IR_LOCAL) return -1;
    if (f->nodes[rv].op != IR_CONST_I && f->nodes[rv].op != IR_CONST_F) return -1;
    VMSym *sym = &f->syms[f->nodes[lv].ki];
    if (sym->is_global || sym->is_host_buf) return -1;
#if VM_REACTIVE
    if (sym->is_extern) return -1;
#endif
    VMType t = ct_set_shift(sym->type, sym->shift);
    VMType st = f->nodes[rv].type;
    if (t.kind != VMT_I32 && t.kind != VMT_I64 && t.kind != VMT_F32 && t.kind != VMT_F64) return -1;
    if (st.kind != t.kind || (t.kind == VMT_I32 && st.len != t.len)) return -1;
    return s;
}

// Compile `body_ast` n times, `name` (when not 0) bound to ki[k] / kf[k] in pass k,
// appending each pass's statements. 1, or -1 on an error.
static int unroll_passes(VM *vm, Func *f, InternID name, VMType t, const long long *ki, const double *kf,
                         int n, ASTNode *body_ast, int **slot, int *count, int *cap) {
    for (int k = 0; k < n; k++) {
        int saved = vm->n_unroll;
        if (name) {
            int u = vm->n_unroll++;
            vm->unroll[u].name      = name;
            vm->unroll[u].f         = f;
            vm->unroll[u].lib_depth = vm->lib_depth;
            vm->unroll[u].type      = t;
            vm->unroll[u].ki        = ki[k];
            vm->unroll[u].kf        = kf[k];
        }
        int blk = compile_block(vm, f, body_ast);
        vm->n_unroll = saved;
        if (blk < 0) return -1;
        for (int j = 0; j < f->nodes[blk].n_items; j++)
            stmt_push_idx(vm, slot, count, cap, f->child_indices[f->nodes[blk].items_begin + j]);
    }
    return 1;
}

// `for i in a..b` with constant ends and at most VM_UNROLL_MAX passes: the body
// once per pass with `i` a constant, then the init, re-pointed at where the
// counter would have stopped. 1 when it unrolled, 0 to compile the loop, -1 on an
// error. See VM_FLAG_UNROLL.
static int try_unroll_range(VM *vm, Func *f, ASTNode *var, int iasn, int end, int inclusive,
                            ASTNode *body_ast, int **slot, int *count, int *cap) {
    if (!(vm->flags & VM_FLAG_UNROLL) || vm->n_unroll >= VM_UNROLL_MAX) return 0;
    if (*count < 1 || (*slot)[*count - 1] != iasn) return 0;
    if (unroll_blocked(vm, body_ast, var->token)) return 0;
    int islot = (int)f->nodes[f->nodes[iasn].a].ki;
    VMType it = ct_set_shift(f->syms[islot].type, f->syms[islot].shift);
    int start = f->nodes[iasn].b;
    double sv = unroll_const_value(f, start), ev = unroll_const_value(f, end);
    int n = 0;
    while (n <= VM_UNROLL_MAX && (inclusive ? sv + n <= ev : sv + n < ev)) n++;
    // Zero passes would compile the body zero times: nothing it declares would
    // exist afterwards, and nothing in it would be checked. The loop costs nothing.
    if (n == 0 || n > VM_UNROLL_MAX) return 0;

    long long one = it.kind == VMT_I32 ? 1LL << it.len : 1;
    long long ki[VM_UNROLL_MAX + 1];
    double    kf[VM_UNROLL_MAX + 1];
    for (int k = 0; k <= n; k++) {
        ki[k] = f->nodes[start].ki + k * one;
        kf[k] = f->nodes[start].kf + k;
    }
    (*count)--;
    if (unroll_passes(vm, f, var->token, it, ki, kf, n, body_ast, slot, count, cap) < 0) return -1;
    int fin = ir_const_of(vm, f, it, ki[n], kf[n]);
    f->nodes[iasn].b = fin;
    stmt_push_idx(vm, slot, count, cap, iasn);
    f->syms[islot].unrolled = 1;
    return 1;
}

// `for i in a..b` -- the counted loop, spelled as in Rust: `..` stops before the
// endpoint, `..=` includes it. Both the paren form and the paren-less indent form
// reach here; by this point the parens are off and the caller has checked that `var`
// is a writable plain name.
//
// Desugars to exactly the three-clause loop (init in the enclosing statement list,
// cond, step), so `break`/`continue` and every backend need to know nothing about
// it. Returns 1, or -1 with the error set.
static int compile_for_range(VM *vm, Func *f, ASTNode *var, ASTNode *range,
                             ASTNode *body_ast, int **slot, int *count, int *cap) {
    ASTNode init, one, bin, step;
    vm->run.sys->memset(&init, 0, sizeof(init));
    vm->run.sys->memset(&one,  0, sizeof(one));
    vm->run.sys->memset(&bin,  0, sizeof(bin));
    vm->run.sys->memset(&step, 0, sizeof(step));
    // i = <start>, declaring i in the enclosing scope exactly as
    // `for(i = 0; ...)` does.
    init.token = TOK_EQ; init.left = var; init.right = range->left;
    // Not a constant declaration however constant `<start>` is: the step below
    // writes `i` every pass, and folding it here would leave the step with
    // nothing to write. See VM.ac_suspend.
    int init_at = *count;
    vm->ac_suspend++;
    int init_r = compile_stmt_into(vm, f, &init, slot, count, cap);
    vm->ac_suspend--;
    if (init_r < 0) return -1;

    // The endpoint is evaluated once, into a hidden local, and the condition reads
    // that -- which keeps `for i in 0..count()` from calling count() every pass, as
    // putting the expression in IR_FOR's cond slot would.
    //
    // Two endpoints skip the hidden local, since the condition can name them
    // directly and still only ever see one value: a constant, and a plain local or
    // parameter the loop never writes. Whether the loop writes it is not known until
    // the body and step exist, so the direct read is built now and retro-fitted
    // further down when it turns out not to hold. Shared variables and host buffers
    // never qualify: a call inside the body can reach those.
    VMType et;
    int end = compile_expr(vm, f, range->right, &et);
    if (end < 0) return -1;
    int end_is_const = (f->nodes[end].op == IR_CONST_I || f->nodes[end].op == IR_CONST_F);
    int iasn = unroll_counter_init(f, *slot, init_at, *count);
    if (iasn >= 0 && end_is_const) {
        int u = try_unroll_range(vm, f, var, iasn, end, range->token == TOK_DOTDOT_EQ,
                                 body_ast, slot, count, cap);
        if (u != 0) return u;
    }
    int eslot = -1;
    int direct_slot = -1;
    if (!end_is_const && f->nodes[end].op == IR_LOCAL) {
        VMSym *ds = &f->syms[f->nodes[end].ki];
        if (!ds->is_global && !ds->is_host_buf && !ds->is_func)
            direct_slot = (int)f->nodes[end].ki;
    }
    if (!end_is_const && direct_slot < 0) {
        VMSym *es = sym_add(vm, f, 0, ct_vmtype_clear_shift(et));
        es->shift = ct_shift(et);
        es->used  = 1;
        eslot = (int)(es - f->syms);
        int elv = ir_new(vm, f, IR_LOCAL);
        f->nodes[elv].type = et;   // shift carried on the node, as compile_ident does
        f->nodes[elv].ki   = eslot;
        int easn = ir_new(vm, f, IR_ASSIGN);
        f->nodes[easn].a = elv; f->nodes[easn].b = end;
        stmt_push_idx(vm, slot, count, cap, easn);
    }

    // i < end  /  i <= end
    VMType it;
    int iv = compile_expr(vm, f, var, &it);
    if (iv < 0) return -1;
    int erd = end;
    if (eslot >= 0) {
        erd = ir_new(vm, f, IR_LOCAL);
        f->nodes[erd].type = et;
        f->nodes[erd].ki   = eslot;
    }
    VMType ct;
    int cond = compile_binop_ir(vm, f, range,
                                range->token == TOK_DOTDOT ? OP_LT : OP_LE,
                                iv, it, erd, et, &ct);
    if (cond < 0) return -1;
    cond = emit_truthy(vm, f, cond, ct);
    if (cond < 0) return -1;

    int body = compile_block(vm, f, body_ast);
    if (body < 0) return -1;
    // i = i + 1
    one.number = 1; one.number_flags = NUM_INTEGER;
    bin.token  = TOK_PLUS; bin.left = var; bin.right = &one;
    step.token = TOK_EQ;   step.left = var; step.right = &bin;
    int stp = compile_block(vm, f, &step);
    if (stp < 0) return -1;

    // The direct endpoint read above only holds while nothing in the loop
    // writes that variable. When something does -- including `for i in 0..i`,
    // where the step itself does -- fall back to the hidden local: copy the
    // variable into it ahead of the loop and repoint the condition's read,
    // which is the one node `end` refers to.
    if (direct_slot >= 0 && (ir_writes_slot(f, body, direct_slot)
                          || ir_writes_slot(f, stp, direct_slot))) {
        VMSym *es = sym_add(vm, f, 0, ct_vmtype_clear_shift(et));
        if (!es) return -1;
        es->shift = ct_shift(et);
        es->used  = 1;
        eslot = (int)(es - f->syms);
        int src = ir_new(vm, f, IR_LOCAL);   // the variable, read once
        f->nodes[src].type = et;
        f->nodes[src].ki   = direct_slot;
        int elv = ir_new(vm, f, IR_LOCAL);
        f->nodes[elv].type = et;
        f->nodes[elv].ki   = eslot;
        int easn = ir_new(vm, f, IR_ASSIGN);
        f->nodes[easn].a = elv; f->nodes[easn].b = src;
        stmt_push_idx(vm, slot, count, cap, easn);
        f->nodes[end].ki = eslot;            // erd IS this node; see above
    }

    int fixed = iasn >= 0 && end_is_const && !ir_writes_slot(f, body, (int)f->nodes[f->nodes[iasn].a].ki);
    int ir = ir_new(vm, f, IR_FOR);
    f->nodes[ir].a = cond; f->nodes[ir].b = body; f->nodes[ir].c = stp;
    f->nodes[ir].ki = fixed;
    stmt_push_idx(vm, slot, count, cap, ir);
    return 1;
}

// A read of the pure operand `ir` -- a leaf, under an inspect maybe -- as a node of
// its own, for a second use.
static int ir_clone_leaf(VM *vm, Func *f, int ir) {
    while (f->nodes[ir].op == IR_INSPECT && f->nodes[ir].a >= 0) ir = f->nodes[ir].a;
    IRNode copy = f->nodes[ir];
    int c = ir_new(vm, f, copy.op);
    f->nodes[c] = copy;
    return c;
}

// `x = xs[ix]` for one pass of `for x in xs`, declaring x on first use. `abase`
// reads the sequence, `ix` is a raw i32 index. The assignment, or -1.
static int loop_elem_assign(VM *vm, Func *f, ASTNode *var, ASTNode *seq, VMType at, int abase, int ix) {
    VMType et;
    int elem;
    if (at.struct_id) {
        // One record per pass, copied into the loop variable like any element.
        et = struct_record_type(vm, at.struct_id);
        elem = ir_field(vm, f, abase, FIELD_ELEM, at.inner_len, ix, et);
    } else {
        elem = emit_index_ir(vm, f, seq, abase, at, ix, &et);
    }
    if (elem < 0) return -1;
    VMSym *xs = sym_find(f, var->token);
    if (!xs) xs = sym_bind_global(vm, f, var->token);
    if (vm->bind_failed) return -1;
    if (!xs && sym_bind_host_buf(vm, f, var->token)) {
        // Same answer as an assignment to one: the name IS the host's storage,
        // so a loop must not rebind it.
        vm_errorf_at(vm, var, "'%s' is a host buffer and cannot be a loop variable",
                     intern_get_cstr(vm->intern, var->token));
        return -1;
    }
    int xslot;
    if (!xs) {
        xs = sym_add(vm, f, var->token, ct_vmtype_clear_shift(et));
        xs->shift = ct_shift(et);
        xslot = (int)(xs - f->syms);
    } else {
        xslot = (int)(xs - f->syms);
        VMType xt = ct_set_shift(xs->type, xs->shift);
        if ((vmt_is_struct(xt) || vmt_is_struct(et)) && !struct_types_same(vm, xt, et)) {
            vm_errorf_at(vm, var, "'%s' already holds something other than a '%s'; name the loop variable differently",
                         intern_get_cstr(vm->intern, var->token), struct_name_s(vm, at.struct_id));
            return -1;
        }
        // An aggregate has no conversion: a row written into an f64 slice was
        // reinterpreted, and read past its end.
        int agg = is_array(xt.kind) || is_slice(xt.kind) || is_array(et.kind) || is_slice(et.kind);
        if ((agg && (xt.kind != et.kind || xt.inner_len != et.inner_len || (is_array(xt.kind) && xt.len != et.len)))
            || (!agg && (xt.kind != et.kind || ct_shift(xt) != ct_shift(et))
                && !(vm->flags & VM_FLAG_LOSSY_ASSIGNMENT)
                && is_lossy_assign(et.kind, ct_shift(et), xt.kind, ct_shift(xt)))) {
            char ebuf[24], xbuf[24];
            vm_errorf_at(vm, var, "'%s' is declared as %s, and this loop gives it %s; name the loop variable differently",
                         intern_get_cstr(vm->intern, var->token), type_label(xt, xbuf, sizeof(xbuf)),
                         type_label(et, ebuf, sizeof(ebuf)));
            return -1;
        }
        if (xt.kind != et.kind || ct_shift(xt) != ct_shift(et))
            elem = insert_fx_convert(vm, f, elem, et, xt);
    }
    int xlhs = sym_read(vm, f, xslot);
    int xasn = ir_new(vm, f, IR_ASSIGN);
    f->nodes[xasn].a = xlhs; f->nodes[xasn].b = elem;
    return xasn;
}

// `for x in xs` / `for i, x in xs` -- iterate an array or a slice. Desugars onto the
// same three-clause IR_FOR compile_for_range builds, with the element read spliced
// in front of the body:
//
//     __i = 0                        (or `i = 0`, when `ixvar` names it)
//     for(; __i < xs.len; __i = __i + 1) { x = xs[__i]; body }
//
// There is no iterator protocol behind this: the sequence is indexed, which is why
// it is arrays and slices and nothing else. Built as IR rather than synthetic AST,
// because without `ixvar` the counter is a hidden local and there is no name to
// write the loop with. `ixvar` is null for the one-variable form. Returns 1, or -1
// with the error set.
static int compile_for_sequence(VM *vm, Func *f, ASTNode *ixvar, ASTNode *var,
                                ASTNode *seq, ASTNode *body_ast,
                                int **slot, int *count, int *cap) {
    VMType at;
    int aref = compile_expr(vm, f, seq, &at);
    if (aref < 0) return -1;
    if (vmt_is_record(at) || vmt_is_ref(at)) {
        vm_set_error_at(vm, seq, "for ... in cannot iterate a struct; name a field, as p.x");
        return -1;
    }
    if (!is_array(at.kind) && !is_slice(at.kind)) {
        char desc[128];
        node_describe(vm, seq, desc, sizeof(desc));
        vm_errorf_at(vm, seq, "for ... in needs a range, an array or a slice, got %s", desc);
        return -1;
    }
    // The sequence is evaluated ONCE, for the reason the range form's endpoint
    // is: it is named twice per loop (the length and the element read), and
    // `for x in build()` must not call build() again every pass. A plain
    // variable read is already pure, so it is indexed where it stands -- no
    // copy, which matters, because copying a fixed array really does copy it.
    int aslot = -1;
    if (!ir_is_pure_operand(f, aref)) {
        VMSym *as = sym_add(vm, f, 0, ct_vmtype_clear_shift(at));
        as->shift = ct_shift(at);
        as->used  = 1;
        aslot = (int)(as - f->syms);
        int alv  = sym_read(vm, f, aslot);
        int aasn = ir_new(vm, f, IR_ASSIGN);
        f->nodes[aasn].a = alv; f->nodes[aasn].b = aref;
        stmt_push_idx(vm, slot, count, cap, aasn);
    }
    VMType i32t;
    vm->run.sys->memset(&i32t, 0, sizeof(i32t));
    i32t.kind = VMT_I32;

    // The length: a constant for a fixed array (and for a string literal), a
    // run-time read for a slice. The same split, and the same expression, as
    // compile_field's `.len`.
    int known = is_array(at.kind) ? (at.inner_len ? at.len / at.inner_len : at.len)
              : (f->nodes[aref].op == IR_DATA_SLICE ? f->nodes[aref].n_items : -1);
    int lenir;
    if (known >= 0) {
        lenir = ir_new(vm, f, IR_CONST_I);
        f->nodes[lenir].ki = known;
    } else {
        int alen = (aslot < 0) ? aref : sym_read(vm, f, aslot);
        lenir = ir_new(vm, f, IR_LEN);
        f->nodes[lenir].a  = alen;
        f->nodes[lenir].ki = at.struct_id ? at.inner_len : 0;   // records, not bytes
    }
    f->nodes[lenir].type = i32t;

    // The counter. `for i, x in xs` names it, and it is then declared in the
    // enclosing statement list and outlives the loop exactly as
    // compile_for_range's `i` does; `for x in xs` gets a hidden local instead.
    int islot, iasn = -1;
    if (ixvar) {
        ASTNode zero, iinit;
        vm->run.sys->memset(&zero,  0, sizeof(zero));
        vm->run.sys->memset(&iinit, 0, sizeof(iinit));
        zero.number = 0; zero.number_flags = NUM_INTEGER;
        iinit.token = TOK_EQ; iinit.left = ixvar; iinit.right = &zero;
        // Same as compile_for_range's init: the step writes it every pass.
        int ii_at = *count;
        vm->ac_suspend++;
        int ii_r = compile_stmt_into(vm, f, &iinit, slot, count, cap);
        vm->ac_suspend--;
        if (ii_r < 0) return -1;
        VMSym *ix = sym_find(f, ixvar->token);
        if (!ix) { vm_set_error_at(vm, ixvar, "for ... in could not declare the index variable"); return -1; }
        islot = (int)(ix - f->syms);
        iasn = unroll_counter_init(f, *slot, ii_at, *count);
    }

    // A short sequence of known length: `x = xs[k]` and the body once per element,
    // `i` the constant k, then the init re-pointed at the length. See VM_FLAG_UNROLL.
    if ((vm->flags & VM_FLAG_UNROLL) && known >= 1 && known <= VM_UNROLL_MAX && vm->n_unroll < VM_UNROLL_MAX
        && (!ixvar || (iasn >= 0 && (*slot)[*count - 1] == iasn))
        && !unroll_blocked(vm, body_ast, var->token) && !(ixvar && unroll_blocked(vm, body_ast, ixvar->token))) {
        VMType ut = ixvar ? ct_set_shift(f->syms[islot].type, f->syms[islot].shift) : i32t;
        long long one = ut.kind == VMT_I32 ? 1LL << ut.len : 1;
        if (ixvar) (*count)--;
        for (int k = 0; k < known; k++) {
            int abase = aslot >= 0 ? sym_read(vm, f, aslot) : k ? ir_clone_leaf(vm, f, aref) : aref;
            int xasn = loop_elem_assign(vm, f, var, seq, at, abase, ir_i32(vm, f, k));
            if (xasn < 0) return -1;
            stmt_push_idx(vm, slot, count, cap, xasn);
            long long ki = k * one;
            double    kf = k;
            if (unroll_passes(vm, f, ixvar ? ixvar->token : 0, ut, &ki, &kf, 1, body_ast, slot, count, cap) < 0) return -1;
        }
        if (ixvar) {
            int fin = ir_const_of(vm, f, ut, known * one, known);
            f->nodes[iasn].b = fin;
            stmt_push_idx(vm, slot, count, cap, iasn);
            f->syms[islot].unrolled = 1;
        }
        return 1;
    }

    if (ixvar) {
        f->syms[islot].used = 1;   // read by the condition, the step and the element
    } else {
        VMSym *ix = sym_add(vm, f, 0, i32t);
        ix->used = 1;
        islot = (int)(ix - f->syms);
        int z = ir_const_i(vm, f, i32t, 0);
        int ilv  = sym_read(vm, f, islot);
        int iasn0 = ir_new(vm, f, IR_ASSIGN);
        f->nodes[iasn0].a = ilv; f->nodes[iasn0].b = z;
        stmt_push_idx(vm, slot, count, cap, iasn0);
    }
    VMType it = ct_set_shift(f->syms[islot].type, f->syms[islot].shift);

    // __i < xs.len
    int ird = sym_read(vm, f, islot);
    VMType cndt;
    int cond = compile_binop_ir(vm, f, seq, OP_LT, ird, it, lenir, i32t, &cndt);
    if (cond < 0) return -1;
    cond = emit_truthy(vm, f, cond, cndt);
    if (cond < 0) return -1;

    // x = xs[__i] -- built BEFORE the body, so the element variable is already
    // in the symbol table when the body resolves it.
    int ixr = sym_read(vm, f, islot);
    VMType raw = { VMT_I32, 0 };
    if (it.kind != VMT_I32 || ct_shift(it) != 0) ixr = insert_fx_convert(vm, f, ixr, it, raw);
    int abase = (aslot < 0) ? aref : sym_read(vm, f, aslot);
    int xasn = loop_elem_assign(vm, f, var, seq, at, abase, ixr);
    if (xasn < 0) return -1;

    int body = compile_block(vm, f, body_ast);
    if (body < 0) return -1;
    // Splice the element read in front of the body's statements: a fresh items
    // run, since the block's own is already laid out and nothing can be
    // inserted into the middle of one.
    {
        int nb     = f->nodes[body].n_items;
        int obegin = f->nodes[body].items_begin;
        int nbegin = ir_alloc_items(vm, f, nb + 1);
        f->child_indices[nbegin] = xasn;
        for (int bi = 0; bi < nb; bi++)
            f->child_indices[nbegin + 1 + bi] = f->child_indices[obegin + bi];
        f->nodes[body].items_begin = nbegin;
        f->nodes[body].n_items     = nb + 1;
    }

    // __i = __i + 1, as the one-statement block IR_FOR's step slot expects
    // (and the C backend's step clause accepts).
    int stp   = ir_new(vm, f, IR_BLOCK);
    int sread = sym_read(vm, f, islot);
    int sone = ir_const_i(vm, f, i32t, 1);
    VMType addt;
    int add = compile_binop_ir(vm, f, seq, OP_ADD, sread, it, sone, i32t, &addt);
    if (add < 0) return -1;
    if (addt.kind != it.kind || ct_shift(addt) != ct_shift(it))
        add = insert_fx_convert(vm, f, add, addt, it);
    int slhs = sym_read(vm, f, islot);
    int sasn = ir_new(vm, f, IR_ASSIGN);
    f->nodes[sasn].a = slhs; f->nodes[sasn].b = add;
    int sbegin = ir_alloc_items(vm, f, 1);
    f->child_indices[sbegin] = sasn;
    f->nodes[stp].items_begin = sbegin;
    f->nodes[stp].n_items     = 1;

    int fixed = known >= 0 && !(ixvar && ir_writes_slot(f, body, islot));
    int ir = ir_new(vm, f, IR_FOR);
    f->nodes[ir].a = cond; f->nodes[ir].b = body; f->nodes[ir].c = stp;
    f->nodes[ir].ki = fixed;
    stmt_push_idx(vm, slot, count, cap, ir);
    return 1;
}

// Refuse a store whose target sits inside a read-only view. `lv` is the compiled
// lvalue -- an IR_INDEX, or an IR_FIELD when a record field is being written --
// and what is asked is whether the CONTAINER it indexes is const.
//
// This is deliberately a one-step question about the lvalue itself, not a walk
// back to whichever symbol originally declared the storage. Const rides on the
// type, so `q = p` and `q = p[0..2]` carry it to `q`, and `p[i].x` carries it to
// the record: there is no copy that sheds it and so nothing to chase.
static int const_lvalue_reject(VM *vm, Func *f, int lv, ASTNode *at) {
    if (lv < 0) return 0;
    IRNode *n = &f->nodes[lv];
    if (n->op != IR_INDEX && n->op != IR_FIELD) return 0;
    VMType base = f->nodes[n->a].type;
    if (!base.is_const) return 0;
    // A record's type_label is its raw [size]u8 storage, which says nothing to
    // the reader -- name the struct instead.
    if (base.struct_id) {
        vm_errorf_at(vm, at, "cannot write to this '%s': it came out of a read-only view",
                     struct_name_s(vm, base.struct_id));
        return -1;
    }
    char bbuf[32];
    vm_errorf_at(vm, at, "cannot write through %s: it is a read-only view",
                 type_label(base, bbuf, sizeof(bbuf)));
    return -1;
}

// `v.x = e` and `v.zx = e`. Returns 0 when the target is not a swizzle (a struct
// field, or swizzles are off), so the struct path runs instead.
//
// Every right-hand lane is staged before the first store, so `v.xy = v.yx`
// swaps rather than smearing. A scalar right-hand side broadcasts.
static int compile_swizzle_store(VM *vm, Func *f, ASTNode *node, int **slot, int *count, int *cap) {
    ASTNode *lhs = node->left;
    ASTNode *fld = paren_inner(lhs->right);
    const char *name = is_ident(fld) ? intern_get_cstr(vm->intern, fld->token) : 0;
    int comp[4], mixed = 0;
    int n = swizzle_parse(name, comp, &mixed);
    if (n == 0 || !(vm->flags & VM_FLAG_SWIZZLE)) return 0;
    int mark = f->n_nodes;
    VMType bt;
    int base = compile_expr(vm, f, lhs->left, &bt);
    if (base < 0) return -1;
    if (vmt_is_struct(bt)) { f->n_nodes = mark; return 0; }
    if (!swizzle_check_base(vm, lhs, bt, name, comp, n)) return -1;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (comp[i] == comp[j]) {
                vm_errorf_at(vm, lhs->right, "swizzle target .%s repeats %c", name, name[j]);
                return -1;
            }
    int bop = f->nodes[base].op;
    if (is_array(bt.kind) && (bop == IR_ARR_LIT || bop == IR_CALL || bop == IR_COMMA)) {
        vm_errorf_at(vm, lhs, "cannot assign to .%s of a temporary; assign it to a variable first", name);
        return -1;
    }

    VMType et;
    int lv0 = emit_index_ir(vm, f, lhs, base, bt, ir_i32(vm, f, comp[0]), &et);
    if (lv0 < 0 || const_lvalue_reject(vm, f, lv0, lhs) < 0) return -1;

    int *pre = 0, n_pre = 0, cap_pre = 0;
    // The base is named once per component: a local is re-read, a slice (a
    // view, so writes still land) is staged, and any other pure base is simply
    // compiled again.
    int bslot = -1;
    if (n > 1) {
        if (bop == IR_LOCAL) bslot = (int)f->nodes[base].ki;
        else if (is_slice(bt.kind)) {
            int st = stage_operand(vm, f, base, &pre, &n_pre, &cap_pre);
            bslot = (int)f->nodes[st].ki;
        } else if (ir_has_side_effects(f, base)) {
            vm_errorf_at(vm, lhs, "the target of .%s would be evaluated once per component; "
                                  "assign it to a variable first", name);
            return -1;
        }
    }

    VMType rt;
    int r = compile_expr(vm, f, node->right, &rt);
    if (r < 0) return -1;
    VecLanes rl;
    if (is_array(rt.kind) || is_slice(rt.kind)) {
        if (!vec_is_vectorizable(rt) || vmt_is_struct(rt) || rt.inner_len) {
            vm_errorf_at(vm, node->right, ".%s = needs %d number%s or an array of %d", name, n, n > 1 ? "s" : "", n);
            return -1;
        }
        if (!vec_lane_source(vm, f, node->right, r, rt, &rl, &pre, &n_pre, &cap_pre)) return -1;
        if (rl.n != n) {
            vm_errorf_at(vm, node->right, ".%s takes %d values; got [%d]", name, n, rl.n);
            return -1;
        }
    } else if (n == 1) {
        rl.n = 1; rl.is_scalar = 1; rl.inner_len = 0; rl.ir[0] = r; rl.t[0] = rt;
    } else {
        vec_scalar_lanes(vm, f, r, &rl, &pre, &n_pre, &cap_pre);
    }
    if (n > 1)
        for (int i = 0; i < rl.n; i++) {
            rl.ir[i] = stage_operand(vm, f, rl.ir[i], &pre, &n_pre, &cap_pre);
            rl.t[i]  = f->nodes[rl.ir[i]].type;
        }

    for (int i = 0; i < n_pre; i++) {
        int it = pre[i];
        if (f->nodes[it].op != IR_ASSIGN) {
            int es = ir_new(vm, f, IR_EXPR_STMT);
            f->nodes[es].a = it;
            it = es;
        }
        stmt_push_idx(vm, slot, count, cap, it);
    }
    for (int i = 0; i < n; i++) {
        int lv = lv0;
        if (i > 0) {
            int b = bslot >= 0 ? strb_local(vm, f, bslot) : compile_expr(vm, f, lhs->left, &bt);
            if (b < 0) return -1;
            lv = emit_index_ir(vm, f, lhs, b, bt, ir_i32(vm, f, comp[i]), &et);
        } else if (bslot >= 0 && bop != IR_LOCAL) {
            lv = emit_index_ir(vm, f, lhs, strb_local(vm, f, bslot), bt, ir_i32(vm, f, comp[0]), &et);
        }
        int li = rl.is_scalar ? 0 : i;
        int v = rl.ir[li];
        if (rl.t[li].kind != et.kind || ct_shift(rl.t[li]) != ct_shift(et))
            v = insert_fx_convert(vm, f, v, rl.t[li], et);
        push_assign(vm, f, lv, v, slot, count, cap);
    }
    return 1;
}

static int compile_stmt_into(VM *vm, Func *f, ASTNode *node, int **slot, int *count, int *cap) {
    if (!node) return 1;
    node = unwrap_chain(node);
    if (!node) return 1;
    vm->err_ctx = node;

    // Bare identifier statements: break / continue.
    //
    // Anything else falls through: a lone `return` reaches the keyword branch
    // below with no argument (an early exit), and a lone variable name becomes
    // an expression statement -- which is what lets a function body end in `x`
    // rather than `(x)` and still return it (see apply_implicit_return).
    // A name that isn't a variable still errors, in compile_ident.
    if (is_ident(node)) {
        const char *s = intern_get_cstr(vm->intern, node->token);
        if (s[0]=='b' && s[1]=='r' && s[2]=='e' && s[3]=='a' && s[4]=='k' && !s[5]) {
            stmt_push_idx(vm, slot, count, cap, ir_new(vm, f, IR_BREAK)); return 1;
        }
        if (s[0]=='c' && s[1]=='o' && s[2]=='n' && s[3]=='t' && s[4]=='i' && s[5]=='n' && s[6]=='u' && s[7]=='e' && !s[8]) {
            stmt_push_idx(vm, slot, count, cap, ir_new(vm, f, IR_CONTINUE)); return 1;
        }
        if (ident_is(vm, node, "else")) {
            // A bodyless `else`: the linefeed after it ended the statement, so
            // its block became a separate one (`else` <newline> `{ ... }`).
            vm_set_error_at(vm, node, "else body must start on the same line as 'else'");
            return -1;
        }
    }

    // Bare annotated declaration: `buf: [64]u8` -- storage only, zeroed by the
    // frame. The initialised form `name: T = expr` is handled in the TOK_EQ
    // branch below, which routes to the same place.
    if (node->token == TOK_COLON && is_ident(node->left) && node->right)
        return compile_annotated_decl(vm, f, node, annotation_swallowed_init(node->right),
                                      slot, count, cap);

    // Brace block: { ... }
    if (is_brace(node)) {
        int blk = compile_block(vm, f, node);
        if (blk < 0) return -1;
        stmt_push_idx(vm, slot, count, cap, blk);
        return 1;
    }

    // "if (...) {...}" / "while (...) {...}" / "return e".
    // Works whether or not there's a space after the keyword (the parser
    // produces different shapes for `if(x)` vs `if (x)` -- find_kw_head
    // normalizes both).  A following `else` is attached by compile_block, which
    // is the only level that can see the sibling statement it arrives as.
    {
      ASTNode *kw_args[8];
      int kw_n = 0;
      ASTNode *head = find_kw_head(vm, node, kw_args, &kw_n, 8);
      if (head) {
        const char *h = intern_get_cstr(vm->intern, head->token);
        if (h[0]=='i' && h[1]=='f' && !h[2]) {
            // find_kw_head appends a one-line `else` body after the `then` one
            // (`if c then a else b`); anything else reaches compile_if with 2.
            ASTNode **el = 0; int el_n = 0;
            if (kw_n >= 3) { el = &kw_args[2]; el_n = 1; kw_n = 2; }
            int ir = compile_if(vm, f, node, kw_args, kw_n, el, el_n);
            if (ir < 0) return -1;
            stmt_push_idx(vm, slot, count, cap, ir);
            return 1;
        }
        if (ident_is(vm, head, "else")) {
            // compile_block consumes an `else` that follows an `if`, so getting
            // here means there was no `if` in front of it.
            vm_set_error_at(vm, head, "'else' without a matching 'if'");
            return -1;
        }
        if (h[0]=='w' && h[1]=='h' && h[2]=='i' && h[3]=='l' && h[4]=='e' && !h[5]) {
            ASTNode **args = kw_args;
            int n = kw_n;
            if (n < 2) { vm_set_error_at(vm, node, "while requires (cond) { body }"); return -1; }
            if (!is_subscope(node))
                { vm_set_error_at(vm, node, "invalid syntax: use while(cond) {...} or while cond do stmt, not while (cond) {...}"); return -1; }
            if (!check_block_opener(vm, args[1], TOK_DO, "while")) return -1;
            VMType ct;
            int cond = compile_expr(vm, f, args[0], &ct);
            if (cond < 0) return -1;
            cond = emit_truthy(vm, f, cond, ct);
            if (cond < 0) return -1;
            int body = compile_block(vm, f, args[1]);
            if (body < 0) return -1;
            int ir = ir_new(vm, f, IR_WHILE);
            f->nodes[ir].a = cond; f->nodes[ir].b = body;
            stmt_push_idx(vm, slot, count, cap, ir);
            return 1;
        }
        if (h[0]=='f' && h[1]=='o' && h[2]=='r' && !h[3]) {
            ASTNode **args = kw_args;
            int n = kw_n;
            // Checked before the arity test: the spaced form flattens to a
            // single arg, so an arity complaint would hide the real problem.
            if (!is_subscope(node))
                { vm_set_error_at(vm, node, "invalid syntax: use indent block under `for x in xs`, `for x in xs do stmt`, `for(i in a..b) {...}` or `for(init; cond; step) {...}`"); return -1; }
            if (n < 2) { vm_set_error_at(vm, node, "for requires (init; cond; step) { body }, or i in a..b / x in xs { body }"); return -1; }
            if (!check_block_opener(vm, args[1], TOK_DO, "for")) return -1;
            ASTNode *ctrl = args[0];
            // An `in` clause is one of the two sugared loops; the paren form
            // `for(x in xs)` and the paren-less indent form differ only in
            // whether `ctrl` still wears its parens.
            ASTNode *rng = paren_inner(unwrap_chain(ctrl));
            // `for i, x in xs` -- the index-and-element form. A comma is a
            // DIVIDER, so the parser splits the control clause into two items
            // and the index arrives BESIDE the `in` node rather than under it:
            // [`i`, `in`(x, xs)]. Not the shape the pair reads like in source,
            // but an unambiguous one -- nothing else makes a two-item control
            // clause with an `in` in the second slot -- and the parser has no
            // business knowing this form exists.
            ASTNode *ixvar = 0;
            if (rng && rng->token == 0 && rng->items.size == 2) {
                ASTNode *a0 = paren_inner(unwrap_chain(*(ASTNode**)array_get(&rng->items, 0)));
                ASTNode *a1 = paren_inner(unwrap_chain(*(ASTNode**)array_get(&rng->items, 1)));
                if (a1 && a1->token == TOK_IN) { ixvar = a0; rng = a1; }
            }
            if (rng && rng->token == TOK_IN && rng->left && rng->right) {
                // The two `in` loops share their variable checks and differ
                // only in what sits to the right of the `in`; each desugaring
                // is its own function below.
                ASTNode *var = paren_inner(unwrap_chain(rng->left));
                ASTNode *seq = paren_inner(unwrap_chain(rng->right));
                if (!is_ident(var))
                    { vm_set_error_at(vm, rng->left, "for ... in needs a plain loop variable: for i in 0..n, or for x in xs"); return -1; }
                if (!const_reject_write(vm, var, var->token, "assigned to")) return -1;
                if (ixvar) {
                    if (!is_ident(ixvar))
                        { vm_set_error_at(vm, ixvar, "for ... in needs a plain index variable: for i, x in xs"); return -1; }
                    if (ixvar->token == var->token)
                        { vm_set_error_at(vm, ixvar, "the index and the element need different names: for i, x in xs"); return -1; }
                }
                if (!seq)
                    { vm_set_error_at(vm, rng->right, "for ... in needs a range or a sequence: for i in 0..n, or for x in xs"); return -1; }
                if (is_range_node(seq)) {
                    if (ixvar)
                        { vm_set_error_at(vm, ixvar, "for i, x in ... iterates a sequence; a range is already its own index"); return -1; }
                    return compile_for_range(vm, f, var, seq, args[1], slot, count, cap);
                }
                return compile_for_sequence(vm, f, ixvar, var, seq, args[1], slot, count, cap);
            }
            if (!is_paren(ctrl) || ctrl->items.size != 3)
                { vm_set_error_at(vm, ctrl, "for requires three ';'-separated clauses: for(init; cond; step) {...}"); return -1; }
            ASTNode *init_ast = *(ASTNode**)array_get(&ctrl->items, 0);
            ASTNode *cond_ast = *(ASTNode**)array_get(&ctrl->items, 1);
            ASTNode *step_ast = *(ASTNode**)array_get(&ctrl->items, 2);
            // The init clause runs once, in the enclosing block's statement
            // list, so a `for(i=0; ...)` declares i exactly like `i = 0` would.
            if (compile_stmt_into(vm, f, init_ast, slot, count, cap) < 0) return -1;
            VMType ct;
            int cond = compile_expr(vm, f, cond_ast, &ct);
            if (cond < 0) return -1;
            cond = emit_truthy(vm, f, cond, ct);
            if (cond < 0) return -1;
            int body = compile_block(vm, f, args[1]);
            if (body < 0) return -1;
            int step = compile_block(vm, f, step_ast);
            if (step < 0) return -1;
            // The C backend emits the step into a real for(;;) increment slot,
            // which only accepts expressions -- keep the clause to shapes that
            // survive that instead of silently emitting broken C.
            for (int i = 0; i < f->nodes[step].n_items; i++) {
                int si = f->child_indices[f->nodes[step].items_begin + i];
                int sop = f->nodes[si].op;
                int ok = (sop == IR_EXPR_STMT || sop == IR_CALL_STMT)
                      || (sop == IR_ASSIGN && f->nodes[f->nodes[si].b].op != IR_ARR_LIT);
                if (!ok) { vm_set_error_at(vm, step_ast, "for step clause must be an assignment or a call"); return -1; }
            }
            int ir = ir_new(vm, f, IR_FOR);
            f->nodes[ir].a = cond; f->nodes[ir].b = body; f->nodes[ir].c = step;
            stmt_push_idx(vm, slot, count, cap, ir);
            return 1;
        }
        if (h[0]=='r' && h[1]=='e' && h[2]=='t' && h[3]=='u' && h[4]=='r' && h[5]=='n' && !h[6]) {
            // return [expr] -- a bare `return` (kw_n == 0) is an early exit from a
            // function that hands back nothing. The two forms can't be mixed: one
            // says the function is void and the other gives it a type, and generated
            // C would be a `return;` inside a value-returning function.
            if (kw_n < 1 && f->has_return && f->ret_type.kind != VMT_VOID) {
                vm_set_error_at(vm, head,
                    "this function returns a value elsewhere, so 'return' needs one here too");
                return -1;
            }
            // One value at most: `return ,x` (or `return a, b`) dropped all but the
            // first, which for `,x` was nothing at all.
            if (kw_n > 1 || (kw_n == 1 && !kw_args[0])) {
                vm_set_error_at(vm, head, "'return' takes one value");
                return -1;
            }
            // The IR_RETURN node is allocated only once the value's type is known:
            // an empty one sitting in the node array reads as a valueless return,
            // which is exactly what has_valueless_return below looks for.
            if (kw_n >= 1 && kw_args[0]) {
                ASTNode *e = kw_args[0];
                VMType et;
                int ee = compile_expr(vm, f, e, &et);
                if (ee < 0) return -1;
                ee = ref_to_value(vm, f, ee, &et);

                // Returning a view of this frame hands back a pointer into
                // storage the caller is about to reuse. The interpreter catches
                // it at run time, but generated C would just corrupt, so reject
                // the two forms that say it outright. Laundering the slice
                // through a local still only trips the runtime check.
                if (slice_escapes_frame(f, ee)) {
                    vm_set_error_at(vm, e,
                        "cannot return a string or slice built here: it points into this "
                        "call's frame. Take a []u8 buffer as a parameter and fill that instead");
                    return -1;
                }
                if (et.kind == VMT_VOID) {
                    // `return f()` where f hands back nothing. The call still has to
                    // run, but the return carries no value: C rejects
                    // `return <void expr>;` outright, so the two are split here rather
                    // than in each of the three backends. This is itself a valueless
                    // return, so it is not checked for mixing against one.
                    int es = ir_new(vm, f, IR_EXPR_STMT);
                    f->nodes[es].a = ee;
                    stmt_push_idx(vm, slot, count, cap, es);
                } else {
                    if (has_valueless_return(f)) {
                        vm_set_error_at(vm, head,
                            "this function has a 'return' with no value, so it cannot return one here");
                        return -1;
                    }
                    if (!note_return_type(vm, f, et, e)) return -1;
                    int ir = ir_new(vm, f, IR_RETURN);
                    f->nodes[ir].a = ee;
                    stmt_push_idx(vm, slot, count, cap, ir);
                    return 1;
                }
            }
            stmt_push_idx(vm, slot, count, cap, ir_new(vm, f, IR_RETURN));
            return 1;
        }
        // `struct NAME { ... }`. `struct` is deliberately not in is_vm_keyword, so
        // the name and body arrive as one implicit call, `NAME { ... }`.
        if (s_strcmp(h, "struct") == 0) {
            ASTNode *u = kw_n == 1 ? unwrap_chain(kw_args[0]) : 0;
            if (!u || (u->token != TOK_EMPTYSTRING && u->token != 0) || !u->left || !u->right) {
                vm_set_error_at(vm, head, "struct is written struct NAME { field: T, ... }");
                return -1;
            }
            return compile_struct_decl(vm, f, u->left, u->right, head);
        }
        // `const NAME = expr` -- like `return`, a single un-flattened argument
        // (which is why `const` is absent from is_vm_keyword). Emits nothing:
        // the declaration is compile-time only.
        if (h[0]=='c' && h[1]=='o' && h[2]=='n' && h[3]=='s' && h[4]=='t' && !h[5]) {
            if (kw_n < 1) { vm_set_error_at(vm, head, "const needs a value: const NAME = expr"); return -1; }
            return compile_const_decl(vm, f, head, kw_args[0]);
        }
        // A block on a head that does not take one. Reported here, where the
        // head still has a name to quote, rather than reaching compile_expr as
        // an unrecognized node.
        if (is_subscope(node)) {
            vm_errorf_at(vm, subscope_block(node), "'%s' does not take a block", h);
            return -1;
        }
      }
      if (is_subscope(node)) {
          vm_set_error_at(vm, subscope_block(node), "a block cannot follow this statement");
          return -1;
      }
    }

    // `a <- b` is the reactive engine's soft write (rvm/), a per-frame write
    // through its manager; an ordinary program has no frame to apply it in.
    if (node->token == TOK_ARROW_L) {
        vm_set_error_at(vm, node, "'<-' is a reactive soft write and only means something in the reactive engine; write a = b");
        return -1;
    }

    // Update operators: `a += b`, `a++`, `++a`, ... As a statement the yielded
    // value is discarded, so prefix and postfix are indistinguishable here and
    // all of them reduce to the plain assignment below.
    {
        ASTNode one;
        UpdateOp u = update_op_split(vm, node, &one);
        if (u.op) return compile_update_stmt(vm, f, &u, slot, count, cap);
    }

    // Assignment: token == '=' binop
    if (node->token == TOK_EQ && node->left && node->right) {
        // Function literal RHS? -> function declaration
        ASTNode *rhs_fn = paren_inner(node->right);
        if (rhs_fn && rhs_fn->token == TOK_ARROW_F && is_ident(node->left)) {
            if (!const_reject_write(vm, node->left, node->left->token, "used as a function name"))
                return -1;
            // Turning a variable's sym into a function's would leave the stores
            // already compiled against its frame slot writing past the frame.
            VMSym *prev = sym_find(f, node->left->token);
            if (prev && !prev->is_func && prev->type.kind != VMT_VOID) {
                vm_errorf_at(vm, node->left, "'%s' is already a variable; give the function its own name",
                             intern_get_cstr(vm->intern, node->left->token));
                return -1;
            }
            Func *g = compile_func_def(vm, node->left->token, rhs_fn);
            if (!g) return -1;
            // Register as a sym in current func so it's resolvable here too.
            VMSym *s = sym_find(f, node->left->token);
            if (!s) {
                VMType vt = { VMT_VOID, 0 };
                s = sym_add(vm, f, node->left->token, vt);
            }
            s->is_func = 1;
            s->fn = g;
            return 1;
        }

        // `NAME = struct { ... }` -- the same declaration as `struct NAME { ... }`.
        if (is_ident(node->left) && (node->right->token == TOK_EMPTYSTRING || node->right->token == 0)
            && ident_is(vm, node->right->left, "struct") && node->right->right)
            return compile_struct_decl(vm, f, node->left, node->right->right, node);

        // Annotated declaration: `name: T = expr`. A packed array's width is exactly
        // the thing inference cannot recover from an initialiser ([0,1,1,0] is a
        // perfectly good [4]i32), so it has to be stated.
        if (node->left->token == TOK_COLON && is_ident(node->left->left)) {
            int r = compile_annotated_decl(vm, f, node->left, node->right, slot, count, cap);
            return r;
        }

        // LHS: ident (declaration or simple assign) or index expression
        if (is_ident(node->left)) {
            InternID nm = node->left->token;
            // The one write to an auto-const's name, reached a SECOND time: a
            // template specialisation recompiles the same body, and the constant
            // made on the first pass is still on the unit's list. Eligibility
            // required exactly one write in the whole unit, so an assignment to a
            // name that is already an auto-const can only be that declaration
            // coming round again -- which has nothing left to do. Checked ahead of
            // const_reject_write, which would otherwise call it a write to a
            // constant. See VM_FLAG_AUTO_CONST.
            {
                VMConst *already = const_find(vm, nm);
                if (already && already->is_auto) return 1;
            }
            // Covers `k = 1` and, via compile_update_stmt's synthetic node,
            // `k += 1` / `k++` as statements.
            if (!const_reject_write(vm, node->left, nm, "assigned to")) return -1;
            if (struct_find(vm, nm)) {
                vm_errorf_at(vm, node->left, "'%s' is a struct type; declare an instance, as p: %s",
                             intern_get_cstr(vm->intern, nm), intern_get_cstr(vm->intern, nm));
                return -1;
            }
            // Bind a shared variable before considering a declaration: plain
            // sym_find would miss it and the "declaration on first assign"
            // branch below would then quietly create a local that shadows it,
            // so the write would go nowhere anyone else could see it.
            VMSym *s0 = sym_find(f, nm);
            if (!s0) s0 = sym_bind_global(vm, f, nm);
            if (vm->bind_failed) return -1;
            // A host buffer is bound here for the same reason a shared variable is
            // -- so "declaration on first assign" below cannot quietly create a
            // local that shadows it -- but the answer is an error rather than a
            // store: the name IS the host's storage, so rebinding it would point the
            // slice somewhere the host never gave us.
            if (!s0) {
                VMSym *hb = sym_bind_host_buf(vm, f, nm);
                if (hb) {
                    vm_errorf_at(vm, node->left,
                        "'%s' is a host buffer and cannot be assigned; write its elements "
                        "instead (%s[i] = ...)",
                        intern_get_cstr(vm->intern, nm), intern_get_cstr(vm->intern, nm));
                    return -1;
                }
            }
            // Index, not pointer, for the same reason as in compile_annotated_decl:
            // compile_expr below can add symbols, and sym_add reallocates f->syms,
            // which would leave `s0` pointing into the freed array. Binding a shared
            // variable above makes that far more likely.
            int si = s0 ? (int)(s0 - f->syms) : -1;
            // `p = { a: 1 }`: a struct literal. A new name infers its struct; an
            // existing instance (or a ref, which writes through) takes the literal
            // as its whole new value.
            {
                ASTNode *rin = paren_inner(node->right);
                if (is_brace(rin)) {
                    if (si < 0) return compile_inferred_struct(vm, f, node->left, rin, slot, count, cap);
                    VMType st0 = f->syms[si].type;
                    if (!vmt_is_record(st0) && !vmt_is_ref(st0)) {
                        vm_errorf_at(vm, node->right, "'%s' is not a struct; a { ... } literal can only "
                                     "initialise one", intern_get_cstr(vm->intern, nm));
                        return -1;
                    }
                    StructDest d;
                    vm->run.sys->memset(&d, 0, sizeof(d));
                    d.kind = SD_SLOT;
                    d.slot = si;
                    d.type = st0;
                    return struct_fill(vm, f, &d, st0.struct_id, rin, 1, node, slot, count, cap);
                }
            }
            VMType rt;
            int r = compile_expr(vm, f, node->right, &rt);
            if (r < 0 || reject_void(vm, node->right, rt)) return -1;
            VMSym *s = (si >= 0) ? &f->syms[si] : 0;
            if (s && vmt_is_ref(s->type)) {
                // A ref cannot be rebound: assigning to one copies into what it
                // refers to.
                int sid = s->type.struct_id;
                r = ref_to_value(vm, f, r, &rt);
                if (!vmt_is_record(rt) || !struct_ids_match(vm->structs, sid, vm->structs, rt.struct_id)) {
                    vm_errorf_at(vm, node->right, "'%s' is a ref %s; assign it a %s",
                                 intern_get_cstr(vm->intern, nm), struct_name_s(vm, sid), struct_name_s(vm, sid));
                    return -1;
                }
                VMType rec = struct_record_type(vm, sid);
                int base = sym_read(vm, f, si);
                int z = ir_i32(vm, f, 0);
                int lv = ir_field(vm, f, base, FIELD_ELEM, rec.len, z, rec);
                push_assign(vm, f, lv, r, slot, count, cap);
                return 1;
            }
            // Declaring from a ref copies the record, as a by-value parameter would.
            r = ref_to_value(vm, f, r, &rt);
            if (s && (vmt_is_struct(s->type) || vmt_is_struct(rt))) {
                if (!struct_types_same(vm, s->type, rt)) {
                    char desc[128];
                    node_describe(vm, node->right, desc, sizeof(desc));
                    vm_errorf_at(vm, node->right, "cannot assign %s to '%s': %s", desc,
                                 intern_get_cstr(vm->intern, nm),
                                 vmt_is_struct(s->type) ? "it holds a different struct" : "the value is a struct");
                    return -1;
                }
                push_assign(vm, f, sym_read(vm, f, si), r, slot, count, cap);
                return 1;
            }
            // A string build and a slice() both point into *this frame*, which
            // is gone by the time anyone else reads a shared variable. Caught
            // here rather than left to dangle: the value looks fine right up
            // until the next call reuses the stack.
            if (s && s->is_global && slice_escapes_frame(f, r)) {
                vm_errorf_at(vm, node->right,
                    "cannot store a built string in shared variable '%s': it points into this "
                    "call's frame and does not outlive it",
                    intern_get_cstr(vm->intern, nm));
                return -1;
            }
            if (!s) {
                // A declaration whose initialiser folded to a single constant, of
                // a name nothing else in the unit writes and which always runs:
                // no storage, no assignment, and every use re-emits this very
                // node. See VM_FLAG_AUTO_CONST.
                if ((f->nodes[r].op == IR_CONST_I || f->nodes[r].op == IR_CONST_F)
                    && autoconst_eligible(vm, nm)
                    && autoconst_declare(vm, f, nm, r, rt))
                    return 1;
                // Declaration on first assign.
                s = sym_add(vm, f, nm, ct_vmtype_clear_shift(rt));
                s->shift = ct_shift(rt);
                si = (int)(s - f->syms);
            } else {
                int rs = ct_shift(rt);
                // An aggregate on either side whose type does not already match.
                // The scalar checks below cannot judge this -- their widening
                // ladder has no rung for an array, so scalar_rank calls every
                // pair lossy -- and nothing downstream can lower the result
                // either: an array value assigned into a slice slot reaches the
                // interpreter as a bare IR_ARR_LIT ("array literal in expression
                // context") and the C emitter as a cast to a slice type, which
                // prints as `p = (void)(...)`. So it stops here, and
                // lossy_assignment does not open it up -- there is nothing on
                // the other side of that flag to allow.
                if ((is_array(s->type.kind) || is_slice(s->type.kind)
                     || is_array(rt.kind) || is_slice(rt.kind))
                    && (s->type.kind != rt.kind || s->shift != rs)) {
                    char rbuf[24], sbuf[24];
                    const char *rl = type_label(rt, rbuf, sizeof(rbuf));
                    const char *sl = type_label(ct_set_shift(s->type, s->shift), sbuf, sizeof(sbuf));
                    const char *name = intern_get_cstr(vm->intern, nm);
                    // A slice is {ptr, len} into storage someone else owns -- an
                    // array parameter's caller, most often. Rebinding it to an
                    // array value is the one shape worth naming, both because it
                    // is what an array parameter reassigned in its own body looks
                    // like and because the fix is not a cast. Same reasoning as
                    // the host-buffer rejection further up.
                    if (is_slice(s->type.kind))
                        vm_errorf_at(vm, node->right,
                            "cannot assign to '%s' (%s -> %s): a slice points at storage owned "
                            "elsewhere and cannot take an array of its own -- write its elements "
                            "instead (%s[i] = ...), or use a new name",
                            name, rl, sl, name);
                    else
                        vm_errorf_at(vm, node->right,
                            "cannot assign to '%s' (%s -> %s): the types do not match",
                            name, rl, sl);
                    return -1;
                }
                // Same kind, but a fixed array's length is part of its type: `v = [7, 8]`
                // into a [3] copied two elements and kept the third.
                if (is_array(s->type.kind) && is_array(rt.kind)
                    && (s->type.len != rt.len || s->type.inner_len != rt.inner_len)) {
                    char rbuf[24], sbuf[24];
                    vm_errorf_at(vm, node->right,
                        "cannot assign to '%s' (%s -> %s): the lengths differ",
                        intern_get_cstr(vm->intern, nm), type_label(rt, rbuf, sizeof(rbuf)),
                        type_label(ct_set_shift(s->type, s->shift), sbuf, sizeof(sbuf)));
                    return -1;
                }
                // Const is not part of `kind`, so the check above cannot see it:
                // repointing a mutable slice at a read-only view would launder
                // the permission away. The reverse is fine and needs no
                // conversion -- see coerce_call_args.
                if (is_slice(s->type.kind) && rt.is_const && !s->type.is_const) {
                    char rbuf[24], sbuf[24];
                    vm_errorf_at(vm, node->right,
                        "cannot assign to '%s' (%s -> %s): that would drop const. "
                        "Declare it as %s",
                        intern_get_cstr(vm->intern, nm),
                        type_label(rt, rbuf, sizeof(rbuf)),
                        type_label(ct_set_shift(s->type, s->shift), sbuf, sizeof(sbuf)),
                        type_label(rt, rbuf, sizeof(rbuf)));
                    return -1;
                }
                // Same policy as a local -- the conversion below writes the SLOT's
                // type -- but say "shared variable" and name the declaration: the
                // fix is usually to change how it was DECLARED, which is in a
                // different block entirely.
                if (s->is_global && !(vm->flags & VM_FLAG_LOSSY_ASSIGNMENT)
                    && (s->type.kind != rt.kind || s->shift != rs)
                    && is_lossy_assign(rt.kind, rs, s->type.kind, s->shift)) {
                    char rbuf[16], sbuf[16];
                    const char *rl = scalar_type_label(rt.kind, rs, rbuf, sizeof(rbuf));
                    const char *sl = scalar_type_label(s->type.kind, s->shift, sbuf, sizeof(sbuf));
                    vm_errorf_at(vm, node->right,
                        "assigning to shared variable '%s' loses information (%s -> %s) -- "
                        "declare it %s where it is shared, or #enable lossy_assignment%s",
                        intern_get_cstr(vm->intern, nm), rl, sl, rl,
                        int_div_hint(node->right, s->type.kind, s->shift));
                    return -1;
                }
                if (s->type.kind != rt.kind || s->shift != rs) {
                    if (!(vm->flags & VM_FLAG_LOSSY_ASSIGNMENT)
                        && is_lossy_assign(rt.kind, rs, s->type.kind, s->shift)) {
                        char rbuf[16], sbuf[16];
                        const char *rl = scalar_type_label(rt.kind, rs, rbuf, sizeof(rbuf));
                        const char *sl = scalar_type_label(s->type.kind, s->shift, sbuf, sizeof(sbuf));
                        vm_errorf_at(vm, node->right,
                            "assigning to '%s' loses information (%s -> %s); "
                            "#enable lossy_assignment to allow%s",
                            intern_get_cstr(vm->intern, nm), rl, sl,
                            int_div_hint(node->right, s->type.kind, s->shift));
                        return -1;
                    }
                    r = insert_fx_convert(vm, f, r, rt, ct_set_shift(s->type, s->shift));
                }
            }
            // Re-read through `si`: insert_fx_convert may itself have added a
            // symbol, so `s` is not guaranteed live past this point.
            int lv = ir_new(vm, f, IR_LOCAL);
            f->nodes[lv].type = f->syms[si].type;
            f->nodes[lv].ki = si;
            int ir = ir_new(vm, f, IR_ASSIGN);
            f->nodes[ir].a = lv; f->nodes[ir].b = r;
            stmt_push_idx(vm, slot, count, cap, ir);
            return 1;
        }
        // Field assignment: p.x = v, p.inner = q, p.buf = [..], a.x = v through a ref.
        ASTNode *lhs = node->left;
        if (lhs->token == TOK_DOT && lhs->left && lhs->right) {
            int sw = compile_swizzle_store(vm, f, node, slot, count, cap);
            if (sw != 0) return sw;
            VMType lt;
            int lv = compile_expr(vm, f, lhs, &lt);
            if (lv < 0) return -1;
            if (f->nodes[lv].op != IR_FIELD) {
                char desc[128];
                node_describe(vm, lhs, desc, sizeof(desc));
                vm_errorf_at(vm, lhs, "unsupported assignment target %s; only a struct field can be assigned", desc);
                return -1;
            }
            if (!struct_lvalue_ok(f, f->nodes[lv].a)) {
                vm_set_error_at(vm, lhs, "cannot assign to a field of a temporary; assign it to a variable first");
                return -1;
            }
            // `s[0].x = 9` where s is a const[]my: compile_index handed the
            // element record the container's const, so it is right here on the
            // base of this field.
            if (const_lvalue_reject(vm, f, lv, lhs) < 0) return -1;
            ASTNode *rin = paren_inner(node->right);
            int sub = f->nodes[lv].sub_op;
            const char *fname = is_ident(paren_inner(lhs->right))
                              ? intern_get_cstr(vm->intern, paren_inner(lhs->right)->token) : "?";
            int r;
            if (sub < FIELD_AGG) {
                VMType ft = lt;
                r = struct_scalar_value(vm, f, node->right, ft, fname);
            } else if ((vmt_is_record(lt) && is_brace(rin)) || (vmt_is_record_array(lt) && is_bracket(rin))) {
                // The target is rebuilt once per element written, so it must be
                // safe to evaluate more than once.
                if (ir_has_side_effects(f, lv)) {
                    vm_set_error_at(vm, lhs, "a literal assigned here would evaluate the target once per "
                                             "field; assign it to a variable first");
                    return -1;
                }
                StructDest d;
                vm->run.sys->memset(&d, 0, sizeof(d));
                d.kind = SD_AST;
                d.ast  = lhs;
                d.type = lt;
                if (vmt_is_record(lt))
                    return struct_fill(vm, f, &d, lt.struct_id, rin, 1, node, slot, count, cap);
                return struct_fill_array(vm, f, &d, lt.struct_id, lt.len / lt.inner_len, rin, 1, node,
                                         slot, count, cap);
            } else if (vmt_is_record(lt)) {
                r = struct_record_value(vm, f, node->right, lt.struct_id, fname);
            } else if (vmt_is_record_array(lt)) {
                VMType rt;
                r = compile_expr(vm, f, node->right, &rt);
                if (r >= 0 && !struct_types_same(vm, rt, lt)) {
                    vm_errorf_at(vm, node->right, "field '%s' is [%d]%s; got a different array", fname,
                                 lt.len / lt.inner_len, struct_name_s(vm, lt.struct_id));
                    return -1;
                }
            } else {
                r = struct_array_value(vm, f, node->right, lt, fname);
            }
            if (r < 0) return -1;
            push_assign(vm, f, lv, r, slot, count, cap);
            return 1;
        }
        // Index assignment: a[i] = expr
        if (lhs->token == TOK_EMPTYSTRING && is_bracket(lhs->right)) {
            // `x[a..b] = y` -- a slice is a view, and there is no whole-view
            // store; the elements have to be written one at a time.
            if (is_range_node(index_expr(lhs))) {
                vm_set_error_at(vm, lhs, "cannot assign to a slice; write its elements, "
                                         "e.g. for(i in a..b) { x[i] = ... }");
                return -1;
            }
            VMType lt;
            int idx = compile_index(vm, f, lhs, &lt);
            if (idx < 0) return -1;
            // Const is asked of the thing being indexed, not chased back to some
            // declaring symbol: `p[0]` of a const[] is a const element wherever
            // `p` came from, so a copy into a local cannot launder it. See
            // const_lvalue_reject.
            if (const_lvalue_reject(vm, f, idx, lhs) < 0) return -1;
            // arr[i] = p / arr[i] = { ... }: a whole record.
            if (f->nodes[idx].op == IR_FIELD) {
                if (!struct_lvalue_ok(f, f->nodes[idx].a)) {
                    vm_set_error_at(vm, lhs, "cannot assign to an element of a temporary; assign it to a variable first");
                    return -1;
                }
                ASTNode *rin = paren_inner(node->right);
                if (is_brace(rin)) {
                    if (ir_has_side_effects(f, idx)) {
                        vm_set_error_at(vm, lhs, "a literal assigned here would evaluate the target once per "
                                                 "field; assign it to a variable first");
                        return -1;
                    }
                    StructDest d;
                    vm->run.sys->memset(&d, 0, sizeof(d));
                    d.kind = SD_AST;
                    d.ast  = lhs;
                    d.type = lt;
                    return struct_fill(vm, f, &d, lt.struct_id, rin, 1, node, slot, count, cap);
                }
                int r = struct_record_value(vm, f, node->right, lt.struct_id, "each element");
                if (r < 0) return -1;
                push_assign(vm, f, idx, r, slot, count, cap);
                return 1;
            }

            // A nested array's `a[i]` is a row slice, not a storage location:
            // there is nowhere to put a whole row, so write its elements.
            if (f->nodes[idx].op != IR_INDEX) {
                vm_set_error_at(vm, lhs, "cannot assign a whole row of a nested array; assign a[i][j] instead");
                return -1;
            }
            // Inline array literal index is read-only; assignment is not supported.
            if (f->nodes[f->nodes[idx].a].op == IR_ARR_LIT) { vm_set_error_at(vm, lhs->left, "cannot assign to inline array literal"); return -1; }
            VMType rt;
            int r = compile_expr(vm, f, node->right, &rt);
            if (r < 0) return -1;
            if (rt.kind != lt.kind || ct_shift(rt) != ct_shift(lt))
                r = insert_fx_convert(vm, f, r, rt, lt);
            int ir = ir_new(vm, f, IR_ASSIGN);
            f->nodes[ir].a = idx; f->nodes[ir].b = r;
            stmt_push_idx(vm, slot, count, cap, ir);
            return 1;
        }
        char desc[128];
        node_describe(vm, node->left, desc, sizeof(desc));
        vm_errorf_at(vm, node->left, "unsupported assignment target %s", desc);
        return -1;
    }

    // Otherwise: expression statement (e.g. a call)
    VMType et;
    int e = compile_expr(vm, f, node, &et);
    if (e < 0) return -1;
    int ir = ir_new(vm, f, IR_EXPR_STMT);
    f->nodes[ir].a = e;
    stmt_push_idx(vm, slot, count, cap, ir);
    return 1;
}

// The heads that take a block. Anything else carrying an indent block picked it
// up from alignment: indentation meant nothing before this syntax existed, and
// scripts are full of continuation lines set in from the margin, so those become
// plain sibling statements again rather than an error. A brace block on such a
// head still is one -- braces were always meaningful.
static int head_takes_block(VM *vm, ASTNode *stmt) {
    ASTNode *args[8];
    int n = 0;
    ASTNode *head = find_kw_head(vm, stmt, args, &n, 8);
    if (!head) return 0;
    return ident_is(vm, head, "if") || ident_is(vm, head, "else")
        || ident_is(vm, head, "while") || ident_is(vm, head, "for");
}

static void stmt_push_node(VM *vm, ASTNode ***slot, int *count, int *cap, ASTNode *n) {
    MEM_GROW(vm, &vm->run.mem, *slot, *count, *cap, *count + 1, 16);
    (*slot)[(*count)++] = n;
}

// Flatten one statement into the list the block compiles: a cosmetic indent
// block contributes its head and then its lines, each flattened in turn.
// Returns 0 once an error has been set, which only #enable strict_indent does.
static int flatten_stmt(VM *vm, ASTNode *st, ASTNode ***slot, int *count, int *cap) {
    ASTNode *u = unwrap_chain(st);
    ASTNode *blk = u ? subscope_block(u) : 0;
    if (blk && is_indent_block(blk) && !head_takes_block(vm, u)) {
        // A directive is exempt: the flag consulted here may be the one that
        // very line is setting, and no directive takes a block in any form, so
        // an indented line under one can only ever have been alignment.
        if ((vm->flags & VM_FLAG_STRICT_INDENT) && !stmt_is_directive(u->left)) {
            // Positioned on the block, like the brace-block form of this error
            // above: the block node itself maps to no text, so the fallback in
            // vm_node_offset lands on the first token inside it -- the line
            // that is wrongly indented, which is the one to point at.
            vm_set_error_at(vm, blk,
                            "indented block under a statement that takes none "
                            "(#disable strict_indent to allow it as alignment)");
            return 0;
        }
        if (!flatten_stmt(vm, u->left, slot, count, cap)) return 0;
        for (size_t i = 0; i < blk->items.size; i++)
            if (!flatten_stmt(vm, *(ASTNode**)array_get(&blk->items, i), slot, count, cap))
                return 0;
        return 1;
    }
    // The same block, one shape further in: on a directive of more than one word the
    // parser hangs it off the innermost step of the chain, too deep for the check
    // above to see. Nothing is rewritten -- the tree still has to unparse byte-exact
    // -- so the statement goes in as it stands and the block's lines follow as
    // siblings.
    ASTNode *dblk = directive_indent_block(st);
    if (dblk) {
        stmt_push_node(vm, slot, count, cap, st);
        for (size_t i = 0; i < dblk->items.size; i++)
            if (!flatten_stmt(vm, *(ASTNode**)array_get(&dblk->items, i), slot, count, cap))
                return 0;
        return 1;
    }
    stmt_push_node(vm, slot, count, cap, st);
    return 1;
}

// Depth 1 is the body's own statement list; anything deeper runs conditionally.
// The count is what auto_const asks "does this statement always run?" with, so it
// has to wrap EVERY entry -- including the ones compile_stmt_into makes for a
// loop's step clause. See VM.blk_depth.
static int compile_block_at_depth(VM *vm, Func *f, ASTNode *block);

static int compile_block(VM *vm, Func *f, ASTNode *block) {
    vm->blk_depth++;
    int r = compile_block_at_depth(vm, f, block);
    vm->blk_depth--;
    return r;
}

static int compile_block_at_depth(VM *vm, Func *f, ASTNode *block) {
    // block may be a brace node OR the top-level items wrapper.
    block = unwrap_chain(block);
    if (!block) return -1;
    int ir = ir_new(vm, f, IR_BLOCK);
    int *items = 0; int count = 0, cap = 0;
    ASTNode **stmts = 0; int n_stmts = 0, stmts_cap = 0;
    if (block->items.size) {
        for (size_t i = 0; i < block->items.size; i++)
            if (!flatten_stmt(vm, *(ASTNode**)array_get(&block->items, i), &stmts, &n_stmts, &stmts_cap))
                return -1;
    } else if (!is_block_node(block)) {
        // Single statement block (no braces, e.g. `while(x) stmt;`). It goes through
        // flatten_stmt too: a lone statement can carry a cosmetic indent block just
        // as well as one in a list. Do NOT recurse here for a brace node with no
        // items -- that is simply an empty block `{}` and should produce an empty
        // IR_BLOCK.
        if (!flatten_stmt(vm, block, &stmts, &n_stmts, &stmts_cap)) return -1;
    }
    for (int i = 0; i < n_stmts; i++) {
        ASTNode *st = stmts[i];
        // Check for compile directives everywhere, not just at top level.
        ASTNode *chain[16];
        int nn = unpack_directive(st, chain, 16);
        if (nn > 0) {
            if (!compile_directive(vm, chain, nn, st)) return -1;
            continue;
        }
        // An `if` claims a following `else` sibling: the parser hands the
        // two over as separate statements (see find_else_head).
        ASTNode *kw_args[8];
        int kw_n = 0;
        ASTNode *head = find_kw_head(vm, unwrap_chain(st), kw_args, &kw_n, 8);
        if (head && ident_is(vm, head, "if") && i + 1 < n_stmts) {
            ASTNode *else_args[8];
            int else_n = 0;
            ASTNode *nxt = stmts[i + 1];
            if (find_else_head(vm, unwrap_chain(nxt), else_args, &else_n, 8) && else_n > 0) {
                // With braces the whole `else if ... else ...` run is one
                // statement; indentation dedents each `else` into another
                // sibling. Gather the run so both forms hand compile_if the
                // same chain -- each further else can only follow an `else if`.
                int consumed = 1;
                ASTNode *tail = else_args[0];
                while (else_n < 8 && i + 1 + consumed < n_stmts) {
                    ASTNode *ta[8]; int tn = 0;
                    ASTNode *th = find_kw_head(vm, unwrap_chain(tail), ta, &tn, 8);
                    if (!th || !ident_is(vm, th, "if")) break;
                    ASTNode *more = stmts[i + 1 + consumed];
                    ASTNode *ma[8]; int mn = 0;
                    if (!find_else_head(vm, unwrap_chain(more), ma, &mn, 8) || mn == 0) break;
                    else_args[else_n++] = unwrap_chain(more);
                    tail = ma[0];
                    consumed++;
                }
                int ir = compile_if(vm, f, unwrap_chain(st), kw_args, kw_n, else_args, else_n);
                if (ir < 0) return -1;
                stmt_push_idx(vm, &items, &count, &cap, ir);
                i += consumed;   // the `else` statements are now part of this `if`
                continue;
            }
        }
        if (compile_stmt_into(vm, f, st, &items, &count, &cap) < 0) return -1;
    }
    if (count > 0) { f->nodes[ir].items_begin = ir_alloc_items(vm, f, count); for (int _cp = 0; _cp < count; _cp++) f->child_indices[f->nodes[ir].items_begin + _cp] = items[_cp]; }
f->nodes[ir].n_items = count;
    return ir;
}

// ---------- template specialisation ----------

// The param AST node for template slot `i`, with any `= default` stripped.
static ASTNode *template_param_node(Func *tmpl, int i) {
    ASTNode *params_ast = tmpl->template_ast->left;
    if (is_ident(params_ast)) return params_ast;
    ASTNode *list[VM_MAX_PARAMS + 1];
    int n = param_list(params_ast, list, VM_MAX_PARAMS);
    if (i >= n || i >= VM_MAX_PARAMS) return 0;
    ASTNode *pdef;
    return param_split_default(paren_inner(list[i]), &pdef);
}

// Effective param type for template slot `i`. An annotated param keeps its
// declared type -- a signature may mix `(a:fx16, b)`, and `a` must stay fx16
// no matter what the call site passes. An unannotated one takes the call-site
// type, with a fixed array becoming a SLICE of it -- the length is kept, so this
// is not a loss of information and find_or_specialize_body still keys on it: one
// specialisation per length, `.len` folding, auto-vec against a known count.
//
// What the slice buys is that an unannotated aggregate param is by REFERENCE,
// always and regardless of the call site. An annotated `(p: [2]i32)` is a C
// struct by value -- writes to `p` stay in the callee and every call copies the
// whole array -- while `(p: []i32)` is {ptr, len}. Both are reachable by writing
// the annotation; the bare `(p)` has to pick one, and by-reference is the only
// one that can pick correctly, because:
//   - an untyped param can also be handed a genuine slice (`f(a[0..n])`), and
//     inferring array-from-array would give the SAME body by-value semantics for
//     `f(a)` and by-reference semantics for `f(a[0..2])`;
//   - by-value would silently stop `f(buf)` mutating buf, with no diagnostic;
//   - by-value copies the array at every call, which on the small targets is the
//     difference between a sum over [1024]f64 and 8KB of stack traffic per call.
// The cost is that a slice cannot be rebound to an array of its own, so `p = ...`
// in the body is an error -- see the message at the aggregate-assign check.
// The call-site type as a VIEW of the caller's storage: what `[]`, `const[]` and
// a bare parameter all resolve to. A struct becomes a ref (an array of them, a
// []T), which is what an array argument has always been.
static VMType template_view_of(VMType at, int is_const) {
    VMType out = at;
    if (is_array(out.kind)) out.kind = slice_of(arr_elem(out.kind));
    if (out.struct_id) out.len = 0;
    out.is_const = is_const;
    return out;
}

// One of the inferred-shape hints resolved against the call-site type `at`:
//
//   []        a writable view of the caller's storage
//   const[]   a read-only one
//   [_]       a private copy, length taken from the call site
//   [N]       a private copy, length pinned
//
// All four leave the ELEMENT type to the call site -- that is what separates
// them from `[]i32` and `[2]i32`, which pin it. Returns 1 when `ty` was a hint
// and `out` now holds its type, 0 when it was not one (an ordinary annotation,
// for parse_type to read), -1 when it was a hint that does not fit this call.
// The shape test on its own, with no call site to resolve against: an annotated
// parameter that is a hint still makes the function a TEMPLATE, so compile_func_def
// has to recognise one before it validates annotations eagerly.
//
// `*is_const` and `*shape` are the details when it returns 1:
//   HINT_VIEW (-2)  `[]` / `const[]`  -- the caller's storage, is_const says which
//   HINT_COPY (-1)  `[_]`             -- a private copy, length from the call site
//   >= 1            `[N]`             -- a private copy of exactly that many
#define HINT_VIEW (-2)
#define HINT_COPY (-1)
static int type_is_shape_hint(VM *vm, ASTNode *ty, int *is_const, int *shape) {
    ASTNode *t  = unwrap_chain(ty);
    ASTNode *br = 0;
    int cst = 0;
    if (is_bracket(t)) br = t;
    else if (t && (t->token == TOK_EMPTYSTRING || t->token == 0)
             && ident_is(vm, t->left, "const") && is_bracket(t->right)
             && t->right->items.size == 0) { br = t->right; cst = 1; }
    if (!br) return 0;
    int sh;
    if (br->items.size == 0) {
        sh = HINT_VIEW;
    } else if (br->items.size == 1) {
        // `[]i32` and `[2]i32` are not hints: they name the element type, so the
        // annotation is impl(bracket, elem) and the bracket never arrives alone.
        ASTNode *a0 = paren_inner(*(ASTNode**)array_get(&br->items, 0));
        if (ident_is(vm, a0, "_")) sh = HINT_COPY;
        else if (is_number(a0) && (a0->number_flags & NUM_TYPE) == NUM_INTEGER && a0->number >= 1)
            sh = (int)a0->number;
        else return 0;                                // [n], [x+1], ... not a hint
    } else {
        return 0;
    }
    if (cst && sh != HINT_VIEW) return 0;             // const[N] -- parse_type reports it
    if (is_const) *is_const = cst;
    if (shape)    *shape    = sh;
    return 1;
}

static int template_hint_type(VM *vm, ASTNode *ty, VMType at, VMType *out) {
    int is_const = 0, shape = HINT_VIEW;
    if (!type_is_shape_hint(vm, ty, &is_const, &shape)) return 0;
    if (shape == HINT_VIEW) {
        *out = template_view_of(at, is_const);
        return 1;
    }
    const char *spelled = shape == HINT_COPY ? "[_]" : "[N]";
    if (!is_array(at.kind) && !is_slice(at.kind)) {
        vm_errorf_at(vm, ty, "a %s parameter takes an array; this argument is not one", spelled);
        return -1;
    }
    // A copy, so the argument has to BE an array. There is no slice-to-array
    // copy at a call boundary -- coerce_call_args has no such path -- so say
    // that here rather than letting it fall out as "incompatible argument type".
    if (is_slice(at.kind)) {
        vm_errorf_at(vm, ty, "a %s parameter takes a copy, and a slice cannot be copied into one; "
                             "pass the array itself, or annotate the parameter [] to share it", spelled);
        return -1;
    }
    int count = at.inner_len ? at.len / at.inner_len : at.len;
    if (shape >= 1 && count != shape) {
        vm_errorf_at(vm, ty, "this parameter is [%d] but the argument has %d elements", shape, count);
        return -1;
    }
    *out = at;
    out->is_const = 0;   // a copy is the callee's own; nothing to protect
    return 1;
}

static int template_param_type(VM *vm, Func *tmpl, int i, VMType at, VMType *out) {
    ASTNode *p  = template_param_node(tmpl, i);
    ASTNode *ty = p ? param_type_of(p) : 0;
    if (ty) {
        int h = template_hint_type(vm, ty, at, out);
        if (h) return h > 0;
        if (!parse_type(vm, ty, out, 1) || out->kind == VMT_VOID) {
            if (!vm->run.last_error) vm_set_error_at(vm, ty, "bad param type");
            return 0;
        }
        return 1;
    }
    // An unannotated aggregate parameter is `const[]`: the caller's storage,
    // read-only. Writing to it is the error the annotation exists to resolve --
    // `[]` to write through, `[_]` for a copy.
    //
    // Structs are left mutable. `ref T` is already their explicit by-reference
    // form and an unannotated struct parameter has always been one, so tightening
    // them is a separate change with its own migration.
    *out = template_view_of(at, is_slice(at.kind) || (is_array(at.kind) && !at.struct_id));
    return 1;
}

static Func *find_or_specialize_body(VM *vm, Func *tmpl, int n_args, const VMType *atypes);

// ---------- straight-line cleanup, once a body has compiled ----------
//
// Unrolling leaves what a loop kept in variables as a run of statements:
// `t = 0; t = t + xs[0]; t = t + xs[1]; return t`. Two passes turn that into
// `return xs[0] + xs[1]`: an unrolled loop variable nothing else reads loses its
// stores, as does `t = t` (what `t = t + 0` folds to), and -- in a library body,
// whose source nobody reads -- a value stored and then read once by the next
// statement moves into that read.

// Reads, arithmetic and selects: nothing a statement could run differently for.
static int sl_pure(Func *f, int ir) {
    if (ir < 0) return 1;
    IRNode *n = &f->nodes[ir];
    switch (n->op) {
        case IR_CONST_I: case IR_CONST_F: case IR_LOCAL:
            return 1;
        case IR_BINOP: case IR_UNOP: case IR_CVT: case IR_INDEX: case IR_SELECT: case IR_LEN: case IR_BITCAST:
            return sl_pure(f, n->a) && sl_pure(f, n->b) && sl_pure(f, n->c);
        default:
            return 0;
    }
}

static int sl_mentions(Func *f, int ir, int slot) {
    if (ir < 0) return 0;
    IRNode *n = &f->nodes[ir];
    int c = n->op == IR_LOCAL && (int)n->ki == slot;
    c += sl_mentions(f, n->a, slot) + sl_mentions(f, n->b, slot) + sl_mentions(f, n->c, slot);
    if (n->items_begin >= 0)
        for (int i = 0; i < n->n_items; i++) c += sl_mentions(f, f->child_indices[n->items_begin + i], slot);
    return c;
}

static void sl_remove(Func *f, int blk, int i) {
    IRNode *b = &f->nodes[blk];
    for (int j = i; j + 1 < b->n_items; j++)
        f->child_indices[b->items_begin + j] = f->child_indices[b->items_begin + j + 1];
    b->n_items--;
}

// The slot a statement stores a plain numeric local into, or -1.
static int sl_store_slot(Func *f, int st) {
    if (f->nodes[st].op != IR_ASSIGN || f->nodes[st].a < 0) return -1;
    IRNode *lv = &f->nodes[f->nodes[st].a];
    if (lv->op != IR_LOCAL) return -1;
    VMSym *s = &f->syms[lv->ki];
    if (s->is_global || s->is_host_buf || vmt_is_struct(s->type)) return -1;
#if VM_REACTIVE
    if (s->is_extern) return -1;
#endif
    VTKind k = s->type.kind;
    return (k == VMT_I32 || k == VMT_I64 || k == VMT_F32 || k == VMT_F64) ? (int)lv->ki : -1;
}

// `ir` with its one read of `slot` replaced by `e`, rebuilt through the folding
// constructors so `0 + x` comes out as `x`.
static int sl_subst(VM *vm, Func *f, int ir, int slot, int e) {
    if (ir < 0) return ir;
    IRNode n = f->nodes[ir];
    if (n.op == IR_LOCAL) return (int)n.ki == slot ? e : ir;
    int a = sl_subst(vm, f, n.a, slot, e);
    int b = sl_subst(vm, f, n.b, slot, e);
    int c = sl_subst(vm, f, n.c, slot, e);
    if (a == n.a && b == n.b && c == n.c) return ir;
    switch (n.op) {
        case IR_BINOP:  return ir_binop(vm, f, n.sub_op, a, b, n.type);
        case IR_UNOP:   return ir_unop(vm, f, n.sub_op, a, n.type);
        case IR_SELECT: return ir_select(vm, f, a, b, c, n.type);
        case IR_CVT: {
            VMType from = f->nodes[a].type;
            return insert_cvt_t(vm, f, a, from, n.type);
        }
        default:
            f->nodes[ir].a = a; f->nodes[ir].b = b; f->nodes[ir].c = c;
            return ir;
    }
}

// Is the value stored into `slot` by statement i of `blk` dead once statement
// i + 1 has run? Overwritten or returned past, or the end of the body.
static int sl_dead_after(Func *f, int blk, int i, int slot, int top) {
    IRNode *b = &f->nodes[blk];
    for (int j = i + 1; j < b->n_items; j++) {
        int st = f->child_indices[b->items_begin + j];
        if (sl_store_slot(f, st) == slot && !sl_mentions(f, f->nodes[st].b, slot)) return 1;
        if (sl_mentions(f, st, slot)) return 0;
        if (f->nodes[st].op == IR_RETURN) return 1;
    }
    return top;
}

// Statement i of `blk` stores E into a local the next statement reads exactly
// once: move E into that read and drop the store.
static int sl_forward(VM *vm, Func *f, int blk, int i, int top) {
    int s1 = f->child_indices[f->nodes[blk].items_begin + i];
    int s2 = f->child_indices[f->nodes[blk].items_begin + i + 1];
    int slot = sl_store_slot(f, s1);
    if (slot < 0) return 0;
    int e = f->nodes[s1].b;
    VMType st = ct_set_shift(f->syms[slot].type, f->syms[slot].shift);
    VMType et = f->nodes[e].type;
    if (!sl_pure(f, e) || et.kind != st.kind || (st.kind == VMT_I32 && et.len != st.len)) return 0;

    int use;
    if (f->nodes[s2].op == IR_RETURN) {
        use = f->nodes[s2].a;
    } else if (f->nodes[s2].op == IR_ASSIGN && f->nodes[s2].a >= 0) {
        int lv = f->nodes[s2].a;
        if (f->nodes[lv].op != IR_LOCAL && !(f->nodes[lv].op == IR_INDEX && sl_pure(f, lv))) return 0;
        if (f->nodes[lv].op == IR_INDEX && sl_mentions(f, lv, slot)) return 0;
        use = f->nodes[s2].b;
    } else {
        return 0;
    }
    if (use < 0 || !sl_pure(f, use) || sl_mentions(f, use, slot) != 1) return 0;
    if (sl_store_slot(f, s2) != slot && f->nodes[s2].op != IR_RETURN && !sl_dead_after(f, blk, i + 1, slot, top))
        return 0;

    int nu = sl_subst(vm, f, use, slot, e);
    if (f->nodes[s2].op == IR_RETURN) f->nodes[s2].a = nu;
    else                              f->nodes[s2].b = nu;
    sl_remove(f, blk, i);
    return 1;
}

static void sl_block(VM *vm, Func *f, int blk, int top, int forward) {
    if (blk < 0 || f->nodes[blk].op != IR_BLOCK) return;
    for (int i = 0; i < f->nodes[blk].n_items; i++) {
        int st = f->child_indices[f->nodes[blk].items_begin + i];
        IRNode *n = &f->nodes[st];
        if (n->op == IR_IF)                         { sl_block(vm, f, n->b, 0, forward); sl_block(vm, f, f->nodes[st].c, 0, forward); }
        else if (n->op == IR_WHILE || n->op == IR_FOR) sl_block(vm, f, n->b, 0, forward);
        else if (n->op == IR_BLOCK)                   sl_block(vm, f, st, 0, forward);
    }
    for (int i = 0; i < f->nodes[blk].n_items; ) {
        int st   = f->child_indices[f->nodes[blk].items_begin + i];
        int slot = sl_store_slot(f, st);
        int rv   = slot >= 0 ? f->nodes[st].b : -1;
        int self = rv >= 0 && f->nodes[rv].op == IR_LOCAL && (int)f->nodes[rv].ki == slot;
        if (self || (slot >= 0 && f->syms[slot].unrolled && !f->syms[slot].used && sl_pure(f, rv))) sl_remove(f, blk, i);
        else i++;
    }
    if (!forward) return;
    for (int i = 0; i + 1 < f->nodes[blk].n_items; ) {
        if (sl_forward(vm, f, blk, i, top)) { if (i > 0) i--; }
        else i++;
    }
}

static void straight_line_cleanup(VM *vm, Func *f, int forward) {
    sl_block(vm, f, f->body, 1, forward);
}

// A specialisation compiles a function body, so it starts its own block-depth
// count exactly as compile_func_def does -- it is reached from a call site,
// which is a statement inside some other body. See VM.blk_depth.
static Func *find_or_specialize(VM *vm, Func *tmpl, int n_args, const VMType *atypes) {
    int saved = vm->blk_depth;
    vm->blk_depth = 0;
    // A library template's body compiles HERE, at the call site, so this is
    // where its errors have to be attributed -- lib_at stays 0 so the position
    // falls back to err_ctx, the user's own statement. See VM.lib_depth.
    int lib_saved = vm->lib_depth;
    InternID lib_saved_nm = vm->lib_name;
    ASTNode *lib_saved_at = vm->lib_at;
    if (tmpl->is_lib) { vm->lib_depth++; vm->lib_name = tmpl->name; vm->lib_at = 0; }
#if VM_REACTIVE
    vm->rx_func_depth++;
#endif
    int n_specs = tmpl->n_specs;
    Func *saved_scope = vm->cur_scope;
    vm->cur_scope = scope_of(tmpl);
    Func *sp = find_or_specialize_body(vm, tmpl, n_args, atypes);
    vm->cur_scope = saved_scope;
    // The spec was registered before its body compiled, for recursion: one that
    // failed must not be found by the next call site as if it had compiled.
    if (!sp) tmpl->n_specs = n_specs;
#if VM_REACTIVE
    vm->rx_func_depth--;
#endif
    vm->blk_depth = saved;
    vm->lib_depth = lib_saved;
    vm->lib_name  = lib_saved_nm;
    vm->lib_at    = lib_saved_at;
    return sp;
}

static Func *find_or_specialize_body(VM *vm, Func *tmpl, int n_args, const VMType *atypes) {
    // Resolve what each param slot actually becomes for this call.
    VMType ptypes[VM_MAX_PARAMS];
    for (int i = 0; i < n_args; i++)
        if (!template_param_type(vm, tmpl, i, atypes[i], &ptypes[i])) return 0;

    // Return an existing specialisation if the param types match.
    for (int i = 0; i < tmpl->n_specs; i++) {
        Func *sp = tmpl->specs[i];
        int match = 1;
        for (int j = 0; j < n_args; j++) {
            VMType want = sp->syms[sp->param_slot[j]].type;
            VMType have = ptypes[j];
            // Packing is part of the layout, so []u4 and []i32 need distinct
            // specialisations even though both are VMT_SLICE_I32.
            if (want.pack_bits != have.pack_bits) { match = 0; break; }
            // And a struct is more than its bytes: one specialisation per shape.
            if (want.inner_len != have.inner_len
                || !struct_ids_match(sp->structs, want.struct_id, vm->structs, have.struct_id)) { match = 0; break; }
            if (want.kind != have.kind || ct_shift(want) != ct_shift(have)) { match = 0; break; }
            // Length is part of the key for a SLICE too, not just an array: an
            // argument of known length keeps it (template_param_type), the body
            // folds `.len` and vectorizes against it, and a specialisation
            // compiled for [2] would be wrong for [3]. A run-time-length slice
            // has len 0 and gets its own specialisation, as before.
            if ((is_array(want.kind) || is_slice(want.kind)) && want.len != have.len) { match = 0; break; }
            // Const is deliberately NOT part of the key: a parameter's constness
            // comes from its annotation, which is the same for every call of this
            // template, so `have` always agrees with `want` and a comparison here
            // could only ever split specialisations that are identical. The
            // assert-shaped check is here to say so, not to filter.
            if (want.is_const != have.is_const) { match = 0; break; }
        }
        if (match) return sp;
    }

    // Build a new specialisation.
    Func *sp    = func_alloc(&vm->run);
    sp->name    = tmpl->name;
    sp->structs = vm->structs;
    sp->scope   = scope_of(tmpl);
    sp->lex_parent = tmpl->lex_parent;
    sp->n_params = n_args;
    func_register_id(&vm->run, sp);

    // Set up params from the template AST with the resolved types.
    for (int i = 0; i < n_args; i++) {
        ASTNode *p = template_param_node(tmpl, i);
        int is_rest = 0;
        ASTNode *pn = p ? param_name_of(p, &is_rest) : 0;
        if (!pn) {
            char desc[128];
            node_describe(vm, p, desc, sizeof(desc));
            vm_errorf_at(vm, p ? p : tmpl->template_ast, "bad param in template specialisation %s", desc);
            return 0;
        }
        InternID pname = pn->token;
        VMSym *s = sym_add(vm, sp, pname, ptypes[i]);
        s->shift = ct_shift(ptypes[i]);
        sp->param_slot[i] = sp->n_syms - 1;
    }

    // Register in tmpl->specs BEFORE compiling the body so that recursive
    // calls inside the body find this (partial) spec rather than re-entering.
    MEM_GROW(vm, &vm->run.mem, tmpl->specs, tmpl->n_specs, tmpl->cap_specs, tmpl->n_specs + 1, 4);
    tmpl->specs[tmpl->n_specs++] = sp;

    // Compile the body (mirrors the logic in compile_func_def).
    ASTNode *body = unwrap_chain(tmpl->template_ast->right);
    int  blk;
    if (is_block_node(body) || is_subscope(body)) {
        blk = compile_block(vm, sp, body);
        if (blk < 0) return 0;
        if (!apply_implicit_return(vm, sp, blk)) return 0;
    } else {
        VMType  et;
        int e = compile_expr(vm, sp, body, &et);
        if (e < 0) return 0;
        e = ref_to_value(vm, sp, e, &et);
        if (slice_escapes_frame(sp, e)) {
            vm_set_error_at(vm, body,
                "cannot return a string or slice built here: it points into this call's frame. "
                "Take a []u8 buffer as a parameter and fill that instead");
            return 0;
        }
        // A body whose expression has no value leaves the function void -- the
        // expression still runs, it just is not returned.
        int stmt;
        if (et.kind == VMT_VOID) {
            stmt = ir_new(vm, sp, IR_EXPR_STMT);
        } else {
            if (!note_return_type(vm, sp, et, body)) return 0;
            stmt = ir_new(vm, sp, IR_RETURN);
        }
        sp->nodes[stmt].a = e;
        blk            = ir_new(vm, sp, IR_BLOCK);
        sp->nodes[blk].items_begin = ir_alloc_items(vm, sp, 1);
        sp->child_indices[sp->nodes[blk].items_begin + 0]  = stmt;
        sp->nodes[blk].n_items   = 1;
    }
    sp->body = blk;
    if (!check_stray_break(vm, sp)) return 0;
    // Every return converts to the joined type here, once no further return can widen it.
    if (!finalize_return_types(vm, sp)) return 0;
    straight_line_cleanup(vm, sp, vm->lib_depth > 0);
    if (!sp->has_return) sp->ret_type.kind = VMT_VOID;
    layout_frame(sp);
    sp->flags = vm->flags | VM_FLAG_SNAPSHOT;
    if (tmpl->accel && !(vm->flags & VM_FLAG_SOURCE_BUILTINS) && lib_accel_kind(sp) != VMT_VOID)
        sp->accel = tmpl->accel;
    return sp;
}

// type_label, plus the struct shapes it cannot spell: `P`, `[3]P`, `[]P`, `ref P`.
static const char *param_type_label(VM *vm, VMType t, char *buf, int buf_sz) {
    if (!vmt_is_struct(t)) return type_label(t, buf, buf_sz);
    const char *nm = struct_name_s(vm, t.struct_id);
    int p = 0;
    buf[0] = 0;
    if (is_slice(t.kind)) {
        s_strcpy(buf, t.inner_len ? "[]" : "ref ");
        p = (int)s_strlen(buf);
    } else if (t.inner_len) {
        buf[p++] = '[';
        label_uint(buf, buf_sz, &p, t.len / t.inner_len);
        buf[p++] = ']';
        buf[p] = 0;
    }
    if (p + (int)s_strlen(nm) < buf_sz) s_strcat(buf, nm);
    return buf;
}

static void sig_cat(char *out, int out_max, int *p, const char *s) {
    int n = (int)s_strlen(s);
    if (*p + n >= out_max) { *p = out_max; return; }
    s_strcpy(out + *p, s);
    *p += n;
}

// "(a: i32, b: f64) => f64", or 0 when it does not fit.
static int signature_label(VM *vm, Func *f, char *out, int out_max) {
    int p = 0;
    char buf[64];
    out[0] = 0;
    sig_cat(out, out_max, &p, "(");
    for (int i = 0; i < f->n_params; i++) {
        VMSym *s = &f->syms[f->param_slot[i]];
        if (i) sig_cat(out, out_max, &p, ", ");
        sig_cat(out, out_max, &p, intern_get_cstr(vm->intern, s->name));
        sig_cat(out, out_max, &p, ": ");
        sig_cat(out, out_max, &p, param_type_label(vm, s->type, buf, (int)sizeof(buf)));
    }
    sig_cat(out, out_max, &p, ") => ");
    sig_cat(out, out_max, &p, f->ret_type.kind == VMT_VOID ? "void" : param_type_label(vm, f->ret_type, buf, (int)sizeof(buf)));
    return p < out_max;
}

int vm_signature_labels(VM *vm, const ASTNode *node, char *out, int out_max) {
    if (!out || out_max <= 0) return 0;
    out[0] = 0;
    if (!vm || !node) return 0;
    int w = 0, n_seen = 0;
    int seen_at[32], seen_len[32];
    for (Func *t = vm->run.funcs; t; t = t->next) {
        ASTNode *arrow = t->template_ast;
        if (!arrow || t->is_lib) continue;
        const ASTNode *c = node;
        while (c && c->parent != arrow) c = c->parent;
        if (!c || c != arrow->left) continue;
        int n_bodies = t->is_template ? t->n_specs : 1;
        for (int s = 0; s < n_bodies && n_seen < 32; s++) {
            char lb[256];
            if (!signature_label(vm, t->is_template ? t->specs[s] : t, lb, (int)sizeof(lb))) continue;
            int n = (int)s_strlen(lb), dup = 0;
            for (int k = 0; k < n_seen && !dup; k++)
                dup = seen_len[k] == n && s_strncmp(out + seen_at[k], lb, (size_t)n) == 0;
            if (dup || w + n + 3 > out_max) continue;
            if (n_seen) sig_cat(out, out_max, &w, ", ");
            seen_at[n_seen] = w;
            seen_len[n_seen++] = n;
            sig_cat(out, out_max, &w, lb);
        }
    }
    return n_seen;
}

// ---------- function definition ----------

Func *func_alloc(VmRun *run) {
    Func *f = (Func*)mem_alloc(&run->mem, sizeof(Func));
    run->sys->memset(f, 0, sizeof(Func));
    f->run = run;
    f->ret_type.kind = VMT_VOID;
    f->func_id = -1;
    return f;
}

void func_register_id(VmRun *run, Func *f) {
    if (f->func_id >= 0) return;
    MEM_GROW((VM*)run, long_mem((VM*)run), run->funcs_by_id, run->n_func_ids, run->cap_func_ids, run->n_func_ids + 1, 16);
    f->func_id = run->n_func_ids;
    run->funcs_by_id[run->n_func_ids++] = f;
}

// ---------- auto-packing of never-written constant arrays ----------
//
// Explicit uN types are the feature; this is an optimisation on top, and it runs
// under one strict precondition: the array is never written. Inferring a width from
// an initialiser alone would be a typing rule, and a bad one -- `x = [0,1,1,0]`
// would become [4]u1 and then `x[0] = 5` has no good answer.
//
// Runs after the whole body is compiled, so it sees every access to every local, and
// before layout_frame, so the smaller slot is what gets allocated.

// Narrowest supported width holding every value in 0 .. max, or 0 if none does.
static int auto_pack_width(long long max_val) {
    if (max_val < 0)  return 0;
    if (max_val < 2)  return 1;
    if (max_val < 4)  return 2;
    if (max_val < 16) return 4;
    if (max_val < 256) return 8;
    if (max_val < 65536) return 16;
    return 0;
}

// True if `slot` is written by any assignment other than node `except_assign`
// (its own initialiser, which is not a mutation of an existing value).
static int slot_is_written(Func *f, int slot, int except_assign) {
    for (int i = 0; i < f->n_nodes; i++) {
        if (i == except_assign) continue;
        if (f->nodes[i].op != IR_ASSIGN) continue;
        IRNode *lv = &f->nodes[f->nodes[i].a];
        if (lv->op == IR_LOCAL && (int)lv->ki == slot) return 1;
        if (lv->op == IR_INDEX) {
            IRNode *base = &f->nodes[lv->a];
            if (base->op == IR_LOCAL && (int)base->ki == slot) return 1;
        }
    }
    return 0;
}

// Locate the single assignment initialising `slot` from an all-constant array
// literal. Writes the IR_ARR_LIT node index to *out_lit and returns the
// IR_ASSIGN node index, or -1 if there is no such unique initialiser.
static int slot_const_init(Func *f, int slot, int *out_lit) {
    int found = -1;
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op != IR_ASSIGN) continue;
        IRNode *lv = &f->nodes[f->nodes[i].a];
        if (lv->op != IR_LOCAL || (int)lv->ki != slot) continue;
        if (found >= 0) return -1;
        int rv = f->nodes[i].b;
        if (f->nodes[rv].op != IR_ARR_LIT) return -1;
        for (int k = 0; k < f->nodes[rv].n_items; k++)
            if (f->nodes[f->child_indices[f->nodes[rv].items_begin + k]].op != IR_CONST_I)
                return -1;
        found = i;
        *out_lit = rv;
    }
    return found;
}

static int slot_is_param(Func *f, int slot) {
    for (int p = 0; p < f->n_params; p++) if (f->param_slot[p] == slot) return 1;
    return 0;
}

static int is_local_of(Func *f, int idx, int slot) {
    return idx >= 0 && f->nodes[idx].op == IR_LOCAL && (int)f->nodes[idx].ki == slot;
}

// Every use of `slot` must be a read -- an IR_INDEX base or an IR_LEN operand --
// besides its own initialiser. A whitelist on purpose: returning the array, passing
// it to a call, or aliasing it into another local all hand the storage to code
// compiled against the *unpacked* layout, and each is a silent corruption rather
// than a diagnostic.
static int slot_uses_are_reads_only(Func *f, int slot, int init_assign) {
    for (int i = 0; i < f->n_nodes; i++) {
        if (i == init_assign) continue;
        IRNode *n = &f->nodes[i];
        int a_is_read = (n->op == IR_INDEX || n->op == IR_LEN);
        if (!a_is_read && is_local_of(f, n->a, slot)) return 0;
        if (is_local_of(f, n->b, slot)) return 0;
        if (is_local_of(f, n->c, slot)) return 0;
        if (n->items_begin >= 0)
            for (int k = 0; k < n->n_items; k++)
                if (is_local_of(f, f->child_indices[n->items_begin + k], slot)) return 0;
    }
    return 1;
}

static void auto_pack_arrays(VM *vm, Func *f) {
    if (!(vm->flags & VM_FLAG_AUTO_PACK)) return;
    for (int si = 0; si < f->n_syms; si++) {
        VMSym *s = &f->syms[si];
        if (s->is_func) continue;
        if (s->type.struct_id) continue;    // a record's bytes are not elements
        if (!is_array(s->type.kind) || s->type.pack_bits) continue;
        if (arr_elem(s->type.kind) != VMT_I32) continue;
        if (ct_shift(s->type) != 0) continue;     // fixed-point: not a small int
        if (s->type.len <= 0) continue;
        if (slot_is_param(f, si)) continue;
        int lit = -1;
        int init_assign = slot_const_init(f, si, &lit);
        if (init_assign < 0 || lit < 0) continue;
        if (f->nodes[lit].n_items != s->type.len) continue;
        if (slot_is_written(f, si, init_assign)) continue;
        if (!slot_uses_are_reads_only(f, si, init_assign)) continue;

        long long max_val = 0;
        for (int k = 0; k < f->nodes[lit].n_items; k++) {
            long long v = f->nodes[f->child_indices[f->nodes[lit].items_begin + k]].ki;
            if (v < 0) { max_val = -1; break; }
            if (v > max_val) max_val = v;
        }
        int bits = auto_pack_width(max_val);
        if (!bits || bits >= 32) continue;

        s->type.pack_bits = bits;
        f->nodes[lit].type.pack_bits = bits;
        // Retag every read of this slot so the interpreter and the emitter
        // unpack it the same way.
        for (int i = 0; i < f->n_nodes; i++) {
            if (f->nodes[i].op != IR_INDEX) continue;
            IRNode *base = &f->nodes[f->nodes[i].a];
            if (base->op == IR_LOCAL && (int)base->ki == si) {
                f->nodes[i].ki = bits;
                base->type.pack_bits = bits;
            }
        }
        for (int i = 0; i < f->n_nodes; i++)
            if (f->nodes[i].op == IR_LOCAL && (int)f->nodes[i].ki == si)
                f->nodes[i].type.pack_bits = bits;
    }
}

// Where a slot of type `t` may start. An array aligns to its element -- and a
// record to its struct, so a pointer to one handed to C is a valid `struct T *`.
static int frame_align_of(Func *f, VMType t) {
    if (!is_array(t.kind)) return vt_align_of(t.kind);
    int al = vt_align_of(arr_elem(t.kind));
    const VMStruct *st = t.struct_id ? vm_struct_get(f->structs, t.struct_id) : 0;
    if (st && st->align > al) al = st->align;
    return al;
}

static void layout_frame(Func *f) {
    // Slot 0 .. : retval (min 8 bytes, or more for arrays/slices).
    size_t ret_sz = vt_slot_bytes(f->ret_type);
    if (ret_sz < 8) ret_sz = 8;
    size_t off = ret_sz;
    f->ret_offset = 0;
    f->params_offset = (int)off;
    // Place params first (in declared order), then locals in sym order.
    for (int i = 0; i < f->n_params; i++) {
        VMSym *s = &f->syms[f->param_slot[i]];
        int al = frame_align_of(f, s->type);
        off = (off + al - 1) & ~(size_t)(al - 1);
        s->offset = (int)off;
        off += vt_slot_bytes(s->type);
    }
    for (int i = 0; i < f->n_syms; i++) {
        if (slot_is_param(f, i)) continue;
        if (f->syms[i].is_func) continue;
        // Shared variables already have an offset -- into the globals block,
        // not this frame -- so they must neither be re-placed nor consume
        // frame bytes. See VMSym.is_global.
        if (f->syms[i].is_global) continue;
        // Same for a host buffer: its offset addresses VmRun.host_bufs.
        if (f->syms[i].is_host_buf) continue;
        VMSym *s = &f->syms[i];
        if (s->type.kind == VMT_VOID) continue;
        int al = frame_align_of(f, s->type);
        off = (off + al - 1) & ~(size_t)(al - 1);
        s->offset = (int)off;
        off += vt_slot_bytes(s->type);
    }
    f->frame_size = (off + 7) & ~(size_t)7;
}

static Func *compile_func_def_body(VM *vm, InternID name, ASTNode *arrow_node);

// A rest parameter takes the gathered arguments as a slice, so `[]T` is the
// only annotation that says anything: `...rest: [3]i32` would pin an arity the
// call site is free to choose, and a packed or struct element type has no
// gather to build it from.
static int rest_type_ok(VM *vm, ASTNode *at, VMType t) {
    if (is_slice(t.kind) && !t.struct_id && !t.pack_bits) return 1;
    vm_set_error_at(vm, at, "a ...rest parameter must be annotated []T, or left bare");
    return 0;
}

// A function literal's body starts its own depth count: its top level runs
// unconditionally whenever the function is called, however deeply nested inside
// the enclosing body the literal was written. Saved and restored rather than
// zeroed on the way out, since the enclosing body is still being compiled.
static Func *compile_func_def(VM *vm, InternID name, ASTNode *arrow_node) {
    int saved = vm->blk_depth;
    Func *saved_scope = vm->cur_scope;
    vm->blk_depth = 0;
#if VM_REACTIVE
    vm->rx_func_depth++;
#endif
    Func *g = compile_func_def_body(vm, name, arrow_node);
#if VM_REACTIVE
    vm->rx_func_depth--;
#endif
    vm->blk_depth = saved;
    vm->cur_scope = saved_scope;
    return g;
}

static Func *compile_func_def_body(VM *vm, InternID name, ASTNode *arrow_node) {
    // arrow_node: tok=TOK_ARROW_F left=paren_params right=brace_body
    Func *f = func_alloc(&vm->run);
    f->name = name;
    f->structs = vm->structs;
    f->scope = f;
    // Written where the enclosing body is being compiled, so that body is the
    // lexical parent. A library function belongs to no scope of the unit.
    f->lex_parent = vm->lib_depth > 0 ? 0 : vm->cur_scope;

    f->template_ast = arrow_node;

    // Link into vm->run.funcs early so recursion works.
    f->next = vm->run.funcs;
    vm->run.funcs = f;
    func_register_id(&vm->run, f);
    // Linked above so recursion resolves, but there is no body yet: a `const`
    // inside this body that calls it must be refused rather than analysed or
    // run. Cleared at every exit that finishes the body. See Func.ct_purity.
    f->ct_purity = CT_PURITY_COMPILING;
    // Params: support `(a, b) => ...`, `(a:i32) => ...`, `() => ...`,
    // and the JS-style single-ident form `x => x * x`.
    ASTNode *params = arrow_node->left;
    ASTNode *single_param = 0;
    ASTNode *plist[VM_MAX_PARAMS + 1];
    int n_params = 0;
    if (is_ident(params)) {
        single_param = params;
        n_params = 1;
    } else if (is_paren(params)) {
        n_params = param_list(params, plist, VM_MAX_PARAMS);
        if (params->items.size == 1) {
            ASTNode *only = *(ASTNode**)array_get(&params->items, 0);
            if (!only->left && !only->right && !only->number_flags && only->token == 0
                && only->items.size == 0 && only->left_bracket == 0)
                n_params = 0;
        }
    } else {
        char desc[128];
        node_describe(vm, params, desc, sizeof(desc));
        vm_errorf_at(vm, params, "bad function params: got %s", desc);
        return 0;
    }
    if (n_params > VM_MAX_PARAMS) {
        vm_errorf_at(vm, arrow_node, "too many params (max %d)", VM_MAX_PARAMS);
        return 0;
    }
    f->n_params = n_params;
    f->params_ast = params;

    // Each name once, and not a keyword: the interpreter bound two params to one
    // name without a word, and no target can declare either.
    static const char *const KEYWORDS[] = { "if", "else", "while", "for", "in", "then", "do",
        "return", "break", "continue", "const", "struct", "ref", "as", 0 };
    for (int i = 0; i < n_params; i++) {
        ASTNode *def;
        int rest;
        ASTNode *nm = param_name_of(param_split_default(paren_inner(single_param ? single_param : plist[i]), &def), &rest);
        if (!nm) continue;
        for (int k = 0; KEYWORDS[k]; k++)
            if (ident_is(vm, nm, KEYWORDS[k])) {
                vm_errorf_at(vm, nm, "'%s' is a keyword and cannot name a parameter", KEYWORDS[k]);
                return 0;
            }
        for (int j = 0; j < i; j++) {
            ASTNode *d2;
            int r2;
            ASTNode *prev = param_name_of(param_split_default(paren_inner(plist[j]), &d2), &r2);
            if (prev && prev->token == nm->token) {
                vm_errorf_at(vm, nm, "parameter '%s' is named twice", intern_get_cstr(vm->intern, nm->token));
                return 0;
            }
        }
    }

    // Default values: `(a, b = 2) => ...`. They must be trailing, so the
    // count alone identifies which slots are optional. Validated before
    // anything else so the template path below sees a consistent Func.
    for (int i = 0; i < n_params; i++) {
        ASTNode *p2 = single_param ? single_param
                                   : plist[i];
        ASTNode *def;
        ASTNode *bare = param_split_default(paren_inner(p2), &def);
        int is_rest = 0;
        param_name_of(bare, &is_rest);
        if (is_rest) {
            // One rest parameter, last, and without a default: relax any of the
            // three and there is no longer a rule saying which arguments it
            // gathers. (Defaults BEFORE it are refused by the check below --
            // a rest parameter has no default, so it is one "without".)
            if (i != n_params - 1) {
                vm_set_error_at(vm, p2, "a ...rest parameter must be the last one");
                return 0;
            }
            if (def) {
                vm_set_error_at(vm, p2, "a ...rest parameter cannot have a default");
                return 0;
            }
            f->has_rest = 1;
        }
        if (!def) {
            if (f->n_defaults) {
                vm_set_error_at(vm, p2, "parameter without default follows a defaulted parameter");
                return 0;
            }
            continue;
        }
        if (!default_is_const(vm, def)) {
            vm_set_error_at(vm, def, "default value must be a constant expression");
            return 0;
        }
        f->n_defaults++;
    }

    // If ANY param is unannotated, the function is a polymorphic template: the body
    // is compiled on demand, once per distinct call-site type signature, by
    // find_or_specialize(). Annotated params keep their declared type in every
    // specialisation, so `(a:fx16, b)` really is fx16 + "whatever the caller
    // passed".
    if (n_params > 0) {
        int any_unann = 0;
        for (int i = 0; i < n_params; i++) {
            ASTNode *p2 = single_param ? single_param
                                       : plist[i];
            ASTNode *def2;
            p2 = param_split_default(paren_inner(p2), &def2);
            int is_rest2 = 0;
            ASTNode *nm2 = param_name_of(p2, &is_rest2);
            ASTNode *ty2 = param_type_of(p2);
            if (!ty2) { any_unann = 1; continue; }
            // `[]` / `const[]` / `[_]` / `[N]` leave the element type to the call
            // site, so they are annotations that still make this a template. They
            // have no type to validate until there is a call -- template_hint_type
            // resolves and reports them.
            if (type_is_shape_hint(vm, ty2, 0, 0)) { any_unann = 1; continue; }
            // Report a bad annotation here rather than at the first call.
            VMType chk;
            if (nm2 && parse_type(vm, ty2, &chk, 1) && chk.kind != VMT_VOID) {
                if (is_rest2 && !rest_type_ok(vm, ty2, chk)) return 0;
                continue;
            }
            if (!vm->run.last_error) vm_set_error_at(vm, ty2, "bad param type");
            return 0;
        }
        if (any_unann) {
            f->is_template   = 1;
            f->flags         = vm->flags | VM_FLAG_SNAPSHOT;
            f->ct_purity     = CT_PURITY_UNKNOWN;
            // Do not compile body or lay out a frame -- done per specialisation.
            return f;
        }
    }

    for (int i = 0; i < n_params; i++) {
        ASTNode *p = single_param ? single_param
                                  : plist[i];
        ASTNode *pdef;
        p = param_split_default(paren_inner(p), &pdef);
        int is_rest = 0;
        ASTNode *pn = param_name_of(p, &is_rest);
        ASTNode *pt = param_type_of(p);
        InternID pname = 0;
        VMType  ptype = { VMT_F64, 0 };   // only reached if every param is annotated
        if (!pn) {
            char desc[128];
            node_describe(vm, p, desc, sizeof(desc));
            vm_errorf_at(vm, p, "bad param %s", desc);
            return 0;
        }
        pname = pn->token;
        if (pt) {
            if (!parse_type(vm, pt, &ptype, 1)) {
                if (!vm->run.last_error) {
                    vm_set_error_at(vm, pt, "bad param type");
                }
                return 0;
            }
            if (is_rest && !rest_type_ok(vm, pt, ptype)) return 0;
        }
        // A param may not shadow a constant. Consts resolve ahead of the symbol table
        // in compile_ident, so the param would be silently unreadable -- every
        // mention of it folding to the constant instead, and the argument going
        // nowhere. The one declare site const_reject_write did not already cover.
        if (!const_reject_write(vm, p, pname, "used as a parameter name")) return 0;
        VMSym *s = sym_add(vm, f, pname, ptype);
        s->shift = ct_shift(ptype);
        f->param_slot[i] = f->n_syms - 1;
    }

    // Body: brace block, a single statement that itself carries a block
    // (`n => if c then a`, `n => while c do b` -- one-line blocks, which the
    // parser hangs on the arrow's body), or a single expression (implicit
    // return). compile_block takes a lone statement through flatten_stmt, so
    // the subscope case needs no special handling beyond getting here.
    // compile_func_def restores the scope.
    vm->cur_scope = f;
    ASTNode *body = arrow_node->right;
    body = unwrap_chain(body);
    int blk;
    if (is_block_node(body) || is_subscope(body)) {
        blk = compile_block(vm, f, body);
        if (blk < 0) return 0;
        if (!apply_implicit_return(vm, f, blk)) return 0;
    } else {
        VMType et;
        int e = compile_expr(vm, f, body, &et);
        if (e < 0) return 0;
        e = ref_to_value(vm, f, e, &et);
        if (slice_escapes_frame(f, e)) {
            vm_set_error_at(vm, body,
                "cannot return a string or slice built here: it points into this call's frame. "
                "Take a []u8 buffer as a parameter and fill that instead");
            return 0;
        }
        // A body whose expression has no value leaves the function void -- the
        // expression still runs, it just is not returned.
        int stmt;
        if (et.kind == VMT_VOID) {
            stmt = ir_new(vm, f, IR_EXPR_STMT);
        } else {
            if (!note_return_type(vm, f, et, body)) return 0;
            stmt = ir_new(vm, f, IR_RETURN);
        }
        f->nodes[stmt].a = e;
        blk = ir_new(vm, f, IR_BLOCK);
        f->nodes[blk].items_begin = ir_alloc_items(vm, f, 1);
        f->child_indices[f->nodes[blk].items_begin + 0] = stmt;
        f->nodes[blk].n_items = 1;
    }
    f->body = blk;
    if (!check_stray_break(vm, f)) return 0;
    // Every return converts to the joined type here, once no further return can widen it.
    if (!finalize_return_types(vm, f)) return 0;
    straight_line_cleanup(vm, f, vm->lib_depth > 0);
    if (!f->has_return) {
        f->ret_type.kind = VMT_VOID;
    }

    auto_pack_arrays(vm, f);
    layout_frame(f);
    f->flags = vm->flags | VM_FLAG_SNAPSHOT;
    f->ct_purity = CT_PURITY_UNKNOWN;   // body finished; derive it on demand
    return f;
}

// ---------- compile directives ----------

static VTKind ident_to_vtkind(VM *vm, ASTNode *n, int *shift_out) {
    *shift_out = 0;
    if (!is_ident(n)) return VMT_VOID;
    const char *s = intern_get_cstr(vm->intern, n->token);
    VTKind base;
    if (parse_base_type_name(s, &base)) return base;
    if (s[0]=='f' && s[1]=='x') {
        int sh = 0;
        for (int i = 2; s[i] >= '0' && s[i] <= '9'; i++) {
            sh = sh * 10 + (s[i] - '0');
            if (s[i+1] != 0) continue;
            if (sh >= 1 && sh <= 32) { *shift_out = sh; return VMT_I32; }
            return VMT_VOID;
        }
    }
    return VMT_VOID;
}

// Recursively flatten a directive AST into leaf tokens.
static int flatten_directive(ASTNode *node, ASTNode **out, int max, int *n) {
    if (!node || *n >= max) return 0;
    node = unwrap_chain(node);
    {
        // An indented line under the directive is not part of it: step over the
        // block and keep reading the chain. flatten_stmt puts those lines back
        // as siblings. See directive_indent_block.
        ASTNode *ib = subscope_block(node);
        if (ib && is_indent_block(ib)) return flatten_directive(node->left, out, max, n);
    }
    if (node->token == TOK_EMPTYSTRING && node->left) {
        flatten_directive(node->left,  out, max, n);
        if (node->right) flatten_directive(node->right, out, max, n);
        return 1;
    }
    if ((node->token == TOK_ARROW_R || node->token == TOK_AS) && node->left) {
        flatten_directive(node->left,  out, max, n);
        if (*n < max) out[(*n)++] = node;
        if (node->right) flatten_directive(node->right, out, max, n);
        return 1;
    }
    if (node->token == 0 && !node->left && !node->right && !node->number_flags
        && node->left_bracket == 0 && node->items.size > 0) {
        for (size_t i = 0; i < node->items.size && *n < max; i++)
            flatten_directive(*(ASTNode**)array_get(&node->items, i), out, max, n);
        return 1;
    }
    if (*n < max) out[(*n)++] = node;
    return 1;
}

// Does this statement start with `#`? The leftmost walk unpack_directive does,
// split out so flatten_stmt can ask without unpacking the whole chain.
static int stmt_is_directive(ASTNode *node) {
    ASTNode *cur = node;
    while (cur) {
        if (cur->token == TOK_EMPTYSTRING && cur->left) cur = cur->left;
        else if ((cur->token == TOK_ARROW_R || cur->token == TOK_AS) && cur->left) cur = cur->left;
        else break;
    }
    return cur && cur->token == TOK_HASH;
}

static ASTNode *find_indent_block_in_chain(ASTNode *node) {
    if (!node) return 0;
    node = unwrap_chain(node);
    if (!node) return 0;
    ASTNode *blk = subscope_block(node);
    if (blk) return is_indent_block(blk) ? blk : 0;
    if (node->token == TOK_EMPTYSTRING || node->token == TOK_ARROW_R || node->token == TOK_AS) {
        ASTNode *r = find_indent_block_in_chain(node->right);
        return r ? r : find_indent_block_in_chain(node->left);
    }
    if (node->token == 0 && !node->left && !node->right
        && node->left_bracket == 0 && node->items.size > 0) {
        for (size_t i = 0; i < node->items.size; i++) {
            ASTNode *r = find_indent_block_in_chain(*(ASTNode**)array_get(&node->items, i));
            if (r) return r;
        }
    }
    return 0;
}

// The indent block that a cosmetically indented line under a directive
// produced, or 0. On a one-word directive it lands on the statement, where
// flatten_stmt's ordinary check finds it; on a longer one indent_attach_target
// (parser.c) descends the juxtaposition chain and hangs it off the last word
// instead, which is what this digs out.
static ASTNode *directive_indent_block(ASTNode *node) {
    if (!stmt_is_directive(node)) return 0;
    return find_indent_block_in_chain(node);
}

static int unpack_directive(ASTNode *node, ASTNode **out, int max) {
    if (!stmt_is_directive(node)) return 0;
    int n = 0;
    flatten_directive(node, out, max, &n);
    return n;
}

static int compile_directive(VM *vm, ASTNode **chain, int n, ASTNode *stmt);

// Does this directive have to be applied where it is written, rather than by the
// whole-unit pre-scan in func_create?
//
// `#rewire` / `#enable` / `#disable` are unit-wide switches: applying them up front
// is what lets one at the bottom of a file re-type the whole thing. The three below
// are order-sensitive -- a `#push` region only means anything if the statements
// inside it are compiled between the push and the pop, and a `#define` starts where
// it is written.
static int directive_is_sequential(VM *vm, ASTNode **chain, int n) {
    for (int i = 1; i < n; i++) {
        if (!is_ident(chain[i])) continue;
        const char *s = intern_get_cstr(vm->intern, chain[i]->token);
        if (!s) continue;
        if (s_strcmp(s, "define") == 0 || s_strcmp(s, "push") == 0 || s_strcmp(s, "pop") == 0)
            return 1;
    }
    return 0;
}

static int directive_push(VM *vm, ASTNode **chain, int n, ASTNode *stmt) {
    snapshot_save(vm);
    if (n <= 2) return 1;
    return compile_directive(vm, chain + 1, n - 1, stmt);
}

static int directive_pop(VM *vm, ASTNode **chain, int n) {
    (void)chain; (void)n;
    if (vm->dir_stack_depth == 0) {
        vm_set_error_at(vm, chain[1], "#pop with no matching #push");
        return 0;
    }
    snapshot_restore(vm);
    return 1;
}

void vm_set_shader_profile(VM *vm, int on) {
    if (!vm) return;
    set_rewire(vm, VMT_F64, on ? VMT_F32 : VMT_F64, 0, REWIRE_BOTH);
    set_rewire(vm, VMT_I64, on ? VMT_I32 : VMT_I64, 0, REWIRE_BOTH);
    // Packing saves nothing on a GPU and has no type there.
    if (on) vm->flags &= ~VM_FLAG_AUTO_PACK;
    else    vm->flags |=  VM_FLAG_AUTO_PACK;
}

static int directive_rewire(VM *vm, ASTNode **chain, int n) {
    RewireScope scope = REWIRE_BOTH;
    int offset = 2;
    if (offset < n && is_ident(chain[offset])) {
        const char *mod = intern_get_cstr(vm->intern, chain[offset]->token);
        if (s_strcmp(mod, "type") == 0)    { scope = REWIRE_TYPE;    offset++; }
        if (s_strcmp(mod, "literal") == 0) { scope = REWIRE_LITERAL; offset++; }
    }
    // The chain after [#, rewire, mod?] has [FROM, ARROW?, TO].
    // The arrow token (-> or as) may appear between FROM and TO.
    if (offset >= n) { vm_set_error_at(vm, chain[1], "#rewire expects FROM -> TO"); return 0; }
    ASTNode *from_node = chain[offset];
    // Skip the arrow token if present.
    if (offset + 2 < n && (chain[offset + 1]->token == TOK_ARROW_R
                        || chain[offset + 1]->token == TOK_AS))
        offset++;
    if (offset + 1 >= n) { vm_set_error_at(vm, chain[1], "#rewire expects FROM -> TO"); return 0; }
    ASTNode *to_node = chain[offset + 1];
    const char *from_str = intern_get_cstr(vm->intern, from_node->token);
    if (!from_str) return 0;
    int to_shift = 0;
    VTKind to = ident_to_vtkind(vm, to_node, &to_shift);
    if (to == VMT_VOID) { vm_set_error_at(vm, to_node, "bad target type"); return 0; }
    if (s_strcmp(from_str, "number") == 0) {
        VTKind all[] = {VMT_I32, VMT_I64, VMT_F32, VMT_F64};
        for (int i = 0; i < 4; i++) set_rewire(vm, all[i], to, to_shift, scope);
    } else {
        int from_shift = 0;
        VTKind from = ident_to_vtkind(vm, from_node, &from_shift);
        if (from == VMT_VOID) { vm_set_error_at(vm, from_node, "bad source type"); return 0; }
        set_rewire(vm, from, to, to_shift, scope);
    }
    return 1;
}

// The #enable / #disable options that are a single flag bit.
static const struct { const char *name; int flag; } directive_flags[] = {
    { "safe_div_by_zero", VM_FLAG_CHECK_DIV_ZERO    },
    { "c_shifts",         VM_FLAG_C_SHIFTS          },
    { "inline_powers",    VM_FLAG_INLINE_POWERS     },
    { "identity_elim",    VM_FLAG_IDENTITY_ELIM     },
    { "lossy_assignment", VM_FLAG_LOSSY_ASSIGNMENT  },
    { "auto_pack",        VM_FLAG_AUTO_PACK         },
    { "auto_vec",         VM_FLAG_AUTO_VEC          },
    { "swizzle",          VM_FLAG_SWIZZLE           },
    { "unroll",           VM_FLAG_UNROLL            },
    { "fx_i64_widening",  VM_FLAG_FX_I64_WIDE       },
    { "int_wrap",         VM_FLAG_INT_WRAP          },
    { "strict_indent",    VM_FLAG_STRICT_INDENT     },
    { "const_folding",    VM_FLAG_CONST_FOLD        },
    { "const_precise",    VM_FLAG_CONST_PRECISE     },
    { "auto_const",       VM_FLAG_AUTO_CONST        },
    { "inline_builtins",  VM_FLAG_INLINE_BUILTINS   },
    { "c_division",       VM_FLAG_C_DIVISION        },
    { "c_int_wrap",       VM_FLAG_C_INT_WRAP        },
    { "c_float_to_int",   VM_FLAG_C_FLOAT_TO_INT    },
};

// #enable (on = 1) and #disable (on = 0).
static int directive_toggle(VM *vm, ASTNode **chain, int n, int on) {
    const char *what = on ? "#enable" : "#disable";
    if (n < 3 || !is_ident(chain[2])) {
        vm_errorf_at(vm, chain[1], "%s expects an option name", what); return 0;
    }
    const char *opt = intern_get_cstr(vm->intern, chain[2]->token);
    for (int i = 0; i < (int)(sizeof(directive_flags) / sizeof(directive_flags[0])); i++) {
        if (s_strcmp(opt, directive_flags[i].name) != 0) continue;
        if (on) vm->flags |= directive_flags[i].flag;
        else    vm->flags &= ~directive_flags[i].flag;
        return 1;
    }
    // Every C undefined behaviour at once: native shifts and float casts, an
    // unguarded divide, and plain signed overflow in the emitted C.
    if (s_strcmp(opt, "undefined_behaviour") == 0) {
        if (on) {
            vm->flags |= VM_FLAG_C_SHIFTS | VM_FLAG_C_FLOAT_TO_INT;
            vm->flags &= ~(VM_FLAG_CHECK_DIV_ZERO | VM_FLAG_C_INT_WRAP);
        } else {
            vm->flags &= ~(VM_FLAG_C_SHIFTS | VM_FLAG_C_FLOAT_TO_INT);
            vm->flags |= VM_FLAG_CHECK_DIV_ZERO;
        }
        return 1;
    }
    if (s_strcmp(opt, "source_builtins") == 0) {
#if VM_REACTIVE
        // An inverse is keyed on the native token (rvm_register_inverse), so a
        // built-in compiled from source would be inverted through its body
        // instead -- `linearstep` would start moving its edges as well as its
        // input. Refused rather than silently changing what a drag does.
        if (on && vm->rx_active) {
            vm_set_error_at(vm, chain[2],
                "#enable source_builtins does not apply here: a built-in compiled from source "
                "loses the inverse that makes dragging work");
            return 0;
        }
#endif
        if (on) vm->flags |= VM_FLAG_SOURCE_BUILTINS;
        else    vm->flags &= ~VM_FLAG_SOURCE_BUILTINS;
        return 1;
    }
    if (s_strcmp(opt, "capture") == 0) {
        // Switching off what is already off is accepted everywhere; it is not a
        // request for something the engine cannot do.
        if (!on) { vm->flags &= ~VM_FLAG_CAPTURE; return 1; }
#if VM_REACTIVE
        // Only the reactive engine can give a body the names around it: there a
        // free name becomes an extern slot filled per call. Here a function is a
        // Func with a symbol table of its own and no path to the enclosing frame
        // (resolve_name), so accepting the directive would only move the failure
        // to the first captured name.
        if (vm->rx_active) { vm->flags |= VM_FLAG_CAPTURE; return 1; }
#endif
        vm_set_error_at(vm, chain[2],
            "#enable capture is not supported here: a function body sees only its parameters, "
            "consts and built-ins. Pass the value in as a parameter");
        return 0;
    }
    vm_errorf_at(vm, chain[2], "unknown %s option '%s'", what, opt);
    return 0;
}

// The body, as the parser left it: everything after `after` in the directive's
// juxtaposition chain, which is a right-leaning `x (rest)` spine. Taken from
// the tree rather than from the flattened chain because flattening is exactly
// what loses the difference between `#define pos x,y` and `#define pos x y`.
static ASTNode *directive_tail_after(ASTNode *stmt, ASTNode *after) {
    for (ASTNode *cur = unwrap_chain(stmt); cur; cur = unwrap_chain(cur->right)) {
        // An indented line under the directive wraps the step it landed on in
        // a subscope; the chain continues inside. See directive_indent_block.
        ASTNode *ib = subscope_block(cur);
        if (ib && is_indent_block(ib)) cur = unwrap_chain(cur->left);
        if (!cur) return 0;
        if (cur->token != TOK_EMPTYSTRING || !cur->left || !cur->right) return 0;
        if (unwrap_chain(cur->left) == after) return unwrap_chain(cur->right);
    }
    return 0;
}

// A comma body arrives pre-split: one items node holding the values, with a
// comma divider on all but the last. Juxtaposition (`#define pos x y`) builds a
// left/right chain instead, so it never looks like this.
static int is_comma_group(ASTNode *n) {
    if (!n || n->token != 0 || n->left || n->right || n->left_bracket != 0) return 0;
    if (n->items.size < 2) return 0;
    for (size_t i = 0; i + 1 < n->items.size; i++)
        if ((*(ASTNode**)array_get(&n->items, i))->divider != TOK_COMMA) return 0;
    return 1;
}

// `#define NAME body`.
//
// The body needs no punctuation of its own. Directives are recognised AFTER parsing
// (see unpack_directive), and the juxtaposition that strings a directive together
// binds looser than any operator, so `#define k 1+2` arrives with the `+` intact and
// is substituted as that expression rather than as text -- which is why `k*10` is 30
// and not 21.
//
// The comma is the one separator the parser acts on, so `#define pos x,y` is handed
// over pre-split, already laid out for expand_call_args.
static int directive_define(VM *vm, ASTNode **chain, int n, ASTNode *stmt) {
    if (n < 3 || !is_ident(chain[2])) {
        vm_set_error_at(vm, chain[1], "#define expects a name: #define NAME body");
        return 0;
    }
    ASTNode *name_node = chain[2];
    InternID nm = name_node->token;
    const char *nm_s = intern_get_cstr(vm->intern, nm);
    ASTNode *body = directive_tail_after(stmt, name_node);
    if (!body) {
        vm_errorf_at(vm, name_node, "#define %s needs a body", nm_s);
        return 0;
    }
    if (const_find(vm, nm)) {
        vm_errorf_at(vm, name_node, "'%s' is already declared as a constant", nm_s);
        return 0;
    }

    // A comma list -- written bare or parenthesised -- expands to its items;
    // anything else is one value, whatever shape it has.
    ASTNode **src = &body;
    int n_values = 1;
    if ((is_paren(body) || is_comma_group(body)) && body->items.size > 0) {
        src = (ASTNode**)body->items.data;
        n_values = (int)body->items.size;
    }

    // Redefining is allowed and the newest wins, so a body can be swapped for
    // the rest of a unit (or, with #push / #pop, for part of one).
    VMDefine *d = (VMDefine*)mem_alloc(&vm->run.mem, sizeof(VMDefine));
    ASTNode **values = (ASTNode**)mem_alloc(&vm->run.mem, sizeof(ASTNode*) * (size_t)n_values);
    if (!d || !values) { vm_set_error_at(vm, chain[1], "out of memory declaring a #define"); return 0; }
    vm->run.sys->memset(d, 0, sizeof(*d));
    for (int i = 0; i < n_values; i++) values[i] = src[i];
    d->name = nm;
    d->values = values;
    d->n_values = n_values;
    d->next = vm->defines;
    vm->defines = d;
    return 1;
}

static int compile_directive(VM *vm, ASTNode **chain, int n, ASTNode *stmt) {
    if (n < 2) return 0;
    const char *cmd = intern_get_cstr(vm->intern, chain[1]->token);
    if (!cmd) return 0;
    if (s_strcmp(cmd, "push") == 0)    return directive_push(vm, chain, n, stmt);
    if (s_strcmp(cmd, "pop") == 0)     return directive_pop(vm, chain, n);
    if (s_strcmp(cmd, "rewire") == 0)  return directive_rewire(vm, chain, n);
    if (s_strcmp(cmd, "enable") == 0)  return directive_toggle(vm, chain, n, 1);
    if (s_strcmp(cmd, "disable") == 0) return directive_toggle(vm, chain, n, 0);
    if (s_strcmp(cmd, "define") == 0)  return directive_define(vm, chain, n, stmt);
    vm_errorf_at(vm, chain[1], "unknown directive: #%s", cmd);
    return 0;
}

// ---------- top-level func_create ----------

static Func *func_create_body(VM *vm, ASTNode *node, ParseResult *pres);

// Wrapper around the compile proper so vm->err_ctx (the fallback node for
// errors raised on nodes with no text of their own) is confined to it: a stale
// context would put a position on a later error that has nothing to do with
// this source.
Func *func_create(VM *vm, ASTNode *node, ParseResult *pres) {
    vm->err_ctx = 0;
    vm->bind_failed = 0;
    uintptr_t saved_base = vm->compile_stack_base;
    if (!saved_base) vm->compile_stack_base = (uintptr_t)&saved_base;
    Func *f = func_create_body(vm, node, pres);
    vm->compile_stack_base = saved_base;
    vm->err_ctx = 0;
    if (vm->bind_failed) { vm->bind_failed = 0; return 0; }
    return f;
}

static Func *func_create_body(VM *vm, ASTNode *node, ParseResult *pres) {
    vm->run.last_error = 0;
    vm->run.error_row = -1;
    vm->run.error_col = -1;
    // Borrow position data from ParseResult for error reporting.
    if (pres) {
        vm->txt_to_ref = pres->txt_to_ref;
        vm->num_txt_to_ref = pres->num_txt_to_ref;
        vm->line_offsets = pres->line_offsets;
        vm->num_lines_offsets = pres->num_lines_offsets;
    } else {
        vm->txt_to_ref = 0;
        vm->num_txt_to_ref = 0;
        vm->line_offsets = 0;
        vm->num_lines_offsets = 0;
    }
    if (!node) { vm_set_error(&vm->run, "null AST"); return 0; }

    // Constants are scoped to one compilation unit. The same prelude text gets
    // compiled once per program -- a host may prepend a shared block to every body
    // -- so a VM-lifetime list would call the second pass over the same `const` a
    // redefinition. See VMConst.
    vm->consts = 0;
    vm->cur_scope = 0;
    // vm->host_consts is deliberately NOT cleared here: the host declared those,
    // not this unit, so clearing them would drop them on the first compile.
    // Defines are scoped the same way, and must be: their bodies point into the
    // ParseResult of the unit being compiled. See VMDefine.
    vm->defines = 0;
    // A new unit: library templates compiled for the previous one are stale,
    // because a template snapshots the compile flags and is keyed only on
    // parameter types. See VM.unit_id and lib_template.
    vm->unit_id++;
    // A fresh struct table per unit, allocated now rather than on the first
    // declaration so every Func the unit creates can point at it from the start
    // -- a recursive call is compiled before its callee is finished.
    vm->structs = (VMStructTable*)mem_alloc(&vm->run.mem, sizeof(VMStructTable));
    if (!vm->structs) { vm_set_error(&vm->run, "out of memory"); return 0; }
    vm->run.sys->memset(vm->structs, 0, sizeof(VMStructTable));

    // If it's a `=>` literal, treat as anonymous function. It carries no
    // directives, so the write census can be taken right here.
    if (node->token == TOK_ARROW_F) {
        autoconst_scan(vm, node);
        return compile_func_def(vm, 0, node);
    }

    // Pre-scan items for compile directives (#rewire, #enable, etc.),
    // process them, and strip them from the item list.
    if (node->items.size > 0) {
        for (size_t i = 0; i < node->items.size; ) {
            ASTNode *item = *(ASTNode**)array_get(&node->items, i);
            ASTNode *chain[16];
            int n = unpack_directive(item, chain, 16);
            if (n > 0) {
                // Left in place for compile_block to apply in sequence, and so
                // that a #define body stays owned by the tree it came from.
                if (directive_is_sequential(vm, chain, n)) { i++; continue; }
                // A line indented under the directive hangs off it as an indent
                // block, so stripping the item would take that line away with it.
                // Apply the switch and keep the item: compile_block flattens the
                // block back out into siblings and applies the directive a second
                // time, which for the sequential switches sets what is already set.
                if (directive_indent_block(item)) {
                    if (!compile_directive(vm, chain, n, item)) return 0;
                    i++; continue;
                }
                if (!compile_directive(vm, chain, n, item)) return 0;
                for (size_t j = i; j + 1 < node->items.size; j++)
                    *(ASTNode**)array_get(&node->items, j) =
                        *(ASTNode**)array_get(&node->items, j + 1);
                node->items.size--;
                free_ast_node(item, vm->run.sys);
            } else {
                i++;
            }
        }
    }

    // Which names this unit writes, and how often -- what auto_const needs before
    // the first declaration compiles. After the directive pre-scan above, so the
    // stripped-and-freed directive items are not walked. See VM_FLAG_AUTO_CONST.
    autoconst_scan(vm, node);
    // Which library names this unit defines for itself, for the same reason and
    // off the same item list: the user's definition wins over a built-in
    // written in the language, wherever in the file it sits.
    lib_mask_unit_names(vm, node);

    // Otherwise treat the node as a code-tree (top-level items) and compile
    // as a zero-arg function.
    Func *f = func_alloc(&vm->run);
    f->structs = vm->structs;
    f->next = vm->run.funcs;
    vm->run.funcs = f;
    func_register_id(&vm->run, f);
    f->n_params = 0;

    int blk = compile_block(vm, f, node);
    if (blk < 0) return 0;
    if (!apply_implicit_return(vm, f, blk)) return 0;
    f->body = blk;
    if (!check_stray_break(vm, f)) return 0;
    // Every return converts to the joined type here, once no further return can widen it.
    if (!finalize_return_types(vm, f)) return 0;
    straight_line_cleanup(vm, f, vm->lib_depth > 0);
    if (!f->has_return) f->ret_type.kind = VMT_VOID;
    auto_pack_arrays(vm, f);
    layout_frame(f);
    f->flags = vm->flags | VM_FLAG_SNAPSHOT;
    f->ct_purity = CT_PURITY_UNKNOWN;   // body finished; derive it on demand
    return f;
}

void func_free(Func *f) { (void)f; }

// ---------- Shared variables ----------

// FNV-1a over the parts of a layout that two VMs must agree on. Deliberately
// covers the NAME TEXT, not the InternID: ids are per-parser (see VMGlobal), so
// hashing them would make two identical layouts hash differently and defeat the
// whole check.
static unsigned long long globals_hash_add(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static unsigned long long globals_layout_hash(const VMGlobalTable *t) {
    unsigned long long h = 14695981039346656037ULL;
    for (int i = 0; i < t->count; i++) {
        const VMGlobal *g = &t->g[i];
        h = globals_hash_add(h, g->name, s_strlen(g->name));
        h = globals_hash_add(h, &g->type.kind, sizeof(g->type.kind));
        h = globals_hash_add(h, &g->type.len, sizeof(g->type.len));
        h = globals_hash_add(h, &g->type.pack_bits, sizeof(g->type.pack_bits));
        h = globals_hash_add(h, &g->shift, sizeof(g->shift));
        h = globals_hash_add(h, &g->offset, sizeof(g->offset));
        h = globals_hash_add(h, g->struct_name, s_strlen(g->struct_name));
        h = globals_hash_add(h, &g->struct_hash, sizeof(g->struct_hash));
    }
    return h;
}

// Shared by vm_declare_globals and vm_collect_top_level_vars: a sym that a
// user wrote as a top-level variable, as opposed to a function, an anonymous
// compiler temporary (name == 0, from ** expansion and friends -- not
// user-visible, and each call needs its own), or an already-imported global.
static int sym_is_top_level_var(const VMSym *s) {
    return !s->is_func && !s->is_global && !s->is_host_buf
        && s->name != 0 && s->type.kind != VMT_VOID;
}

int vm_collect_top_level_vars(VM *vm, ASTNode *node, ParseResult *pres,
                              const char **out, int max_out) {
    int saved_flags = vm->flags;
    // Both for the same reason as in vm_declare_globals: this source's top-level
    // variables are about to become shared ones, and neither "never written" nor
    // "written once" can be established from it.
    vm->flags &= ~(VM_FLAG_AUTO_PACK | VM_FLAG_AUTO_CONST);
    Func *f = func_create(vm, node, pres);
    vm->flags = saved_flags;
    if (!f) return -1;

    int n = 0;
    for (int i = 0; i < f->n_syms && n < max_out; i++) {
        if (!sym_is_top_level_var(&f->syms[i])) continue;
        const char *nm = intern_get_cstr(vm->intern, f->syms[i].name);
        if (nm) out[n++] = nm;
    }
    return n;
}

Func *vm_declare_globals(VM *vm, ASTNode *node, ParseResult *pres,
                         const char *const *exclude, int n_exclude,
                         VMGlobalTable *out) {
    if (!out) return 0;
    vm->run.sys->memset(out, 0, sizeof(*out));

    // Compile the declaration source exactly as an ordinary program: its top level
    // becomes a zero-arg function whose LOCALS are the variables we are after, and
    // whose BODY is their initialiser -- which is why a non-constant initialiser
    // needs no special handling.
    //
    // Except auto-packing, which must be off: it requires "never written", and that
    // is exactly what this source cannot establish, since a shared variable exists
    // precisely to be written by programs compiled later and separately. Left on,
    // every one of those would write through a layout nobody else agreed to.
    //
    // auto_const goes with it, and for a sharper version of the same reason: a
    // declaration it folded would not become a local at all, so the variable would
    // be missing from the block entirely -- `bpm = 120` in a globals block is a
    // shared variable, not a constant, however constant its initialiser looks.
    int saved_flags = vm->flags;
    vm->flags &= ~(VM_FLAG_AUTO_PACK | VM_FLAG_AUTO_CONST);
    Func *init = func_create(vm, node, pres);
    vm->flags = saved_flags;
    if (!init) return 0;

    // Promote those locals to shared variables, packing them into a block that
    // starts at offset 0 (unlike a frame, there is no return slot to skip).
    // Anonymous compiler temporaries (name == 0, from ** expansion and
    // friends) stay frame-local: they are not user-visible and each call needs
    // its own.
    size_t off = 0;
    for (int i = 0; i < init->n_syms; i++) {
        VMSym *s = &init->syms[i];
        if (!sym_is_top_level_var(s)) continue;
        const char *nm = intern_get_cstr(vm->intern, s->name);
        if (!nm) continue;
        // Declared by the prelude compiled alongside, not by the declaration
        // source proper -- leave it a local. See `exclude` in vm.h.
        int skip = 0;
        for (int k = 0; k < n_exclude; k++)
            if (exclude[k] && s_strcmp(exclude[k], nm) == 0) { skip = 1; break; }
        if (skip) continue;
        if (out->count >= VM_MAX_GLOBALS) {
            vm_set_error(&vm->run, "too many shared variables (raise VM_MAX_GLOBALS)");
            return 0;
        }
        size_t nlen = s_strlen(nm);
        if (nlen >= VM_GLOBAL_NAME_MAX) {
            vm_errorf_at(vm, 0, "shared variable name '%s' is too long", nm);
            return 0;
        }

        int al = frame_align_of(init, s->type);
        off = (off + al - 1) & ~(size_t)(al - 1);

        VMGlobal *g = &out->g[out->count++];
        vm->run.sys->memset(g, 0, sizeof(*g));
        for (size_t c = 0; c < nlen; c++) g->name[c] = nm[c];
        g->type   = s->type;
        g->shift  = s->shift;
        g->offset = (int)off;
        if (s->type.struct_id) {
            // The struct goes into the table by name: its id means nothing to
            // another program. An inferred shape has no name to give.
            const VMStruct *st = vm_struct_get(init->structs, s->type.struct_id);
            const char *sn = st && st->name ? intern_get_cstr(vm->intern, st->name) : 0;
            if (!sn || s_strlen(sn) >= VM_GLOBAL_NAME_MAX) {
                vm_errorf_at(vm, 0, "shared variable '%s' needs a declared struct type; "
                                    "declare one with struct NAME { ... }", nm);
                return 0;
            }
            for (size_t c = 0; sn[c]; c++) g->struct_name[c] = sn[c];
            g->struct_hash = struct_layout_hash(vm, init->structs, s->type.struct_id, 0);
            g->type.struct_id = 0;
        }
        off += vt_slot_bytes(s->type);

        // Re-point the initialiser's own sym at the block, so running `init`
        // writes the shared storage rather than a frame that is about to be
        // discarded. This is what makes `init` the initialiser rather than
        // merely a description of one.
        s->offset    = g->offset;
        s->is_global = 1;
    }
    out->size = (off + 7) & ~(size_t)7;
    out->hash = globals_layout_hash(out);
    vm->globals_structs = init->structs;

    // The promoted syms no longer belong in the frame; re-lay it out so it
    // holds only the return slot and any temporaries.
    layout_frame(init);

    vm_import_globals(vm, out);
    return init;
}

void vm_set_globals_c_prefix(VM *vm, const char *prefix) {
    if (!vm) return;
    size_t n = prefix ? s_strlen(prefix) : 0;
    if (n >= sizeof(vm->globals_c_prefix)) n = sizeof(vm->globals_c_prefix) - 1;
    for (size_t i = 0; i < n; i++) vm->globals_c_prefix[i] = prefix[i];
    vm->globals_c_prefix[n] = '\0';
}

// A constant the HOST declares, outside any compilation unit. See vm.h.
//
// Same VMConst the source-level `const` builds, on a list func_create does not
// clear -- so one call covers every body compiled afterwards, rather than
// needing to be repeated per unit. Re-declaring a name overwrites it in place
// for the same reason: the host's value changing is an update, not a second
// constant shadowing the first.
int vm_declare_const_num(VM *vm, const char *name, double value, int is_int) {
    if (!vm || !name || !name[0]) return 0;
    InternID nm = intern_c_string(&vm->globals_owner, name);
    if (nm == 0) return 0;
    if (define_find(vm, nm)) return 0;

    VMConst *c = 0;
    for (VMConst *h = vm->host_consts; h; h = h->next)
        if (h->name == nm) { c = h; break; }

    if (!c) {
        c = (VMConst*)mem_alloc(&vm->run.mem, sizeof(VMConst));
        if (!c) return 0;
        vm->run.sys->memset(c, 0, sizeof(*c));
        c->name = nm;
        // has_type stays 0 (the memset): no annotation, so this is an untyped
        // literal re-typed at each use under the active #rewire -- which is what
        // makes `width` usable as an fx16 coordinate and an i32 loop bound in
        // the same unit.
        c->next = vm->host_consts;
        vm->host_consts = c;
    }
    c->value     = value;
    c->num_flags = is_int ? NUM_INTEGER : NUM_DOUBLE;
    c->str       = 0;
    return 1;
}

// A buffer the HOST owns and the script indexes. See vm.h.
//
// The name is interned into globals_owner, which is VM-lifetime, for the same
// reason a host const's is: no compilation unit declares this, so it must
// survive every func_create.
int vm_declare_host_buffer(VM *vm, int id, const char *name, VMType type,
                           const char *c_name, const char *c_len_expr) {
    if (!vm || !name || !name[0]) return 0;
    if (id < 0 || id >= VM_MAX_HOST_BUFS) return 0;
    // Slice only, and deliberately so: the slot is the {ptr,len} pair, which is
    // what lets the host rebind it. A fixed array would have to live IN the
    // slot, and there would then be nothing for the host to point at.
    if (!is_slice(type.kind)) return 0;
    InternID nm = intern_c_string(&vm->globals_owner, name);
    if (nm == 0) return 0;
    if (define_find(vm, nm)) return 0;

    vm->host_bufs[id].name       = nm;
    vm->host_bufs[id].type       = type;
    vm->host_bufs[id].c_name     = c_name ? c_name : name;
    vm->host_bufs[id].c_len_expr = c_len_expr;
    vm->host_bufs[id].declared   = 1;
    return 1;
}

void vm_import_globals(VM *vm, const VMGlobalTable *tbl) {
    if (!vm) return;
    if (!tbl || tbl->count <= 0) { vm->globals = 0; vm->n_globals = 0; return; }

    vm->globals = (VMGlobalRef*)mem_alloc(&vm->run.mem, sizeof(VMGlobalRef) * (size_t)tbl->count);
    if (!vm->globals) { vm_set_error(&vm->run, "out of memory"); vm->n_globals = 0; return; }
    vm->n_globals = tbl->count;
    for (int i = 0; i < tbl->count; i++) {
        // Re-intern into THIS VM's context -- the table's names are text
        // precisely because the declaring VM's ids mean nothing here.
        vm->globals[i].name   = intern_c_string(&vm->globals_owner, tbl->g[i].name);
        vm->globals[i].type   = tbl->g[i].type;
        vm->globals[i].offset = tbl->g[i].offset;
        vm->globals[i].shift  = tbl->g[i].shift;
        vm->globals[i].type.struct_id = 0;
        vm->globals[i].struct_name = 0;
        vm->globals[i].struct_hash = tbl->g[i].struct_hash;
        if (tbl->g[i].struct_name[0]) {
            vm->globals[i].struct_name = intern_c_string(&vm->globals_owner, tbl->g[i].struct_name);
            // In the VM that declared the table, the struct table it was declared
            // against is still here: resolve the id against it, so this VM can emit
            // the variables' C declaration. Anywhere else it stays 0.
            const VMStructTable *gt = vm->globals_structs;
            for (int k = 0; gt && k < gt->count; k++) {
                if (gt->items[k].name != vm->globals[i].struct_name) continue;
                if (struct_layout_hash(vm, gt, k + 1, 0) == tbl->g[i].struct_hash)
                    vm->globals[i].type.struct_id = k + 1;
                break;
            }
        }
    }
}

// Pull in the runtime interpreter (also usable standalone as vm_run.c).
#define VM_HAS_FUNC_ALLOC
#include "vm_run.c"

// The reactive engine's compile entry points, which need this file's statics.
#if VM_REACTIVE
#include "vm_reactive.c"
#endif

// C versions of the geometry built-ins; they read frames with vm_run.c's helpers.
// Last, because it turns off floating-point contraction for itself.
#include "vm_lib_accel.c"
