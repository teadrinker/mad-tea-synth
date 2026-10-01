#ifndef VM_EMIT_SHARED_H
#define VM_EMIT_SHARED_H

#include "vm.h"

// An inspection wrapper (__ins(x, id) / inspect(x)) is invisible to every backend.
// Exported code is the code without it, and the cheapest way to make that true of
// EVERY decision an emitter makes -- not just the ones somebody remembered to look
// through -- is for it never to be handed the wrapper: ir_child / ir_item, the two
// ways an emitter fetches a child, are redefined here to skip past IR_INSPECT. An
// aggregate is where it matters: whether a call argument is copied, whether a
// slice base needs a temporary and whether `.len` needs parentheses are all
// decided by the operand's node kind, and reading `xs` through a wrapper would
// otherwise answer differently from reading `xs`.
//
// (Each emitter's own `case IR_INSPECT` is therefore unreachable through a child
// fetch; it stays for a wrapper that is itself the node being emitted.)
static inline IRNodeT *emit_skip_inspect(struct Func *f, IRNodeT *n) {
    while (n && n->op == IR_INSPECT && n->a >= 0) n = ir_child(f, n->a);
    return n;
}
static inline IRNodeT *emit_ir_child(struct Func *f, int idx) {
    return emit_skip_inspect(f, ir_child(f, idx));
}
static inline IRNodeT *emit_ir_item(const struct Func *f, const IRNodeT *n, int i) {
    return emit_skip_inspect((struct Func *)f, ir_item(f, n, i));
}
#define ir_child(f, idx)   emit_ir_child((f), (idx))
#define ir_item(f, n, i)   emit_ir_item((f), (n), (i))

#define MAX_EMIT_FUNCS 256
#define EMIT_STR_(x) #x
#define EMIT_STR(x)  EMIT_STR_(x)

// Fill funcs_out[] with all emittable Funcs (non-native, non-template;
// template specs are included instead of their template parent).
// Returns the count, or -1 (with last_error set) when there are more than
// `max`: dropping the rest would emit calls to functions never defined.
// The list is reversed so it follows definition order (vm->run.funcs is LIFO).
static int emit_collect_funcs(VM *vm, Func **funcs_out, int max) {
    int n = 0;
    for (Func *f = vm->run.funcs; f; f = f->next) {
        if (f->native_tok != 0) continue;
        int k = f->is_template ? f->n_specs : 1;
        if (n + k > max) {
            vm->run.last_error = "too many functions to emit (max " EMIT_STR(MAX_EMIT_FUNCS)
                                 ", every template specialisation counts)";
            return -1;
        }
        if (f->is_template) {
            for (int i = 0; i < f->n_specs; i++)
                funcs_out[n++] = f->specs[i];
        } else {
            funcs_out[n++] = f;
        }
    }
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        Func *t = funcs_out[i]; funcs_out[i] = funcs_out[j]; funcs_out[j] = t;
    }
    return n;
}

// The directive flag state an emitter must compile `f` against.
//
// NOT vm->flags: that is the VM's live state, i.e. whatever the LAST body
// compiled left it at, so a `#disable` in one body would decide the code
// emitted for the next and emission order would change the output. Func.flags
// carries the snapshot taken when `f` was compiled; VM_FLAG_SNAPSHOT is what
// distinguishes "this body disabled everything" from "no snapshot was taken"
// (a deserialized Func), which falls back to the VM.
static inline int emit_func_flags(VM *vm, Func *f) {
    if (f && (f->flags & VM_FLAG_SNAPSHOT)) return f->flags;
    return vm->flags;
}

// ---------------------------------------------------------------------------
// Structs, shared by every backend
// ---------------------------------------------------------------------------

// The struct a type names, against the table of the Func it appears in.
static const VMStruct *emit_struct_of(Func *f, VMType t) {
    return f ? vm_struct_get(f->structs, t.struct_id) : 0;
}

