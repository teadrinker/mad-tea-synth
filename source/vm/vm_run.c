// vm_run.c - Minimal runtime interpreter + serialization for embedded systems.
// No parser code dependency.  parser/tokens.h is just enum constants.
// When included from vm.c (unity build), skip re-compilation.
#ifndef VM_RUN_INCLUDED
#define VM_RUN_INCLUDED

#include "vm_run.h"
#include "vm_types.h"
#include "parser/tokens.h"
#ifndef VM_NO_MATH
#include "common/math_pure.h"
#include "common/math_fixedp.h"
#include "vm_fmt.h"
#endif
#include "common/string_pure.h"
#include <stdint.h>

// Public-symbol decoration (vm_config.h VM_API_SUFFIX/VM_API()): rename every
// externally-visible definition and call site below to VM_API(name), so this file
// can be compiled a second time under a different suffix without colliding at link
// time. With VM_API_SUFFIX empty (the default) each expands to itself.
#define func_alloc               VM_API(func_alloc)
#define func_register_id         VM_API(func_register_id)
#define register_c_func_1arg     VM_API(register_c_func_1arg)
#define register_c_func_2arg     VM_API(register_c_func_2arg)
#define register_c_func_3arg     VM_API(register_c_func_3arg)
#define register_c_func_4arg     VM_API(register_c_func_4arg)
#define register_c_func_5arg     VM_API(register_c_func_5arg)
#define register_c_func_6arg     VM_API(register_c_func_6arg)
#define register_c_func_7arg     VM_API(register_c_func_7arg)
#define register_c_func_1arg_ctx VM_API(register_c_func_1arg_ctx)
#define register_c_func_2arg_ctx VM_API(register_c_func_2arg_ctx)
#define register_c_func_3arg_ctx VM_API(register_c_func_3arg_ctx)
#define register_c_func_4arg_ctx VM_API(register_c_func_4arg_ctx)
#define register_c_func_5arg_ctx VM_API(register_c_func_5arg_ctx)
#define register_c_func_6arg_ctx VM_API(register_c_func_6arg_ctx)
#define register_c_func_7arg_ctx VM_API(register_c_func_7arg_ctx)
#define vm_register_builtins     VM_API(vm_register_builtins)
#define vm_run_register_builtins VM_API(vm_run_register_builtins)
#define vm_run_init              VM_API(vm_run_init)
#define vm_run_deinit            VM_API(vm_run_deinit)
#define vm_create                VM_API(vm_create)
#define vm_create_minimal        VM_API(vm_create_minimal)
#define vm_destroy               VM_API(vm_destroy)
#define vm_last_error            VM_API(vm_last_error)
#define vm_last_error_row        VM_API(vm_last_error_row)
#define vm_last_error_col        VM_API(vm_last_error_col)
#define vm_set_user_data          VM_API(vm_set_user_data)
#define vm_set_globals           VM_API(vm_set_globals)
#define vm_get_globals           VM_API(vm_get_globals)
#define func_first               VM_API(func_first)
#define func_frame_size          VM_API(func_frame_size)
#define func_param_count         VM_API(func_param_count)
#define func_param_type          VM_API(func_param_type)
#define func_param_used          VM_API(func_param_used)
#define func_return_type         VM_API(func_return_type)
#define args_bind                VM_API(args_bind)
#define args_set_i32             VM_API(args_set_i32)
#define args_set_f32             VM_API(args_set_f32)
#define args_set_f64             VM_API(args_set_f64)
#define args_set_i64             VM_API(args_set_i64)
#define args_set_arr             VM_API(args_set_arr)
#define args_get_i32_result      VM_API(args_get_i32_result)
#define args_get_f32_result      VM_API(args_get_f32_result)
#define args_get_f64_result      VM_API(args_get_f64_result)
#define args_get_i64_result      VM_API(args_get_i64_result)
#define func_serialize_size      VM_API(func_serialize_size)
#define func_serialize           VM_API(func_serialize)
#define func_serialize_compact_size VM_API(func_serialize_compact_size)
#define func_serialize_compact   VM_API(func_serialize_compact)
#define func_deserialize         VM_API(func_deserialize)
#define func_run                 VM_API(func_run)
#define func_budget_left         VM_API(func_budget_left)

// When building standalone (not via vm.c), provide stubs for parser data. Gated
// separately from VM_HAS_FUNC_ALLOC so a second decorated copy of this file linked
// beside the real tokens.c -- which already defines these two arrays undecorated --
// can opt out of the stub alone. Define VM_HAS_TOKEN_NAMES to skip this.
#if !defined(VM_HAS_FUNC_ALLOC) && !defined(VM_HAS_TOKEN_NAMES)
// Both sizes come from tokens.h, which this file always has: the line below
// uses TOK_MATHS_FIRST unguarded. A `#ifndef TOK_COUNT / #define TOK_COUNT 49`
// fallback sat here and was dead -- and 20 tokens out of date.
const char *FixedTokenNames[TOK_COUNT] = { 0 };
const char *MathTokenNames[TOK_MATHS_LAST - TOK_MATHS_FIRST] = { 0 };
#endif

// When building without vm.c, provide func_alloc / func_register_id.

// arena-backed MemBackend allocator (dynamic build).
static void *mem_arena_alloc(MemBackend *m, size_t n) {
    return arena_alloc(m->arena, n);
}

// Fixed-buffer bump allocator (compiled always; selected at runtime via
// cfg->mem_buf in vm_run_init).  arena==NULL means we can't grow.
static void *mem_bump_alloc(MemBackend *m, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (m->used + n > m->cap) return NULL;
    void *p = m->buf + m->used;
    m->used += n;
    return p;
}

#ifndef VM_HAS_FUNC_ALLOC

Func *func_alloc(VmRun *run) {
    Func *f = (Func*)mem_alloc(&run->mem, sizeof(Func));
    if (!f) { run->last_error = "out of memory"; return NULL; }
    run->sys->memset(f, 0, sizeof(Func));
    f->run = run;
    f->ret_type.kind = VMT_VOID;
    f->func_id = -1;
    return f;
}

void func_register_id(VmRun *run, Func *f) {
    if (f->func_id >= 0) return;
    if (run->n_func_ids == run->cap_func_ids) {
        if (run->mem.arena == NULL) {
            run->last_error = "func id table full";
            return;
        }
        int nc = run->cap_func_ids ? run->cap_func_ids * 2 : 16;
        struct Func **ns = (struct Func**)mem_alloc(&run->mem, sizeof(struct Func*) * nc);
        if (!ns) { run->last_error = "out of memory"; return; }
        for (int i = 0; i < run->n_func_ids; i++) ns[i] = run->funcs_by_id[i];
        run->funcs_by_id = ns;
        run->cap_func_ids = nc;
    }
    f->func_id = run->n_func_ids;
    run->funcs_by_id[run->n_func_ids++] = f;
}
#endif

#ifndef VM_NO_COMPILER
static void vm_set_error(VmRun *run, const char *msg) {
    // Store a copy in the arena so it outlives transient strings.
    int n = 0; while (msg[n]) n++;
    char *buf = (char*)mem_alloc(&run->mem, n + 1);
    for (int i = 0; i <= n; i++) buf[i] = msg[i];
    run->last_error = buf;
    // Position stays at whatever was last set by vm_set_error_at.
}
#endif


static int is_scalar(VTKind k) { return k == VMT_I32
    || (VM_HAS_F32 && k == VMT_F32) || (VM_HAS_F64 && k == VMT_F64) || (VM_HAS_I64 && k == VMT_I64); }
#ifndef VM_NO_ARRAYS
static int is_array(VTKind k)  { return k == VMT_ARR_I32
    || (VM_HAS_F32 && k == VMT_ARR_F32) || (VM_HAS_F64 && k == VMT_ARR_F64) || (VM_HAS_I64 && k == VMT_ARR_I64); }
static int is_slice(VTKind k)  { return k == VMT_SLICE_I32
    || (VM_HAS_F32 && k == VMT_SLICE_F32) || (VM_HAS_F64 && k == VMT_SLICE_F64) || (VM_HAS_I64 && k == VMT_SLICE_I64); }
static VTKind arr_elem(VTKind k) {
    if (VM_HAS_I64 && (k == VMT_ARR_I64 || k == VMT_SLICE_I64)) return VMT_I64;
    if (VM_HAS_F64 && (k == VMT_ARR_F64 || k == VMT_SLICE_F64)) return VMT_F64;
    if (VM_HAS_F32 && (k == VMT_ARR_F32 || k == VMT_SLICE_F32)) return VMT_F32;
    if (               k == VMT_ARR_I32 || k == VMT_SLICE_I32 ) return VMT_I32;
    return VMT_VOID;
}
#else
static int is_array(VTKind k) { (void)k; return 0; }
static int is_slice(VTKind k) { (void)k; return 0; }
static VTKind arr_elem(VTKind k) { (void)k; return VMT_VOID; }
#endif
#ifndef VM_NO_COMPILER
#ifndef VM_NO_ARRAYS
static VTKind arr_of(VTKind elem) {
    if (elem == VMT_I32) return VMT_ARR_I32;
    if (elem == VMT_F32) return VMT_ARR_F32;
    if (elem == VMT_F64) return VMT_ARR_F64;
    if (elem == VMT_I64) return VMT_ARR_I64;
    return VMT_VOID;
}
static VTKind slice_of(VTKind elem) {
    if (elem == VMT_I32) return VMT_SLICE_I32;
    if (elem == VMT_F32) return VMT_SLICE_F32;
    if (elem == VMT_F64) return VMT_SLICE_F64;
    if (elem == VMT_I64) return VMT_SLICE_I64;
    return VMT_VOID;
}
#else
static VTKind arr_of(VTKind elem) { (void)elem; return VMT_VOID; }
static VTKind slice_of(VTKind elem) { (void)elem; return VMT_VOID; }
#endif
#endif

static int vt_size_of(VTKind k) {
    if (               k == VMT_I32  || (VM_HAS_F32 && k == VMT_F32)) return 4;
    if ((VM_HAS_F64 && k == VMT_F64) || (VM_HAS_I64 && k == VMT_I64)) return 8;
    return 0;
}

// ---------- Sub-word element packing ----------
// A packed array is a plain i32 word array; element i lives at word (i / epw), bit
// offset (i % epw) * bits, where epw = 32 / bits. Valid widths are 1/2/4/8/16 --
// all divide 32, so no element straddles a word boundary.
//
// Because write_i32 stores LSB-first *explicitly*, the frame layout is
// little-endian by definition rather than by host accident: for bits == 8 element i
// is exactly frame byte i on every host. pack_load/pack_store still go through
// read_i32/write_i32 for every width, so correctness never depends on that -- it is
// only what makes the u8 fast path and the (const unsigned char*) native ABI
// valid.

// Defined even under VM_NO_ARRAYS: the array branches of args_set_arr and
// friends still compile there (is_array() just always answers 0), so the
// symbol has to exist. static inline keeps it from warning when unused.
static inline int pack_bytes_for(int n_elems, int bits) {
    if (bits <= 0) return 0;
    int words = (n_elems * bits + 31) / 32;
    return words * 4;
}

// Bytes a COPY of `n` elements touches. u8 and u16 are exact rather than whole
// words: their elements are read byte by byte (pack_load), and a struct record --
// an [S]u8 whose stride is S, not a word multiple -- must not drag its neighbour's
// first bytes along.
static inline size_t agg_copy_bytes(VTKind kind, int pack_bits, int n) {
    if (pack_bits == 8)  return (size_t)n;
    if (pack_bits == 16) return (size_t)n * 2;
    if (pack_bits)       return (size_t)pack_bytes_for(n, pack_bits);
    return (size_t)n * (size_t)vt_size_of(arr_elem(kind));
}
#ifndef VM_NO_COMPILER
static int vt_align_of(VTKind k) {
    if ((VM_HAS_F64 && k == VMT_F64) || (VM_HAS_I64 && k == VMT_I64)) return 8;
    if (is_slice(k)) return 8;
    return 4;
}
static int vt_slot_bytes(VMType t) {
    if (is_scalar(t.kind)) return vt_size_of(t.kind);
    if (is_array(t.kind))  return t.pack_bits ? pack_bytes_for(t.len, t.pack_bits)
                                              : vt_size_of(arr_elem(t.kind)) * t.len;
    if (is_slice(t.kind))  return 16;  // ptr (8) + len (4) padded to 16
    return 0;
}
#endif

static int op_is_compare(int op) {
    return op == OP_LT || op == OP_LE || op == OP_GT || op == OP_GE
        || op == OP_EQ || op == OP_NE;
}

// ---------- C-function registration ----------

#ifndef VM_NO_MATH
// Ensure cfunc_table has a slot for tok; resize with realloc if needed.
static CFuncEntry *cfunc_ensure(VmRun *run, int tok) {
    int idx = tok - TOK_MATHS_FIRST;
    if (idx < 0) return 0;
    if (idx >= run->cfunc_table_cap) {
        int new_cap = idx + 1;
        if (new_cap < run->cfunc_table_cap * 2)
            new_cap = run->cfunc_table_cap * 2;
        if (run->mem.arena == NULL) {
            run->last_error = "cfunc table overflow";
            return NULL;
        }
        CFuncEntry *nt = (CFuncEntry*)S_REALLOC(run->sys, 
            run->cfunc_table, sizeof(CFuncEntry) * new_cap);
        if (!nt) return 0;
        run->sys->memset(nt + run->cfunc_table_cap, 0,
            sizeof(CFuncEntry) * (new_cap - run->cfunc_table_cap));
        run->cfunc_table     = nt;
        run->cfunc_table_cap = new_cap;
    }
    return &run->cfunc_table[idx];
}
#endif

// Shared tail: create the script-visible Func wrapper.
#ifndef VM_NO_MATH
static void register_native_func(VM *vm, int tok, const char *script_name, int n_params) {
    Func *f          = func_alloc(&vm->run);
    if (!f) return;
#ifdef VM_NO_COMPILER
    f->name          = 0;
#else
    f->name          = intern_c_string(&vm->builtin_owner, script_name);
#endif
    f->native_tok    = tok;
    f->n_params      = n_params;
    f->ret_type.kind = VMT_VOID;  // resolved per call-site
    f->next          = vm->run.funcs;
    vm->run.funcs    = f;
    func_register_id(&vm->run, f);
}

#ifndef VM_NO_MATH
void register_c_func_0arg(VM *vm, int tok, const char *script_name,
    const char *f64_name, double (*f64_fn)(void))
{
    CFuncEntry *e = cfunc_ensure(&vm->run, tok);
    if (!e) return;
    vm->run.sys->memset(e, 0, sizeof(*e));
    e->n_args   = 0;
    e->f64_name = f64_name;
    e->f64_fn   = (void*)f64_fn;
    register_native_func(vm, tok, script_name, 0);
}

// One body per arity, so the fn-pointer types stay declared rather than cast at
// the call site. The IF_VM_HAS_* wrappers stand in for #if, which a macro body
// can't contain. CTX marks the ctx variants: every fn slot takes the VM's
// user_data as a leading void* (see CFuncEntry.wants_ctx), while the NAME slots
// stay the plain global-bound C names the emitter writes.
#if VM_HAS_F32
#define IF_VM_HAS_F32(x) x
#else
#define IF_VM_HAS_F32(x)
#endif
#if VM_HAS_F64
#define IF_VM_HAS_F64(x) x
#else
#define IF_VM_HAS_F64(x)
#endif

#define REG_TAIL(N, CTX)                                       \
    CFuncEntry *e = cfunc_ensure(&vm->run, tok);               \
    if (!e) return;                                            \
    e->n_args   = (N);                                         \
    IF_VM_HAS_F32(e->f32_name = f32_name;  e->f32_fn = (void*)f32_fn;) \
    IF_VM_HAS_F64(e->f64_name = f64_name;  e->f64_fn = (void*)f64_fn;) \
    e->i32_name = i32_name;  e->i32_fn = (void*)i32_fn;        \
    e->fx_name  = fx_name;   e->fx_fn  = (void*)fx_fn;         \
    e->fx_shift = fx_shift;                                    \
    e->ext      = 0;                                           \
    if (CTX) e->wants_ctx = 1;                                 \
    register_native_func(vm, tok, script_name, (N))

void register_c_func_1arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float),
    const char *f64_name, double (*f64_fn)(double),
    const char *i32_name, int    (*i32_fn)(int),
    const char *fx_name,  int    (*fx_fn)(int),
    int fx_shift)
{ REG_TAIL(1, 0); }

void register_c_func_2arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float),
    const char *f64_name, double (*f64_fn)(double, double),
    const char *i32_name, int    (*i32_fn)(int, int),
    const char *fx_name,  int    (*fx_fn)(int, int),
    int fx_shift)
{ REG_TAIL(2, 0); }

// As register_c_func_2arg, but every variant takes the VM's user_data
// (VmRun.user_data) as a leading void* argument. Lets a native reach
// host-attached state without a global; see CFuncEntry.wants_ctx.
void register_c_func_2arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int),
    int fx_shift)
{ REG_TAIL(2, 1); }

void register_c_func_3arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int),
    int fx_shift)
{ REG_TAIL(3, 0); }

void register_c_func_4arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int),
    int fx_shift)
{ REG_TAIL(4, 0); }

void register_c_func_5arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(5, 0); }

void register_c_func_6arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(6, 0); }

void register_c_func_7arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(7, 0); }

void register_c_func_1arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float),
    const char *f64_name, double (*f64_fn)(void*, double),
    const char *i32_name, int    (*i32_fn)(void*, int),
    const char *fx_name,  int    (*fx_fn)(void*, int),
    int fx_shift)
{ REG_TAIL(1, 1); }

void register_c_func_3arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int),
    int fx_shift)
{ REG_TAIL(3, 1); }

void register_c_func_4arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int),
    int fx_shift)
{ REG_TAIL(4, 1); }

void register_c_func_5arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(5, 1); }

void register_c_func_6arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(6, 1); }

void register_c_func_7arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int, int),
    int fx_shift)
{ REG_TAIL(7, 1); }

#undef REG_TAIL
#undef IF_VM_HAS_F32
#undef IF_VM_HAS_F64

void register_c_func_sig(VM *vm, int tok, const char *script_name,
    const char *c_name, void *fn,
    int n_args, const VTKind *arg_kinds, int ret_kind, int scalar_shift)
{
    CFuncEntry *e = cfunc_ensure(&vm->run, tok);
    if (!e) return;
    if (n_args < 0 || n_args > VM_MAX_CFUNC_ARGS) return;
    e->n_args     = n_args;
    e->has_sig    = 1;
    e->n_sig_args = n_args;
    e->sig_scalar_shift = scalar_shift;
    e->ret_kind   = (VTKind)ret_kind;
    e->sig_name   = c_name;
    e->sig_fn     = fn;
    for (int i = 0; i < n_args; i++) e->arg_kinds[i] = arg_kinds[i];
    e->ext        = 0;
    register_native_func(vm, tok, script_name, n_args);
}

// register_c_func_sig with wants_ctx: `fn` takes the VM's user_data as a leading
// void*, ahead of the scalars and the (ptr, len) slice pair. `c_name` still names
// the plain, ctx-less C function the emitter writes into generated C. `fn` is void*
// because the shape varies per registration, so dispatch casts it to the exact
// prototype.
void register_c_func_sig_ctx(VM *vm, int tok, const char *script_name,
    const char *c_name, void *fn,
    int n_args, const VTKind *arg_kinds, int ret_kind, int scalar_shift)
{
    register_c_func_sig(vm, tok, script_name, c_name, fn,
                        n_args, arg_kinds, ret_kind, scalar_shift);
    int idx = tok - TOK_MATHS_FIRST;
    // register_c_func_sig refuses out-of-range arities; only mark an entry it
    // actually wrote.
    if (idx >= 0 && idx < vm->run.cfunc_table_cap && vm->run.cfunc_table[idx].has_sig)
        vm->run.cfunc_table[idx].wants_ctx = 1;
}

// Mark the last `n_defaults` parameters of an already-registered native as
// optional, defaulting to 0 when the caller omits them; compile_call synthesizes
// the literal. Natives have no param-list AST for the usual default machinery, so
// the only values this path supplies are 0 and (per slot, via
// register_c_func_default_one) 1.0. Must be called AFTER the matching
// register_c_func_*, and defaults are trailing.
void register_c_func_defaults(VM *vm, int tok, int n_defaults) {
    for (Func *fn = vm->run.funcs; fn; fn = fn->next) {
        if (fn->native_tok == tok) { fn->n_defaults = n_defaults; return; }
    }
}

void register_c_func_param_names(VM *vm, int tok, const char *names) {
    for (Func *fn = vm->run.funcs; fn; fn = fn->next) {
        if (fn->native_tok == tok) { fn->param_names = names; return; }
    }
}

void register_c_func_no_value(VM *vm, int tok) {
    for (Func *fn = vm->run.funcs; fn; fn = fn->next) {
        if (fn->native_tok == tok) { fn->native_no_value = 1; return; }
    }
}

// Switch one optional slot's synthesized default from 0 to 1.0 -- see vm.h for
// why the draw primitives' alpha needs it.
void register_c_func_default_one(VM *vm, int tok, int arg_index) {
    if (arg_index < 0 || arg_index >= 32) return;   // native_default_one is 32 bits
    for (Func *fn = vm->run.funcs; fn; fn = fn->next) {
        if (fn->native_tok == tok) {
            fn->native_default_one |= 1u << arg_index;
            return;
        }
    }
}

void register_c_func_arg_bytes(VM *vm, int tok, int arg_index) {
    int idx = tok - TOK_MATHS_FIRST;
    if (idx < 0 || idx >= vm->run.cfunc_table_cap) return;
    if (arg_index < 0 || arg_index >= VM_MAX_CFUNC_ARGS) return;
    vm->run.cfunc_table[idx].arg_pack_bits[arg_index] = 8;
}

// See vm.h. Two separate questions on one entry: a pure function may still need
// its call kept (mix is both), and a kept call may be impure.
void set_c_func_keep_call(VM *vm, int tok, int keep) {
    int idx = tok - TOK_MATHS_FIRST;
    if (idx < 0 || idx >= vm->run.cfunc_table_cap) return;
    vm->run.cfunc_table[idx].keep_call = (unsigned char)(keep ? 1 : 0);
}

void set_c_func_pure(VM *vm, int tok, int pure) {
    int idx = tok - TOK_MATHS_FIRST;
    if (idx < 0 || idx >= vm->run.cfunc_table_cap) return;
    vm->run.cfunc_table[idx].is_pure = (unsigned char)(pure ? 1 : 0);
}
#endif

#endif

