// vm_reactive.h -- what the reactive engine (rvm/) needs from the compiler and
// the interpreter, and nothing else does. Every declaration here is compiled
// only under VM_REACTIVE (vm_config.h); a build without rvm/ never sees it.
//
// The model, in one paragraph: rvm/
// compiles ONE STATEMENT at a time into a scratch Func. A name the statement
// does not declare is resolved through the host's callback and becomes an
// EXTERN symbol -- an ordinary frame slot the host fills from a live value
// before running the body. The host then runs the body with vm_rx_exec, reads
// the published slot back with vm_rx_load_slot, and when a value flows
// backwards walks the IR with vm_rx_eval to recover sibling values, ending at
// an extern (write the live value) or a literal (write the text, through the
// provenance recorded in Func.lits).

#ifndef VM_REACTIVE_H
#define VM_REACTIVE_H

#include "vm/vm.h"

#if VM_REACTIVE

// ---- the host's resolver ----
// name_fn is asked for every identifier the unit does not declare, with
// in_func > 0 when the read sits inside a `=>` body (whose frame the host does
// not fill). call_fn is asked for a call whose callee name_fn marked is_comp:
// it instantiates the component and answers with the output's type and key, or
// returns 0 with an error set through vm's error path (or none, for a generic
// message). NULL clears.
void vm_set_extern_resolver(VM *vm, VmExternNameFn name_fn, VmExternCallFn call_fn, void *user);

// ---- the unit ----
// The session is the compilation unit. Call once when it starts and again when
// the whole program is rebuilt: clears consts, #defines and the struct table
// exactly as func_create does at the start of a body, and turns provenance
// recording on. The per-statement compiles below never clear them.
void vm_rx_begin_unit(VM *vm);
// Where error positions come from; the current parse, updated on every edit.
void vm_rx_set_source(VM *vm, ParseResult *pres);
// Sequential directive state, replayed by the host: reset to `flags` (the
// VM_FLAG_* set the session compiles under) with identity type maps and no
// #defines, then apply each directive statement that precedes the statement
// about to be compiled, in program order. apply returns 1 for a non-directive
// too, 0 with an error set when the directive is bad.
void vm_rx_directive_reset(VM *vm, int flags);
int  vm_rx_directive_apply(VM *vm, ASTNode *stmt);
// The default flag set a fresh VM compiles under.
int  vm_rx_default_flags(void);

// ---- per-statement compile ----
// The compiler's allocator, swapped for the length of one compile. Everything
// vm_compile_stmt / vm_compile_expr allocates -- the Func, its IR, its symbols,
// its literal provenance -- comes out of `a`, so a statement's code can be freed
// with the statement instead of living as long as the session. The host restores
// the returned backend afterwards; compiles nest (a call inside a statement
// instantiates its body), so save and restore rather than assume one level.
// A `const` / `struct` statement must NOT use this: what it registers into the
// unit's tables outlives it.
MemBackend vm_rx_use_arena(VM *vm, Arena *a);
void       vm_rx_restore_mem(VM *vm, MemBackend saved);

// `stmt` is one statement of the program (a `name = expr`, an expression, a
// `struct` / `const` declaration, an `f = (...) => ...`); the sub-block forms
// (`if`, `for`) are the host's own and never reach here. The Func's body is
// that one statement; extern reads are syms with is_extern set, the statement's
// own declaration is the sym named on its left. Frame laid out, nothing run.
// NULL on error (vm_last_error), like func_create.
Func *vm_compile_stmt(VM *vm, ASTNode *stmt);
// `expr` alone, stored into a hidden slot (Func.rx_out_slot; -1 when void).
Func *vm_compile_expr(VM *vm, ASTNode *expr);
// Undo what a removed declaration statement registered into the unit's tables.
// Structs keep their id (compiled Funcs index it); only the name is freed.
int  vm_rx_forget_const(VM *vm, InternID name);
int  vm_rx_forget_struct(VM *vm, InternID name);
// Every template's cached specialisations: a body compiled against a function
// binding that has since changed must be compiled again at its next call site.
void   vm_rx_forget_specs(VM *vm);

