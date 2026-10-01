#include "render_lowspec.h"
#include "glyph_render.h"     // GLYPH_FLAG_*, GLYPH_UNPACK_*, GLYPH_CAP_*/GLYPH_WIDTH_*, RG_FLAGS_IGNORE_GRADE_FIT

// ===================================================================
// Fixed-point instantiation of glyph_render_core.h.
// ===================================================================

typedef struct { fx16 x, y; } FVec2;

#define RGC_FLOAT       fx16
#define RGC_MUL(a,b)    fx16_mul(a,b)
#define RGC_DIV(a,b)    fx16_div(a,b)
#define RGC_SQRT(a)     fx16_sqrt(a)
// fx16 override: the default RGC_SQRT(dx*dx+dy*dy) overflows once |dx|/|dy| >
// ~181 (routine for the axis-snap cap extension) -- see fx16_hypot.
#define RGC_HYPOT(dx,dy) fx16_hypot(dx,dy)
#define RGC_SIN(a)      fx16_sin(a)
#define RGC_COS(a)      fx16_cos(a)
#define RGC_ACOS(a)     fx16_acos(a)
#define RGC_ATAN2(y,x)  fx16_atan2(y,x)
#define RGC_ABS(a)      fx16_abs(a)
#define RGC_CONST(lit)  fx16_from_float(lit)
#define RGC_FROM_INT(i) fx16_from_int(i)
// Ceil to int for RGC_BEZIER_ADAPTIVE: floor(v + (1 - eps)) in Q16.16.
#define RGC_CEIL_INT(v) fx16_to_int((v) + (FX16_ONE - 1))
#define RGC_FX(v)       (v)   // already Q16.16, no conversion needed
// The float default RGC_CONST(1e-6f) truncates to 0 in Q16.16, disabling every
// divisor guard (and integer div-by-0 crashes, unlike float). 16 (~0.00024) is
// above the fixed-point noise floor but only trips on degenerate geometry.
#define RGC_EPS         16

#include "glyph_render_core.h"
#include "common/render_aaline.h"
#include "common/render_triangle.h"
#include "common/render_convex.h"

// Which rasterizer fills a CONVEX shape -- render_line's thick quad and
// render_ellipse's boundary n-gon, the only two fills in this file whose
// outline is convex by construction.
//
// 1 (default): common/render_convex.h. Same antialiasing, no active-edge
//   table, no per-sub-scanline sort, no scratch at all, and the fully covered
//   interior of every row goes through the cached Tier B table at one indexed
//   load per pixel instead of through a coverage accumulation.
// 0: the previous path, gr_fill_polygon. Kept as a one-line escape hatch --
//   the two agree to within 1 level on 8 pixels in 4.6M (tests/
//   render_convex_test.c quantifies it), so flipping this is a fair A/B.
//
// The GLYPH fill is untouched either way: its outlines are arbitrary, often
// concave and self-intersecting, which is exactly what gr_rasterize's winding
// rules are for.
#ifndef RENDER_USE_CONVEX_FILL
#define RENDER_USE_CONVEX_FILL 1
#endif

// x4/y4 (as packed, i.e. coordinate*4) -> Q16.16: (x4<<16)/4 == x4<<14,
// exact (no rounding loss -- 16 - 2 = 14 bits of headroom always available).
static fx16 unpack_coord_fx(unsigned int w0, int is_y) {
    int v4 = is_y ? GLYPH_UNPACK_Y4(w0) : GLYPH_UNPACK_X4(w0);
    return (fx16)((int32_t)v4 << (FX16_SHIFT - 2));
}

// fx16 counterpart of glyph_render.c's stroke_subpath_outline: grade-fit box
// scale, width interpolation and per-point pts/flags/hw from packed words, then
// the shared core builds the outline into out_contour[]. Pure geometry -- the
// caller feeds the result to gr_add_contour for fill.
static int stroke_subpath_outline_lowspec(FVec2* out_contour, int max_contour,
                                        const unsigned int *packed_data,
                                        int start, int end,
                                        fx16 half_width, const FONT_SETTINGS_LOWSPEC *settings,
                                        fx16 scale_x, fx16 scale_y,
                                        fx16 ox, fx16 oy, int flags,
                                        char *scratch, char *scratch_end) {
    fx16 center_scale = scale_y;

    fx16 box_center_x = settings->glyph_box_center[0];
    fx16 box_center_y = settings->glyph_box_center[1];
    fx16 box_default_hw = settings->glyph_box_default_hw;
    // A zero scale collapses the whole outline to a point, and fx16_div is a
    // raw integer divide -- so this would be a hard divide-by-zero crash, not
    // an inf. Nothing to stroke either way.
    if (scale_x == 0 || scale_y == 0) return 0;
    fx16 glyph_hw = fx16_div(half_width, scale_x);

    if (!(flags & RG_FLAGS_IGNORE_GRADE_FIT)) {
        fx16 box_scale_y = fx16_div(settings->glyph_box_hsize[1] - glyph_hw,
                                     settings->glyph_box_hsize[1] - box_default_hw);

        scale_x = fx16_mul(scale_x, box_scale_y);
        scale_y = fx16_mul(scale_y, box_scale_y);
    }

    fx16 uniform_scale = scale_y;

    fx16 global_radius        = settings->radius;
    fx16 global_small_radius  = fx16_mul(settings->small_radius_mul,  global_radius);
    fx16 global_medium_radius = fx16_mul(settings->medium_radius_mul, global_radius);

    // Interpolate width multipliers: lerp(1, mul, t), t = clamp01((hw - range_start) / (range_end - range_start))
    fx16 hw_range = settings->half_width_settings_range_end - settings->half_width_settings_range_start;
    fx16 hw_t = (hw_range > fx16_from_float(0.001f))
                    ? fx16_div(glyph_hw - settings->half_width_settings_range_start, hw_range)
                    : FX16_ONE;
    if      (hw_t < 0)         hw_t = 0;
    else if (hw_t > FX16_ONE)  hw_t = FX16_ONE;

    fx16 wmul[16];
    wmul[0] = half_width;
    for (int i = 1; i < 16; i++)
        wmul[i] = fx16_mul(FX16_ONE - hw_t + fx16_mul(hw_t, settings->width_mul[i]), half_width);

    int n_orig = end - start;
    if (n_orig < 2) return 0;

    unsigned int w1_start = packed_data[start * 2 + 1];
    unsigned int w1_end   = packed_data[(end - 1) * 2 + 1];
    int start_cap = GLYPH_FLAG_CAP((int)w1_start);
    int end_cap   = GLYPH_FLAG_CAP((int)w1_end);

    fx16 scaled_radius        = fx16_mul(global_radius, uniform_scale);
    fx16 scaled_small_radius  = fx16_mul(global_small_radius, uniform_scale);
    fx16 scaled_medium_radius = fx16_mul(global_medium_radius, uniform_scale);

    FVec2 pts[GLYPH_MAX_SEGS];
    int pt_flags[GLYPH_MAX_SEGS];
    for (int i = 0; i < n_orig && i < GLYPH_MAX_SEGS; i++) {
        unsigned int w0 = packed_data[(start + i) * 2];
        unsigned int w1 = packed_data[(start + i) * 2 + 1];
        fx16 x = unpack_coord_fx(w0, 0);
        fx16 y = unpack_coord_fx(w0, 1);
        int  seg_flags = (int)w1;

#ifdef RGC_LOWSPEC_MOVE_DESC
        int move_id = GLYPH_FLAG_MOVE_DESC(seg_flags);
        if (move_id != 0) {
            x += fx16_mul(settings->move_desc_x[move_id], settings->descend);
            y += fx16_mul(settings->move_desc_y[move_id], settings->descend);
        }
#endif
        // move_grid is intentionally unsupported (see FONT_SETTINGS_LOWSPEC comment).

        pts[i].x = ox + fx16_mul(scale_x, x - box_center_x) + fx16_mul(center_scale, box_center_x);
        pts[i].y = oy + fx16_mul(scale_y, y - box_center_y) + fx16_mul(center_scale, box_center_y);
        pt_flags[i] = seg_flags;
    }

    fx16 pt_hw[GLYPH_MAX_SEGS];
    for (int i = 0; i < n_orig; i++) {
        // GLYPH_FLAG_WIDTH is a 4-bit field, always in [0,15] -- matches wmul[16].
        int wv = GLYPH_FLAG_WIDTH(pt_flags[i]);
        pt_hw[i] = wmul[wv];
    }

    return stroke_build_outline(out_contour, max_contour,
                         pts, pt_flags, pt_hw, n_orig, start_cap, end_cap,
                         scaled_radius, scaled_small_radius, scaled_medium_radius, half_width,
                         scratch, scratch_end);
}

