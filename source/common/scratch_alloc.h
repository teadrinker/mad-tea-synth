#ifndef SCRATCH_ALLOC_H
#define SCRATCH_ALLOC_H

#include <stddef.h>
#include <stdint.h>

// ===================================================================
// Scratch (sidestack) bump allocator.
//
// Geometry/rasterizer code needs bytes-to-tens-of-KB of temporaries.
// Rather than stack locals (fatal on embedded, where a single deep call can
// hard-fault before its first statement runs) or permanent `static` arrays,
// a function takes (char *scratch, char *scratch_end) and bump-allocates via
// scratch_alloc(type, count). scratch is passed by value, so a callee only
// advances its own copy -- its temporaries are implicitly freed on return
// (LIFO reuse, no push/pop). The backing buffer itself is owned by the top
// of the call chain (font_lowspec_pebble.c's heap allocation on Pebble,
// glyph_render.c's static array on desktop); nothing in this header
// allocates it. scratch_alloc reads and writes locals literally named
// `scratch`/`scratch_end`, so every function using it declares parameters
// (or locals) with those exact names.
//
// Lives here rather than in glyph_render_core.h because it has nothing to do
// with glyphs -- common/render_polygon.h's gr_rasterize_scratch carves
// GrRasterBuffers with it the same way.
// ===================================================================

#ifndef SCRATCH_ALIGN
#define SCRATCH_ALIGN 8
#endif

static void *scratch_bump(char **scratch, char *scratch_end, size_t bytes) {
    uintptr_t p = (uintptr_t)*scratch;
    uintptr_t aligned = (p + (SCRATCH_ALIGN - 1)) & ~(uintptr_t)(SCRATCH_ALIGN - 1);
    char *start = (char*)aligned;
    char *next = start + bytes;
    if (next > scratch_end || next < start) return 0; // out of space, or bytes overflowed the pointer
    *scratch = next;
    return (void*)start;
}

#define scratch_alloc(type, count) \
    ((type*)scratch_bump(&scratch, scratch_end, sizeof(type) * (size_t)(count)))

#endif // SCRATCH_ALLOC_H
