// vm_reactive.c -- the reactive engine's compile entry points (vm_reactive.h).
// Included at the end of vm.c under VM_REACTIVE: everything here is a thin
// door onto that file's statics, so the per-statement compile is the very
// same code path func_create runs over a whole body.

#include "vm_reactive.h"

void vm_set_extern_resolver(VM *vm, VmExternNameFn name_fn, VmExternCallFn call_fn, void *user) {
    vm->rx_name_fn = name_fn;
    vm->rx_call_fn = call_fn;
    vm->rx_user    = user;
}

int vm_rx_default_flags(void) {
    return VM_FLAG_CHECK_DIV_ZERO | VM_FLAG_INLINE_POWERS | VM_FLAG_IDENTITY_ELIM | VM_FLAG_AUTO_PACK
         | VM_FLAG_AUTO_VEC | VM_FLAG_FX_I64_WIDE | VM_FLAG_INT_WRAP | VM_FLAG_CONST_FOLD
         | VM_FLAG_CONST_PRECISE | VM_FLAG_AUTO_CONST | VM_FLAG_SWIZZLE | VM_FLAG_UNROLL;
}

void vm_rx_begin_unit(VM *vm) {
    vm->consts  = 0;
    vm->defines = 0;
    // A session IS a compilation unit, so a library template compiled for the
    // previous one is stale here for the same reason it is in func_create: it
    // snapshotted that unit's directive state. See VM.unit_id.
    vm->unit_id++;
    vm->structs = (VMStructTable*)mem_alloc(&vm->run.mem, sizeof(VMStructTable));
    if (vm->structs) vm->run.sys->memset(vm->structs, 0, sizeof(VMStructTable));
    vm->rx_active = 1;
    vm->rx_func_depth = 0;
    vm->ac_off = 1;   // no unit-wide write census exists for a statement compiled alone
}

void vm_rx_set_source(VM *vm, ParseResult *pres) {
    if (pres) {
        vm->txt_to_ref        = pres->txt_to_ref;
        vm->num_txt_to_ref    = pres->num_txt_to_ref;
        vm->line_offsets      = pres->line_offsets;
        vm->num_lines_offsets = pres->num_lines_offsets;
    } else {
        vm->txt_to_ref = 0; vm->num_txt_to_ref = 0;
        vm->line_offsets = 0; vm->num_lines_offsets = 0;
    }
}

void vm_rx_directive_reset(VM *vm, int flags) {
    for (int i = 0; i < 8; i++) {
        vm->type_map[i] = (VTKind)i;  vm->literal_map[i] = (VTKind)i;
        vm->type_shift[i] = 0;        vm->literal_shift[i] = 0;
    }
    vm->flags = flags;
    vm->dir_stack_depth = 0;
    vm->defines = 0;
}

int vm_rx_directive_apply(VM *vm, ASTNode *stmt) {
    ASTNode *chain[16];
    int n = unpack_directive(stmt, chain, 16);
    if (n <= 0) return 1;
    vm->run.last_error = 0;
    vm->err_ctx = stmt;
    int ok = compile_directive(vm, chain, n, stmt);
    vm->err_ctx = 0;
    return ok;
}

MemBackend vm_rx_use_arena(VM *vm, Arena *a) {
    MemBackend saved = vm->run.mem;
    if (vm->rx_arena_depth++ == 0) { vm->rx_home_mem = saved; vm->rx_funcs_mark = vm->run.funcs; }
    vm->run.mem.arena = a;
    vm->run.mem.alloc = mem_arena_alloc;
    vm->run.mem.buf   = 0;
    vm->run.mem.used  = 0;
    vm->run.mem.cap   = 0;
    return saved;
}

// Leaving the outermost arena: nothing compiled into the arenas may outlive
// them through the unit's tables. Templates linked since go off the function
// list (a library template was compiled into the home backend and stays), and
// every specialisation made since is forgotten.
void vm_rx_restore_mem(VM *vm, MemBackend saved) {
    vm->run.mem = saved;
    if (vm->rx_arena_depth == 0 || --vm->rx_arena_depth > 0) return;
    Func **pp = &vm->run.funcs;
    while (*pp && *pp != vm->rx_funcs_mark) {
        if ((*pp)->is_lib) pp = &(*pp)->next;
        else *pp = (*pp)->next;
    }
    vm_rx_forget_specs(vm);
}

