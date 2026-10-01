/*

The goal of this language is coding in realtime for any platform,
with the feeling of using an untyped language, but still with the 
ability of type control when desired, and exporting to C.

Types:
 * base types: i32 / i64 / f32 / f64 (int, float, double are aliases)
 * fixed point: fx1 - fx32 (fx16 is 16.16) compiles to base types and shifts
 * [N]T arrays / []T slices; u1/u2/u4/u8/u16 packs multiple

Basics:
 * for, while, if/else, break/continue/return,
 * C style arithmetic, bitwise and comparison operators.
 * js style => lambdas, 'as' casts, bitcast(T, v), const, array/string literals

Compile directives:
   #rewire f64 -> fx16         // change every f64 to fixed point 16.16
   #rewire literal f64 -> f32  // 1.0 now is float instead of double
   #disable safe_div_by_zero   // don't inject zero check

*/

#ifndef VM_H
#define VM_H

#include "common/tsys.h"
#include "parser/parser.h"
#include "common/arena.h"
#include "common/intern.h"

typedef struct VM   VM;
typedef struct Func Func;
typedef struct Args Args;

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
    int    len;   // scalars: fixed-point shift (compile-time only); arrays: element count
    // Arrays/slices need `len` for their element count, so the fixed-point
    // shift of an i32 *element* lives here instead. Compile-time only: the
    // runtime never reads it and it is not serialized.
    int    elem_shift;
    // Sub-word element packing: 0 = unpacked (one element per 32/64-bit slot),
    // else 1/2/4/8/16 bits per element stored in an i32 word array. The
    // aggregate's `kind` stays VMT_ARR_I32 / VMT_SLICE_I32 and `len` stays the
    // *logical* element count, so bounds checks and .len need no special case
    // and no new VTKind is consumed.
    //
    // Unlike elem_shift this DOES affect the frame layout, so it has to survive
    // serialization -- it rides in the high byte of the sym record's type_len
    // (vm_run.c, SYM_LEN_PACK_SHIFT) rather than costing a format version bump.
    int    pack_bits;
    // Row width of a nested array: `[[1,2],[3,4]]` is one flat [4]i32 with
    // inner_len 2, so the outer length is len/inner_len. 0 = a flat array (or
    // any non-array). Storage is unchanged -- nesting is only how indexing
    // reads it, `a[i]` yielding a slice of inner_len elements -- so like
    // elem_shift this is compile-time only and never serialized.
    int    inner_len;
    // Nonzero on a struct record, a [N] array of records, or a ref / slice of
    // them: 1 + index into the Func's struct table (Func.structs). The storage is
    // an ordinary [S]u8 / []u8 aggregate either way; this is what stops it from
    // being USED as one. Compile-time only, like elem_shift and inner_len: never
    // serialized.
    int    struct_id;
    // 1 = a read-only view: `const[]i32`, and what an unannotated aggregate
    // parameter infers. Storage is identical to the mutable slice -- this only
    // decides what may be WRITTEN through it -- so like elem_shift, inner_len and
    // struct_id it is compile-time only and never serialized.
    //
    // The whole guarantee rests on this bit surviving every place a type is
    // copied or compared. Copy VMTypes whole, never field by field (see
    // ret_type_join), and when a comparison enumerates fields, decide explicitly
    // whether const takes part -- forgetting it does not fail, it silently stops
    // enforcing.
    int    is_const;
} VMType;

// The fixed-point shift of a value of type t -- of each element, for an
// aggregate -- or 0 when it is not fixed point. A scalar keeps it in len, an
// aggregate in elem_shift, so read it through here rather than picking one.
static inline int vmtype_fx_shift(VMType t) {
    if (t.kind == VMT_I32) return t.len;
    if (t.kind == VMT_ARR_I32 || t.kind == VMT_SLICE_I32) return t.elem_shift;
    return 0;
}

typedef enum {
    VM_OK              = 0,
    VM_ERR             = -1,
    VM_BUDGET          = -2,   // execution stopped: op budget exhausted
    VM_CALLEE_OVERFLOW = -3    // callee-frame bump pool exhausted
} VMStatus;

