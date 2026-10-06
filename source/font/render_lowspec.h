#ifndef RENDER_LOWSPEC_H
#define RENDER_LOWSPEC_H

// Fixed-point (Q16.16) stroke-font renderer for low-spec targets (Pebble
// Time 2): same pipeline as glyph_render.c via glyph_render_core.h with
// RGC_FLOAT=fx16, reading straight from the packed word0/word1 table format.
// FONT_SETTINGS_LOWSPEC drops fields current fonts leave at zero.
//
// ARCHITECTURE -- this file (render_lowspec.c) is the ONE cross-platform
// implementation of the drawing primitives (render_line / render_point /
// render_rect / render_glyph* / render_text*). They are STATELESS: each takes
// an RSurface, a blend mode and a scratch range, and draws.
//
// Three layers, one direction:
//
//   render_lowspec.c   render_* -- stateless, takes an RSurface.  <- you are here
//   font/render_ctx.c  render_ctx_* -- the state that persists between draws
//                      (current target, off-screen images, selected font, text
//                      align), in one caller-owned RenderCtx. Calls render_*.
//   vscreen.c          vscreen_* -- per-platform provisioning (which
//   (per target)       framebuffer, how much scratch, what table sizes, which
//                      allocator) plus one shim per primitive. The names the
//                      generated song.c calls.
//
// Nothing here may include font/render_ctx.h or mention RenderCtx: a consumer
// with no screen must be able to compile this file without paying for one.
//
// ALL rendering behaviour -- new parameters, compositing, coverage/alpha --
// belongs in render_*, and everything stateful in render_ctx_*, NEVER in a
// platform wrapper. There are two of those wrappers and no compiler checks they
// agree: a param one drops (e.g. `(void)alpha`) simply never reaches the other
// platform. Thread it down instead, even if the render_* body only TODOs it for
// now.

#include "common/math_fixedp.h"
#include "common/render_polygon.h"

// ===== Glyph raster cache =====
//
// A lazy precalc, not a cache in the evicting sense: nothing is ever cleared
// or reused, glyphs are rasterized once into a bump-allocated blob and blitted
// from there afterwards. One FONT_CACHE_LOWSPEC covers exactly ONE rendering
// configuration -- a hit needs cap_height, stroke_width, flags, sub_x and
// sub_y to match the call, because any of them changing changes the pixels.
// (sub_x/sub_y are the sub-pixel remainders render_glyph_lowspec_ascii derives
// from the requested centre; they are 0 or 0.5 for integer coordinates, which
// is the case worth caching.)
//
// The width/height/sub_x/sub_y fields plus `owner` are resolved on the first
// call that matches the level's cap_height/stroke_width/flags -- a level is
// declared with just those three plus its memory, and claims the geometry
// (and the font) of the first caller that fits. (Under
// LOWSPEC_GLYPH_CACHE_WILDCARD a level declared with cap_height <= 0 claims
// the size too -- see render_glyph_lowspec_font_with_cache.)
//
// One level serves one font: `owner` is part of the match, because two fonts
// at the same size are different pixels. An app that switches between N fonts
// wants N levels per size, or it will only ever cache the first one to draw.
//
// Only the FILL path is cached. A wireframe glyph (negative stroke width /
// RG_FLAGS_RENDER_LINE_OUTLINE) always takes the plain render path: it strokes
// straight into the destination with no tile of its own, so there is nothing
// to snapshot.
//
// Per-glyph blob layout, reduced to the glyph's ink bounding box:
//   [0] row_count   rows that need drawing
//   [1] y_offset    gap from the tile's top edge to the box
//   then per row:
//   [0] x_offset    gap from the tile's left edge to this row's first pixel
//   [1] px_count    pixels in this row (0 for a blank row inside the box)
//     ceil(px_count/4) bytes of coverage, 2 bits per pixel, low pixel in the
//     low bits, tail padded with zero.
// (px_count is not in the sketch this was written from, but per-row x_offset
// implies per-row lengths -- without it a row's data has no end.)
typedef struct {
    fx16 cap_height;    // \ a hit needs all of these to match the call
    fx16 sub_x;         // |
    fx16 sub_y;         // |
    fx16 stroke_width;  // / (always positive -- wireframe is never cached)
    int  flags;         // and these too: RG_FLAGS_* change the geometry
    fx16 seen_cap_height;  // LOWSPEC_GLYPH_CACHE_WILDCARD bookkeeping only: the
                           // size seen once, claimed if it turns up again.
                           // Unused (and left 0) in a default build.

    int width;          // fx16_to_int(box_w) + 2 * LOWSPEC_GLYPH_PIXEL_PAD
    int height;         // fx16_to_int(box_h) + 2 * LOWSPEC_GLYPH_PIXEL_PAD

    char *mem_start;      // the start of the buffer
    int mem_end_offset;   // its size; running out falls back to a plain render
    int free_offset;      // offset from mem_start to memory still free.
                          // 0 == this level has not claimed its geometry yet
                          // (offset 0 is reserved so glyph_offset can use it
                          // as its "not yet calculated" value).
    short int *glyph_offset;  // per-glyph offsets from mem_start; 0 = not yet
                              // calculated, -1 = tried and did not fit
    int glyph_slots;          // entries in glyph_offset[]
    const void *owner;        // the FONT_LOWSPEC this level was claimed by
} FONT_CACHE_LOWSPEC;

