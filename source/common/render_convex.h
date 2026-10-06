#ifndef RENDER_CONVEX_H
#define RENDER_CONVEX_H

// ===================================================================
// An antialiased fill for CONVEX polygons -- quads, triangles, pentagons,
// hexagons, an ellipse boundary. One function, because every one of those is
// the same shape to a scanline: a left chain and a right chain, both monotone
// in y, with exactly one span between them on every scanline.
//
// WHY, when render_polygon.h already fills polygons. gr_rasterize is a general
// rasterizer -- self-intersection, multiple contours, nonzero/union winding --
// and pays for that generality on every row: it activates edges out of a
// y-sorted list, insertion-sorts the crossings, walks them in pairs, and
// accumulates into a caller-provided scratch buffer. For a thick line's quad
// all of that machinery resolves to THE SAME TWO EDGES on every sub-scanline,
// and to a sort over two elements.
//
// What convexity buys, in order of how much it is worth:
//
//   1. THE INTERIOR STOPS BEING AN AA PROBLEM. On a given row, the pixels
//      covered by EVERY sub-scanline are at constant full coverage -- which is
//      a Tier B draw, i.e. one indexed load per pixel out of a table that is
//      already built and cached (RBlendCache, common/render_blend.h). Only the
//      1-2 px fringe at each end pays per-pixel coverage. For a stroke-3 line
//      that is ~4 fringe pixels a row against a core costing what a hard-edged
//      triangle fill costs. This is the point of the file; everything else is
//      a bonus.
//
//   2. NO EDGE LIST, NO SORT. Two chains, each advanced by comparing y against
//      the current edge's lower endpoint, and one multiply per sub-scanline to
//      get its x. The inverse slope is computed ONCE per edge, when a chain
//      steps onto it -- not once per sub-scanline, which is the waste the
//      general path pays.
//
//   3. NO SCRATCH AT ALL. gr_rasterize needs a coverage row plus edge tables,
//      which is the reason render_line and render_ellipse carry scratch
//      pointers. Here the only per-row state is the S sub-scanline spans, in
//      two fixed stack arrays (64 bytes at S=8). A fringe pixel is resolved by
//      summing those S spans at that one column, rather than by accumulating
//      every span into a buffer and reading it back.
//
// WHAT IT DOES NOT DO: concave or self-intersecting outlines. A caller that
// cannot promise convexity wants gr_fill_polygon, which winds properly. There
// is no check here -- the promise is the caller's, exactly as it is for
// triangle_fill_convex_polygon.
//
// UNIFIED BLEND ONLY. This is new code, so it is written against
// common/render_blend.h directly and has no legacy arm. Nothing here consults
// USE_UNIFIED_BLEND.
//
// Alternatives weighed and rejected: a fan of watertight triangles plus an
// outer-half AA line, and full signed-distance coverage.
// ===================================================================

#include <stdint.h>
#include "common/render_surface.h"

#ifndef fixed
typedef int32_t fixed;
#endif

// Vertical supersampling. Defaults to POLY_SUBSAMPLES when render_polygon.h is
// already in scope, which is what keeps this filler's output directly
// comparable to gr_fill_polygon's: the two then quantize coverage identically,
// so a differential test between them measures the RASTERIZER and not a
// sampling-rate difference. The low-memory TU sets POLY_SUBSAMPLES to 2 and
// this follows it there, for the same reason.
#ifndef CONVEX_SUBSAMPLES
#ifdef POLY_SUBSAMPLES
#define CONVEX_SUBSAMPLES POLY_SUBSAMPLES
#else
#define CONVEX_SUBSAMPLES 8
#endif
#endif