// ===================================================================
// Public API
// ===================================================================

int render_glyph_lowspec(const unsigned int *packed_data,
                          int glyph_start, int glyph_count,
                          int width, int height,
                          const RSurface *surf, int org_x, int org_y,
                          int blending_flags, fx16 alpha,
                          const FONT_SETTINGS_LOWSPEC *settings,
                          fx16 strokewidth,
                          fx16 scaleCoordsX, fx16 scaleCoordsY,
                          fx16 offsetX, fx16 offsetY, int flags,
                          char *scratch, char *scratch_end) {
    // Fully transparent: nothing to draw, and nothing went wrong -- 1, not 0,
    // so the raster cache doesn't mistake it for a scratch shortfall.
    if (alpha <= 0) return 1;

    fx16 half_width = strokewidth >> 1;

    // Wireframe mode strokes each contour as it is assembled and never
    // touches the GrPath/rasterizer; fill accumulates every contour into one
    // path first, because the winding rule needs all of them at once.
    int line_outline = (flags & RG_FLAGS_RENDER_LINE_OUTLINE) != 0;

    // From scratch, not static/stack (GrPath.edges alone is ~320 KB at the
    // 16384-edge default). Layout: [GrPath | contour | per-subpath temps].
    // GrPath lives across both the edge loop and the rasterize pass; the
    // contour and temps after it are reclaimed, and rasterize reuses that
    // tail (raster_scratch, just past GrPath).
    char *cur = scratch;
    GrPath *path = (GrPath*)scratch_bump(&cur, scratch_end, sizeof(GrPath));
    if (!path) return 0;
    gr_path_init(path);
    char *raster_scratch = cur;

    FVec2 *contour = (FVec2*)scratch_bump(&cur, scratch_end,
                                          sizeof(FVec2) * (size_t)RGC_OUTLINE_MAX);
    if (!contour) return 0;

    int end_all = glyph_start + glyph_count;
    int i = glyph_start;
    while (i < end_all) {
        unsigned int w0 = packed_data[i * 2];
        if (!GLYPH_UNPACK_MOVE(w0)) { i++; continue; }
        int start = i;
        i++;
        while (i < end_all && !GLYPH_UNPACK_MOVE(packed_data[i * 2])) i++;
        int end = i;

        if (half_width > fx16_from_float(1.0f / 256.0f)) {
            int n = stroke_subpath_outline_lowspec(contour, RGC_OUTLINE_MAX,
                                        packed_data, start, end,
                                        half_width, settings,
                                        scaleCoordsX, scaleCoordsY,
                                        offsetX, offsetY, flags,
                                        cur, scratch_end);
            // (const fixed*)contour: FVec2 here is {fx16 x, y} -- two
            // adjacent int32_t, no padding -- so it's exactly the interleaved
            // Q16.16 pair layout aaline_polygon expects (common/render_
            // aaline.h). RGC_FX is identity in this (fixed-point) TU.
            // blending_flags carries the compositing mode on this path (see
            // render_glyph_lowspec_ascii's note on the slot), so it doubles as
            // aaline's blend_option; `alpha` is the per-draw coverage
            // multiplier, which aaline_polygon takes directly.
            if (line_outline) aaline_polygon(surf, (const fixed*)contour, n, org_x, org_y,
                                             alpha, blending_flags);
            else              gr_add_contour(path, contour, n);
        }
    }

    if (!line_outline)
        return gr_rasterize_scratch(path, width, height, surf, org_x, org_y, blending_flags,
                                    alpha, raster_scratch, scratch_end);
    return 1;
}

// ===== ASCII text convenience layer (see render_lowspec.h) =====

int glyph_lowspec_ascii_slot(int ascii) {
//    if (ascii >= 'a' && ascii <= 'z') ascii -= 32;
    if (ascii >= 32 && ascii <= 126) return ascii - 32;
    return -1;
}

// Breathing room (whole pixels, all four sides) between the scaled glyph box
// and the tile edge, so the stroke's antialiased fringe lands inside the tile.
#define LOWSPEC_GLYPH_PIXEL_PAD 1
#define FONT_SPACE_PAD_X 1 // this should actually be in the font metrics...
#define FONT_SPACE_PAD_Y 1 // this should actually be in the font metrics...
#define FONT_SPACE_DEFAULT_LETTER_SPACING 2 // this should actually be in the font metrics...

// ===== Glyph raster cache (see FONT_CACHE_LOWSPEC in the header) =====
//
// Lazy precalc: the first time a glyph is drawn at a cached configuration it
// is rasterized into a scratch tile, squeezed to its ink bounding box at 2
// bits of coverage per pixel, and appended to the level's blob. Every later
// draw of it is the blit below -- no geometry, no GrPath, no scanline sweep.
// Nothing is ever evicted; when the blob fills up the remaining glyphs just
// keep taking the plain render path.
//
// Why a tile and not "snapshot what landed on screen": the cached bytes must
// be pure coverage, independent of whatever was underneath. Rendering onto a
// zeroed tile gives exactly that -- gr_rasterize's screen blend against a
// black destination is gray + a*(255-0)/255 == a -- and the blit then
// re-composites that coverage over the real destination.

// glyph_offset is a short, so an offset has to fit in one -- and -1 is taken
// as the "does not fit" marker, so cap a level's usable memory below that.
#define LOWSPEC_GLYPH_CACHE_MAX_BYTES 32767

// The biggest glyph tile this build may carve out of the caller's scratch to
// rasterize a cache entry into. This is a PROMISE ABOUT THE SCRATCH, not a
// memory limit: the tile comes off the front, so the render behind it runs
// with that many bytes less than a plain render would have had -- and on a
// target whose scratch is sized exactly, less scratch means silently
// degraded geometry that would then be cached. Targets like that add this many
// bytes to their scratch and set this to match; the default is past the
// largest tile a byte-sized width*height can produce, i.e. no limit, which is
// right for a desktop-sized scratch.
#ifndef LOWSPEC_GLYPH_CACHE_MAX_TILE_BYTES
#define LOWSPEC_GLYPH_CACHE_MAX_TILE_BYTES 65536
#endif

// Every byte the cache uses is the caller's: the levels, their offset tables
// and their blobs all hang off FONT_SETTINGS_LOWSPEC (see
// render_glyph_lowspec_font_with_cache below). Nothing here is static, which
// is what makes sizes changeable at runtime -- and keeps this off the Pebble
// .bss budget without a per-target #ifdef.

