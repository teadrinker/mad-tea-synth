// vm_emit_curlywas.c
// Walk the VM's IR tree and generate CurlyWas source code.
//
// CurlyWas (https://github.com/exoticorn/curlywas) is a curly-braced,
// infix syntax for WebAssembly.  This emitter translates the VM's typed
// IR into CurlyWas source that can be compiled with the `curlywas` tool.
//
// Pipeline:  IRNode tree  -->  vm_emit_curlywas()  -->  char* (CurlyWas source)
//
// Design notes:
//   - No libc; all allocation goes through vm->sys (malloc/realloc/free).
//   - Internal bookkeeping uses the VM's arena so it does not need to be
//     freed explicitly.
//   - The returned string is malloc'd; the caller frees it with sys->free.
//   - VM array types are not directly representable in CurlyWas (which uses
//     linear memory).  Array operations are emitted as comments with a
//     placeholder.
//   - Template specialisations that share a name get type-mangled CWA names
//     (e.g. "sq" specialised for i32 and f64 becomes "sq_i32" / "sq_f64").

#include "vm_emit_curlywas.h"
#include "vm_emit_shared.h"
#include "parser/tokens.h"
#include "common/string_pure.h"

#include <stdint.h>

// ============================================================================
// Small helpers (no libc)
// ============================================================================

static int cw_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int cw_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *cw_wasm_type(VTKind k) {
    switch (k) {
        case VMT_I32: return "i32";
        case VMT_I64: return "i64";
        case VMT_F32: return "f32";
        case VMT_F64: return "f64";
        default:     return "i32";  // fallback
    }
}

static int cw_is_scalar(VTKind k) {
    return k == VMT_I32 || k == VMT_F32 || k == VMT_F64 || k == VMT_I64;
}

static int cw_is_array(VTKind k) {
    return k == VMT_ARR_I32 || k == VMT_ARR_F32 || k == VMT_ARR_F64 || k == VMT_ARR_I64;
}

static const char *cw_type_tag(VTKind k) {
    switch (k) {
        case VMT_I32: return "i32";
        case VMT_I64: return "i64";
        case VMT_F32: return "f32";
        case VMT_F64: return "f64";
        case VMT_ARR_I32: return "arr_i32";
        case VMT_ARR_I64: return "arr_i64";
        case VMT_ARR_F32: return "arr_f32";
        case VMT_ARR_F64: return "arr_f64";
        default:     return "v";
    }
}

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
// Dynamic output buffer (uses common/string_pure.h StrBuf)
// ============================================================================

typedef StrBuf CWBuf;
#define cw_buf_init(b, vm)  sb_init(b, (vm)->run.sys)
#define cw_buf_str   sb_str
#define cw_buf_char  sb_char
#define cw_buf_int   sb_int

// Emit a floating-point literal.
// CurlyWas uses the same decimal notation as C: "1.0", "1.5", "1e10".
// No suffix needed; the type context determines i32/f32/f64 at the use site.
static void cw_buf_float_lit(CWBuf *b, double v) {
    char tmp[S_FROM_NUMBER_MAX_CHARS];
    // SHORTEST: the emitted literal must be the value the VM ran, not %g's
    // 6 significant digits (which turns 110566002.1 into 110566000).
    s_from_number_flags(v, tmp, S_FROM_NUMBER_FLAG_SHORTEST);
    sb_str(b, tmp);
    int has_dot = 0, has_exp = 0;
    for (int i = 0; tmp[i]; i++) {
        if (tmp[i] == '.') has_dot = 1;
        if (tmp[i] == 'e' || tmp[i] == 'E') has_exp = 1;
    }
    if (!has_dot && !has_exp) sb_str(b, ".0");
}



