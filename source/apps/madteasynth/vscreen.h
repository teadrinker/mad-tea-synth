// Not VSCREEN_H: that is the screen height below.
#ifndef VSCREEN_H_INCLUDED
#define VSCREEN_H_INCLUDED

// A simulated Pebble Time 2 display (200x228, GColor8 0bAARRGGBB, 8bpp) that
// bodies draw into. This is the desktop binding of the API documented in
// font/render_ctx.h: it provisions storage and forwards each vscreen_* name to
// render_ctx_*. Behaviour belongs in font/render_ctx.c or render_lowspec.c.
// Small literals work as colours: putpixel(x, y, 3) is blue.

// FX16_SHIFT: the primitives' fixed-point format.
#include "common/math_fixedp.h"
#include "font/render_ctx.h"

#if defined(__cplusplus)
extern "C" {
#endif

// Starting screen size; an exported win32 project overrides it in song_config.h.
// VSCREEN_FRAMEBUFFER_ADDR makes the framebuffer borrowed: vscreen draws into
// the host's framebuffer at that address (microw8: 0x78). VSCREEN_W must then
// match the host's stride.
#ifndef VSCREEN_W
#define VSCREEN_W 200
#endif
#ifndef VSCREEN_H
#define VSCREEN_H 228
#endif

// The largest size the framebuffer holds, sizing the static array. The VST
// raises it so the preview can follow the selected export target.
#ifndef VSCREEN_MAX_W
#define VSCREEN_MAX_W VSCREEN_W
#endif
#ifndef VSCREEN_MAX_H
#define VSCREEN_MAX_H VSCREEN_H
#endif

// The `screen` and `palette` host buffers. Generated song.c names them directly,
// so they are part of the API. vscreen_screen is a macro so the address stays a
// link-time constant. Index with `stride`, not `width`. Unlike the primitives,
// they ignore push_target(), don't bump vscreen_generation(), and are unchecked
// outside the VM interpreter.
#ifdef VSCREEN_FRAMEBUFFER_ADDR
#define vscreen_screen ((unsigned char *)(VSCREEN_FRAMEBUFFER_ADDR))
#else
// Sized to the ceiling.
extern unsigned char vscreen_screen_storage[VSCREEN_MAX_W * VSCREEN_MAX_H];
#define vscreen_screen vscreen_screen_storage
#endif
#define VSCREEN_PIXELS vscreen_screen

// `screen.len` in emitted C: the start size, since exported targets never resize.
#define VSCREEN_SCREEN_LEN (VSCREEN_W * VSCREEN_H)

// 256 entries of 0xAABBGGRR (red in the low byte): the layout microw8's palette
// and the desktop DIB both use, so microw8 can alias it and the expand is a
// table lookup. Build entries with rgb(). Defaults to the Pebble colour cube.
// VSCREEN_PALETTE_ADDR borrows the host's palette, like VSCREEN_FRAMEBUFFER_ADDR.
#ifdef VSCREEN_PALETTE_ADDR
#define vscreen_palette ((int *)(VSCREEN_PALETTE_ADDR))
#else
extern int vscreen_palette[256];
#endif
#define VSCREEN_PALETTE_LEN 256

// Entry `i` of the default cube, shared by the static table and cart.c's startup fill.
#define VSCREEN_PALETTE_LEVEL(v)  (((v) & 3) * 85)
#define VSCREEN_PALETTE_DEFAULT(i)                     \
    ( VSCREEN_PALETTE_LEVEL((i) >> 4)                  \
    | (VSCREEN_PALETTE_LEVEL((i) >> 2) <<  8)          \
    | (VSCREEN_PALETTE_LEVEL(i)        << 16)          \
    | 0xFF000000 )

// rgb(r,g,b): an opaque palette entry. static inline since there's no context to
// bind; the Pebble binding has an identical copy. Ternaries, not paired ifs:
// Pebble builds with -Werror=misleading-indentation.
static inline int vscreen_rgb_i32(int r, int g, int b) {
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (int)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16) | 0xFF000000u);
}

// image_alloc() ceilings, capped at the screen.
#define VSCREEN_IMAGE_MAX_W VSCREEN_MAX_W
#define VSCREEN_IMAGE_MAX_H VSCREEN_MAX_H

// Corners of the GColor8 cube.
#define VSCREEN_BLACK  0x00
#define VSCREEN_BLUE   0x03
#define VSCREEN_GREEN  0x0C
#define VSCREEN_RED    0x30
#define VSCREEN_WHITE  0x3F

// Two bindings: vscreen_foo() on the file-scope context in vscreen.c, and
// vscreen_foo_ctx(RenderCtx*) on one you own. The flat form exists for exported
// song.c, one program with one screen. A host with several renderers (the VST,
// one screen per instance) must use the _ctx form and hand its context to the VM
// as user_data. The primitives' _ctx twins are in codesynth/vscreen_vm_bind.c.

// The file-scope context; static, never freed.
RenderCtx *vscreen_ctx(void);

