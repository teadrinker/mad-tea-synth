#ifndef RENDER_POLYGON_H
#define RENDER_POLYGON_H

// ===================================================================
// Scanline poly-renderer. Self-contained (no libc/malloc/tsys); caller
// supplies all buffers via GrRasterBuffers; coords are 16.16 fixed point.
// All-`static`, #include into one TU.
//
// Compile-time options (define before including):
//   POLY_MAX_EDGES      max edges per path                       (16384)
//   POLY_SUBSAMPLES     vertical supersampling factor            (8)
//   POLY_MAX_CROSSINGS  max edge crossings per scanline          (32)
//   POLY_UNION_FILL     1 = non-zero winding, 0 = even-odd       (1)
//   POLY_SKIP_EMPTY_ROWS   restrict the sweep to the path's own rows  (off)
//   POLY_BUCKET_SORT       counting-sort the edge list by row         (off)
//   POLY_FAST_ENTRY_X      multiply instead of divide at activation   (off)
//   POLY_SLOPE_DIV(dx,dy)  hook for a faster fixed-point divide
// The first two are pure speed levers, off by default so a build opts in
// deliberately -- see each one's comment at its use site for what it costs.
// POLY_FAST_ENTRY_X defaults off because it is the one option here that
// changes output (by <= 1/65536 px, i.e. a sub-level shift in an AA fringe);
// POLY_SLOPE_DIV changes nothing by itself. The low-memory build turns
// POLY_FAST_ENTRY_X on -- see font/render_lowspec_lowmem.c for why the libgcc
// call it removes was worth the fringe.
//
// Two speed properties are NOT optional, because they are also correctness:
// the sweep never touches a pixel no edge covered (an uncovered pixel has no
// business being rewritten -- see POLY_ZERO_COV_SKIP), and the per-row work is
// proportional to the span the path actually covered rather than to the tile
// width (see the dirty range in gr_rasterize).
// The destination pixel format is RS_PEBBLE_TIME2, in render_surface.h --
// it is shared with aaline.h rather than owned by this file.
//
// gr_rasterize draws a tile-local path into an RSurface (common/render_
// surface.h) at an arbitrary origin, clipping to the surface's clip rect.
// For a plain packed width*height tile, pass rs_tile(buf, w, h) at (0,0).
// ===================================================================

#include "common/render_surface.h"
#include "common/scratch_alloc.h"

#ifndef POLY_MAX_EDGES
#define POLY_MAX_EDGES      16384
#endif

#ifndef POLY_SUBSAMPLES
#define POLY_SUBSAMPLES     8
#endif

#ifndef POLY_MAX_CROSSINGS
#define POLY_MAX_CROSSINGS  32
#endif

#ifndef POLY_UNION_FILL
#define POLY_UNION_FILL     1
#endif

// Speed levers, all opt-in (see the option list at the top of this file).
// Declared as 0/1 rather than #ifdef so a build can turn one back off with
// -DPOLY_BUCKET_SORT=0 without editing the file that switched it on.
#ifndef POLY_BUCKET_SORT
#define POLY_BUCKET_SORT    0
#endif
#ifndef POLY_SKIP_EMPTY_ROWS
#define POLY_SKIP_EMPTY_ROWS 0
#endif

// The edge-slope divide, dx/dy in 16.16 -- i.e. exactly fx16_div(dx, dy). One
// per edge (see edge_step in GrRasterBuffers), and on a core with no 64-bit
// divide it is a call to libgcc's fully general __aeabi_ldivmod.
//
// A hook rather than an #include: this header is self-contained on purpose, and
// common/math_fixedp.h is not something a caller should have to drag in to use
// it. A target that already has a faster divide points this at it --
//   #define POLY_SLOPE_DIV(dx, dy) poly_fx16_slope((dx), (dy))
// -- where the target's wrapper falls back to the form below whenever the
// quotient will not fit 32 bits (fx16_div's own FX16_DIV_FAST32 tests the same
// condition, `|a| < |b| << 16`). That fallback costs nothing in practice: a
// quotient past 32 bits means dy < |dx| * 2^-15, an edge far too shallow to
// cross two sample rows, and such an edge's step is never read (it is dropped
// the row after it activates).
#ifndef POLY_SLOPE_DIV
#define POLY_SLOPE_DIV(dx, dy) ((long long)(dx) * 65536 / (long long)(dy))
#endif

// Derive an activating edge's entry x by multiplying its precomputed slope
// instead of dividing again. See the use site for the exactness argument -- it
// is the same rational either way, but truncation lands elsewhere, so the
// result can differ by one 16.16 unit (1/65536 px). That cannot move a coverage
// byte except at an exact tie, but it is a difference, which is why this is the
// one option in this file that is off by default rather than on.
#ifndef POLY_FAST_ENTRY_X
#define POLY_FAST_ENTRY_X 0
#endif