#ifndef VM_NO_MATH

static void mark_builtin_math_pure(VmRun *run) {
    for (int tok = TOK_MATHS_FIRST; tok < TOK_MATHS_LAST; tok++) {
        int idx = tok - TOK_MATHS_FIRST;
        if (idx < run->cfunc_table_cap) run->cfunc_table[idx].is_pure = 1;
    }
}
#endif

#if !defined(VM_NO_COMPILER) && !defined(VM_NO_MATH)
// What the editor shows for a call to a built-in: one table, so a new built-in
// has one place to go.
static void builtin_param_names(VM *vm) {
    static const struct { int tok; const char *names; } k_names[] = {
        { TOK_SIN, "x" },  { TOK_COS, "x" },  { TOK_EXP, "x" },  { TOK_FLOOR, "x" },
        { TOK_FRAC, "x" }, { TOK_LOG, "x" },  { TOK_ABS, "x" },  { TOK_SQRT, "x" },
        { TOK_SIN01, "t" }, { TOK_COS01, "t" },
        { TOK_POW, "x, y" }, { TOK_IPOW, "x, n" }, { TOK_FMOD, "x, y" }, { TOK_ATAN2, "y, x" },
        { TOK_MIN, "a, b" }, { TOK_MAX, "a, b" },
        { TOK_CLAMP, "x, lo, hi" },
        { TOK_LINEARSTEP, "a, b, x" },  { TOK_SMOOTHSTEP, "a, b, x" },  { TOK_SMOOTHERSTEP, "a, b, x" },
        { TOK_LINEARSTEPA, "a, b, x" }, { TOK_SMOOTHSTEPA, "a, b, x" }, { TOK_SMOOTHERSTEPA, "a, b, x" },
        { TOK_MIX, "a, b, t" },
        { TOK_I32, "x" }, { TOK_F32, "x" }, { TOK_F64, "x" }, { TOK_I64, "x" },
    };
    for (int i = 0; i < (int)(sizeof(k_names) / sizeof(k_names[0])); i++)
        register_c_func_param_names(vm, k_names[i].tok, k_names[i].names);
}
#endif

void vm_register_builtins(VM *vm) {
    VmRun *run = &vm->run;
#ifndef VM_NO_MATH
    // i32_name/i32_fn (raw int, shift 0) and fx_name/fx_fn/fx_shift (fixed QM.N)
    // are independent slots -- see CFuncEntry. floor, frac, min and max never
    // actually reach either at i32/fixed kind: compile_call inlines those as
    // bitwise ops / selects at whatever shift the argument carries, so their i32
    // slots below are left NULL (min/max keep their raw-int C names registered for
    // hand-built IR and codegen).
    register_c_func_1arg(vm, TOK_SIN,   "sin",   REG_C_FUNC_F32_1ARG("m_sinf", m_sinf), REG_C_FUNC_F64_1ARG("m_sin", m_sin), NULL, NULL, "fx16_sin",  fx16_sin,  16);
    register_c_func_1arg(vm, TOK_COS,   "cos",   REG_C_FUNC_F32_1ARG("m_cosf", m_cosf), REG_C_FUNC_F64_1ARG("m_cos", m_cos), NULL, NULL, "fx16_cos",  fx16_cos,  16);
    register_c_func_1arg(vm, TOK_EXP,   "exp",   REG_C_FUNC_F32_1ARG("m_expf", m_expf), REG_C_FUNC_F64_1ARG("m_exp", m_exp), NULL, NULL, "fx16_exp",  fx16_exp,  16);
    register_c_func_1arg(vm, TOK_FLOOR, "floor", REG_C_FUNC_F32_1ARG("m_floorf",m_floorf),REG_C_FUNC_F64_1ARG("m_floor",m_floor), NULL, NULL, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_FRAC,  "frac",  REG_C_FUNC_F32_1ARG("m_fracf",m_fracf), REG_C_FUNC_F64_1ARG("m_frac", m_frac), NULL, NULL, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_POW,   "pow",   REG_C_FUNC_F32_2ARG("m_powf", m_powf), REG_C_FUNC_F64_2ARG("m_pow", m_pow), NULL, NULL, "fx16_pow",  fx16_pow,  16);
    // ipow: exact integer power via its i32 slot (m_ipow). Float operands fall
    // back to pow so a promoted call still evaluates; ** compilation only routes
    // integer operands here (see compile_expr in vm.c).
    register_c_func_2arg(vm, TOK_IPOW,  "ipow",  REG_C_FUNC_F32_2ARG("m_powf", m_powf), REG_C_FUNC_F64_2ARG("m_pow", m_pow), "m_ipow", m_ipow, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_LOG,   "log",   REG_C_FUNC_F32_1ARG("m_logf", m_logf), REG_C_FUNC_F64_1ARG("m_log", m_log), NULL, NULL, "fx16_log",  fx16_log,  16);
    // abs is the motivating example for the raw/fixed split: the two slots
    // may diverge in the future, but today they're the same bit operation
    // (sign-flip on the raw two's-complement word) regardless of where the
    // binary point sits, so both point at m_iabs.
    register_c_func_1arg(vm, TOK_ABS,   "abs",   REG_C_FUNC_F32_1ARG("m_fabsf",m_fabsf), REG_C_FUNC_F64_1ARG("m_fabs", m_fabs), "m_iabs", m_iabs, "m_iabs", m_iabs, 16);
    register_c_func_2arg(vm, TOK_MIN,   "min",   REG_C_FUNC_F32_2ARG("m_fminf",m_fminf), REG_C_FUNC_F64_2ARG("m_fmin", m_fmin), "m_imin", m_imin, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_MAX,   "max",   REG_C_FUNC_F32_2ARG("m_fmaxf",m_fmaxf), REG_C_FUNC_F64_2ARG("m_fmax", m_fmax), "m_imax", m_imax, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_FMOD,  "fmod",  REG_C_FUNC_F32_2ARG("m_fmodf",m_fmodf), REG_C_FUNC_F64_2ARG("m_fmod", m_fmod), NULL, NULL, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_SQRT,  "sqrt",  REG_C_FUNC_F32_1ARG("m_sqrtf",m_sqrtf), REG_C_FUNC_F64_1ARG("m_sqrt", m_sqrt), NULL, NULL, "fx16_sqrt", fx16_sqrt, 16);
    // fx16_atan2 returns Q16.16 radians in (-pi, pi], same range/units as
    // the f32/f64 variants above -- callers see consistent atan2()
    // semantics no matter which variant the VM ends up dispatching to.
    register_c_func_2arg(vm, TOK_ATAN2, "atan2", REG_C_FUNC_F32_2ARG("m_atan2f",m_atan2f),REG_C_FUNC_F64_2ARG("m_atan2",m_atan2), NULL, NULL, "fx16_atan2",fx16_atan2, 16);
    // linearstep/smoothstep are float-or-fixed only (no raw-int slot): an
    // all-int call promotes to f64, exactly like sqrt(4). clamp is the
    // exception -- it only compares and selects, so an all-int clamp stays
    // integer via m_iclamp (same reasoning as min/max above). sin01/cos01 take
    // their input in turns (0..1 == one cycle) -- see math_pure.h/math_fixedp.h.
    register_c_func_3arg(vm, TOK_CLAMP,      "clamp",      REG_C_FUNC_F32_3ARG("m_clampf",m_clampf),           REG_C_FUNC_F64_3ARG("m_clamp",m_clamp),           "m_iclamp", m_iclamp, "fx16_clamp",      fx16_clamp,      16);
    register_c_func_3arg(vm, TOK_LINEARSTEP, "linearstep", REG_C_FUNC_F32_3ARG("m_linearstepf",m_linearstepf), REG_C_FUNC_F64_3ARG("m_linearstep",m_linearstep), NULL, NULL, "fx16_linearstep", fx16_linearstep, 16);
    register_c_func_3arg(vm, TOK_SMOOTHSTEP, "smoothstep", REG_C_FUNC_F32_3ARG("m_smoothstepf",m_smoothstepf), REG_C_FUNC_F64_3ARG("m_smoothstep",m_smoothstep), NULL, NULL, "fx16_smoothstep", fx16_smoothstep, 16);
    register_c_func_1arg(vm, TOK_SIN01,      "sin01",      REG_C_FUNC_F32_1ARG("m_sin01f",m_sin01f),           REG_C_FUNC_F64_1ARG("m_sin01",m_sin01),           NULL, NULL, "fx16_sin01",      fx16_sin01,      16);
    register_c_func_1arg(vm, TOK_COS01,      "cos01",      REG_C_FUNC_F32_1ARG("m_cos01f",m_cos01f),           REG_C_FUNC_F64_1ARG("m_cos01",m_cos01),           NULL, NULL, "fx16_cos01",      fx16_cos01,      16);
    register_c_func_3arg(vm, TOK_LINEARSTEPA, "linearstepa", REG_C_FUNC_F32_3ARG("m_linearstepaf",m_linearstepaf), REG_C_FUNC_F64_3ARG("m_linearstepa",m_linearstepa), NULL, NULL, "fx16_linearstepa", fx16_linearstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHSTEPA, "smoothstepa", REG_C_FUNC_F32_3ARG("m_smoothstepaf",m_smoothstepaf), REG_C_FUNC_F64_3ARG("m_smoothstepa",m_smoothstepa), NULL, NULL, "fx16_smoothstepa", fx16_smoothstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHERSTEPA, "smootherstepa", REG_C_FUNC_F32_3ARG("m_smootherstepaf",m_smootherstepaf), REG_C_FUNC_F64_3ARG("m_smootherstepa",m_smootherstepa), NULL, NULL, "fx16_smootherstepa", fx16_smootherstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHERSTEP, "smootherstep", REG_C_FUNC_F32_3ARG("m_smootherstepf",m_smootherstepf), REG_C_FUNC_F64_3ARG("m_smootherstep",m_smootherstep), NULL, NULL, "fx16_smootherstep", fx16_smootherstep, 16);
    // mix has no i32 and no fx slot on purpose: an all-int call promotes to
    // f64 (like sqrt(4)), and a fixed-point call never gets here at all --
    // compile_mix_fixed (vm.c) expands it inline at the call site's own shift,
    // which keeps full precision at any fxN and pulls in no fx library code.
    register_c_func_3arg(vm, TOK_MIX,        "mix",        REG_C_FUNC_F32_3ARG("m_mixf",m_mixf),               REG_C_FUNC_F64_3ARG("m_mix",m_mix),               NULL, NULL, NULL, NULL, 0);
    mark_builtin_math_pure(run);
#endif

    // Cast functions: handled at compile-time as IR_CVT; no C dispatch needed.
    // Registered here so resolve_name() finds them.
    {
        Func *fc;
        fc = func_alloc(run); if (!fc) return;
#ifdef VM_NO_COMPILER
        fc->name = 0;
#else
        fc->name = intern_c_string(&vm->builtin_owner, "i32");
#endif
        fc->native_tok = TOK_I32;
        fc->n_params = 1;
        fc->next = run->funcs;
        run->funcs = fc;
        func_register_id(run, fc);

#if VM_HAS_F32
        fc = func_alloc(run); if (!fc) return;
#ifdef VM_NO_COMPILER
        fc->name = 0;
#else
        fc->name = intern_c_string(&vm->builtin_owner, "f32");
#endif
        fc->native_tok = TOK_F32;
        fc->n_params = 1;
        fc->next = run->funcs;
        run->funcs = fc;
        func_register_id(run, fc);
#endif

#if VM_HAS_F64
        fc = func_alloc(run); if (!fc) return;
#ifdef VM_NO_COMPILER
        fc->name = 0;
#else
        fc->name = intern_c_string(&vm->builtin_owner, "f64");
#endif
        fc->native_tok = TOK_F64;
        fc->n_params = 1;
        fc->next = run->funcs;
        run->funcs = fc;
        func_register_id(run, fc);
#endif

#if VM_HAS_I64
        fc = func_alloc(run); if (!fc) return;
#ifdef VM_NO_COMPILER
        fc->name = 0;
#else
        fc->name = intern_c_string(&vm->builtin_owner, "i64");
#endif
        fc->native_tok = TOK_I64;
        fc->n_params = 1;
        fc->next = run->funcs;
        run->funcs = fc;
        func_register_id(run, fc);
#endif
    }
#if !defined(VM_NO_COMPILER) && !defined(VM_NO_MATH)
    builtin_param_names(vm);
#endif
}


// VmRun-only builtin registration (no VM wrapper needed).
void vm_run_register_builtins(VmRun *run) {
#ifndef VM_NO_MATH
    VM *vm = (VM*)run;
    // See the matching block in vm_register_builtins above for the full
    // rationale (i32_fn/fx_fn split, floor/frac/min/max compiler inlining).
    register_c_func_1arg(vm, TOK_SIN,   "sin",   REG_C_FUNC_F32_1ARG("m_sinf", m_sinf), REG_C_FUNC_F64_1ARG("m_sin", m_sin), NULL, NULL, "fx16_sin",  fx16_sin,  16);
    register_c_func_1arg(vm, TOK_COS,   "cos",   REG_C_FUNC_F32_1ARG("m_cosf", m_cosf), REG_C_FUNC_F64_1ARG("m_cos", m_cos), NULL, NULL, "fx16_cos",  fx16_cos,  16);
    register_c_func_1arg(vm, TOK_EXP,   "exp",   REG_C_FUNC_F32_1ARG("m_expf", m_expf), REG_C_FUNC_F64_1ARG("m_exp", m_exp), NULL, NULL, "fx16_exp",  fx16_exp,  16);
    register_c_func_1arg(vm, TOK_FLOOR, "floor", REG_C_FUNC_F32_1ARG("m_floorf",m_floorf),REG_C_FUNC_F64_1ARG("m_floor",m_floor), NULL, NULL, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_FRAC,  "frac",  REG_C_FUNC_F32_1ARG("m_fracf",m_fracf), REG_C_FUNC_F64_1ARG("m_frac", m_frac), NULL, NULL, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_POW,   "pow",   REG_C_FUNC_F32_2ARG("m_powf", m_powf), REG_C_FUNC_F64_2ARG("m_pow", m_pow), NULL, NULL, "fx16_pow",  fx16_pow,  16);
    register_c_func_2arg(vm, TOK_IPOW,  "ipow",  REG_C_FUNC_F32_2ARG("m_powf", m_powf), REG_C_FUNC_F64_2ARG("m_pow", m_pow), "m_ipow", m_ipow, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_LOG,   "log",   REG_C_FUNC_F32_1ARG("m_logf", m_logf), REG_C_FUNC_F64_1ARG("m_log", m_log), NULL, NULL, "fx16_log",  fx16_log,  16);
    register_c_func_1arg(vm, TOK_ABS,   "abs",   REG_C_FUNC_F32_1ARG("m_fabsf",m_fabsf), REG_C_FUNC_F64_1ARG("m_fabs", m_fabs), "m_iabs", m_iabs, "m_iabs", m_iabs, 16);
    register_c_func_2arg(vm, TOK_MIN,   "min",   REG_C_FUNC_F32_2ARG("m_fminf",m_fminf), REG_C_FUNC_F64_2ARG("m_fmin", m_fmin), "m_imin", m_imin, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_MAX,   "max",   REG_C_FUNC_F32_2ARG("m_fmaxf",m_fmaxf), REG_C_FUNC_F64_2ARG("m_fmax", m_fmax), "m_imax", m_imax, NULL, NULL, 0);
    register_c_func_2arg(vm, TOK_FMOD,  "fmod",  REG_C_FUNC_F32_2ARG("m_fmodf",m_fmodf), REG_C_FUNC_F64_2ARG("m_fmod", m_fmod), NULL, NULL, NULL, NULL, 0);
    register_c_func_1arg(vm, TOK_SQRT,  "sqrt",  REG_C_FUNC_F32_1ARG("m_sqrtf",m_sqrtf), REG_C_FUNC_F64_1ARG("m_sqrt", m_sqrt), NULL, NULL, "fx16_sqrt", fx16_sqrt, 16);
    // fx16_atan2 returns Q16.16 radians in (-pi, pi], same range/units as
    // the f32/f64 variants above -- callers see consistent atan2()
    // semantics no matter which variant the VM ends up dispatching to.
    register_c_func_2arg(vm, TOK_ATAN2, "atan2", REG_C_FUNC_F32_2ARG("m_atan2f",m_atan2f),REG_C_FUNC_F64_2ARG("m_atan2",m_atan2), NULL, NULL, "fx16_atan2",fx16_atan2, 16);
    register_c_func_3arg(vm, TOK_CLAMP,      "clamp",      REG_C_FUNC_F32_3ARG("m_clampf",m_clampf),           REG_C_FUNC_F64_3ARG("m_clamp",m_clamp),           "m_iclamp", m_iclamp, "fx16_clamp",      fx16_clamp,      16);
    register_c_func_3arg(vm, TOK_LINEARSTEP, "linearstep", REG_C_FUNC_F32_3ARG("m_linearstepf",m_linearstepf), REG_C_FUNC_F64_3ARG("m_linearstep",m_linearstep), NULL, NULL, "fx16_linearstep", fx16_linearstep, 16);
    register_c_func_3arg(vm, TOK_SMOOTHSTEP, "smoothstep", REG_C_FUNC_F32_3ARG("m_smoothstepf",m_smoothstepf), REG_C_FUNC_F64_3ARG("m_smoothstep",m_smoothstep), NULL, NULL, "fx16_smoothstep", fx16_smoothstep, 16);
    register_c_func_1arg(vm, TOK_SIN01,      "sin01",      REG_C_FUNC_F32_1ARG("m_sin01f",m_sin01f),           REG_C_FUNC_F64_1ARG("m_sin01",m_sin01),           NULL, NULL, "fx16_sin01",      fx16_sin01,      16);
    register_c_func_1arg(vm, TOK_COS01,      "cos01",      REG_C_FUNC_F32_1ARG("m_cos01f",m_cos01f),           REG_C_FUNC_F64_1ARG("m_cos01",m_cos01),           NULL, NULL, "fx16_cos01",      fx16_cos01,      16);
    register_c_func_3arg(vm, TOK_LINEARSTEPA, "linearstepa", REG_C_FUNC_F32_3ARG("m_linearstepaf",m_linearstepaf), REG_C_FUNC_F64_3ARG("m_linearstepa",m_linearstepa), NULL, NULL, "fx16_linearstepa", fx16_linearstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHSTEPA, "smoothstepa", REG_C_FUNC_F32_3ARG("m_smoothstepaf",m_smoothstepaf), REG_C_FUNC_F64_3ARG("m_smoothstepa",m_smoothstepa), NULL, NULL, "fx16_smoothstepa", fx16_smoothstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHERSTEPA, "smootherstepa", REG_C_FUNC_F32_3ARG("m_smootherstepaf",m_smootherstepaf), REG_C_FUNC_F64_3ARG("m_smootherstepa",m_smootherstepa), NULL, NULL, "fx16_smootherstepa", fx16_smootherstepa, 16);
    register_c_func_3arg(vm, TOK_SMOOTHERSTEP, "smootherstep", REG_C_FUNC_F32_3ARG("m_smootherstepf",m_smootherstepf), REG_C_FUNC_F64_3ARG("m_smootherstep",m_smootherstep), NULL, NULL, "fx16_smootherstep", fx16_smootherstep, 16);
    // mix has no i32 and no fx slot on purpose: an all-int call promotes to
    // f64 (like sqrt(4)), and a fixed-point call never gets here at all --
    // compile_mix_fixed (vm.c) expands it inline at the call site's own shift,
    // which keeps full precision at any fxN and pulls in no fx library code.
    register_c_func_3arg(vm, TOK_MIX,        "mix",        REG_C_FUNC_F32_3ARG("m_mixf",m_mixf),               REG_C_FUNC_F64_3ARG("m_mix",m_mix),               NULL, NULL, NULL, NULL, 0);
    mark_builtin_math_pure(run);
#endif
    // Cast functions
    Func *fc;
    fc = func_alloc(run); if (!fc) return;
    fc->name = 0;
    fc->native_tok = TOK_I32;
    fc->n_params = 1;
    fc->ret_type.kind = VMT_VOID;
    fc->next = run->funcs;
    run->funcs = fc;
    func_register_id(run, fc);

#if VM_HAS_F32
    fc = func_alloc(run); if (!fc) return;
    fc->name = 0;
    fc->native_tok = TOK_F32;
    fc->n_params = 1;
    fc->ret_type.kind = VMT_VOID;
    fc->next = run->funcs;
    run->funcs = fc;
    func_register_id(run, fc);
#endif

#if VM_HAS_F64
    fc = func_alloc(run); if (!fc) return;
    fc->name = 0;
    fc->native_tok = TOK_F64;
    fc->n_params = 1;
    fc->ret_type.kind = VMT_VOID;
    fc->next = run->funcs;
    run->funcs = fc;
    func_register_id(run, fc);
#endif

#if VM_HAS_I64
    fc = func_alloc(run); if (!fc) return;
    fc->name = 0;
    fc->native_tok = TOK_I64;
    fc->n_params = 1;
    fc->ret_type.kind = VMT_VOID;
    fc->next = run->funcs;
    run->funcs = fc;
    func_register_id(run, fc);
#endif
}

// ---------- VM lifecycle ----------

// ---------- VM lifecycle ----------

void vm_run_init(VmRun *run, Tsys *sys, const VmRunConfig *cfg) {
    sys->memset(run, 0, sizeof(*run));
    run->sys = sys;
    // Select backend based on cfg->mem_buf presence (runtime, not compile-time).
    if (cfg && cfg->mem_buf && cfg->mem_buf_cap > 0) {
        // Fixed bump buffer (no-alloc mode).  Zero the buffer so stale
        // pointers from a previous app instance (a device may not clear .bss)
        // become NULL and can be detected.
        sys->memset(cfg->mem_buf, 0, cfg->mem_buf_cap);
        run->mem.buf   = cfg->mem_buf;
        run->mem.cap   = cfg->mem_buf_cap;
        run->mem.used  = 0;
        run->mem.alloc = mem_bump_alloc;
        run->mem.arena = NULL;     // signals "fixed mode"
    } else {
        // Dynamic: use arena.
        arena_init(&run->arena, sys);
        run->mem.arena = &run->arena;
        run->mem.alloc = mem_arena_alloc;
        run->mem.buf   = NULL;
        run->mem.used  = 0;
        run->mem.cap   = 0;
    }
    // Func ID table.
    if (cfg && cfg->funcs_by_id_buf) {
        run->funcs_by_id   = cfg->funcs_by_id_buf;
        run->cap_func_ids  = cfg->funcs_by_id_buf_cap;
    }
#ifndef VM_NO_MATH
    // CFunc table.
    int default_cap = TOK_MATHS_LAST - TOK_MATHS_FIRST;
    if (cfg && cfg->cfunc_table_buf) {
        run->cfunc_table     = cfg->cfunc_table_buf;
        run->cfunc_table_cap = cfg->cfunc_table_buf_cap;
    } else {
        run->cfunc_table = (CFuncEntry*)S_MALLOC(sys, sizeof(CFuncEntry) * default_cap);
        run->cfunc_table_cap = default_cap;
    }
    sys->memset(run->cfunc_table, 0, sizeof(CFuncEntry) * run->cfunc_table_cap);
#endif
}