// Points arrive as interleaved Q16.16 pairs in BUFFER space -- the same layout
// and the same convention as gr_fill_polygon's pts_xy, aaline_polygon's and
// triangle_fill_convex_polygon's, so one array can be filled, AA-filled or
// stroked interchangeably. No org_x/org_y: unlike gr_rasterize this does not
// work in a tile, so there is no origin to add (and dithering, which needs
// absolute coordinates, gets them for free).
//
// Coordinate v is a pixel BOUNDARY -- pixel i covers [i, i+1), centre i+0.5 --
// which is render_polygon.h's convention natively. Note this file needs NO
// half-pixel bias: render_aaline.h and render_triangle.h subtract one because
// their rasterizers were written to the other convention (see
// AALINE_PIXEL_CENTRE_BIAS), and this one was not.
#define CVX_X(p, i) ((fixed)(p)[2 * (i)])
#define CVX_Y(p, i) ((fixed)(p)[2 * (i) + 1])

// ===================================================================
// One chain of the outline: the run of edges from the topmost vertex to the
// bottommost, in one direction around the ring. Convexity is what makes it
// monotone in y, and monotonicity is what lets the walk only ever move
// forward -- so covering the whole shape costs O(n) edge setups in total,
// not O(n) per row.
// ===================================================================
typedef struct {
    const int *pts;
    int   n, dir, stop;   // ring size, +1/-1 direction, the bottom vertex
    int   at;             // vertex at the LOWER end of the current edge
    fixed ax, ay;         // the current edge's UPPER endpoint
    fixed by;             // y of its lower endpoint
    fixed inv;            // dx/dy of the current edge, Q16.16
    int   last;           // this is the final edge; never advance past it
} CvxChain;

// Load the edge leaving `at`. A zero-height edge (a horizontal top or bottom)
// gets inv = 0 and by == ay, so cvx_at's advance loop steps straight over it
// and its dy is never a divisor.
static void cvx_step(CvxChain *c) {
    int from = c->at;
    int to   = from + c->dir;
    fixed dy;
    if (to < 0)       to = c->n - 1;
    else if (to >= c->n) to = 0;
    c->ax = CVX_X(c->pts, from);
    c->ay = CVX_Y(c->pts, from);
    c->by = CVX_Y(c->pts, to);
    dy    = c->by - c->ay;
    // ONE divide per edge, not one per sub-scanline. At S = 8 that is the
    // difference between ~2 divides a row and ~16.
    c->inv = dy ? (fixed)((((int64_t)(CVX_X(c->pts, to) - c->ax)) << 16) / dy) : 0;
    c->at   = to;
    c->last = (to == c->stop);
}

static void cvx_open(CvxChain *c, const int *pts, int n, int top, int bottom, int dir) {
    c->pts = pts; c->n = n; c->dir = dir; c->stop = bottom;
    c->at = top; c->last = 0;
    cvx_step(c);
}

// This chain's x at sub-scanline `ys`, advancing edges as needed. Callers must
// present ys in non-decreasing order -- which the row loop does by
// construction, and which is the whole reason the walk is O(n).
static fixed cvx_at(CvxChain *c, fixed ys) {
    while (!c->last && ys >= c->by) cvx_step(c);
    return c->ax + (fixed)((((int64_t)(ys - c->ay)) * c->inv) >> 16);
}

// ===================================================================
// The coverage a sub-scanline span [xs, xe] contributes to column x, 0..255.
//
// Written to reproduce gr_rasterize's accumulation EXACTLY -- the `>> 8 & 0xFF`
// fractional position, the `255 - f1` / `255` / `f2` split, and its handling of
// a span that starts and ends inside one pixel. Not because that arithmetic is
// ideal (255 where 256 would be exact costs a fraction of a level), but because
// matching it is what lets a differential test against gr_fill_polygon show
// only the differences this rasterizer MEANT to introduce.
// ===================================================================
static inline int cvx_span_cov(fixed xs, fixed xe, int x) {
    int ix1 = (int)(xs >> 16);          // arithmetic shift == floor, incl. negatives
    int ix2 = (int)(xe >> 16);
    int f1, f2;
    if (x < ix1 || x > ix2) return 0;
    f1 = (int)(xs >> 8) & 0xFF;
    f2 = (int)(xe >> 8) & 0xFF;
    if (ix1 == ix2) return f2 - f1;     // both ends inside one pixel
    if (x == ix1)   return 255 - f1;
    if (x == ix2)   return f2;
    return 255;
}