// Define RGC_LOWSPEC_MOVE_DESC (project-wide) to add descend/move_desc support
// (fonts using GLYPH_FLAG_MOVE_DESC). Changes this struct's layout, so define
// it consistently everywhere this header is used.
typedef struct {
    fx16 radius, small_radius_mul, medium_radius_mul;
    // GLYPH_FLAG_WIDTH is 4-bit, so ids 0-15 can appear (real fonts use custom
    // ids beyond the four named GlyphWidth values). id 0 unused (= half_width).
    fx16 width_mul[16];
    fx16 half_width_settings_range_start, half_width_settings_range_end;
    fx16 glyph_box_hsize[2];
    fx16 glyph_box_center[2];
    fx16 glyph_box_default_hw;
    int  glyph_grid_size[2];
#ifdef RGC_LOWSPEC_MOVE_DESC
    fx16 descend;
    fx16 move_desc_x[8];
    fx16 move_desc_y[8];
#endif
    // Glyph raster cache levels (see FONT_CACHE_LOWSPEC). Zero = no caching,
    // which is what the generated font headers leave them at: their settings
    // are `static const`, and the levels are written to as glyphs are cached,
    // so they must live in RAM. render_glyph_lowspec_font_with_cache builds
    // the RAM copy that carries them.
    int cache_levels;
    FONT_CACHE_LOWSPEC *caches;
} FONT_SETTINGS_LOWSPEC;

// One entry of a generated font's per-glyph span table (the anonymous
// `{ int start; int count; }[]` the *_lowspec.h headers emit). Layout-
// identical, so callers cast their table pointer to this -- same house-style
// reinterpret as render_lowspec.c's `(const fixed*)contour`.
typedef struct { int start, count; } LowspecGlyphSpan;

// Bundles one *_lowspec.h font export's four pieces (packed segment data,
// per-glyph span table, glyph count, settings) so the ASCII layer below takes
// one pointer instead of four parallel arguments -- callers that switch fonts
// at runtime (see vscreen.h's font()) just swap which FONT_LOWSPEC they pass.
// The *_lowspec.h headers each emit a `static const FONT_LOWSPEC
// <prefix>_font` instance wired to their own _data/_info/_settings.
typedef struct {
    int glyph_count;
    const unsigned int *packed_data;
    const LowspecGlyphSpan *spans;
    const FONT_SETTINGS_LOWSPEC *settings;
} FONT_LOWSPEC;