static Func *rx_func_begin(VM *vm, ASTNode *at) {
    vm->run.last_error = 0;
    vm->run.error_row = -1;
    vm->run.error_col = -1;
    vm->bind_failed = 0;
    vm->err_ctx = at;
    Func *f = func_alloc(&vm->run);
    if (!f) return 0;
    f->structs = vm->structs;
    f->rx_out_slot = -1;
    return f;
}

static Func *rx_func_finish(VM *vm, Func *f, int *items, int count, int ok) {
    vm->err_ctx = 0;
    if (!ok || vm->bind_failed) { vm->bind_failed = 0; return 0; }
    int blk = ir_new(vm, f, IR_BLOCK);
    if (count > 0) {
        f->nodes[blk].items_begin = ir_alloc_items(vm, f, count);
        for (int i = 0; i < count; i++) f->child_indices[f->nodes[blk].items_begin + i] = items[i];
    }
    f->nodes[blk].n_items = count;
    f->body = blk;
    f->ret_type.kind = VMT_VOID;
    layout_frame(f);
    f->flags = vm->flags | VM_FLAG_SNAPSHOT;
    f->ct_purity = CT_PURITY_UNKNOWN;
    return f;
}

Func *vm_compile_stmt(VM *vm, ASTNode *stmt) {
    Func *f = rx_func_begin(vm, stmt);
    if (!f) return 0;
    int *items = 0, count = 0, cap = 0;
    int saved_depth = vm->blk_depth;
    vm->blk_depth = 1;
    int r = compile_stmt_into(vm, f, stmt, &items, &count, &cap);
    vm->blk_depth = saved_depth;
    return rx_func_finish(vm, f, items, count, r >= 0);
}

Func *vm_compile_expr(VM *vm, ASTNode *expr) {
    Func *f = rx_func_begin(vm, expr);
    if (!f) return 0;
    int saved_depth = vm->blk_depth;
    vm->blk_depth = 1;
    VMType t;
    int e = compile_expr(vm, f, expr, &t);
    int stmt = -1;
    if (e >= 0) {
        e = ref_to_value(vm, f, e, &t);
        if (t.kind == VMT_VOID) {
            stmt = ir_new(vm, f, IR_EXPR_STMT);
            f->nodes[stmt].a = e;
        } else {
            VMSym *s = sym_add(vm, f, 0, ct_vmtype_clear_shift(t));
            s->shift = ct_shift(t);
            s->used  = 1;
            int slot = (int)(s - f->syms);
            int lv = ir_new(vm, f, IR_LOCAL);
            f->nodes[lv].type = f->syms[slot].type;
            f->nodes[lv].ki   = slot;
            stmt = ir_new(vm, f, IR_ASSIGN);
            f->nodes[stmt].a = lv;
            f->nodes[stmt].b = e;
            f->rx_out_slot = slot;
        }
    }
    vm->blk_depth = saved_depth;
    return rx_func_finish(vm, f, &stmt, 1, e >= 0);
}

int vm_rx_forget_const(VM *vm, InternID name) {
    VMConst **pp = &vm->consts;
    while (*pp) {
        if ((*pp)->name == name) { *pp = (*pp)->next; return 1; }
        pp = &(*pp)->next;
    }
    return 0;
}

void vm_rx_forget_specs(VM *vm) {
    for (Func *g = vm->run.funcs; g; g = g->next) { g->n_specs = 0; g->specs = 0; g->cap_specs = 0; }
}

int vm_rx_forget_struct(VM *vm, InternID name) {
    int id = struct_find(vm, name);
    if (!id) return 0;
    vm->structs->items[id - 1].name = 0;
    return 1;
}