// Pick the level covering this exact draw, claiming an unused one if its
// declared cap_height/stroke_width/flags are what we are drawing. A level
// claims the sub-pixel phase of integer coordinates only -- which is 0 or a
// half pixel purely by the tile's parity, since the phase is frac(coord -
// size/2) -- so scrolling/animated text at fractional positions never takes a
// level hostage. Returns 0 when this draw is not cacheable.
//
// Under LOWSPEC_GLYPH_CACHE_WILDCARD a level declared with cap_height <= 0
// takes its size from the content instead, on the SECOND sighting: a size
// drawn once and never again is animation (a song sweeping cap_height or
// stroke width per frame), and claiming that would spend the level forever on
// something that can never be hit. Each wildcard remembers one candidate size;
// an unmatched size goes into the first wildcard with no candidate yet, or
// over the last wildcard's if they all have one. So a few stable sizes settle
// into their own levels even when drawn interleaved, while pure animation
// churns the last candidate and claims nothing. Off by default -- without it
// a level only ever caches the size it was declared with, and one declared
// with cap_height <= 0 is simply inert.
static FONT_CACHE_LOWSPEC *lowspec_cache_find(const FONT_LOWSPEC *font,
                                              fx16 cap_height, fx16 stroke_width,
                                              int flags, fx16 sub_x, fx16 sub_y,
                                              int width, int height) {
    FONT_CACHE_LOWSPEC *caches = font->settings->caches;
    int levels = font->settings->cache_levels;
    if (!caches) return 0;

    // row_count/y_offset/x_offset/px_count are single bytes.
    if (width <= 0 || width > 255 || height <= 0 || height > 255) return 0;
    // Checked before a level can be claimed, not just before a store: a size
    // whose tile we will never be able to rasterize must not take a wildcard
    // level hostage.
    if (width * height > LOWSPEC_GLYPH_CACHE_MAX_TILE_BYTES) return 0;

#ifdef LOWSPEC_GLYPH_CACHE_WILDCARD
    // Where an unrecognized size gets remembered if no level takes this draw:
    // the first wildcard still without a candidate, else the last one.
    FONT_CACHE_LOWSPEC *free_wild = 0, *last_wild = 0;
#endif

    for (int i = 0; i < levels; i++) {
        FONT_CACHE_LOWSPEC *c = &caches[i];
        if (c->flags != flags) continue;
        if (!c->mem_start || !c->glyph_offset || c->mem_end_offset < 2 ||
            font->glyph_count > c->glyph_slots) continue;

#ifdef LOWSPEC_GLYPH_CACHE_WILDCARD
        if (c->free_offset == 0 && c->cap_height <= 0) {          // wildcard
            if (c->seen_cap_height != cap_height || c->stroke_width != stroke_width) {
                if (!free_wild && c->seen_cap_height == 0) free_wild = c;
                last_wild = c;
                continue;                       // not this level's size (yet)
            }
            // Second sighting -- fall through and claim it.
        } else
#endif
        if (c->cap_height != cap_height || c->stroke_width != stroke_width) {
            continue;
        }

        if (c->free_offset == 0) {          // unclaimed: this call fixes it
            if (sub_x != ((width  & 1) ? FX16_HALF : 0) ||
                sub_y != ((height & 1) ? FX16_HALF : 0)) continue;
            if (c->mem_end_offset > LOWSPEC_GLYPH_CACHE_MAX_BYTES)
                c->mem_end_offset = LOWSPEC_GLYPH_CACHE_MAX_BYTES;
            c->cap_height   = cap_height;   // no-ops unless this is a wildcard
            c->stroke_width = stroke_width;
            c->owner  = font;
            c->width  = width;
            c->height = height;
            c->sub_x  = sub_x;
            c->sub_y  = sub_y;
            c->free_offset = 1;             // offset 0 == "not yet calculated"
            for (int g = 0; g < c->glyph_slots; g++) c->glyph_offset[g] = 0;
            return c;
        }
        if (c->owner == font && c->sub_x == sub_x && c->sub_y == sub_y &&
            c->width == width && c->height == height) return c;
    }

#ifdef LOWSPEC_GLYPH_CACHE_WILDCARD
    // No level covers this draw. Remember the size so a repeat of it can claim
    // a wildcard (see the note above); this draw itself renders uncached.
    FONT_CACHE_LOWSPEC *rec = free_wild ? free_wild : last_wild;
    if (rec) {
        rec->seen_cap_height = cap_height;
        rec->stroke_width    = stroke_width;
    }
#endif
    return 0;
}

// ---- Building a cached font out of one caller-owned block ----------------
//
// The block holds everything: the FONT_LOWSPEC, its writable settings copy,
// the level array, each level's offset table and each level's blob, in that
// order. One allocation, one pointer to keep, and the caller can re-do it at
// a different size whenever it likes -- the previous block simply stops being
// used.
// See the header for the caller's side of this.

// Rounds up to the alignment scratch_bump uses, so every carved struct lands
// on an address the target is happy to dereference.
static int lowspec_cache_align(int n) {
    return (n + (SCRATCH_ALIGN - 1)) & ~(SCRATCH_ALIGN - 1);
}

int render_glyph_lowspec_cache_bytes(const FONT_CACHE_LOWSPEC *levels,
                                     int level_count, int glyph_slots) {
    if (level_count <= 0 || glyph_slots <= 0) return 0;
    // SCRATCH_ALIGN - 1 up front, and it is not padding-for-luck: the sum below
    // is what the block costs when it STARTS at a SCRATCH_ALIGN boundary, and
    // nothing makes the caller's block start there. scratch_bump rounds the
    // running pointer up before each carve, so a base that is only 4-aligned
    // burns those 4 bytes at the very first one and every later carve is
    // shifted by them -- and since each item's size is already rounded here,
    // the shortfall lands entirely on the LAST allocation, i.e. the last
    // level's blob. That level then gets a null body and goes inert
    // (font_with_cache below), silently, while every earlier level works
    // perfectly: on Pebble, whose heap hands out 4-aligned blocks, the second
    // of two declared levels never cached a single glyph while the first was
    // fine. Desktop malloc returns 16-aligned blocks, so no sim ever showed it.
    // One SCRATCH_ALIGN - 1 covers any base: past the first carve the pointer
    // is aligned and each rounded size keeps it that way.
    int total = SCRATCH_ALIGN - 1
              + lowspec_cache_align((int)sizeof(FONT_LOWSPEC))
              + lowspec_cache_align((int)sizeof(FONT_SETTINGS_LOWSPEC))
              + lowspec_cache_align((int)sizeof(FONT_CACHE_LOWSPEC) * level_count);
    for (int i = 0; i < level_count; i++) {
        total += lowspec_cache_align(glyph_slots * (int)sizeof(short));
        total += lowspec_cache_align(levels[i].mem_end_offset);
    }
    return total;
}

const FONT_LOWSPEC *render_glyph_lowspec_font_with_cache(const FONT_LOWSPEC *src,
                                                         const FONT_CACHE_LOWSPEC *levels,
                                                         int level_count, int glyph_slots,
                                                         void *mem, int bytes) {
    if (!src || !levels || !mem || level_count <= 0 || glyph_slots <= 0) return 0;
    if (glyph_slots < src->glyph_count) return 0;   // a level that can't index
                                                    // every glyph is never used

    char *cur = (char*)mem;
    char *end = (char*)mem + bytes;

    FONT_LOWSPEC          *font = (FONT_LOWSPEC*)scratch_bump(&cur, end, sizeof(FONT_LOWSPEC));
    FONT_SETTINGS_LOWSPEC *set  = (FONT_SETTINGS_LOWSPEC*)scratch_bump(&cur, end, sizeof(FONT_SETTINGS_LOWSPEC));
    FONT_CACHE_LOWSPEC    *lv   = (FONT_CACHE_LOWSPEC*)scratch_bump(&cur, end,
                                      sizeof(FONT_CACHE_LOWSPEC) * (size_t)level_count);
    if (!font || !set || !lv) return 0;

    *font = *src;
    *set  = *src->settings;      // the const original stays untouched; this RAM
                                 // copy is what carries the levels
    font->settings = set;

    int armed = 0;
    for (int i = 0; i < level_count; i++) {
        lv[i] = levels[i];       // cap_height/stroke_width/flags as declared
        lv[i].seen_cap_height = 0;
        lv[i].sub_x = lv[i].sub_y = 0;
        lv[i].width = lv[i].height = 0;
        lv[i].free_offset = 0;   // unclaimed -- geometry comes from first use
        lv[i].owner = 0;

        int blob = levels[i].mem_end_offset;
        if (blob > LOWSPEC_GLYPH_CACHE_MAX_BYTES) blob = LOWSPEC_GLYPH_CACHE_MAX_BYTES;
        short *table = (short*)scratch_bump(&cur, end, sizeof(short) * (size_t)glyph_slots);
        char  *body  = (char*)scratch_bump(&cur, end, (size_t)blob);
        // A level with no room for a glyph is worse than no level: it would
        // match, miss, fail to store, and mark every glyph uncacheable. Leave
        // it inert instead (lowspec_cache_find skips a null mem_start).
        if (!table || !body || blob < 64) {
            lv[i].mem_start = 0;
            lv[i].glyph_offset = 0;
            lv[i].mem_end_offset = 0;
            lv[i].glyph_slots = 0;
            continue;
        }
        lv[i].glyph_offset   = table;
        lv[i].glyph_slots    = glyph_slots;
        lv[i].mem_start      = body;
        lv[i].mem_end_offset = blob;
        armed++;
    }
    if (!armed) return 0;

    set->caches      = lv;
    set->cache_levels = level_count;
    return font;
}