// Render one glyph into a grayscale buffer, fixed-point only.
// packed_data/glyph_start/glyph_count: the glyph's segments in the packed
//   word0/word1 table (GLYPH_PACK_WORD0).
// strokewidth/scale/offset: as render_glyph(), all Q16.16.
// scratch/scratch_end: bump-allocator region for all geometry + rasterizer
//   buffers (no malloc/Tsys). Caller-owned; must fit this font's worst case
//   (font_lowspec_pebble.c documents it) -- undersizing drops parts of a glyph
//   rather than crashing.
// surf/org_x/org_y: destination, and where this glyph's width*height tile
//   lands in it. For a standalone tile, pass rs_tile(buf, width, height) at
//   (0, 0). NOTE: does NOT clear -- memset the buffer yourself if you want a
//   fresh tile. Clearing here would be wrong when blitting into a live
//   framebuffer, which is the case that must not break.
// blending_flags: coverage threshold (0-255); pixels below it are left
//   untouched, so the background shows through around the glyph. 0 = write
//   every tile pixel.
//   Ignored under RG_FLAGS_RENDER_LINE_OUTLINE (aaline blends over what is
//   already there rather than replacing it, so it never needs the threshold).
// alpha: Q16.16 coverage multiplier for the whole glyph. FX16_ONE (65536) is
//   full strength -- the historical behaviour -- values below it scale every
//   pixel's coverage down, and <= 0 draws nothing (returns 1: transparent is a
//   successful no-op, not a failure). Honoured on BOTH paths: the fill folds it
//   into gr_rasterize's coverage, the outline passes it to aaline_polygon.
//
// Careful: offsetX/offsetY (fx16) place the glyph *within the tile*, while
// org_x/org_y (int pixels) place *the tile within surf*. Two offsets, two
// coordinate spaces. Callers compose them: pass offset=(pad,pad) and
// org=(x-pad, y-pad) to land the glyph's own origin on screen at (x, y).
//
// Returns 0 if it ran out of scratch before drawing anything, 1 otherwise (an
// empty glyph counts as 1 -- nothing to draw is not a failure). Callers that
// just want pixels can ignore it; the raster cache can't, since it has to tell
// "this glyph is blank" from "this render never happened".
int render_glyph_lowspec(const unsigned int *packed_data,
                          int glyph_start, int glyph_count,
                          int width, int height,
                          const RSurface *surf, int org_x, int org_y,
                          int blending_flags, fx16 alpha,
                          const FONT_SETTINGS_LOWSPEC *settings,
                          fx16 strokewidth,
                          fx16 scaleCoordsX, fx16 scaleCoordsY,
                          fx16 offsetX, fx16 offsetY, int flags,
                          char *scratch, char *scratch_end);

// ===== ASCII text convenience layer =====
//
// One shared implementation of "draw an ASCII character/string with a lowspec
// stroke font", so the desktop binding (apps/madteasynth/vscreen.c) and the
// Pebble one do not each carry their own copy of the slot mapping and
// glyph-advance loop -- the parts most likely to drift apart. The
// per-target differences (which framebuffer, which flags -- Pebble must use
// RG_FLAGS_RENDER_LINE_OUTLINE, its scratch can't fit the fill path's GrPath)
// stay as plain arguments at the call site.


// Returns a cache-enabled twin of `src`, built inside the caller's `mem`
// block: the FONT_LOWSPEC, a writable copy of its FONT_SETTINGS_LOWSPEC, the
// level array, the per-level offset tables and the per-level blobs all live
// there, in that one allocation. Draw through the returned pointer to get
// caching; `src` and its const settings are never touched. Returns 0 if the
// arguments don't add up or the block is too small for even one level.
//
// The renderer owns no cache memory of its own -- no statics anywhere -- so
// sizing is entirely the caller's, changeable at runtime by building a new
// font over a new block. (It is also what keeps this off Pebble's .bss, which
// counts against the 64K virtual-size cap while the heap does not.)
//
// `levels` is a template array, read and copied: set cap_height, stroke_width,
// flags and mem_end_offset (= how many blob bytes that level should get) on
// each entry and leave the rest zero. Every level caches exactly the size it
// was declared with; a level declared with cap_height <= 0 is inert.
//
// Compiling render_lowspec.c with LOWSPEC_GLYPH_CACHE_WILDCARD instead
// makes cap_height <= 0 mean WILDCARD -- the level takes cap_height/
// stroke_width from the content, on the second sighting of a size rather than
// the first (a size drawn once is animation, and nothing is ever evicted, so
// claiming it would spend the level forever on something that can never be
// hit). Off by default: it is for content that picks its own sizes, and it
// trades the guarantee that a level covers a known size for that.
//
// `glyph_slots` is the offset-table length; it must be >= src->glyph_count
// (96 covers the full-ASCII fonts) or the whole call fails.
//
// Size the block with render_glyph_lowspec_cache_bytes(levels, level_count,
// glyph_slots) -- same arguments, so the two calls can't drift. Rough blob
// budget: a glyph costs about 2 + h*(2 + w/4) bytes at its ink bounding box
// w*h, so ~45 B per glyph at cap_height 10 and ~135 B at cap_height 20.
const FONT_LOWSPEC *render_glyph_lowspec_font_with_cache(const FONT_LOWSPEC *src,
                                                         const FONT_CACHE_LOWSPEC *levels,
                                                         int level_count, int glyph_slots,
                                                         void *mem, int bytes);