// ===================================================================
// COORDINATE CONVENTION -- this sweep is the reference.
//
// Coordinate v is a pixel BOUNDARY: pixel i covers [i, i+1) and is sampled at
// its centre, i + 0.5. So a fill of [3, 8) lands crisply on pixels 3..7, and
// rect(2,2,2,1) covers exactly the same pixels as a 1px-wide fill of the band
// y in [2,3), x in [2,4).
//
// render_aaline.h and render_triangle.h were each written to the OTHER
// convention (coordinate v at pixel INDEX v) and now carry a half-pixel bias
// at their entry points to match this one -- see AALINE_PIXEL_CENTRE_BIAS and
// TRI_PIXEL_CENTRE_BIAS. Nothing in this file changed; it is what the other
// two were aligned TO.
// ===================================================================

// ===================================================================
// Types
// ===================================================================

typedef struct {
    int x0, y0, x1, y1;  // 16.16 fixed point, y0 <= y1
    int dir;             // union fill: +1 for up, -1 for down
} GrEdge;

typedef struct GrPath {
    GrEdge edges[POLY_MAX_EDGES];
    int edge_count;
    int cur_x, cur_y;  // 16.16 fixed point
} GrPath;

// Caller-owned scratch for gr_rasterize (active-edge-table sweep -- only one
// scanline's state is live, nothing scales with height). Sizes in elements
// (E = edge_count <= POLY_MAX_EDGES, K = POLY_MAX_CROSSINGS):
//   coverage: width;  edge_order: E;  active_count/next_idx: POLY_SUBSAMPLES;
//   active_x/step/end/dir (dir: POLY_UNION_FILL only): POLY_SUBSAMPLES*K
//   row_start (POLY_BUCKET_SORT only): height + 2;  edge_step: E
typedef struct {
    int *coverage;
    int *edge_order;
#if POLY_BUCKET_SORT
    // Counting-sort buckets, height+2 ints. May be left NULL -- gr_rasterize
    // falls back to the insertion sort, so a caller that hasn't been taught
    // about this field still renders, just slower.
    int *row_start;
#endif
    // Per-edge slope dx/dy in 16.16, E entries, indexed by EDGE (not by
    // edge_order position). Optional, same deal as row_start: NULL means
    // gr_rasterize computes each slope inline the way it always did.
    //
    // Worth the 8*E bytes because the slope does not depend on the subsample --
    // totalDy reduces to y1 - y0, with y_off cancelling -- so without this the
    // same divide runs POLY_SUBSAMPLES times per edge for the same answer.
    long long *edge_step;
    long long *active_x;
    long long *active_step;
    int *active_end;
#if POLY_UNION_FILL
    int *active_dir;
#endif
    int *active_count;
    int *next_idx;
} GrRasterBuffers;

// ===================================================================
// Path building
// ===================================================================

static void gr_path_init(GrPath* p) {
    p->edge_count = 0;
    p->cur_x = 0;
    p->cur_y = 0;
}

static void gr_move_to(GrPath* p, int x, int y) {
    p->cur_x = x;
    p->cur_y = y;
}

static void gr_line_to(GrPath* p, int x, int y) {
    if (p->edge_count >= POLY_MAX_EDGES) return;
    int x0 = p->cur_x, y0 = p->cur_y;
    int x1 = x, y1 = y;
    if (y0 != y1) {
        int dir = (y1 > y0) ? 1 : -1;      // union fill
        if (y0 > y1) { int t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; }
        p->edges[p->edge_count].x0 = x0;
        p->edges[p->edge_count].y0 = y0;
        p->edges[p->edge_count].x1 = x1;
        p->edges[p->edge_count].y1 = y1;
        p->edges[p->edge_count].dir = dir; // union fill
        p->edge_count++;
    }
    p->cur_x = x;
    p->cur_y = y;
}

// ===================================================================
// Rasterization
// ===================================================================

// Leave pixels no edge covered alone. Unconditional, and it is a correctness
// rule before it is a speed one: a pixel the path did not cover has no business
// being rewritten.
//
// For an 8-bit gray destination, skipping is exactly identity on both blend
// paths: the legacy arm computes gray + (0 * reach) / divisor == gray, and
// rb_px_gray reduces to v = d.
//
// Under RS_PEBBLE_TIME2 it is identity only for a pixel that is ALREADY gray,
// and this catches BOTH paths -- which the flag's old comment got wrong, so
// measure before believing either of us:
//   * legacy: rs_gray_of_b reads the BLUE channel alone and rs_px_of_b writes
//     all three channels from it.
//   * unified: rb_px at cov == 0 resolves to enc[cur & 3], and rb_make builds
//     enc[i] as 0xC0 | (i<<4) | (i<<2) | i -- also all three channels from the
//     blue level, and also forcing the alpha bits opaque.
// So blending zero over a pixel whose channels differ DESATURATES it either
// way. That is what made a diagonal line leave a gray rectangle the size of its
// bounding box across coloured content, on every build.
//
// This was an opt-in flag (POLY_SKIP_ZERO_COVERAGE) precisely because it
// changes those pixels; it is unconditional now because that change is the fix.
//
// NO do/while(0) WRAPPER, deliberately, and it is not a style slip: `continue`
// inside a do/while(0) binds to the do/while. It jumps to the `while (0)`,
// falls straight out, and execution lands on the next statement in the loop
// body -- i.e. the blend it was supposed to skip. The flag this replaced
// carried that wrapper and so had never skipped a single pixel in any build
// that set it; the two Pebble TUs had it on and were paying for it in .text
// while getting nothing. Do not "fix" the missing wrapper.
//
// Safe without one because every use is a standalone statement at the top of a
// loop body -- it can never end up as the body of an `if` without braces.
#define POLY_ZERO_COV_SKIP(cov) if (!(cov)) continue

