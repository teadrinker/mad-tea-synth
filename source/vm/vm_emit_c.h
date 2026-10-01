#ifndef VM_EMIT_C_H
#define VM_EMIT_C_H

#include "vm.h"

// EVERY entry point here returns a malloc'd, NUL-terminated C string, or NULL
// on allocation failure. Free it with the emitting VM's own allocator:
// vm->run.sys->free() or f->run->sys->free().

// All user-defined functions compiled in vm, template specialisations included.
// Native built-ins are referenced by name, not redefined.
char *vm_emit_c(VM *vm);

// File-scope declarations for vm's shared variables (vm_declare_globals in
// vm.h): the aggregate typedefs, a struct holding one field per variable, and
// one static instance of it.
//
// Call ONCE per translation unit, BEFORE any func_emit_c_named_* output that
// references a shared variable -- bodies name the instance, they do not
// declare it.
//
// A struct rather than loose variables is what lets a host declare several
// independent instances of one layout: call once per instance with a different
// `var_name`, and a distinct `type_name` per distinct LAYOUT (repeats of a
// type_name are include-guarded via the typedefs). Point each VM at its
// instance with vm_set_globals_c_prefix.
char *vm_emit_c_globals(VM *vm, const char *type_name, const char *var_name);

// One function, with no dependency walk. For inspection; vm_emit_c emits the
// whole program.
char *func_emit_c(Func *f);

// As func_emit_c, under `name_override` instead of the function's own interned
// name -- so several functions sharing a source name get distinct C identifiers.
// NULL name_override behaves as func_emit_c.
char *func_emit_c_named(Func *f, const char *name_override);

// As func_emit_c_named, but a param the body never referenced (func_param_used(),
// vm.h) is dropped from the emitted signature: a 4-param body reading only its
// third param emits as a 1-param C function. The body is unchanged -- it cannot
// reference a stripped param, which is what makes it strippable.
char *func_emit_c_named_stripped(Func *f, const char *name_override);

// ---------------------------------------------------------------------------
// Cross-call helper de-duplication
// ---------------------------------------------------------------------------
// A caller that concatenates many func_emit_c_named_* results into ONE translation
// unit normally gets a private copy of every helper each top-level function
// reaches, named "<top>__<helper>" so the copies do not collide. That is the right
// default for independent bodies -- but not when the same helper is deliberately
// shared, e.g. a prelude prepended to every body before compiling.
//
// Passing an EmitCDedup to func_emit_c_named_dedup switches helper naming from
// "<top>__<helper>" to "<helper>_<content hash>", where the hash covers the C text
// of the helper AND of everything it calls, with every function in that call
// closure renamed to its position in a deterministic walk. That makes the name a
// property of what the helper *does* -- identical helpers compiled into two
// different VMs hash identically, differing ones (including two specialisations of
// the same template) do not -- so the second and later copies can be referenced
// instead of re-emitted.
//
// Zero-initialise the struct once and pass the SAME one to every call whose output
// goes into that translation unit. Nothing is allocated or freed.
//
// The table remembers the C NAME each hash was first emitted under, not just the
// hash: identical helpers can reach here under different script names, and the
// second must be CALLED by the first one's name -- deriving "<own base>_<hash>"
// locally would declare a name nothing defines.
#define EMIT_C_DEDUP_MAX 512
#define EMIT_C_DEDUP_NAMES 16384                  // bytes of name text, all entries together
typedef struct {
    unsigned long long hash[EMIT_C_DEDUP_MAX]; // content hashes already emitted
    int                name_off[EMIT_C_DEDUP_MAX]; // hash[i]'s name, as an offset into names[]
    char               names[EMIT_C_DEDUP_NAMES];  // NUL-separated, never rewritten
    int                names_used;             // bytes of names[] in use
    int                count;                  // entries in use; further helpers stop deduping
} EmitCDedup;

// Like func_emit_c_named_stripped (strip_unused == 1) / func_emit_c_named
// (strip_unused == 0), but with the content-addressed, cross-call helper
// naming described above. Helpers new to `dd` are emitted in full and recorded;
// helpers already in `dd` are forward-declared and called, their definition
// left to the earlier chunk carrying it -- so the concatenated output holds
// exactly one definition of each distinct helper. `dd == NULL` behaves as
// func_emit_c_named/_stripped.
char *func_emit_c_named_dedup(Func *f, const char *name_override, int strip_unused,
                              EmitCDedup *dd);

#endif
