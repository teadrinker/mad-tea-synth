#ifndef VM_TYPES_H
#define VM_TYPES_H

#include "common/tsys.h"
#include "common/arena.h"
#include "vm/vm_config.h"
#include <stdint.h>

#ifndef VM_NO_COMPILER
#include "common/intern.h"
#include "common/array.h"
#else
typedef int InternID;
#endif


struct VM;
struct Func;

// Entry in the VM's C-function lookup table, indexed by (token_id - TOK_MATHS_FIRST).
// Each slot is a C name (for codegen) plus a raw function pointer; NULL means that
// variant is not provided. Pointers are cast at call time using n_args, e.g.
// n_args == 2 is float(*)(float,float) / double(*)(double,double) / int(*)(int,int).
//
// i32_fn and fx_fn are independent slots so one builtin can have a genuinely
// different implementation for raw ints than for fixed-point values. fx_fn is the
// QM.N variant and fx_shift is its N; the compiler converts every argument to that
// shift and tags the call's result with it, but only when a call-site i32 argument
// actually carries a shift -- a plain int promotes to f64 unless i32_fn covers it.
// Which slot an IR_CALL resolved to is recorded on the node (sub_op: 0=i32, 1=fx).

// Highest arity register_c_func_*arg supports. The interpreter's native-call path
// evaluates arguments into fixed-size stack arrays of this length and dispatches
// through a cast matching n_args, so raising it means adding both a
// register_c_func_Narg and its dispatch arm; the emitters are arity-agnostic.
// arg_kinds[] is sized per script arg, including a slice slot. Raise this BEFORE
// adding an arg: register_c_func_sig silently refuses a registration over the cap,
// which surfaces as a spurious "unknown identifier" at compile time.
#define VM_MAX_CFUNC_ARGS 9

// Parameters a script function may declare, and arguments a call may pass.
#define VM_MAX_PARAMS 16

typedef struct CFuncEntry {
    int         n_args;

    const char *f32_name;   // C identifier for codegen, e.g. "m_floorf"
    void       *f32_fn;

    const char *f64_name;   // e.g. "m_floor"
    void       *f64_fn;

    const char *i32_name;   // NULL for no raw-int variant
    void       *i32_fn;

    const char *fx_name;    // NULL for no fixed-point variant
    void       *fx_fn;
    int         fx_shift;   // fractional bits of fx_fn's Q-format; 0 if fx_fn is NULL

    // Set by register_c_func_*arg_ctx: every fn slot takes VmRun.user_data as a
    // leading void* argument, so a native can reach host state without a global.
    // 0 = plain signatures.
    unsigned char wants_ctx;

    // May the compiler call this at compile time, on constant arguments, and keep
    // the answer? Default 0 -- a host native is assumed to do something. Set for
    // the whole TOK_MATHS_FIRST..TOK_MATHS_LAST range by mark_builtin_math_pure;
    // per-entry so a host can opt one of its own natives in later. The fold happens
    // after compile_call has chosen the variant, so it calls exactly what the
    // interpreter would have. See VM_FLAG_CONST_FOLD.
    unsigned char is_pure;

    // The host inverts, snaps or otherwise rewrites this call BY ITS TOKEN, so
    // the compiler must leave it as a real IR_CALL: no inline expansion, no
    // constant fold. Default 0; set through set_c_func_keep_call, which is what
    // rvm_register_inverse does for every token it gives an inverse.
    //
    // Honoured only while VM.rx_active -- an export build still wants floor(v)
    // on an fx16 array to become four AND masks with no call, which is the
    // entire reason those inline blocks exist.
    unsigned char keep_call;

    void       *ext;        // reserved: future per-arg-type metadata

    // ---- Explicit mixed-signature variant (register_c_func_sig) ----
    // Set when a function takes per-argument types the uniform-numeric slots above
    // can't express -- notably a slice argument (passed to C as a (const T* ptr,
    // int len) pair). When has_sig is set the numeric slots are ignored. The one
    // supported shape is leading VMT_I32 scalars followed by exactly one trailing
    // VMT_SLICE_I32, returning VMT_VOID or VMT_I32.
    int         has_sig;
    int         n_sig_args;                 // script-level arity (incl. the slice arg)
    int         sig_scalar_shift;           // Q-format fractional bits for i32 scalar slots (0 = raw int)
    VTKind      arg_kinds[VM_MAX_CFUNC_ARGS];
    // Sub-word element width of a slice slot: 0 = i32 elements (const int*),
    // 8 = bytes (const unsigned char*). Set by register_c_func_arg_bytes; the
    // (ptr, len) convention is identical either way, only the pointer's element
    // type differs. Registration-side only: never serialized.
    unsigned char arg_pack_bits[VM_MAX_CFUNC_ARGS];
    VTKind      ret_kind;
    const char *sig_name;                   // C identifier for codegen
    void       *sig_fn;                     // exact-prototype function pointer
} CFuncEntry;

// New ops are appended at the END so every earlier IROp value stays stable for the
// serializer.
typedef enum {
    // Statements
    IR_BLOCK,        // items: stmt list
    IR_IF,           // a: cond; b: then-block; c: else-block (or null)
    IR_WHILE,        // a: cond; b: body
    IR_BREAK,
    IR_CONTINUE,
    IR_RETURN,       // a: value (or null for void)
    IR_ASSIGN,       // a: lvalue (IR_LOCAL or IR_INDEX); b: rvalue
    IR_EXPR_STMT,    // a: expr (result discarded)
    IR_CALL_STMT,    // wraps IR_CALL when used as statement

    // Expressions
    IR_CONST_I,
    IR_CONST_F,
    IR_LOCAL,        // slot read
    IR_INDEX,        // a: array local (IR_LOCAL); b: index expr
    IR_BINOP,        // sub_op below; a, b operands
    IR_UNOP,         // sub_op; a operand
    IR_CVT,          // a operand; type field is dst type; src in sub_op
    IR_CALL,         // fn_id; items: arg exprs
    IR_ARR_LIT,      // items: element exprs (for inline init at start of run)
    IR_SELECT,       // a: cond; b: true-expr; c: false-expr (ternary ?:)
    IR_DATA_SLICE,   // string / constant-data literal: ki = borrowed pointer to
                     // VM-arena-owned element storage, n_items = element count,
                     // type = VMT_SLICE_*. Evaluates standalone to a {ptr,len}
                     // slice value (unlike IR_ARR_LIT). items_begin stays -1:
                     // n_items is NOT a child list, so a walker over any op's
                     // items must test items_begin >= 0 first.
    IR_COMMA,        // sequence expression (C comma / block-with-value): items are
                     // evaluated left-to-right; items[0..n-2] run for their side
                     // effects, items[n-1] is the value the node yields.
    IR_LEN,          // `.len` of a slice: a = the slice, type = raw VMT_I32. Only
                     // slices reach here -- a fixed array's (and a string
                     // literal's) length is known at compile time, so compile_field
                     // folds those to IR_CONST_I instead.
    IR_FOR,          // a: cond; b: body; c: step block. The init clause is not a
                     // child: it is compiled as a plain statement emitted just
                     // before this node. `continue` runs the step, which is why
                     // this is its own op rather than sugar over IR_WHILE.
                     // ki = 1 when the trip count is fixed at compile time, so
                     // an emitter's runaway guard has nothing to catch.
    IR_SLICE,       // subslice: items = [base, start, len]. ki = the base's
                     // sub-word element width (0 = one i32 per element, 8 = bytes),
                     // carried the same way IR_INDEX carries its packing. type =
                     // the resulting slice type. start/len are clamped to the base,
                     // so a bad pair yields a shorter slice rather than a wild
                     // pointer; sub_op = 1 means both are known in range and the
                     // emitter may skip the clamp (the interpreter clamps either
                     // way, where it costs four compares). Every operand is a
                     // constant or a plain local read, because the emitted C names
                     // them several times. The operands live in `items` rather than
                     // a/b/c so `ki` stays available: in the compact encoding `c`
                     // and the payload word are the same slot.
    IR_FMT,          // append one formatted value to a byte buffer:
                     // items = [dst, cursor, value]; sub_op = FMT_*; ki = the
                     // per-kind extra (FMT_FX: shift | decimals<<8; FMT_F64:
                     // decimals; 0 otherwise). type = VMT_I32 -- the node evaluates
                     // to the new cursor, so a concatenation is an IR_COMMA of
                     // `cursor = IR_FMT(...)` assignments. dst supplies both the
                     // pointer and the capacity, so overflow silently truncates
                     // rather than running past the buffer.
    IR_BITCAST,      // bit-preserving reinterpretation, `bitcast(T, x)`: a =
                     // operand, type = destination, sub_op = the source VTKind --
                     // exactly IR_CVT's layout, so the compact wire encoding
                     // already carries it. Only same-width pairs are built: within
                     // i32 storage (raw i32 and every fxN) src == dst and the node
                     // is a pass-through carrying the new compile-time type, since
                     // retyping the operand in place would leak into every other
                     // read of a shared IR_LOCAL. i32<->f32 and i64<->f64 are the
                     // real reinterpretations.

    IR_PRINT,        // print(...): a = a []u8 slice expression; type = VMT_VOID.
                     // Hands the operand's (ptr, len) to the run's print sink. With
                     // no sink installed the operand is still evaluated and the
                     // bytes discarded, so print() costs a NULL check. Deliberately
                     // NOT a registered native: a native's c_name is emitted
                     // verbatim by every backend, and the point of print is that no
                     // export target has to provide it. The emitters drop it unless
                     // vm_set_print_emit says otherwise.
    IR_INSPECT,      // __ins(x, id): a = the operand; ki = the inspect slot id;
                     // sub_op = the operand's fixed-point shift (0 when not fx);
                     // type = the OPERAND's type, copied verbatim. Reports
                     // (id, kind, value-as-f64) to the inspect sink and yields the
                     // operand unchanged; an fx operand is reported already scaled
                     // by sub_op, since its raw word is not the number it stands
                     // for. An array or slice operand reports its elements through
                     // the separate array sink instead, with the ELEMENT's fx
                     // shift on sub_op; a literal is first bound to a hidden local
                     // (an IR_COMMA around this node) by the slotted form.
                     //
                     // Type transparency is why this is an IR op and not a native:
                     // `a / __ins(b, 0)` must stay integer division when b is i32,
                     // and no native signature preserves its argument's type (i32
                     // would coerce into an f64 parameter and win every time). With
                     // no sink installed it is a NULL check and nothing else, and
                     // every emitter renders it as its child alone.
    IR_FIELD         // a byte offset into a struct record. a = the base, an
                     // expression yielding the record as {ptr, len}: a record
                     // local, a ref, a call result, or another IR_FIELD. What the
                     // node yields is picked by sub_op (FIELD_* below):
                     //   FIELD_ELEM     b = element index; ki = stride S. Yields
                     //                  element b of a record array as a record
                     //                  {ptr + b*S, S}.
                     //   FIELD_NAT/U8/U16  b = null; ki = byte offset. Loads (or,
                     //                  as an lvalue, stores) a scalar field of
                     //                  type.kind at its natural width, or as a
                     //                  zero-extended byte / 16-bit word.
                     //   FIELD_AGG + p  b = IR_CONST_I element count; ki = offset.
                     //                  Yields an array field or a nested record
                     //                  in place, {ptr + off, count}. p is the
                     //                  field's packing (field_agg_pack), which
                     //                  a whole-field copy needs and the compact
                     //                  node has nowhere else to keep.
                     // Nesting is never folded into one offset: the C emitter
                     // rebuilds `p.inner.c` from the chain.
} IROp;