// ----- Lifecycle -----
VM   *vm_create(Tsys *sys, Parser *parser);
void  vm_destroy(VM *vm);

Func *func_create(VM *vm, ASTNode *node, ParseResult *pres);   // node = code tree (items list) OR a `=>` literal; pres may be NULL
void  func_free(Func *f); // note this does not do anything, you can't really free a func due to arena alloc

// Last compile / run error (interned in VM context). NULL when none.
const char *vm_last_error(VM *vm);
int        vm_last_error_row(VM *vm);  // 0-based, -1 if unknown
int        vm_last_error_col(VM *vm);

// After a compile: the signature of every body compiled for the lambda whose
// parameter list contains `node`, as "(a: i32) => f64, (a: f64) => f64".
// Returns how many; 0 when `node` is not in a parameter list or the function
// was never specialised.
int   vm_signature_labels(VM *vm, const ASTNode *node, char *out, int out_max);

// Opaque user data attached to the VM. Delivered to natives registered with
// register_c_func_*arg_ctx as a leading void* argument. Set once before the
// first func_run; never freed by the VM.
void  vm_set_user_data(VM *vm, void *data);

// ----- Shared variables -----
// An ordinary named variable whose storage lives in a host-owned block instead
// of the per-call frame, so it persists across calls AND can be pointed at by
// several separately compiled VMs -- which is how two independently compiled
// programs share state without sharing a VM.
//
// Two steps, in order:
//   1. vm_declare_globals() once, on the source that DECLARES them, to fix the
//      layout. Hand the table to every VM that should see them via
//      vm_import_globals(), before compiling anything that names one.
//   2. vm_set_globals() on each of those VMs, to bind the actual bytes.
//
// Importing one derived layout, rather than letting each VM re-infer it from
// its own copy of the declaration text, is what makes a mismatch a compile
// error: a `#rewire` is applied to a VM's whole item list before compilation
// (see func_create), so a directive in one program would otherwise silently
// re-type the shared declarations for that VM alone and overlay two different
// layouts on the same bytes.

#ifndef VM_MAX_GLOBALS
#define VM_MAX_GLOBALS 64
#endif
#ifndef VM_GLOBAL_NAME_MAX
#define VM_GLOBAL_NAME_MAX 48
#endif

// One shared variable's fixed place in the block.
//
// The name is TEXT, not an InternID: every Parser owns its own InternCtx, so
// ids mean nothing outside the VM that produced them, and a table whose job is
// to cross between VMs would mis-resolve every name. vm_import_globals
// re-interns each name into the receiving VM.
typedef struct {
    char   name[VM_GLOBAL_NAME_MAX];
    VMType type;
    int    offset;   // byte offset into the globals block
    int    shift;    // fixed-point shift, as VMSym.shift
    // A struct-typed variable (a record, or an array of them) is laid out exactly
    // as C lays out the struct, so a host declaring the same struct reads it
    // directly. Its struct is named here as TEXT, for the same reason `name` is,
    // with a hash of the layout: a program importing the table has to declare a
    // struct of that name with the same layout, or binding the variable fails.
    // Empty / 0 for everything else. type.struct_id is always 0 in the table.
    char   struct_name[VM_GLOBAL_NAME_MAX];
    unsigned long long struct_hash;
} VMGlobal;

// The layout, shared by every VM that agrees on it. Plain value type, no
// ownership: pass by pointer, keep on the stack, copy freely. A VM keeps its
// own resolved arena copy, so the caller's may go out of scope immediately.
typedef struct VMGlobalTable {
    VMGlobal g[VM_MAX_GLOBALS];
    int      count;
    size_t   size;   // total bytes the block must hold
    unsigned long long hash;  // identity of this layout; see vm_set_globals
} VMGlobalTable;

