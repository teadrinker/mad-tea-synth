#ifndef RENDER_CTX_H
#define RENDER_CTX_H

// ===================================================================
// RenderCtx -- the stateful layer above font/render_lowspec.h.
//
// render_lowspec's render_* primitives are stateless: each one takes an
// RSurface, a blend mode and a scratch range, and draws. Everything that has
// to PERSIST between two draws -- which surface is current, which font is
// selected, how text anchors, the off-screen images a body allocated -- lives
// here, in one struct the caller owns.
//
// LAYERING (three files, one direction):
//
//   render_lowspec.c   render_line / render_glyph_lowspec_ascii / ...
//                      stateless, takes an RSurface
//        ^
//   render_ctx.c       render_ctx_line_fx16 / render_ctx_font / ...
//                      stateful, takes a RenderCtx*, calls render_*
//        ^
//   vscreen.c          vscreen_line_fx16(...) -- per-platform provisioning
//   (per target)       plus one global-bound shim per primitive. The names
//                      the generated song.c calls.
//
// Nothing in render_lowspec.* may include this header or mention RenderCtx.
// That is what keeps a consumer with no screen from compiling or linking any
// of this: it is a separate translation unit,
// and it holds NO file-scope state of its own -- every instance is declared
// by whoever provisions it.
//
// This file also does not convert pixels: it builds RSurfaces, folds blend
// flags and forwards. The pixel encoding (RS_PEBBLE_TIME2, see
// common/render_surface.h) therefore stays decided in render_lowspec's TU,
// which is where both platforms already set it. Do not call rs_px_of /
// rs_gray_of from here.
// ===================================================================