void vm_run_deinit(VmRun *run) {
    if (!run) return;
    if (!run->mem.buf) {
        arena_free_all(&run->arena);
    }
#ifndef VM_NO_MATH
    // Free cfunc_table if heap-allocated.
    if (!run->mem.buf && run->cfunc_table) {
        S_FREE(run->sys, run->cfunc_table);
    }
#endif
}

#ifndef VM_NO_COMPILER
void vm_init(VM *vm, Tsys *sys, struct Parser *parser) {
    vm_run_init(&vm->run, sys, NULL);
    vm->parser = parser;
    vm->intern = &parser->intern;
    vm->num_default_depth = 0;
    vm->num_default_stack[0] = VMT_I32;
    for (int i = 0; i < 8; i++) { vm->type_map[i] = (VTKind)i; vm->literal_map[i] = (VTKind)i; vm->type_shift[i] = 0; vm->literal_shift[i] = 0; }
    vm->flags = VM_FLAG_CHECK_DIV_ZERO | VM_FLAG_INLINE_POWERS | VM_FLAG_IDENTITY_ELIM | VM_FLAG_AUTO_PACK | VM_FLAG_AUTO_VEC | VM_FLAG_FX_I64_WIDE | VM_FLAG_INT_WRAP | VM_FLAG_CONST_FOLD | VM_FLAG_CONST_PRECISE | VM_FLAG_AUTO_CONST | VM_FLAG_SWIZZLE | VM_FLAG_UNROLL;
    intern_owner_init(&vm->builtin_owner, vm->intern);
    intern_owner_init(&vm->globals_owner, vm->intern);
    vm_register_builtins(vm);
}
#endif

static VM *vm_create_common(Tsys *sys) {
    VM *vm = (VM*)S_MALLOC(sys, sizeof(VM));
    sys->memset(vm, 0, sizeof(VM));
    vm->run.sys = sys;
    arena_init(&vm->run.arena, sys);
    vm->run.mem.arena = &vm->run.arena;
    vm->run.mem.alloc = mem_arena_alloc;
#ifndef VM_NO_MATH
    int default_cap = TOK_MATHS_LAST - TOK_MATHS_FIRST;
    vm->run.cfunc_table = (CFuncEntry*)S_MALLOC(sys, sizeof(CFuncEntry) * default_cap);
    sys->memset(vm->run.cfunc_table, 0, sizeof(CFuncEntry) * default_cap);
    vm->run.cfunc_table_cap = default_cap;
#endif
    return vm;
}

#ifdef VM_NO_COMPILER
VM *vm_create(Tsys *sys) {
    VM *vm = vm_create_common(sys);
    vm_register_builtins(vm);
    return vm;
}
#else
VM *vm_create(Tsys *sys, struct Parser *parser) {
    VM *vm = vm_create_common(sys);
    vm->parser = parser;
    vm->intern = &parser->intern;
    vm->num_default_depth = 0;
    vm->num_default_stack[0] = VMT_I32;
    for (int i = 0; i < 8; i++) { vm->type_map[i] = (VTKind)i; vm->literal_map[i] = (VTKind)i; vm->type_shift[i] = 0; vm->literal_shift[i] = 0; }
    vm->flags = VM_FLAG_CHECK_DIV_ZERO | VM_FLAG_INLINE_POWERS | VM_FLAG_IDENTITY_ELIM | VM_FLAG_AUTO_PACK | VM_FLAG_AUTO_VEC | VM_FLAG_FX_I64_WIDE | VM_FLAG_INT_WRAP | VM_FLAG_CONST_FOLD | VM_FLAG_CONST_PRECISE | VM_FLAG_AUTO_CONST | VM_FLAG_SWIZZLE | VM_FLAG_UNROLL;
    intern_owner_init(&vm->builtin_owner, vm->intern);
    intern_owner_init(&vm->globals_owner, vm->intern);
    vm_register_builtins(vm);
    // Built-ins written in the language (vm/vm_lib.h). Costs nothing until a
    // unit names one: the source is not parsed, let alone compiled, before that.
    vm_register_lib_defaults(vm);
    return vm;
}
#endif

VM *vm_create_minimal(Tsys *sys) {
    VM *vm = vm_create_common(sys);
#ifndef VM_NO_COMPILER
    vm->parser = NULL;
    vm->intern = (InternCtx*)S_MALLOC(sys, sizeof(InternCtx));
    if (vm->intern) {
        intern_ctx_init(vm->intern, sys);
        intern_init_fixed_tokens(vm->intern);
    }
    vm->num_default_depth = 0;
    vm->num_default_stack[0] = VMT_I32;
    for (int i = 0; i < 8; i++) { vm->type_map[i] = (VTKind)i; vm->literal_map[i] = (VTKind)i; vm->type_shift[i] = 0; vm->literal_shift[i] = 0; }
    vm->flags = VM_FLAG_CHECK_DIV_ZERO | VM_FLAG_INLINE_POWERS | VM_FLAG_IDENTITY_ELIM | VM_FLAG_AUTO_PACK | VM_FLAG_AUTO_VEC | VM_FLAG_FX_I64_WIDE | VM_FLAG_INT_WRAP | VM_FLAG_CONST_FOLD | VM_FLAG_CONST_PRECISE | VM_FLAG_AUTO_CONST | VM_FLAG_SWIZZLE | VM_FLAG_UNROLL;
    if (vm->intern) {
        intern_owner_init(&vm->builtin_owner, vm->intern);
        intern_owner_init(&vm->globals_owner, vm->intern);
    }
#endif
    vm_register_builtins(vm);
    return vm;
}

void vm_destroy(VM *vm) {
    if (!vm) return;
#ifndef VM_NO_COMPILER
    // Each library snippet this VM parsed owns a parse arena of its own.
    vm_lib_free(vm);
    intern_owner_deinit(&vm->builtin_owner);
    intern_owner_deinit(&vm->globals_owner);
    if (!vm->parser && vm->intern) {
        intern_ctx_deinit(vm->intern);
        S_FREE(vm->run.sys, vm->intern);
    }
#endif
    vm_run_deinit(&vm->run);
    S_FREE(vm->run.sys, vm);
}

// ---------- Serialization ----------

// Binary blob format (little-endian):
//   Header: magic(4) version(4) frame_size(4) ret_offset(4) ret_type(8)
//           n_params(4) param_slot[16](64) n_syms(4) n_nodes(4)
//           n_child_indices(4) n_specs(4) func_id(4) native_tok(4) is_template(4)
//           body(4) syms_offset(4) nodes_offset(4) child_indices_offset(4) specs_offset(4)
//           [version 2 only: const_i_offset(4) const_f_offset(4)]
//   Syms:  n_syms entries of { type_kind(4) type_len(4) offset(4) is_func(4) fn_id(4) }
//          type_len packs the sub-word element width in its high byte:
//          len in bits 0..23, VMType.pack_bits in bits 24..31 (see
//          SYM_LEN_PACK_SHIFT). Unpacked syms write 0 there, so every blob
//          written before packing existed still reads back identically and
//          the record stays 20 bytes -- no format version bump.
//   Nodes: version 1/2 -- n_nodes copies of IRNode (raw struct, 56 bytes each)
//          version 3    -- n_nodes packed 8-byte records (hdr:u32 + payload:i32,
//                           same encoding as the in-memory IRNodeC -- see
//                           func_serialize_compact()/wire_pack_compact_node
//                           below); optional, smaller transport format.
//   Child indices: n_child_indices * 4 bytes
//   Specs: n_specs * 4 bytes (func_ids)
//   Const streams (version 2 only): n_const_i * 8 bytes then n_const_f * 8 bytes
//   (version 3 has no const streams -- each packed node carries its constant
//   inline in its payload word)

// Sub-word packing width rides in the sym record's type_len high byte; see
// the format doc above. 24 bits still allow a 16M-element array, far past
// what a frame can hold.
#define SYM_LEN_PACK_SHIFT 24
#define SYM_LEN_MASK       0x00FFFFFF
// pack_bits is only ever 0/1/2/4/8/16, so it needs bits 24..28 and leaves 29..31
// spare. VMSym.is_global rides in the top one, for the same reason pack_bits rides
// here: it changes how the runtime interprets `offset`, so it MUST survive
// serialization, and borrowing a spare bit keeps the record 20 bytes. Blobs written
// before shared variables existed have the bit clear, which is exactly "not a
// global".
#define SYM_LEN_GLOBAL_BIT 0x80000000u
// VMSym.is_host_buf takes the next one down, for exactly the same reason: it is
// the other answer to "which base pointer does `offset` belong to" (see
// sym_base), so a deserialized func that lost it would index the frame. One
// spare bit (29) is left.
#define SYM_LEN_HOSTBUF_BIT 0x40000000u
#define SYM_LEN_PACK_MASK  0x1F

#define FUNC_BLOB_MAGIC 0x4D464D56  // "VMFM" little-endian
#define FUNC_BLOB_VERSION 2
// Version 3: same header shape as version 1 (no const_i_off/const_f_off -- see
// V3_HEADER_SIZE below), but the node area holds one packed 8-byte record per node
// instead of a raw 56-byte IRNode. A given Func either fits this encoding or it
// doesn't (see func_fits_compact_wire); callers call func_serialize_compact() and
// fall back to func_serialize() when it returns 0.
#define FUNC_BLOB_VERSION_COMPACT 3

// ---- Wire-format compact node encoding ----
// Deliberately independent of this translation unit's own VM_COMPACT_NODES setting:
// the wire format must decode the same way whichever (possibly narrower) local
// IRNodeC bit-widths the receiver's build uses, so it always uses the widest
// split -- 4-bit type, 9-bit a/b -- which contains the narrower device split as a
// subset of representable values.
#define WIRE_IRC_OP_BITS    5
#define WIRE_IRC_TYPE_BITS  4
#define WIRE_IRC_SUBOP_BITS 5
#define WIRE_IRC_A_BITS     9
#define WIRE_IRC_B_BITS     9
#define WIRE_IRC_A_NULL     ((1 << WIRE_IRC_A_BITS) - 1)
#define WIRE_IRC_B_NULL     ((1 << WIRE_IRC_B_BITS) - 1)
#define WIRE_IRC_TYPE_SHIFT  (WIRE_IRC_OP_BITS)
#define WIRE_IRC_SUBOP_SHIFT (WIRE_IRC_TYPE_SHIFT + WIRE_IRC_TYPE_BITS)
#define WIRE_IRC_A_SHIFT     (WIRE_IRC_SUBOP_SHIFT + WIRE_IRC_SUBOP_BITS)
#define WIRE_IRC_B_SHIFT     (WIRE_IRC_A_SHIFT + WIRE_IRC_A_BITS)

static int32_t wire_f32_as_payload(float f) { union { int32_t i; float f; } u; u.f = f; return u.i; }
static float   wire_payload_as_f32(int32_t p) { union { int32_t i; float f; } u; u.i = p; return u.f; }

#ifndef VM_NO_COMPILER
#define V2_HEADER_SIZE 148
// Same layout as V2_HEADER_SIZE minus the const_i_off/const_f_off words --
// version 3 has no const side-streams, since each packed node already
// carries its constant inline in its payload word.
#define V3_HEADER_SIZE 140

// True if every node in f can round-trip through the 8-byte wire encoding:
// child/item indices must fit 9 bits (511 nodes/function, one value
// reserved as the null sentinel) and IR_CONST_I values must fit the
// payload's single 32-bit word (IR_CONST_F already always narrows to f32,
// same lossy tradeoff the runtime IRNodeC representation accepts).
static int func_fits_compact_wire(Func *f) {
    if (f->n_nodes > WIRE_IRC_A_NULL) return 0;
    for (int i = 0; i < f->n_nodes; i++) {
        IRNode *n = &f->nodes[i];
        int a = n->a, b = n->b;
        switch (n->op) {
            case IR_CONST_I:
                if (n->ki < INT32_MIN || n->ki > INT32_MAX) return 0;
                a = -1; b = -1; break;
            case IR_DATA_SLICE:
                // ki is a raw process pointer into VM-arena storage -- neither
                // 32-bit-packable nor meaningful across a serialize boundary, so a
                // function embedding a string literal is not wire-serializable.
                return 0;
            case IR_PRINT:
                // Same answer, for a different reason: print's sink is a host
                // facility that no receiver of a blob has, so refusing the whole
                // function here means those builds need no IR_PRINT case at all.
                return 0;
            case IR_INSPECT:
                // Ditto, and more so: the inspect sink exists only inside the
                // live editor, which never serializes what it is inspecting.
                // VM_HAS_INSPECT already compiles the node out of a blob
                // receiver, so refusing here is what keeps the two consistent.
                return 0;
            case IR_CONST_F:
            case IR_LOCAL:
                a = -1; b = -1; break;
            case IR_CALL:
            case IR_ARR_LIT:
            case IR_BLOCK:
            case IR_COMMA:
            case IR_SLICE:
            case IR_FMT:
                a = n->items_begin; b = n->n_items; break;
            default:
                break;
        }
        if ((a >= 0 && a >= WIRE_IRC_A_NULL) || (b >= 0 && b >= WIRE_IRC_B_NULL)) return 0;
    }
    return 1;
}

// Packs one raw (56B) IRNode into its 8-byte wire form at dst[0..7]
// (little-endian hdr, then little-endian payload). Caller must have already
// confirmed func_fits_compact_wire() so every field here is in range.
static void wire_pack_compact_node(const IRNode *src, unsigned char *dst) {
    int a = src->a, b = src->b;
    int32_t payload = 0;
    switch (src->op) {
        case IR_CONST_I: payload = (int32_t)src->ki; a = -1; b = -1; break;
        case IR_CONST_F: payload = wire_f32_as_payload((float)src->kf); a = -1; b = -1; break;
        case IR_LOCAL:   payload = (int32_t)src->ki; a = -1; b = -1; break;
        case IR_CALL:    payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_INDEX:   payload = (int32_t)src->ki; break;   // sub-word packing width (0 = unpacked)
        case IR_FIELD:   payload = (int32_t)src->ki; break;   // byte offset / record stride
        case IR_LEN:     payload = (int32_t)src->ki; break;   // record stride of a []struct (0 = none)
        case IR_ARR_LIT: payload = 0; a = src->items_begin; b = src->n_items; break;
        case IR_BLOCK:   payload = 0; a = src->items_begin; b = src->n_items; break;
        case IR_COMMA:   payload = 0; a = src->items_begin; b = src->n_items; break;
        // items + a small ki: the payload word carries the ki (element stride
        // / format extra) while a/b carry the item span. VMType.pack_bits is
        // dropped, like everywhere else in the compact encoding -- nothing at
        // run time reads it off these nodes (the stride lives in ki).
        case IR_SLICE:   payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_FMT:     payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_IF:      payload = src->c; break;
        case IR_SELECT:  payload = src->c; break;
        case IR_FOR:     payload = src->c; break;
        default: payload = 0; break;
    }
    uint32_t hdr =  ((uint32_t)src->op & ((1u<<WIRE_IRC_OP_BITS)-1))
                  | (((uint32_t)src->type.kind & ((1u<<WIRE_IRC_TYPE_BITS)-1)) << WIRE_IRC_TYPE_SHIFT)
                  | (((uint32_t)src->sub_op    & ((1u<<WIRE_IRC_SUBOP_BITS)-1)) << WIRE_IRC_SUBOP_SHIFT)
                  | (((uint32_t)(a < 0 ? WIRE_IRC_A_NULL : a) & ((1u<<WIRE_IRC_A_BITS)-1)) << WIRE_IRC_A_SHIFT)
                  | (((uint32_t)(b < 0 ? WIRE_IRC_B_NULL : b) & ((1u<<WIRE_IRC_B_BITS)-1)) << WIRE_IRC_B_SHIFT);
    dst[0] = (unsigned char)hdr;         dst[1] = (unsigned char)(hdr >> 8);
    dst[2] = (unsigned char)(hdr >> 16); dst[3] = (unsigned char)(hdr >> 24);
    dst[4] = (unsigned char)payload;         dst[5] = (unsigned char)(payload >> 8);
    dst[6] = (unsigned char)(payload >> 16); dst[7] = (unsigned char)(payload >> 24);
}

size_t func_serialize_size(Func *f) {
    size_t sz = V2_HEADER_SIZE;
    sz += f->n_syms * 20;
    sz += f->n_nodes * (int)sizeof(IRNode);
    sz += f->n_child_indices * 4;
    sz += f->n_specs * 4;
    int n_const_i = 0, n_const_f = 0;
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op == IR_CONST_I) n_const_i++;
        else if (f->nodes[i].op == IR_CONST_F) n_const_f++;
    }
    sz += n_const_i * 8;
    sz += n_const_f * 8;
    return sz;
}

size_t func_serialize(Func *f, void *buf, size_t buf_size) {
    size_t need = func_serialize_size(f);
    if (!buf) return need;
    if (buf_size < need) return 0;
    unsigned char *p = (unsigned char*)buf;
    unsigned char *start = p;
    Tsys *sys = f->run->sys;

    #define W32(v) do { int _v = (int)(v); sys->memcpy(p, &_v, 4); p += 4; } while(0)
    #define W64(v) do { long long _v = (long long)(v); sys->memcpy(p, &_v, 8); p += 8; } while(0)

    W32(FUNC_BLOB_MAGIC);
    W32(FUNC_BLOB_VERSION);
    W32((int)f->frame_size);
    W32(f->ret_offset);
    W32(f->ret_type.kind);
    // Packing rides in the high byte exactly as it does on a sym: a returned
    // [S]u8 (a struct record) copies S bytes, not S words.
    W32((f->ret_type.len & SYM_LEN_MASK) | (f->ret_type.pack_bits << SYM_LEN_PACK_SHIFT));
    W32(f->n_params);
    for (int i = 0; i < 16; i++) W32(i < f->n_params ? f->param_slot[i] : 0);
    W32(f->n_syms);
    W32(f->n_nodes);
    W32(f->n_child_indices);
    W32(f->n_specs);
    W32(f->func_id);
    W32(f->native_tok);
    W32(f->is_template);
    W32(f->body);

    int n_const_i = 0, n_const_f = 0;
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op == IR_CONST_I) n_const_i++;
        else if (f->nodes[i].op == IR_CONST_F) n_const_f++;
    }

    int syms_off     = V2_HEADER_SIZE;
    int nodes_off    = syms_off + f->n_syms * 20;
    int child_off    = nodes_off + f->n_nodes * (int)sizeof(IRNode);
    int specs_off    = child_off + f->n_child_indices * 4;
    int const_i_off  = specs_off + f->n_specs * 4;
    int const_f_off  = const_i_off + n_const_i * 8;
    W32(syms_off);
    W32(nodes_off);
    W32(child_off);
    W32(specs_off);
    W32(const_i_off);
    W32(const_f_off);

    // Syms (same as v1)
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        W32(s->type.kind);
        W32((s->type.len & SYM_LEN_MASK) | (s->type.pack_bits << SYM_LEN_PACK_SHIFT)
                                     | (s->is_global ? SYM_LEN_GLOBAL_BIT : 0)
                                     | (s->is_host_buf ? SYM_LEN_HOSTBUF_BIT : 0));
        W32(s->offset);
        W32(s->is_func);
        W32(s->is_func && s->fn ? s->fn->func_id : -1);
    }

    // Write raw IRNode structs (compact format disabled for now)
    sys->memcpy(p, f->nodes, f->n_nodes * sizeof(IRNode));
    p += f->n_nodes * sizeof(IRNode);

    // Child indices
    sys->memcpy(p, f->child_indices, f->n_child_indices * 4);
    p += f->n_child_indices * 4;

    // Specs (func_ids)
    for (int i = 0; i < f->n_specs; i++)
        W32(f->specs[i] ? f->specs[i]->func_id : -1);

    // Const I stream
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op == IR_CONST_I) {
            W64(f->nodes[i].ki);
        }
    }

    // Const F stream
    for (int i = 0; i < f->n_nodes; i++) {
        if (f->nodes[i].op == IR_CONST_F) {
            sys->memcpy(p, &f->nodes[i].kf, 8);
            p += 8;
        }
    }

    #undef W32
    #undef W64
    return (size_t)(p - start);
}

// Optional smaller wire format: same blob shape as func_serialize(), except the
// node area is n_nodes * 8 bytes (packed IRNodeC-style records) and there is no
// separate const_i/const_f side-stream, since each node carries its constant inline.
// Shrinks the *transported* blob, independent of VM_COMPACT_NODES's in-memory
// savings on the receiving end. Returns 0 if `f` doesn't fit the encoding.
size_t func_serialize_compact_size(Func *f) {
    if (!func_fits_compact_wire(f)) return 0;
    size_t sz = V3_HEADER_SIZE;
    sz += f->n_syms * 20;
    sz += f->n_nodes * 8;
    sz += f->n_child_indices * 4;
    sz += f->n_specs * 4;
    return sz;
}