// sub_op codes for IR_FIELD.
enum {
    FIELD_ELEM = 0,
    FIELD_NAT  = 1,
    FIELD_U8   = 2,
    FIELD_U16  = 3,
    FIELD_AGG  = 4   // + 0..5 for pack_bits 0/1/2/4/8/16
};

static inline int field_agg_sub(int pack_bits) {
    switch (pack_bits) {
        case 1: return FIELD_AGG + 1;
        case 2: return FIELD_AGG + 2;
        case 4: return FIELD_AGG + 3;
        case 8: return FIELD_AGG + 4;
        case 16: return FIELD_AGG + 5;
        default: return FIELD_AGG;
    }
}
static inline int field_agg_pack(int sub_op) {
    static const unsigned char bits[6] = { 0, 1, 2, 4, 8, 16 };
    int k = sub_op - FIELD_AGG;
    return (k >= 0 && k < 6) ? bits[k] : 0;
}

// sub_op codes for IR_FMT: what the value operand is and how it prints.
// vm/vm_fmt.h holds the interpreter's implementation of each; vm_emit_c.c
// emits a textually equivalent copy into generated C.
enum {
    FMT_BYTES,   // value is a []u8 slice/array -- copied through verbatim
    FMT_I32,     // raw integer
    FMT_I64,
    FMT_FX,      // fixed-point i32; ki = shift | (decimals << 8)
    FMT_F64      // f64 (an f32 value is widened first); ki = decimals
};

// sub_op codes for IR_BINOP / IR_UNOP. The op picks an interpreter case
// based on (sub_op, type.kind).
enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_DIV_NATIVE, OP_MOD_NATIVE,
    OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
    OP_AND, OP_OR,           // logical, short-circuit
    OP_BAND, OP_BOR, OP_BXOR, OP_BSHL, OP_BSHL_NATIVE, OP_BSHR,
    OP_NEG, OP_NOT
};

// Maximum lanes one elementwise expression may unroll to. See VM_FLAG_AUTO_VEC.
#ifndef VM_VEC_MAX_LANES
#define VM_VEC_MAX_LANES 64
#endif

// Bitmask flags for boolean directive state (stored in VM.flags).
#define VM_FLAG_CHECK_DIV_ZERO  0x1   // default ON: guard div/mod by zero
#define VM_FLAG_C_SHIFTS        0x2   // default OFF: shift counts as C takes them -- not masked,
                                      // undefined past the width (#enable c_shifts, once
                                      // spelled undefined_behaviour)
#define VM_FLAG_INLINE_POWERS   0x4   // default ON: expand x ** <small int> to
                                      // repeated multiplication (else emit ipow/pow)
#define VM_FLAG_IDENTITY_ELIM   0x8   // default ON: literal-operand algebraic
                                      // simplification (x+0, x*1, x/1 ...) plus
                                      // the annihilators (0*x, 0/x, x&0, 0<<x,
                                      // x%1) that discard the other operand.
                                      // Applied on the AST before the operands are
                                      // compiled, and again on the IR after -- the
                                      // IR half is what makes `0 * (...)`, the
                                      // live-coding mute, actually delete the term,
                                      // since at AST level the operand is still a
                                      // `*` node rather than a literal.
                                      //
                                      // Annihilation discards a whole subtree, so
                                      // it is gated on ir_has_side_effects: `0*f()`
                                      // keeps the call. The subtree is COMPILED
                                      // either way, so a typo inside a muted term
                                      // is still an error.
                                      //
                                      // Not value-preserving for floats: 0 * NaN
                                      // folds to 0, deliberately -- a muted term
                                      // should not poison the mix with a NaN it
                                      // computed while muted.
#define VM_FLAG_AUTO_PACK       0x20  // default ON: shrink an array local whose
                                      // elements are all small constants and which
                                      // is never written (nor returned, passed, or
                                      // otherwise aliased) to the narrowest uN that
                                      // holds them. Safe precisely because there is
                                      // no write whose semantics could shift. Does
                                      // change emitted C: `a = [1,2,3]` becomes
                                      // packed words plus vm_pack_get/set rather
                                      // than a plain int_3.
#define VM_FLAG_AUTO_VEC        0x40  // default ON: `+ - * / % & | ^ == !=` work
                                      // elementwise on fixed arrays, with scalar
                                      // broadcast. Disabling it does NOT restore
                                      // the old silent-nonsense behaviour -- an
                                      // array operand is then a compile error.
                                      // Vectorization is unrolling, so the lane
                                      // count is capped (VM_VEC_MAX_LANES): `a=b*c`
                                      // on [512]f32 would emit 512 multiplies into
                                      // the IR and into generated C.
#define VM_FLAG_LOSSY_ASSIGNMENT 0x10 // default OFF: reassigning an existing
                                      // variable to a narrower type (int-typed var
                                      // later assigned a float, i64->i32, f64->f32,
                                      // plain int <-> fixed-point) is a compile
                                      // error unless this is enabled, in which case
                                      // the value is silently converted
#define VM_FLAG_FX_I64_WIDE         0x80 // default ON: fixed-point mul/div/pow may
                                      // widen to an i64 intermediate so the product
                                      // keeps its low bits, instead of pre-shifting
                                      // both operands apart and losing them.
                                      //
                                      // `#rewire i64 -> i32` also turns this off,
                                      // but takes the i64 TYPE away with it. This
                                      // flag is the other half of that choice:
                                      // disabling it alone keeps i64 usable as a
                                      // declared type while holding fixed-point
                                      // arithmetic to i32.