// Compiles `node` to discover what variables its top level declares and packs
// them into *out. Functions defined at that top level compile into `vm` as
// usual and are NOT part of the table; only variables are.
//
// Returns the tree as a zero-arg Func -- the INITIALISER. Run it once against a
// freshly zeroed block to apply whatever initialisers the source wrote, so
// non-constant ones need no special handling. NULL on a parse/compile error
// (message via vm_last_error).
//
// `exclude` / `n_exclude`: top-level variable names to leave as ordinary locals
// rather than promote. A caller often has to compile MORE than the declaration
// source -- a prelude of `#directive`s and helpers the declarations are meant
// to be read under -- so pass the prelude's own top-level variable names here
// and only the real declarations are promoted. NULL/0 when there is no prelude.
Func *vm_declare_globals(VM *vm, ASTNode *node, ParseResult *pres,
                         const char *const *exclude, int n_exclude,
                         VMGlobalTable *out);

// The top-level variable names `node` declares, written into `out` as pointers
// into vm's intern table (valid as long as vm is). Returns the count, or -1 on
// a compile error. Used to build vm_declare_globals' `exclude` list from a
// prelude, and to diagnose variables written where they will not be shared.
int vm_collect_top_level_vars(VM *vm, ASTNode *node, ParseResult *pres,
                              const char **out, int max_out);

// Make `tbl`'s variables resolvable by name in everything compiled into `vm`
// afterwards. Call before func_create on any body that names one.
void  vm_import_globals(VM *vm, const VMGlobalTable *tbl);

// Bind the actual storage. `layout_hash` is the table's `hash`, recorded so a
// caller binding one block to several VMs can be checked for agreement. The
// block is borrowed: never freed, never copied, and outlives any single call.
void  vm_set_globals(VM *vm, void *block, size_t size, unsigned long long layout_hash);
void *vm_get_globals(VM *vm);

// Prefix for a shared variable's name in emitted C -- typically "<instance>."
// so `phase` emits as `inst.phase`. Per VM, so two VMs compiled from the
// same declarations can be pointed at two different C instances without either
// body mentioning one. Empty by default, which emits the bare name.
void  vm_set_globals_c_prefix(VM *vm, const char *prefix);

// ----- Host-declared constants -----
// A `const NAME = value` the HOST supplies rather than the source: the script
// reads it as a compile-time constant it cannot assign to, and the emitted C
// gets the folded literal.
//
// Unlike a unit-level `const` these survive func_create -- they belong to no
// compilation unit, so recompiling is not a redefinition -- which is what makes
// them usable for values the host owns and the script only reads.
//
// Redeclaring a name replaces the value rather than shadowing it. Every body
// compiled afterwards folds the NEW value; an already-compiled Func keeps the
// one it folded, so the host must recompile for a change to take effect.
//
// `is_int` picks how the value is re-emitted at each use (NUM_INTEGER vs
// NUM_DOUBLE), the same untyped-literal treatment a source `const` gets, so the
// active #rewire re-types it at the use site. Returns 1, or 0 if the name is
// taken by something the VM cannot shadow (a #define) or the allocation failed.
int   vm_declare_const_num(VM *vm, const char *name, double value, int is_int);

// Compile for a GPU: `#rewire f64 -> f32` and `#rewire i64 -> i32` (types and
// literals both) as if every unit began with them, without prepending text, so
// error positions stay on the user's lines. Set it before func_create on the VM
// that compiles for vm_emit_glsl / vm_emit_hlsl, AND on the VM whose run is the
// reference those are compared against -- f32 on the GPU against f64 in the VM
// is not a comparison. A unit's own #rewire still applies on top. It also turns
// auto_pack off: packing saves nothing on a GPU and has no type there. 0
// restores the identity tables and auto_pack.
void  vm_set_shader_profile(VM *vm, int on);