size_t func_serialize_compact(Func *f, void *buf, size_t buf_size) {
    size_t need = func_serialize_compact_size(f);
    if (!need) return 0;
    if (!buf) return need;
    if (buf_size < need) return 0;
    unsigned char *p = (unsigned char*)buf;
    unsigned char *start = p;
    Tsys *sys = f->run->sys;

    #define W32(v) do { int _v = (int)(v); sys->memcpy(p, &_v, 4); p += 4; } while(0)

    W32(FUNC_BLOB_MAGIC);
    W32(FUNC_BLOB_VERSION_COMPACT);
    W32((int)f->frame_size);
    W32(f->ret_offset);
    W32(f->ret_type.kind);
    // Packing rides in the high byte exactly as it does on a sym: a returned
    // [S]u8 (a struct record) copies S bytes, not S words.
    W32((f->ret_type.len & SYM_LEN_MASK) | (f->ret_type.pack_bits << SYM_LEN_PACK_SHIFT));
    W32(f->n_params);
    for (int i = 0; i < 16; i++) W32(i < f->n_params ? f->param_slot[i] : 0);
    W32(f->n_syms);
    W32(f->n_nodes);
    W32(f->n_child_indices);
    W32(f->n_specs);
    W32(f->func_id);
    W32(f->native_tok);
    W32(f->is_template);
    W32(f->body);

    int syms_off  = V3_HEADER_SIZE;
    int nodes_off = syms_off + f->n_syms * 20;
    int child_off = nodes_off + f->n_nodes * 8;
    int specs_off = child_off + f->n_child_indices * 4;
    W32(syms_off);
    W32(nodes_off);
    W32(child_off);
    W32(specs_off);

    // Syms (identical shape to func_serialize)
    for (int i = 0; i < f->n_syms; i++) {
        VMSym *s = &f->syms[i];
        W32(s->type.kind);
        W32((s->type.len & SYM_LEN_MASK) | (s->type.pack_bits << SYM_LEN_PACK_SHIFT)
                                     | (s->is_global ? SYM_LEN_GLOBAL_BIT : 0)
                                     | (s->is_host_buf ? SYM_LEN_HOSTBUF_BIT : 0));
        W32(s->offset);
        W32(s->is_func);
        W32(s->is_func && s->fn ? s->fn->func_id : -1);
    }

    // Packed 8-byte nodes.
    for (int i = 0; i < f->n_nodes; i++) {
        wire_pack_compact_node(&f->nodes[i], p);
        p += 8;
    }

    // Child indices (same 4-byte-per-entry shape as func_serialize).
    sys->memcpy(p, f->child_indices, f->n_child_indices * 4);
    p += f->n_child_indices * 4;

    // Specs (func_ids)
    for (int i = 0; i < f->n_specs; i++)
        W32(f->specs[i] ? f->specs[i]->func_id : -1);

    #undef W32
    return (size_t)(p - start);
}
#endif // !VM_NO_COMPILER

static int read_u32(unsigned char **pp) {
    unsigned int v = (unsigned)(*pp)[0] | ((unsigned)(*pp)[1]<<8) | ((unsigned)(*pp)[2]<<16) | ((unsigned)(*pp)[3]<<24);
    *pp += 4;
    return (int)v;
}

#if VM_COMPACT_NODES
// Convert one wire-format (56B) IRNode into the packed 8B IRNodeC used by
// this build's Func.nodes. The wire format itself is unchanged; only the
// in-memory representation differs.
static int irc_convert_node(VmRun *run, const IRNode *src, IRNodeC *dst) {
#if VM_COMPACT_NODES_DEVICE
    // This build can't execute arrays/calls at all (VM_NO_ARRAYS/VM_NO_CALL);
    // a blob containing them was compiled for a different profile and would
    // silently misbehave rather than erroring, so reject it here instead.
    if (src->op == IR_INDEX || src->op == IR_FIELD || src->op == IR_CALL || src->op == IR_CALL_STMT || src->op == IR_ARR_LIT) {
        run->last_error = "blob uses an op unsupported on this device profile (arrays/calls disabled)";
        return 0;
    }
    if (src->type.kind != VMT_VOID && src->type.kind != VMT_I32) {
        run->last_error = "blob uses a type unsupported on this device profile (only i32)";
        return 0;
    }
#endif
    int a = src->a, b = src->b;
    int32_t payload = 0;
    switch (src->op) {
        case IR_CONST_I: payload = (int32_t)src->ki; a = -1; b = -1; break;
        case IR_CONST_F: payload = irc_f32_as_payload((float)src->kf); a = -1; b = -1; break;
        case IR_LOCAL:   payload = (int32_t)src->ki; a = -1; b = -1; break;
        case IR_CALL:    payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_INDEX:   payload = (int32_t)src->ki; break;   // sub-word packing width (0 = unpacked)
        case IR_FIELD:   payload = (int32_t)src->ki; break;   // byte offset / record stride
        case IR_LEN:     payload = (int32_t)src->ki; break;   // record stride of a []struct (0 = none)
        case IR_ARR_LIT: payload = 0; a = src->items_begin; b = src->n_items; break;
        case IR_BLOCK:   payload = 0; a = src->items_begin; b = src->n_items; break;
        case IR_COMMA:   payload = 0; a = src->items_begin; b = src->n_items; break;
        // items + a small ki: the payload word carries the ki (element stride
        // / format extra) while a/b carry the item span. VMType.pack_bits is
        // dropped, like everywhere else in the compact encoding -- nothing at
        // run time reads it off these nodes (the stride lives in ki).
        case IR_SLICE:   payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_FMT:     payload = (int32_t)src->ki; a = src->items_begin; b = src->n_items; break;
        case IR_IF:      payload = src->c; break;
        case IR_SELECT:  payload = src->c; break;
        case IR_FOR:     payload = src->c; break;
        default: payload = 0; break;
    }
    if ((a >= 0 && a >= IRC_A_NULL) || (b >= 0 && b >= IRC_B_NULL)) {
        run->last_error = "function too large for compact node format (index overflow)";
        return 0;
    }
    dst->hdr = irc_pack_hdr(src->op, (int)src->type.kind, src->sub_op, a, b);
    dst->payload = payload;
    return 1;
}
#endif // VM_COMPACT_NODES

Func *func_deserialize(VmRun *run, void *buf, size_t buf_size) {
    if (!buf || buf_size < 100) { run->last_error = "blob too small"; return 0; }
    unsigned char *p = (unsigned char*)buf;
    Tsys *sys = run->sys; (void)sys;

    // Use a local function for reading to avoid macro issues.
    // The compiler may optimize it away entirely.
    #define R32() read_u32(&p)

    int magic = R32();
    if (magic != FUNC_BLOB_MAGIC) { run->last_error = "bad blob magic"; return 0; }
    int version = R32();
    if (version != 1 && version != 2 && version != FUNC_BLOB_VERSION_COMPACT) { run->last_error = "bad blob version"; return 0; }

    Func *f = func_alloc(run);
    if (!f) { run->last_error = "out of memory"; return NULL; }
    f->frame_size = (size_t)R32();
    f->ret_offset = R32();
    f->ret_type.kind = R32();
    {
        int packed_ret = R32();
        f->ret_type.len       = packed_ret & SYM_LEN_MASK;
        f->ret_type.pack_bits = ((unsigned)packed_ret >> SYM_LEN_PACK_SHIFT) & SYM_LEN_PACK_MASK;
    }
    f->n_params = R32();
    for (int i = 0; i < 16; i++) f->param_slot[i] = R32();
    int n_syms = R32(); f->n_syms = n_syms;
    int n_nodes = R32(); f->n_nodes = n_nodes;
    int n_child  = R32(); f->n_child_indices = n_child;
int n_specs = R32(); f->n_specs = n_specs;
    int orig_id = R32();  // original func_id from serialization
    f->native_tok = R32();
f->is_template = R32();
    f->body = R32();
    int syms_off  = R32();
    int nodes_off = R32();
    int child_off = R32();
    int specs_off = R32(); (void)specs_off;
    int const_i_off = 0, const_f_off = 0;
    if (version == 2) {
        const_i_off = R32();
        const_f_off = R32();
    }
    // Unused when VM_COMPACT_NODES (irc_convert_node reads ki/kf straight off
    // the raw wire IRNode instead of these side streams -- see below); avoid
    // -Werror=unused-but-set-variable on that path.
    (void)const_i_off; (void)const_f_off;

    // Allocate owned copies of nodes and child_indices so the blob buffer
    // is pure transient input.  Fixes the device bug: two different functions
    // no longer alias a shared blob_pool and overwrite each other's data.
    f->nodes = NULL;
    f->child_indices = NULL;
    if (version == FUNC_BLOB_VERSION_COMPACT) {
        // Wire nodes are already packed 8-byte records (wire_pack_compact_node),
        // independent of this build's own Func.nodes representation -- unpack
        // each one into whichever local shape applies (IRNodeC directly, or a
        // reconstructed full IRNode -- see the two branches below).
        if (n_nodes > 0) {
#if VM_COMPACT_NODES
            f->nodes = (IRNodeC*)mem_alloc(&run->mem, sizeof(IRNodeC) * n_nodes);
            if (!f->nodes) { run->last_error = "out of memory"; return NULL; }
#else
            f->nodes = (IRNode*)mem_alloc(&run->mem, sizeof(IRNode) * n_nodes);
            if (!f->nodes) { run->last_error = "out of memory"; return NULL; }
            sys->memset(f->nodes, 0, sizeof(IRNode) * n_nodes);
#endif
            unsigned char *wp = (unsigned char*)buf + nodes_off;
            for (int i = 0; i < n_nodes; i++) {
                unsigned char *np = wp + (size_t)i * 8;
                uint32_t hdr = (uint32_t)np[0] | ((uint32_t)np[1]<<8) | ((uint32_t)np[2]<<16) | ((uint32_t)np[3]<<24);
                int32_t payload = (int32_t)((uint32_t)np[4] | ((uint32_t)np[5]<<8) | ((uint32_t)np[6]<<16) | ((uint32_t)np[7]<<24));
                int op     = (int)(hdr & ((1u<<WIRE_IRC_OP_BITS)-1));
                int type   = (int)((hdr >> WIRE_IRC_TYPE_SHIFT)  & ((1u<<WIRE_IRC_TYPE_BITS)-1));
                int sub_op = (int)((hdr >> WIRE_IRC_SUBOP_SHIFT) & ((1u<<WIRE_IRC_SUBOP_BITS)-1));
                int araw   = (int)((hdr >> WIRE_IRC_A_SHIFT) & ((1u<<WIRE_IRC_A_BITS)-1));
                int braw   = (int)((hdr >> WIRE_IRC_B_SHIFT) & ((1u<<WIRE_IRC_B_BITS)-1));
                int a = (araw == WIRE_IRC_A_NULL) ? -1 : araw;
                int b = (braw == WIRE_IRC_B_NULL) ? -1 : braw;
#if VM_COMPACT_NODES
#if VM_COMPACT_NODES_DEVICE
                // Same device-capability guard as irc_convert_node: a v3 blob
                // may have been packed by a general (non-device) writer for
                // ops/types this device profile can't execute.
                if (op == IR_INDEX || op == IR_FIELD || op == IR_CALL || op == IR_CALL_STMT || op == IR_ARR_LIT) {
                    run->last_error = "blob uses an op unsupported on this device profile (arrays/calls disabled)";
                    return NULL;
                }
                if (type != VMT_VOID && type != VMT_I32) {
                    run->last_error = "blob uses a type unsupported on this device profile (only i32)";
                    return NULL;
                }
#endif
                f->nodes[i].hdr     = irc_pack_hdr(op, type, sub_op, a, b);
                f->nodes[i].payload = payload;
#else
                IRNode *dn = &f->nodes[i];
                dn->op = op; dn->sub_op = sub_op;
                dn->type.kind = type; dn->type.len = 0;
                dn->a = a; dn->b = b; dn->c = -1;
                dn->items_begin = -1; dn->n_items = 0;
                dn->ki = 0; dn->kf = 0;
                switch (op) {
                    case IR_CONST_I: dn->ki = payload; break;
                    case IR_CONST_F: dn->kf = wire_payload_as_f32(payload); break;
                    case IR_LOCAL:   dn->ki = payload; break;
                    case IR_CALL:    dn->ki = payload; dn->items_begin = a; dn->n_items = b; dn->a = -1; dn->b = -1; break;
                    case IR_INDEX:   dn->ki = payload; break;   // sub-word packing width
                    case IR_FIELD:   dn->ki = payload; break;
                    case IR_LEN:     dn->ki = payload; break;
                    case IR_ARR_LIT: dn->items_begin = a; dn->n_items = b; dn->a = -1; dn->b = -1; break;
                    case IR_BLOCK:   dn->items_begin = a; dn->n_items = b; dn->a = -1; dn->b = -1; break;
                    case IR_COMMA:   dn->items_begin = a; dn->n_items = b; dn->a = -1; dn->b = -1; break;
                    case IR_SLICE:
                    case IR_FMT:     dn->ki = payload; dn->items_begin = a; dn->n_items = b; dn->a = -1; dn->b = -1; break;
                    case IR_IF:      dn->c = payload; break;
                    case IR_SELECT:  dn->c = payload; break;
                    case IR_FOR:     dn->c = payload; break;
                    default: break;
                }
#endif
            }
        }
    } else {
#if VM_COMPACT_NODES
    // The wire format is unchanged (raw 56B IRNode structs, same as below);
    // only the in-memory Func.nodes representation is compact. Convert each
    // wire node into the packed 8B IRNodeC as it's read.
    if (n_nodes > 0) {
        f->nodes = (IRNodeC*)mem_alloc(&run->mem, sizeof(IRNodeC) * n_nodes);
        if (!f->nodes) { run->last_error = "out of memory"; return NULL; }
        unsigned char *wp = (unsigned char*)buf + nodes_off;
        for (int i = 0; i < n_nodes; i++) {
            IRNode tmp;
            sys->memset(&tmp, 0, sizeof(tmp));
            sys->memcpy(&tmp, wp + (size_t)i * sizeof(IRNode), sizeof(IRNode));
            if (!irc_convert_node(run, &tmp, &f->nodes[i])) return NULL;
        }
    }
#else
    if (n_nodes > 0) {
        f->nodes = (IRNode*)mem_alloc(&run->mem, sizeof(IRNode) * n_nodes);
        if (!f->nodes) { run->last_error = "out of memory"; return NULL; }
        sys->memset(f->nodes, 0, sizeof(IRNode) * n_nodes);
        if (version == 1) {
            // v1: raw IRNode structs
            sys->memcpy(f->nodes, (unsigned char*)buf + nodes_off, sizeof(IRNode) * n_nodes);
        } else {
            // v2: raw IRNode structs (temp: compact format disabled)
            sys->memcpy(f->nodes, (unsigned char*)buf + nodes_off, sizeof(IRNode) * n_nodes);
            // Fill const values from side streams (redundant for full-size but needed for compact)
            if (n_nodes > 0 && const_i_off > 0) {
                unsigned char *cip_raw = (unsigned char*)buf + const_i_off;
                unsigned char *cfp_raw = (unsigned char*)buf + const_f_off;
                for (int i = 0; i < n_nodes; i++) {
                    if (f->nodes[i].op == IR_CONST_I) {
                        sys->memcpy(&f->nodes[i].ki, cip_raw, 8);
                        cip_raw += 8;
                    } else if (f->nodes[i].op == IR_CONST_F) {
                        sys->memcpy(&f->nodes[i].kf, cfp_raw, 8);
                        cfp_raw += 8;
                    }
                }
            }
            // Fill const values from side streams
            if (n_nodes > 0) {
                unsigned char *cip_raw = (unsigned char*)buf + const_i_off;
                unsigned char *cfp_raw = (unsigned char*)buf + const_f_off;
                for (int i = 0; i < n_nodes; i++) {
                    if (f->nodes[i].op == IR_CONST_I) {
                        sys->memcpy(&f->nodes[i].ki, cip_raw, 8);
                        cip_raw += 8;
                    } else if (f->nodes[i].op == IR_CONST_F) {
                        sys->memcpy(&f->nodes[i].kf, cfp_raw, 8);
                        cfp_raw += 8;
                    }
                }
            }
        }
    }
#endif
    }
    if (n_child > 0) {
        f->child_indices = (int*)mem_alloc(&run->mem, sizeof(int) * n_child);
        if (!f->child_indices) { run->last_error = "out of memory"; return NULL; }
        sys->memcpy(f->child_indices, (unsigned char*)buf + child_off, sizeof(int) * n_child);
    } else {
        f->child_indices = NULL;
    }

    // Syms are serialised field-by-field (name is omitted, fn stored as
    // 4-byte func_id), so allocate a proper array and parse each entry.
    if (n_syms > 0) {
        f->syms = (VMSym*)mem_alloc(&run->mem, sizeof(VMSym) * n_syms);
        if (!f->syms) { run->last_error = "out of memory"; return NULL; }
        unsigned char *sp = (unsigned char*)buf + syms_off;
        for (int i = 0; i < n_syms; i++) {
            VMSym *s = &f->syms[i];
            s->name       = 0;  // not stored in blob
            s->hidden     = 0;
            s->name_dup   = 0;
            s->type.kind  = (int)((unsigned)sp[0] | ((unsigned)sp[1]<<8) | ((unsigned)sp[2]<<16) | ((unsigned)sp[3]<<24)); sp += 4;
            int packed_len = (int)((unsigned)sp[0] | ((unsigned)sp[1]<<8) | ((unsigned)sp[2]<<16) | ((unsigned)sp[3]<<24)); sp += 4;
            s->type.len       = packed_len & SYM_LEN_MASK;
            s->type.pack_bits = ((unsigned)packed_len >> SYM_LEN_PACK_SHIFT) & SYM_LEN_PACK_MASK;
            s->type.elem_shift = 0;   // compile-time only; never in the blob
            s->type.inner_len  = 0;   // ditto -- nesting is gone once compiled
            s->type.struct_id  = 0;   // ditto -- no struct table survives either

            // Read before anything dereferences this sym: unlike `shift` and
            // `used` (compile-time only, left alone here), is_global decides
            // which base pointer the interpreter adds `offset` to.
            s->is_global  = ((unsigned)packed_len & SYM_LEN_GLOBAL_BIT) ? 1 : 0;
            s->is_host_buf = ((unsigned)packed_len & SYM_LEN_HOSTBUF_BIT) ? 1 : 0;
            s->offset     = (int)((unsigned)sp[0] | ((unsigned)sp[1]<<8) | ((unsigned)sp[2]<<16) | ((unsigned)sp[3]<<24)); sp += 4;
            s->is_func    = (int)((unsigned)sp[0] | ((unsigned)sp[1]<<8) | ((unsigned)sp[2]<<16) | ((unsigned)sp[3]<<24)); sp += 4;
            int fn_id     = (int)((unsigned)sp[0] | ((unsigned)sp[1]<<8) | ((unsigned)sp[2]<<16) | ((unsigned)sp[3]<<24)); sp += 4;
            s->fn = (fn_id >= 0 && fn_id < run->n_func_ids)
                        ? run->funcs_by_id[fn_id] : 0;
        }
        f->cap_syms = n_syms;
    } else {
        f->syms = 0;
    }
#ifndef VM_NO_COMPILER
    if (n_specs > 0) {
        f->specs = (struct Func**)mem_alloc(&run->mem, sizeof(struct Func*) * n_specs);
        int *spec_ids = (int*)((unsigned char*)buf + specs_off);
        for (int i = 0; i < n_specs; i++)
            f->specs[i] = (spec_ids[i] >= 0 && spec_ids[i] < run->n_func_ids) ? run->funcs_by_id[spec_ids[i]] : 0;
        f->cap_specs = n_specs;
    }
#endif
    f->cap_nodes = n_nodes;
    f->cap_child_indices = n_child;

    // Re-register with a fresh func_id so the function gets added to
    // funcs_by_id at the next sequential position.
    f->func_id = -1;
    func_register_id(run, f);

    // Also place an alias at the original func_id so that IR_CALL ki
    // values (which hold original func_ids from serialization) resolve
    // correctly.
    if (orig_id != f->func_id) {
        if (orig_id >= run->cap_func_ids) {
            if (run->mem.arena == NULL) {
                run->last_error = "orig_id out of range";
                return NULL;
            }
            int nc = run->cap_func_ids;
            while (nc <= orig_id) nc *= 2;
            struct Func **ns = (struct Func**)mem_alloc(&run->mem, sizeof(struct Func*) * nc);
            if (!ns) { run->last_error = "out of memory"; return NULL; }
            run->sys->memset(ns, 0, sizeof(struct Func*) * nc);
            for (int i = 0; i < run->n_func_ids; i++)
                ns[i] = run->funcs_by_id[i];
            run->funcs_by_id = ns;
            run->cap_func_ids = nc;
        }
        run->funcs_by_id[orig_id] = f;
        if (orig_id >= run->n_func_ids)
            run->n_func_ids = orig_id + 1;
    }

    // Link into run->funcs so func_first() finds this function.
    f->next = run->funcs;
    run->funcs = f;

    #undef R32
    return f;
}

Func *func_first(VM *vm) { return vm->run.funcs; }

const char *vm_last_error(VM *vm) { return vm ? vm->run.last_error : 0; }
int vm_last_error_row(VM *vm) { return vm ? vm->run.error_row : -1; }
int vm_last_error_col(VM *vm) { return vm ? vm->run.error_col : -1; }

void vm_set_user_data(VM *vm, void *data) { if (vm) vm->run.user_data = data; }

#if VM_HAS_INSPECT
void vm_set_inspect_sink(VM *vm, VMInspectFn fn, void *user) {
    if (!vm) return;
    vm->run.inspect_fn   = fn;
    vm->run.inspect_user = user;
}

void vm_set_inspect_array_sink(VM *vm, VMInspectArrFn fn, void *user) {
    if (!vm) return;
    vm->run.inspect_arr_fn   = fn;
    vm->run.inspect_arr_user = user;
}
#endif

#ifndef VM_NO_ARRAYS
void vm_set_print_sink(VM *vm, VMPrintFn fn, void *user) {
    if (!vm) return;
    vm->run.print_fn   = fn;
    vm->run.print_user = user;
}

void vm_set_print_emit(VM *vm, VMPrintEmit mode) {
    if (vm) vm->run.print_emit = (int)mode;
}
#endif

void vm_set_globals(VM *vm, void *block, size_t size, unsigned long long layout_hash) {
    if (!vm) return;
    vm->run.globals      = block;
    vm->run.globals_size = size;
    vm->run.globals_hash = layout_hash;
}

void *vm_get_globals(VM *vm) { return vm ? vm->run.globals : 0; }

// ---------- Args API ----------

// Safe memcpy: simple byte loop, always safe on Cortex-M33.
static void vm_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

// Overlap-safe variant: the callee-pool slice copy in IR_CALL moves elements
// down over the region they already live in when alignment shifts them.
#if !defined(VM_NO_ARRAYS) && !defined(VM_NO_CALL)
static void vm_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dst;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s || n == 0) return;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (size_t i = n; i-- > 0; ) d[i] = s[i]; }
}
#endif