// ==== API spec: what a script can call ====================================
// Registered on each VM by CodeSynthRegisterCFuncs / CodeSynthRegisterSmp
// (codesynth/CodeSynthParse.cpp) and bound per platform to the render_ctx_*
// entry points at the bottom of this header -- see apps/madteasynth/vscreen.h
// (desktop) and the Pebble template's song/vscreen.h.
//
// Each arg carries the ONE registered ABI type the VM coerces every call to:
//   i32       raw pixel / whole-number int (scalar_shift 0)
//   fx16      Q16.16 fixed-point -- the format this layer works in
//   fx16(int) a small int (col / ascii / blend) riding the fx16 channel; the
//             wrapper shifts it back down with >> FX16_SHIFT
//   slice     a "str" literal, reaching C as (const int* ptr, int len)
//   smp only  follows the sound's numeric type (f64 / f32 / fx22); no i32
//             variant, so smp(1,0) promotes to f64 rather than a shift-0 int
//
// Trailing [args] default to 0, EXCEPT every `alpha`, which defaults to 1.0 --
// alpha is literal coverage, so a 0 default would make circle(x,y,r) invisible.
// `visual` means the audio compile path does not register it. Everything but
// text() returns a value (the VM has no void type); only image_alloc,
// image_getpixel, image_sample and push_target return one worth reading.
//
//  putpixel(x:i32, y:i32, c:i32)                                       visual  one pixel to GColor8 c; off-screen dropped
//  point(x:fx16, y:fx16, amount:fx16, blend:fx16(int))                 visual  antialiased point; amount = coverage
//  line(x1,y1,x2,y2, stroke_width:fx16[, alpha:fx16, blend:fx16(int)]) visual  antialiased line; negative blend = triangle-fill backend (thick, no AA)
//  ellipse(x,y, radius_w,radius_h:fx16[, alpha, blend])                visual  filled; negative radius_w = wireframe; radius<=1px draws a point
//  circle(x,y, radius:fx16[, alpha, blend])                            visual  ellipse(radius,radius)
//  rect(x,y,w,h:fx16, col:fx16(int)[, alpha, blend])                   visual  filled w*h in GColor8 col; blend 0=screen, 1=inverted
//  background(col:fx16(int)[, transparency:fx16])                      visual  fill the whole target
//  glyph(x,y,size,stroke_width:fx16, ascii:fx16(int)[, alpha, blend])  visual  stroke one glyph (digits + capitals; lowercase folded)
//  text(x,y,size,stroke_width:fx16, "str":slice[, alpha, blend,
//       letter_spacing:fx16, line_height:fx16])                        visual  stroke a string, advancing x per glyph; the only void-returning call
//  font(id:i32)                                                        visual  0=default, 1=Idealist Hacker Mono, 2=Assembly Line (0 and 1 are the same font)
//  text_align(id:i32)                                                  visual  how text() anchors each line on x: 0/2=centre, 1=left, 3=right
//  image_alloc(w:i32, h:i32)                                           visual  w*h off-screen 8-bit GRAYSCALE image, zero-filled; id >= 1, or 0 if it did not fit
//  image_getpixel(image_id:i32, x:i32, y:i32)                          visual  0-255 gray; 0 for an unknown id or an off-image coordinate
//  image_sample(image_id:fx16(int), x:fx16, y:fx16)                    visual  image_getpixel, bilinear: fractional (x,y), and the RETURN is fx16
//  push_target(image_id:i32)                                           visual  redirect every later draw into that image; 1 if it took, 0 if not
//  pop_target()                                                        visual  restore the previous target; no-op when nothing is pushed
//  smp(rate, filter)                                                   audio   stream this voice's WAV sample at playback ratio `rate`
//
// `width` / `height` are registered alongside these (CodeSynthRegisterScreenConsts)
// as compile-time CONSTANTS, not calls: `width/2` folds to a number, `width = 5`
// is a compile error, and they are untyped, so the use site's #rewire retypes
// them -- an fx16 coordinate in circle(width/2, ...), a plain int in
// for(x=0; x<width; x++). They are what THIS export's screen is (emery 200x228,
// microw8 320x240; in the plugin, whichever target is selected). Because they
// fold, changing the screen size needs a recompile, which the plugin and the
// exporter both do.
//
// Coverage is composited in render_lowspec.c's render_*, not here and not in the
// platform wrappers. Every path honours `alpha`, and alpha <= 0 short-circuits
// before any geometry.
//
// `background` col is fx16(int) rather than a raw i32 so a fractional
// `transparency` can share the call -- the register_c_func_*arg ABI applies one
// shift to every arg. Integer colour literals round-trip unchanged.
//
// ---- off-screen images (image_alloc / push_target) -----------------------
// Every primitive draws into the CURRENT target: the screen, or the top of the
// target stack once a body has pushed one.
//
//     img = image_alloc(32, 32)
//     push_target(img)
//     background(0)                 // clears the image, not the screen
//     circle(16.0, 16.0, 10.0)      // draws into the image
//     pop_target()
//     putpixel(0, 0, image_getpixel(img, 16, 16))
//
// An image is FULL 8-BIT GRAYSCALE, not GColor8: 256 levels against the screen's
// 4 per channel, so the same circle() antialiases far more smoothly into one --
// which is the point of drawing through it. This layer ORs BLEND_8BIT_GRAYSCALE
// (common/render_surface.h) into the `blend` it hands each rasterizer, so no
// script asks for it. Two consequences:
//   - inside a push_target, a colour arg (putpixel's c, rect/background's col)
//     is a 0-255 GRAY, and image_getpixel returns one.
//   - to get an image back onto the screen, map that gray yourself:
//     putpixel(x, y, image_getpixel(img,u,v) * 3 / 255 * 0b010101).
//
// A push that cannot take -- unknown id, stack full -- returns 0 and swallows
// the draws up to its pop_target(), but is still PAIRED with that pop: the
// failure never unwinds an enclosing target, so a body that ignores the return
// value keeps drawing where it meant to.
//
// Images are FRAME-TEMPORARY and there is no image_free. They are bump-
// allocated, and render_ctx_images_reset() rewinds all of them before each
// visual body runs, dropping any target the previous body forgot to pop -- so
// an id means something only inside the body that allocated it. The space is
// finite and image_alloc returns 0 rather than growing it. Budget the PEBBLE
// side: there the images come off the front of the shared render sidestack,
// currently 512 B -- ONE 25x20 image, table capped at 2 -- against a 256 KB
// arena on the desktop.
// ==========================================================================

#include "font/render_lowspec.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct { unsigned char *pixels; int w, h; } RenderImage;

