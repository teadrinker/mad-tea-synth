#ifndef RENDER_TRIANGLE_H
#define RENDER_TRIANGLE_H

#include <stdint.h>
#include "common/render_surface.h"

// ===================================================================
// Flat-fill triangle rasterizer. Self-contained (no libc/malloc/tsys, no
// scratch), coords are 16.16 fixed point in plain `int` -- the same
// convention render_polygon.h's pts_xy uses and the same buffer-space
// coords render_aaline.h takes, so a point array can be handed to any of
// the three. Unlike gr_rasterize there is no tile/origin indirection and
// no coverage AA: one scanline loop straight into the RSurface, clipped to
// its clip rect.
//
// Watertight. A pixel is filled iff its sample point (x+0.5, y+0.5) lands
// in the half-open region yTop <= y_s < yBot, xLeft <= x_s < xRight -- the
// standard top-left rule. Both triangles sharing an edge derive that
// edge's x from the same anchor (its topmost endpoint) and the same
// truncated slope, so the boundary value is bit-identical on both sides:
// the edge is then inclusive for the triangle on its right and exclusive
// for the one on its left. No gaps along a shared edge, and no pixel
// covered twice -- which is what makes the blend modes below safe to use
// on a whole fan or mesh, not just on one isolated triangle.
//
// Ported from a float rasterizer; every rounding step is the fixed-point
// image of the original's (int)(-floor(0.5f - v)) == ceil(v - 0.5).
// Inverse slopes and the running x are kept in int64 rather than 16.16:
// a near-horizontal edge has |dx/dy| far past what Q16.16 holds, and
// silently wrapping there would tear the shared edge apart.
//
// Tested by tests/render_triangle_test.c (mesh coverage-count == 1,
// diagonal-flip invariance, vertex-order invariance).
// ===================================================================

#define TRI_SHIFT 16
#define TRI_ONE   (1 << TRI_SHIFT)
#define TRI_HALF  (TRI_ONE >> 1)

// Compositing for the solid `color` (0-255 gray, encoded via rs_px_of).
// REPLACE is the reference behaviour (a plain framebuffer store); the other
// two are render_polygon.h's two modes evaluated at full coverage, so a
// triangle and a gr_rasterize fill of the same shape agree.
//
// These are not a private enum any more: they ARE the mode field's own bits
// (render_surface.h), so a caller hands this backend the same `blend` it hands
// every other one and screen/inverted come out the same. REPLACE is the only
// thing here no other backend has, and it is a bit of its own -- it cannot be
// 0, because 0 is what "screen over every channel" looks like, and it cannot
// be 2, because that is BLEND_LOCK_R.
//
// The flags above bit 15 (BLEND_8BIT_GRAYSCALE and friends) ride along in the
// same int, hence the masked reads below rather than raw ==.
#define TRI_BLEND_SCREEN  0
#define TRI_BLEND_INVERT  BLEND_INVERT
#define TRI_BLEND_REPLACE BLEND_REPLACE

// ===================================================================
// COORDINATE CONVENTION -- see the same block in common/render_polygon.h,
// which is the reference this is aligned to.
//
// Coordinate v is a pixel BOUNDARY: pixel i covers [i, i+1) and is sampled at
// its centre, i + 0.5 -- exactly what the header comment above claims. The
// `+= TRI_HALF` on entry below was cancelling that: it slid the geometry half
// a pixel so the effective sample landed on the integer coordinate i instead,
// which drew every slanted edge half a pixel down-right of a gr_rasterize fill
// of the same points. Subtracting the bias restores the documented behaviour.
//
// Written as a bias rather than by deleting the shift so the two halves stay
// visible and 0 disables the bias. Everything else -- the top-left
// rule, the shared-edge slope anchoring, watertightness -- is a uniform
// translation away and is unaffected.
// ===================================================================
#ifndef TRI_PIXEL_CENTRE_BIAS
#define TRI_PIXEL_CENTRE_BIAS TRI_HALF
#endif

// ceil(q / TRI_ONE). >> is floor, so negate around it.
static inline int tri_ceil_to_int(int64_t q) { return (int)(-((-q) >> TRI_SHIFT)); }

static inline int64_t tri_inv_slope(int dx, int dy) {
    return dy != 0 ? (((int64_t)dx << TRI_SHIFT) / dy) : 0;
}