#define VM_FLAG_INT_WRAP        0x100 // default ON: integer arithmetic in the
                                      // SCRIPT backends is wrapped back to the
                                      // declared width -- `|0` / Math.imul in JS, a
                                      // mask and sign-extend in Lua -- so an i32
                                      // that overflows wraps as it does in C and in
                                      // the interpreter instead of becoming a
                                      // double. The wrap is one call or one `|0`
                                      // per arithmetic node, the single biggest
                                      // cost the script backends add, so a body
                                      // known not to overflow can opt out. No
                                      // effect on emitted C or the interpreter.
#define VM_FLAG_STRICT_INDENT   0x400 // default OFF: an indented block under a head
                                      // that takes none (anything but `if` /
                                      // `else` / `while` / `for`) is a compile
                                      // error instead of being read as cosmetic
                                      // alignment and dissolved back into sibling
                                      // statements. The forgiving default exists
                                      // because indentation meant nothing before
                                      // indent blocks were syntax, so old scripts
                                      // are full of continuation lines set in from
                                      // the margin. Only reaches the vm.c half: the
                                      // parser still builds the block, and a dedent
                                      // matching no open level is a parse error
                                      // either way.
#define VM_FLAG_CONST_FOLD      0x800 // default ON: an operator whose operands are
                                      // both constants is evaluated at compile time
                                      // and replaced by the result. The fold calls
                                      // the interpreter's own eval_binop
                                      // (vm_fold_binop) rather than repeating the
                                      // arithmetic, so a folded value cannot
                                      // disagree with an unfolded one. Division or
                                      // modulo by a zero constant and out-of-range
                                      // shift counts are left alone: those are
                                      // undefined behaviour, and picking an answer
                                      // for UB at compile time is worse than
                                      // leaving the op where the target can define
                                      // it. Changes no result, only node count.
#define VM_FLAG_CONST_PRECISE   0x1000 // default ON: an ANNOTATED const's
                                      // initialiser is evaluated at full f64
                                      // precision and rounded once to the declared
                                      // type. Disabling it compiles the initialiser
                                      // AT the declared type instead, so natives
                                      // inside it resolve to that type's variant
                                      // (fx16_pow rather than m_pow) and the value
                                      // is what the same expression would have
                                      // computed at run time -- for reproducing an
                                      // existing fixed-point pipeline bit for bit.
                                      //
                                      // Precise is the default because it is the
                                      // point of moving work to compile time:
                                      // `const k : fx16 = 2 ** (1/12.0)` quantises
                                      // 1/12.0 before pow ever starts otherwise,
                                      // and every step after compounds it.
                                      //
                                      // An UNANNOTATED const is unaffected: it has
                                      // no type at its declaration, so full
                                      // precision is forced, not chosen.
#define VM_FLAG_AUTO_CONST     0x2000 // default ON: a top-level `name = expr` whose
                                      // initialiser folds to a single constant and
                                      // whose name is written exactly once in the
                                      // whole unit becomes a compile-time constant
                                      // instead of a frame local -- the statement
                                      // emits nothing and every use re-emits the
                                      // folded literal, exactly as a written
                                      // `const` does. See VMConst.is_auto.
                                      //
                                      // Value-preserving by construction: the
                                      // initialiser is compiled either way, and the
                                      // literal re-emitted at each use is the very
                                      // node the local would have been assigned --
                                      // so no arithmetic is repeated at a second
                                      // precision and no new error can appear.
                                      //
                                      // Three conditions, each load-bearing.
                                      // TOP-LEVEL: `if (c) { x = 5 }` writes x once
                                      // too, and folding it would give 5 where the
                                      // frame's zero runs today. FOLDS TO ONE
                                      // CONSTANT: anything else has to be computed.
                                      // WRITTEN ONCE IN THE UNIT: consts resolve
                                      // ahead of the symbol table, so a second
                                      // write -- in this body, in another function,
                                      // or as a parameter name -- would make the
                                      // name unusable there rather than folded.
                                      //
                                      // Off in vm_declare_globals: see the note on
                                      // VM_FLAG_AUTO_PACK there, which it shares.

#define VM_FLAG_SOURCE_BUILTINS 0x4000 // default OFF: where a built-in has BOTH a
                                      // registered C function and a definition in
                                      // the language (vm_lib.h -- smoothstep and
                                      // the rest of the step family), compile the
                                      // source one. It then specialises per call
                                      // site, so fx8 and fx22 stop round-tripping
                                      // through the single fx16 helper and an
                                      // export links no fx library code.
                                      //
                                      // Off by default because the C call is ~11x
                                      // cheaper in the INTERPRETER, which is what a
                                      // live preview runs; an export compiles to C
                                      // and the difference disappears. Refused in a
                                      // reactive unit: an inverse is keyed on the
                                      // native token, so a source built-in would
                                      // silently change what a drag does.

#define VM_FLAG_INLINE_BUILTINS 0x8000 // default OFF: expand a small library body
                                      // into the caller's IR instead of emitting a
                                      // call -- what build_pow_inline and
                                      // compile_mix_fixed already do by hand for
                                      // two specific built-ins. Its own switch
                                      // rather than part of source_builtins, so the
                                      // two can be measured apart.

#define VM_FLAG_CAPTURE        0x10000 // default OFF: a `=>` body may read the
                                      // names in scope where it was WRITTEN --
                                      // lexical capture. Off by default so that
                                      // a body sees only its parameters, consts
                                      // and built-ins, which is what the plain
                                      // compiler can do: vm/ gives a function its
                                      // own symbol table and no access to the
                                      // enclosing frame (resolve_name).
                                      //
                                      // Only the reactive engine implements it;
                                      // the plain compiler REFUSES the directive
                                      // rather than accept it and then fail on
                                      // the first captured name.

#define VM_FLAG_SWIZZLE        0x20000 // default ON: `v.x`, `v.zy`, `c.rgb` on any
                                      // flat array or slice, GLSL-style. One
                                      // component is `v[k]`; several are a vector
                                      // expression. A struct field of the same
                                      // name always wins.

#define VM_FLAG_UNROLL         0x40000 // default ON: a `for` over a range with
                                      // constant ends, or over a sequence of
                                      // known length, that runs at most
                                      // VM_UNROLL_MAX times compiles its body
                                      // once per pass with the loop variable a
                                      // constant -- no counter, no loop. Skipped
                                      // when the body breaks, continues, writes
                                      // the variable, defines a function or
                                      // holds a loop of its own.
#define VM_UNROLL_MAX          4

#define VM_FLAG_C_DIVISION     0x80000 // default OFF: `/` on two integers
                                      // truncates, as in C. Off, it widens both
                                      // to f64 (fxN under #rewire) and `/~` is
                                      // the truncating form.
#define VM_FLAG_C_INT_WRAP     0x100000 // default OFF: emitted C does integer + - * and
                                      // negation through unsigned, so overflow wraps as
                                      // in the VM rather than being undefined. Off, the
                                      // output stays readable and needs -fwrapv to
                                      // promise the same.
#define VM_FLAG_C_FLOAT_TO_INT 0x200000 // default OFF: a float converted to an integer
                                      // is C's cast -- undefined for nan or out of range.
                                      // Off, it saturates and nan gives 0, as Rust `as`
                                      // and wasm's trunc_sat do.

// Set on Func.flags when the compiler snapshotted the directive state onto it.
// Never a directive of its own -- it is what lets an emitter tell "this body
// disabled everything" from "this Func predates the snapshot" (a deserialized
// one, say), so a zero snapshot is not read as "no flags known".
#define VM_FLAG_SNAPSHOT        0x200

#define VM_MAX_DIRECTIVE_STACK 16

// Distinct written names one compilation unit's auto_const scan can track. A
// unit with more than this simply gets no auto-consts (see VM.ac_off).
#ifndef VM_AUTOCONST_MAX
#define VM_AUTOCONST_MAX 256
#endif

typedef struct {
    VTKind  type_map[8];
    VTKind  literal_map[8];
    int     type_shift[8];
    int     literal_shift[8];
    int     flags;
    // The head of the #define list. Restoring it un-defines everything declared
    // since the matching #push, which works because defines are only prepended.
    struct VMDefine *defines;
} DirectiveSnapshot;