// Widen a row's dirty range to cover the span [a, b] about to be accumulated
// into coverage[], so the blit and the re-clear that follow can run over the
// covered span instead of the whole tile width.
//
// Two compares and no clamping: this runs per span per subsample per row, which
// on a narrow tile is the only thing the dirty range ADDS and there is no wasted
// span there for it to remove. Clamping to the tile happens once per row
// instead, at POLY_ROW_DIRTY -- the bounds are inclusive and raw until then, so
// this side stays as cheap as tracking a min and a max can be.
#define POLY_MARK_SPAN(lo, hi, a, b) do {                 \
        if ((a) < (lo)) (lo) = (a);                       \
        if ((b) > (hi)) (hi) = (b);                       \
    } while (0)

// Turn a row's raw inclusive [lo, hi] into a clamped half-open [d0, d1), the
// range the blit and the clear actually walk. Empty (d1 <= d0) when the row
// marked nothing, or when everything it marked fell outside the tile.
#define POLY_ROW_DIRTY(lo, hi, w, d0, d1) do {            \
        (d0) = (lo) < 0 ? 0 : (lo);                       \
        (d1) = (hi) + 1;                                  \
        if ((d1) > (w)) (d1) = (w);                       \
    } while (0)

// Rasterize the path into `surf`. Path coords are tile-local: the tile is
// width*height and its (0,0) lands at surf pixel (org_x, org_y) -- which may
// be negative or off the edge, the clip rect sorts it out. Caller supplies
// all scratch via `buf`. blending_flags is a coverage threshold: pixels with
// alpha (0-255) below it are left untouched (0 = write every pixel), so
// background shows through a fill.
//
// `alpha` is a Q16.16 coverage multiplier for the whole path: 65536 (1.0) is
// the historical full-strength fill, values below it scale every pixel's
// coverage down proportionally, and <= 0 draws nothing at all. Same convention
// as aaline_polygon's alpha, so a shape reads the same whether it is filled
// here or stroked there.
//
// Tile-local coords are load-bearing, not incidental: `coverage` is sized to
// the tile width and the sweep runs over tile rows, which is what keeps the
// footprint independent of the destination buffer's size.
//
// Active-edge-table sweep, row-major: only one row's coverage + active edges
// (<= POLY_MAX_CROSSINGS) are live at once, independent of height (an older
// tile-sized version OOM'd on Pebble for big glyphs). Needs edges in
// ascending top-y order -- one sort by unshifted y0 serves every subsample,
// since each y_off is a uniform shift that can't reorder them.
static void gr_rasterize(const GrPath* p, int width, int height,
                         const RSurface* surf, int org_x, int org_y,
                         int blending_flags, int alpha, GrRasterBuffers* buf) {
    int edge_count = p->edge_count;
    if (edge_count == 0) return;
    if (alpha <= 0) return;                       // fully transparent: nothing to do

#if !USE_UNIFIED_BLEND
    // Coverage multiplier as 0-255, applied per pixel below. 255 (the >= 1.0
    // case) is the fast path -- the multiply is skipped entirely, so a
    // full-strength fill produces byte-identical pixels to before.
    // (Under USE_UNIFIED_BLEND alpha is folded into rb_make's tables instead,
    // so it costs nothing per pixel at all.)
    int alpha255 = (alpha >= 65536) ? 255 : (alpha * 255) >> 16;
#endif

    int* coverage      = buf->coverage;
    int* edge_order     = buf->edge_order;
    long long* edge_step = buf->edge_step;
    long long* active_x    = buf->active_x;
    long long* active_step = buf->active_step;
    int* active_end     = buf->active_end;
#if POLY_UNION_FILL
    int* active_dir      = buf->active_dir;
#endif
    int* active_count    = buf->active_count;
    int* next_idx        = buf->next_idx;

    // Sort edge indices by top y. The activation loop below relies on this
    // ordering to be able to `break` on the first not-yet-started edge, and
    // that break is only sound if the order is exact -- startRow is a
    // non-decreasing function of y0, so ascending y0 gives non-decreasing
    // startRow at every subsample offset. Two orderings, same result:
#if POLY_BUCKET_SORT
    // Counting sort into per-row buckets, then insertion sort inside each.
    //
    // Why not counting sort alone: bucketing on y0 >> 16 groups edges by whole
    // row, but two edges in the same row can still have startRow one apart
    // (startRow adds 1 when the fractional part survives the subsample offset).
    // Leaving them unordered would let the loop break on the later one and
    // activate the earlier a row late -- a visibly wrong scanline. The buckets
    // are small (a glyph outline averages well under ten edges per row), so
    // finishing them with insertion sort costs almost nothing and restores
    // exactness.
    //
    // Against the plain insertion sort this is O(E + sum(b_i^2)) rather than
    // O(E^2): for the ~243-edge worst case in this tree that is roughly 2,000
    // operations instead of ~14,700.
    if (buf->row_start) {
        int *row_start = buf->row_start;
        int nrows = height + 1;   // bucket `height` collects everything at or
                                  // below the tile's last row
        for (int r = 0; r <= nrows; r++) row_start[r] = 0;

        // Histogram. Clamped, not assumed in range: contour points may sit
        // outside the tile, and a bucket index derived from one must not
        // scribble past the array.
        for (int i = 0; i < edge_count; i++) {
            int r = p->edges[i].y0 >> 16;
            if (r < 0) r = 0; else if (r > height) r = height;
            row_start[r + 1]++;
        }
        for (int r = 0; r < nrows; r++) row_start[r + 1] += row_start[r];

        // Scatter. Ascending i within a bucket, so the insertion sort that
        // follows sees a stable starting order.
        for (int i = 0; i < edge_count; i++) {
            int r = p->edges[i].y0 >> 16;
            if (r < 0) r = 0; else if (r > height) r = height;
            edge_order[row_start[r]++] = i;
        }

        // row_start[r] is now the END of bucket r, i.e. the start of r+1 --
        // so bucket r spans [r ? row_start[r-1] : 0, row_start[r]).
        int lo = 0;
        for (int r = 0; r < nrows; r++) {
            int hi = row_start[r];
            for (int i = lo + 1; i < hi; i++) {
                int key = edge_order[i];
                int keyY0 = p->edges[key].y0;
                int j = i - 1;
                while (j >= lo && p->edges[edge_order[j]].y0 > keyY0) {
                    edge_order[j + 1] = edge_order[j];
                    j--;
                }
                edge_order[j + 1] = key;
            }
            lo = hi;
        }
    } else
#endif
    {
    // Insertion sort -- O(n^2), tolerable for the few hundred edges these
    // glyph/UI paths produce, not for the 16384 default cap.
    for (int i = 0; i < edge_count; i++) edge_order[i] = i;
    for (int i = 1; i < edge_count; i++) {
        int key = edge_order[i];
        int keyY0 = p->edges[key].y0;
        int j = i - 1;
        while (j >= 0 && p->edges[edge_order[j]].y0 > keyY0) {
            edge_order[j + 1] = edge_order[j];
            j--;
        }
        edge_order[j + 1] = key;
    }
    }

    // Slopes, once per edge rather than once per edge per subsample. The
    // activation loop below derives its dy as ey1 - ey0 with both ends shifted
    // by the same y_off, so the shift cancels and the slope it computes is the
    // same number for every subsample -- POLY_SUBSAMPLES divides producing one
    // answer. Hoisting them here is bit-identical and costs one pass.
    //
    // dy > 0 for every edge gr_line_to builds (it drops horizontals and swaps
    // the endpoints), but GrPath is public enough to be filled by hand, and a
    // divide is not a place to find out.
    if (edge_step) {
        for (int i = 0; i < edge_count; i++) {
            int dy = p->edges[i].y1 - p->edges[i].y0;
            edge_step[i] = dy > 0
                ? POLY_SLOPE_DIV((long long)p->edges[i].x1 - p->edges[i].x0, dy)
                : 0;
        }
    }

    int y_off_arr[POLY_SUBSAMPLES];
    for (int s = 0; s < POLY_SUBSAMPLES; s++) {
        y_off_arr[s] = (s * 65536) / POLY_SUBSAMPLES + 65536 / (2 * POLY_SUBSAMPLES);
        active_count[s] = 0;
        next_idx[s] = 0;
    }

    unsigned char* output = surf->pixels;
    int dst_stride = surf->stride;
    int clip_x0 = surf->clip_x, clip_x1 = surf->clip_x + surf->clip_w;
    int clip_y0 = surf->clip_y, clip_y1 = surf->clip_y + surf->clip_h;
#if USE_UNIFIED_BLEND
    // One resolve for the whole path: mode, alpha and channel mask all fold
    // into the tables here and never appear in the blit loop below.
    RBlend rb = rb_make(blending_flags, -1, alpha);
#else
    int divisor = 255 * POLY_SUBSAMPLES;

    // Compositing mode carried in blending_flags (not a 0-255 coverage
    // threshold it once named is unimplemented -- the branch below is
    // commented out -- so the value is repurposed): 1 = "inverted", every
    // other value = "screen". Screen lifts each pixel toward white by its
    // coverage (gray + a*(255-gray)); inverted lifts it toward its own inverse
    // instead (gray + a*((255-gray)-gray) = gray + a*(255-2*gray)), so a shape
    // reads white on black (255-0 == 255, same as screen) but black on white.
    // Read through rs_blend_inverted, not compared raw: the channel locks
    // share the mode field and BLEND_8BIT_GRAYSCALE rides above it.
    int inverted = rs_blend_inverted(blending_flags);
#endif

    // The writable x span is the same for every row, so clip it once here
    // rather than bounds-testing each pixel inside the blit loop.
    int span_x0 = clip_x0 - org_x; if (span_x0 < 0)     span_x0 = 0;
    int span_x1 = clip_x1 - org_x; if (span_x1 > width) span_x1 = width;

    // Restrict the row sweep to the path's own vertical extent. Rows above the
    // topmost edge or below the bottommost produce no coverage, so running the
    // full per-subsample activate/sort/fill for them is pure waste (glyph tiles
    // are padded and strokes rarely fill the whole height). No edge can activate
    // before first_row (miny minus one row of subsample slack), so starting the
    // loop there keeps the active-edge bookkeeping correct.
    int row_lo = 0, row_hi = height;
#if POLY_SKIP_EMPTY_ROWS
    {
        int miny = 0x7fffffff, maxy = -0x7fffffff;
        for (int i = 0; i < edge_count; i++) {
            if (p->edges[i].y0 < miny) miny = p->edges[i].y0;
            if (p->edges[i].y1 > maxy) maxy = p->edges[i].y1;
        }
        row_lo = (miny >> 16) - 1; if (row_lo < 0)      row_lo = 0;
        row_hi = (maxy >> 16) + 1; if (row_hi > height) row_hi = height;
    }
#endif

    // Rows outside the vertical clip produce nothing visible, so drop them from
    // the sweep rather than sweeping them and discarding the result -- which is
    // what an `oy < clip_y0` test at the bottom of this loop would do, after
    // every subsample pass for that row had already run. For a long line mostly
    // off-screen that was most of the draw.
    //
    // Starting late is safe because the activation loop already knows how to
    // join an edge mid-span: edges past their end hit `endRow <= y` and are
    // skipped, and edges spanning row_lo get their curX fast-forwarded. Both
    // were written for startRow < 0 and neither cares why y is ahead of them.
    {
        int lo = clip_y0 - org_y; if (lo > row_lo) row_lo = lo;
        int hi = clip_y1 - org_y; if (hi < row_hi) row_hi = hi;
    }
    if (row_lo >= row_hi) return;

    // coverage[] is cleared ONCE here and then restored per row over exactly
    // the cells that row disturbed (see the dirty range below), so a row costs
    // its covered span rather than the tile width. Full width, not the clipped
    // span: the fill loops clamp their writes to [0, width), so those are the
    // cells the "zero at row entry" invariant has to cover.
    for (int x = 0; x < width; x++) coverage[x] = 0;

    for (int y = row_lo; y < row_hi; y++) {
        // The x range this row's fills wrote, INCLUSIVE and unclamped -- see
        // POLY_MARK_SPAN, which widens it per emitted span, and POLY_ROW_DIRTY,
        // which clamps it to the tile once, below. Starts empty.
        int dirt_lo = width, dirt_hi = -1;

        for (int s = 0; s < POLY_SUBSAMPLES; s++) {
            int y_off = y_off_arr[s];
            long long* ax    = &active_x[s * POLY_MAX_CROSSINGS];
            long long* astep = &active_step[s * POLY_MAX_CROSSINGS];
            int* aend         = &active_end[s * POLY_MAX_CROSSINGS];
#if POLY_UNION_FILL
            int* adir          = &active_dir[s * POLY_MAX_CROSSINGS];
#endif
            int ac = active_count[s];

            // Drop edges whose span ended before this row (swap-remove --
            // order doesn't matter, everything gets re-sorted below).
            for (int i = 0; i < ac; ) {
                if (aend[i] <= y) {
                    ac--;
                    ax[i] = ax[ac]; astep[i] = astep[ac]; aend[i] = aend[ac];
#if POLY_UNION_FILL
                    adir[i] = adir[ac];
#endif
                } else {
                    i++;
                }
            }

            // Activate edges whose top row is now <= y. edge_order is sorted
            // by top y, so the first edge past the cutoff means all later ones
            // are too -- safe to stop scanning.
            int ni = next_idx[s];
            while (ni < edge_count) {
                int e = edge_order[ni];
                int ey0 = p->edges[e].y0 - y_off;
                int ey1 = p->edges[e].y1 - y_off;
                int y0_frac = ey0 & 0xFFFF;
                int y1_frac = ey1 & 0xFFFF;
                int startRow = (ey0 >> 16) + (y0_frac > 0 ? 1 : 0);
                if (startRow > y) break;
                ni++;

                int endRow = (ey1 >> 16) + (y1_frac > 0 ? 1 : 0);
                if (startRow >= endRow || endRow <= y) continue; // degenerate, or already past

                long long totalDx = (long long)p->edges[e].x1 - p->edges[e].x0;
                int totalDy = ey1 - ey0;
                long long xStep = edge_step ? edge_step[e]
                                            : POLY_SLOPE_DIV(totalDx, totalDy);
                int advance = y0_frac > 0 ? (65536 - y0_frac) : 0;
#if POLY_FAST_ENTRY_X
                // advance * totalDx / totalDy and (advance * xStep) >> 16 are
                // the same rational; only the truncation differs, by at most
                // one 16.16 unit. Trades the second divide for a multiply.
                //
                // The product cannot overflow, and the bound is not obvious:
                // advance is the gap from ey0 up to row startRow, so
                // ey0 + advance == startRow << 16, and the degenerate test just
                // above guarantees startRow < endRow, hence
                // startRow << 16 < ey1, hence advance < totalDy -- strictly.
                // So |advance * xStep| <= |totalDx| * 65536 < 2^48.
                long long curX = (long long)p->edges[e].x0 + (((long long)advance * xStep) >> 16);
#else
                long long curX = (long long)p->edges[e].x0 + ((long long)advance * totalDx / totalDy);
#endif
                // Fast-forward curX past rows before y. Happens when startRow
                // is negative, and also whenever the sweep joins an edge
                // mid-span because row_lo was raised (an off-screen tile top).
                if (y > startRow) curX += (long long)(y - startRow) * xStep;

                if (ac < POLY_MAX_CROSSINGS) {
                    ax[ac] = curX;
                    astep[ac] = xStep;
                    aend[ac] = endRow;
#if POLY_UNION_FILL
                    adir[ac] = p->edges[e].dir;
#endif
                    ac++;
                }
                // else: silently dropped, matching the old
                // scan_count[y] < POLY_MAX_CROSSINGS cap.
            }
            next_idx[s] = ni;

            if (ac >= 2) {
#if POLY_UNION_FILL
                // insertion sort by current x: swap x/dir/step/end together
                for (int i = 1; i < ac; i++) {
                    long long keyX = ax[i], keyStep = astep[i];
                    int keyEnd = aend[i], keyDir = adir[i];
                    int j = i - 1;
                    while (j >= 0 && ax[j] > keyX) {
                        ax[j+1] = ax[j]; astep[j+1] = astep[j];
                        aend[j+1] = aend[j]; adir[j+1] = adir[j];
                        j--;
                    }
                    ax[j+1] = keyX; astep[j+1] = keyStep; aend[j+1] = keyEnd; adir[j+1] = keyDir;
                }

                // Non-Zero Winding Fill
                int winding = 0;
                long long xStart = 0;
                for (int i = 0; i < ac; i++) {
                    if (winding == 0) xStart = ax[i];
                    winding += adir[i];
                    if (winding == 0) {
                        long long xEnd = ax[i];
                        int ix1 = (int)(xStart >> 16);
                        int ix2 = (int)(xEnd   >> 16);
                        int f1  = (int)(xStart >> 8) & 0xFF;
                        int f2  = (int)(xEnd   >> 8) & 0xFF;
                        // ix1 <= ix2: both read out of the x-sorted ax[], so
                        // one call covers all three write sites below.
                        POLY_MARK_SPAN(dirt_lo, dirt_hi, ix1, ix2);
                        if (ix1 == ix2) {
                            if (ix1 >= 0 && ix1 < width)
                                coverage[ix1] += f2 - f1;
                        } else {
                            if (ix1 >= 0 && ix1 < width)
                                coverage[ix1] += 255 - f1;
                            int sx = ix1 + 1; if (sx < 0) sx = 0;
                            int ex = ix2;       if (ex > width) ex = width;
                            for (int x = sx; x < ex; x++)
                                coverage[x] += 255;
                            if (ix2 >= 0 && ix2 < width)
                                coverage[ix2] += f2;
                        }
                    }
                }
#else
                // insertion sort by current x
                for (int i = 1; i < ac; i++) {
                    long long keyX = ax[i], keyStep = astep[i];
                    int keyEnd = aend[i];
                    int j = i - 1;
                    while (j >= 0 && ax[j] > keyX) {
                        ax[j+1] = ax[j]; astep[j+1] = astep[j]; aend[j+1] = aend[j];
                        j--;
                    }
                    ax[j+1] = keyX; astep[j+1] = keyStep; aend[j+1] = keyEnd;
                }

                // even-odd fill with X-AA
                for (int i = 0; i + 1 < ac; i += 2) {
                    long long xStart = ax[i];
                    long long xEnd   = ax[i+1];
                    int ix1 = (int)(xStart >> 16);
                    int ix2 = (int)(xEnd   >> 16);
                    int f1  = (int)(xStart >> 8) & 0xFF;
                    int f2  = (int)(xEnd   >> 8) & 0xFF;

                    // ix1 <= ix2: both read out of the x-sorted ax[], so one
                    // call covers all three write sites below.
                    POLY_MARK_SPAN(dirt_lo, dirt_hi, ix1, ix2);
                    if (ix1 == ix2) {
                        if (ix1 >= 0 && ix1 < width)
                            coverage[ix1] += f2 - f1;
                    } else {
                        if (ix1 >= 0 && ix1 < width)
                            coverage[ix1] += 255 - f1;
                        int sx = ix1 + 1; if (sx < 0) sx = 0;
                        int ex = ix2;       if (ex > width) ex = width;
                        for (int x = sx; x < ex; x++)
                            coverage[x] += 255;
                        if (ix2 >= 0 && ix2 < width)
                            coverage[ix2] += f2;
                    }
                }
#endif
            }

            // Step surviving edges so their x is ready for row y+1.
            for (int i = 0; i < ac; i++) ax[i] += astep[i];
            active_count[s] = ac;
        }

        // Blit the intersection of the writable span with what this row
        // actually covered. row_lo/row_hi already took care of the vertical
        // clip, so oy is in range by construction.
        int oy  = org_y + y;
        int d0, d1;
        POLY_ROW_DIRTY(dirt_lo, dirt_hi, width, d0, d1);
        int bx0 = d0 > span_x0 ? d0 : span_x0;
        int bx1 = d1 < span_x1 ? d1 : span_x1;
#if USE_UNIFIED_BLEND
        {
            // coverage[] is 0..255*POLY_SUBSAMPLES; the shared blend path
            // speaks Q8, so normalize once here. At the default
            // POLY_SUBSAMPLES of 8 this is a shift and replaces the old
            // divide-by-2040 outright -- but it does drop three bits of
            // sub-pixel coverage precision, which is the one place this path
            // is NOT byte-identical to the legacy one below.
            // Rounded, not truncated -- see RB_COV8.
            #define RB_COV8(c) (((c) + POLY_SUBSAMPLES / 2) / POLY_SUBSAMPLES)
            unsigned char *row = output + (size_t)oy * dst_stride + org_x;
  #if RS_PEBBLE_TIME2
            if (!rb.gray8) {
                for (int x = bx0; x < bx1; x++) {
                    POLY_ZERO_COV_SKIP(coverage[x]);
                    row[x] = rb_px(&rb, row[x], RB_COV8(coverage[x]),
                                   RB_DITHER_AT(org_x + x, oy));
                }
            } else
  #endif
            {
                for (int x = bx0; x < bx1; x++) {
                    POLY_ZERO_COV_SKIP(coverage[x]);
                    row[x] = rb_px_gray(&rb, row[x], RB_COV8(coverage[x]));
                }
            }
            #undef RB_COV8
        }
#else
        for (int x = bx0; x < bx1; x++) {
            POLY_ZERO_COV_SKIP(coverage[x]);
            int ox = org_x + x;

            //int a = (coverage[x] * 255) / divisor;
            //if (a > 255) a = 255;
            //else if (a < 0) a = 0;
            //if (a < blending_flags) continue;
            // output[oy * dst_stride + ox] = rs_px_of(a);

            // coverage[x] tops out at 255*POLY_SUBSAMPLES, so the alpha
            // multiply below stays well inside an int.
            int cov = coverage[x];
            if (alpha255 != 255) cov = (cov * alpha255) / 255;

            int gray = rs_gray_of_b((unsigned char)output[oy * dst_stride + ox], blending_flags);
            int reach = inverted ? (255 - 2 * gray) : (255 - gray);
            output[oy * dst_stride + ox] =
                rs_px_of_b(gray + (cov * reach) / divisor, blending_flags);

        }
#endif

        // Restore "coverage is all-zero at row entry" over exactly the cells
        // this row disturbed. AFTER the blit, which reads them, and outside any
        // early exit -- there is none left in this loop, and adding one would
        // have to jump here rather than `continue`.
        for (int x = d0; x < d1; x++) coverage[x] = 0;
    }
}