// ----- Host buffers -----
// A named array the HOST owns and the script indexes: `screen[i] = c`,
// `palette[id] = rgb`. The script reads and writes its ELEMENTS; it may not
// assign the name, and never declares one.
//
// The third kind of non-frame storage, because neither of the other two fits a
// framebuffer: a shared variable's layout is derived from declaration source,
// which a host-owned buffer has none of, and a const is folded and cannot be
// written. What this adds is a {ptr,len} slot the host rebinds at will --
// including between runs of an already-compiled Func, so the length follows a
// resize with no recompile.
//
// Mechanically a VMSym like any other with a different base pointer
// (VMSym.is_host_buf), so indexing, assignment, bounds checks, `.len` and
// slice() all work with no special case.
//
// `id` is chosen by the host and must be STABLE across builds -- the same rule
// native tokens follow -- because it is what a serialized VMSym.offset means.
// 0 <= id < VM_MAX_HOST_BUFS (vm_types.h).
//
// `type` must be a SLICE type; the usual two are a byte buffer
// ({VMT_SLICE_I32, .pack_bits = 8}, i.e. []u8, where element i is byte i) and a
// word buffer ({VMT_SLICE_I32}, i.e. []i32). Build it with a `= {0}`
// initialiser -- see the VMType field-init trap in vm.c.
//
// `c_name` is the identifier vm_emit_c.c writes into generated C, and
// `c_len_expr` what it writes for `.len` (NULL means "<c_name>_len"). The
// emitted form is a PLAIN C ARRAY -- `screen[i] = c`, not `screen.data[i]` --
// so the target declares an ordinary `unsigned char *` or `int[]` and needs to
// know nothing about the emitter's slice structs.
//
// Re-declaring an id replaces it. Returns 1, or 0 for a bad id, a non-slice
// type, or a name already taken by a #define.
//
// NOTE: needs arrays, so a VM_NO_ARRAYS build cannot use one. Those targets
// reach a framebuffer through the C export instead.
int   vm_declare_host_buffer(VM *vm, int id, const char *name, VMType type,
                             const char *c_name, const char *c_len_expr);

// Borrowed -- never freed, never copied -- and rebindable at any time. An
// unbound buffer is {NULL,0}, and any access fails the run with "array index
// out of bounds" rather than dereferencing NULL, so passing {NULL,0} is how you
// take a buffer away from a VM that should not touch it.
void  vm_bind_host_buffer(VM *vm, int id, void *ptr, int len);

// ----- Args / call frame -----
size_t   func_frame_size(Func *f);
int      func_param_count(Func *f);
VMType   func_param_type(Func *f, int i);
// 1 if the compiled body referenced param `i` at least once -- a wrap signature
// may offer more params than a given body needs.
int      func_param_used(Func *f, int i);
VMType   func_return_type(Func *f);

void     args_bind(Args *a, Func *f, void *frame_buf, size_t frame_size);

void     args_set_i32(Args *a, int param_idx, int v);
void     args_set_f32(Args *a, int param_idx, float v);
void     args_set_f64(Args *a, int param_idx, double v);
void     args_set_i64(Args *a, int param_idx, long long v);
// Borrowed pointer; array slot in the frame just stores ptr+len.
void     args_set_arr(Args *a, int param_idx, void *ptr, int len);

int      args_get_i32_result(Args *a);
#ifndef VM_NO_ARRAYS
// A numeric result as the numbers it stands for: the first `max` elements as f64,
// an fx value already divided by its shift (vmtype_fx_shift), a scalar as one
// element. Returns how many were written and sets *total to the real length, or
// returns -1 -- with *total 0 -- when the result is not an i32/f32/f64 scalar or
// unpacked aggregate. Only the interpreter's
// frame is read, so call it before the Args' frame is reused.
int      args_get_result_elems(Args *a, double *out, int max, int *total);
#endif
float    args_get_f32_result(Args *a);
double   args_get_f64_result(Args *a);
long long args_get_i64_result(Args *a);

// The Args struct is exposed (in vm_types.h) so callers may stack-allocate it.
#include "vm_types.h"