// `lut` is the caller's precomputed Tier B table (USE_UNIFIED_BLEND only) or
// ignored. It is built once per POLYGON rather than per triangle: a fan over
// an n-gon calls this from n-2 triangles, and rebuilding 64 entries each time
// would cost more than the spans do. NULL selects the REPLACE path, which
// needs no table.
static inline void tri_span(const RSurface* surf, int y, int xs, int xe, int color,
                            int blending_flags, const RBlendLut* lut) {
    unsigned char* row = surf->pixels + (int64_t)y * surf->stride;
    if (rs_blend_mode(blending_flags) & TRI_BLEND_REPLACE) {
        unsigned char px = rs_px_of_b(color, blending_flags);
#if USE_UNIFIED_BLEND && RS_PEBBLE_TIME2
        // A locked channel is locked against a store too, not just against a
        // blend -- otherwise BLEND_LOCK_* would mean different things to the
        // two halves of this function. Same gray8 caveat as rb_make: an 8-bit
        // gray byte has no channels to lock.
        if (!rs_blend_gray8(blending_flags)) {
            int m = rs_blend_chanmask(blending_flags);
            if (m != 0xFF) {
                unsigned char keep = (unsigned char)~m;
                px = (unsigned char)(px & m);
                for (int x = xs; x < xe; x++) row[x] = (unsigned char)(px | (row[x] & keep));
                return;
            }
        }
#endif
        for (int x = xs; x < xe; x++) row[x] = px;
        return;
    }
#if USE_UNIFIED_BLEND
    (void)color;
  #if RS_PEBBLE_TIME2
    if (lut) {
        // The whole blend, per-channel (and dithered under RB_DITHER, where
        // RB_LUT_PHASE stops folding to 0), in one indexed load -- this is what
        // constant coverage buys.
        for (int x = xs; x < xe; x++)
            row[x] = lut->t[RB_LUT_PHASE(x, y)][row[x] & 0x3F];
        return;
    }
  #else
    (void)lut;
  #endif
    {
        // 8-bit gray destination: 256 states is too many to tabulate cheaply,
        // so fall back to the Tier A closed form with coverage constant.
        RBlend rb = rb_make(blending_flags, -1, 65536);
        for (int x = xs; x < xe; x++)
            row[x] = rb_px_gray(&rb, row[x], color);
    }
#else
    (void)lut;
    int inverted = rs_blend_inverted(blending_flags);
    for (int x = xs; x < xe; x++) {
        int gray = rs_gray_of_b(row[x], blending_flags);
        int reach = inverted ? (255 - 2 * gray) : (255 - gray);
        row[x] = rs_px_of_b(gray + (color * reach) / 255, blending_flags);
    }
#endif
}

// ===================================================================
// The Tier B table on this path comes from rb_lut_get(bc, flags, -1, color):
// `col` is -1 because a triangle draw always blends toward white, and `color`
// IS the coverage here (see the header note on the two meanings of that
// argument), so it goes straight in as `cov`.
//
// Which draws have no table -- REPLACE, an 8-bit gray destination, a build with
// no Tier B at all -- is rb_lut_get's business, not this file's: whether a
// blend is tabulatable is a question about the blend, not about triangles.
//
// `bc` may be NULL at either entry point below, which costs a table built for
// this call and thrown away. Both supply a stack cache in that case, 12 bytes
// more than a bare RBlendLut.
// ===================================================================