// gr_rasterize, but carving GrRasterBuffers from a caller-owned scratch
// bump allocator (common/scratch_alloc.h) instead of static arrays or the
// heap -- the common case on embedded targets with no malloc budget to
// spare for this. Silently drops the path if `p` is empty or scratch runs
// out (matches gr_rasterize's own "just don't draw it" failure mode).
//
// Returns 0 ONLY for the scratch shortfall, 1 otherwise -- an empty path is
// a successful "nothing to draw", not a failure. Most callers ignore this;
// it exists for the one that cannot tell an empty result from a dropped one
// by looking at the destination (render_lowspec.c's raster cache, which
// must not memorize a blank tile that was really a scratch shortfall).
//
// No frees: bump-allocated from the caller's scratch, reclaimed when the
// caller's own scratch pointer goes out of scope (see common/scratch_alloc.h).
static int gr_rasterize_scratch(const GrPath* p, int width, int height,
                                const RSurface* surf, int org_x, int org_y,
                                int blending_flags, int alpha,
                                char *scratch, char *scratch_end) {
    if (p->edge_count == 0) return 1;
    // Fully transparent: a successful "nothing to draw", not a scratch
    // shortfall -- the raster cache must be able to tell those apart.
    if (alpha <= 0) return 1;

    GrRasterBuffers buf;
    buf.coverage      = scratch_alloc(int, width);
    buf.edge_order    = scratch_alloc(int, p->edge_count);
    buf.active_x      = scratch_alloc(long long, POLY_SUBSAMPLES * POLY_MAX_CROSSINGS);
    buf.active_step   = scratch_alloc(long long, POLY_SUBSAMPLES * POLY_MAX_CROSSINGS);
    buf.active_end    = scratch_alloc(int, POLY_SUBSAMPLES * POLY_MAX_CROSSINGS);
#if POLY_UNION_FILL
    buf.active_dir    = scratch_alloc(int, POLY_SUBSAMPLES * POLY_MAX_CROSSINGS);
#endif
    buf.active_count  = scratch_alloc(int, POLY_SUBSAMPLES);
    buf.next_idx      = scratch_alloc(int, POLY_SUBSAMPLES);
    // The optional buffers go LAST, and deliberately so. Every buffer above is
    // required -- if one of them comes up short the whole draw is dropped, and
    // on a target whose scratch is sized to the byte that is the
    // difference between a glyph and a blank. row_start and edge_step are the
    // optional ones: gr_rasterize falls back to the insertion sort without the
    // first and to an inline divide without the second. Allocating them after
    // the required set means a scratch too tight for them costs speed and
    // nothing else, instead of turning a speed lever into a correctness cliff.
    // It is also why they are absent from the check below.
    //
    // row_start first, so a scratch that fits exactly one of them keeps getting
    // the buckets it got before edge_step existed.
#if POLY_BUCKET_SORT
    buf.row_start     = scratch_alloc(int, height + 2);
#endif
    buf.edge_step     = scratch_alloc(long long, p->edge_count);

    if (buf.coverage && buf.edge_order && buf.active_x && buf.active_step && buf.active_end
#if POLY_UNION_FILL
        && buf.active_dir
#endif
        && buf.active_count && buf.next_idx
        ) {
        gr_rasterize(p, width, height, surf, org_x, org_y, blending_flags, alpha, &buf);
        return 1;
    }
    return 0;
}