// Bytes render_glyph_lowspec_font_with_cache needs for exactly these levels.
// Valid for a block at ANY alignment -- the count includes the worst-case
// round-up the internal bump allocator does on the first carve. Do not "trim"
// that back out: without it, a block whose base is not SCRATCH_ALIGN-aligned
// (Pebble's heap hands out 4-aligned ones) comes up exactly those bytes short
// on the final carve, and the LAST declared level silently goes inert.
int render_glyph_lowspec_cache_bytes(const FONT_CACHE_LOWSPEC *levels,
                                     int level_count, int glyph_slots);

// Map an ASCII code to its slot: digits 0-9 -> 0-9, capitals A-Z -> 10-35,
// remaining ASCII 32-126 -> 36+ (lowercase folded to uppercase first).
// Returns -1 for codes with no glyph.
int glyph_lowspec_ascii_slot(int ascii);

// Stroke one ASCII glyph: resolves its span via glyph_lowspec_ascii_slot +
// `spans[glyph_count]`, then forwards to render_glyph_lowspec with a uniform
// `scale` on both axes and no coverage threshold. No-op for codes with no
// glyph. flags/scratch pass straight through.
//
// (x, y) is where settings->glyph_box_center lands in `surf` -- the glyph is
// centred on it, not hung off its corner -- and it is Q16.16 all the way
// through: the sub-pixel part rides in the glyph offset, so sliding (x, y)
// moves the glyph smoothly. The tile is sized here from `scale` and the
// font's glyph box (plus a pixel of pad each side), so there are no width/
// height arguments -- the caller does not pick the destination extent.
//
// A negative `strokewidth` means "wireframe": its magnitude is used and
// RG_FLAGS_RENDER_LINE_OUTLINE is added to `flags`. That is the mode Pebble
// needs (its scratch can't fit the fill path's GrPath), and it saves callers
// threading the flag separately.
//
// `blend` is the compositing mode, forwarded into render_glyph_lowspec's
// blending_flags slot (its legacy coverage-threshold meaning is unimplemented,
// so the slot is free). It is a set of BITS, not an enum: 0 = "screen" --
// coverage blends the pixel toward white, the historical behaviour;
// BLEND_INVERT (1) = coverage blends the pixel toward its own inverse, so a
// glyph reads white on a black background (identical to screen there, since
// 255-0 == 255) but black on a white one. Honoured by the
// desktop fill path (gr_rasterize); the outline path now forwards it to
// aaline_polygon as the aaline blend_option, which only consults it under
// AALINE_PLOT_4_X_64 -- elsewhere that path is still effectively screen.
//
// BLEND_LOCK_CH0 / _CH1 / _CH2 (2 / 4 / 8) join it in the same field and leave
// that channel of the destination untouched, counting from the low bits: on
// GColor8 blend = 2 | 4 writes red only. Unified-blend path only
// (USE_UNIFIED_BLEND), and ignored on a one-channel destination (8-bit gray or
// a single-channel RSurface::layout).
//
// That mode is only the LOW 16 BITS of `blend`. Above it sit flags describing
// the destination, currently just BLEND_8BIT_GRAYSCALE (common/render_surface.h,
// which owns the whole layout): with it set this draw treats the destination as
// full 0-255 grayscale rather than whatever the TU was compiled for, so a
// GColor8 build can render into an 8-bit offscreen image at full precision.
// Read the mode with rs_blend_inverted() / rs_blend_chanmask() -- comparing
// `blend` to 1 breaks the moment any other bit rides along -- and note that a
// glyph drawn with
// BLEND_8BIT_GRAYSCALE bypasses the raster cache, whose entries hold 2-bit
// coverage (lossless against GColor8, lossy against 256 levels).
//
// `alpha` is Q16.16 coverage for the whole glyph, and it is LITERAL: 0 (or
// less) draws nothing at all, FX16_ONE (65536) is full strength, and values
// between dim the glyph proportionally. Every path respects it: the fill folds
// it into
// gr_rasterize's coverage, the outline hands it to aaline_polygon, and a
// cached glyph gets it at blit time (the blob itself always stores
// full-strength coverage, since one blob serves every alpha).
//
// Note there is no "omitted means opaque" sentinel: 0 means invisible, so the
// SCRIPT-side default for an omitted alpha is 1.0, not 0 (see
// register_c_func_default_one in CodeSynthParse.cpp).
//
// `font` bundles packed_data/spans/glyph_count/settings (see FONT_LOWSPEC
// above) -- callers hold onto whichever *_lowspec.h font instance is
// currently selected and pass its address straight through.
void render_glyph_lowspec_ascii(const FONT_LOWSPEC *font,
                                const RSurface *surf,
                                fx16 strokewidth, fx16 scale,
                                fx16 x, fx16 y, int ascii, int flags,
                                char *scratch, char *scratch_end,
                                fx16 alpha, int blend);