// const RSurface*: writes the pixels, never the descriptor, so a caller
// holding a read-only surface can still draw (same as aaline).
//
// As triangle_fill, but taking a caller-built Tier B table so a fan can share
// one across all its triangles. Pass NULL to mean "no table".
static void triangle_fill_lut(const RSurface* surf, int x1, int y1, int x2, int y2, int x3, int y3,
                              int color, int blending_flags, const RBlendLut* lut) {
    // See TRI_PIXEL_CENTRE_BIAS above -- at the default bias these two lines
    // are a no-op, and the sample point really is (x+0.5, y+0.5).
    const int b = TRI_HALF - TRI_PIXEL_CENTRE_BIAS;
    x1 += b; x2 += b; x3 += b;
    y1 += b; y2 += b; y3 += b;
    if (y1 > y2) { int t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }
    if (y2 > y3) { int t=x2;x2=x3;x3=t; t=y2;y2=y3;y3=t; }
    if (y1 > y2) { int t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; }

    // Anchored at each edge's top endpoint -- y1<=y2<=y3 now, and C division
    // truncates toward zero, so edge 3->1 written as (x1-x3)/(y1-y3) yields
    // the identical slope a neighbouring triangle gets from (x3-x1)/(y3-y1).
    // That identity is the whole watertightness argument; don't "simplify"
    // these three into a common helper that reorders the operands.
    const int64_t inv_slope12 = tri_inv_slope(x2 - x1, y2 - y1);
    const int64_t inv_slope23 = tri_inv_slope(x3 - x2, y3 - y2);
    const int64_t inv_slope31 = tri_inv_slope(x1 - x3, y1 - y3);

    const int clip_x0 = surf->clip_x, clip_x1 = surf->clip_x + surf->clip_w;
    const int clip_y0 = surf->clip_y, clip_y1 = surf->clip_y + surf->clip_h;

    int y_start = tri_ceil_to_int((int64_t)y1 - TRI_HALF);
    int y_end   = tri_ceil_to_int((int64_t)y3 - TRI_HALF);
    if (y_start < clip_y0) y_start = clip_y0;
    if (y_end   > clip_y1) y_end   = clip_y1;

    for (int y = y_start; y < y_end; y++) {
        const int64_t yc = (int64_t)y * TRI_ONE + TRI_HALF;
        int64_t x_left, x_right;
        if (yc < y2) x_left = (int64_t)x1 + (((yc - y1) * inv_slope12) >> TRI_SHIFT);
        else         x_left = (int64_t)x2 + (((yc - y2) * inv_slope23) >> TRI_SHIFT);
        x_right      = (int64_t)x1 + (((yc - y1) * inv_slope31) >> TRI_SHIFT);
        if (x_left > x_right) { int64_t t = x_left; x_left = x_right; x_right = t; }

        int xs = tri_ceil_to_int(x_left  - TRI_HALF);
        int xe = tri_ceil_to_int(x_right - TRI_HALF);
        if (xs < clip_x0) xs = clip_x0;
        if (xe > clip_x1) xe = clip_x1;
        if (xs < xe) tri_span(surf, y, xs, xe, color, blending_flags, lut);
    }
}

// One isolated triangle. A fan should go through triangle_fill_convex_polygon
// (or triangle_fill_lut directly) so the table lookup is paid once rather than
// n-2 times. `bc` is the caller's Tier B cache, or NULL to build and discard.
static void triangle_fill(const RSurface* surf, int x1, int y1, int x2, int y2, int x3, int y3,
                          int color, int blending_flags, RBlendCache* bc) {
    RBlendCache local;                        // only touched when bc == 0
    if (!bc) { rb_cache_reset(&local); bc = &local; }
    triangle_fill_lut(surf, x1, y1, x2, y2, x3, y3, color, blending_flags,
                      rb_lut_get(bc, blending_flags, -1, color));
}

// Fills a *convex* polygon as a triangle fan from vertex 0: n distinct
// vertices as interleaved 16.16 pairs (pts_xy[2*i], pts_xy[2*i+1]),
// auto-closed -- do not repeat the first point. Same array layout as
// gr_fill_polygon and aaline_polygon, so the same points can be filled,
// AA-filled or stroked interchangeably.
//
// Convexity is the caller's promise: a fan over a concave (or
// self-intersecting) outline covers area outside it. Fill those with
// render_polygon.h instead, which winds properly. Every fan diagonal is
// shared by exactly two triangles, so the seams inherit triangle_fill's
// watertightness -- no pixel is filled twice even under a blend mode.
//
// `bc` is the caller's Tier B cache (NULL to build and discard). Passing one
// that persists across draws is the difference between a 60-px quad paying for
// its own 3.5k-cycle table and paying a three-int compare -- see RBlendCache in
// common/render_blend.h.
static void triangle_fill_convex_polygon(const RSurface* surf, const int* pts_xy, int n,
                                        int color, int blending_flags, RBlendCache* bc) {
    if (n < 3) return;
    RBlendCache local;                        // only touched when bc == 0
    if (!bc) { rb_cache_reset(&local); bc = &local; }
    const RBlendLut* l = rb_lut_get(bc, blending_flags, -1, color);  // once, not n-2 times
    for (int i = 1; i + 1 < n; i++)
        triangle_fill_lut(surf, pts_xy[0], pts_xy[1],
                                pts_xy[2*i],     pts_xy[2*i + 1],
                                pts_xy[2*i + 2], pts_xy[2*i + 3],
                                color, blending_flags, l);
}

#endif // RENDER_TRIANGLE_H
