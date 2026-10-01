#ifndef VM_RUN_H
#define VM_RUN_H

#include "common/tsys.h"
#include "common/arena.h"
#include "vm/vm_config.h"

// Type-availability switches for embedded builds.
// Define VM_SKIP_F32 / VM_SKIP_F64 / VM_SKIP_I64 (or the legacy
// VM_RUN_SKIP_* names) before including this header to elide type code.
// The derived VM_HAS_* constants are defined in vm/vm_config.h.

// Forward declares (full types in vm_types.h when needed)
typedef struct VM    VM;
typedef struct Func  Func;
typedef struct Args  Args;
typedef struct VmRun VmRun;

// When included from vm.h, these types are already defined.
#ifndef VM_H

typedef enum {
    VMT_VOID = 0,
    VMT_I32,
    VMT_F32,
    VMT_F64,
    VMT_I64,
    VMT_ARR_I32,
    VMT_ARR_F32,
    VMT_ARR_F64,
    VMT_ARR_I64,
    VMT_SLICE_I32,
    VMT_SLICE_F32,
    VMT_SLICE_F64,
    VMT_SLICE_I64
} VTKind;

typedef struct {
    VTKind kind;
    int    len;
    int    elem_shift;   // compile-time only; see vm.h
    int    pack_bits;    // sub-word element packing; see vm.h
    int    inner_len;    // nested-array row width, compile-time only; see vm.h
    int    struct_id;    // struct record / ref, compile-time only; see vm.h
    int    is_const;     // read-only view, compile-time only; see vm.h
} VMType;

static inline int vmtype_fx_shift(VMType t) {   // see vm.h
    if (t.kind == VMT_I32) return t.len;
    if (t.kind == VMT_ARR_I32 || t.kind == VMT_SLICE_I32) return t.elem_shift;
    return 0;
}

typedef enum {
    VM_OK              = 0,
    VM_ERR             = -1,
    VM_BUDGET          = -2,
    VM_CALLEE_OVERFLOW = -3
} VMStatus;

#endif // !VM_H

// ----- Minimal VM lifecycle (no parser needed) -----
#ifdef VM_NO_COMPILER
VM   *VM_API(vm_create)(Tsys *sys);
#endif
VM   *VM_API(vm_create_minimal)(Tsys *sys);

#ifndef VM_H
void  VM_API(vm_destroy)(VM *vm);
const char *VM_API(vm_last_error)(VM *vm);

// ----- Args / call frame -----
size_t   VM_API(func_frame_size)(Func *f);
int      VM_API(func_param_count)(Func *f);
VMType   VM_API(func_param_type)(Func *f, int i);
int      VM_API(func_param_used)(Func *f, int i);
VMType   VM_API(func_return_type)(Func *f);

void     VM_API(args_bind)(Args *a, Func *f, void *frame_buf, size_t frame_size);
void     VM_API(args_set_i32)(Args *a, int param_idx, int v);
#if VM_HAS_F32
void     VM_API(args_set_f32)(Args *a, int param_idx, float v);
#endif
#if VM_HAS_F64
void     VM_API(args_set_f64)(Args *a, int param_idx, double v);
#endif
#if VM_HAS_I64
void     VM_API(args_set_i64)(Args *a, int param_idx, long long v);
#endif
void     VM_API(args_set_arr)(Args *a, int param_idx, void *ptr, int len);
int      VM_API(args_get_i32_result)(Args *a);
#if VM_HAS_F32
float    VM_API(args_get_f32_result)(Args *a);
#endif
#if VM_HAS_F64
double   VM_API(args_get_f64_result)(Args *a);
#endif
#if VM_HAS_I64
long long VM_API(args_get_i64_result)(Args *a);
#endif

// ----- C-function registration -----
// i32_fn/fx_fn are independent slots (see CFuncEntry in vm_types.h):
// i32_fn is a plain raw-int (shift 0) variant, fx_fn is a fixed-point QM.N
// variant (fx_shift == N, e.g. 16 for fx16). Either or both may be NULL.
void VM_API(register_c_func_1arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float),
    const char *f64_name, double (*f64_fn)(double),
    const char *i32_name, int    (*i32_fn)(int),
    const char *fx_name,  int    (*fx_fn)(int),
    int fx_shift);