#if RS_PEBBLE_TIME2
// One fringe pixel on a GColor8 destination, blended PER CHANNEL.
//
// This is rb_lut_chan -- the very function rb_lut_make tabulates -- evaluated
// inline instead of looked up, because the fringe's coverage varies per pixel
// and a table is indexed by destination byte alone.
//
// It would be ~3x cheaper to call rb_px here, which is what gr_rasterize does.
// It is deliberately not: rb_px is Tier A, a SINGLE-CHANNEL path (it reads the
// destination's blue channel as the level and writes all three channels to the
// result -- see the note on col_eff in rb_make), so it desaturates. Against a
// Tier B core, which blends each channel toward its own target, that would put
// a one-level hue step exactly along the shape's edge on any coloured
// background at alpha < 1 -- a visible rim in the one place AA exists to make
// smooth. Paying 3x on 2-4 pixels a row to keep one colour model throughout is
// the right side of that trade; on a 200 px row it is under 2% of the row.
static inline unsigned char cvx_px(const RBlend *b, unsigned int cur, int a, int dth,
                                   int cr, int cg, int cb) {
    int r = rb_lut_chan(b, (int)((cur >> 4) & 3) * 85, cr, a, dth);
    int g = rb_lut_chan(b, (int)((cur >> 2) & 3) * 85, cg, a, dth);
    int u = rb_lut_chan(b, (int)( cur       & 3) * 85, cb, a, dth);
    unsigned char out = (unsigned char)(0xC0 | (r << 4) | (g << 2) | u);
    return (unsigned char)((out & b->mask) | ((unsigned char)cur & b->nmask));
}
#define CVX_PX(b, cur, a, x, y) cvx_px((b), (cur), (a), RB_LUT_DITHER_AT((x), (y)), cr, cg, cbl)
#elif RB_LAYOUTS
// The runtime-layout twin: rb_px_chan is what rb_lut_make_l tabulates.
#define CVX_PX(b, cur, a, x, y) rb_px_chan((b), (cur), (a))
#endif