// ----- C-function registration -----
// Register a 1-, 2- or 3-argument C function visible in scripts as `script_name`.
// tok:          token ID from tokens.h (e.g. TOK_FLOOR) or a user-defined ID
//               >= TOK_MATHS_LAST; the table is grown automatically.
// script_name:  the identifier scripts use to call this function.
// *_name:       C identifier of that variant (stored for future codegen); NULL if unused.
// *_fn:         function pointer for that variant; NULL if unused.
// A NULL i32_fn AND a NULL fx_fn marks the function as float-only (i32 args
// promoted to f64). i32_fn/fx_fn are independent slots -- e.g. abs can have
// a plain-int m_iabs in i32_fn and a *different* fixed-point implementation
// in fx_fn if ever needed -- see CFuncEntry in vm_types.h for the full
// dispatch rules. fx_shift is fx_fn's Q-format fractional-bit count (e.g.
// 16 for fx16); 0 if fx_fn is NULL.
void register_c_func_1arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float),
    const char *f64_name, double (*f64_fn)(double),
    const char *i32_name, int    (*i32_fn)(int),
    const char *fx_name,  int    (*fx_fn)(int),
    int fx_shift);

// A native taking no arguments -- a clock, a counter -- which has only an f64
// variant: with no argument to promote from, a call resolves to f64 anyway.
void register_c_func_0arg(VM *vm, int tok, const char *script_name,
    const char *f64_name, double (*f64_fn)(void));

void register_c_func_2arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float),
    const char *f64_name, double (*f64_fn)(double, double),
    const char *i32_name, int    (*i32_fn)(int, int),
    const char *fx_name,  int    (*fx_fn)(int, int),
    int fx_shift);

// As register_c_func_2arg, but each variant takes the VM's user_data
// (see vm_set_user_data) as a leading void* argument. Use for natives that
// need host-attached state without reaching for a global.
void register_c_func_2arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int),
    int fx_shift);

// Three args, otherwise identical to the two above.
void register_c_func_3arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int),
    int fx_shift);

// Four / five / six / seven args, otherwise identical to the above.
// VM_MAX_CFUNC_ARGS (vm_types.h) is the ceiling on a registered function's arity.
void register_c_func_4arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int),
    int fx_shift);

void register_c_func_5arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int),
    int fx_shift);

void register_c_func_6arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int),
    int fx_shift);

void register_c_func_7arg(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(int, int, int, int, int, int, int),
    int fx_shift);

// The remaining ctx arities, alongside register_c_func_2arg_ctx above: every fn
// slot takes the VM's user_data as a leading void*, so a native reaches
// host-attached state without a global.
//
// Only the fn POINTER types change. The *_name slots still hold the plain,
// ctx-less C identifier the C emitter writes into generated code -- which is
// what lets one registration be ctx-based in the interpreter and global-bound
// in the emitted C.
void register_c_func_1arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float),
    const char *f64_name, double (*f64_fn)(void*, double),
    const char *i32_name, int    (*i32_fn)(void*, int),
    const char *fx_name,  int    (*fx_fn)(void*, int),
    int fx_shift);

void register_c_func_3arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int),
    int fx_shift);

void register_c_func_4arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int),
    int fx_shift);

void register_c_func_5arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int),
    int fx_shift);

void register_c_func_6arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int),
    int fx_shift);

void register_c_func_7arg_ctx(VM *vm, int tok, const char *script_name,
    const char *f32_name, float  (*f32_fn)(void*, float, float, float, float, float, float, float),
    const char *f64_name, double (*f64_fn)(void*, double, double, double, double, double, double, double),
    const char *i32_name, int    (*i32_fn)(void*, int, int, int, int, int, int, int),
    const char *fx_name,  int    (*fx_fn)(void*, int, int, int, int, int, int, int),
    int fx_shift);