// Fills a closed polygon in one call: n *distinct* vertices as interleaved
// 16.16 fixed-point pairs (pts_xy[2*i], pts_xy[2*i+1]), auto-closed (wraps
// the last vertex back to the first -- do not repeat it). Equivalent to:
//   GrPath path; gr_path_init(&path);
//   gr_move_to(&path, pts_xy[0], pts_xy[1]);
//   for (i = 1..n-1) gr_line_to(&path, pts_xy[2*i], pts_xy[2*i+1]);
//   gr_line_to(&path, pts_xy[0], pts_xy[1]);  // close
//   gr_rasterize_scratch(&path, width, height, surf, org_x, org_y,
//                        blending_flags, alpha, scratch, scratch_end);
// except `path` itself also comes from scratch rather than being a
// caller-owned local -- at a large POLY_MAX_EDGES, a stack GrPath is
// exactly the kind of frame that has hard-faulted on Pebble hardware
// before (font_lowspec_pebble.c documents the ~3KB frame that did it).
static void gr_fill_polygon(const RSurface* surf, const int* pts_xy, int n,
                           int width, int height, int org_x, int org_y,
                           int blending_flags, int alpha,
                           char *scratch, char *scratch_end) {
    if (n < 2) return;
    if (alpha <= 0) return;   // fully transparent -- don't even build the path

    GrPath *path = scratch_alloc(GrPath, 1);
    if (!path) return;
    gr_path_init(path);

    gr_move_to(path, pts_xy[0], pts_xy[1]);
    for (int i = 1; i < n; i++)
        gr_line_to(path, pts_xy[2*i], pts_xy[2*i + 1]);
    gr_line_to(path, pts_xy[0], pts_xy[1]);   // close

    gr_rasterize_scratch(path, width, height, surf, org_x, org_y, blending_flags, alpha,
                         scratch, scratch_end);
}

#endif // RENDER_POLYGON_H
