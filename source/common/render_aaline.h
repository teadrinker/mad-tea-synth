#ifndef RENDER_AALINE_H
#define RENDER_AALINE_H

#include <stdint.h>
#include "common/render_surface.h"


#ifndef fixed
typedef int32_t fixed;
#endif
#ifndef FIXED_SHIFT
#define FIXED_SHIFT 16
#endif
#ifndef FIXED_ONE
#define FIXED_ONE  ((fixed)1 << FIXED_SHIFT)
#endif
#ifndef FIXED_HALF
#define FIXED_HALF (FIXED_ONE >> 1)
#endif
#ifndef fixed_from_int
#define fixed_from_int(i)   ((fixed)((int32_t)(i) << FIXED_SHIFT))
#endif
#ifndef fixed_to_int
#define fixed_to_int(f)     ((int32_t)((f) >> FIXED_SHIFT))
#endif
#ifndef fixed_from_float
#define fixed_from_float(v) ((fixed)((v) * (float)FIXED_ONE))
#endif
#ifndef fixed_mul
#define fixed_mul(a, b) ((fixed)(((int64_t)(a) * (int64_t)(b)) >> FIXED_SHIFT))
#endif
#ifndef fixed_div
#define fixed_div(a, b) ((fixed)(((int64_t)(a) << FIXED_SHIFT) / (int64_t)(b)))
#endif
#ifndef fixed_abs
#define fixed_abs(a) ((a) < 0 ? -(a) : (a))
#endif
#ifndef fixed_floor
static inline fixed fixed_floor(fixed x) { return x & ~(fixed)(FIXED_ONE - 1); }
#endif
#ifndef fixed_frac
static inline fixed fixed_frac(fixed x)  { return x &  (fixed)(FIXED_ONE - 1); }
#endif


// ===================================================================
// COORDINATE CONVENTION -- see the same block in common/render_polygon.h,
// which is the reference this is aligned to.
//
// Coordinate v is a pixel BOUNDARY: pixel i covers [i, i+1) and its centre is
// i + 0.5. So a hairline at y = 2.5 lands solid on row 2, and
//   line(2, 2.5, 4, 2.5, 1)  covers exactly  rect(2, 2, 2, 1)
// -- the identity that pins the three rasterizers and render_rect together.
//
// The Wu code below was written to the other convention (coordinate v at pixel
// INDEX v, so y = 2.0 lights row 2 solid), which drew everything half a pixel
// down and right of a gr_rasterize fill of the same points -- invisible on
// axis-aligned edges, plain on every slanted one, and the reason a filled
// polygon never sat under its own aaline_polygon outline.
//
// Rather than re-deriving the rasterizer, both entry points (aaline, aapixel)
// subtract this bias from their incoming coordinates and are otherwise
// untouched. It is applied BEFORE clipping, so the clipper still works in
// plain pixel-index space. Set it to 0 to disable the bias.
// ===================================================================
#ifndef AALINE_PIXEL_CENTRE_BIAS
#define AALINE_PIXEL_CENTRE_BIAS FIXED_HALF
#endif

// Draws in buffer-space coords, so it needs no origin -- just the RSurface.
typedef RSurface AALineSurface;