// Register a C function with an explicit per-argument type signature, for
// functions the uniform-numeric register_c_func_*arg forms can't express --
// specifically ones taking an i32 slice, which reaches C as a (ptr, len) pair.
//
// arg_kinds[]:  one VTKind per script-level argument. The supported shape is
//               zero or more VMT_I32 scalars followed by exactly one trailing
//               VMT_SLICE_I32 (the slice must be last). A script call like
//               drawtext(10, 20, "hi") then dispatches to the C function
//               `ret_kind drawtext(int, int, const int* str, int len)`.
// ret_kind:     VMT_VOID or VMT_I32.
// c_name:       C identifier of the function, stored for codegen.
// fn:           the exact-prototype function pointer.
// scalar_shift: fixed-point fractional bits every i32 scalar slot is coerced to
//               before the call (0 = raw int). A call-site fx value is scaled to
//               this Q-format and an f64/f32 arg is multiplied up, so the C
//               function receives its scalars in Q(scalar_shift). Slice slots
//               are unaffected.
//
// A script literal string argument (VMT_SLICE_I32) satisfies the slice slot;
// so does an i32 array/slice local. The interpreter widens each string byte to
// one i32 element (see compile_string_literal in vm.c).
void register_c_func_sig(VM *vm, int tok, const char *script_name,
    const char *c_name, void *fn,
    int n_args, const VTKind *arg_kinds, int ret_kind, int scalar_shift);

// As register_c_func_sig, but `fn` takes the VM's user_data (vm_set_user_data)
// as a leading void*, ahead of the scalars and the (ptr, len) slice pair --
// the mixed-signature counterpart of register_c_func_*arg_ctx. `c_name` still
// names the plain, ctx-less C function the emitter writes into generated code.
//
// Only the shape actually in use is dispatched: leading scalars, one mid-list
// slice, trailing scalars, VMT_VOID return. A ctx registration with a trailing
// slice fails at call time with a clear error rather than calling through a
// prototype missing the leading void*.
void register_c_func_sig_ctx(VM *vm, int tok, const char *script_name,
    const char *c_name, void *fn,
    int n_args, const VTKind *arg_kinds, int ret_kind, int scalar_shift);

// Mark the last `n_defaults` parameters of an already-registered native (any
// register_c_func_* form) as optional, defaulting to 0 when omitted at the call
// site. Call AFTER registering the function. Only a literal-0 default is
// supported (natives carry no param AST); compile_call synthesizes it. Defaults
// must be trailing.
void register_c_func_defaults(VM *vm, int tok, int n_defaults);

// Make ONE of those optional parameters default to 1.0 (an fx16 FX16_ONE
// constant, rescaled to whatever Q-format the slot wants) instead of 0.
// `arg_index` is 0-based over the script-visible parameters.
//
// For a slot where 0 is a meaningful "do nothing" value -- a coverage/alpha
// argument -- and omitting it should mean "full". Call AFTER
// register_c_func_defaults has marked the slot optional; marking a slot that
// isn't optional does nothing, since the value is only ever synthesized for an
// argument the call site omitted.
void register_c_func_default_one(VM *vm, int tok, int arg_index);

// Declare that slice argument `arg_index` of an already-registered
// register_c_func_sig native carries *bytes* rather than i32 elements, so its
// C prototype is `(..., const unsigned char *s, int len)` instead of
// `(..., const int *s, int len)`. The (ptr, len) convention is unchanged --
// only the pointer's element type differs -- so call sites, arity and the
// emitted code shape all stay the same.
//
// A `[]u8` array/slice argument then reaches the native with zero copying, and
// so does a string literal (the compiler materialises its byte form at compile
// time). Natives left alone keep receiving widened i32 elements exactly as
// before. Call AFTER register_c_func_sig.
void register_c_func_arg_bytes(VM *vm, int tok, int arg_index);

// ----- Built-ins written in the language itself -----
// `src` is DSL source defining exactly the function `name`, e.g.
//   vm_declare_lib_func(vm, "mean", "mean = (xs) => sum(xs) / xs.len\n");
// It is attached to a compilation unit only when that name resolves to nothing
// there, parsed once per VM, and compiled once per unit -- so it specialises per
// call site like any other template, which is the point: one text becomes exact
// f64, f32 or fx<N> code depending on where it is used.
//
// `src` is BORROWED for the VM's lifetime. Re-declaring a name replaces it. A
// name the unit defines itself always wins, wherever in the file it sits. A name
// that also has a registered C native (smoothstep, ...) is reached only under
// `#enable source_builtins`.
void vm_declare_lib_func(VM *vm, const char *name, const char *src);