// ---- the interpreter, on a frame the caller owns ----
VMStatus vm_rx_exec(Func *f, unsigned char *frame, long long budget);
int      vm_rx_eval(Func *f, unsigned char *frame, int node, RV *out);     // 1 = ok
void     vm_rx_load_slot(Func *f, unsigned char *frame, int slot, RV *out);
int      vm_rx_store_slot(Func *f, unsigned char *frame, int slot, RV val);
int      vm_rx_binop(Func *f, int sub_op, VTKind k, RV a, RV b, RV *out);  // 1 = ok
RV       vm_rx_cvt(RV in, VTKind to);
int      vm_rx_elem_size(VTKind k);                            // bytes per element (or scalar)
int      vm_rx_agg_bytes(VTKind kind, int pack_bits, int n);   // bytes n elements occupy
int      vm_rx_slot_bytes(VMType t);

// ---- type helpers the compiler keeps private otherwise ----
int    vm_rx_shift(VMType t);                  // ct_shift: the fixed-point shift a type carries
VMType vm_rx_set_shift(VMType t, int shift);   // ct_set_shift
VMType vm_rx_clear_shift(VMType t);
VTKind vm_rx_promote(VM *vm, VTKind a, VTKind b);
const char *vm_rx_type_label(VMType t, int shift, char *buf, int buf_sz);
int    vm_rx_is_array(VTKind k);
int    vm_rx_is_slice(VTKind k);
VTKind vm_rx_arr_elem(VTKind k);

// ---- AST shape, as the statement compiler reads it ----
// The keyword head of a statement (`if`, `for`, `else`, `while`, `return`,
// `struct`, `const`, ...): the head identifier's token, with its arguments
// flattened as compile_stmt_into sees them and any sub-block last. 0 when the
// statement has no keyword head.
InternID vm_rx_kw_head(VM *vm, ASTNode *stmt, ASTNode **args, int *n, int max);
ASTNode *vm_rx_subscope_block(ASTNode *n);
ASTNode *vm_rx_unwrap_chain(ASTNode *n);
ASTNode *vm_rx_paren_inner(ASTNode *n);
int      vm_rx_is_block_node(ASTNode *n);
int      vm_rx_is_indent_block(ASTNode *n);
int      vm_rx_is_ident(ASTNode *n);
// The operand of a `...x` spread, or 0 when `n` is not one.
ASTNode *vm_rx_splat_operand(ASTNode *n);
// `x[i]` (not a range): the base `x`, with the subscript in *index; else 0.
ASTNode *vm_rx_index_base(ASTNode *n, ASTNode **index);
int      vm_rx_stmt_is_directive(ASTNode *stmt);
ASTNode *vm_rx_directive_indent_block(ASTNode *stmt);
int      vm_rx_expand_call_args(VM *vm, ASTNode *node, ASTNode *par, ASTNode **out, int max);
// A function literal `(a, b = 2) => body` read without compiling it: the
// parameter names (annotations dropped) and each default expression or NULL.
// Returns the count, or -1 with an error set when the list is not a parameter
// list. The body is what the compiler would compile.
int      vm_rx_arrow_params(VM *vm, ASTNode *arrow, InternID *names, ASTNode **defaults, int max);
ASTNode *vm_rx_arrow_body(ASTNode *arrow);
// `(params) => body`, possibly parenthesised: the `=>` node, else NULL.
ASTNode *vm_rx_as_arrow(ASTNode *n);
// Reports an error positioned at `at`, through the same path the compiler uses.
void     vm_rx_error_at(VM *vm, ASTNode *at, const char *msg);

#endif // VM_REACTIVE
#endif // VM_REACTIVE_H
