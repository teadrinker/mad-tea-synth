#ifndef VM_EMIT_SCRIPT_H
#define VM_EMIT_SCRIPT_H

#include "vm.h"

// One IR walker, two dialects: JavaScript and Lua 5.5. The front end has already
// resolved every type, so what the two targets need is not a token table but code
// for the five places their arithmetic disagrees with C's -- int wraparound,
// division and shift semantics, float width, truthiness, and fixed-array value
// semantics.
//
// EVERY entry point returns a malloc'd, NUL-terminated string, or NULL on
// failure. Free it with the emitting VM's own allocator (vm->run.sys->free).
// On failure vm_last_error(vm) says why -- an unsupported type names the
// `#rewire` that fixes it.
//
// What the two targets cannot represent, and refuse rather than approximate:
//
//   i64 in JS   -- there is no 64-bit integer without BigInt, which is viral
//                  and slow. `#rewire i64 -> f64` (exact to 2^53) or, for a
//                  fixed-point script whose i64 comes from vm.c's own fx
//                  mul/div lowering rather than from any declared type,
//                  `#rewire i64 -> i32`.
//   f32 in Lua  -- Lua has no single precision at any level, and the only
//                  faithful emulation (a string.pack round-trip per node)
//                  allocates twice per arithmetic op. `#rewire f32 -> f64`.
//   u1/u2/u4 slices -- a sub-word SLICE has no offset the emitted view can
//                  carry in element units. Fixed u1/u2/u4 ARRAYS are fine.
//
// Output shape:
//   - Self-contained. A prelude defining `VM`, then one function per compiled
//     Func, then an export line -- `module.exports = {...}` in JS,
//     `return {...}` in Lua. No imports, no bundler, no build step:
//     `node out.js` / `lua out.lua`.
//   - Only the helpers a module actually reaches are emitted. Each has its own
//     entry in SCRIPT_HELPERS (vm_emit_script_prelude.h) with its own
//     dependencies; the walk marks what it calls and the prelude is the
//     closure of that. `return 42` costs two lines.
//   - Aggregates. JS uses typed arrays throughout (Int32Array / Float32Array /
//     Float64Array / Uint8Array / Uint16Array) and a slice is a `subarray`
//     view, structurally identical to C's {ptr,len}. Lua uses 0-BASED tables
//     -- `{[0]=a, b, c}` -- and a slice is a `{a=, off=, len=}` view. Those
//     tables are deliberately NOT Lua sequences: `#`, `ipairs`,
//     `table.concat`, `table.unpack` and `table.sort` are all off-limits on
//     them, in emitted code and in any host function that receives one.
//   - Natives. A call to a registered C function emits `VM.<c_name>(...)`,
//     with the same variant name (f32/f64/i32/fx) the C backend would pick, so
//     a host adds its own natives by putting them on the same object. The
//     standard math set is defined by the prelude.
//   - Slice ABI for natives. A []T argument is passed as ONE value (the view /
//     subarray), not as C's (ptr, len) pair -- there is no pointer to fold an
//     offset into, so the pair shape cannot survive on either target. This is
//     host-facing: a native taking a slice sees one argument here and two in
//     emitted C.
//
// NOTE on transcendentals: the prelude maps sin/cos/exp/log/pow/sqrt/atan2 to
// the HOST's math library, not to common/math_pure.c's polynomial
// approximations, which is what the interpreter and emitted C both use. Those
// agree to a few ulp, not bit-for-bit. Everything else -- floor, frac, fmod,
// abs, min, max, clamp, mix, the step family, ipow -- is ported exactly.

// All user-defined functions compiled in vm, template specialisations
// included. Native built-ins are referenced by name, not redefined.
char *vm_emit_js(VM *vm);
char *vm_emit_lua(VM *vm);

// Module-scope storage for vm's shared variables (vm_declare_globals), under
// `var_name`. Emit ONCE, before the functions that name one -- bodies read
// `<prefix>x` (vm_set_globals_c_prefix), they do not declare it. Pass the
// prefix without its trailing '.': a VM set to "G." wants var_name "G".
char *vm_emit_js_globals(VM *vm, const char *var_name);
char *vm_emit_lua_globals(VM *vm, const char *var_name);

#endif