void VM_API(register_c_func_2arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float),
    const char *f64_name, double (*f64_fn)(double, double),
    const char *i32_name, int    (*i32_fn)(int, int),
    const char *fx_name,  int    (*fx_fn)(int, int),
    int fx_shift);

// Ctx variants: each fn slot takes the VM's user_data (vm_set_user_data) as a
// leading void*, so a native reaches host-attached state without a global.
// There is one per arity, alongside the plain forms; only the fn POINTER types
// change. The *_name slots do not: they still name the plain, ctx-less C
// function the C emitter writes into generated code, which is what lets a
// native be ctx-based in the interpreter and global-bound in the generated C.
void VM_API(register_c_func_1arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float),
    const char *f64_name, double (*f64_fn)(void*, double),
    const char *i32_name, int    (*i32_fn)(void*, int),
    const char *fx_name,  int    (*fx_fn)(void*, int),
    int fx_shift);

void VM_API(register_c_func_2arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int),
    int fx_shift);

void VM_API(register_c_func_3arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int),
    int fx_shift);

void VM_API(register_c_func_4arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_5arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_6arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_7arg_ctx)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_3arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int),
    int fx_shift);

void VM_API(register_c_func_4arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_5arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_6arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int),
    int fx_shift);

void VM_API(register_c_func_7arg)(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int, int),
    int fx_shift);

// ----- Run -----
VMStatus VM_API(func_run)(Func *f, Args *a, long long budget);
#endif // !VM_H

// Convenience: register with only the types that are enabled.
// (Defined outside #ifndef VM_H so available in unity builds.)
#if VM_HAS_F32
#define REG_C_FUNC_F32_1ARG(f32_n, f32_f) f32_n, f32_f
#define REG_C_FUNC_F32_2ARG(f32_n, f32_f) f32_n, f32_f
#define REG_C_FUNC_F32_3ARG(f32_n, f32_f) f32_n, f32_f
#else
#define REG_C_FUNC_F32_1ARG(f32_n, f32_f) NULL, NULL
#define REG_C_FUNC_F32_2ARG(f32_n, f32_f) NULL, NULL
#define REG_C_FUNC_F32_3ARG(f32_n, f32_f) NULL, NULL
#endif
#if VM_HAS_F64
#define REG_C_FUNC_F64_1ARG(f64_n, f64_f) f64_n, f64_f
#define REG_C_FUNC_F64_2ARG(f64_n, f64_f) f64_n, f64_f
#define REG_C_FUNC_F64_3ARG(f64_n, f64_f) f64_n, f64_f
#else
#define REG_C_FUNC_F64_1ARG(f64_n, f64_f) NULL, NULL
#define REG_C_FUNC_F64_2ARG(f64_n, f64_f) NULL, NULL
#define REG_C_FUNC_F64_3ARG(f64_n, f64_f) NULL, NULL
#endif

// ----- Serialization -----
// Returns size needed to serialize a Func. Returns 0 on error.
size_t  VM_API(func_serialize_size)(Func *f);

// Serialize into buf. If buf is NULL, returns required size without writing.
// Returns number of bytes written, or 0 on error.
size_t  VM_API(func_serialize)(Func *f, void *buf, size_t buf_size);

// Optional smaller wire format: identical semantics to
// func_serialize_size()/func_serialize(), but packs each IR node into 8 bytes (the
// same encoding VM_COMPACT_NODES uses in memory) to shrink the blob sent to a
// memory-constrained receiver. Returns 0 if `f` doesn't fit the encoding (more than
// ~510 nodes/child-indices, or a constant that doesn't survive the 32-bit payload
// word); fall back to func_serialize() then. func_deserialize() auto-detects which
// format a blob uses.
size_t  VM_API(func_serialize_compact_size)(Func *f);
size_t  VM_API(func_serialize_compact)(Func *f, void *buf, size_t buf_size);

// Deserialize from buffer. Takes ownership: the buffer must have been
// allocated with sys->malloc and will be freed by vm_destroy.
// Returns NULL on error (sets vm->last_error).
Func   *VM_API(func_deserialize)(VmRun *run, void *buf, size_t buf_size);

#endif