int    vm_rx_shift(VMType t)                 { return ct_shift(t); }
VMType vm_rx_set_shift(VMType t, int shift)  { return ct_set_shift(t, shift); }
VMType vm_rx_clear_shift(VMType t)           { return ct_vmtype_clear_shift(t); }
VTKind vm_rx_promote(VM *vm, VTKind a, VTKind b) { return promote(vm, a, b); }
const char *vm_rx_type_label(VMType t, int shift, char *buf, int buf_sz) {
    return type_label(ct_set_shift(t, shift), buf, buf_sz);
}
int    vm_rx_is_array(VTKind k) { return is_array(k); }
int    vm_rx_is_slice(VTKind k) { return is_slice(k); }
VTKind vm_rx_arr_elem(VTKind k) { return arr_elem(k); }

InternID vm_rx_kw_head(VM *vm, ASTNode *stmt, ASTNode **args, int *n, int max) {
    *n = 0;
    ASTNode *u = unwrap_chain(stmt);
    if (!u) return 0;
    ASTNode *head = find_kw_head(vm, u, args, n, max);
    return head ? head->token : 0;
}
ASTNode *vm_rx_subscope_block(ASTNode *n)  { return subscope_block(n); }
ASTNode *vm_rx_unwrap_chain(ASTNode *n)    { return unwrap_chain(n); }
ASTNode *vm_rx_paren_inner(ASTNode *n)     { return paren_inner(n); }
int      vm_rx_is_block_node(ASTNode *n)   { return is_block_node(n); }
int      vm_rx_is_indent_block(ASTNode *n) { return is_indent_block(n); }
int      vm_rx_is_ident(ASTNode *n)        { return is_ident(n); }
ASTNode *vm_rx_splat_operand(ASTNode *n)   { return is_splat_node(n) ? paren_inner(n->right) : 0; }
ASTNode *vm_rx_index_base(ASTNode *n, ASTNode **index) {
    n = paren_inner(n);
    if (!n || n->token != TOK_EMPTYSTRING || !n->left || !is_bracket(n->right)) return 0;
    ASTNode *ie = index_expr(n);
    if (!ie || is_range_node(ie)) return 0;
    *index = ie;
    return n->left;
}
int      vm_rx_stmt_is_directive(ASTNode *stmt) { return stmt_is_directive(stmt); }
ASTNode *vm_rx_directive_indent_block(ASTNode *stmt) { return directive_indent_block(stmt); }
int      vm_rx_expand_call_args(VM *vm, ASTNode *node, ASTNode *par, ASTNode **out, int max) {
    return expand_call_args(vm, node, par, out, max);
}
void     vm_rx_error_at(VM *vm, ASTNode *at, const char *msg) { vm_set_error_at(vm, at, msg); }

int vm_rx_arrow_params(VM *vm, ASTNode *arrow, InternID *names, ASTNode **defaults, int max) {
    ASTNode *params = arrow ? arrow->left : 0;
    ASTNode *plist[VM_MAX_PARAMS + 1];
    int n = 0;
    if (is_ident(params)) {
        plist[0] = params;
        n = 1;
    } else if (is_paren(params)) {
        n = param_list(params, plist, VM_MAX_PARAMS);
        if (params->items.size == 1) {
            ASTNode *only = *(ASTNode**)array_get(&params->items, 0);
            if (!only->left && !only->right && !only->number_flags && only->token == 0
                && only->items.size == 0 && only->left_bracket == 0)
                n = 0;
        }
    } else {
        vm_set_error_at(vm, params ? params : arrow, "bad function params");
        return -1;
    }
    if (n > max || n > VM_MAX_PARAMS) { vm_set_error_at(vm, arrow, "too many params"); return -1; }
    for (int i = 0; i < n; i++) {
        ASTNode *def;
        ASTNode *p = param_split_default(paren_inner(plist[i]), &def);
        if (p && p->token == TOK_COLON && is_ident(p->left)) p = p->left;
        if (!is_ident(p)) { vm_set_error_at(vm, plist[i], "a parameter is a name"); return -1; }
        names[i] = p->token;
        defaults[i] = def;
    }
    return n;
}

ASTNode *vm_rx_arrow_body(ASTNode *arrow) { return arrow ? unwrap_chain(arrow->right) : 0; }

ASTNode *vm_rx_as_arrow(ASTNode *n) {
    n = paren_inner(n);
    return n && n->token == TOK_ARROW_F ? n : 0;
}