// Everything the caller provisions is BORROWED -- this struct owns nothing and
// frees nothing. Capacities are the caller's, which is what lets the desktop
// hand over 16 images / 8 target levels and Pebble hand over 2 / 2 without
// either paying for the other's ceiling, and without an allocator in here.
//
// Zero-initialise, then fill in: the screen (or leave it unbound), the scratch
// range, the image arena + table, and the target stack + flags. Anything left
// zero simply disables that feature: no scratch means the glyph paths draw
// nothing, no image table means image_alloc always returns 0.
typedef struct RenderCtx {
    // ---- output ----
    // The screen/framebuffer. pixels == 0 means UNBOUND: every draw that would
    // land here is dropped, which is what makes a stray call outside a render
    // pass harmless. Bind with render_ctx_bind_screen.
    RSurface screen;
    // Bumped by every draw that reached the screen (not by draws into an
    // image). A host that re-uploads a texture polls this to skip idle frames;
    // one that composites every frame anyway can ignore it.
    unsigned generation;

    // ---- renderer scratch (borrowed) ----
    // Bump range handed to render_line/render_ellipse/render_glyph_* as-is.
    char *scratch;
    char *scratch_end;

    // ---- Tier B blend cache (borrowed) ----
    // Memoizes the constant-coverage blend table that line/ellipse/rect fills
    // would otherwise rebuild per draw at ~3.5k cycles a time -- against ~5
    // cycles a filled pixel, so a thick line spends most of its cost on a table
    // it then discards. Persisting one here is the whole point, exactly like
    // the glyph cache below: nothing about it is per-draw.
    //
    // LEAVE IT 0 unless you need isolation -- which is what both platforms do.
    // 0 does not disable the cache; it selects the one render_lowspec's TU owns,
    // so nothing has to be provisioned for a target to get the speedup and no
    // target can silently lose it by forgetting to. Set this only to give a
    // renderer its own (the cache is not thread-safe): ask
    // render_blend_cache_bytes(), hand a block to render_blend_cache_init(),
    // keep the pointer.
    //
    // OPAQUE because its size depends on RS_PEBBLE_TIME2/RB_DITHER, which are
    // per-TU and which this file deliberately does not know (see the header note
    // above) -- only render_lowspec's TU may size it. Never freed, never reset,
    // never invalidated: the table is a pure function of the blend flags, the
    // colour and the coverage, and a push_target changes the flags, so a stale
    // entry is impossible rather than merely unlikely.
    struct RBlendCache *blend_cache;

    // ---- off-screen images (borrowed arena + borrowed table) ----
    char *image_arena;         // [image_arena, image_arena_end) is image_alloc's space
    char *image_arena_end;
    char *image_cursor;        // bumps up; 0 before the first reset (alloc re-checks)
    RenderImage *images;       // capacity image_cap
    int   image_cap;
    int   image_count;
    int   image_max_w;         // per-dimension ceiling; the arena is the real limit
    int   image_max_h;

    // ---- target stack (borrowed tables) ----
    // target_stack[i] is the surface at depth i+1, target_flags[i] its blend
    // flags. A NULL-pixels entry is the failed-push sentinel; target_overflow
    // counts pushes that could not nest at all. See render_ctx_push_target.
    RSurface *target_stack;    // capacity target_cap
    int      *target_flags;    // parallel array, same capacity
    int       target_cap;
    int       target_depth;
    int       target_overflow;

    // ---- draw state ----
    // The font glyph()/text() draw with, and how the caller maps a font() id
    // to one. `font` starts 0 = nothing selected yet: the first draw resolves
    // id 0 through font_resolve, so a song that never draws text never builds
    // a glyph cache. Which fonts exist, which of them are cached and with
    // whose allocator are all the caller's business -- see
    // render_ctx_build_font_cache below for the shared half of that.
    // font_resolve returns 0 for an id it does not know, which leaves the
    // current font selected.
    const FONT_LOWSPEC *font;
    const FONT_LOWSPEC *(*font_resolve)(void *user, int id);
    void *font_resolve_user;
    int text_align;                 // 0 = centre, 1 = left, 3 = right

    // ---- palette (borrowed) ----
    // The host's index -> pixel-word table, palette_len entries. A framebuffer
    // byte is an index into it; a script may also index it directly as
    // `palette[i]` and overwrite entries, which is exactly why it cannot be a
    // global: two renderers in one process (two plugin instances) must be able
    // to recolour independently.
    //
    // CARRIED, NEVER DEREFERENCED HERE. Nothing in render_ctx.c reads it, and
    // nothing may: a palette entry is an encoded pixel word, and this layer
    // deliberately does not know the encoding (see the header note at the top).
    // It lives here because it is per-renderer STATE and this struct is what
    // "one renderer" means -- whoever expands the framebuffer to RGBA, or binds
    // `palette` to a VM, reads it back off the context it is already holding.
    // 0 means the caller keeps its palette elsewhere; only that caller cares.
    int *palette;
    int  palette_len;
} RenderCtx;

// fb == 0 unbinds: every draw that would reach the screen is dropped, while
// draws into a pushed image still work.
void render_ctx_bind_screen(RenderCtx *c, const RSurface *fb);

// ---- drawing primitives ----
// The C entry points behind the script API above, in the same arg order. Each
// fx16(int) / blend arg arrives fx16-coerced and is shifted back down here;
// geometry and coverage stay raw fx16. All resolve the current draw target
// first and no-op when there is none, and all but render_ctx_text return 0.
int  render_ctx_putpixel_i32(RenderCtx *c, int x, int y, int col);
int  render_ctx_point_fx16(RenderCtx *c, int x, int y, int amount, int blend);
int  render_ctx_line_fx16(RenderCtx *c, int x1, int y1, int x2, int y2,
                          int stroke_width, int alpha, int blend);
