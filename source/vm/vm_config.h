// vm_config.h — Internal VM build flags (single source of truth).
//
// vm/ only ever reads the VM_*-prefixed flags documented below; it has no
// knowledge of any higher-level naming convention. An embedder with its own
// flag vocabulary should translate into these in its own adapter header,
// reached before this file.
//
// Naming convention:
//   VM_NO_COMPILER       — strip compiler/authoring code (parser, intern, etc.)
//   VM_NO_SERIALIZE      — strip the serialize writer (device never writes blobs)
//   VM_SKIP_F32          — omit f32 interpreter cases
//   VM_SKIP_F64          — omit f64 interpreter cases
//   VM_SKIP_I64          — omit i64 interpreter cases
//   VM_CALLEE_FRAME_SIZE — stack-poison guard for call frames
//   VM_COMPACT_NODES     — switch Func.nodes from IRNode (56B) to IRNodeC (8B)
//
// The derived VM_HAS_F32 / VM_HAS_F64 / VM_HAS_I64 macros are 0/1 constants
// usable in conditionals.

#ifndef VM_CONFIG_H
#define VM_CONFIG_H


// ---- Legacy vm_run-only aliases (pre-date the VM_* naming above; kept for
// out-of-tree callers that still set them). Must run before the VM_HAS_*
// derivation below, since that reads VM_SKIP_*. Remove once no caller sets
// them, in favour of VM_NO_COMPILER/VM_SKIP_*/VM_CALLEE_FRAME_SIZE directly.
// Old name                 -> New name
// VM_RUN_ONLY              -> VM_NO_COMPILER (+ VM_NO_SERIALIZE implied)
// VM_RUN_SKIP_F32          -> VM_SKIP_F32
// VM_RUN_SKIP_F64          -> VM_SKIP_F64
// VM_RUN_SKIP_I64          -> VM_SKIP_I64
// VM_RUN_CALLEE_FRAME_SIZE -> VM_CALLEE_FRAME_SIZE

#ifndef VM_NO_COMPILER
#ifdef VM_RUN_ONLY
#define VM_NO_COMPILER  1
#endif
#endif

#ifndef VM_NO_SERIALIZE
#ifdef VM_RUN_ONLY
#define VM_NO_SERIALIZE 1
#endif
#endif

#ifndef VM_SKIP_F32
#ifdef VM_RUN_SKIP_F32
#define VM_SKIP_F32
#endif
#endif
#ifndef VM_SKIP_F64
#ifdef VM_RUN_SKIP_F64
#define VM_SKIP_F64
#endif
#endif
#ifndef VM_SKIP_I64
#ifdef VM_RUN_SKIP_I64
#define VM_SKIP_I64
#endif
#endif

#ifndef VM_CALLEE_FRAME_SIZE
#ifdef VM_RUN_CALLEE_FRAME_SIZE
#define VM_CALLEE_FRAME_SIZE  VM_RUN_CALLEE_FRAME_SIZE
#endif
#endif

// ---- Derived type-availability constants ----

#ifndef VM_HAS_I64
#ifdef VM_SKIP_I64
#define VM_HAS_I64  0
#else
#define VM_HAS_I64  1
#endif
#endif

#ifndef VM_HAS_F64
#ifdef VM_SKIP_F64
#define VM_HAS_F64  0
#else
#define VM_HAS_F64  1
#endif
#endif

#ifndef VM_HAS_F32
#ifdef VM_SKIP_F32
#define VM_HAS_F32  0
#else
#define VM_HAS_F32  1
#endif
#endif

// ---- Hover inspection (IR_INSPECT, vm_set_inspect_sink) ----
// Off wherever VM_NO_ARRAYS is, which marks a size-constrained device profile: the
// sink is a host facility no such target has. Tied to VM_NO_ARRAYS rather than set
// per build script so those profiles get it for free.
#ifndef VM_HAS_INSPECT
#if defined(VM_SKIP_INSPECT) || defined(VM_NO_ARRAYS)
#define VM_HAS_INSPECT  0
#else
#define VM_HAS_INSPECT  1
#endif
#endif