// Stroke a run of `len` ASCII codes, advancing x by 0.9 em per character
// (em = settings->glyph_grid_size[0] cells, scaled by `scale`). '\n' (10)
// starts a new line, advancing y by the em height (glyph_grid_size[1]); the
// whole block is centred on (x, y) -- vertically over all lines, and each line
// horizontally over its own glyph count. Each character goes through
// render_glyph_lowspec_ascii, so a negative strokewidth means wireframe, and
// codes with no glyph leave a blank slot.
//
// `alpha` (per-glyph coverage) and `blend` (compositing mode) are passed to
// every glyph (see render_glyph_lowspec_ascii). alpha <= 0 skips the whole run.
// `letter_spacing` (fx16) is added to the per-glyph advance -- extra
// inter-glyph gap in pixels, 0 for the historical spacing, negative to tighten.
//
// `font` -- see render_glyph_lowspec_ascii above; forwarded unchanged to
// every glyph in the run.
//
// `align` selects each line's horizontal anchor relative to `x`: 1 = left
// (line's first glyph sits at x, text grows rightward), 3 = right (line's
// last glyph sits at x, text grows leftward), anything else (0, 2, ...) =
// centred on x, which is the default (see vscreen.h's text_align()).
// Vertical layout (line_y / block centring)
// is unaffected by `align`.
//
// `str` is a byte string: one ASCII code per byte, `len` bytes long, not
// NUL-terminated (the length is the only terminator). This matches the VM's
// []u8 slice ABI, so a script string literal reaches here with no conversion.
void render_text_lowspec_ascii(const FONT_LOWSPEC *font,
                               const RSurface *surf,
                               fx16 strokewidth, fx16 scale,
                               fx16 x, fx16 y, const unsigned char *str, int len,
                               int flags, char *scratch, char *scratch_end,
                               fx16 alpha, int blend, fx16 letter_spacing,
                               int align, fx16 line_height);

// ===== General-purpose polygon drawing =====
//
// Not tied to glyphs at all -- exposed here so other code sharing this TU
// (font_lowspec_pebble.c on Pebble, linked once into the final binary) can
// fill/stroke an arbitrary polygon without pulling in its own private copy
// of common/render_polygon.h / common/render_aaline.h. Every function in
// those headers is `static` (by design, so any TU can instantiate its own
// private copy at whatever POLY_MAX_EDGES/etc. it needs), which means two
// TUs that each #include them independently really do get two compiled
// copies of the same code -- a real cost on Pebble's 64KB binary cap.
// These two functions are this TU's ONE compiled instance instead.
//
// pts_xy: n *distinct* vertices, interleaved 16.16 fixed-point pairs
// (pts_xy[2*i], pts_xy[2*i+1]). Both auto-close (wrap the last vertex back
// to the first) -- do not repeat the first point.

// Scanline-fills into a width*height tile at (org_x, org_y) in `surf`.
// blending_flags: coverage threshold (0-255), see render_glyph_lowspec's own
// doc above. `alpha` is the Q16.16 coverage multiplier (FX16_ONE = full
// strength, <= 0 = nothing drawn), same convention as everything else here.
// scratch/scratch_end: bump allocator -- the polygon's GrPath is
// carved from this, not the stack (this TU's POLY_MAX_EDGES makes a stack
// GrPath ~4.8KB, well past the ~3KB frame that already hard-faulted this
// hardware -- see PEBBLE_GLYPH_SCRATCH_BYTES above).
void render_lowspec_fill_polygon(const RSurface *surf, const int *pts_xy, int n,
                                 int width, int height, int org_x, int org_y,
                                 int blending_flags, fx16 alpha,
                                 char *scratch, char *scratch_end);