// As vm_declare_lib_func, plus a C version for the interpreter. `accel` runs in
// place of a specialisation's body once the call has marshalled its arguments
// into `frame` (the callee's frame, laid out as for the body); it writes the
// return slot and returns 1, or returns 0 to decline and let the body run. It is
// attached only to a specialisation whose parameters and result share one float
// kind, and `#enable source_builtins` turns it off. It must compute exactly what
// the source does: emitters always emit the source.
void vm_declare_lib_func_accel(VM *vm, const char *name, const char *src,
                               int (*accel)(struct Func *spec, unsigned char *frame));

// The default library (vm/vm_lib.h): sum, mean, fromPolar, toPolar, rotate, the
// GLSL geometry functions (dot, length, distance, normalize, reflect, refract,
// cross, faceforward) and the source forms of the step built-ins. Called by vm_create, so a host gets
// them without doing anything; a host that wants its own version of one calls
// vm_declare_lib_func afterwards.
void vm_register_lib_defaults(VM *vm);

// Releases the parse arenas of whatever library sources this VM actually used.
// Called by vm_destroy; a host never needs it.
void vm_lib_free(VM *vm);

// ----- Keeping a native call intact -----
// Tell the compiler that the HOST rewrites this call by its token -- inverting
// it for a drag, snapping it, or anything else that reads IR_CALL.ki -- so the
// call must survive as a real IR_CALL: no inline expansion into arithmetic, no
// constant fold. Without it a reactive unit's `mix` at fixed point arrives as
// an IR_BINOP chain and its inverse never fires.
//
// Honoured only while the VM is compiling for the reactive engine
// (vm_rx_begin_unit), so an export build keeps inlining floor/frac/min/max/abs
// and mix exactly as before. Call AFTER the matching register_c_func_*.
void set_c_func_keep_call(VM *vm, int tok, int keep);

// Opt a host native into compile-time folding and elementwise array calls
// (`f([1,2])` becomes one call per lane). Only for a function with no side
// effects and no dependence on call order: N calls inside one C initializer
// list are indeterminately sequenced (C11 6.7.9p23). The built-in math
// functions are marked pure at registration; a host native is assumed not to be.
void set_c_func_pure(VM *vm, int tok, int pure);

// ----- print() -----
// `print(x, ...)` is a compiler intrinsic (like str/slice/bitcast), not a
// registered native, and it exists for ONE consumer: a host that wants to
// watch a script's own debug output. Everyone else gets a no-op.
//
//   print("hi")            -- a literal
//   print("x = " ~ x)      -- the ordinary `~` build, passed through uncopied
//   print("x =", x, y)     -- several args, joined with a single space
//   print()                -- an empty payload (the sink's blank line)
//
// Each argument is formatted by its own type through the same machinery `~`
// and str() use, so numbers, fx, i64, f64 and []u8 all just work. As with any
// string build, a runtime-length part budgets 32 bytes and truncates.
//
// Install a sink to see the output; with none (the default) the argument is
// still evaluated -- side effects are preserved -- and the bytes are
// discarded. NULL clears.
//
// NOTE: print()'s payload is a []u8 slice, so a VM_NO_ARRAYS build has no
// print() to sink and neither of these two functions is compiled there.
void vm_set_print_sink(VM *vm, VMPrintFn fn, void *user);