// ===================================================================
// Fill a convex polygon, antialiased.
//
// `pts_xy`  n distinct vertices, interleaved Q16.16, auto-closed -- do NOT
//           repeat the first point. Winding may be either way.
// `alpha`   Q16.16 coverage, taken literally: 0 draws nothing, FIXED_ONE is
//           full strength. Same rule as everything else in this tree.
// `blend`   the usual packed int (mode bits, channel locks,
//           BLEND_8BIT_GRAYSCALE).
// `bc`      the Tier B cache, or 0 for the caller-has-none path. Supplying one
//           that outlives the draw is what keeps a small shape from paying
//           ~3.5k cycles to build a table it uses for 90 pixels.
//
// const RSurface*: writes the pixels, never the descriptor, so a caller holding
// a read-only surface can still draw (same as aaline and triangle_fill).
// ===================================================================
static void convex_fill_aa(const RSurface *surf, const int *pts_xy, int n,
                           fixed alpha, int blend, RBlendCache *bc) {
    const int S = CONVEX_SUBSAMPLES;
    fixed xl[CONVEX_SUBSAMPLES], xr[CONVEX_SUBSAMPLES];
    fixed yoff[CONVEX_SUBSAMPLES];
    CvxChain ca, cb;
    RBlend rb;
    fixed ytop, ybot;
    int top = 0, bot = 0, i, y, y0, y1;
    int cx0, cx1, cy0, cy1;
#if RS_PEBBLE_TIME2 || RB_LAYOUTS
    // Everything the Tier B half needs. An 8-bit gray destination reaches none
    // of it -- no table, no per-channel targets -- so it is not merely unused
    // there, it is inapplicable.
    const RBlendLut *lut;
    RBlendCache local;
    int a8;
#endif
#if RS_PEBBLE_TIME2
    int cr, cg, cbl;
#endif

    if (n < 3 || alpha <= 0) return;
    if (alpha > 65536) alpha = 65536;

    for (i = 1; i < n; i++) {
        if (CVX_Y(pts_xy, i) < CVX_Y(pts_xy, top)) top = i;
        if (CVX_Y(pts_xy, i) > CVX_Y(pts_xy, bot)) bot = i;
    }
    ytop = CVX_Y(pts_xy, top);
    ybot = CVX_Y(pts_xy, bot);
    if (ybot <= ytop) return;                       // zero height: nothing to fill

    cx0 = surf->clip_x; cx1 = surf->clip_x + surf->clip_w;
    cy0 = surf->clip_y; cy1 = surf->clip_y + surf->clip_h;

    y0 = (int)(ytop >> 16);                         // floor
    y1 = (int)((ybot + 0xFFFF) >> 16);              // ceil
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y0 >= y1) return;

    // Sub-scanline offsets within a row: (s + 0.5) / S, matching gr_rasterize's
    // y_off_arr exactly.
    for (i = 0; i < S; i++)
        yoff[i] = (fixed)((i * 65536) / S + 65536 / (2 * S));

    // Both chains run from the top vertex to the bottom one, one each way
    // around the ring. WHICH is left and which is right depends on winding, and
    // nothing here needs to know: the row loop takes min and max of the two,
    // which costs one compare and makes the function winding-agnostic.
    cvx_open(&ca, pts_xy, n, top, bot, +1);
    cvx_open(&cb, pts_xy, n, top, bot, -1);

    // Per-draw blend state. The RBlend carries the mode, the channel mask and
    // the draw's alpha (so the fringe gets alpha folded in for free, exactly as
    // gr_rasterize does); the table carries the core, whose coverage is the
    // constant `alpha` and nothing else.
    rb = RB_MAKE(surf, blend, -1, alpha);
#if RS_PEBBLE_TIME2 || RB_LAYOUTS
    if (!bc) { rb_cache_reset(&local); bc = &local; }
    a8  = rb.alpha8;
    lut = RB_LUT_GET(bc, surf, blend, -1, a8);
#else
    (void)bc;
#endif
#if RS_PEBBLE_TIME2
    cr  = ((rb.col_raw >> 4) & 3) * 85;
    cg  = ((rb.col_raw >> 2) & 3) * 85;
    cbl = ( rb.col_raw       & 3) * 85;