// Draws the same vertices as AA line segments instead -- the wireframe twin.
// `alpha` (Q16.16 coverage, <=0 = nothing drawn) and `blend` (aaline
// blend_option, only consulted under AALINE_PLOT_4_X_64) are the same
// convention render_line uses. No scratch needed (no GrPath involved).
void render_lowspec_stroke_polygon(const RSurface *surf, const int *pts_xy, int n,
                                   int org_x, int org_y, fx16 alpha, int blend);

// ===== General-purpose line / rectangle drawing =====
//
// The same "this TU owns the one compiled instance" rationale as the polygon
// forwards above (see render_lowspec_fill_polygon's block): a single AA line
// primitive and a flat rectangle fill exposed for callers that share this TU
// rather than instantiating their own copy of render_aaline.h.

// ---- the Tier B blend cache ----
// Every primitive below that can reach a constant-coverage fill takes a
// `blend_cache`. It memoizes common/render_blend.h's Tier B table, which costs
// ~3.5k cycles to build (~14k under RB_DITHER) against ~5 cycles a filled
// pixel -- so a 60-150 px thick line spends most of its time on a table it
// then throws away. One cache that outlives the draws turns that into a
// three-int compare. See RBlendCache there for why the key needs no
// invalidation: the table is a pure function of (blend, col, coverage), and a
// push_target into a grayscale image changes the blend flags, hence the key.
//
// PASSING 0 IS THE NORMAL CASE and costs nothing: the primitives fall back to
// one cache owned by render_lowspec's TU, so a target gets the speedup without
// provisioning anything. Both platforms here do exactly that. Supply your own
// only to get ISOLATION -- a second renderer on another thread, say, since the
// cache is not thread-safe (and neither is sharing it wrong: the key is
// complete, so an entry either matches or misses).
//
// OPAQUE ON PURPOSE, and this is a trap rather than a style choice. The type is
// visible here -- render_surface.h pulls in render_blend.h transitively -- but
// sizeof(RBlendCache) depends on RB_DITHER, which depends on RS_PEBBLE_TIME2,
// which is decided PER TU (render_lowspec_lowmem.c sets it before including
// render_lowspec.c; render_ctx.c does not). So two TUs that both see
// this header can disagree about the size. Hold a POINTER. Do not embed one by
// value in anything two TUs share, and do not take its size outside
// render_lowspec's TU -- ask render_blend_cache_bytes() instead, the same
// arrangement as render_ctx_font_cache_bytes / render_ctx_build_font_cache.
struct RBlendCache;
// How many bytes render_blend_cache_init needs.
int render_blend_cache_bytes(void);
// Turns a caller-owned block of at least that many bytes into an empty cache
// and returns it, or 0 if `mem` is null or the block is short (in which case
// the caller simply passes 0 to the primitives and pays what it pays today).
// Borrowed, never freed, and safe to keep for the process's lifetime -- there
// is no reset to call and no state that goes stale.
struct RBlendCache *render_blend_cache_init(void *mem, int bytes);

// One line from (x1,y1) to (x2,y2), all coords Q16.16 buffer-space pixels (no
// org offset). A negative `stroke_width` selects the wireframe (outline) path;
// `alpha` (Q16.16 coverage: <= 0 draws NOTHING, FX16_ONE = full strength, in
// between dims proportionally) and `blend` (aaline blend_option, only consulted
// under AALINE_PLOT_4_X_64) apply per the two width paths:
//   * stroke_width <= 1px: hairline -- one antialiased segment; endpoint
//     coverage = stroke_width*alpha. scratch unused.
//   * stroke_width  > 1px: thick -- widened into a quad, then either stroked
//     via aaline_polygon (wireframe) or filled via gr_fill_polygon (needs bump
//     scratch; dropped if it runs out). alpha is honoured on both.
// A negative `blend` selects the alternate fill backend for that last case:
// triangle_fill_convex_polygon (common/render_triangle.h) instead of
// gr_fill_polygon -- no scratch needed at all, no AA (hard edges), alpha
// honoured. The sign only picks the backend; |blend| keeps its usual meaning
// (1 = inverted, else screen), so blend -1 is the triangle-backend twin of
// blend 1. It is consumed right here, alongside stroke_width's and radius_w's
// signs -- everything this function calls receives a non-negative mode, so the
// convention lives at this boundary and nowhere else. Flag bits above the mode
// (BLEND_8BIT_GRAYSCALE) survive the negation and reach the backend intact.
void render_line(const RSurface *surf, fx16 x1, fx16 y1, fx16 x2, fx16 y2,
                 fx16 stroke_width, fx16 alpha, int blend,
                 char *scratch, char *scratch_end, struct RBlendCache *blend_cache);