typedef struct IRNode IRNode;
struct IRNode {
    int       op;        // IROp
    int       sub_op;    // for BINOP/UNOP, or src-type for CVT
    VMType    type;      // result type (VMT_VOID for stmts)
    long long ki;        // const integer / slot index / func_id
    double    kf;        // const float
    int       a;         // child node index, -1 if null
    int       b;
    int       c;
    int       items_begin;  // start index in Func.child_indices, -1 if none
    int       n_items;
};

// ---- Compact in-memory IR node ----
// Fixed 8 bytes/node: a packed 32-bit header word + a 32-bit payload word.
// Only compiled in when VM_COMPACT_NODES is set (vm_config.h); the default
// (VM_COMPACT_NODES==0) build never sees this and Func.nodes stays IRNode*.
//
// type.len (the fixed-point shift / array length carried on IRNode) is dropped
// entirely here: the interpreter never reads it off an IRNode at runtime, only
// off VMSym.type.len / Func.ret_type.len. Bit widths differ between the general
// layout and the device-tuned one (VM_COMPACT_NODES_DEVICE, auto-selected when
// the build already implies VM_NO_ARRAYS/VM_NO_CALL/no F32-F64-I64): with only
// VMT_VOID/VMT_I32 live, `type` shrinks from 4 bits to 1 and the freed bits
// widen `a`/`b` from 9 to 10 bits each.
#if VM_COMPACT_NODES

#if VM_COMPACT_NODES_DEVICE
#define IRC_TYPE_BITS 1
#define IRC_A_BITS    10
#define IRC_B_BITS    10
// 1 spare bit in word 0 (5+1+5+10+10=31), reserved, must be 0.
#else
#define IRC_TYPE_BITS 4
#define IRC_A_BITS    9
#define IRC_B_BITS    9
// exact fit: 5+4+5+9+9=32
#endif

#define IRC_OP_BITS    5
#define IRC_SUBOP_BITS 5

#define IRC_A_NULL  ((1 << IRC_A_BITS) - 1)
#define IRC_B_NULL  ((1 << IRC_B_BITS) - 1)

typedef struct IRNodeC IRNodeC;
struct IRNodeC {
    uint32_t hdr;      // op | type<<.. | sub_op<<.. | a<<.. | b<<..
    int32_t  payload;  // meaning depends on op
};

#define IRC_TYPE_SHIFT  (IRC_OP_BITS)
#define IRC_SUBOP_SHIFT (IRC_TYPE_SHIFT + IRC_TYPE_BITS)
#define IRC_A_SHIFT     (IRC_SUBOP_SHIFT + IRC_SUBOP_BITS)
#define IRC_B_SHIFT     (IRC_A_SHIFT + IRC_A_BITS)

static inline uint32_t irc_pack_hdr(int op, int type, int sub_op, int a, int b) {
    return  ((uint32_t)op     &  ((1u<<IRC_OP_BITS)-1))
          | (((uint32_t)type   & ((1u<<IRC_TYPE_BITS)-1)) << IRC_TYPE_SHIFT)
          | (((uint32_t)sub_op & ((1u<<IRC_SUBOP_BITS)-1)) << IRC_SUBOP_SHIFT)
          | (((uint32_t)(a < 0 ? IRC_A_NULL : a) & ((1u<<IRC_A_BITS)-1)) << IRC_A_SHIFT)
          | (((uint32_t)(b < 0 ? IRC_B_NULL : b) & ((1u<<IRC_B_BITS)-1)) << IRC_B_SHIFT);
}

#define IRC_OP(n)     ((int)((n)->hdr & ((1u<<IRC_OP_BITS)-1)))
#define IRC_TYPE(n)   ((int)(((n)->hdr >> IRC_TYPE_SHIFT)  & ((1u<<IRC_TYPE_BITS)-1)))
#define IRC_SUBOP(n)  ((int)(((n)->hdr >> IRC_SUBOP_SHIFT) & ((1u<<IRC_SUBOP_BITS)-1)))
#define IRC_A_RAW(n)  ((int)(((n)->hdr >> IRC_A_SHIFT)     & ((1u<<IRC_A_BITS)-1)))
#define IRC_B_RAW(n)  ((int)(((n)->hdr >> IRC_B_SHIFT)     & ((1u<<IRC_B_BITS)-1)))
// Child-index accessors: null (IRC_*_NULL sentinel) maps to -1, matching the
// IRNode convention (ir_child returns 0 for idx<0) so interpreter code can
// share the same "idx < 0 means no child" checks either way.
#define IRC_A(n)      (IRC_A_RAW(n) == IRC_A_NULL ? -1 : IRC_A_RAW(n))
#define IRC_B(n)      (IRC_B_RAW(n) == IRC_B_NULL ? -1 : IRC_B_RAW(n))
// SELECT's false-expr / IF's else-stmt: the third child index, stored in the
// payload word since only these two ops need it and neither has any other
// payload. A full 32-bit slot, so it uses -1-means-null directly.
#define IRC_C(n)      ((n)->payload)

#endif // VM_COMPACT_NODES

// A shared variable as one VM sees it: the portable VMGlobalTable entry (vm.h)
// with its name resolved into that VM's own intern context. Kept separate from
// VMGlobal so the compile-time lookup is an integer compare rather than a
// string compare, and so the per-VM copy costs nothing per unused slot.
typedef struct VMGlobalRef {
    InternID name;
    VMType   type;       // struct_id set only in the VM that declared the table (globals_structs)
    int      offset;
    int      shift;
    InternID struct_name;             // 0 unless a struct type; see VMGlobal
    unsigned long long struct_hash;
} VMGlobalRef;

// A compile-time constant: `const NAME = expr` or `const NAME : T = expr`.
//
// Unlike a shared variable it has no storage anywhere. The initialiser is
// folded once, at the declaration, and every use re-emits the value as an
// IR_CONST_* node inside whatever function named it -- so a const costs
// exactly what writing the literal there would, in the interpreter and in
// emitted C alike, and it never reaches VMGlobalTable.
//
// An unannotated const stays an *untyped* literal (zig's comptime_float): each
// use re-runs the same literal-to-type rules compile_number uses, under the
// #rewire active at the USE site, so one `const kGain = 0.5` serves an f64
// expression and an fx16 one. An annotated one is pinned to `type` instead.
//
// Scoped to one compilation unit -- func_create clears the list -- because a
// shared prelude is compiled once per program, and a VM-lifetime list would call
// the second pass a redefinition.
typedef struct VMConst {
    struct VMConst *next;
    InternID  name;
    double    value;       // the folded value
    int       num_flags;   // NUM_INTEGER / NUM_DOUBLE / NUM_FLOAT: how to re-emit
    VMType    type;        // the annotation, when there was one
    int       has_type;
    struct ASTNode *str;   // string-literal const: recompiled from the AST instead
    int       evaluating;  // recursion guard, for `const a = a`
    // 1 = the compiler made this one, from a plain `name = expr` that turned out
    // to be constant (VM_FLAG_AUTO_CONST). It re-emits `ivalue`/`value` at `type`
    // VERBATIM rather than going through the literal rules: the value is already a
    // compiled IR constant of that exact type, which is what makes the fold
    // value-preserving. `value` holds a float node's number, `ivalue` an integer
    // node's word -- separately, since an i64 past 2^53 does not survive a double.
    int       is_auto;
    long long ivalue;
    // The body that declared it (VM.cur_scope), and the declaration itself, so a
    // specialisation re-compiling that body finds its own const rather than a
    // redefinition.
    struct Func    *scope;
    struct ASTNode *decl;
} VMConst;

// A `#define NAME body` substitution.
//
// Nothing is folded and nothing is checked at the declaration: `values` are the
// AST the parser already built, compiled afresh at every use, so the names
// inside them resolve where they are USED rather than where the define was
// written. That late binding is the whole difference from a VMConst, which
// folds once and can only contain literal arithmetic.
//
// A comma body (`#define pos x,y`) holds several values and expands into
// several call arguments -- see expand_call_args -- and is rejected anywhere
// one value is expected.
//
// Scoped to one compilation unit like VMConst, and for a harder reason: the
// values point into the ParseResult this unit was parsed from, which the host
// frees once compilation is done.
typedef struct VMDefine {
    struct VMDefine *next;
    InternID  name;
    struct ASTNode **values;
    int       n_values;
    int       expanding;    // recursion guard, for `#define a a`
} VMDefine;