// Block sizes vscreen_ctx_provision borrows, so callers can size their own
// storage identically.
#ifndef VSCREEN_GLYPH_SCRATCH_BYTES
#define VSCREEN_GLYPH_SCRATCH_BYTES (768 * 1024)
#endif
#ifndef VSCREEN_IMAGE_ARENA_BYTES
#define VSCREEN_IMAGE_ARENA_BYTES (256 * 1024)
#endif
#ifndef VSCREEN_MAX_IMAGES
#define VSCREEN_MAX_IMAGES        16
#endif
#ifndef VSCREEN_TARGET_STACK_MAX
#define VSCREEN_TARGET_STACK_MAX   8
#endif
// ~29 KB on 64-bit; too small (or 0) draws text uncached.
#ifndef VSCREEN_FONT_CACHE_BYTES
#define VSCREEN_FONT_CACHE_BYTES  (32 * 1024)
#endif

// Caller-owned storage plus the cached font built into it on the first text draw.
typedef struct VScreenFontCache {
    char               *mem;
    int                 bytes;
    const FONT_LOWSPEC *font;
} VScreenFontCache;

// Provisions `c` over caller-owned storage with a w x h screen (clamped). All
// blocks are borrowed for the context's life; the framebuffer must outlive every
// VM given it. `palette` gets the default cube; 0 leaves none. `font_cache` is
// per-context, since drawing fills it. The screen is not cleared.
void vscreen_ctx_provision(RenderCtx *c,
                           unsigned char *pixels, int w, int h,
                           int *palette, int palette_len,
                           char *scratch, int scratch_bytes,
                           char *image_arena, int image_arena_bytes,
                           RenderImage *images, int image_cap,
                           RSurface *target_stack, int *target_flags, int target_cap,
                           VScreenFontCache *font_cache,
                           char *font_cache_mem, int font_cache_bytes);

// VSCREEN_W * VSCREEN_H bytes, row-major, one GColor8 per pixel.
unsigned char *vscreen_pixels(void);
unsigned char *vscreen_pixels_ctx(RenderCtx *c);

// Read off the context, so `palette[i]` recolours one instance only.
int *vscreen_palette_ctx(RenderCtx *c);
int  vscreen_palette_len_ctx(RenderCtx *c);

// Bumped on every draw, so the UI uploads only on change.
unsigned int vscreen_generation(void);
unsigned int vscreen_generation_ctx(RenderCtx *c);

// For changes the primitives didn't make, like raw `screen[i]` writes.
void vscreen_touch(void);
void vscreen_touch_ctx(RenderCtx *c);

// The live size; the macros are only the default.
int vscreen_width(void);
int vscreen_height(void);
int vscreen_width_ctx(RenderCtx *c);
int vscreen_height_ctx(RenderCtx *c);

// Row pitch in bytes; equals the width everywhere this binding serves.
int vscreen_stride(void);
int vscreen_stride_ctx(RenderCtx *c);

// Clamped into 1..VSCREEN_MAX_W/H and cleared, since the stride changes. Returns
// 1 if the size changed. Refused for a borrowed framebuffer. Folded
// `width`/`height` need a recompile. Call between frames on the drawing thread.
int vscreen_set_size(int w, int h);
int vscreen_set_size_ctx(RenderCtx *c, int w, int h);

void vscreen_clear(int argb8);
void vscreen_clear_ctx(RenderCtx *c, int argb8);

// Expand to RGBA for display through the palette.
void vscreen_expand_rgba(unsigned int *dst, int dst_stride);
// With no palette, every pixel expands to 0.
void vscreen_expand_rgba_ctx(RenderCtx *c, unsigned int *dst, int dst_stride);

// VM-callable primitives, forwarding to render_ctx_*; see render_ctx.h.
int    vscreen_putpixel_i32(int x, int y, int c);
int    vscreen_point_fx16(int x, int y, int amount, int blend);
int    vscreen_line_fx16(int x1, int y1, int x2, int y2, int stroke_width, int alpha, int blend);
int    vscreen_ellipse_fx16(int x, int y, int radius_w, int radius_h, int alpha, int blend);
int    vscreen_circle_fx16(int x, int y, int radius, int alpha, int blend);
int    vscreen_rect_fx16(int x, int y, int w, int h, int col, int alpha, int blend);
int    vscreen_background_fx16(int col, int transparency);
int    vscreen_glyph_fx16(int x, int y, int size, int stroke_width, int ascii, int alpha, int blend);
void   vscreen_text(int x, int y, int size, int stroke_width,
                    const unsigned char *str, int len, int alpha, int blend, int letter_spacing, int line_height);
int    vscreen_font_i32(int id);
int    vscreen_text_align_i32(int id);

// ---- off-screen image targets ----
int    vscreen_image_alloc_i32(int w, int h);
int    vscreen_image_getpixel_i32(int image_id, int x, int y);
int    vscreen_image_sample_fx16(int image_id, int x, int y);
int    vscreen_push_target_i32(int image_id);
int    vscreen_pop_target_i32(int unused);
// Frees every image and empties the target stack; called before each visual
// body, which makes ids frame-temporary.
void   vscreen_images_reset(void);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // VSCREEN_H_INCLUDED