// A filled ellipse/circle, centred at (x,y) with radii radius_w/radius_h
// (circle: radius_w == radius_h == radius). Q16.16 throughout. The boundary is
// approximated as N scratch-allocated points (common/scratch_alloc.h) -- N
// scales with the larger radius (~pi points per pixel of it, roughly a 2px
// chord), clamped to [8, 64] to bound scratch usage and per-call cost -- then
// filled or stroked as one polygon, by the same size/sign rules as render_line:
//   * radius_w (and radius_h) magnitude <= 1px: too small to resolve a shape,
//     drawn as one antialiased point via aapixel (alpha/blend honoured, same
//     convention as render_point). alpha <= 0 draws nothing at all.
//   * radius_w < 0 (magnitude used for the geometry): wireframe -- the
//     boundary polygon is stroked via aaline_polygon, which takes both, so
//     alpha and blend are honoured.
//   * otherwise: filled via gr_fill_polygon (needs bump scratch; dropped if it
//     runs out), which scales its coverage by alpha -- or, if `blend` is
//     negative, by the triangle backend instead (see render_line's note: no AA,
//     |blend| keeps its meaning). The N boundary points still come from
//     scratch, so that path is cheaper but not scratch-free like render_line's.
// Only radius_w's sign selects wireframe vs. fill (radius_h's sign is ignored,
// its magnitude used) -- render_circle forwards its one radius as both, so the
// same rule covers circles.
void render_ellipse(const RSurface *surf, fx16 x, fx16 y, fx16 radius_w, fx16 radius_h,
                    fx16 alpha, int blend,
                    char *scratch, char *scratch_end, struct RBlendCache *blend_cache);

// render_circle(surf, x, y, radius, ...) is exactly render_ellipse(surf, x, y,
// radius, radius, ...) -- see there for the size/sign rules radius selects.
void render_circle(const RSurface *surf, fx16 x, fx16 y, fx16 radius,
                   fx16 alpha, int blend,
                   char *scratch, char *scratch_end, struct RBlendCache *blend_cache);

// One antialiased point at sub-pixel (x, y) (Q16.16 buffer-space, no org
// offset), splayed over its 4 neighboring pixels by bilinear coverage.
// `amount` is Q16.16 coverage (FX16_ONE = fully opaque), same convention as
// render_line's stroke_width-as-alpha at a hairline's endpoints. `blend` is
// the aaline blend_option, only consulted under AALINE_PLOT_4_X_64 (see
// common/render_aaline.h) and otherwise ignored.
void render_point(const RSurface *surf, fx16 x, fx16 y, fx16 amount, int blend);

// Fills the axis-aligned rectangle [x, x+w) x [y, y+h) (Q16.16, truncated to
// whole pixels) with the raw byte `col`, clipped to surf's clip rect. `col` is
// whatever one pixel byte means on the destination (a GColor8 value for a
// Pebble Time 2 framebuffer). `alpha` is Q16.16 coverage, same convention as
// render_line: <= 0 draws nothing, >= FX16_ONE is fully opaque, values between
// alpha-blend per pixel. `blend` selects the
// compositing mode, same convention as render_glyph_lowspec_ascii: 0 =
// "screen" (blend toward `col`), 1 = "inverted" (blend toward each pixel's own
// inverse, ignoring `col`). Blending is per channel of the destination's
// encoding (GColor8 under RS_PEBBLE_TIME2, else surf->layout, else one 8-bit
// gray channel); alpha>=FX16_ONE with blend==0 takes a fast path that writes
// `col` verbatim.
void render_rect(const RSurface *surf, fx16 x, fx16 y, fx16 w, fx16 h, int col, fx16 alpha,
                 int blend, struct RBlendCache *blend_cache);

// Sets n bytes at p to v -- the fill render_rect uses, and this library's
// memset: word-at-a-time, any alignment, no libc. n <= 0 does nothing.
void render_fill_bytes(unsigned char *p, unsigned char v, int n);

#endif // RENDER_LOWSPEC_H