// ---- Inspection: __ins(x, id) and inspect(x) -------------------------------
// The live editor's inspection intrinsic, in two spellings that build the same
// node. Both evaluate x, report (id, kind, value) to the sink installed here,
// and yield x's value with x's own type completely unchanged -- so splicing
// either around a subexpression cannot alter what the program computes.
// `7 / __ins(2, 0)` is 3, not 3.5.
//
//   __ins(x, id)  the editor's private form, spliced into a throwaway copy of
//                 the buffer. `id` is a literal slot number, so several
//                 inspections in one run can be told apart.
//   inspect(x)    the form that lives in the USER'S BUFFER, written there by
//                 the editor's pin gesture and thereafter ordinary source: it
//                 reaches on_code_changed, the host's engine and every export
//                 target. Reports under VM_INSPECT_ID_NONE. Because it is
//                 type-transparent it can simply be WRAPPED to give it a slot
//                 -- __ins(inspect(x), 3) -- which is how the editor reads a
//                 pin without rewriting the user's text.
//
// That type transparency is why it is an IR op rather than a registered native:
// no native signature preserves its argument's type (an i32 argument coerces
// into an f64 parameter and the expression's type changes underneath it).
//
// Only i32, fx, f32 and f64 operands are reported -- and arrays or slices OF
// those, through the separate array sink below. Any other type -- a string, a
// packed or struct aggregate, a void call -- compiles to the operand ALONE, with
// no node built: hovering something unsupported must not fail the compile, it
// must simply produce no value.
//
// An array LITERAL is the one aggregate that cannot be wrapped as it stands:
// assignment, return and call arguments each special-case the bare literal node.
// __ins(literal, id) therefore binds it to a hidden local and reports a read of
// that, at the cost of a copy. inspect(literal) does not -- it lives in the
// user's buffer and reaches every export -- and reports nothing.
//
// An fx operand is reported as the NUMBER it stands for -- its raw i32 word is
// 2^shift times that, and reporting the word would be a lie -- under VMT_I32
// with the sink's `fx_shift` set. The node's own type is still the operand's fx
// type, so the arithmetic around it is untouched.
//
// Install a sink to see the values; with none (the default) the operand is
// still evaluated and the node costs a NULL check. NULL clears.
//
// NOTE: where VM_HAS_INSPECT is 0 there is no host to report to, so no sink
// exists to install. Both spellings still COMPILE there -- a buffer carrying
// inspect(x) has to build for every target it is exported to -- but they build
// no node at all, so the emitted code and the serialized blob are byte-identical
// to the same source without the wrapper.
#if VM_HAS_INSPECT
void vm_set_inspect_sink(VM *vm, VMInspectFn fn, void *user);
// Where an array or slice operand reports (VMInspectArrFn). Independent of the
// scalar sink: with none installed an aggregate operand is still evaluated and
// yielded, and nothing is reported.
void vm_set_inspect_array_sink(VM *vm, VMInspectArrFn fn, void *user);
#endif

// What the C / JS / Lua emitters do with a print() call.
//
//   VM_PRINT_EMIT_DROP (default) -- the statement is not emitted at all, and
//     the string build feeding it goes with it as dead code. A script that
//     prints therefore costs an export target exactly zero bytes and needs no
//     print of its own, which is the point. The consequence, and the reason
//     this is a choice rather than a rule: print's ARGUMENTS must be
//     side-effect-free, because dropping the statement drops their evaluation.
//
//   VM_PRINT_EMIT_CALL -- emitted C calls VM_PRINT(ptr, len), under a
//     `#ifndef VM_PRINT` no-op definition the output carries itself, so it
//     still compiles standalone and a target that WANTS print just defines the
//     macro first. JS/Lua emit VM.print(view), whose prelude helper defaults to
//     console.log / io.write.
//
// CurlyWas always drops: it has no array or slice support at all.
void vm_set_print_emit(VM *vm, VMPrintEmit mode);

// ----- Run -----
// `budget` caps the number of IR ops executed (each statement / expression
// node counts as one). Pass a negative value for "unlimited". When the
// budget is exhausted, func_run stops and returns VM_BUDGET; the caller
// may inspect the partial frame but cannot resume.
VMStatus func_run(Func *f, Args *a, long long budget);
// What the last run left of its budget: ops used = budget - left. <0 after an
// unlimited run.
long long func_budget_left(Func *f);

#endif