// Safe typed access helpers (byte-by-byte, safe on Cortex-M33).
static inline int    read_i32(const unsigned char *p) { return (int)( (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24) ); }
#if VM_HAS_F32
static inline float  read_f32(const unsigned char *p) { union { int i; float f; } u; u.i = read_i32(p); return u.f; }
#endif
#if VM_HAS_I64 || VM_HAS_F64
static inline long long read_i64(const unsigned char *p) {
    unsigned long long lo = (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24);
    unsigned long long hi = (unsigned)p[4] | ((unsigned)p[5]<<8) | ((unsigned)p[6]<<16) | ((unsigned)p[7]<<24);
    return (long long)(lo | (hi << 32));
}
#endif
#if VM_HAS_F64
static inline double read_f64(const unsigned char *p) { union { long long i; double d; } u; u.i = read_i64(p); return u.d; }
#endif
static inline void  write_i32(unsigned char *p, int v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
#if VM_HAS_F32
static inline void  write_f32(unsigned char *p, float v)  { union { int i; float f; } u; u.f = v; write_i32(p, u.i); }
#endif
#if VM_HAS_I64 || VM_HAS_F64
static inline void  write_i64(unsigned char *p, long long v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
    p[4]=(unsigned char)(v>>32); p[5]=(unsigned char)(v>>40); p[6]=(unsigned char)(v>>48); p[7]=(unsigned char)(v>>56);
}
#endif
#if VM_HAS_F64
static inline void  write_f64(unsigned char *p, double v) { union { long long i; double d; } u; u.d = v; write_i64(p, u.i); }
#endif
#ifndef VM_NO_ARRAYS
// Read element `idx` of a `bits`-wide packed array based at `base`.
// Result is always a plain unsigned i32 in 0 .. (1<<bits)-1 -- packed widths
// are storage, never a value type, so nothing downstream sees a `u4`.
static inline int pack_load(const unsigned char *base, int idx, int bits) {
    if (bits == 8) return base[idx];   // exact: see pack_bytes_for's comment
    // Byte-exact too, for the same reason as u8: a u16 array inside a struct
    // record may end on an odd word boundary, and a whole-word read would reach
    // past it. Same value either way -- the layout is little-endian by definition.
    if (bits == 16) return base[2 * idx] | (base[2 * idx + 1] << 8);
    int epw = 32 / bits;
    unsigned w = (unsigned)read_i32(base + (idx / epw) * 4);
    unsigned mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    return (int)((w >> ((idx % epw) * bits)) & mask);
}

static inline void pack_store(unsigned char *base, int idx, int bits, int v) {
    if (bits == 8) { base[idx] = (unsigned char)v; return; }
    if (bits == 16) { base[2 * idx] = (unsigned char)v; base[2 * idx + 1] = (unsigned char)(v >> 8); return; }
    int epw = 32 / bits;
    unsigned char *wp = base + (idx / epw) * 4;
    unsigned mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    int sh = (idx % epw) * bits;
    unsigned w = (unsigned)read_i32(wp);
    w = (w & ~(mask << sh)) | (((unsigned)v & mask) << sh);
    write_i32(wp, (int)w);
}
#endif

static inline void *read_ptr(const unsigned char *p) {
    void *v = NULL;
    for (int i = 0; i < (int)sizeof(void*); i++) ((unsigned char*)&v)[i] = p[i];
    return v;
}
static inline void write_ptr(unsigned char *p, const void *v) {
    for (int i = 0; i < (int)sizeof(void*); i++) p[i] = ((const unsigned char*)&v)[i];
}

#ifndef VM_NO_ARRAYS
// Point host buffer `id` at ptr/len. See vm.h.
//
// Written through the same helpers load_slot READS a slice slot with, rather than
// by assigning the struct's fields: what makes a host buffer work is that a
// VMHostBufSlot is byte-for-byte what load_slot expects, and writing it this way
// keeps that true rather than merely likely. (Which is also why it lives down here,
// below those helpers.)
void vm_bind_host_buffer(VM *vm, int id, void *ptr, int len) {
    if (!vm || id < 0 || id >= VM_MAX_HOST_BUFS) return;
    if (len < 0) len = 0;
    unsigned char *slot = (unsigned char*)&vm->run.host_bufs[id];
    write_ptr(slot, ptr);
    write_i32(slot + sizeof(void*), len);
}
#endif

#ifndef VM_NO_ARRAYS
// The first `max` elements of an unpacked i32/f32/f64 aggregate as f64, each
// divided by 2^shift when the elements are fixed point. Returns how many were
// written, or -1 for an element kind that has no number to report.
//
// i64 elements are the exception: each double carries the element's BIT PATTERN,
// not its value, so no digit is lost past 2^53. Only the inspect sink wants that.
//
// Byte-wise reads (read_i32 & co.), like every other access to frame storage:
// a slice into a host buffer carries no alignment promise.
static int agg_elems_to_f64(VTKind k, const unsigned char *base, int len, int shift,
                            double *out, int max) {
    VTKind elem = arr_elem(k);
    int es = vt_size_of(elem);
    if (es == 0) return -1;
    int n = len < max ? len : max;
    if (n < 0 || (n > 0 && !base)) n = 0;
    double div = shift > 0 ? (double)(1LL << shift) : 1.0;
    for (int i = 0; i < n; i++) {
        const unsigned char *p = base + i * es;
        double v = 0.0;
        if (elem == VMT_I32) v = (double)read_i32(p) / div;
#if VM_HAS_F32
        else if (elem == VMT_F32) v = (double)read_f32(p);
#endif
#if VM_HAS_F64
        else if (elem == VMT_F64) v = read_f64(p);
#endif
#if VM_HAS_I64
        else if (elem == VMT_I64) { union { long long i; double d; } u; u.i = read_i64(p); v = u.d; }
#endif
        out[i] = v;
    }
    return n;
}

int args_get_result_elems(Args *a, double *out, int max, int *total) {
    Func *f = a->func;
    VMType rt = f->ret_type;
    if (total) *total = 0;
    if (rt.pack_bits) return -1;
    const unsigned char *rp = (const unsigned char*)a->frame + f->ret_offset;
    const unsigned char *base;
    int len;
    if (is_array(rt.kind)) {
        base = rp;
        len  = rt.len;
    } else if (is_slice(rt.kind)) {
        base = (const unsigned char*)read_ptr(rp);
        len  = read_i32(rp + sizeof(void*));
    } else if (rt.kind == VMT_I32 || rt.kind == VMT_F32 || rt.kind == VMT_F64) {
        base = rp;
        len  = 1;
        rt.kind = arr_of(rt.kind);
        rt.elem_shift = vmtype_fx_shift(f->ret_type);
    } else {
        return -1;
    }
    if (arr_elem(rt.kind) == VMT_I64) return -1;
    int n = agg_elems_to_f64(rt.kind, base, len, rt.elem_shift, out, max);
    if (n >= 0 && total) *total = len > 0 ? len : 0;
    return n;
}
#endif

// ---------- Args API ----------

size_t func_frame_size(Func *f) { return f->frame_size; }
int    func_param_count(Func *f) { return f->n_params; }
VMType func_param_type(Func *f, int i) { return f->syms[f->param_slot[i]].type; }
int    func_param_used(Func *f, int i) { return f->syms[f->param_slot[i]].used; }
VMType func_return_type(Func *f) { return f->ret_type; }

void args_bind(Args *a, Func *f, void *frame, size_t sz) {
    a->func = f; a->frame = frame; a->frame_size = sz;
    // Zero frame so unused locals are deterministic.
    f->run->sys->memset(frame, 0, sz < f->frame_size ? sz : f->frame_size);
}

static void *param_ptr(Args *a, int i) {
    Func *f = a->func;
    VMSym *s = &f->syms[f->param_slot[i]];
    return (unsigned char*)a->frame + s->offset;
}

void args_set_i32(Args *a, int i, int v)    { write_i32((unsigned char*)param_ptr(a, i), v); }
#if VM_HAS_F32
void args_set_f32(Args *a, int i, float v)  { write_f32((unsigned char*)param_ptr(a, i), v); }
#endif
#if VM_HAS_F64
void args_set_f64(Args *a, int i, double v) { write_f64((unsigned char*)param_ptr(a, i), v); }
#endif
#if VM_HAS_I64
void args_set_i64(Args *a, int i, long long v) { write_i64((unsigned char*)param_ptr(a, i), v); }
#endif
void args_set_arr(Args *a, int i, void *ptr, int len) {
    Func *f = a->func;
    VMSym *s = &f->syms[f->param_slot[i]];
    if (is_array(s->type.kind)) {
        int n = len < s->type.len ? len : s->type.len;
        unsigned char *dst = (unsigned char*)a->frame + s->offset;
        // A packed param expects storage already in the VM's packed layout.
        // For pack_bits == 8 that is just a byte buffer, so a host can bind one
        // directly; narrower widths require the host to pre-pack.
        if (s->type.pack_bits) vm_memcpy(dst, ptr, (size_t)pack_bytes_for(n, s->type.pack_bits));
        else vm_memcpy(dst, ptr, (size_t)(n * vt_size_of(arr_elem(s->type.kind))));
    } else if (is_slice(s->type.kind)) {
        unsigned char *dst = (unsigned char*)a->frame + s->offset;
        write_ptr(dst, ptr);
        write_i32(dst + sizeof(void*), len);
    }
}

int    args_get_i32_result(Args *a) { return read_i32((const unsigned char*)a->frame + a->func->ret_offset); }
#if VM_HAS_F32
float  args_get_f32_result(Args *a) { return read_f32((const unsigned char*)a->frame + a->func->ret_offset); }
#endif
#if VM_HAS_F64
double args_get_f64_result(Args *a) { return read_f64((const unsigned char*)a->frame + a->func->ret_offset); }
#endif
#if VM_HAS_I64
long long args_get_i64_result(Args *a) { return read_i64((const unsigned char*)a->frame + a->func->ret_offset); }
#endif

// ---------- Runtime interpreter ----------

// RV, the value cell, is declared in vm_types.h.

// Internal exec status.
enum { X_OK = 0, X_RET = 1, X_BRK = 2, X_CONT = 3, X_ERR = -1, X_BUDGET = -2, X_CALLEE = -3 };

// Returns X_BUDGET when exhausted; otherwise X_OK. Called once per visited
// IR node (statement or expression) by exec_stmt / eval_expr.
static inline int tick(Func *f) {
    long long *b = &f->run->budget;
    if (*b < 0) return X_OK;
    if (*b == 0) return X_BUDGET;
    (*b)--;
    return X_OK;
}

#ifndef VM_NO_CALL
// Only the outermost entry sets the base, so a nested entry (a native calling
// back in, vm_fold_node mid-run) measures from where the whole run began.
static inline uintptr_t stack_enter(VmRun *run, const void *mark) {
    uintptr_t prev = run->stack_base;
    if (!prev) run->stack_base = (uintptr_t)mark;
    return prev;
}

static inline int stack_too_deep(VmRun *run, const void *mark) {
#if VM_MAX_C_STACK
    uintptr_t here = (uintptr_t)mark, base = run->stack_base;
    return base && (base > here ? base - here : here - base) > (uintptr_t)VM_MAX_C_STACK;
#else
    (void)run; (void)mark;
    return 0;
#endif
}
#endif

// A float to an integer, saturating, nan giving 0 -- Rust's `as`, wasm's
// trunc_sat. C's plain cast is undefined for exactly those inputs.
static int f2i_sat(double v) {
    return v != v ? 0 : v >= 2147483647.0 ? 2147483647 : v <= -2147483648.0 ? (-2147483647 - 1) : (int)v;
}
static long long f2l_sat(double v) {
    return v != v ? 0 : v >= 9223372036854775807.0 ? 9223372036854775807LL
         : v <= -9223372036854775807.0 ? (-9223372036854775807LL - 1) : (long long)v;
}

// `native`: C's cast (VM_FLAG_C_FLOAT_TO_INT), whatever the host does with it.
static RV cvt_to(RV in, VTKind to, int native) {
    RV o; o.k = to;
    if (to == VMT_I32) {
        if (in.k == VMT_I32) o.v.i = (int)in.v.i;
        else if (VM_HAS_I64 && in.k == VMT_I64) o.v.i = (int)in.v.i;
        else o.v.i = native ? (int)in.v.f : f2i_sat(in.v.f);
    }
#if VM_HAS_I64
    else if (to == VMT_I64) {
        if (in.k == VMT_I64) o.v.i = in.v.i;
        else if (in.k == VMT_I32) o.v.i = (long long)(int)in.v.i;
        else o.v.i = native ? (long long)in.v.f : f2l_sat(in.v.f);
    }
#endif
    else {
        double d = (in.k == VMT_I32) ? (double)(int)in.v.i : (VM_HAS_I64 && in.k == VMT_I64) ? (double)in.v.i : in.v.f;
        if (VM_HAS_F32 && to == VMT_F32) o.v.f = (float)d;
        else o.v.f = d;
    }
    return o;
}

static int eval_expr(Func *f, unsigned char *frame, IRNodeT *n, RV *out);

// Forward
static int exec_stmt(Func *f, unsigned char *frame, IRNodeT *n);

static int eval_binop(Func *f, int sub_op, VTKind k, RV a, RV b, RV *out) {
    out->k = k;
    if (k == VMT_I32) {
        int L = (int)a.v.i, R = (int)b.v.i;
        int r;
        switch (sub_op) {
            case OP_ADD: r = L + R; break;
            case OP_SUB: r = L - R; break;
            case OP_MUL: r = L * R; break;
            // R == -1 negates rather than divides: INT_MIN / -1 traps on x86.
            case OP_DIV: r = R ? (R == -1 ? (int)(0u - (unsigned int)L) : L / R) : 0; break;
            case OP_MOD: r = (R && R != -1) ? L % R : 0; break;
            case OP_DIV_NATIVE: r = L / R; break;
            case OP_MOD_NATIVE: r = L % R; break;
            case OP_LT:  r = L <  R; break;
            case OP_LE:  r = L <= R; break;
            case OP_GT:  r = L >  R; break;
            case OP_GE:  r = L >= R; break;
            case OP_EQ:  r = L == R; break;
            case OP_NE:  r = L != R; break;
            case OP_AND: r = (L && R) ? 1 : 0; break;
            case OP_OR:  r = (L || R) ? 1 : 0; break;
            case OP_BAND:r = L & R; break;
            case OP_BOR: r = L | R; break;
            case OP_BXOR:r = L ^ R; break;
            // The count is taken mod the width, which is what x86 did with the raw
            // count anyway -- but defined, so the emitted C can say the same.
            case OP_BSHL:       r = (int)((unsigned int)L << (R & 31)); break;
            case OP_BSHL_NATIVE:r = L << R; break;
            case OP_BSHR:       r = L >> (R & 31); break;
            default: f->run->last_error = "internal error: unknown i32 binop"; return X_ERR;
        }
        out->v.i = r;
        return X_OK;
    }
#if VM_HAS_I64
    if (k == VMT_I64) {
        long long L = a.v.i, R = b.v.i;
        long long r;
        switch (sub_op) {
            case OP_ADD: r = L + R; break;
            case OP_SUB: r = L - R; break;
            case OP_MUL: r = L * R; break;
            case OP_DIV: r = R ? (R == -1 ? (long long)(0ull - (unsigned long long)L) : L / R) : 0; break;
            case OP_MOD: r = (R && R != -1) ? L % R : 0; break;
            case OP_DIV_NATIVE: r = L / R; break;
            case OP_MOD_NATIVE: r = L % R; break;
            case OP_LT:  r = L <  R; break;
            case OP_LE:  r = L <= R; break;
            case OP_GT:  r = L >  R; break;
            case OP_GE:  r = L >= R; break;
            case OP_EQ:  r = L == R; break;
            case OP_NE:  r = L != R; break;
            case OP_AND: r = (L && R) ? 1 : 0; break;
            case OP_OR:  r = (L || R) ? 1 : 0; break;
            case OP_BAND:r = L & R; break;
            case OP_BOR: r = L | R; break;
            case OP_BXOR:r = L ^ R; break;
            case OP_BSHL:       r = (long long)((unsigned long long)L << (R & 63)); break;
            case OP_BSHL_NATIVE:r = L << R; break;
            case OP_BSHR:       r = L >> (R & 63); break;
            default: f->run->last_error = "internal error: unknown i64 binop"; return X_ERR;
        }
        // A comparison is an i32 whatever it compared, as in the float branches:
        // left at I64, a native call argument read it through v.f.
        if (op_is_compare(sub_op)) out->k = VMT_I32;
        out->v.i = r;
        return X_OK;
    }
#endif
#if VM_HAS_F32
    // f32
    if (k == VMT_F32) {
        float L = (float)a.v.f, R = (float)b.v.f;
        if (op_is_compare(sub_op)) {
            long long r = 0;
            switch (sub_op) {
                case OP_LT: r = L <  R; break;
                case OP_LE: r = L <= R; break;
                case OP_GT: r = L >  R; break;
                case OP_GE: r = L >= R; break;
                case OP_EQ: r = L == R; break;
                case OP_NE: r = L != R; break;
            }
            out->k = VMT_I32;
            out->v.i = r;
            return X_OK;
        }
        float r = 0;
        switch (sub_op) {
            case OP_ADD: r = L + R; break;
            case OP_SUB: r = L - R; break;
            case OP_MUL: r = L * R; break;
            case OP_DIV: r = R ? L / R : 0; break;
            case OP_DIV_NATIVE: r = L / R; break;
            default: f->run->last_error = "internal error: unknown f32 binop"; return X_ERR;
        }
        out->v.f = r;
        return X_OK;
    }
#endif
#if VM_HAS_F64
    // f64
    {
    double L = a.v.f, R = b.v.f;
    if (op_is_compare(sub_op)) {
        long long r = 0;
        switch (sub_op) {
            case OP_LT: r = L <  R; break;
            case OP_LE: r = L <= R; break;
            case OP_GT: r = L >  R; break;
            case OP_GE: r = L >= R; break;
            case OP_EQ: r = L == R; break;
            case OP_NE: r = L != R; break;
        }
        out->k = VMT_I32;
        out->v.i = r;
        return X_OK;
    }
    double r = 0;
    switch (sub_op) {
        case OP_ADD: r = L + R; break;
        case OP_SUB: r = L - R; break;
        case OP_MUL: r = L * R; break;
        case OP_DIV: r = R ? L / R : 0; break;
        case OP_DIV_NATIVE: r = L / R; break;
        default: f->run->last_error = "internal error: unknown f64 binop"; return X_ERR;
    }
    out->v.f = r;
    return X_OK;
    }
#endif
    f->run->last_error = "internal error: unknown type in binop";
    return X_ERR;
}

// Which base pointer `s->offset` is relative to. A shared variable lives in the
// host-owned globals block; everything else lives in the caller's frame. This is the
// entire runtime difference between the two -- see VMSym.is_global.
//
// Returns NULL for a shared variable when no block is bound; every caller must treat
// that as a run failure. Reaching it means the host compiled a program using shared
// variables and forgot vm_set_globals.
static unsigned char *sym_base(Func *f, unsigned char *frame, const VMSym *s) {
    if (s->is_global) return (unsigned char*)f->run->globals;
    // A host buffer's table is inline in VmRun, so unlike the globals block it
    // is never absent -- an unbound id simply reads {NULL,0} and fails the
    // ordinary bounds check. See VmRun.host_bufs.
#ifndef VM_NO_ARRAYS
    if (s->is_host_buf) return (unsigned char*)f->run->host_bufs;
#endif
    return frame;
}

// Guard for the sites that CAN report an error. Every dereference of a shared
// variable goes through this first, so "host forgot vm_set_globals" surfaces as
// a run failure with a message instead of a read through NULL + offset.
static int sym_base_ok(Func *f, const VMSym *s) {
    if (!s->is_global || f->run->globals) return 1;
    f->run->last_error = "shared variable used but no globals block is bound";
    return 0;
}

#ifndef VM_NO_COMPILER
static unsigned char *slot_addr(Func *f, unsigned char *frame, int slot) {
    unsigned char *base = sym_base(f, frame, &f->syms[slot]);
    return base ? base + f->syms[slot].offset : 0;
}
#endif

// Callers must have cleared sym_base_ok() for this slot first -- load_slot has
// no error channel of its own.
static RV load_slot(Func *f, unsigned char *frame, int slot) {
    RV o;
    o.v.i = 0;
    o.len = 0;
    VMSym *s = &f->syms[slot];
    o.k = s->type.kind;
    unsigned char *p = sym_base(f, frame, s) + s->offset;
#ifndef VM_NO_ARRAYS
    if (is_array(s->type.kind)) {
        o.v.ptr = p;
        o.len = s->type.len;
    } else if (is_slice(s->type.kind)) {
        o.v.ptr = read_ptr(p);
        o.len = read_i32(p + sizeof(void*));
    } else
#endif
    {
        switch (o.k) {
            case VMT_I32: o.v.i = read_i32(p); break;
#if VM_HAS_I64
            case VMT_I64: o.v.i = read_i64(p); break;
#endif
#if VM_HAS_F32
            case VMT_F32: o.v.f = read_f32(p); break;
#endif
#if VM_HAS_F64
            case VMT_F64: o.v.f = read_f64(p); break;
#endif
            default: break;
        }
    }
    return o;
}

#if VM_HAS_INSPECT
// Its own function so the element buffer is not part of eval_expr's frame, which
// every level of a deep expression pays for.
static void inspect_report_agg(Func *f, const RV *v, int id, int shift) {
    double buf[VM_INSPECT_ARR_MAX];
    int n = agg_elems_to_f64(v->k, (const unsigned char*)v->v.ptr, v->len, shift,
                             buf, VM_INSPECT_ARR_MAX);
    if (n < 0) return;
    f->run->inspect_arr_fn(f->run->inspect_arr_user, id, (int)arr_elem(v->k), shift,
                           v->len > 0 ? v->len : 0, buf, n);
}
#endif

#ifndef VM_NO_CALL
static int eval_native_call(Func *f, unsigned char *frame, IRNodeT *n, RV *out, Func *callee) {
#if !defined(VM_NO_MATH) && !defined(VM_NO_ARRAYS)
    // Explicit mixed-signature native (register_c_func_sig): a run
    // of i32 scalars followed by exactly one trailing i32 slice,
    // which reaches C as a (const int* ptr, int len) pair. Handled
    // before the uniform-scalar path below, which can't carry a
    // pointer argument.
    {
        CFuncEntry *se = &callee->run->cfunc_table[callee->native_tok - TOK_MATHS_FIRST];
        if (se->has_sig) {
            int sc[VM_MAX_CFUNC_ARGS] = {0}; // i32 scalars, in order
            const void *sp = 0; int slen = 0; int ns = 0;
            int slice_pos = -1; // scalar index the slice sits before
            // Slice element width, from register_c_func_arg_bytes:
            // 8 means the native was declared with a
            // (const unsigned char*, int) pair instead of the
            // default (const int*, int). The argument shape is the
            // same, so only the prototype cast below differs.
            int byte_slice = 0;
            for (int i = 0; i < NODE_N_ITEMS(n); i++) {
                RV av2;
                int st2 = eval_expr(f, frame, ir_item(f, n, i), &av2);
                if (st2 != X_OK) return st2;
                if (se->arg_kinds[i] == VMT_SLICE_I32) {
                    sp = av2.v.ptr; slen = av2.len; slice_pos = ns;
                    byte_slice = (se->arg_pack_bits[i] == 8);
                } else {
                    sc[ns++] = (av2.k == VMT_I32) ? (int)av2.v.i : f2i_sat(av2.v.f);
                }
            }
            // Each arm below is spelled once per slice element type,
            // since the prototype the function pointer is cast to
            // differs even though the argument list does not. PT is
            // that element type.
#define SIG_MID_VOID(PT)                                                                    \
    do {                                                                                    \
        const PT *p = (const PT *)sp;                                                       \
        if (ns == 6)                                                                        \
            ((void(*)(int,int,int,int,const PT*,int,int,int))se->sig_fn)(                   \
                sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5]);                         \
        else if (ns == 7)                                                                   \
            ((void(*)(int,int,int,int,const PT*,int,int,int,int))se->sig_fn)(               \
                sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5], sc[6]);                  \
        else                                                                                \
            ((void(*)(int,int,int,int,const PT*,int,int,int,int,int))se->sig_fn)(           \
                sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5], sc[6], sc[7]);           \
    } while (0)

            // The same three shapes again with the VM's user_data
            // as a leading void* (register_c_func_sig_ctx). Spelled
            // out rather than folded into SIG_MID_VOID: a macro
            // argument can't carry the extra `void*,` in the cast
            // and the extra `ctx,` in the call as one token.
#define SIG_MID_VOID_CTX(PT)                                                                \
    do {                                                                                    \
        const PT *p = (const PT *)sp;                                                       \
        void *cx = callee->run->user_data;                                                  \
        if (ns == 6)                                                                        \
            ((void(*)(void*,int,int,int,int,const PT*,int,int,int))se->sig_fn)(             \
                cx, sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5]);                     \
        else if (ns == 7)                                                                   \
            ((void(*)(void*,int,int,int,int,const PT*,int,int,int,int))se->sig_fn)(         \
                cx, sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5], sc[6]);              \
        else                                                                                \
            ((void(*)(void*,int,int,int,int,const PT*,int,int,int,int,int))se->sig_fn)(     \
                cx, sc[0], sc[1], sc[2], sc[3], p, slen, sc[4], sc[5], sc[6], sc[7]);       \
    } while (0)

            // Scalars AFTER the slice (slice_pos < ns): only the
            // shapes actually registered are wired, like the ctx path
            // below. The (ptr,len) pair lands mid-args.
            if (slice_pos >= 0 && slice_pos < ns) {
                if (se->ret_kind != VMT_VOID || slice_pos != 4 || ns < 6 || ns > 8) {
                    f->run->last_error = "unsupported mixed-signature native shape";
                    return X_ERR;
                }
                if (se->wants_ctx) {
                    if (byte_slice) SIG_MID_VOID_CTX(unsigned char);
                    else            SIG_MID_VOID_CTX(int);
                }
                else if (byte_slice) SIG_MID_VOID(unsigned char);
                else                 SIG_MID_VOID(int);
                out->k = VMT_VOID; out->v.i = 0;
                return X_OK;
            }