// Applies alpha_fx coverage to buffer index `id` per the active blend policy.
// No clip check -- aaline_plot does that then forwards here; aapixel below
// clips once for all 4 corners instead and calls this directly.
//
// px/py are the pixel's absolute buffer coordinates. They are used only to
// index the dither matrix (USE_UNIFIED_BLEND + RB_DITHER) and are otherwise
// dead -- but they have to be threaded here rather than derived, because `id`
// is a linear index and recovering (x, y) from it would need a division by the
// stride in the hottest loop in the renderer. Both callers have the
// coordinates already.
//
// `rb` is the draw's resolved RBlend, built once by aaline/aapixel. It is
// unused on the legacy path, which reads blend_option directly.
static inline void aaline_blend(const RSurface *surf, int id, int px, int py,
                                fixed alpha_fx, int blend_option, const RBlend *rb) {
    int cur = surf->pixels[id];
#ifdef AALINE_PLOT_4_X_64
    // A fourth pixel encoding entirely -- 6-bit ramp plus 2-bit colour -- which
    // RBlend does not model and which has 64 levels, so nothing to dither.
    // Left exactly as it was; ui/textmode.c is its only consumer.
    (void)px; (void)py; (void)rb;
    int ramp_bits = (cur & 192) * (blend_option >> 2) | ((blend_option & 3) << 6); // blend_option: 0-3 == replace color, 4 == use current color
    surf->pixels[id] = ramp_bits | ((cur & 63) + ((alpha_fx * (63 - (cur & 63))) >> FIXED_SHIFT));
#elif USE_UNIFIED_BLEND
    (void)blend_option;
    // Q16.16 coverage -> the Q8 the tables are built for. Rounded, not
    // truncated: the bare >>8 biases every pixel dark by up to 1/256 of
    // coverage, which showed up as a max 2-level drift against the legacy path
    // on an 8-bit gray destination. A touch over 255 at full coverage, which
    // the level clamp in rb_px absorbs.
    int cov = (alpha_fx + 128) >> 8;
  #if RS_PEBBLE_TIME2
    // One loop-invariant branch, not the two the legacy path pays. aaline has
    // no loop to hoist it out of (Wu plots scattered pixels, not spans -- see
    // the plan's 3.8), so it lands here.
    if (rb->gray8) surf->pixels[id] = rb_px_gray(rb, (unsigned)cur, cov);
    else           surf->pixels[id] = rb_px(rb, (unsigned)cur, cov, RB_DITHER_AT(px, py));
  #else
    // This build's byte IS an 8-bit gray, so the flag cannot change anything
    // and the branch folds away.
    (void)px; (void)py;
    surf->pixels[id] = rb_px_gray(rb, (unsigned)cur, cov);
  #endif
#else
    // Decode/encode via render_surface.h so this blends correctly against a
    // GColor8 framebuffer (RS_PEBBLE_TIME2), not just an 8-bit gray tile --
    // and, with BLEND_8BIT_GRAYSCALE in blend_option, against a full 256-level
    // gray destination regardless of how this TU was compiled.
    (void)px; (void)py; (void)rb;
    int gray = rs_gray_of_b((unsigned char)cur, blend_option);
    surf->pixels[id] = rs_px_of_b(gray + ((alpha_fx * (255 - gray)) >> FIXED_SHIFT), blend_option);
#endif
}

// const RSurface*: these write the pixels, never the descriptor itself, so a
// caller holding a read-only surface (render_glyph_lowspec) can still draw.
//
// This bounds check is NOT redundant with aaline()'s endpoint clip, under
// either clipper. The default per-axis clamp can leave an endpoint hundreds of
// px outside the rect for a corner-grazing segment, and this check is the only
// thing stopping those pixels from corrupting the buffer. Even with
// USE_LB_CLIPPER (which bounds the geometry correctly), the Wu rasterizer still
// emits a companion pixel one step across the minor axis (iy+1 / ypxl+1) and
// rounds endpoints to nearest, so a plotted pixel can land exactly 1px past a
// clip edge -- on a packed tile (stride == clip_w) that is an out-of-buffer
// write. Either way, keep this check.
static inline void aaline_plot(const RSurface *surf, int x, int y, fixed alpha_fx,
                               int blend_option, const RBlend *rb) {
    if (x < surf->clip_x || x >= surf->clip_x + surf->clip_w ||
        y < surf->clip_y || y >= surf->clip_y + surf->clip_h) return;
    aaline_blend(surf, y * surf->stride + x, x, y, alpha_fx, blend_option, rb);
}