// Default size of the whole callee-frame pool -- the interpreter's call stack,
// and with it the ceiling on any single called function's frame. Hosts hand the
// ENTRY function its frame themselves, so this only ever bounds what is reached
// through a call; a host that wraps a body in a generated function and calls it
// therefore has to fit that body in here, and 2048 is well under the frame
// buffers those hosts pass their entry points. Keyed on the size-constrained
// device profile the same way VM_HAS_INSPECT is, so those keep the small pool
// (and the ones that set VM_NO_CALL have no pool at all).
#ifndef VM_CALLEE_FRAME_SIZE
#ifdef VM_NO_ARRAYS
#define VM_CALLEE_FRAME_SIZE  2048
#else
#define VM_CALLEE_FRAME_SIZE  16384
#endif
#endif

// Ceiling on the C stack the interpreter may use below a run's entry point. The
// callee pool bounds recursion by frame bytes, but each VM call level also costs
// several C frames (more in debug builds, more again under __ins), so a deep
// recursion with a small frame overflowed the host's stack before the pool
// filled. Checked on every call; 0 disables it.
#ifndef VM_MAX_C_STACK
#ifdef VM_NO_ARRAYS
#define VM_MAX_C_STACK  0
#else
#define VM_MAX_C_STACK  (512 * 1024)
#endif
#endif

// The same ceiling for the compiler, which recurses per tree level and per
// nested specialisation at roughly 4KB a level. Just inside a default 1MB
// thread stack, so nothing that compiled before is refused. 0 disables it.
#ifndef VM_MAX_COMPILE_STACK
#ifdef VM_NO_ARRAYS
#define VM_MAX_COMPILE_STACK  0
#else
#define VM_MAX_COMPILE_STACK  (896 * 1024)
#endif
#endif

// ---- Public-symbol decoration, so vm_run.c's body can be compiled a second
// time (under a different macro configuration) into the same binary without
// colliding with the default-named copy. Empty by default: every existing
// caller of vm_create/func_run/args_bind/etc. is unaffected. A second driver
// (e.g. vm_run_lowspec.c) defines VM_API_SUFFIX to a distinct token (e.g.
// _lowspec) before including vm_run.h / vm_types.h / vm_run.c, and gets
// distinctly-named symbols instead.
#ifndef VM_API_SUFFIX
#define VM_API_SUFFIX
#endif
#define VM_API_PASTE_(base, suf) base##suf
#define VM_API_PASTE(base, suf)  VM_API_PASTE_(base, suf)
#define VM_API(name) VM_API_PASTE(name, VM_API_SUFFIX)

// ---- Compact in-memory IR nodes ----
#ifndef VM_COMPACT_NODES
#define VM_COMPACT_NODES 0
#endif

// ---- Reactive engine hooks (rvm/) ----
// Extern symbols resolved through a host callback, literal provenance and the
// per-statement compile entry points that rvm/ lowers from. Default off: a build
// that does not list rvm/ compiles exactly what it compiles today. See
// vm/vm_reactive.h.
#ifndef VM_REACTIVE
#define VM_REACTIVE 0
#endif

// Auto-selected device-tuned sub-variant: only valid when the build already can't
// produce IR_INDEX/IR_CALL/IR_CALL_STMT/IR_ARR_LIT nodes and only VMT_VOID/VMT_I32
// are live. A profile that enables i64 no longer qualifies and falls back to the
// general compact layout below. Not user-set: it falls out of flags the build sets
// for other reasons.
#if VM_COMPACT_NODES && defined(VM_NO_ARRAYS) && defined(VM_NO_CALL) \
    && !VM_HAS_F32 && !VM_HAS_F64 && !VM_HAS_I64
#define VM_COMPACT_NODES_DEVICE 1
#else
#define VM_COMPACT_NODES_DEVICE 0
#endif

#endif // VM_CONFIG_H