// Ink bounds of one tile row, as [x0, x1). rs_gray_of, not a raw byte test:
// under RS_PEBBLE_TIME2 a pixel touched with zero coverage still comes back
// with its alpha bits set, so only the decoded gray says "blank".
static void lowspec_row_bounds(const unsigned char *row, int w, int *x0, int *x1) {
    int a = 0, b = w;
    while (a < b && rs_gray_of(row[a])     == 0) a++;
    while (b > a && rs_gray_of(row[b - 1]) == 0) b--;
    *x0 = a; *x1 = b;
}

// Appends the tile's ink to the level's blob in the layout the header
// documents. Returns the offset (> 0), or 0 if the blob is full.
static int lowspec_cache_store(FONT_CACHE_LOWSPEC *c, const unsigned char *tile) {
    int w = c->width, h = c->height;

    int y0 = 0, y1 = h, x0, x1;
    while (y0 < y1) { lowspec_row_bounds(tile + (size_t)y0 * w, w, &x0, &x1); if (x1 > x0) break; y0++; }
    while (y1 > y0) { lowspec_row_bounds(tile + (size_t)(y1 - 1) * w, w, &x0, &x1); if (x1 > x0) break; y1--; }

    int need = 2;
    for (int y = y0; y < y1; y++) {
        lowspec_row_bounds(tile + (size_t)y * w, w, &x0, &x1);
        need += 2 + ((x1 - x0) + 3) / 4;
    }
    if (c->free_offset + need > c->mem_end_offset) return 0;

    int off = c->free_offset;
    unsigned char *p = (unsigned char*)c->mem_start + off;
    *p++ = (unsigned char)(y1 - y0);
    *p++ = (unsigned char)y0;
    for (int y = y0; y < y1; y++) {
        const unsigned char *row = tile + (size_t)y * w;
        lowspec_row_bounds(row, w, &x0, &x1);
        int n = x1 - x0;
        *p++ = (unsigned char)x0;
        *p++ = (unsigned char)n;
        for (int i = 0; i < (n + 3) / 4; i++) p[i] = 0;
        for (int i = 0; i < n; i++) {
            // 0-255 coverage -> 2 bits, rounded: 0, 85, 170, 255.
            int q = (rs_gray_of(row[x0 + i]) * 3 + 127) / 255;
            p[i >> 2] |= (unsigned char)(q << (2 * (i & 3)));
        }
        p += (n + 3) / 4;
    }
    c->free_offset = off + need;
    return off;
}

// Composites a stored glyph over the destination at the tile origin
// (org_x, org_y). Same arithmetic gr_rasterize ends on -- coverage lifts each
// pixel toward white (screen) or toward its own inverse (blend == 1) -- so a
// cached glyph lands on the same pixels the fill path would have written, bar
// the 2-bit quantization of the coverage itself.
//
// `alpha` (Q16.16) scales the stored coverage the same way gr_rasterize scales
// its own -- which is why it belongs HERE and not in the entry: what the blob
// holds is pure geometric coverage, shared by every draw of that glyph, so a
// per-draw alpha must never reach lowspec_cache_build.
static void lowspec_cache_blit(const FONT_CACHE_LOWSPEC *c, int off,
                               const RSurface *surf, int org_x, int org_y,
                               fx16 alpha, int blend) {
    const unsigned char *p = (const unsigned char*)c->mem_start + off;
    int rows  = p[0];
    int y_off = p[1];
    p += 2;

#if USE_UNIFIED_BLEND
    // Mode and alpha resolved once for the whole glyph.
    RBlend rbl = rb_make(blend, -1, alpha);
#else
    int inverted = rs_blend_inverted(blend);
    // 0-255 coverage multiplier; 255 skips the multiply, so a full-strength
    // blit is byte-identical to what this did before alpha existed.
    int alpha255 = (alpha >= FX16_ONE) ? 255 : (int)((alpha * 255) >> FX16_SHIFT);
#endif
    int clip_x0 = surf->clip_x, clip_x1 = surf->clip_x + surf->clip_w;
    int clip_y0 = surf->clip_y, clip_y1 = surf->clip_y + surf->clip_h;

    for (int r = 0; r < rows; r++) {
        int x_off = p[0];
        int n     = p[1];
        const unsigned char *data = p + 2;
        p = data + (n + 3) / 4;

        int y = org_y + y_off + r;
        if (y < clip_y0 || y >= clip_y1) continue;
        unsigned char *dst = surf->pixels + (size_t)y * surf->stride;

        for (int i = 0; i < n; i++) {
            int q = (data[i >> 2] >> (2 * (i & 3))) & 3;
            if (!q) continue;
            int x = org_x + x_off + i;
            if (x < clip_x0 || x >= clip_x1) continue;
            int cov = q * 85;
#if USE_UNIFIED_BLEND
            // alpha is already in rbl's tables, so the coverage the blob
            // stores goes straight in. Dither indexes ABSOLUTE (x, y) -- both
            // are already in destination space here, which is what keeps a
            // moving glyph from crawling through the pattern.
  #if RS_PEBBLE_TIME2
            if (!rbl.gray8) dst[x] = rb_px(&rbl, dst[x], cov, RB_DITHER_AT(x, y));
            else            dst[x] = rb_px_gray(&rbl, dst[x], cov);
  #else
            dst[x] = rb_px_gray(&rbl, dst[x], cov);
  #endif
#else
            if (alpha255 != 255) cov = (cov * alpha255) / 255;
            int gray  = rs_gray_of_b(dst[x], blend);
            int reach = inverted ? (255 - 2 * gray) : (255 - gray);
            dst[x] = rs_px_of_b(gray + cov * reach / 255, blend);
#endif
        }
    }
}

// First draw of a glyph at a cached configuration: rasterize it into a scratch
// tile and store it. Returns the blob offset, or -1 if it will never fit (the
// blob is full -- recorded so we stop retrying), or 0 if only the scratch was
// short this time and it is worth trying again next frame.
static int lowspec_cache_build(FONT_CACHE_LOWSPEC *c, const FONT_LOWSPEC *font, int gi,
                               fx16 strokewidth, fx16 scale,
                               fx16 off_x, fx16 off_y, int flags,
                               char *scratch, char *scratch_end) {
    int w = c->width, h = c->height;
    char *cur = scratch;
    unsigned char *tile = (unsigned char*)scratch_bump(&cur, scratch_end, (size_t)w * (size_t)h);
    if (!tile) return 0;
    render_fill_bytes(tile, 0, w * h);

    // The tile came off the front of the scratch, so what is left for the
    // render is smaller than a plain render would have had -- if that made it
    // bail, the tile is blank for the wrong reason and must not be memorized.
    // Full alpha, always: the blob stores pure geometric coverage that every
    // later draw of this glyph re-composites, so the per-draw alpha is applied
    // in lowspec_cache_blit instead.
    RSurface ts = rs_tile(tile, w, h);
    if (!render_glyph_lowspec(font->packed_data, font->spans[gi].start, font->spans[gi].count,
                              w, h, &ts, 0, 0, 0 /* screen: coverage over black */,
                              FX16_ONE,
                              font->settings, strokewidth, scale, scale,
                              off_x, off_y, flags, cur, scratch_end))
        return 0;

    int off = lowspec_cache_store(c, tile);
    return off ? off : -1;
}