// Plots a single point at a sub-pixel (x, y), splaying `amount` over the 4
// surrounding integer pixels by bilinear coverage -- the 2D analog of
// aaline_plot's 1D endpoint split. Ported from a fixed-point splat routine
// that shared its x-weights (c1/c2) and y-weighted terms (fya/t1) across all
// 4 corners instead of recomputing per corner -- kept that here too (6
// fixed_muls total, not 12 from 3 independent per-corner products), calling
// aaline_blend directly rather than aaline_plot so the bounds check isn't
// redone 4 times either.
// One check up front, not 4: like the routine this came from, it trades
// exactness at the clip_w-2/clip_h-2 edge (2px margin instead of the tight
// 1px the x+1/y+1 corners actually need) for a single branch.
static inline void aapixel(const RSurface *surf, fixed x, fixed y, fixed amount, int blend_option) {
    x -= AALINE_PIXEL_CENTRE_BIAS;   // see AALINE_PIXEL_CENTRE_BIAS above
    y -= AALINE_PIXEL_CENTRE_BIAS;
    fixed fx = fixed_frac(x);
    fixed fy = fixed_frac(y);
    int ix = fixed_to_int(fixed_floor(x));
    int iy = fixed_to_int(fixed_floor(y));

    if (ix < surf->clip_x || iy < surf->clip_y ||
        ix >= surf->clip_x + surf->clip_w - 2 || iy >= surf->clip_y + surf->clip_h - 2)
        return;

    fixed c1  = FIXED_ONE - fx;              // left weight
    fixed c2  = fx;                          // right weight
    fixed fya = fixed_mul(fy, amount);
    fixed c3  = fixed_mul(fya, c1);          // bottom-left
    fixed c4  = fixed_mul(fya, c2);          // bottom-right
    fixed t1  = fixed_mul(FIXED_ONE - fy, amount);
    c1 = fixed_mul(c1, t1);                  // top-left
    c2 = fixed_mul(c2, t1);                  // top-right

    RBlend rb = rb_make(blend_option, -1, FIXED_ONE);
    int id = iy * surf->stride + ix;
    aaline_blend(surf, id,                    ix,     iy,     c1, blend_option, &rb);
    aaline_blend(surf, id + 1,                ix + 1, iy,     c2, blend_option, &rb);
    aaline_blend(surf, id + surf->stride,     ix,     iy + 1, c3, blend_option, &rb);
    aaline_blend(surf, id + surf->stride + 1, ix + 1, iy + 1, c4, blend_option, &rb);
}