int  render_ctx_ellipse_fx16(RenderCtx *c, int x, int y, int radius_w, int radius_h,
                             int alpha, int blend);
int  render_ctx_circle_fx16(RenderCtx *c, int x, int y, int radius, int alpha, int blend);
int  render_ctx_rect_fx16(RenderCtx *c, int x, int y, int w, int h, int col,
                          int alpha, int blend);
int  render_ctx_background_fx16(RenderCtx *c, int col, int transparency);
int  render_ctx_glyph_fx16(RenderCtx *c, int x, int y, int size, int stroke_width,
                           int ascii, int alpha, int blend);
// `str` is `len` bytes, one ASCII code each, not NUL-terminated -- the VM's
// []u8 slice ABI, so a script string literal arrives with no copy.
void render_ctx_text(RenderCtx *c, int x, int y, int size, int stroke_width,
                     const unsigned char *str, int len, int alpha, int blend,
                     int letter_spacing, int line_height);

// Asks font_resolve for `id`; an id it does not know leaves the current font
// selected.
int  render_ctx_font_i32(RenderCtx *c, int id);
// 0/2 centre, 1 = left (line starts at x), 3 = right (line ends at x). Any
// other id centres.
int  render_ctx_text_align_i32(RenderCtx *c, int id);

// ---- off-screen image targets ----
// Model (frame-temporary arena, no free, balanced push/pop) is in the
// "off-screen images" note above. All but image_sample are raw-i32.
//
// Returns the new id (>= 1), or 0 if w/h are non-positive or over
// image_max_w/_h, the arena is full, or image_cap images are already live.
int  render_ctx_image_alloc_i32(RenderCtx *c, int w, int h);
// 0-255 GRAY, not GColor8. Returns 0 for an unknown id or an off-image
// coordinate -- indistinguishable from a black pixel, which is what a fresh
// image is made of anyway.
int  render_ctx_image_getpixel_i32(RenderCtx *c, int image_id, int x, int y);
// Bilinear gray at a FRACTIONAL (x, y): Q16.16 in, Q16.16 out on the same
// 0-255 scale -- the fraction is what stops a slow gradient banding back to 256
// levels. A whole-pixel coordinate is one tap and exactly image_getpixel;
// otherwise it blends the four taps around it, reading 0 outside the image, so
// a sample straddling the border fades to black rather than clamping outward.
//
// `image_id` rides the fx16 channel (one shift applies to every arg) and is
// shifted back down here, like background's `col`.
int  render_ctx_image_sample_fx16(RenderCtx *c, int image_id, int x, int y);
// 1 if the id resolved and the stack had room, 0 otherwise -- but a 0 still
// consumes one pop_target, so every call needs its pop either way.
int  render_ctx_push_target_i32(RenderCtx *c, int image_id);
// `unused` exists only because the VM has no zero-arg native registration:
// pop_target() is registered 1-arg with that argument defaulted, so scripts
// still write `pop_target()`.
int  render_ctx_pop_target_i32(RenderCtx *c, int unused);
// Releases every image allocated since the last call and empties the target
// stack -- the host calls it before each visual body, which is what makes ids
// frame-temporary. A pointer rewind; safe when nothing was allocated.
void render_ctx_images_reset(RenderCtx *c);

// ---- glyph raster cache ----
// The shared half of every caller's font_resolve: builds the cache-enabled twin
// of `src` that glyph()/text() draw through -- one block holding the
// FONT_LOWSPEC, a writable settings copy, the levels and their blobs.
//
// Two levels, each one exact (cap_height, stroke_width) pair at whole pixel
// coordinates; any other size, or a fractional position, renders uncached. Blob
// sizes are measured: the full 95-glyph ASCII set costs 3132 B at cap 10 and
// 7477 B at cap 20, so both levels hold every glyph with room to spare, ~12 KB
// in total.
//
// The caller allocates and never frees -- the twin has to outlive every draw.
// Ask render_ctx_font_cache_bytes() how much, then pass the block here. mem ==
// 0, or a block too small, returns `src` itself: same pixels, uncached, so the
// result is always usable.
int render_ctx_font_cache_bytes(void);
const FONT_LOWSPEC *render_ctx_build_font_cache(const FONT_LOWSPEC *src,
                                                void *mem, int bytes);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // RENDER_CTX_H