#undef SIG_MID_VOID
#undef SIG_MID_VOID_CTX

            // Trailing-slice shapes have no ctx flavour: the only ctx
            // mixed-signature shape wired today is mid-slice, handled
            // above. Refuse rather than call through a prototype that
            // is missing the leading void*, which would pass every
            // argument one slot off.
            if (se->wants_ctx) {
                f->run->last_error = "unsupported ctx mixed-signature native shape";
                return X_ERR;
            }

            // Dispatch by leading-scalar count and return kind. The
            // slice is the trailing (ptr,len) pair.
#define SIG_TRAIL_I32(PT)                                                                   \
    do {                                                                                    \
        const PT *p = (const PT *)sp;                                                       \
        switch (ns) {                                                                       \
            case 0: out->v.i = ((int(*)(const PT*,int))se->sig_fn)(p, slen); break;         \
            case 1: out->v.i = ((int(*)(int,const PT*,int))se->sig_fn)(sc[0], p, slen); break; \
            case 2: out->v.i = ((int(*)(int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], p, slen); break; \
            case 3: out->v.i = ((int(*)(int,int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], sc[2], p, slen); break; \
            default: out->v.i = ((int(*)(int,int,int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], sc[2], sc[3], p, slen); break; \
        }                                                                                   \
    } while (0)
#define SIG_TRAIL_VOID(PT)                                                                  \
    do {                                                                                    \
        const PT *p = (const PT *)sp;                                                       \
        switch (ns) {                                                                       \
            case 0: ((void(*)(const PT*,int))se->sig_fn)(p, slen); break;                   \
            case 1: ((void(*)(int,const PT*,int))se->sig_fn)(sc[0], p, slen); break;        \
            case 2: ((void(*)(int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], p, slen); break; \
            case 3: ((void(*)(int,int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], sc[2], p, slen); break; \
            default: ((void(*)(int,int,int,int,const PT*,int))se->sig_fn)(sc[0], sc[1], sc[2], sc[3], p, slen); break; \
        }                                                                                   \
    } while (0)
            if (se->ret_kind == VMT_I32) {
                out->k = VMT_I32;
                if (byte_slice) SIG_TRAIL_I32(unsigned char);
                else            SIG_TRAIL_I32(int);
            } else {
                out->k = VMT_VOID; out->v.i = 0;
                if (byte_slice) SIG_TRAIL_VOID(unsigned char);
                else            SIG_TRAIL_VOID(int);
            }
#undef SIG_TRAIL_I32
#undef SIG_TRAIL_VOID
            return X_OK;
        }
    }
#endif
    // Sized by VM_MAX_CFUNC_ARGS (vm_types.h): compile_call
    // (vm.c) rejects any call whose argument count differs from
    // the registered n_args, and register_c_func_*arg caps that
    // at VM_MAX_CFUNC_ARGS, so NODE_N_ITEMS can't exceed these.
    double da[VM_MAX_CFUNC_ARGS] = {0.0};
    int    ia[VM_MAX_CFUNC_ARGS] = {0};
    int i2;
    for (i2 = 0; i2 < NODE_N_ITEMS(n); i2++) {
        RV av2;
        int st2 = eval_expr(f, frame, ir_item(f, n, i2), &av2);
        if (st2 != X_OK) return st2;
        if (av2.k == VMT_I32)      { ia[i2] = (int)av2.v.i; da[i2] = (double)av2.v.i; }
        else if (av2.k == VMT_I64) { ia[i2] = (int)av2.v.i; da[i2] = (double)av2.v.i; }
        else                       { da[i2] = av2.v.f;       ia[i2] = f2i_sat(av2.v.f);   }
    }
#ifndef VM_NO_MATH
    int idx = callee->native_tok - TOK_MATHS_FIRST;
    CFuncEntry *e = &callee->run->cfunc_table[idx];
    float fa0 = (float)da[0], fa1 = (float)da[1];
    out->k = NODE_TYPE_KIND(n);
    // NODE_SUBOP flags which i32 slot the compiler resolved this
    // call to (1 = fixed-point fx_fn, 0 = raw-int i32_fn) -- see
    // CFuncEntry in vm_types.h. Set at compile time (vm.c) since
    // the compact IR node format doesn't carry a fixed-point
    // shift at runtime for the interpreter to re-derive it from.
    void *ifn = NODE_SUBOP(n) ? e->fx_fn : e->i32_fn;
    // Context natives (register_c_func_*arg_ctx): pass the VM's
    // user_data as a leading void*, then the same arguments the
    // plain path below passes. Mirrors that path arity for arity.
    if (e->wants_ctx) {
        void *ctx = callee->run->user_data;
        if (out->k == VMT_I32 && ifn) {
            if      (e->n_args == 1) out->v.i = ((int(*)(void*,int))                     ifn)(ctx, ia[0]);
            else if (e->n_args == 2) out->v.i = ((int(*)(void*,int,int))                 ifn)(ctx, ia[0], ia[1]);
            else if (e->n_args == 3) out->v.i = ((int(*)(void*,int,int,int))             ifn)(ctx, ia[0], ia[1], ia[2]);
            else if (e->n_args == 4) out->v.i = ((int(*)(void*,int,int,int,int))         ifn)(ctx, ia[0], ia[1], ia[2], ia[3]);
            else if (e->n_args == 5) out->v.i = ((int(*)(void*,int,int,int,int,int))     ifn)(ctx, ia[0], ia[1], ia[2], ia[3], ia[4]);
            else if (e->n_args == 6) out->v.i = ((int(*)(void*,int,int,int,int,int,int)) ifn)(ctx, ia[0], ia[1], ia[2], ia[3], ia[4], ia[5]);
            else                     out->v.i = ((int(*)(void*,int,int,int,int,int,int,int)) ifn)(ctx, ia[0], ia[1], ia[2], ia[3], ia[4], ia[5], ia[6]);
        }
#if VM_HAS_F32
        else if (out->k == VMT_F32 && e->f32_fn) {
            if      (e->n_args == 1) out->v.f = ((float(*)(void*,float))                          e->f32_fn)(ctx, fa0);
            else if (e->n_args == 2) out->v.f = ((float(*)(void*,float,float))                    e->f32_fn)(ctx, fa0, fa1);
            else if (e->n_args == 3) out->v.f = ((float(*)(void*,float,float,float))              e->f32_fn)(ctx, fa0, fa1, (float)da[2]);
            else if (e->n_args == 4) out->v.f = ((float(*)(void*,float,float,float,float))        e->f32_fn)(ctx, fa0, fa1, (float)da[2], (float)da[3]);
            else if (e->n_args == 5) out->v.f = ((float(*)(void*,float,float,float,float,float))  e->f32_fn)(ctx, fa0, fa1, (float)da[2], (float)da[3], (float)da[4]);
            else if (e->n_args == 6) out->v.f = ((float(*)(void*,float,float,float,float,float,float)) e->f32_fn)(ctx, fa0, fa1, (float)da[2], (float)da[3], (float)da[4], (float)da[5]);
            else                     out->v.f = ((float(*)(void*,float,float,float,float,float,float,float)) e->f32_fn)(ctx, fa0, fa1, (float)da[2], (float)da[3], (float)da[4], (float)da[5], (float)da[6]);
        }
#endif
#if VM_HAS_F64
        else if (e->f64_fn) {
            if      (e->n_args == 1) out->v.f = ((double(*)(void*,double))                             e->f64_fn)(ctx, da[0]);
            else if (e->n_args == 2) out->v.f = ((double(*)(void*,double,double))                      e->f64_fn)(ctx, da[0], da[1]);
            else if (e->n_args == 3) out->v.f = ((double(*)(void*,double,double,double))               e->f64_fn)(ctx, da[0], da[1], da[2]);
            else if (e->n_args == 4) out->v.f = ((double(*)(void*,double,double,double,double))        e->f64_fn)(ctx, da[0], da[1], da[2], da[3]);
            else if (e->n_args == 5) out->v.f = ((double(*)(void*,double,double,double,double,double)) e->f64_fn)(ctx, da[0], da[1], da[2], da[3], da[4]);
            else if (e->n_args == 6) out->v.f = ((double(*)(void*,double,double,double,double,double,double)) e->f64_fn)(ctx, da[0], da[1], da[2], da[3], da[4], da[5]);
            else                     out->v.f = ((double(*)(void*,double,double,double,double,double,double,double)) e->f64_fn)(ctx, da[0], da[1], da[2], da[3], da[4], da[5], da[6]);
        }
#endif
        else { f->run->last_error = "no matching native function variant"; return X_ERR; }
        return X_OK;
    }
    if (out->k == VMT_I32 && ifn) {
        if      (e->n_args == 1) out->v.i = ((int(*)(int))                     ifn)(ia[0]);
        else if (e->n_args == 2) out->v.i = ((int(*)(int,int))                 ifn)(ia[0], ia[1]);
        else if (e->n_args == 3) out->v.i = ((int(*)(int,int,int))             ifn)(ia[0], ia[1], ia[2]);
        else if (e->n_args == 4) out->v.i = ((int(*)(int,int,int,int))         ifn)(ia[0], ia[1], ia[2], ia[3]);
        else if (e->n_args == 5) out->v.i = ((int(*)(int,int,int,int,int))     ifn)(ia[0], ia[1], ia[2], ia[3], ia[4]);
        else if (e->n_args == 6) out->v.i = ((int(*)(int,int,int,int,int,int)) ifn)(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5]);
        else                     out->v.i = ((int(*)(int,int,int,int,int,int,int)) ifn)(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5], ia[6]);
    }
#if VM_HAS_F32
    else if (out->k == VMT_F32 && e->f32_fn) {
        if      (e->n_args == 1) out->v.f = ((float(*)(float))                          e->f32_fn)(fa0);
        else if (e->n_args == 2) out->v.f = ((float(*)(float,float))                    e->f32_fn)(fa0, fa1);
        else if (e->n_args == 3) out->v.f = ((float(*)(float,float,float))              e->f32_fn)(fa0, fa1, (float)da[2]);
        else if (e->n_args == 4) out->v.f = ((float(*)(float,float,float,float))        e->f32_fn)(fa0, fa1, (float)da[2], (float)da[3]);
        else if (e->n_args == 5) out->v.f = ((float(*)(float,float,float,float,float))  e->f32_fn)(fa0, fa1, (float)da[2], (float)da[3], (float)da[4]);
        else if (e->n_args == 6) out->v.f = ((float(*)(float,float,float,float,float,float)) e->f32_fn)(fa0, fa1, (float)da[2], (float)da[3], (float)da[4], (float)da[5]);
        else                     out->v.f = ((float(*)(float,float,float,float,float,float,float)) e->f32_fn)(fa0, fa1, (float)da[2], (float)da[3], (float)da[4], (float)da[5], (float)da[6]);
    }
#endif
#if VM_HAS_F64
    else if (e->f64_fn) {
        if      (e->n_args == 0) out->v.f = ((double(*)(void))                               e->f64_fn)();
        else if (e->n_args == 1) out->v.f = ((double(*)(double))                             e->f64_fn)(da[0]);
        else if (e->n_args == 2) out->v.f = ((double(*)(double,double))                      e->f64_fn)(da[0], da[1]);
        else if (e->n_args == 3) out->v.f = ((double(*)(double,double,double))               e->f64_fn)(da[0], da[1], da[2]);
        else if (e->n_args == 4) out->v.f = ((double(*)(double,double,double,double))        e->f64_fn)(da[0], da[1], da[2], da[3]);
        else if (e->n_args == 5) out->v.f = ((double(*)(double,double,double,double,double)) e->f64_fn)(da[0], da[1], da[2], da[3], da[4]);
        else if (e->n_args == 6) out->v.f = ((double(*)(double,double,double,double,double,double)) e->f64_fn)(da[0], da[1], da[2], da[3], da[4], da[5]);
        else                     out->v.f = ((double(*)(double,double,double,double,double,double,double)) e->f64_fn)(da[0], da[1], da[2], da[3], da[4], da[5], da[6]);
    }
#endif
    else {
        f->run->last_error = "no matching native function variant";
        return X_ERR;
    }
#else
    f->run->last_error = "math builtins not supported";
    return X_ERR;
#endif
    return X_OK;
}

static int eval_call(Func *f, unsigned char *frame, IRNodeT *n, RV *out) {
    Func *callee = f->run->funcs_by_id[(int)NODE_KI(n)];

    // Native C-function: dispatch through the VM's cfunc_table.
    if (callee->native_tok != 0) return eval_native_call(f, frame, n, out, callee);

    // Allocate callee frame from VM's bump pool (avoids stack overflow on embedded).
    int frame_sz = (int)((callee->frame_size + 7) & ~(size_t)7);
#ifndef VM_NO_ARRAYS
    // Pre-scan for array literals passed to slice params: we need temp
    // space after the callee frame to hold the elements so the slice
    // has valid storage to point to.
    int temp_off = frame_sz;
    for (int i = 0; i < NODE_N_ITEMS(n); i++) {
        if (NODE_OP(ir_item(f, n, i)) == IR_ARR_LIT && is_slice(callee->syms[callee->param_slot[i]].type.kind)) {
            IRNodeT *lit = ir_item(f, n, i);
            VMType pt = callee->syms[callee->param_slot[i]].type;
            VTKind elem = arr_elem(pt.kind);
            int nlit = NODE_N_ITEMS(lit);
            // A packed parameter's temp holds backing WORDS, not one
            // slot per element, and is word-aligned whatever the
            // element width. Must match the copy pass below exactly.
            int al = pt.pack_bits ? 4 : vt_align_of(elem);
            temp_off = (temp_off + al - 1) & ~(al - 1);
            temp_off += pt.pack_bits ? pack_bytes_for(nlit, pt.pack_bits)
                                     : nlit * vt_size_of(elem);
        }
    }
    frame_sz = (temp_off + 7) & ~(size_t)7;
#endif
    int base_used = f->run->callee_used;
    if (base_used + frame_sz > VM_CALLEE_FRAME_SIZE) { f->run->last_error = "callee frame too large"; return X_CALLEE; }
    if (stack_too_deep(f->run, &frame_sz)) { f->run->last_error = "call stack too deep"; return X_CALLEE; }
    unsigned char *buf = f->run->callee_pool + base_used;
    f->run->sys->memset(buf, 0, (size_t)frame_sz);
    // Reserve the frame BEFORE evaluating the arguments. An argument
    // expression may itself contain a call (`f(g(x), h(y))`), and that
    // nested call allocates at callee_used -- if we bumped only after
    // the fill loop it would hand out this very buffer and scribble
    // over the parameters we already stored.
    f->run->callee_used = base_used + frame_sz;
#ifndef VM_NO_ARRAYS
    // Reset temp offset for the copy pass below.
    temp_off = (int)callee->frame_size;
    temp_off = (temp_off + 7) & ~7;
#endif
    for (int i = 0; i < NODE_N_ITEMS(n); i++) {
#ifndef VM_NO_ARRAYS
        // For array literals passed to array params, evaluate elements directly.
        if (NODE_OP(ir_item(f, n, i)) == IR_ARR_LIT && is_array(callee->syms[callee->param_slot[i]].type.kind)) {
            IRNodeT *lit = ir_item(f, n, i);
            VMSym *ps = &callee->syms[callee->param_slot[i]];
            unsigned char *dst = buf + ps->offset;
            VTKind elem = arr_elem(ps->type.kind);
            int es = vt_size_of(elem);
            int pack = ps->type.pack_bits;
            for (int j = 0; j < NODE_N_ITEMS(lit) && j < ps->type.len; j++) {
                RV ev;
                int st2 = eval_expr(f, frame, ir_item(f, lit, j), &ev);
                if (st2 != X_OK) { f->run->callee_used = base_used; return st2; }
                if (pack) { pack_store(dst, j, pack, (int)ev.v.i); continue; }
                unsigned char *dp = dst + j * es;
                if (elem == VMT_I32) write_i32(dp, (int)ev.v.i);
#if VM_HAS_I64
                else if (elem == VMT_I64) write_i64(dp, ev.v.i);
#endif
#if VM_HAS_F32
                else if (elem == VMT_F32) write_f32(dp, (float)ev.v.f);
#endif
#if VM_HAS_F64
                else write_f64(dp, ev.v.f);
#endif
            }
            continue;
        }
        // For array literals passed to slice params, copy elements
        // into the temp space after the callee frame and set the
        // slice's ptr/len to point there.
        if (NODE_OP(ir_item(f, n, i)) == IR_ARR_LIT && is_slice(callee->syms[callee->param_slot[i]].type.kind)) {
            IRNodeT *lit = ir_item(f, n, i);
            VMSym *ps = &callee->syms[callee->param_slot[i]];
            VTKind elem = arr_elem(ps->type.kind);
            int pack = ps->type.pack_bits;
            int es = vt_size_of(elem);
            int al = pack ? 4 : vt_align_of(elem);
            int nlit = NODE_N_ITEMS(lit);
            temp_off = (temp_off + al - 1) & ~(al - 1);
            unsigned char *elems = buf + temp_off;
            for (int j = 0; j < nlit; j++) {
                RV ev;
                int st2 = eval_expr(f, frame, ir_item(f, lit, j), &ev);
                if (st2 != X_OK) { f->run->callee_used = base_used; return st2; }
                if (pack) { pack_store(elems, j, pack, (int)ev.v.i); continue; }
                unsigned char *dp = elems + j * es;
                if (elem == VMT_I32) write_i32(dp, (int)ev.v.i);
#if VM_HAS_I64
                else if (elem == VMT_I64) write_i64(dp, ev.v.i);
#endif
#if VM_HAS_F32
                else if (elem == VMT_F32) write_f32(dp, (float)ev.v.f);
#endif
#if VM_HAS_F64
                else write_f64(dp, ev.v.f);
#endif
            }
            unsigned char *dst = buf + ps->offset;
            write_ptr(dst, elems);
            write_i32(dst + sizeof(void*), nlit);
            temp_off += pack ? pack_bytes_for(nlit, pack) : nlit * es;
            continue;
        }
#endif
        RV av;
        int st = eval_expr(f, frame, ir_item(f, n, i), &av);
        if (st != X_OK) { f->run->callee_used = base_used; return st; }
        VMSym *ps = &callee->syms[callee->param_slot[i]];
        unsigned char *dst = buf + ps->offset;
#ifndef VM_NO_ARRAYS
        if (is_array(ps->type.kind)) {
            // Pass fixed array by value: memcpy from caller's array.
            int ncpy = av.len < ps->type.len ? av.len : ps->type.len;
            vm_memcpy(dst, av.v.ptr, agg_copy_bytes(ps->type.kind, ps->type.pack_bits, ncpy));
        } else if (is_slice(ps->type.kind)) {
            // A slice param stores a borrowed pointer, so the storage has
            // to outlive the call. If the argument came from a nested call
            // it points into pool space above our frame that the callee's
            // own calls will reuse -- copy those elements up into space we
            // hold for the duration of this call.
            const unsigned char *sp = (const unsigned char*)av.v.ptr;
            if (sp >= f->run->callee_pool + f->run->callee_used &&
                sp <  f->run->callee_pool + VM_CALLEE_FRAME_SIZE) {
                int es = vt_size_of(arr_elem(ps->type.kind));
                int nb = av.len * es;
                int at = (f->run->callee_used + 7) & ~7;
                if (at + nb > VM_CALLEE_FRAME_SIZE) { f->run->callee_used = base_used; f->run->last_error = "callee frame too large"; return X_CALLEE; }
                unsigned char *cp = f->run->callee_pool + at;
                vm_memmove(cp, sp, (size_t)nb);
                f->run->callee_used = at + nb;
                av.v.ptr = cp;
            }
            write_ptr(dst, av.v.ptr);
            write_i32(dst + sizeof(void*), av.len);
        } else
#endif
        if (ps->type.kind == VMT_I32) write_i32(dst, (int)av.v.i);
#if VM_HAS_I64
        else if (ps->type.kind == VMT_I64) write_i64(dst, av.v.i);
#endif
#if VM_HAS_F32
        else if (ps->type.kind == VMT_F32) write_f32(dst, (float)av.v.f);
#endif
#if VM_HAS_F64
        else if (ps->type.kind == VMT_F64) write_f64(dst, av.v.f);
#endif
    }
    // A library spec with a C version runs that instead of its body; the
    // arguments are already in `buf`, laid out exactly as the body reads them.
    int st = (callee->accel && callee->accel(callee, buf))
           ? X_OK : exec_stmt(callee, buf, &callee->nodes[callee->body]);
    f->run->callee_used = base_used;
    // X_BUDGET has to travel too. Without it a callee that ran out of
    // budget looked like a callee that returned, and the caller read
    // whatever was in the return slot -- so an endless loop one call
    // deep produced 0 instead of stopping the run.
    if (st == X_ERR || st == X_CALLEE || st == X_BUDGET) return st;
    // Read return value.
    out->k = callee->ret_type.kind;
    unsigned char *rp = buf + callee->ret_offset;
#ifndef VM_NO_ARRAYS
    if (is_array(callee->ret_type.kind)) {
        // Returned fixed array: caller must copy; we set ptr+len.
        out->v.ptr = rp;
        out->len = callee->ret_type.len;
    } else if (is_slice(callee->ret_type.kind)) {
        // Returned slice: copy ptr+len from callee frame.
        out->v.ptr = read_ptr(rp);
        out->len = read_i32(rp + sizeof(void*));
    } else
#endif
    if (out->k == VMT_I32) out->v.i = read_i32(rp);
#if VM_HAS_I64
    else if (out->k == VMT_I64) out->v.i = read_i64(rp);
#endif
#if VM_HAS_F32
    else if (out->k == VMT_F32) out->v.f = read_f32(rp);
#endif
#if VM_HAS_F64
    else if (out->k == VMT_F64) out->v.f = read_f64(rp);
#endif
    return X_OK;
}
#endif