#endif

    for (y = y0; y < y1; y++) {
        unsigned char *row = surf->pixels + (size_t)y * surf->stride;
        int nvalid = 0;
        int loL = 0, hiL = 0, loR = 0, hiR = 0;
        int core0, core1, frin0, frin1, x, s;

        for (s = 0; s < S; s++) {
            fixed ys = (fixed)(((int64_t)y << 16)) + yoff[s];
            fixed a, b, l, r;
            int il, ir;
            // Above the topmost vertex or below the bottommost: this
            // sub-scanline misses the shape entirely. Skipping it WITHOUT
            // touching the chains is what keeps their walk monotone.
            if (ys < ytop || ys >= ybot) { xl[s] = 1; xr[s] = 0; continue; }
            a = cvx_at(&ca, ys);
            b = cvx_at(&cb, ys);
            l = a < b ? a : b;
            r = a < b ? b : a;
            xl[s] = l; xr[s] = r;
            il = (int)(l >> 16);
            ir = (int)(r >> 16);
            if (nvalid == 0) {
                loL = hiL = il;
                loR = hiR = ir;
            } else {
                // One statement per line: the ARM build compiles
                // -Werror=misleading-indentation, and two `if`s sharing a line
                // is exactly what that flag exists to reject.
                if (il < loL) loL = il;
                if (il > hiL) hiL = il;
                if (ir < loR) loR = ir;
                if (ir > hiR) hiR = ir;
            }
            nvalid++;
        }
        if (!nvalid) continue;

        // A column is at FULL coverage exactly when every sub-scanline's span
        // strictly contains it -- i.e. it is past the rightmost left-edge pixel
        // and before the leftmost right-edge pixel. That is cvx_span_cov's
        // `return 255` case, expressed once for the whole row instead of
        // S times per pixel.
        //
        // A row with any missing sub-scanline (the first and last row of the
        // shape) has no full column at all, however wide it looks.
        core0 = hiL + 1;
        core1 = loR;
        if (nvalid < S) core0 = core1 = 0;
        frin0 = loL;
        frin1 = hiR + 1;

        // Clip, then order. BOTH ends of the fringe need both bounds: a shape
        // entirely to the RIGHT of the clip rect leaves frin0 above cx1, and
        // clamping only frin0-up-to-cx0 and frin1-down-to-cx1 leaves
        // frin0 > frin1 -- harmless for a single loop bounded by frin1, but the
        // GColor8 arm below runs THREE loops and its first is bounded by core0,
        // which is not frin1. That wrote past the row end and spilled into the
        // next row. Hence the explicit empty-row exit, and hence core0/core1
        // being pinned inside [frin0, frin1] rather than merely ordered.
        if (frin0 < cx0) frin0 = cx0;
        if (frin1 > cx1) frin1 = cx1;
        if (frin1 <= frin0) continue;          // no part of this row is writable
        if (core0 < frin0) core0 = frin0;
        if (core0 > frin1) core0 = frin1;
        if (core1 > frin1) core1 = frin1;
        if (core1 < core0) core1 = core0;

        // --- fringe left, core, fringe right -----------------------------
        // Written as three loops rather than one loop with a branch: the core
        // loop is the one that has to stay tight, and hoisting the test out of
        // it is the entire reason for computing core0/core1 above.
#if RS_PEBBLE_TIME2 || RB_LAYOUTS
        if (lut) {
            for (x = frin0; x < core0; x++) {
                int acc = 0, cov;
                for (s = 0; s < S; s++) acc += cvx_span_cov(xl[s], xr[s], x);
                if (acc <= 0) continue;
                cov = (acc + S / 2) / S;                    // gr_rasterize's RB_COV8
                row[x] = CVX_PX(&rb, row[x], (a8 == 255) ? cov : (cov * a8) / 255, x, y);
            }
            for (x = core0; x < core1; x++)
                row[x] = RB_LUT_AT(lut, x, y, row[x]);
            for (x = core1; x < frin1; x++) {
                int acc = 0, cov;
                for (s = 0; s < S; s++) acc += cvx_span_cov(xl[s], xr[s], x);
                if (acc <= 0) continue;
                cov = (acc + S / 2) / S;
                row[x] = CVX_PX(&rb, row[x], (a8 == 255) ? cov : (cov * a8) / 255, x, y);
            }
            continue;
        }
#endif
        // 8-bit gray destination (BLEND_8BIT_GRAYSCALE, or any !RS_PEBBLE_TIME2
        // build): 256 levels, so there is no table and no per-channel question
        // -- one closed form covers core and fringe alike, and no seam is
        // possible between them.
        for (x = frin0; x < frin1; x++) {
            int cov;
            if (x >= core0 && x < core1) cov = 255;
            else {
                int acc = 0;
                for (s = 0; s < S; s++) acc += cvx_span_cov(xl[s], xr[s], x);
                if (acc <= 0) continue;
                cov = (acc + S / 2) / S;
            }
#if RB_LAYOUTS
            // BLEND_REPLACE has no table either; blend it rather than write gray.
            if (!rb.gray8) { row[x] = rb_px_ramp(&rb, row[x], cov); continue; }
#endif
            row[x] = rb_px_gray(&rb, row[x], cov);
        }
    }
}

#endif // RENDER_CONVEX_H