void render_glyph_lowspec_ascii(const FONT_LOWSPEC *font,
                                const RSurface *surf,
                                fx16 strokewidth, fx16 cap_height,
                                fx16 x, fx16 y, int ascii, int flags,
                                char *scratch, char *scratch_end,
                                fx16 alpha, int blend) {
    if (alpha <= 0) return;   // fully transparent -- see header
    if (cap_height <= 0) return;  // zero/negative size -- nothing to draw
    const unsigned int *packed_data = font->packed_data;
    const LowspecGlyphSpan *spans = font->spans;
    const FONT_SETTINGS_LOWSPEC *settings = font->settings;
    int gi = glyph_lowspec_ascii_slot(ascii);
    if (gi < 0 || gi >= font->glyph_count) return;

    fx16 scale = cap_height / (settings->glyph_box_hsize[1] >> 15);

    // Negative stroke width selects the wireframe path (see header).
    if (strokewidth < 0) {
        strokewidth = -strokewidth;
        flags |= RG_FLAGS_RENDER_LINE_OUTLINE;
    }

//    const fx16 aacutoff = (FX16_ONE * 128 >> 8);
//    if (strokewidth < aacutoff) {
//        strokewidth = aacutoff;
//        alpha = fx16_mul(alpha, strokewidth);
//    }

    // Wireframe strokes straight into the destination and never builds a tile,
    // so there is nothing for the raster cache to snapshot -- caught here
    // rather than in lowspec_cache_find so the flag the caller passed counts
    // too, not just the stroke width's sign.
    //
    // BLEND_8BIT_GRAYSCALE is the other disqualifier, and for the opposite
    // reason -- the cache works fine, it just isn't good enough. Entries hold
    // 2-bit coverage, which is LOSSLESS against a GColor8 destination (4 levels
    // is all one survives) but throws away 6 bits against a 256-level gray one.
    // A glyph drawn into an 8-bit image renders the long way so it keeps every
    // level; the screen keeps its cache.
    int cacheable = (flags & RG_FLAGS_RENDER_LINE_OUTLINE) == 0 && !rs_blend_gray8(blend);

    // Tile = the scaled glyph box (hsize is a half-extent) + pad each side,
    // rounded up so a fractional box never loses its last column/row.
//    int grade_pad_mul = flags & RG_FLAGS_IGNORE_GRADE_FIT ? 1 : 2; // we might need to increase the padding for grade fit?
    fx16 box_w = fx16_mul(scale, (settings->glyph_box_hsize[0] + (FONT_SPACE_PAD_X<<16)  ) << 1) + (strokewidth >> 1);
    fx16 box_h = fx16_mul(scale, (settings->glyph_box_hsize[1] + (FONT_SPACE_PAD_Y<<16)  ) << 1) + (strokewidth >> 1);
    int width  = fx16_to_int(box_w) + 2 * LOWSPEC_GLYPH_PIXEL_PAD;
    int height = fx16_to_int(box_h) + 2 * LOWSPEC_GLYPH_PIXEL_PAD;

    // Where the box centre sits inside that tile: half the tile, which is a
    // whole pixel for even sizes and a half-pixel for odd ones. Keeping it in
    // fx16 lets both cases fall out of the same arithmetic.
    fx16 cx_tile = fx16_from_int(width)  >> 1;
    fx16 cy_tile = fx16_from_int(height) >> 1;

    // Split the requested centre into a whole-pixel tile origin plus a
    // sub-pixel remainder. The remainder rides along in the fx16 glyph offset,
    // so moving the caller's (x, y) slides the glyph smoothly instead of
    // snapping it a pixel at a time. fx16_to_int is an arithmetic shift, i.e.
    // floor, so the remainder is always in [0, 1).
    int org_x = fx16_to_int(x - cx_tile);
    int org_y = fx16_to_int(y - cy_tile);

    // Early out: the tile is the glyph's whole footprint (every contour the
    // stroker emits stays inside it -- that is what the pad is for), so if it
    // misses the surface's clip rect nothing can land. Bail before building
    // any geometry: no GrPath (~320 KB of scratch touched), no outline pass.
    // The sub-pixel remainder below only slides the glyph within the tile (by
    // < 1px, which the pad absorbs), so it cannot pull an off-screen glyph in.
    if (org_x >= surf->clip_x + surf->clip_w || org_x + width  <= surf->clip_x ||
        org_y >= surf->clip_y + surf->clip_h || org_y + height <= surf->clip_y)
        return;

    fx16 sub_x = (x - cx_tile) - fx16_from_int(org_x);
    fx16 sub_y = (y - cy_tile) - fx16_from_int(org_y);

    // render_glyph_lowspec puts the box centre at offset + scale*box_centre
    // (that term uses the pre-grade-fit scale, so it holds whatever the stroke
    // width does -- see stroke_subpath_outline_lowspec), hence the offset that
    // lands it on cx_tile + sub_x is that minus scale*box_centre.
    fx16 off_x = cx_tile + sub_x - fx16_mul(scale, settings->glyph_box_center[0]);
    fx16 off_y = cy_tile + sub_y - fx16_mul(scale, settings->glyph_box_center[1]);

    // Raster cache: everything the glyph's pixels depend on is settled by now
    // (the sub-pixel phase was the last of it), so this is where a hit can be
    // decided. A miss on a claimed level rasterizes the tile once and stores
    // it; anything that doesn't fit falls through to the plain render below.
    FONT_CACHE_LOWSPEC *cache = cacheable
        ? lowspec_cache_find(font, cap_height, strokewidth, flags,
                             sub_x, sub_y, width, height)
        : 0;
    if (cache) {
        int off = cache->glyph_offset[gi];
        if (off == 0) {
            off = lowspec_cache_build(cache, font, gi, strokewidth, scale,
                                      off_x, off_y, flags, scratch, scratch_end);
            if (off) cache->glyph_offset[gi] = (short)off;
        }
        if (off > 0) {
            lowspec_cache_blit(cache, off, surf, org_x, org_y, alpha, blend);
            return;
        }
    }

    // blend rides in on render_glyph_lowspec's blending_flags slot (see the
    // header): its low byte's legacy "coverage threshold" is unimplemented
    // (commented out in gr_rasterize), so the field is free to carry the
    // compositing mode instead -- 0 = screen (toward white), 1 = inverted
    // (toward the pixel's own inverse). The desktop fill path honours it in
    // gr_rasterize; the outline path leaves it screen for now.
    render_glyph_lowspec(packed_data, spans[gi].start, spans[gi].count,
                         width, height, surf, org_x, org_y, blend, alpha, settings,
                         strokewidth, scale, scale, off_x, off_y, flags,
                         scratch, scratch_end);
}

void render_text_lowspec_ascii(const FONT_LOWSPEC *font,
                               const RSurface *surf,
                               fx16 strokewidth, fx16 cap_height,
                               fx16 x, fx16 y, const unsigned char *str, int len,
                               int flags, char *scratch, char *scratch_end,
                               fx16 alpha, int blend, fx16 letter_spacing,
                               int align, fx16 line_height) {
    if (alpha <= 0) return;   // fully transparent -- skip the whole run
    if (cap_height <= 0) return;  // zero/negative size -- nothing to draw
    const FONT_SETTINGS_LOWSPEC *settings = font->settings;

    fx16 scale = cap_height / (settings->glyph_box_hsize[1] >> 15);

    fx16 advance = letter_spacing;
    if(advance == 0) {
        advance = (fx16)(((long long)scale * (settings->glyph_grid_size[0])));
        advance &= ~0xffff; // pixel-snap for more legible small text
    }

    // Vertical line pitch: the em height (grid_size[1], e.g. 22 vs the 14-wide
    // em) scaled to cap_height, pixel-snapped like the horizontal advance.
    // When line_height is non-zero it overrides the computed pitch (fx16 pixels).
    fx16 line_advance = line_height;
    if (line_advance == 0) {
        line_advance = (fx16)(((long long)scale * (settings->glyph_grid_size[1])));
        line_advance &= ~0xffff;
    }

    // First pass: count lines ('\n'-separated) so we can centre the whole block
    // vertically. y is the vertical centre of the block, so the first line's
    // centre sits half the block height (num_lines-1 pitches) above it.
    int num_lines = 1;
    for (int i = 0; i < len; i++)
        if (str[i] == '\n') num_lines++;

    fx16 block_offset = ((num_lines - 1) * line_advance) >> 1;
    if(line_height == 0)
        block_offset &= ~0xffff; // pixel-snap for more legible small text
    fx16 line_y = y - block_offset;

    // Second pass: render one line at a time, anchored horizontally on x per
    // `align` (see header): each line's own glyph count decides how far its
    // first glyph sits from x, from 0 (left) through half (centre) to the
    // full span (right) of (line_len-1)*advance.
    int line_start = 0;
    while (line_start <= len) {
        int line_end = line_start;
        while (line_end < len && str[line_end] != '\n') line_end++;
        int line_len = line_end - line_start;

        fx16 line_span = (fx16)((line_len - 1) * advance);
        fx16 align_offset = (align == 1) ? 0 : (align == 3) ? line_span : (line_span >> 1);
        if(letter_spacing == 0)
            align_offset &= ~0xffff; // pixel-snap for more legible small text
        fx16 line_x = x - align_offset;

        for (int i = 0; i < line_len; i++)
            render_glyph_lowspec_ascii(font, surf, strokewidth, cap_height,
                                       line_x + (fx16)(i * advance), line_y,
                                       str[line_start + i], flags,
                                       scratch, scratch_end, alpha, blend);

        line_y += line_advance;
        if (line_end == len) break;
        line_start = line_end + 1;
    }
}