static int eval_expr(Func *f, unsigned char *frame, IRNodeT *n, RV *out) {
    int ts = tick(f);
    if (ts != X_OK) return ts;
    switch (NODE_OP(n)) {
        case IR_CONST_I:
            out->k = NODE_TYPE_KIND(n); out->v.i = NODE_KI(n);
            if (out->k == VMT_I32) out->v.i = (int)out->v.i;
            return X_OK;
        case IR_CONST_F:
            out->k = NODE_TYPE_KIND(n); out->v.f = NODE_KF(n);
#if VM_HAS_F32
            if (out->k == VMT_F32) out->v.f = (float)out->v.f;
#endif
            return X_OK;
        case IR_LOCAL: {
            if (!sym_base_ok(f, &f->syms[(int)NODE_KI(n)])) return X_ERR;
            *out = load_slot(f, frame, (int)NODE_KI(n));
            return X_OK;
        }
#ifndef VM_NO_ARRAYS
        case IR_INDEX: {
            // Inline array literal index: [e0, e1, ...][idx]
            if (NODE_OP(ir_child(f, NODE_A(n))) == IR_ARR_LIT) {
                RV ie;
                int st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &ie);
                if (st != X_OK) return st;
                int idx = (int)ie.v.i;
                if (idx < 0 || idx >= NODE_N_ITEMS(ir_child(f, NODE_A(n)))) { f->run->last_error = "array index out of bounds"; return X_ERR; }
                return eval_expr(f, frame, ir_item(f, ir_child(f, NODE_A(n)), idx), out);
            }
            // Evaluate the array/slice expression to get base ptr+len.
            RV av;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &av);
            if (st != X_OK) return st;
            RV ie;
            st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &ie);
            if (st != X_OK) return st;
            int idx = (int)ie.v.i;
            VTKind elem = arr_elem(av.k);
            int es = vt_size_of(elem);
            int bound = av.len;
            if (idx < 0 || idx >= bound) { f->run->last_error = "array index out of bounds"; return X_ERR; }
            // Packed element: NODE_KI carries the width (0 = unpacked). av.len
            // is the logical count either way, so the check above already
            // covers this case.
            int pack = (int)NODE_KI(n);
            if (pack) {
                out->k = VMT_I32;
                out->v.i = pack_load((const unsigned char*)av.v.ptr, idx, pack);
                return X_OK;
            }
            unsigned char *p = (unsigned char*)av.v.ptr + idx * es;
            out->k = elem;
            if (elem == VMT_I32) out->v.i = read_i32(p);
#if VM_HAS_I64
            else if (elem == VMT_I64) out->v.i = read_i64(p);
#endif
#if VM_HAS_F32
            else if (elem == VMT_F32) out->v.f = read_f32(p);
#endif
#if VM_HAS_F64
            else out->v.f = read_f64(p);
#endif
            return X_OK;
        }
#endif
        case IR_CVT: {
            RV in;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &in);
            if (st != X_OK) return st;
            // Use the recorded source type, except when it is VOID: that means
            // the source expression's type was unresolved at compile time (e.g.
            // a recursive call whose return type was not yet known). In that
            // case trust the value's actual runtime kind.
            if (NODE_SUBOP(n) != VMT_VOID) in.k = NODE_SUBOP(n);
            *out = cvt_to(in, NODE_TYPE_KIND(n), (f->flags & VM_FLAG_C_FLOAT_TO_INT) != 0);
            return X_OK;
        }
        case IR_BITCAST: {
            // Reinterpret the operand's bits, never its value. Only same-width
            // pairs are ever built (vm.c rejects the rest), so this is a
            // pass-through plus, for the two float pairs, a punning union.
            RV in;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &in);
            if (st != X_OK) return st;
            VTKind src = (VTKind)NODE_SUBOP(n);
            if (src == VMT_VOID) src = in.k;   // same fallback as IR_CVT
            VTKind dst = (VTKind)NODE_TYPE_KIND(n);
            *out = in;
            out->k = dst;
            // i32 <-> fxN: the word is already exactly what the target wants,
            // and the shift is compile-time only, so `*out = in` was the whole
            // conversion.
            if (src == dst) return X_OK;
#if VM_HAS_F32
            if (src == VMT_I32 && dst == VMT_F32) {
                out->v.f = (double)wire_payload_as_f32((int32_t)in.v.i);
                return X_OK;
            }
            if (src == VMT_F32 && dst == VMT_I32) {
                out->v.i = (long long)wire_f32_as_payload((float)in.v.f);
                return X_OK;
            }
#endif
#if VM_HAS_I64 && VM_HAS_F64
            if (src == VMT_I64 && dst == VMT_F64) {
                union { int64_t i; double d; } u; u.i = (int64_t)in.v.i;
                out->v.f = u.d;
                return X_OK;
            }
            if (src == VMT_F64 && dst == VMT_I64) {
                union { int64_t i; double d; } u; u.d = in.v.f;
                out->v.i = (long long)u.i;
                return X_OK;
            }
#endif
            // A pair this build's profile has no storage for (e.g. f32 bitcast
            // in a float-free build). Nothing sensible to reinterpret.
            f->run->last_error = "internal error: unsupported bitcast pair";
            return X_ERR;
        }
        case IR_UNOP: {
            RV in;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &in);
            if (st != X_OK) return st;
            if (NODE_SUBOP(n) == OP_NEG) {
                out->k = in.k;
                if (in.k == VMT_I32) out->v.i = -(int)in.v.i;
#if VM_HAS_I64
                else if (in.k == VMT_I64) out->v.i = -in.v.i;
#endif
#if VM_HAS_F32
                else if (in.k == VMT_F32) out->v.f = -(float)in.v.f;
                else out->v.f = -(double)in.v.f;
#else
                else out->v.f = -in.v.f;
#endif
            } else if (NODE_SUBOP(n) == OP_NOT) {
                out->k = VMT_I32;
                out->v.i = (in.v.i == 0) ? 1 : 0;
            }
            return X_OK;
        }
        case IR_BINOP: {
            RV la, rb;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &la);
            if (st != X_OK) return st;
            // Short-circuit AND/OR
            if (NODE_SUBOP(n) == OP_AND) {
                if (!la.v.i) { out->k = VMT_I32; out->v.i = 0; return X_OK; }
                st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &rb);
                if (st != X_OK) return st;
                out->k = VMT_I32;
                out->v.i = rb.v.i ? 1 : 0;
                return X_OK;
            }
            if (NODE_SUBOP(n) == OP_OR) {
                if (la.v.i) { out->k = VMT_I32; out->v.i = 1; return X_OK; }
                st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &rb);
                if (st != X_OK) return st;
                out->k = VMT_I32;
                out->v.i = rb.v.i ? 1 : 0;
                return X_OK;
            }
            st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &rb);
            if (st != X_OK) return st;
            return eval_binop(f, NODE_SUBOP(n), NODE_TYPE_KIND(n) == VMT_I32 && op_is_compare(NODE_SUBOP(n))
                              ? la.k : NODE_TYPE_KIND(n), la, rb, out);
        }
#ifndef VM_NO_CALL
        case IR_CALL: return eval_call(f, frame, n, out);
#endif
#ifndef VM_NO_ARRAYS
        case IR_ARR_LIT: {
            // Should only be evaluated in IR_ASSIGN context; the assign code
            // handles inline initialization. Not used standalone.
            f->run->last_error = "internal error: array literal in expression context";
            return X_ERR;
        }
        case IR_LEN: {
            // `.len` of a slice -- the length half of the {ptr,len} pair the
            // child expression evaluates to (array/data-slice lengths are
            // compile-time constants and never reach here, see compile_field).
            RV av;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &av);
            if (st != X_OK) return st;
            out->k   = VMT_I32;
            // A slice of struct records counts bytes here; ki is the stride
            // that turns that into records (0 everywhere else).
            out->v.i = NODE_KI(n) > 0 ? av.len / (int)NODE_KI(n) : av.len;
            return X_OK;
        }
        case IR_FIELD: {
            RV bv;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &bv);
            if (st != X_OK) return st;
            int sub = NODE_SUBOP(n);
            int k   = (int)NODE_KI(n);
            if (sub == FIELD_ELEM) {
                RV iv;
                st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &iv);
                if (st != X_OK) return st;
                int idx = (int)iv.v.i;
                if (idx < 0 || (long long)(idx + 1) * k > bv.len) {
                    f->run->last_error = "array index out of bounds";
                    return X_ERR;
                }
                out->k   = NODE_TYPE_KIND(n);
                out->v.ptr = (unsigned char*)bv.v.ptr + (size_t)idx * (size_t)k;
                out->len = k;
                return X_OK;
            }
            if (sub >= FIELD_AGG) {
                RV cv;
                st = eval_expr(f, frame, ir_child(f, NODE_B(n)), &cv);
                if (st != X_OK) return st;
                if (!bv.v.ptr || k > bv.len) { f->run->last_error = "struct field out of bounds"; return X_ERR; }
                out->k   = NODE_TYPE_KIND(n);
                out->v.ptr = (unsigned char*)bv.v.ptr + k;
                out->len = (int)cv.v.i;
                return X_OK;
            }
            const unsigned char *p = (const unsigned char*)bv.v.ptr + k;
            int w = sub == FIELD_U8 ? 1 : sub == FIELD_U16 ? 2 : vt_size_of(NODE_TYPE_KIND(n));
            if (!bv.v.ptr || k + w > bv.len) { f->run->last_error = "struct field out of bounds"; return X_ERR; }
            out->k = NODE_TYPE_KIND(n);
            if (sub == FIELD_U8)       out->v.i = p[0];
            else if (sub == FIELD_U16) out->v.i = p[0] | (p[1] << 8);
            else if (out->k == VMT_I32) out->v.i = read_i32(p);
#if VM_HAS_I64
            else if (out->k == VMT_I64) out->v.i = read_i64(p);
#endif
#if VM_HAS_F32
            else if (out->k == VMT_F32) out->v.f = read_f32(p);
#endif
#if VM_HAS_F64
            else if (out->k == VMT_F64) out->v.f = read_f64(p);
#endif
            return X_OK;
        }
        case IR_DATA_SLICE: {
            // String / constant-data literal: a borrowed {ptr,len} slice into
            // VM-arena storage (see compile_string_literal in vm.c). Unlike
            // IR_ARR_LIT this is a real slice value, safe standalone -- so it
            // flows through the native/user call arg paths that read av.v.ptr/len.
            out->k   = NODE_TYPE_KIND(n);
            out->v.ptr = (void*)(intptr_t)NODE_KI(n);
            out->len = NODE_N_ITEMS(n);
            return X_OK;
        }
        case IR_SLICE: {
            // Subslice: items = [base, start, len]. The stride comes from the
            // node's ki (the base's pack_bits) rather than from the RV, which
            // carries no packing -- same convention as IR_INDEX.
            RV bv, sv, lv;
            int st = eval_expr(f, frame, ir_item(f, n, 0), &bv);
            if (st != X_OK) return st;
            st = eval_expr(f, frame, ir_item(f, n, 1), &sv);
            if (st != X_OK) return st;
            st = eval_expr(f, frame, ir_item(f, n, 2), &lv);
            if (st != X_OK) return st;
            // pack is 0, 8 or 16 here -- vm.c rejects subslicing a u1/u2/u4
            // array, whose elements have no byte address to point at.
            int pack   = (int)NODE_KI(n);
            int stride = pack ? pack / 8 : vt_size_of(arr_elem(bv.k));
            int start  = (int)sv.v.i;
            int len    = (int)lv.v.i;
            // Clamp rather than error: the cursor a string build hands in is
            // already bounded by the buffer, and a clamped slice is a far
            // friendlier failure than a wild pointer.
            if (start < 0) start = 0;
            if (start > bv.len) start = bv.len;
            if (len < 0) len = 0;
            if (len > bv.len - start) len = bv.len - start;
            out->k   = NODE_TYPE_KIND(n);
            out->v.ptr = (unsigned char*)bv.v.ptr + (size_t)start * (size_t)stride;
            out->len = len;
            return X_OK;
        }
        case IR_FMT: {
            // Append one formatted value to a byte buffer; yields the new
            // cursor. items = [dst, cursor, value]; dst carries the capacity.
            RV dv, cv, vv;
            int st = eval_expr(f, frame, ir_item(f, n, 0), &dv);
            if (st != X_OK) return st;
            st = eval_expr(f, frame, ir_item(f, n, 1), &cv);
            if (st != X_OK) return st;
            st = eval_expr(f, frame, ir_item(f, n, 2), &vv);
            if (st != X_OK) return st;
            unsigned char *dst = (unsigned char*)dv.v.ptr;
            int cap = dv.len, at = (int)cv.v.i, extra = (int)NODE_KI(n);
            switch (NODE_SUBOP(n)) {
                case FMT_BYTES:
                    at = vm_fmt_bytes(dst, cap, at, (const unsigned char*)vv.v.ptr, vv.len);
                    break;
                case FMT_I32: at = vm_fmt_i32(dst, cap, at, (int)vv.v.i); break;
#if VM_HAS_I64
                case FMT_I64: at = vm_fmt_i64(dst, cap, at, vv.v.i); break;
#endif
                case FMT_FX:
                    at = vm_fmt_fx(dst, cap, at, (int)vv.v.i, extra & 0xff, (extra >> 8) & 0xff);
                    break;
#if VM_HAS_F64
                case FMT_F64: at = vm_fmt_f64(dst, cap, at, vv.v.f, extra & 0xff); break;
#endif
                default:
                    f->run->last_error = "internal error: unknown IR_FMT kind";
                    return X_ERR;
            }
            out->k   = VMT_I32;
            out->v.i = at;
            return X_OK;
        }
        case IR_PRINT: {
            // The operand is evaluated whether or not anyone is listening, so
            // installing a sink cannot change what a script computes.
            RV v;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &v);
            if (st != X_OK) return st;
            if (f->run->print_fn)
                f->run->print_fn(f->run->print_user,
                                 (const unsigned char*)v.v.ptr, v.len);
            out->k = VMT_VOID;
            return X_OK;
        }
#endif
#if VM_HAS_INSPECT
        case IR_INSPECT: {
            // Evaluate straight into `out`: this node's value IS its operand's,
            // and its type is the operand's type, so there is nothing to
            // convert on the way back out. The report is the only side effect.
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), out);
            if (st != X_OK) return st;
            if (is_array(out->k) || is_slice(out->k)) {
                // An aggregate reports its elements, with the ELEMENT's shift
                // riding on sub_op (compile_inspect_intrinsic).
                if (f->run->inspect_arr_fn)
                    inspect_report_agg(f, out, (int)NODE_KI(n), NODE_SUBOP(n));
                return X_OK;
            }
            if (f->run->inspect_fn) {
                // sub_op is the operand's fixed-point shift, 0 for anything
                // that is not fx (compile_inspect_intrinsic). It goes to the
                // sink alongside the kind because the two together ARE the
                // type: only the host can say "fx16" from them.
                int    shift = NODE_SUBOP(n);
                double dv;
                if (out->k == VMT_I32) {
                    // An fx word is not the number it stands for, so scale it
                    // back -- reporting the raw word would be a lie.
                    dv = (double)out->v.i;
                    if (shift > 0) dv /= (double)(1 << shift);
#if VM_HAS_I64
                } else if (out->k == VMT_I64) {
                    // The bit pattern, not the value: a double cannot hold every i64.
                    union { long long i; double d; } u; u.i = out->v.i; dv = u.d;
#endif
                } else {
                    dv = (double)out->v.f;
                }
                f->run->inspect_fn(f->run->inspect_user, (int)NODE_KI(n),
                                   (int)out->k, shift, dv);
            }
            return X_OK;
        }
#endif
        case IR_SELECT: {
            RV cv;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &cv);
            if (st != X_OK) return st;
            int cond = (cv.k == VMT_I32) ? (cv.v.i != 0) : (cv.v.f != 0);
            if (cond) return eval_expr(f, frame, ir_child(f, NODE_B(n)), out);
            else      return eval_expr(f, frame, ir_child(f, NODE_C(n)), out);
        }
        case IR_COMMA: {
            // Sequence: run items[0..n-2] as statements (side effects), then
            // yield items[n-1] as the value.
            int last = NODE_N_ITEMS(n) - 1;
            for (int i = 0; i < last; i++) {
                int st = exec_stmt(f, frame, ir_item(f, n, i));
                if (st != X_OK) return st;
            }
            return eval_expr(f, frame, ir_item(f, n, last), out);
        }
    }
    f->run->last_error = "internal error: unknown IR op in expression";
    return X_ERR;
}

static int do_store_local(Func *f, unsigned char *frame, int slot, RV val) {
    VMSym *s = &f->syms[slot];
    if (!sym_base_ok(f, s)) return X_ERR;
    unsigned char *p = sym_base(f, frame, s) + s->offset;
#ifndef VM_NO_ARRAYS
    if (is_array(s->type.kind)) {
        int ncpy = val.len < s->type.len ? val.len : s->type.len;
        // Packed source and destination share the same width (the compiler
        // rejects mismatched pack_bits), so this is a straight copy.
        vm_memcpy(p, val.v.ptr, agg_copy_bytes(s->type.kind, s->type.pack_bits, ncpy));

    } else if (is_slice(s->type.kind)) {
        write_ptr(p, val.v.ptr);
        write_i32(p + sizeof(void*), val.len);
    } else
#endif
    if (s->type.kind == VMT_I32) write_i32(p, (int)val.v.i);
#if VM_HAS_I64
    else if (s->type.kind == VMT_I64) write_i64(p, val.v.i);
#endif
#if VM_HAS_F32
    else if (s->type.kind == VMT_F32) write_f32(p, (float)val.v.f);
#endif
#if VM_HAS_F64
    else if (s->type.kind == VMT_F64) write_f64(p, val.v.f);
#endif
    return X_OK;
}

#ifndef VM_NO_ARRAYS
// A vector expression whose operands needed staging arrives as
// IR_COMMA(t = ..., IR_ARR_LIT) rather than as a bare literal (see
// compile_vec_binop). Run those leading items and hand back the literal, so the
// array-literal fast paths below need to know only one shape.
static int arr_lit_unwrap(Func *f, unsigned char *frame, IRNodeT *v, IRNodeT **out) {
    *out = v;
    if (NODE_OP(v) != IR_COMMA || NODE_N_ITEMS(v) == 0) return X_OK;
    IRNodeT *last = ir_item(f, v, NODE_N_ITEMS(v) - 1);
    if (NODE_OP(last) != IR_ARR_LIT) return X_OK;
    for (int i = 0; i < NODE_N_ITEMS(v) - 1; i++) {
        int st = exec_stmt(f, frame, ir_item(f, v, i));
        if (st != X_OK) return st;
    }
    *out = last;
    return X_OK;
}

static void arr_store_elem(unsigned char *base, int i, VTKind elem, int pack, RV ev) {
    if (pack) { pack_store(base, i, pack, (int)ev.v.i); return; }
    unsigned char *p = base + i * vt_size_of(elem);
    if (elem == VMT_I32) write_i32(p, (int)ev.v.i);
#if VM_HAS_I64
    else if (elem == VMT_I64) write_i64(p, ev.v.i);
#endif
#if VM_HAS_F32
    else if (elem == VMT_F32) write_f32(p, (float)ev.v.f);
#endif
#if VM_HAS_F64
    else write_f64(p, ev.v.f);
#endif
}

// Store an array literal's elements into `base`: every element is evaluated
// BEFORE any is stored, so a literal that reads the array it is assigned to
// (`v = [v[1], v[0]]`, `z = vec2(z.y, z.x)`) sees the old values -- as the
// emitted C's compound literal, and every other target, does. The new value is
// built in the callee pool, reserved for the duration so a call inside an
// element cannot reuse it; one too large for the pool is stored as it goes.
static int store_arr_lit(Func *f, unsigned char *frame, IRNodeT *lit, unsigned char *base,
                         VTKind elem, int pack, int len) {
    int n = NODE_N_ITEMS(lit) < len ? NODE_N_ITEMS(lit) : len;
#ifndef VM_NO_CALL
    int nb = pack ? pack_bytes_for(n, pack) : n * vt_size_of(elem);
    int at = (f->run->callee_used + 7) & ~7;
    if (at + nb <= VM_CALLEE_FRAME_SIZE) {
        int saved = f->run->callee_used;
        unsigned char *tmp = f->run->callee_pool + at;
        // Packed words also hold the bits past the last element; keep them.
        if (pack) vm_memcpy(tmp, base, (size_t)nb);
        f->run->callee_used = at + nb;
        for (int i = 0; i < n; i++) {
            RV ev;
            int st = eval_expr(f, frame, ir_item(f, lit, i), &ev);
            if (st != X_OK) { f->run->callee_used = saved; return st; }
            arr_store_elem(tmp, i, elem, pack, ev);
        }
        f->run->callee_used = saved;
        vm_memcpy(base, tmp, (size_t)nb);
        return X_OK;
    }
#endif
    for (int i = 0; i < n; i++) {
        RV ev;
        int st = eval_expr(f, frame, ir_item(f, lit, i), &ev);
        if (st != X_OK) return st;
        arr_store_elem(base, i, elem, pack, ev);
    }
    return X_OK;
}
#endif

