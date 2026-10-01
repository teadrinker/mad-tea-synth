#ifndef VM_EMIT_CURLYWAS_H
#define VM_EMIT_CURLYWAS_H

#include "vm.h"

// CurlyWas (https://github.com/exoticorn/curlywas) is a curly-braced, infix
// syntax for WebAssembly mapping roughly 1:1 onto wasm instructions; the
// `curlywas` tool compiles this output to a .wasm binary.
//
// Both return a malloc'd, NUL-terminated string (NULL on allocation failure);
// free it with the emitting VM's own allocator.

// All user-defined functions compiled in vm.
char *vm_emit_curlywas(VM *vm);

// One function, no dependency walk. For inspection.
char *func_emit_curlywas(Func *f);

#endif // VM_EMIT_CURLYWAS_H