// ===== General-purpose polygon drawing (see render_lowspec.h) =====
//
// Thin forwards to the `static` primitives in common/render_polygon.h /
// common/render_aaline.h -- non-static here so this TU's ONE compiled
// instance (font_lowspec_pebble.c on Pebble) is callable from elsewhere in
// the program instead of every caller instantiating its own private copy.

void render_lowspec_fill_polygon(const RSurface *surf, const int *pts_xy, int n,
                                 int width, int height, int org_x, int org_y,
                                 int blending_flags, fx16 alpha,
                                 char *scratch, char *scratch_end) {
    gr_fill_polygon(surf, pts_xy, n, width, height, org_x, org_y, blending_flags,
                    alpha, scratch, scratch_end);
}

void render_lowspec_stroke_polygon(const RSurface *surf, const int *pts_xy, int n,
                                   int org_x, int org_y, fx16 alpha, int blend) {
    if (alpha <= 0) return;   // fully transparent, same rule as everywhere here
    aaline_polygon(surf, (const fixed*)pts_xy, n, org_x, org_y, alpha, blend);
}

// ---- Triangle-fan fill backend ---------------------------------------
//
// A negative `blend` ARGUMENT routes render_line's and render_ellipse's fill
// through common/render_triangle.h instead of gr_fill_polygon. That sign is
// purely a signal to those two entry points, and they consume it on the way in
// (blend_take_backend below) exactly as they already consume stroke_width's and
// radius_w's -- so `blend` is non-negative everywhere below this boundary and
// nothing downstream has to know the convention exists.
//
// What it buys: no GrPath and no active-edge-table scratch, and one pass
// over the covered rows instead of an 8x-supersampled sweep. render_line
// needs no scratch at all this way (its quad is a local); render_ellipse
// still allocates its boundary points (<= 512 bytes, and still bails if
// that fails) but not the ~4.8KB GrPath behind them. What it costs: hard
// edges, no AA. Alpha is honoured here, unlike on the gr_fill_polygon path
// -- it scales the fill's 0-255 coverage.

// Consumes the backend-selecting sign of a public `blend` argument. Returns the
// blend to use from here on -- same flags, mode made non-negative -- and sets
// *use_triangles if the sign asked for the triangle backend.
//
// rs_blend_norm first: a bare negative mode (-1) is sign-extended, so every
// flag bit above the mode field is set and has to be folded away before those
// bits can be read as flags.
static int blend_take_backend(int blend, int *use_triangles) {
    blend = rs_blend_norm(blend);
    int mode = rs_blend_mode(blend);
    *use_triangles = (mode < 0);
    if (mode < 0) mode = -mode;
    return (mode & BLEND_MODE_MASK) | (blend & ~BLEND_MODE_MASK);
}

// There is no mode mapping left to do: TRI_BLEND_SCREEN/TRI_BLEND_INVERT ARE
// the mode field's own bits (BLEND_INVERT), and the channel locks and the
// flags above bit 15 mean the same thing to render_triangle.h's span loop as
// to every other backend here. So a normalized `blend` goes straight in, with
// no per-backend translation step.

static int tri_color_of(fx16 alpha) {
    return alpha >= FX16_ONE ? 255 : (int)(((long long)alpha * 255) >> FX16_SHIFT);
}

// ===================================================================
// The Tier B blend cache (common/render_blend.h). Allocated by whoever owns
// the RenderCtx and handed back in through the `blend_cache` argument of the
// primitives below.
//
// It lives behind an opaque pointer rather than inside RenderCtx by value, and
// that is not fussiness: sizeof(RBlendLut) depends on RB_DITHER, RB_DITHER is
// folded to 0 unless RS_PEBBLE_TIME2, and RS_PEBBLE_TIME2 is set PER TU --
// render_lowspec_lowmem.c defines it before including this file, while
// render_ctx.c (which may not even mention the encoding, see render_ctx.h) does
// not. A by-value member would therefore have a different layout in the two
// TUs that share the struct. This TU is the only one that ever takes the size,
// which is exactly what the two functions below are for -- the same shape as
// render_ctx_font_cache_bytes / render_ctx_build_font_cache.
// ===================================================================
int render_blend_cache_bytes(void) {
    return (int)sizeof(RBlendCache);
}

RBlendCache *render_blend_cache_init(void *mem, int bytes) {
    if (!mem || bytes < (int)sizeof(RBlendCache)) return 0;
    rb_cache_reset((RBlendCache *)mem);
    return (RBlendCache *)mem;
}

// The cache a primitive uses when the caller passed none -- which is what both
// platforms do, deliberately.
//
// Requiring provisioning would mean any target that forgot the malloc silently
// went back to rebuilding a 3.5k-cycle table per draw, with nothing to notice
// it by. Defaulting costs one pointer test and ~80 bytes of .bss (against the
// Pebble 64 KB virtual-size cap; a device build's other malloc'd pools are
// KB-scale, this is not).
//
// Zero-init IS the empty state -- rb_cache_reset only clears `valid` -- so this
// needs no initialiser and no first-use hook. Sharing it between two RenderCtx
// is correct for the same reason the cache needs no invalidation: the key is
// complete, so an entry either matches or misses. It is NOT safe to share
// across THREADS (see RBlendCache), which no consumer here does -- vscreen is
// one global context and the Pebble side is single-threaded. A caller that
// wants isolation provisions its own with the two functions above and passes it
// down through RenderCtx::blend_cache.
static RBlendCache *rl_blend_cache(RBlendCache *c) {
    static RBlendCache s_shared;
    return c ? c : &s_shared;
}