static int exec_assign(Func *f, unsigned char *frame, IRNodeT *n) {
    IRNodeT *lv = ir_child(f, NODE_A(n));
    IRNodeT *rv = ir_child(f, NODE_B(n));
    if (NODE_OP(lv) == IR_LOCAL) {
#ifndef VM_NO_ARRAYS
        VMSym *s = &f->syms[(int)NODE_KI(lv)];
        if (is_array(s->type.kind)) {
            int stu = arr_lit_unwrap(f, frame, rv, &rv);
            if (stu != X_OK) return stu;
        }
        if (is_array(s->type.kind) && NODE_OP(rv) == IR_ARR_LIT) {
            // Inline init array slot.
            if (!sym_base_ok(f, s)) return X_ERR;
            unsigned char *base = sym_base(f, frame, s) + s->offset;
            return store_arr_lit(f, frame, rv, base, arr_elem(s->type.kind), s->type.pack_bits, s->type.len);
        }
#endif
        RV v;
        int st = eval_expr(f, frame, rv, &v);
        if (st != X_OK) return st;
        return do_store_local(f, frame, (int)NODE_KI(lv), v);
    }
#ifndef VM_NO_ARRAYS
    if (NODE_OP(lv) == IR_INDEX) {
        // Evaluate the array/slice expression to get base ptr+len.
        RV av;
        int st = eval_expr(f, frame, ir_child(f, NODE_A(lv)), &av);
        if (st != X_OK) return st;
        RV iv;
        st = eval_expr(f, frame, ir_child(f, NODE_B(lv)), &iv);
        if (st != X_OK) return st;
        RV rvv;
        st = eval_expr(f, frame, rv, &rvv);
        if (st != X_OK) return st;
        VTKind elem = arr_elem(av.k);
        int es = vt_size_of(elem);
        // Checked like a read: a stray index in a live-edited script must fail the
        // run, not write past the array into the host's memory.
        int idx = (int)iv.v.i;
        if (idx < 0 || idx >= av.len) { f->run->last_error = "array index out of bounds"; return X_ERR; }
        int pack = (int)NODE_KI(lv);
        if (pack) {
            pack_store((unsigned char*)av.v.ptr, idx, pack, (int)rvv.v.i);
            return X_OK;
        }
        unsigned char *p = (unsigned char*)av.v.ptr + idx * es;
        if (elem == VMT_I32) write_i32(p, (int)rvv.v.i);
#if VM_HAS_I64
        else if (elem == VMT_I64) write_i64(p, rvv.v.i);
#endif
#if VM_HAS_F32
        else if (elem == VMT_F32) write_f32(p, (float)rvv.v.f);
#endif
#if VM_HAS_F64
        else write_f64(p, rvv.v.f);
#endif
        return X_OK;
    }
    if (NODE_OP(lv) == IR_FIELD) {
        int sub = NODE_SUBOP(lv);
        if (sub == FIELD_ELEM || sub >= FIELD_AGG) {
            // A whole record or array field: the lvalue node evaluates to the
            // destination's own {ptr, len}, so it is read like any expression.
            RV dv;
            int st = eval_expr(f, frame, lv, &dv);
            if (st != X_OK) return st;
            int pack = sub == FIELD_ELEM ? 8 : field_agg_pack(sub);
            st = arr_lit_unwrap(f, frame, rv, &rv);
            if (st != X_OK) return st;
            if (NODE_OP(rv) == IR_ARR_LIT)
                return store_arr_lit(f, frame, rv, (unsigned char*)dv.v.ptr, arr_elem(NODE_TYPE_KIND(lv)), pack, dv.len);
            RV sv;
            st = eval_expr(f, frame, rv, &sv);
            if (st != X_OK) return st;
            int ncpy = sv.len < dv.len ? sv.len : dv.len;
            vm_memcpy(dv.v.ptr, sv.v.ptr, agg_copy_bytes(NODE_TYPE_KIND(lv), pack, ncpy));
            return X_OK;
        }
        RV bv;
        int st = eval_expr(f, frame, ir_child(f, NODE_A(lv)), &bv);
        if (st != X_OK) return st;
        RV rvv;
        st = eval_expr(f, frame, rv, &rvv);
        if (st != X_OK) return st;
        int k = (int)NODE_KI(lv);
        VTKind kind = NODE_TYPE_KIND(lv);
        int w = sub == FIELD_U8 ? 1 : sub == FIELD_U16 ? 2 : vt_size_of(kind);
        if (!bv.v.ptr || k + w > bv.len) { f->run->last_error = "struct field out of bounds"; return X_ERR; }
        unsigned char *p = (unsigned char*)bv.v.ptr + k;
        if (sub == FIELD_U8)       p[0] = (unsigned char)rvv.v.i;
        else if (sub == FIELD_U16) { p[0] = (unsigned char)rvv.v.i; p[1] = (unsigned char)(rvv.v.i >> 8); }
        else if (kind == VMT_I32)  write_i32(p, (int)rvv.v.i);
#if VM_HAS_I64
        else if (kind == VMT_I64)  write_i64(p, rvv.v.i);
#endif
#if VM_HAS_F32
        else if (kind == VMT_F32)  write_f32(p, (float)rvv.v.f);
#endif
#if VM_HAS_F64
        else if (kind == VMT_F64)  write_f64(p, rvv.v.f);
#endif
        return X_OK;
    }
#endif
    f->run->last_error = "invalid assignment target";
    return X_ERR;
}

// A value nobody reads. An array literal only exists as an assignment's source,
// so a discarded one runs its elements instead: `map(xs, draw)` as a statement.
static int exec_discard(Func *f, unsigned char *frame, IRNodeT *n) {
    int op = NODE_OP(n);
    int seq = op == IR_COMMA && NODE_N_ITEMS(n) > 0
           && (NODE_TYPE_KIND(n) == VMT_VOID || NODE_OP(ir_item(f, n, NODE_N_ITEMS(n) - 1)) == IR_ARR_LIT);
    if (op == IR_ARR_LIT || seq) {
        for (int i = 0; i < NODE_N_ITEMS(n); i++) {
            IRNodeT *it = ir_item(f, n, i);
            int st = NODE_OP(it) == IR_ASSIGN ? exec_stmt(f, frame, it) : exec_discard(f, frame, it);
            if (st != X_OK) return st;
        }
        return X_OK;
    }
    RV tmp;
    return eval_expr(f, frame, n, &tmp);
}

static int exec_stmt(Func *f, unsigned char *frame, IRNodeT *n) {
    if (!n) return X_OK;
    int ts = tick(f);
    if (ts != X_OK) return ts;
    switch (NODE_OP(n)) {
        case IR_BLOCK:
            for (int i = 0; i < NODE_N_ITEMS(n); i++) {
                int st = exec_stmt(f, frame, ir_item(f, n, i));
                if (st != X_OK) return st;
            }
            return X_OK;
        case IR_IF: {
            RV c;
            int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &c);
            if (st != X_OK) return st;
            int cond = (c.k == VMT_I32) ? (c.v.i != 0) : (c.v.f != 0);
            if (cond) return exec_stmt(f, frame, ir_child(f, NODE_B(n)));
            if (NODE_C(n) >= 0) return exec_stmt(f, frame, ir_child(f, NODE_C(n)));
            return X_OK;
        }
        case IR_WHILE: {
            for (;;) {
                RV c;
                int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &c);
                if (st != X_OK) return st;
                int cond = (c.k == VMT_I32) ? (c.v.i != 0) : (c.v.f != 0);
                if (!cond) return X_OK;
                st = exec_stmt(f, frame, ir_child(f, NODE_B(n)));
                if (st == X_BRK) return X_OK;
                if (st == X_CONT) continue;
                if (st == X_RET || st == X_ERR || st == X_CALLEE) return st;
            }
        }
        case IR_FOR: {
            for (;;) {
                RV c;
                int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &c);
                if (st != X_OK) return st;
                int cond = (c.k == VMT_I32) ? (c.v.i != 0) : (c.v.f != 0);
                if (!cond) return X_OK;
                st = exec_stmt(f, frame, ir_child(f, NODE_B(n)));
                if (st == X_BRK) return X_OK;
                if (st != X_OK && st != X_CONT) return st;
                st = exec_stmt(f, frame, ir_child(f, NODE_C(n)));
                if (st != X_OK) return st;
            }
        }
        case IR_BREAK:    return X_BRK;
        case IR_CONTINUE: return X_CONT;
        case IR_RETURN: {
            if (NODE_A(n) >= 0) {
                unsigned char *rp = frame + f->ret_offset;
#ifndef VM_NO_ARRAYS
                // Array literal return: evaluate elements directly into ret slot.
                IRNodeT *rval = ir_child(f, NODE_A(n));
                if (is_array(f->ret_type.kind)) {
                    int stu = arr_lit_unwrap(f, frame, rval, &rval);
                    if (stu != X_OK) return stu;
                }
                if (NODE_OP(rval) == IR_ARR_LIT && is_array(f->ret_type.kind)) {
                    IRNodeT *lit = rval;
                    VTKind elem = arr_elem(f->ret_type.kind);
                    int es = vt_size_of(elem);
                    int pack = f->ret_type.pack_bits;
                    for (int i = 0; i < NODE_N_ITEMS(lit) && i < f->ret_type.len; i++) {
                        RV ev;
                        int st2 = eval_expr(f, frame, ir_item(f, lit, i), &ev);
                        if (st2 != X_OK) return st2;
                        if (pack) { pack_store(rp, i, pack, (int)ev.v.i); continue; }
                        unsigned char *p = rp + i * es;
                        if (elem == VMT_I32) write_i32(p, (int)ev.v.i);
#if VM_HAS_I64
                        else if (elem == VMT_I64) write_i64(p, ev.v.i);
#endif
#if VM_HAS_F32
                        else if (elem == VMT_F32) write_f32(p, (float)ev.v.f);
#endif
#if VM_HAS_F64
                        else write_f64(p, ev.v.f);
#endif
                    }
                    return X_RET;
                }
#endif
                RV v;
                int st = eval_expr(f, frame, ir_child(f, NODE_A(n)), &v);
                if (st != X_OK) return st;
                // Coerce to func's return type, write at ret_offset.
#ifndef VM_NO_ARRAYS
                if (is_array(f->ret_type.kind)) {
                    int ncpy = v.len < f->ret_type.len ? v.len : f->ret_type.len;
                    vm_memcpy(rp, v.v.ptr, agg_copy_bytes(f->ret_type.kind, f->ret_type.pack_bits, ncpy));
                } else if (is_slice(f->ret_type.kind)) {
                    // Runtime safety check: verify slice ptr is NOT inside this
                    // function's frame (which is about to be destroyed).
                    unsigned char *frame_end = frame + f->frame_size;
                    if (v.v.ptr && (unsigned char*)v.v.ptr >= frame && (unsigned char*)v.v.ptr < frame_end) {
                        // Slice points into own frame -> dangling pointer.  Abort.
                        {
#ifdef VM_NO_COMPILER
                        const char *pre = "returned slice pointing into local frame in $";
                        const char *suf = "$";
                        int plen = 0, slen = 0;
                        while (pre[plen]) plen++;
                        while (suf[slen]) slen++;
                        int id = f->name ? f->name : 0;
                        char idbuf[16];
                        int ipos = 0;
                        if (id < 0) { idbuf[ipos++] = '-'; id = -id; }
                        int dstart = ipos;
                        do { idbuf[ipos++] = '0' + (id % 10); id /= 10; } while (id > 0);
                        // reverse digits
                        for (int ri = dstart, rj = ipos - 1; ri < rj; ri++, rj--) { char t = idbuf[ri]; idbuf[ri] = idbuf[rj]; idbuf[rj] = t; }
                        char *ebuf = (char*)mem_alloc(&f->run->mem, plen + ipos + slen + 1);
                        vm_memcpy(ebuf, pre, plen);
                        vm_memcpy(ebuf + plen, idbuf, ipos);
                        vm_memcpy(ebuf + plen + ipos, suf, slen + 1);
                        f->run->last_error = ebuf;
#else
                        const char *fn = f->name ? intern_get_cstr(((VM*)(f->run))->intern, f->name) : "<script>";
                        const char *pre = "returned slice pointing into local frame in ";
                        int plen = 0, flen = 0;
                        while (pre[plen]) plen++;
                        while (fn[flen]) flen++;
                        char *ebuf = (char*)mem_alloc(&f->run->mem, plen + flen + 1);
                        vm_memcpy(ebuf, pre, plen);
                        vm_memcpy(ebuf + plen, fn, flen + 1);
                        f->run->last_error = ebuf;
#endif
                    }
                        return X_ERR;
                    }
                    write_ptr(rp, v.v.ptr);
                    write_i32(rp + sizeof(void*), v.len);
                } else
#endif
                if (is_scalar(f->ret_type.kind)) {
                    RV cv = cvt_to(v, f->ret_type.kind, (f->flags & VM_FLAG_C_FLOAT_TO_INT) != 0);
                    if (f->ret_type.kind == VMT_I32) write_i32(rp, (int)cv.v.i);
#if VM_HAS_I64
                    else if (f->ret_type.kind == VMT_I64) write_i64(rp, cv.v.i);
#endif
#if VM_HAS_F32
                    else if (f->ret_type.kind == VMT_F32) write_f32(rp, (float)cv.v.f);
#endif
#if VM_HAS_F64
                    else write_f64(rp, cv.v.f);
#endif
                }
            }
            return X_RET;
        }
        case IR_ASSIGN:    return exec_assign(f, frame, n);
        case IR_EXPR_STMT: return exec_discard(f, frame, ir_child(f, NODE_A(n)));
#ifndef VM_NO_CALL
        case IR_CALL_STMT: {
            RV tmp;
            return eval_expr(f, frame, ir_child(f, NODE_A(n)), &tmp);
        }
#endif
    }
    f->run->last_error = "internal error: unknown IR op in statement";
    return X_ERR;
}

VMStatus func_run(Func *f, Args *a, long long budget) {
    if (!f || !a || a->func != f) { if (f && f->run) f->run->last_error = "func_run: bad arguments"; return VM_ERR; }
    if (a->frame_size < f->frame_size) { f->run->last_error = "func_run: frame buffer too small"; return VM_ERR; }
    f->run->budget = budget;
#ifndef VM_NO_CALL
    f->run->callee_used = 0;
    uintptr_t saved_base = stack_enter(f->run, &budget);
#endif
    int st = exec_stmt(f, (unsigned char*)a->frame, &f->nodes[f->body]);
#ifndef VM_NO_CALL
    f->run->stack_base = saved_base;
#endif
    if (st == X_RET || st == X_OK) return VM_OK;
    if (st == X_BUDGET) return VM_BUDGET;
    if (st == X_CALLEE) return VM_CALLEE_OVERFLOW;
    if (!f->run->last_error) f->run->last_error = "runtime error";
    return VM_ERR;
}

long long func_budget_left(Func *f) { return f && f->run ? f->run->budget : -1; }

#ifndef VM_NO_COMPILER
// ---------- compile-time constant folding (see VM_FLAG_CONST_FOLD) ----------
//
// The compiler folds a binop over two constants by running it through the
// interpreter's own eval_binop / cvt_to rather than repeating the arithmetic in
// vm.c: a second implementation of i32 wrap, checked division, unsigned shift and
// f32 rounding would be the one that drifts, and a fold that disagrees with the
// interpreter is a miscompile visible only as the preview and the export computing
// different numbers.
//
// The scalars cross as (long long, double) pairs because RV is declared here, after
// vm.c's body. Returns 0 when the operation is not foldable.
int vm_fold_binop(Func *f, int sub_op, VTKind k,
                  long long ai, double af, long long bi, double bf,
                  long long *oi, double *of) {
    RV a, b, o;
    a.k = b.k = k;
    a.len = b.len = 0;
    if (k == VMT_F32 || k == VMT_F64) { a.v.f = af; b.v.f = bf; }
    else                              { a.v.i = ai; b.v.i = bi; }
    // eval_binop answers X_OK / X_ERR, and on X_ERR it writes last_error. A
    // fold that cannot be done is not a compile error -- the caller just emits
    // the op -- so put last_error back the way it was found.
    const char *saved = f->run->last_error;
    if (eval_binop(f, sub_op, k, a, b, &o) != X_OK) {
        f->run->last_error = saved;
        return 0;
    }
    *oi = o.v.i;
    *of = o.v.f;
    return 1;
}

// Evaluate one already-built node whose operands are all constants, by running it
// through the evaluator itself. Used for native math calls: the variant was chosen
// by compile_call before this node existed, so dispatching here picks exactly the
// one the call would have used. `frame` is NULL -- with every operand a constant no
// IR_LOCAL is reached, which the caller guarantees.
int vm_fold_node(Func *f, int node, VTKind *out_kind, long long *oi, double *of) {
    const char *saved_err = f->run->last_error;
    long long   saved_budget = f->run->budget;
    f->run->budget = -1;
#ifndef VM_NO_CALL
    int saved_callee = f->run->callee_used;
    f->run->callee_used = 0;
    uintptr_t saved_base = stack_enter(f->run, &saved_callee);
#endif
    RV o;
    int st = eval_expr(f, 0, &f->nodes[node], &o);
    f->run->budget = saved_budget;
#ifndef VM_NO_CALL
    f->run->callee_used = saved_callee;
    f->run->stack_base = saved_base;
#endif
    if (st != X_OK) {
        f->run->last_error = saved_err;
        return 0;
    }
    *out_kind = o.k;
    *oi = o.v.i;
    *of = o.v.f;
    return 1;
}

// Same, for IR_UNOP. This one mirrors eval_expr's `case IR_UNOP` rather than
// calling it -- that arm is inline in the evaluator and reads its operand from
// a frame, so there is nothing to call. It lives here, beside the code it must
// agree with, and the two must be changed together: OP_NEG in particular is
// NOT `0 - x`, which would turn -0.0 into +0.0.
int vm_fold_unop(int sub_op, VTKind k, long long ii, double fi,
                 VTKind *out_kind, long long *oi, double *of) {
    if (sub_op == OP_NEG) {
        *out_kind = k;
        if (k == VMT_I32)      *oi = -(int)ii;
#if VM_HAS_I64
        else if (k == VMT_I64) *oi = -ii;
#endif
#if VM_HAS_F32
        else if (k == VMT_F32) *of = -(float)fi;
        else                   *of = -(double)fi;
#else
        else                   *of = -fi;
#endif
        return 1;
    }
    if (sub_op == OP_NOT) {
        *out_kind = VMT_I32;
        *oi = (ii == 0) ? 1 : 0;
        return 1;
    }
    return 0;
}

// Same, for IR_CVT. `to` is the destination kind; the source kind decides which
// half of the input pair is read.
int vm_fold_cvt(VTKind from, VTKind to, long long ii, double fi,
                long long *oi, double *of) {
    RV in, o;
    in.k = from;
    in.len = 0;
    if (from == VMT_F32 || from == VMT_F64) in.v.f = fi; else in.v.i = ii;
    o = cvt_to(in, to, 0);
    *oi = o.v.i;
    *of = o.v.f;
    return 1;
}

#if VM_REACTIVE
// ---------- entry points for the reactive engine (vm_reactive.h) ----------
//
// The same interpreter on a frame the caller owns: rvm/ fills a statement's
// extern slots, runs its body, reads the published slot back, and walks the IR
// with vm_rx_eval to recover sibling values when inverting an operator.

VMStatus vm_rx_exec(Func *f, unsigned char *frame, long long budget) {
    f->run->budget = budget;
#ifndef VM_NO_CALL
    f->run->callee_used = 0;
    uintptr_t saved_base = stack_enter(f->run, &budget);
#endif
    int st = exec_stmt(f, frame, &f->nodes[f->body]);
#ifndef VM_NO_CALL
    f->run->stack_base = saved_base;
#endif
    if (st == X_RET || st == X_OK) return VM_OK;
    if (st == X_BUDGET) return VM_BUDGET;
    if (st == X_CALLEE) return VM_CALLEE_OVERFLOW;
    if (!f->run->last_error) f->run->last_error = "runtime error";
    return VM_ERR;
}

int vm_rx_eval(Func *f, unsigned char *frame, int node, RV *out) {
    long long saved_budget = f->run->budget;
    f->run->budget = -1;
#ifndef VM_NO_CALL
    int saved_callee = f->run->callee_used;
    f->run->callee_used = 0;
    uintptr_t saved_base = stack_enter(f->run, &saved_callee);
#endif
    f->run->sys->memset(out, 0, sizeof(*out));
    int st = eval_expr(f, frame, &f->nodes[node], out);
    f->run->budget = saved_budget;
#ifndef VM_NO_CALL
    f->run->callee_used = saved_callee;
    f->run->stack_base = saved_base;
#endif
    return st == X_OK;
}

void vm_rx_load_slot(Func *f, unsigned char *frame, int slot, RV *out) {
    *out = load_slot(f, frame, slot);
}

int vm_rx_store_slot(Func *f, unsigned char *frame, int slot, RV val) {
    return do_store_local(f, frame, slot, val) == X_OK;
}

int vm_rx_binop(Func *f, int sub_op, VTKind k, RV a, RV b, RV *out) {
    const char *saved = f->run->last_error;
    if (eval_binop(f, sub_op, k, a, b, out) != X_OK) { f->run->last_error = saved; return 0; }
    return 1;
}

RV vm_rx_cvt(RV in, VTKind to) { return cvt_to(in, to, 0); }

int vm_rx_elem_size(VTKind k) { return vt_size_of(is_array(k) || is_slice(k) ? arr_elem(k) : k); }

int vm_rx_agg_bytes(VTKind kind, int pack_bits, int n) {
    return (int)agg_copy_bytes(kind, pack_bits, n);
}

int vm_rx_slot_bytes(VMType t) { return vt_slot_bytes(t); }
#endif // VM_REACTIVE
#endif // VM_NO_COMPILER

#endif // VM_RUN_INCLUDED