// One field of a struct.
typedef struct VMStructField {
    InternID        name;
    VMType          type;      // scalar, [M]T array, or a nested record (struct_id set)
    int             shift;     // fixed-point shift of a scalar field
    int             offset;    // byte offset inside the record
    int             size;      // bytes the field occupies
    int             width;     // FIELD_NAT / FIELD_U8 / FIELD_U16 for a scalar, else FIELD_AGG
    struct ASTNode *def;       // default value, null = zero; only read while the unit compiles
} VMStructField;

// `struct NAME { ... }`, or the anonymous shape an inferred `{ a: 1 }` builds.
// Laid out exactly as a C compiler lays out `struct NAME`, so a host declaring
// the same struct reads the bytes directly. Little-endian by definition, like
// every other frame value (write_i32 in vm_run.c).
typedef struct VMStruct {
    InternID       name;      // 0 for an inferred shape
    VMStructField *fields;
    int            n_fields;
    int            size;      // stride S: C sizeof
    int            align;     // C alignof
} VMStruct;

// The structs one compilation unit declared. A VMType's struct_id is 1 + an index
// into the table of the Func it appears in; every Func compiled in a unit points at
// that unit's table, which is arena-allocated and outlives the compile, because the
// emitters run afterwards and need the field names back.
typedef struct VMStructTable {
    VMStruct *items;
    int       count;
    int       cap;
} VMStructTable;

// A struct instance, array of instances, ref or slice of them -- anything whose
// bytes script code must not see as bytes. See the guard list in vm.c.
static inline int vmt_is_struct(VMType t) { return t.struct_id != 0; }

static inline const VMStruct *vm_struct_get(const VMStructTable *tab, int struct_id) {
    if (!tab || struct_id <= 0 || struct_id > tab->count) return 0;
    return &tab->items[struct_id - 1];
}

// Symbol entry in a function's compile-time symbol table.
typedef struct {
    InternID name;
    VMType   type;
    int      offset;   // byte offset into the frame -- or into VmRun.globals when is_global
    int      is_func;  // 1 if this name resolves to a Func*
    struct Func *fn;   // non-null when is_func
    int      shift;    // fixed-point shift: 0=raw integer, 1..32=fx1..fx32, -1=f32, -2=f64
    int      used;     // 1 once compile_ident resolves an identifier to this sym at least once
    // 1 = shared variable: `offset` is relative to VmRun.globals (host-owned,
    // outlives any one call) instead of the caller's frame. Everything else about
    // the symbol -- and every IR node referring to it -- is unchanged, which is
    // why shared variables need no IR op of their own.
    int      is_global;
    // 1 = host buffer: `offset` is relative to VmRun.host_bufs instead of the
    // caller's frame. Mutually exclusive with is_global -- both answer "which base
    // pointer does `offset` belong to" -- and sym_base tests them in that order.
    // Always a SLICE type, so the slot it names is the {ptr,len} pair load_slot
    // already knows how to read.
    int      is_host_buf;
    // 1 = the variable of an unrolled loop. Its reads there were constants, so if
    // nothing else reads it (`used` still 0) its stores are dead and dropped.
    int      unrolled;
    // 1 = a `for ... in` loop variable past its loop: the name no longer
    // resolves, but the slot stays, and a later declaration of the same name
    // and type takes it back (sym_declare).
    int      hidden;
    // 1 = declared while a hidden sym of the same name exists; the C emitter
    // suffixes the name so the two never share a declaration.
    int      name_dup;
#if VM_REACTIVE
    // The fourth non-frame-owned kind (vm_reactive.h): a name the statement does
    // not declare, bound through the host's resolver. Storage IS an ordinary frame
    // slot -- the host fills it before the statement runs -- so the interpreter
    // needs no special case; `ext_id` is the host's key for what fills it.
    int      is_extern;
    int      ext_id;
    // A component name (the resolver said so): only callable, never a value.
    int      is_comp;
#endif
} VMSym;