void render_line(const RSurface *surf, fx16 x1, fx16 y1, fx16 x2, fx16 y2,
                 fx16 stroke_width, fx16 alpha, int blend,
                 char *scratch, char *scratch_end, RBlendCache *blend_cache) {

    // Three sign-carried selectors, all consumed here so the rest of the
    // function works with plain magnitudes: stroke_width's picks wireframe,
    // blend's picks the triangle fill backend (see blend_take_backend).
    int use_triangles = 0;
    blend = blend_take_backend(blend, &use_triangles);

    int outline_only = 0;
    if (stroke_width < 0) {
        stroke_width = -stroke_width;
        outline_only = 1;
    }

    // alpha is literal coverage: 0 draws nothing, FX16_ONE is full strength.
    if (alpha <= 0) return;
    if (alpha > FX16_ONE) alpha = FX16_ONE;

    if (stroke_width <= FX16_ONE) {
        // Hairline: endpoint coverage folds stroke_width (the <=1px width) and
        // alpha, so a sub-pixel line still fades and alpha dims it. At full
        // alpha (FX16_ONE) coverage == stroke_width.
        fx16 cov = fx16_mul(stroke_width, alpha);
        aaline(surf, x1, y1, cov, x2, y2, cov, blend);
        return;
    }

    fx16 dx = x2 - x1;
    fx16 dy = y2 - y1;
    fx16 len = fx16_hypot(dx, dy);
    if (len == 0) {
        return;
    }

    fx16 inv = fx16_div(stroke_width >> 1, len);
    fx16 nx  = fx16_mul(-dy, inv);
    fx16 ny  = fx16_mul(dx, inv);

    // Quad corners in Q16.16 buffer space, wound P1+n -> P2+n -> P2-n -> P1-n.
    int pts_xy[8] = {
        x1 + nx, y1 + ny,
        x2 + nx, y2 + ny,
        x2 - nx, y2 - ny,
        x1 - nx, y1 - ny,  
    };

    if(outline_only) {
        aaline_polygon(surf, (const fixed*)pts_xy, 4, 0, 0, alpha, blend);
        return;
    }
    if (use_triangles) {
        // Triangle backend: the quad is already in buffer space and convex,
        // so it needs neither the tile rebase below nor scratch.
        triangle_fill_convex_polygon(surf, pts_xy, 4, tri_color_of(alpha), blend,
                                     rl_blend_cache(blend_cache));
        return;
    }
#if RENDER_USE_CONVEX_FILL
    // The quad is convex and already in buffer space, so this needs neither the
    // tile rebase below nor a byte of scratch -- which makes render_line
    // SCRATCH-FREE on every one of its paths (hairline, wireframe, triangle
    // backend and now this one). The two parameters stay in the signature
    // because callers pass them positionally and because flipping
    // RENDER_USE_CONVEX_FILL back needs them; nothing reads them.
    (void)scratch; (void)scratch_end;
    convex_fill_aa(surf, pts_xy, 4, alpha, blend, rl_blend_cache(blend_cache));
    return;
#else
    // gr_fill_polygon rasterises a width*height tile whose (0,0) sits at pixel
    // (org_x, org_y), with the polygon in tile-local Q16.16. Bound the tile to
    // the quad (floor the min corner for the origin, +2px margin so the AA'd
    // far edges aren't clipped) and rebase the corners into it.
    fx16 minx = pts_xy[0], maxx = pts_xy[0], miny = pts_xy[1], maxy = pts_xy[1];
    for (int i = 1; i < 4; i++) {
        if (pts_xy[i*2  ] < minx) minx = pts_xy[i*2];
        if (pts_xy[i*2  ] > maxx) maxx = pts_xy[i*2];
        if (pts_xy[i*2+1] < miny) miny = pts_xy[i*2+1];
        if (pts_xy[i*2+1] > maxy) maxy = pts_xy[i*2+1];
    }
    int org_x  = fx16_to_int(minx);   // arithmetic shift == floor
    int org_y  = fx16_to_int(miny);
    int width  = fx16_to_int(maxx) - org_x + 2;
    int height = fx16_to_int(maxy) - org_y + 2;


    fx16 ox = fx16_from_int(org_x), oy = fx16_from_int(org_y);
    for (int i = 0; i < 4; i++) {
        pts_xy[2*i]     = pts_xy[2*i] - ox;
        pts_xy[2*i + 1] = pts_xy[2*i + 1] - oy;
    }

    // blending_flags=32 (~1/8 coverage) matches the star/glyph fill in this TU:
    // fully-covered interior pixels take the solid colour, sub-threshold AA
    // fringes are left showing the background so edges don't flicker.
    gr_fill_polygon(surf, pts_xy, 4, width, height, org_x, org_y, blend, alpha,
                    scratch, scratch_end);
#endif
}

void render_point(const RSurface *surf, fx16 x, fx16 y, fx16 amount, int blend) {
    aapixel(surf, x, y, amount, blend);
}

// Fill n bytes at p with v, a word at a time.
//
// This exists because render_rect's opaque fast path below is the full-screen
// background clear -- every codesynth visual body opens with
// vscreen_background_fx16, which is 200x228 = 45,600 bytes on Pebble, and it
// runs once per visual per frame. Written as `*row++ = c` it compiled (at -Os,
// Cortex-M) to a byte store plus a TAKEN BRANCH per byte: five instructions
// and ~6-9 cycles to write one byte.
//
// Three phases, and the middle one is the point: eight word stores in a row is
// what gets the compiler to emit a single STMIA writing 32 bytes in 1+8
// cycles. The head/tail exist so the caller does not have to care about
// alignment -- a rect can start at any x, and a row of an odd-stride surface
// starts anywhere.
//
// Not memset: on Pebble that is a firmware syscall reached through a table
// trampoline (jump_to_pbl_function), so its cost is neither visible nor
// controllable from here. This is, and it is the same code on every target.
//
// The unsigned char* -> unsigned int* cast is the usual fill idiom. It is safe
// here for the usual reason: this function only WRITES, so there is no load of
// the same memory through another type for the compiler to reorder against.
void render_fill_bytes(unsigned char *p, unsigned char v, int n) {
    if (n <= 0) return;
    while (n > 0 && ((size_t)p & 3u)) { *p++ = v; n--; }

    unsigned int  w  = (unsigned int)v * 0x01010101u;
    unsigned int *wp = (unsigned int *)(void *)p;
    while (n >= 32) {
        wp[0] = w; wp[1] = w; wp[2] = w; wp[3] = w;
        wp[4] = w; wp[5] = w; wp[6] = w; wp[7] = w;
        wp += 8; n -= 32;
    }
    while (n >= 4) { *wp++ = w; n -= 4; }

    p = (unsigned char *)(void *)wp;
    while (n-- > 0) *p++ = v;
}

void render_rect(const RSurface *surf, fx16 x, fx16 y, fx16 w, fx16 h, int col, fx16 alpha,
                 int blend, RBlendCache *blend_cache) {
    int x0 = fx16_to_int(x);
    int y0 = fx16_to_int(y);
    int x1 = fx16_to_int(x + w);
    int y1 = fx16_to_int(y + h);

    // Clip to the surface's writable rect (coords are buffer-space pixels).
    int cx0 = surf->clip_x,                  cy0 = surf->clip_y;
    int cx1 = surf->clip_x + surf->clip_w,   cy1 = surf->clip_y + surf->clip_h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;

    // Literal coverage, same rule as render_line: 0 draws nothing.
    if (alpha <= 0) return;
    if (alpha > FX16_ONE) alpha = FX16_ONE;

    unsigned char c = (unsigned char)(col & 0xFF);
    int inverted = rs_blend_inverted(blend);
    int gray8    = rs_blend_gray8(blend);
#if !RS_PEBBLE_TIME2
    (void)gray8;   // this build's byte IS an 8-bit gray, so the flag changes nothing
#endif

    // A channel-locked draw has to read the destination back, so it cannot
    // take the store-the-byte fast path below -- it goes to Tier B, which
    // applies the mask. rs_blend_chanmask is 0xFF exactly when nothing is
    // masked, so an ordinary rect is unaffected.
    int masked = 0;
#if USE_UNIFIED_BLEND && RS_PEBBLE_TIME2
    masked = !gray8 && rs_blend_chanmask(blend) != 0xFF;
#endif

    if (alpha >= FX16_ONE && !inverted && !masked) {  // fast path: write col verbatim
        int n = x1 - x0;
        for (int yy = y0; yy < y1; yy++)
            render_fill_bytes(surf->pixels + (size_t)yy * surf->stride + x0, c, n);
        return;
    }
    // NOTE the fast path above stays undithered under RB_DITHER, correctly:
    // it writes an exact GColor8 value, so there is no quantization error to
    // spread and dithering it would only add noise.

#if RB_LUT_ENABLED
    {
        // Tier B. Coverage is constant over the whole rectangle, so the blend
        // is a pure dstbyte -> dstbyte map over the 64 reachable GColor8
        // states -- per-channel colour and dither included. One lookup (or, on
        // a miss, one build), then the inner loop is a single indexed load,
        // replacing the six divides per pixel the #else path below pays.
        //
        // rb_lut_get returns 0 for gray8 draws, which is what drops them
        // through to the closed form below.
        // `alpha` folds into the coverage rather than into the RBlend, so the
        // cache sees the same key shape as the triangle path -- see rb_lut_get.
        const RBlendLut *lut;
        // BLEND_REPLACE is masked off because a rect has never honoured it (it
        // is documented triangle-backend-only, and this function's own opaque
        // fast path is what "store the byte" means here). Without the mask
        // rb_lut_get would refuse the table and silently drop a REPLACE rect
        // onto the slow closed form -- same pixels, six divides each, and no
        // dither. Masking also lets two draws differing only in that bit share
        // one cache entry, which is right, because they render the same.
        lut = rb_lut_get(rl_blend_cache(blend_cache), blend & ~BLEND_REPLACE, c,
                         rb_alpha8(alpha));
        if (lut) {
            for (int yy = y0; yy < y1; yy++) {
                unsigned char *row = surf->pixels + (size_t)yy * surf->stride;
                for (int xx = x0; xx < x1; xx++)
                    row[xx] = lut->t[RB_LUT_PHASE(xx, yy)][row[xx] & 0x3F];
            }
            return;
        }
    }
#else
    (void)blend_cache;
#endif

    // Blend per pixel by coverage `a` (0..255): toward `col` (screen) or toward
    // each pixel's own inverse (inverted, same reach formula as gr_rasterize's
    // inverted glyph fill). Under RS_PEBBLE_TIME2 the byte is a GColor8 (three
    // 2-bit RGB channels), so blend per channel in 0..255 space and
    // re-quantize; otherwise it's a single 8-bit gray channel.
    int a = (alpha >= FX16_ONE) ? 255 : (int)(((long long)alpha * 255) >> FX16_SHIFT);
    for (int yy = y0; yy < y1; yy++) {
        unsigned char *row = surf->pixels + (size_t)yy * surf->stride + x0;
        for (int xx = x0; xx < x1; xx++) {
            unsigned char d = *row;
#if RS_PEBBLE_TIME2
            // BLEND_8BIT_GRAYSCALE overrides the build's encoding for this
            // draw: one 0-255 channel, `col` read as a gray level. Same
            // arithmetic as the #else below, which is the non-GColor8 build's
            // only path.
            if (gray8) {
                int reach = inverted ? (255 - 2 * (int)d) : ((int)c - (int)d);
                int v = (int)d + reach * a / 255;
                if (v < 0) v = 0; else if (v > 255) v = 255;
                *row++ = (unsigned char)v;
                continue;
            }
            int dr = ((d >> 4) & 3) * 85, dg = ((d >> 2) & 3) * 85, db = (d & 3) * 85;
            int cr = ((c >> 4) & 3) * 85, cg = ((c >> 2) & 3) * 85, cb = (c & 3) * 85;
            int reach_r = inverted ? (255 - 2 * dr) : (cr - dr);
            int reach_g = inverted ? (255 - 2 * dg) : (cg - dg);
            int reach_b = inverted ? (255 - 2 * db) : (cb - db);
            int rr = dr + reach_r * a / 255;
            int rg = dg + reach_g * a / 255;
            int rb = db + reach_b * a / 255;
            // /85 rounded back to a 2-bit channel; alpha bits set opaque (0xC0).
            *row++ = (unsigned char)(0xC0
                        | (((rr + 42) / 85) << 4)
                        | (((rg + 42) / 85) << 2)
                        |  ((rb + 42) / 85));
#else
            int reach = inverted ? (255 - 2 * (int)d) : ((int)c - (int)d);
            *row++ = (unsigned char)(d + reach * a / 255);
#endif
        }
    }
}