// Build a CurlyWas identifier name for each collected Func.
static void cw_build_names(VM *vm, Func **list, int count, const char **names_out) {
    for (int i = 0; i < count; i++) {
        Func *f = list[i];
        const char *base = (f->name != 0)
            ? intern_get_cstr(vm->intern, f->name)
            : "__script__";

        // Check whether any other function shares this base name.
        int conflict = 0;
        for (int j = 0; j < count && !conflict; j++) {
            if (i == j) continue;
            Func *g = list[j];
            const char *gbase = (g->name != 0)
                ? intern_get_cstr(vm->intern, g->name)
                : "__script__";
            if (cw_streq(base, gbase)) conflict = 1;
        }

        if (!conflict) {
            names_out[i] = base;
        } else {
            // Mangle: "name_T0_T1_..."
            int blen = cw_strlen(base);
            int total = blen + f->n_params * 4 + 1;
            char *out = (char *)mem_alloc(&vm->run.mem, (size_t)total);
            int p = 0;
            for (int k = 0; k < blen; k++) out[p++] = base[k];
            for (int k = 0; k < f->n_params; k++) {
                VTKind pk = f->syms[f->param_slot[k]].type.kind;
                const char *tag = cw_type_tag(pk);
                out[p++] = '_';
                for (int t = 0; tag[t]; t++) out[p++] = tag[t];
            }
            out[p] = '\0';
            names_out[i] = out;
        }
    }
}

// ============================================================================
// Emitter state
// ============================================================================