static inline void aaline(const RSurface *surf, fixed x0, fixed y0, fixed a0, fixed x1, fixed y1, fixed a1, int blend_option) {
    // Resolved once for the whole segment. Alpha is FIXED_ONE here because the
    // caller's alpha is already folded into a0/a1, which arrive as per-pixel
    // coverage; the RBlend carries only the mode, the channel mask and the
    // destination encoding.
    RBlend rb = rb_make(blend_option, -1, FIXED_ONE);

    // See AALINE_PIXEL_CENTRE_BIAS above. Before the clip, so everything from
    // here down -- clipper included -- works in plain pixel-index space.
    x0 -= AALINE_PIXEL_CENTRE_BIAS; y0 -= AALINE_PIXEL_CENTRE_BIAS;
    x1 -= AALINE_PIXEL_CENTRE_BIAS; y1 -= AALINE_PIXEL_CENTRE_BIAS;

    fixed clip_x0 = fixed_from_int(surf->clip_x);
    fixed clip_y0 = fixed_from_int(surf->clip_y);
    fixed clip_x1 = fixed_from_int(surf->clip_x + surf->clip_w);
    fixed clip_y1 = fixed_from_int(surf->clip_y + surf->clip_h);

    fixed dx = x1 - x0;
    fixed dy = y1 - y0;

#define USE_LB_CLIPPER 1
#ifndef USE_LB_CLIPPER
    // ---- Original per-axis clamp -----------------------------------------
    // Clips x, then y, then x, then y in sequence. KNOWN LIMITATION: a later
    // axis can shove an earlier one back out of bounds -- a near-horizontal
    // segment grazing a corner can end up with an endpoint hundreds of px
    // outside the rect -- and the half-plane reject only catches segments
    // wholly past a *single* edge, so corner-grazing segments are mangled
    // rather than rejected. aaline_plot's per-pixel bounds check is what keeps
    // those stray pixels from corrupting the buffer. Define USE_LB_CLIPPER for
    // the parametric replacement that clips correctly (see the #else branch).
    if ((x0 < clip_x0 && x1 < clip_x0) ||
        (x0 >= clip_x1 && x1 >= clip_x1) ||
        (y0 < clip_y0 && y1 < clip_y0) ||
        (y0 >= clip_y1 && y1 >= clip_y1)) {
        return;
    }
    if (x0 < clip_x0) {
        fixed t = fixed_div(clip_x0 - x0, dx);
        y0 = y0 + fixed_mul(t, dy);
        a0 = a0 + fixed_mul(t, a1 - a0);
        x0 = clip_x0;
    }
    if (x0 >= clip_x1) {
        fixed t = fixed_div(clip_x1 - FIXED_ONE - x0, dx);
        y0 = y0 + fixed_mul(t, dy);
        a0 = a0 + fixed_mul(t, a1 - a0);
        x0 = clip_x1 - FIXED_ONE;
    }
    if (y0 < clip_y0) {
        fixed t = fixed_div(clip_y0 - y0, dy);
        x0 = x0 + fixed_mul(t, dx);
        a0 = a0 + fixed_mul(t, a1 - a0);
        y0 = clip_y0;
    }
    if (y0 >= clip_y1) {
        fixed t = fixed_div(clip_y1 - FIXED_ONE - y0, dy);
        x0 = x0 + fixed_mul(t, dx);
        a0 = a0 + fixed_mul(t, a1 - a0);
        y0 = clip_y1 - FIXED_ONE;
    }
    if (x1 < clip_x0) {
        fixed t = fixed_div(clip_x0 - x1, dx);
        y1 = y1 + fixed_mul(t, dy);
        a1 = a1 + fixed_mul(t, a1 - a0);
        x1 = clip_x0;
    }
    if (x1 >= clip_x1) {
        fixed t = fixed_div(clip_x1 - FIXED_ONE - x1, dx);
        y1 = y1 + fixed_mul(t, dy);
        a1 = a1 + fixed_mul(t, a1 - a0);
        x1 = clip_x1 - FIXED_ONE;
    }
    if (y1 < clip_y0) {
        fixed t = fixed_div(clip_y0 - y1, dy);
        x1 = x1 + fixed_mul(t, dx);
        a1 = a1 + fixed_mul(t, a1 - a0);
        y1 = clip_y0;
    }
    if (y1 >= clip_y1) {
        fixed t = fixed_div(clip_y1 - FIXED_ONE - y1, dy);
        x1 = x1 + fixed_mul(t, dx);
        a1 = a1 + fixed_mul(t, a1 - a0);
        y1 = clip_y1 - FIXED_ONE;
    }
#else
    // ---- Liang-Barsky parametric clip (USE_LB_CLIPPER) -------------------
    // Clip the segment to the writable pixel rect
    //   [clip_x0 .. clip_x1-ONE] x [clip_y0 .. clip_y1-ONE]
    // (the last writable pixel is clip_hi - ONE, not clip_hi). A single
    // parametric interval [t0,t1] is intersected against all four edges and
    // both endpoints are re-derived from it at once, so -- unlike the per-axis
    // clamp above -- a later axis can never shove an earlier one out of bounds,
    // and a segment that only grazes a corner without entering the rect is
    // rejected cleanly (t0 > t1). Overshoot past the clip rect drops from
    // hundreds of px to the irreducible 1px Wu AA spill, which aaline_plot's
    // guard still absorbs.
    if ((x0 < clip_x0 && x1 < clip_x0) ||
        (x0 >= clip_x1 && x1 >= clip_x1) ||
        (y0 < clip_y0 && y1 < clip_y0) ||
        (y0 >= clip_y1 && y1 >= clip_y1)) {
        return;   // cheap reject; also keeps the ratios below well-conditioned
    }

    // t0,t1 are kept in int64: for a near-parallel segment the ratio q/p is far
    // larger than Q16.16 can hold, so forming it as `fixed` (fixed_div) would
    // overflow and corrupt the interval. int64 holds any ratio; the t0 > t1
    // reject discards the out-of-range ones. Surviving t0,t1 are always in
    // [0, FIXED_ONE], so they fit `fixed` for the interpolation below.
    int64_t t0 = 0, t1 = FIXED_ONE;
    // p<0: entering edge -> raise t0; p>0: leaving edge -> lower t1; p==0 with
    // q<0: parallel and wholly outside -> reject.
    #define LB_CLIP(p, q) do {                                          \
        fixed _p = (p), _q = (q);                                       \
        if (_p == 0) { if (_q < 0) return; }                            \
        else { int64_t _r = ((int64_t)(_q) << FIXED_SHIFT) / (_p);      \
               if (_p < 0) { if (_r > t0) t0 = _r; }                    \
               else        { if (_r < t1) t1 = _r; } }                  \
    } while (0)
    LB_CLIP(-dx, x0 - clip_x0);                 // left
    LB_CLIP( dx, (clip_x1 - FIXED_ONE) - x0);   // right
    LB_CLIP(-dy, y0 - clip_y0);                 // top
    LB_CLIP( dy, (clip_y1 - FIXED_ONE) - y0);   // bottom
    #undef LB_CLIP
    if (t0 > t1) return;                        // never enters the rect
    {
        fixed nda = a1 - a0;
        fixed nx0 = x0 + fixed_mul((fixed)t0, dx);
        fixed ny0 = y0 + fixed_mul((fixed)t0, dy);
        fixed na0 = a0 + fixed_mul((fixed)t0, nda);
        x1 = x0 + fixed_mul((fixed)t1, dx);
        y1 = y0 + fixed_mul((fixed)t1, dy);
        a1 = a0 + fixed_mul((fixed)t1, nda);
        x0 = nx0; y0 = ny0; a0 = na0;
    }