// The interpreter's value cell. Declared here rather than in vm_run.c so the
// reactive engine (vm_reactive.h) can hand values in and out of a frame.
// A scalar uses v.i / v.f, an array or slice uses v.ptr + len; never both, so
// ptr shares the union and RV is 16 bytes. Setting v.ptr clobbers the scalar.
typedef struct {
    union { long long i; double f; void *ptr; } v;
    int    len;
    VTKind k;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((aligned(8)))
#endif
RV;

#if VM_REACTIVE
// What the host's resolver answers for a name (or a component call) the unit
// being compiled does not declare. See vm_set_extern_resolver.
typedef struct VmExtern {
    VMType type;       // the binding's type, shift cleared (VMSym.type convention)
    int    shift;      // its fixed-point shift (VMSym.shift convention)
    int    ext_id;     // host key; what vm_rx lowering maps back to a value source
    int    is_func;    // the name is a function binding: `fn` is what a call compiles to
    struct Func *fn;
    int    is_comp;    // the name is a component: calls go through the call resolver
} VmExtern;
// 1 = resolved into *out, 0 = not ours (the compiler goes on to its natives),
// -1 = not ours AND already reported (see VM.rx_name_reported).
typedef int (*VmExternNameFn)(void *user, InternID name, int in_func, VmExtern *out);
typedef int (*VmExternCallFn)(void *user, struct ASTNode *call, InternID name,
                              struct ASTNode **args, int n_args, VmExtern *out);
// Where a literal in the IR came from: the AST node that spelled it.
typedef struct VMLitRef {
    int             ir;
    struct ASTNode *ast;
} VMLitRef;
#endif

// One host-buffer slot: exactly the layout load_slot reads a slice slot as
// (read_ptr at 0, read_i32 at sizeof(void*)), so binding one is a plain write
// and the interpreter needs no special case. `pad` makes the stride 16 on a
// 64-bit host, matching vt_slot_bytes' answer for a slice.
typedef struct {
    void *ptr;
    int   len;
    int   pad;
} VMHostBufSlot;

#ifndef VM_MAX_HOST_BUFS
#define VM_MAX_HOST_BUFS 4
#endif

// Memory backend abstraction: routes allocations through either
// an Arena (dynamic) or a fixed bump buffer (no-alloc).
typedef struct MemBackend {
    void *(*alloc)(struct MemBackend *m, size_t n);
    // backing: either an Arena* (dynamic) or a fixed buffer (no-alloc)
    Arena *arena;
    unsigned char *buf;
    size_t used, cap;
} MemBackend;

// Where print() output goes (vm_set_print_sink). `s` is NOT NUL-terminated
// and is valid only for the duration of the call; it carries no trailing
// newline -- the sink adds whatever line separator it wants.
typedef void (*VMPrintFn)(void *user, const unsigned char *s, int len);

// Where hover-inspection values go (vm_set_inspect_sink). `id` is the slot the
// compiler was handed in __ins(x, id).
//
// `kind` and `fx_shift` together are the operand's VMType: VMT_I32 with fx_shift
// 16 is `fx16`, whose value reads as a real number rather than an integer.
// `value` is ALWAYS f64 -- i32 and f32 both widen losslessly, and one number type
// keeps the host free of a per-kind branch. A fixed-point operand arrives already
// SCALED by 2^fx_shift. Called once per evaluation of the node.
//
// VMT_I64 is the one exception: `value` then carries the i64's BIT PATTERN, not
// its value, since a double holds only 53 bits of integer. Read it back through
// a union; never compare or print it as a number.
typedef void (*VMInspectFn)(void *user, int id, int kind, int fx_shift, double value);

// The same, for an array or slice operand (vm_set_inspect_array_sink). `kind` and
// `fx_shift` describe one ELEMENT -- VMT_I32 with shift 16 is an []fx16 -- and
// `vals` holds the first `n` of them, widened to f64 and already turned into the
// numbers they stand for (an fx element is divided by 2^fx_shift, as in the
// scalar sink), valid only for the duration of the call. `total` is the real length, so `n < total`
// says the tail was not delivered: the interpreter hands over at most
// VM_INSPECT_ARR_MAX elements, which is far more than a one-line label can show.
// Called once per evaluation of the node, like the scalar sink.
//
// Only unpacked i32/f32/f64/i64 aggregates report, i64 elements as bit patterns
// the way the scalar sink carries them. A packed one (a string, a []u8
// buffer, a struct record) is a byte view rather than a list of numbers.
typedef void (*VMInspectArrFn)(void *user, int id, int kind, int fx_shift,
                               int total, const double *vals, int n);
#define VM_INSPECT_ARR_MAX 64

// The id an unslotted `inspect(x)` reports under. The editor learns a pinned
// site's values by wrapping it -- __ins(inspect(x), N) -- so it never needs the
// inner call's report and its sink drops this id. A host that wants every
// inspect() in the source can accept it instead.
#define VM_INSPECT_ID_NONE (-1)

// A node the compiler wraps as if the source said __ins(node, id), without the
// source saying it (vm_set_inspect_probes).
typedef struct VMProbe { struct ASTNode *node; int id; } VMProbe;
#define VM_MAX_PROBES 32

// How the code emitters treat an IR_PRINT node. The default is DROP, so a
// script that prints still exports to a target with no print of its own.
typedef enum {
    VM_PRINT_EMIT_DROP = 0,  // the statement is not emitted at all
    VM_PRINT_EMIT_CALL       // emit a call the target may define; see vm.h
} VMPrintEmit;

// Configuration for vm_run_init.
typedef struct VmRunConfig {
    // Fixed arrays for no-alloc builds (NULL/0 = dynamic).
    Func        **funcs_by_id_buf;
    int           funcs_by_id_buf_cap;
#ifndef VM_NO_MATH
    CFuncEntry   *cfunc_table_buf;
    int           cfunc_table_buf_cap;
#endif
    unsigned char *mem_buf;        // MemBackend bump buffer
    size_t         mem_buf_cap;
} VmRunConfig;

// Runtime state shared by all Funcs in a VM instance.
typedef struct VmRun {
    Tsys     *sys;
    MemBackend mem;              // memory backend (arena or bump)
    Arena     arena;             // arena used when mem.alloc == arena_alloc
    Func     *funcs;             // head of compiled func list

    // Last error message (interned-ish; just a malloc'd cstr in arena).
    const char *last_error;

    // Per-run op budget. <0 means unlimited. Decremented by the
    // interpreter; on reaching 0 the run aborts with VM_BUDGET.
    long long budget;

#ifndef VM_NO_MATH
    // C-function lookup table, indexed by (token_id - TOK_MATHS_FIRST).
    // Allocated on the heap (sys->malloc/realloc/free), not the arena.
    CFuncEntry  *cfunc_table;
    int          cfunc_table_cap;  // current allocated capacity in entries
#endif

#ifndef VM_NO_CALL
    // Callee-frame bump pool: avoids large stack allocs inside eval_expr.
    // Each IR_CALL bumps callee_used by frame_size, runs the callee, then pops it back.
    unsigned char callee_pool[VM_CALLEE_FRAME_SIZE];
    int          callee_used;
    uintptr_t    stack_base;     // C stack address at the outermost run entry, 0 outside a run
#endif

    // Function ID lookup (O(1), for IR_CALL dispatch)
    struct Func **funcs_by_id;
    int          n_func_ids;
    int          cap_func_ids;

    // Last compile error position (0-based, -1 if unknown).
    int error_row;
    int error_col;

    // Opaque user data, set once by the host before func_run (vm_set_user_data).
    // Delivered to natives registered with register_c_func_*arg_ctx as a
    // leading void* argument (see CFuncEntry.wants_ctx).
    void *user_data;

    // ---- Shared variables ----
    // Backing store for every VMSym with is_global set. Borrowed, never freed
    // here: the host owns it and deliberately shares ONE block between several
    // VMs, which is the whole point -- separately compiled programs that agree on
    // a layout can then read and write each other's state.
    //
    // Being shared, it is also the one piece of VM state with no thread affinity
    // of its own: whoever binds it is asserting that every VM pointing at it runs
    // on one thread.
    //
    // NULL when the program declares no shared variables. A func that uses one
    // with no block bound fails the run rather than dereferencing NULL.
    void  *globals;
    size_t globals_size;
    // Hash of the layout `globals` was built for (see vm_globals_layout_hash).
    // Every VM sharing a block must agree; a mismatch means two programs
    // compiled against different declarations are about to overlay each
    // other's variables, which is silent corruption rather than a wrong answer.
    unsigned long long globals_hash;

    // ---- Host buffers ----
    // Storage for every VMSym with is_host_buf set: one {ptr,len} slot per
    // host-declared buffer id, addressed as `offset = id * sizeof slot`.
    //
    // Inline rather than a pointer, so an unbound buffer reads as {NULL,0} from
    // the zeroing at vm_create rather than needing a "no table bound" check on
    // every access. {NULL,0} then fails the ordinary bounds check with "array
    // index out of bounds", the right answer for a body that touches a buffer
    // this VM was never given. Compiled out with arrays: a host buffer is a
    // slice, so a VM_NO_ARRAYS build can never name one.
#ifndef VM_NO_ARRAYS
    VMHostBufSlot host_bufs[VM_MAX_HOST_BUFS];

    // ---- print() sink (vm_set_print_sink) ----
    // NULL (the zeroed state) makes IR_PRINT a no-op: the operand is still
    // evaluated, the bytes go nowhere. Inside the array guard because print()'s
    // operand is a []u8 slice, so a VM_NO_ARRAYS build can never build one.
    VMPrintFn print_fn;
    void     *print_user;
    // How the emitters render IR_PRINT (VMPrintEmit). Lives here rather than
    // on VM so func_emit_* can reach it through f->run.
    int       print_emit;
#endif

    // ---- hover-inspection sink (vm_set_inspect_sink) ----
    // NULL (the zeroed state) makes IR_INSPECT a pure pass-through. Its own guard
    // rather than the array one above: an inspected expression is a scalar, so
    // what rules it out is a target with no host to report to at all (see
    // VM_HAS_INSPECT in vm_config.h).
#if VM_HAS_INSPECT
    VMInspectFn inspect_fn;
    void       *inspect_user;
    // Aggregate operands report here instead; NULL leaves them pass-through.
    VMInspectArrFn inspect_arr_fn;
    void          *inspect_arr_user;
#endif
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((aligned(8)))
#endif
VmRun;

// A function bound to the VM (either compiled or being compiled).
typedef struct Func {
    struct VmRun *run;
    InternID   name;          // 0 if anonymous

    // Compile state
    VMSym     *syms;
    int        n_syms;
    int        cap_syms;
    // Name -> first sym with that name, open addressing, slot = index + 1.
    // Rebuilt with syms whenever it grows, from the same memory; NULL for a
    // small table or a deserialized Func, which sym_find then scans.
    int       *sym_index;
    int        sym_index_mask;
    int        n_hidden;

    // Parameters
    int        n_params;
    int        param_slot[VM_MAX_PARAMS];   // indices into syms[]

    // Default parameter values. The defaults are trailing (a param with a default
    // is never followed by one without), so n_defaults alone says which slots are
    // optional: params [n_params-n_defaults, n_params). The expressions stay in
    // the AST; params_ast is the callee's param list, walked by the caller's
    // compile_call to fill omitted args. Compile-time only, so it is not
    // serialized.
    int        n_defaults;
    struct ASTNode *params_ast;

    // `(a, ...rest: []i32)`: the LAST parameter gathers every argument past
    // the fixed ones. The call site collects them into a hidden array local
    // and passes it as an ordinary slice, so the callee's frame, its param
    // count and its ABI are exactly those of a plain one-slice parameter and
    // nothing at run time knows the function is variadic. Compile-time only,
    // like n_defaults; a rest parameter may not also carry a default.
    unsigned char has_rest;

    // NATIVES ONLY (params_ast is 0 for those, so the AST path above can't serve
    // them): bit i set means the value synthesized for an omitted param i is 1.0
    // as an fx16 constant instead of the plain 0 every other native default uses
    // -- see register_c_func_default_one. Compile-time only, like n_defaults.
    unsigned int native_default_one;

    // NATIVES ONLY: the parameter names as written in a signature,
    // "x0, y0, col" -- borrowed, host lifetime. For the editor's hints alone;
    // see register_c_func_param_names. Compile-time only, like n_defaults.
    const char *param_names;

    // NATIVES ONLY: its value means nothing (draw calls) -- see
    // register_c_func_no_value. For the editor's hints alone.
    unsigned char native_no_value;

    // Return
    VMType     ret_type;
    int        has_return;       // set true once we see a typed return

    // Frame
    size_t     frame_size;
    int        ret_offset;       // 0; result lives at frame[0]
    int        params_offset;    // bytes

    // Flat IR storage (index-based, serializable)
#if VM_COMPACT_NODES
    IRNodeC   *nodes;            // flat array of compact (8B) IR nodes
#else
    IRNode    *nodes;            // flat array of IR nodes owned by this function
#endif
    int        n_nodes;
    int        cap_nodes;
    int       *child_indices;    // child node indices (flattened items arrays)
    int        n_child_indices;
    int        cap_child_indices;

    // IR entry point
    int        body;             // node index into nodes[]

    struct Func *next;           // intrusive list inside VM
    int        func_id;          // assigned by VM, unique across all functions

    // Non-zero when this Func wraps a registered C function.
    // Value is the token ID (e.g. TOK_SIN); 0 means user-defined.
    int        native_tok;

    // The VM.flags directive state this body was compiled under, snapshotted at
    // the end of compilation with VM_FLAG_SNAPSHOT set. Emitters must read this
    // rather than VM.flags: the VM's live flag state is whatever the LAST body
    // compiled left it at, so a `#disable` in one body would otherwise decide
    // the code emitted for the next and emission order would change the output.
    // Zero (no VM_FLAG_SNAPSHOT) means no snapshot was taken -- a deserialized
    // Func -- and an emitter falls back to VM.flags for those.
    int        flags;

    // Polymorphic template support: body compiled on-demand per call-site type.
    int        is_template;       // 1 = no body; re-compiled per arg-type set
    // Compiled from library source (vm_lib.h) rather than from the unit's own
    // text. Compile-time only, never serialized. resolve_name's scan of every
    // Func ever compiled must skip these: the one that answers is whichever the
    // CURRENT unit built, and lib_attach owns that choice.
    unsigned char is_lib;
    // Library functions only: a C version the interpreter runs in place of this
    // specialisation's body (vm_declare_lib_func_accel, vm_lib_accel.c). On a
    // template it is the candidate; a spec gets it only when its parameters and
    // result share one float kind. Returns 0 to decline, and the body runs.
    // Compile-time only, never serialized, never read by an emitter.
    int (*accel)(struct Func *spec, unsigned char *frame);
    struct ASTNode *template_ast; // the => AST node (borrowed from parser); set on every script function
    struct Func **specs;          // arena array of compiled specialisations
    int        n_specs;
    int        cap_specs;

    // May a `const` initialiser CALL this at compile time? Derived from the body,
    // not declared: a script function is pure when it reads no shared variable and
    // no host buffer and calls nothing impure. Compile-time only, never
    // serialized; func_alloc memsets, so a deserialized Func arrives UNKNOWN and
    // is re-derived.
    //   0 = unknown, 1 = pure, 2 = impure, 3 = being walked, 4 = being compiled
    // The walk treats "being walked" as pure so a recursive function does not make
    // itself impure. "Being compiled" is refused instead: there is no finished
    // body to judge, and it cannot be inferred from `body`, which is legitimately
    // 0 for the first function compiled.
    unsigned char ct_purity;

    // The struct table every struct_id in this Func's types indexes. Compile-time
    // only, never serialized: a deserialized Func has no structs to name.
    struct VMStructTable *structs;

    // Name scoping, compile-time only. `scope` is the Func that stands for this
    // body: the template for a specialisation, itself otherwise. `lex_parent` is
    // the scope the `=>` was written in, 0 for the unit's top level.
    struct Func *scope;
    struct Func *lex_parent;

#if VM_REACTIVE
    // Literal provenance (vm_reactive.h): which AST node each IR_CONST_* /
    // IR_DATA_SLICE was compiled from, so a value flowing backwards can be
    // written into the text. Compile-time only, recorded while VM.rx_active.
    VMLitRef *lits;
    int       n_lits;
    int       cap_lits;
    // vm_compile_expr: the hidden slot the expression's value is stored in,
    // -1 for a void expression.
    int       rx_out_slot;
#endif
} Func;

#define CT_PURITY_UNKNOWN   0
#define CT_PURITY_PURE      1
#define CT_PURITY_IMPURE    2
#define CT_PURITY_WALKING   3
#define CT_PURITY_COMPILING 4

// IRNodeT/NODE_*: the interpreter (eval_expr/exec_stmt) is written once against
// these, so the same control-flow logic works unmodified whether Func.nodes is
// the classic 56B IRNode[] or the compact 8B IRNodeC[].
#if VM_COMPACT_NODES
typedef IRNodeC IRNodeT;

static inline float irc_payload_as_f32(int32_t payload) {
    union { int32_t i; float f; } u; u.i = payload; return u.f;
}
static inline int32_t irc_f32_as_payload(float f) {
    union { int32_t i; float f; } u; u.f = f; return u.i;
}

static inline IRNodeT *ir_child(struct Func *f, int idx) { return idx < 0 ? 0 : &f->nodes[idx]; }
static inline IRNodeT *ir_item(const struct Func *f, const IRNodeT *n, int i) { return &f->nodes[f->child_indices[IRC_A(n) + i]]; }

#define NODE_OP(n)          IRC_OP(n)
#define NODE_SUBOP(n)       IRC_SUBOP(n)
#define NODE_TYPE_KIND(n)   ((VTKind)IRC_TYPE(n))
#define NODE_A(n)           IRC_A(n)
#define NODE_B(n)           IRC_B(n)
#define NODE_C(n)           IRC_C(n)
#define NODE_KI(n)          ((long long)(n)->payload)
#define NODE_KF(n)          ((double)irc_payload_as_f32((n)->payload))
#define NODE_N_ITEMS(n)     IRC_B(n)
#define NODE_ITEMS_BEGIN(n) IRC_A(n)

#else
typedef IRNode IRNodeT;

// Resolve a child node or an items-array element to an IRNode pointer.
static inline IRNode *ir_child(struct Func *f, int idx) { return idx < 0 ? 0 : &f->nodes[idx]; }
static inline IRNode *ir_item(const struct Func *f, const IRNode *n, int i) { return &f->nodes[f->child_indices[n->items_begin + i]]; }

#define NODE_OP(n)          ((n)->op)
#define NODE_SUBOP(n)       ((n)->sub_op)
#define NODE_TYPE_KIND(n)   ((n)->type.kind)
#define NODE_A(n)           ((n)->a)
#define NODE_B(n)           ((n)->b)
#define NODE_C(n)           ((n)->c)
#define NODE_KI(n)          ((n)->ki)
#define NODE_KF(n)          ((n)->kf)
#define NODE_N_ITEMS(n)     ((n)->n_items)
#define NODE_ITEMS_BEGIN(n) ((n)->items_begin)
#endif

typedef struct VM {
    VmRun run;                 // runtime core

#ifndef VM_NO_COMPILER
    struct Parser   *parser;
    InternCtx      *intern;           // borrowed from parser
    VTKind          num_default_stack[16];
    int             num_default_depth;
    VTKind          type_map[8];         // directive: type rewire table (indexed by VTKind)
    int             type_shift[8];       // fixed-point shift applied when VMT_I32 is the target
    VTKind          literal_map[8];      // directive: literal rewire table
    int             literal_shift[8];
    DirectiveSnapshot dir_stack[VM_MAX_DIRECTIVE_STACK];
    int             dir_stack_depth;
    int             flags;               // bitmask of VM_FLAG_* (default VM_FLAG_CHECK_DIV_ZERO)
    InternOwner     builtin_owner;
    // Shared-variable layout this VM compiles against, resolved into THIS VM's
    // intern context (the portable VMGlobalTable carries names as text -- see
    // vm.h). NULL / 0 when the VM has none. Arena-allocated to the actual
    // count, so it follows the VM's lifetime and the caller's table may be a
    // stack temporary.
    VMGlobalRef    *globals;
    int             n_globals;
    // Owns the re-interned names above, and the names of any host-declared
    // constants (vm_declare_const_num) -- both are host-supplied text this VM
    // has to hold an id for until it is destroyed.
    InternOwner     globals_owner;
    // Compile-time constants declared by the unit being compiled, newest
    // first. Cleared by func_create -- see VMConst. Arena-allocated, so an
    // unused const list costs one null pointer.
    VMConst        *consts;
    // Constants declared by the HOST (vm_declare_const_num), newest first.
    // Searched after `consts` and, unlike it, kept for the VM's lifetime: no
    // compilation unit writes them, so re-compiling one is not a redefinition. A
    // unit declaring a `const` of the same name is still rejected.
    VMConst        *host_consts;
    // Host buffers this VM knows the NAME of (vm_declare_host_buffer), indexed by
    // id -- `declared` says whether the slot is in use. Kept for the VM's lifetime
    // for the same reason host_consts is. Separate from the run-side
    // VmRun.host_bufs, which holds the POINTERS: a VM may know a buffer's name
    // without being given one, and the pointer may be rebound between runs of an
    // already-compiled body. c_name / c_len_expr are borrowed host strings.
    struct {
        InternID    name;
        VMType      type;
        const char *c_name;
        const char *c_len_expr;
        int         declared;
        // vm_place_host_buffer: where it sits in a flat linear memory.
        int         mem_placed;
        int         mem_addr;
        int         mem_len;
    } host_bufs[VM_MAX_HOST_BUFS];
    // `#define`s in force, newest first. Cleared by func_create -- see VMDefine.
    VMDefine       *defines;
    // ---- Built-ins written in the language itself (vm_declare_lib_func) ----
    // Source text the compiler attaches to a unit only when a name resolves to
    // nothing there. Parsed once per VM (the AST is pointed at for as long as
    // the VM lives, so its ParseResult cannot be a per-unit arena); the template
    // Func is rebuilt once per unit, because a template snapshots the flags and
    // is keyed only on parameter types -- one kept across units would hand unit
    // B a body compiled under unit A's #rewire.
    struct VMLibFunc {
        InternID        name;
        const char     *src;        // borrowed from the host, VM lifetime
        // ParseResult*, kept as void* because it is a typedef of an anonymous
        // struct in parser.h and this header must not depend on that.
        void           *pres;       // parsed on first reference, or NULL
        struct ASTNode *arrow;      // the `(params) => body` inside pres
        struct Func    *tmpl;       // compiled for `unit`
        unsigned        unit;
        unsigned        masked;     // the unit whose own definition wins
        unsigned char   busy;       // being compiled right now (recursion guard)
        int           (*accel)(struct Func *spec, unsigned char *frame);   // or NULL
    } *lib;                         // heap, grown by vm_declare_lib_func
    int             n_lib;
    int             cap_lib;
    // Bumped wherever consts / defines / structs are cleared: identifies the
    // compilation unit a library template belongs to.
    unsigned        unit_id;
    // Non-zero while a library body is being parsed or compiled. Errors then
    // report the CALL SITE's position (lib_at) rather than an offset into text
    // the user cannot see, and name the built-in they came from.
    int             lib_depth;
    struct ASTNode *lib_at;
    InternID        lib_name;
    // The loop variables of the unrolled passes being compiled, innermost last.
    // compile_ident answers a read of one with its value for this pass. Keyed on
    // the Func and lib_depth, so a body compiled meanwhile -- a library template
    // attached from inside the loop -- does not see them.
    struct {
        InternID     name;
        struct Func *f;
        int          lib_depth;
        VMType       type;
        long long    ki;
        double       kf;
    } unroll[VM_UNROLL_MAX];
    int             n_unroll;
    // What a shared variable's name is prefixed with in emitted C, e.g. "inst."
    // so `sidechain` emits as `inst.sidechain`. Empty (the memset default)
    // means emit the bare name. Set per VM by the host, since which instance a
    // body's shared variables live in is the host's decision. See
    // vm_set_globals_c_prefix.
    char            globals_c_prefix[32];
    struct ASTNode **txt_to_ref;
    size_t          num_txt_to_ref;
    size_t         *line_offsets;
    int             num_lines_offsets;
    // Innermost statement/expression being compiled, so an error raised on a
    // node with no text of its own still reports a position. Only valid while
    // func_create is running; cleared on entry and exit.
    struct ASTNode *err_ctx;
    // C stack address at the outermost func_create, 0 outside one: the compiler
    // recurses per tree level and refuses a tree deeper than VM_MAX_COMPILE_STACK allows.
    uintptr_t       compile_stack_base;
    // How many blocks deep compile_block currently is, counting from the body of
    // the function being compiled: 1 is the body's own statement list, 2 and up
    // are inside an `if` / `while` / `for` / brace block. Reset per function
    // (compile_func_def, find_or_specialize), so a nested function literal's body
    // is depth 1 again. Only auto_const reads it -- it is the "runs
    // unconditionally" test. See VM_FLAG_AUTO_CONST.
    int             blk_depth;
    // How many times each name is WRITTEN in the compilation unit, counted off the
    // AST before anything is compiled (autoconst_scan). A name written once, at a
    // depth-1 `name = <folds to a constant>`, becomes a constant instead of a
    // local. Anything the scan cannot fit -- more distinct written names than the
    // table holds -- sets `ac_off` and turns the whole optimisation off for the
    // unit, which costs nothing but the storage it would have saved.
    // `scope` is the `=>` the first write sits in (0 = the unit's top level) and
    // `scopes` goes to 2 once a write turns up in a second one -- enough to tell
    // a body that a name it cannot see is somebody else's.
    struct { InternID name; int writes; struct ASTNode *scope; int scopes; } ac_names[VM_AUTOCONST_MAX];
    int             ac_count;
    int             ac_off;
    struct ASTNode *ac_scope;
    // The body whose names are being resolved: the template for a specialisation,
    // 0 for the unit's top level. A const is visible from the body that declared
    // it and, unless it is an auto-const, from the bodies written inside that
    // one. See const_visible.
    struct Func    *cur_scope;
    // Non-zero while a statement the COMPILER made up is being compiled --
    // a loop desugaring's `i = <start>`, an update operator's `x = x + 1`.
    // Those reach compile_stmt_into's declaration branch like any other
    // assignment, but the AST census never saw them, so auto_const must not
    // read a write count that does not account for the step that follows.
    int             ac_suspend;
    // Structs declared by the unit being compiled. Cleared by func_create --
    // scoped like `consts`, for the same reason -- and allocated on the first
    // declaration. Funcs keep the pointer; see VMStructTable.
    VMStructTable  *structs;
    // The table the shared variables' struct types index, kept from the unit
    // vm_declare_globals compiled -- the one place a struct-typed shared variable's
    // layout can be named from when emitting its C declaration.
    VMStructTable  *globals_structs;
    // Set when a shared variable could not be bound because its struct does not
    // match this unit's. Makes the error that says so stick: the lookup that failed
    // would otherwise be reported again, less usefully, as an unknown name.
    int             bind_failed;
#if VM_HAS_INSPECT
    // Copied in by vm_set_inspect_probes. probe_busy has bit i set while probe
    // i's node is being compiled, so a re-entry on the same node wraps once.
    VMProbe         probes[VM_MAX_PROBES];
    int             n_probes;
    unsigned int    probe_busy;
#endif
#if VM_REACTIVE
    // The host's resolver for names and component calls the unit does not
    // declare (vm_set_extern_resolver), and whether literal provenance is being
    // recorded. rx_func_depth counts how deep inside `=>` bodies the compiler is,
    // so the resolver can tell a statement's read from a function body's.
    VmExternNameFn  rx_name_fn;
    VmExternCallFn  rx_call_fn;
    void           *rx_user;
    int             rx_active;
    int             rx_func_depth;
    // Set when rx_name_fn returned -1: it has already said why the name did not
    // resolve, in terms only the engine knows ("defined outside this function"),
    // and the generic "unknown identifier" that follows would bury it. Cleared
    // at the top of every resolve_name, so it never outlives the lookup.
    int             rx_name_reported;
    // The backend in place before the host swapped in a per-statement arena
    // (vm_rx_use_arena). What outlives one statement -- a library built-in's
    // parse and template -- is allocated from it, not from the statement's.
    // rx_funcs_mark: the head of run.funcs when the outermost arena went in.
    MemBackend      rx_home_mem;
    int             rx_arena_depth;
    struct Func    *rx_funcs_mark;
#endif
#endif
} VM;

struct Args {
    Func  *func;
    void  *frame;
    size_t frame_size;
};

// Shared between vm.c (compiler) and vm_run.c (runtime).
struct Func *VM_API(func_alloc)(struct VmRun *run);
void         VM_API(func_register_id)(struct VmRun *run, struct Func *f);
void         VM_API(vm_register_builtins)(struct VM *vm);
void         VM_API(vm_run_register_builtins)(struct VmRun *run);

// In-place init (Phase 2).
void VM_API(vm_run_init)(VmRun *run, Tsys *sys, const VmRunConfig *cfg);
void VM_API(vm_run_deinit)(VmRun *run);
#ifndef VM_NO_COMPILER
void vm_init(VM *vm, Tsys *sys, struct Parser *parser);
#endif

// MemBackend helper.
static inline void *mem_alloc(MemBackend *m, size_t n) { return m->alloc(m, n); }

#endif