typedef struct {
    CWBuf       out;
    VM         *vm;
    Func       *cur_func;       // function currently being emitted
    int         indent;         // current indentation level (2 spaces each)
    Func      **func_list;      // collected function pointers
    const char **func_cnames;   // CWA names parallel to func_list
    int         func_count;
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

// Emit the name of a local variable (slot index into cur_func->syms).
static void cw_emit_local_name(CWEmitt *e, int slot) {
    VMSym *s = &e->cur_func->syms[slot];
    if (s->name != 0) {
        cw_buf_str(&e->out, intern_get_cstr(e->vm->intern, s->name));
    } else {
        cw_buf_str(&e->out, "__v");
        cw_buf_int(&e->out, (long long)slot);
    }
}

// ============================================================================
// Expression emitter
// ============================================================================

// Forward declaration.
static void cw_emit_expr(CWEmitt *e, IRNode *n);

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

static void cw_emit_expr(CWEmitt *e, IRNode *n) {
    if (!n) { cw_buf_str(&e->out, "/*null*/"); return; }

    switch (n->op) {

        // ---- constants -------------------------------------------------------
        case IR_CONST_I:
            cw_buf_int(&e->out, n->ki);
            break;

        case IR_CONST_F:
            cw_buf_float_lit(&e->out, n->kf);
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

        // Struct records are arrays underneath, and arrays
        // are not implemented here, so a field gets the same placeholder an element
        // does.
        case IR_FIELD:
            cw_buf_str(&e->out, "/* struct field */ 0");
            break;

        // ---- conversion ------------------------------------------------------
        case IR_CVT:
            cw_buf_char(&e->out, '(');
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, " as ");
            cw_buf_str(&e->out, cw_wasm_type(n->type.kind));
            cw_buf_char(&e->out, ')');
            break;

#if VM_HAS_INSPECT
        // Editor-only: renders as its operand alone (see vm_emit_c.c).
        case IR_INSPECT:
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            break;
#endif

        // ---- bit-preserving reinterpretation ---------------------------------
        case IR_BITCAST:
            // Same kind means i32 <-> fxN: the shift only ever existed at
            // compile time, so the operand is already the result.
            if (n->sub_op == (int)n->type.kind) {
                cw_buf_char(&e->out, '(');
                cw_emit_expr(e, ir_child(e->cur_func, n->a));
                cw_buf_char(&e->out, ')');
                break;
            }
            // The i32<->f32 / i64<->f64 pairs are wasm's *.reinterpret_*
            // instructions, which this emitter has no CurlyWas spelling for.
            // Emit a marker that will not compile rather than ` as `, which
            // would silently convert the value instead of its bits.
            cw_buf_str(&e->out, "/*?bitcast unsupported?*/");
            break;

        // ---- unary ops -------------------------------------------------------
        case IR_UNOP:
            if (n->sub_op == OP_NEG) {
                cw_buf_str(&e->out, "(-(");
                cw_emit_expr(e, ir_child(e->cur_func, n->a));
                cw_buf_str(&e->out, "))");
            } else { // OP_NOT
                cw_buf_str(&e->out, "(!(");
                cw_emit_expr(e, ir_child(e->cur_func, n->a));
                cw_buf_str(&e->out, "))");
            }
            break;

        // ---- binary ops ------------------------------------------------------
        case IR_BINOP:
            cw_buf_char(&e->out, '(');
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, cw_binop_str(n->sub_op));
            cw_emit_expr(e, ir_child(e->cur_func, n->b));
            cw_buf_char(&e->out, ')');
            break;

        case IR_SELECT:
            cw_buf_str(&e->out, "select(");
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ", ");
            cw_emit_expr(e, ir_child(e->cur_func, n->b));
            cw_buf_str(&e->out, ", ");
            cw_emit_expr(e, ir_child(e->cur_func, n->c));
            cw_buf_char(&e->out, ')');
            break;

        // ---- comma / sequence expression -------------------------------------
        // Emitted as an inline block whose last expression is the value:
        // { t = x; t = t * t; t * t }. Leading items are IR_ASSIGN staging steps.
        case IR_COMMA: {
            cw_buf_str(&e->out, "{ ");
            for (int i = 0; i < n->n_items; i++) {
                IRNode *it = ir_item(e->cur_func, n, i);
                if (it->op == IR_ASSIGN) {
                    IRNode *lv = ir_child(e->cur_func, it->a);
                    if (lv->op == IR_LOCAL) {
                        cw_emit_local_name(e, (int)lv->ki);
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
// needed if it's a return-with-value — it becomes the block's implicit return).
// In CurlyWas the last expression in a block is the return value and has no
// semicolon; early returns use the `return` keyword explicitly.
static void cw_emit_stmt(CWEmitt *e, IRNode *n) {
    cw_emit_stmt_last(e, n, 0);
}

static void cw_emit_stmt_last(CWEmitt *e, IRNode *n, int is_last) {
    if (!n) return;

    switch (n->op) {

        // ---- block -----------------------------------------------------------
        case IR_BLOCK:
            cw_emit_indent(e); cw_buf_str(&e->out, "{\n");
            e->indent++;
            for (int i = 0; i < n->n_items; i++)
                cw_emit_stmt_last(e, ir_item(e->cur_func, n, i), (i == n->n_items - 1) ? is_last : 0);
            e->indent--;
            cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
            break;

        // ---- if / else -------------------------------------------------------
        case IR_IF:
            cw_emit_indent(e); cw_buf_str(&e->out, "if (");
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ") {\n");
            e->indent++;
            {
                IRNode *tb = ir_child(e->cur_func, n->b);
                // then-block: only propagate is_last when there's no else
                int then_last = is_last && (n->c < 0);
                if (tb) {
                    for (int i = 0; i < tb->n_items; i++)
                        cw_emit_stmt_last(e, ir_item(e->cur_func, tb, i),
                            (i == tb->n_items - 1) ? then_last : 0);
                }
            }
            e->indent--;
            cw_emit_indent(e); cw_buf_char(&e->out, '}');
            if (n->c >= 0) {
                IRNode *eb = ir_child(e->cur_func, n->c);
                cw_buf_str(&e->out, " else {\n");
                e->indent++;
                if (eb) {
                    for (int i = 0; i < eb->n_items; i++)
                        cw_emit_stmt_last(e, ir_item(e->cur_func, eb, i),
                            (i == eb->n_items - 1) ? is_last : 0);
                }
                e->indent--;
                cw_emit_indent(e); cw_buf_char(&e->out, '}');
            }
            cw_buf_char(&e->out, '\n');
            break;

        // ---- while -> loop + branch_if ---------------------------------------
        case IR_WHILE:
            cw_emit_indent(e); cw_buf_str(&e->out, "loop while_");
            cw_buf_int(&e->out, (long long)(intptr_t)n);
            cw_buf_str(&e->out, " {\n");
            e->indent++;
            // Condition check at top
            cw_emit_indent(e); cw_buf_str(&e->out, "branch_if (");
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ") == 0: while_");
            cw_buf_int(&e->out, (long long)(intptr_t)n);
            cw_buf_str(&e->out, ";\n");
            // Loop body (is_last not propagated — loop body never contributes
            // to the outer block's return value)
            {
                IRNode *body = ir_child(e->cur_func, n->b);
                if (body) {
                    for (int i = 0; i < body->n_items; i++)
                        cw_emit_stmt(e, ir_item(e->cur_func, body, i));
                }
            }
            e->indent--;
            cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
            break;

        // ---- for -> loop + branch_if, step appended to the body --------------
        case IR_FOR:
            cw_emit_indent(e); cw_buf_str(&e->out, "loop for_");
            cw_buf_int(&e->out, (long long)(intptr_t)n);
            cw_buf_str(&e->out, " {\n");
            e->indent++;
            cw_emit_indent(e); cw_buf_str(&e->out, "branch_if (");
            cw_emit_expr(e, ir_child(e->cur_func, n->a));
            cw_buf_str(&e->out, ") == 0: for_");
            cw_buf_int(&e->out, (long long)(intptr_t)n);
            cw_buf_str(&e->out, ";\n");
            {
                IRNode *body = ir_child(e->cur_func, n->b);
                if (body) {
                    for (int i = 0; i < body->n_items; i++)
                        cw_emit_stmt(e, ir_item(e->cur_func, body, i));
                }
                IRNode *step = ir_child(e->cur_func, n->c);
                if (step) {
                    for (int i = 0; i < step->n_items; i++)
                        cw_emit_stmt(e, ir_item(e->cur_func, step, i));
                }
            }
            e->indent--;
            cw_emit_indent(e); cw_buf_str(&e->out, "}\n");
            break;

        // ---- loop controls ---------------------------------------------------
        case IR_BREAK:
            cw_emit_indent(e); cw_buf_str(&e->out, "branch break_label;\n");
            break;

        case IR_CONTINUE:
            cw_emit_indent(e); cw_buf_str(&e->out, "branch continue_label;\n");
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
                    // Early return — use explicit `return` keyword
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

    if (f->n_params == 0) {
        // No params
    } else {
        for (int i = 0; i < f->n_params; i++) {
            if (i) cw_buf_str(&e->out, ", ");
            VMSym *s = &f->syms[f->param_slot[i]];
            const char *pname = (s->name != 0)
                ? intern_get_cstr(e->vm->intern, s->name) : "__p";
            cw_buf_str(&e->out, pname);
            cw_buf_str(&e->out, ": ");
            cw_buf_str(&e->out, cw_wasm_type(s->type.kind));
        }
    }
    cw_buf_char(&e->out, ')');
}

// Emit a complete function definition.
static void cw_emit_func(CWEmitt *e, Func *f, const char *cname) {
    Func *saved   = e->cur_func;
    e->cur_func   = f;
    e->indent     = 0;

    // Return type annotation
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
    e->cur_func = saved;
}

// ============================================================================
// Public API
// ============================================================================

char *vm_emit_curlywas(VM *vm) {
    // Collect emittable functions.
    Func       *func_list [MAX_EMIT_FUNCS];
    const char *func_cnames[MAX_EMIT_FUNCS];

    int count = emit_collect_funcs(vm, func_list, MAX_EMIT_FUNCS);
    if (count < 0) return 0;
    cw_build_names(vm, func_list, count, func_cnames);

    CWEmitt e;
    e.vm          = vm;
    e.cur_func    = 0;
    e.indent      = 0;
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = count;
    cw_buf_init(&e.out, vm);

    // Emit a memory import (required for any wasm module)
    cw_buf_str(&e.out, "import \"env.env\" memory(1);\n\n");

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
    func_cnames[0] = (f->name != 0)
        ? intern_get_cstr(vm->intern, f->name)
        : "__script__";

    CWEmitt e;
    e.vm          = vm;
    e.cur_func    = 0;
    e.indent      = 0;
    e.func_list   = func_list;
    e.func_cnames = func_cnames;
    e.func_count  = 1;
    cw_buf_init(&e.out, vm);

    cw_buf_str(&e.out, "import \"env.env\" memory(1);\n\n");
    cw_emit_func(&e, f, func_cnames[0]);

    if (!e.out.ok) { vm->run.sys->free(e.out.buf); return 0; }
    return e.out.buf;
}