#endif // USE_LB_CLIPPER

    int steep = fixed_abs(y1 - y0) > fixed_abs(x1 - x0);
    if (steep) {
        fixed tmp = x0; x0 = y0; y0 = tmp;
        tmp = x1; x1 = y1; y1 = tmp;
    }
    if (x0 > x1) {
        fixed tmp = x0; x0 = x1; x1 = tmp;
        tmp = y0; y0 = y1; y1 = tmp;
        tmp = a0; a0 = a1; a1 = tmp;
    }
    dx = x1 - x0;
    dy = y1 - y0;
    fixed gradient = (dx == 0) ? FIXED_ONE : fixed_div(dy, dx);

    fixed x0h = x0 + FIXED_HALF;
    fixed xend = fixed_floor(x0h);
    fixed yend = y0 + fixed_mul(gradient, xend - x0);
    fixed xgap = FIXED_ONE - fixed_frac(x0h);
    int xpxl1 = fixed_to_int(xend);
    int ypxl1 = fixed_to_int(fixed_floor(yend));
    fixed yfrac = fixed_frac(yend);
    if (steep) {
        aaline_plot(surf, ypxl1,     xpxl1, fixed_mul(fixed_mul(FIXED_ONE - yfrac, xgap), a0), blend_option, &rb);
        aaline_plot(surf, ypxl1 + 1, xpxl1, fixed_mul(fixed_mul(yfrac,             xgap), a0), blend_option, &rb);
    } else {
        aaline_plot(surf, xpxl1, ypxl1,     fixed_mul(fixed_mul(FIXED_ONE - yfrac, xgap), a0), blend_option, &rb);
        aaline_plot(surf, xpxl1, ypxl1 + 1, fixed_mul(fixed_mul(yfrac,             xgap), a0), blend_option, &rb);
    }
    fixed intery = yend + gradient;

    fixed x1h = x1 + FIXED_HALF;
    xend = fixed_floor(x1h);
    yend = y1 + fixed_mul(gradient, xend - x1);
    xgap = fixed_frac(x1h);
    int xpxl2 = fixed_to_int(xend);
    int ypxl2 = fixed_to_int(fixed_floor(yend));
    yfrac = fixed_frac(yend);
    if (steep) {
        aaline_plot(surf, ypxl2,     xpxl2, fixed_mul(fixed_mul(FIXED_ONE - yfrac, xgap), a1), blend_option, &rb);
        aaline_plot(surf, ypxl2 + 1, xpxl2, fixed_mul(fixed_mul(yfrac,             xgap), a1), blend_option, &rb);
    } else {
        aaline_plot(surf, xpxl2, ypxl2,     fixed_mul(fixed_mul(FIXED_ONE - yfrac, xgap), a1), blend_option, &rb);
        aaline_plot(surf, xpxl2, ypxl2 + 1, fixed_mul(fixed_mul(yfrac,             xgap), a1), blend_option, &rb);
    }

    int span = xpxl2 - xpxl1;
    if (span > 0) {
        // Do NOT re-derive the alpha ramp per pixel as
        //   fixed_div(fixed_from_int(x - xpxl1), fixed_from_int(span))
        // -- that is a 64-bit division (`(int64_t)a << 16 / (int64_t)b`, see
        // fixed_div above) in the innermost loop of the whole renderer. No
        // Cortex-M part has a 64-bit divider, so on Pebble it costs an
        // __aeabi_ldivmod CALL per plotted pixel, and outline-mode glyphs (the
        // only mode that fits Pebble's scratch) are made of nothing but these
        // spans.
        //
        // It is a straight line in `a`, so step it: one 32-bit divide hoisted
        // out, one add per pixel.
        //
        // The residual is below the destination's precision, so this is not an
        // approximation in practice: truncation accumulates to under `span`
        // Q16.16 units across the whole span, i.e. under span/65536 of full
        // coverage, which cannot move a 0-255 byte (let alone a 2-bit GColor8
        // channel). Checked differentially against the divide-per-pixel version
        // over 40k random clipped lines, both encodings, both a0 == a1 and
        // a0 != a1: byte-identical output, zero pixels differing. (a0 == a1 is
        // what every caller in this tree passes anyway -- aaline_polygon and
        // render_line hand the same alpha to both endpoints -- so the ramp is
        // usually flat and `da` is exactly 0.)
        fixed da = (a1 - a0) / span;
        fixed a  = a0;
        for (int x = xpxl1 + 1; x < xpxl2; ++x) {
            a += da;
            fixed fy = fixed_floor(intery);
            fixed f = fixed_frac(intery);
            int iy = fixed_to_int(fy);
            if (steep) {
                aaline_plot(surf, iy,     x, fixed_mul(FIXED_ONE - f, a), blend_option, &rb);
                aaline_plot(surf, iy + 1, x, fixed_mul(f,             a), blend_option, &rb);
            } else {
                aaline_plot(surf, x, iy,     fixed_mul(FIXED_ONE - f, a), blend_option, &rb);
                aaline_plot(surf, x, iy + 1, fixed_mul(f,             a), blend_option, &rb);
            }
            intery += gradient;
        }
    }
}