// See header. N scales with the larger radius (~pi points per pixel, i.e. a
// ~2px chord), clamped to bound scratch usage and per-call cost.
#define ELLIPSE_MIN_SEGS 8
#define ELLIPSE_MAX_SEGS 64
#define ELLIPSE_TAU fx16_from_float(6.28318530f)

static int ellipse_segment_count(fx16 radius_w, fx16 radius_h) {
    fx16 r = radius_w > radius_h ? radius_w : radius_h;
    int r_px = fx16_to_int(r < 0 ? -r : r);
    int n = (r_px * 314) / 100;   // ~pi * r_px
    if (n < ELLIPSE_MIN_SEGS) n = ELLIPSE_MIN_SEGS;
    if (n > ELLIPSE_MAX_SEGS) n = ELLIPSE_MAX_SEGS;
    return n;
}

void render_ellipse(const RSurface *surf, fx16 x, fx16 y, fx16 radius_w, fx16 radius_h,
                    fx16 alpha, int blend,
                    char *scratch, char *scratch_end, RBlendCache *blend_cache) {
    // Same sign/magnitude convention as render_line: radius_w's sign selects
    // wireframe vs. fill (radius_h's sign is ignored, magnitude used), blend's
    // selects the triangle fill backend. Both consumed here.
    int use_triangles = 0;
    blend = blend_take_backend(blend, &use_triangles);

    int outline_only = 0;
    if (radius_w < 0) { radius_w = -radius_w; outline_only = 1; }
    if (radius_h < 0) radius_h = -radius_h;

    // Literal coverage, same as render_line: 0 draws nothing at all.
    if (alpha <= 0 || (radius_w==0 && radius_h==0)) return;
    if (alpha > FX16_ONE) alpha = FX16_ONE;

    // Too small to resolve a boundary polygon: one antialiased point, same as
    // render_line's stroke_width <= 1px hairline threshold.
    if (radius_w <= FX16_ONE && radius_h <= FX16_ONE) {
        aapixel(surf, x, y, fx16_mul(alpha, radius_w), blend);
        return;
    }

    int n = ellipse_segment_count(radius_w, radius_h);
    fx16 *pts = scratch_alloc(fx16, n * 2);
    if (!pts) return;

    fx16 step = fx16_div(ELLIPSE_TAU, fx16_from_int(n));
    fx16 angle = 0;
#if !RENDER_USE_CONVEX_FILL
    // The bounding box exists only to size gr_fill_polygon's tile. The convex
    // filler works in buffer space and finds its own y extent, so tracking it
    // would be up to 64*4 dead compares per ellipse on a path that does not
    // want it.
    fx16 minx = 0x7fffffff, maxx = -0x7fffffff, miny = 0x7fffffff, maxy = -0x7fffffff;
#endif
    for (int i = 0; i < n; i++) {
        fx16 px = x + fx16_mul(radius_w, fx16_cos(angle));
        fx16 py = y + fx16_mul(radius_h, fx16_sin(angle));
        pts[2*i] = px; pts[2*i + 1] = py;
#if !RENDER_USE_CONVEX_FILL
        if (px < minx) minx = px;
        if (px > maxx) maxx = px;
        if (py < miny) miny = py;
        if (py > maxy) maxy = py;
#endif
        angle += step;
    }

    // scratch was bumped past pts[] above (scratch_alloc mutates the local
    // `scratch` param in place -- see common/scratch_alloc.h); the remainder is
    // free for gr_fill_polygon's own scratch use below.

    if (outline_only) {
        // Boundary points are already in buffer-space coords, so org is (0,0)
        // -- same reinterpret-as-fixed* aaline_polygon uses elsewhere in this
        // TU (e.g. render_glyph_lowspec's line-outline path).
        aaline_polygon(surf, (const fixed*)pts, n, 0, 0, alpha, blend);
        return;
    }

    if (use_triangles) {
        // Triangle backend. The boundary is convex and still
        // in buffer space, so the tile rebase below is skipped too.
        triangle_fill_convex_polygon(surf, (const int*)pts, n, tri_color_of(alpha), blend,
                                     rl_blend_cache(blend_cache));
        return;
    }

#if RENDER_USE_CONVEX_FILL
    // The boundary n-gon is convex and already in buffer space, so the tile
    // rebase below is skipped -- and the scratch this function still needs for
    // pts[] is now ALL it needs.
    convex_fill_aa(surf, (const int*)pts, n, alpha, blend, rl_blend_cache(blend_cache));
    return;
#else
    // Filled path: bound the tile to the boundary (+2px margin so the AA'd far
    // edge isn't clipped) and rebase into it, same scheme as render_line's
    // thick path.
    int org_x  = fx16_to_int(minx);
    int org_y  = fx16_to_int(miny);
    int width  = fx16_to_int(maxx) - org_x + 2;
    int height = fx16_to_int(maxy) - org_y + 2;

    fx16 ox = fx16_from_int(org_x), oy = fx16_from_int(org_y);
    for (int i = 0; i < n; i++) {
        pts[2*i]     -= ox;
        pts[2*i + 1] -= oy;
    }

    // blending_flags=32, same convention as render_line's thick path.
    gr_fill_polygon(surf, (const int*)pts, n, width, height, org_x, org_y, blend, alpha,
                    scratch, scratch_end);
#endif
}

void render_circle(const RSurface *surf, fx16 x, fx16 y, fx16 radius,
                   fx16 alpha, int blend,
                   char *scratch, char *scratch_end, RBlendCache *blend_cache) {
    render_ellipse(surf, x, y, radius, radius, alpha, blend, scratch, scratch_end, blend_cache);
}