// The identifier a struct is emitted under: its own name, or, for a shape
// inferred from a `{ ... }` literal, one derived from its layout -- so the same
// shape compiled into two units names one type, and two different ones never
// collide under an include guard.
static const char *emit_struct_name(VM *vm, const VMStruct *st) {
    if (!st) return "__struct";
    if (st->name) return intern_get_cstr(vm->intern, st->name);
    unsigned long long h = 14695981039346656037ULL;
    for (int i = 0; i < st->n_fields; i++) {
        const char *nm = intern_get_cstr(vm->intern, st->fields[i].name);
        for (int k = 0; nm && nm[k]; k++) { h ^= (unsigned char)nm[k]; h *= 1099511628211ULL; }
        int words[6] = { (int)st->fields[i].type.kind, st->fields[i].type.len, st->fields[i].type.pack_bits,
                         st->fields[i].offset, st->fields[i].width, st->fields[i].shift };
        for (int w = 0; w < 6; w++)
            for (int k = 0; k < 4; k++) { h ^= (unsigned char)(words[w] >> (k * 8)); h *= 1099511628211ULL; }
    }
    static const char hexd[] = "0123456789abcdef";
    char *out = (char *)mem_alloc(&vm->run.mem, 16);
    if (!out) return "__struct";
    const char *pre = "anon_";
    int p = 0;
    for (; pre[p]; p++) out[p] = pre[p];
    for (int i = 7; i >= 0; i--) out[p++] = hexd[(h >> (i * 4)) & 0xf];
    out[p] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Name mangling, shared by every backend
// ---------------------------------------------------------------------------

static int es_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int es_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int es_is_array_kind(VTKind k) {
    return k == VMT_ARR_I32 || k == VMT_ARR_F32 || k == VMT_ARR_F64 || k == VMT_ARR_I64;
}

static int es_is_slice_kind(VTKind k) {
    return k == VMT_SLICE_I32 || k == VMT_SLICE_F32 || k == VMT_SLICE_F64 || k == VMT_SLICE_I64;
}

// Short type tag used in mangled names. For aggregates the length is appended
// after the tag by the caller.
static const char *es_type_tag(VTKind k) {
    switch (k) {
        case VMT_I32: return "i32";
        case VMT_F32: return "f32";
        case VMT_F64: return "f64";
        case VMT_I64: return "i64";
        case VMT_ARR_I32: case VMT_SLICE_I32: return "ai32";
        case VMT_ARR_F32: case VMT_SLICE_F32: return "af32";
        case VMT_ARR_F64: case VMT_SLICE_F64: return "af64";
        case VMT_ARR_I64: case VMT_SLICE_I64: return "ai64";
        default:     return "v";
    }
}

// 1 when `name` is in the NULL-terminated `reserved` list. A NULL list (the C
// backend: the DSL and C have near-identical keyword sets) matches nothing.
static int es_is_reserved(const char *const *reserved, const char *name) {
    if (!reserved) return 0;
    for (int i = 0; reserved[i]; i++) if (es_streq(reserved[i], name)) return 1;
    return 0;
}

// Append '_' until `name` is neither a target keyword nor already taken by an
// earlier entry of `taken[0..n_taken)`. Re-checking against BOTH is what keeps
// a script declaring `end` and `end_` from collapsing into two locals called
// `end_` -- a silent aliasing bug rather than a syntax error.
//
// Returns `name` itself when nothing needs escaping, so the common case costs
// no allocation and emits the script's own spelling.
static const char *es_escape_name(VM *vm, const char *name,
                                  const char *const *reserved,
                                  const char **taken, int n_taken) {
    int clash = es_is_reserved(reserved, name);
    for (int i = 0; i < n_taken && !clash; i++)
        if (taken[i] && es_streq(taken[i], name)) clash = 1;
    if (!clash) return name;

    int base = es_strlen(name);
    char *out = (char *)mem_alloc(&vm->run.mem, (size_t)(base + 16 + 1));
    if (!out) return name;
    for (int i = 0; i < base; i++) out[i] = name[i];
    int p = base;
    for (int guard = 0; guard < 16; guard++) {
        out[p++] = '_';
        out[p] = '\0';
        int again = es_is_reserved(reserved, out);
        for (int i = 0; i < n_taken && !again; i++)
            if (taken[i] && es_streq(taken[i], out)) again = 1;
        if (!again) return out;
    }
    return out;
}

// Build a target-identifier name for each collected Func.
//
// When two functions share the same script name (i.e. template specs), a suffix is
// appended for each parameter type so the names become unique ("sq" -> "sq_i32",
// "sq_f64"); aggregate parameter types include their length (fn([3]i32) ->
// fn_ai32_3). Plain functions keep their original name.
//
// `reserved` is the target's keyword list (NULL for C). A name colliding with a
// keyword, or with one already assigned, gets '_' appended until it does not.
// Names are arena-allocated; names_out[] is stack-owned by the caller.
static void emit_build_cnames(VM *vm, Func **list, int count, const char **names_out,
                              const char *const *reserved) {
    for (int i = 0; i < count; i++) {
        Func *f = list[i];
        const char *base = (f->name != 0)
            ? intern_get_cstr(vm->intern, f->name)
            : "__script__";

        // Check whether any other function in the list shares this base name.
        int conflict = 0;
        for (int j = 0; j < count && !conflict; j++) {
            if (i == j) continue;
            Func *g = list[j];
            const char *gbase = (g->name != 0)
                ? intern_get_cstr(vm->intern, g->name)
                : "__script__";
            if (es_streq(base, gbase)) conflict = 1;
        }

        if (!conflict) {
            names_out[i] = base;
        } else {
            // Mangle: "name_T0_T1_..."
            int blen = es_strlen(base);
            // Worst-case per param: "_" + tag (5 chars "ai32") + "fx"/"_" + len
            // (up to 6 digits) = ~15
            int total = blen + f->n_params * 16 + 1;
            char *out = (char *)mem_alloc(&vm->run.mem, (size_t)total);
            int p = 0;
            for (int k = 0; k < blen; k++) out[p++] = base[k];
            for (int k = 0; k < f->n_params; k++) {
                VMType pt = f->syms[f->param_slot[k]].type;
                out[p++] = '_';
                if (pt.struct_id) {
                    // A struct parameter is told apart by the struct, not its bytes.
                    const char *sn = emit_struct_name(vm, emit_struct_of(f, pt));
                    int sl = es_strlen(sn);
                    if (p + sl + 8 >= total) {
                        char *grown = (char *)mem_alloc(&vm->run.mem, (size_t)(total + sl + 64));
                        for (int c = 0; c < p; c++) grown[c] = out[c];
                        out = grown;
                        total += sl + 64;
                    }
                    for (int t = 0; t < sl; t++) out[p++] = sn[t];
                    if (es_is_slice_kind(pt.kind)) out[p++] = pt.inner_len ? 's' : 'r';
                    else if (pt.inner_len) {
                        int cnt = pt.len / pt.inner_len;
                        char t2[12]; int tn = 0;
                        out[p++] = '_';
                        do { t2[tn++] = (char)('0' + cnt % 10); cnt /= 10; } while (cnt > 0);
                        while (tn > 0) out[p++] = t2[--tn];
                    }
                    continue;
                }

                const char *tag = es_type_tag(pt.kind);
                for (int t = 0; tag[t]; t++) out[p++] = tag[t];
                // Append the length for fixed arrays (ai32_3), and the
                // fixed-point shift for a scalar integer that carries one
                // (i32fx22). Both live in VMType.len and both distinguish
                // otherwise-identical specialisations, which would otherwise
                // collide on one C identifier.
                int x = pt.len;
                if (es_is_array_kind(pt.kind)) {
                    out[p++] = '_';
                } else if (es_is_slice_kind(pt.kind) || x == 0) {
                    continue;
                } else {
                    out[p++] = 'f'; out[p++] = 'x';
                }
                // simple itoa inline
                if (x == 0) out[p++] = '0';
                else {
                    char t2[12]; int tn = 0;
                    while (x > 0) { t2[tn++] = (char)('0' + (x % 10)); x /= 10; }
                    for (int r = tn-1; r >= 0; r--) out[p++] = t2[r];
                }
            }
            out[p] = '\0';
            names_out[i] = out;
        }
        names_out[i] = es_escape_name(vm, names_out[i], reserved, names_out, i);
    }
}

// ---------------------------------------------------------------------------
// Declaration planning, shared by every emitter that declares locals
// ---------------------------------------------------------------------------

static inline int emit_is_param(Func *f, int idx) {
    for (int i = 0; i < f->n_params; i++) if (f->param_slot[i] == idx) return 1;
    return 0;
}

// Mark every local slot a subtree mentions, reads and writes alike. Ops that
// carry their operands in `items` walk only those: in the compact encoding a/b
// ARE items_begin and n_items. With `drop_print`, an IR_PRINT subtree is skipped
// -- a print() being elided takes its string build's hidden locals with it,
// and those are created with VMSym.used already set.
static inline void emit_mark_slots(Func *f, IRNodeT *n, char *mark, int n_syms, int drop_print) {
    if (!n) return;
    int op = NODE_OP(n);
    if (op == IR_PRINT && drop_print) return;
    if (op == IR_LOCAL) {
        int slot = (int)NODE_KI(n);
        if (slot >= 0 && slot < n_syms) mark[slot] = 1;
        return;
    }
    if (op == IR_CONST_I || op == IR_CONST_F || op == IR_DATA_SLICE
        || op == IR_BREAK || op == IR_CONTINUE) return;
    if (op == IR_BLOCK || op == IR_CALL || op == IR_ARR_LIT || op == IR_COMMA
        || op == IR_SLICE || op == IR_FMT) {
        for (int i = 0; i < NODE_N_ITEMS(n); i++)
            emit_mark_slots(f, ir_item(f, n, i), mark, n_syms, drop_print);
        return;
    }
    if (NODE_A(n) >= 0) emit_mark_slots(f, ir_child(f, NODE_A(n)), mark, n_syms, drop_print);
    if (NODE_B(n) >= 0) emit_mark_slots(f, ir_child(f, NODE_B(n)), mark, n_syms, drop_print);
    if (NODE_C(n) >= 0) emit_mark_slots(f, ir_child(f, NODE_C(n)), mark, n_syms, drop_print);
}

// Which top-level statement, if any, carries a local's DECLARATION instead of the
// prologue.
//
// The prologue declares and zero-initialises every local, matching the VM's zeroed
// frame, and the very next thing the body usually does is overwrite it. So when the
// first thing that happens to a local is a whole-value assignment at the top level
// of the body, the declaration moves onto that assignment. Conservative in four
// ways, each load-bearing:
//
//   - only a TOP-LEVEL statement qualifies, so the name still scopes to the rest of
//     the function (in Lua a sunk `local` would leave later reads finding a nil
//     GLOBAL rather than failing);
//   - no earlier statement may mention the slot anywhere in its subtree;
//   - the right-hand side may not mention it either, which rules out `x = x+1`;
//   - with `need_used`, the symbol must be READ somewhere (VMSym.used): C gives a
//     write-only local a `(void)x;` in the prologue, ahead of any declaration
//     that sank into the body.
//
// Anything that does not qualify keeps its prologue entry.
static inline void emit_plan_decls(VM *vm, Func *f, IRNodeT *body, int n_stmt, int need_used,
                            char *moved, IRNodeT **decl_of) {
    int n_syms = f->n_syms;
    char *seen = (char *)mem_alloc(&vm->run.mem, (size_t)n_syms + 1);
    if (!seen) return;
    for (int i = 0; i < n_syms; i++) seen[i] = 0;

    for (int i = 0; i < n_stmt; i++) {
        IRNodeT *st = ir_item(f, body, i);
        if (NODE_OP(st) == IR_ASSIGN) {
            IRNodeT *lv = ir_child(f, NODE_A(st));
            int slot = (lv && NODE_OP(lv) == IR_LOCAL) ? (int)NODE_KI(lv) : -1;
            if (slot >= 0 && slot < n_syms && !seen[slot]) {
                VMSym *s = &f->syms[slot];
                if (!s->is_global && !s->is_host_buf && !s->is_func && (s->used || !need_used)
                    && s->type.kind != VMT_VOID && !emit_is_param(f, slot)) {
                    // Mark the RHS into `seen` first: if the slot shows up
                    // there it read itself, and the same pass that answers
                    // that question is the one that has to run anyway.
                    emit_mark_slots(f, ir_child(f, NODE_B(st)), seen, n_syms, 0);
                    if (!seen[slot]) { moved[slot] = 1; decl_of[i] = st; }
                }
            }
        }
        emit_mark_slots(f, st, seen, n_syms, 0);
    }
}

#endif