// Strokes a closed polygon as AA line segments: n *distinct* vertices as
// interleaved Q16.16 pairs (pts_xy[2*i], pts_xy[2*i+1]), wrapping the last
// back to the first -- the same "open contour, caller doesn't repeat the
// first point" convention render_polygon.h's gr_add_contour uses for fill, so
// the two are interchangeable: draw a GrPath's contour as a wireframe by
// passing the identical point array here instead. org_x/org_y place the
// (tile-local) polygon in `surf`, same meaning as gr_rasterize's org_x/org_y
// (0,0 if pts_xy is already in buffer space).
//
// alpha is the Q16.16 coverage every segment is drawn at (FIXED_ONE = opaque),
// multiplied into the Wu endpoint/span weights exactly as aaline's own a0/a1
// -- it is taken literally here, so alpha 0 draws nothing. The <=0-means-
// opaque sentinel the script-facing wrappers use (render_line, render_ellipse)
// is theirs to normalize before calling in. blend_option is forwarded
// unchanged to aaline (only consulted under AALINE_PLOT_4_X_64).
static inline void aaline_polygon(const RSurface *surf, const fixed *pts_xy, int n,
                                  int org_x, int org_y, fixed alpha, int blend_option) {
    if (n < 2) return;
    fixed ox = fixed_from_int(org_x);
    fixed oy = fixed_from_int(org_y);
    for (int i = 0; i < n; i++) {
        int j = (i + 1 == n) ? 0 : i + 1;
        aaline(surf, pts_xy[2*i] + ox, pts_xy[2*i + 1] + oy, alpha,
                     pts_xy[2*j] + ox, pts_xy[2*j + 1] + oy, alpha, blend_option);
    }
}

#endif // RENDER_AALINE_H
