// vm_emit_c_dialect.h - the C emitter's IR walker, opened to other C-family
// targets. Internal to vm_emit_c.c and its dialects (vm_emit_shader.c); a host
// never includes this.
//
// GLSL and HLSL share C's statement syntax, its static types and -- the thing
// that decides it -- its value semantics for arrays, so they are dialects of the
// C walker rather than a third copy of it. A dialect is a table: spellings and
// flags where C and the target differ by a word, and a few hooks where the
// target needs code of its own. The C dialect is the walker's own behaviour, so
// vm_emit_c.c knows nothing about any other target, and output under it is
// byte-identical to what it was before dialects existed.
//
// What a dialect cannot express never reaches the walker: vm_emit_shader.c
// refuses it in a pre-pass (strings, unknown-length slices, i64 / f64, structs,
// recursion), so the C-only branches carry no guards.

#ifndef VM_EMIT_C_DIALECT_H
#define VM_EMIT_C_DIALECT_H

#include "vm.h"
#include "common/string_pure.h"

typedef struct ECEmit ECEmit;

typedef struct ECDialect {
    const char *name;
    int bool_is_type;     // a condition must be a bool, and a comparison IS one: a
                          // value-position compare wraps int(), every other
                          // condition gets an explicit != 0
    int void_casts;       // `(void)x;` for never-read symbols
    int ctor_casts;       // T(x) rather than (T)(x)
    int direct_index;     // arrays index directly -- v[i], not v.data[i]
    int arrays_as_slices; // a known-length []T parameter takes the array itself
    int elide_print;      // print() emits nothing, whatever the VM was told
    const char *f32_suffix;   // after an f32 literal: "f" or ""
    const char *u32_type;     // the unsigned type the << lowering goes through
    const char *imod_fn;      // integer % as a call (GLSL leaves negatives undefined), or NULL
    const char *void_expr;    // how a discarded value is written: "(void)" or ""
    int loop_guard;           // > 0: every loop breaks after this many iterations
    int c_helpers;            // the output defines C helpers it may call: vm_div_* /
                              // vm_mod_* (each operand evaluated once), vm_f2i /
                              // vm_f2l (saturating), and the non-finite constants

    // Hooks. Each is called only where the target differs; NULL is C.
    void (*type_name)  (ECEmit *e, Func *owner, VMType t);
    void (*zero_value) (ECEmit *e, VMType t);                 // what follows " = "
    void (*arr_lit)    (ECEmit *e, IRNode *lit);
    void (*native_call)(ECEmit *e, Func *callee, IRNode *n);
    void (*param_decl) (ECEmit *e, Func *f, int param);       // "in vec3 p"
    void (*sym_name)   (ECEmit *e, Func *f, int slot);        // a local or parameter's name
    const char *(*bitcast_fn)(int from_kind, int to_kind);
    int  (*user_call)  (ECEmit *e, Func *callee, IRNode *n);  // 1 = emitted it (an intrinsic)
    void (*slice_value)(ECEmit *e, IRNode *slice);            // a row of a nested array, as a value
    void *user;                                               // the dialect's own state
} ECDialect;

// For a dialect's hooks: where output goes, what is being emitted, and the
// walker itself for sub-expressions.
StrBuf *ec_out(ECEmit *e);
VM     *ec_vm(ECEmit *e);
Func   *ec_func(ECEmit *e);
const ECDialect *ec_dialect(ECEmit *e);
void    ec_expr(ECEmit *e, IRNode *n);

// Prototypes, then definitions, of list[0..count) under names[], through `d`,
// appended to `out`. Returns 0 if the buffer ran out of memory.
int     vm_emit_c_funcs(VM *vm, const ECDialect *d, Func **list, const char **names,
                        int count, StrBuf *out);

#endif
