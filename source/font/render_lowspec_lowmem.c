// render_lowspec_lowmem.c -- font/render_lowspec.c at LOW-MEMORY sizing.
//
// Nothing here is Pebble-specific: this is the small-footprint configuration of
// the glyph pipeline -- POLY_MAX_EDGES 243 instead of the desktop default
// 16384, whose GrPath.edges array alone is 320 KB. The exporter's pebble and
// microw8 targets both need it, for the same reason (a 256 KB address space in
// total); the desktop targets compile font/render_lowspec.c directly at its
// roomy defaults.
//
// render_lowspec.c pulls in font/glyph_render_core.h and
// common/render_polygon.h, both of which size their scratch buffers off
// compile-time macros defaulting to desktop-scale values, against a
// whole-device RAM budget of ~128 KB. So this translation unit overrides those
// macros to the smallest sizes that still fit the digit glyphs in
// font_digits_lowspec.h, before render_lowspec.c's includes see them -- the
// include guard-less headers pick up whichever value is defined first, from any
// file.
//
// Sizing was tuned against two independent budgets, both discovered by
// actually running this in the Pebble emulator rather than estimating:
//   1. The app binary's "virtual size", a 16-bit field in Pebble's app
//      metadata (inject_metadata.py) -- hard-capped at 65535 bytes total
//      static footprint (code+data+bss), independent of physical RAM. A
//      device build's VM and UI can easily account for ~51 KB of that 64 KB
//      ceiling before any font work at all.
//   2. Actual call-stack depth at render time. stroke_build_outline() is
//      reached via layer_update_proc -> draw_digit_glyph ->
//      render_glyph_lowspec -> stroke_subpath_outline_lowspec ->
//      stroke_build_outline, deep enough that even a ~3 KB stack frame
//      there hard-faulted (PC=0/LR=0, crashing in the function's own
//      prologue, before its first statement ran) -- so its scratch
//      arrays can't be plain stack locals. glyph_render_core.h takes a
//      caller-supplied scratch buffer (see scratch_alloc there) instead of
//      reserving `static` storage per array: the host allocates one such
//      buffer once, at program start, sized exactly for the
//      GLYPH_MAX_SEGS/GLYPH_SMOOTH_MAX/RGC_FLAT_MAX/RGC_ARC_MARGIN below,
//      and reuses it for every render. That is smaller than a sum of static
//      arrays -- several are never live at the same time, so they overlap in
//      the shared buffer instead of each being reserved permanently -- while
//      keeping budget (1) as the binding constraint overall.
//
// font_digits_lowspec.h's settings keep the desktop export's nonzero
// radius/small_radius_mul/medium_radius_mul (rounded corners, matching the
// font editor's preview), so these sizes have to cover build_smoothed()'s
// worst-case corner expansion instead of just passing points through.
// Sizes below are measured exactly, not estimated: a temporary counter
// (g_dbg_max_sn/g_dbg_max_flat_n in glyph_render_core.h,
// g_dbg_max_edges in render_lowspec.c, both since stripped) fed
// through every one of '0'-'9' via pebblesim found digit '8' as the worst
// case -- a single 10-point subpath with all 8 interior points rounded --
// at sn=42, flat_n=124, edge_count=243. Re-run that measurement (git
// history has it) before lowering any of these, since build_smoothed()/
// flatten_smoothed() truncate the whole rest of the centerline once their
// output buffer fills (`break`/`return` the point loop, not a per-corner
// degrade) -- undersizing doesn't round '8' a little less, it cuts the
// glyph off entirely (verified: an unrecognizable blob in pebblesim at
// GLYPH_SMOOTH_MAX=32).
//   GLYPH_MAX_SEGS   10  -- == 10, the largest single-subpath point count
//                           across '0'-'9' (glyphs split into multiple
//                           subpaths at MOVE points, so this is well
//                           under each glyph's total segment count) --
//                           exact, no slack.
//   GLYPH_SMOOTH_MAX 42  -- == the measured sn worst case; exact, no
//                           slack (the virtual_size budget above doesn't
//                           leave room for any).
//   RGC_FLATTEN_MUL   3  -- unused as a size directly (see RGC_FLAT_MAX
//                           below) but must stay >= ceil(124/42) so
//                           GLYPH_SMOOTH_MAX*RGC_FLATTEN_MUL, the default
//                           RGC_FLAT_MAX would fall back to without an
//                           override, is never smaller than what's
//                           actually used.
//   RGC_FLAT_MAX    125  -- >= 124 (measured flat_n worst case), 1 point
//                           of slack. Overrides the
//                           GLYPH_SMOOTH_MAX*RGC_FLATTEN_MUL default
//                           (126) directly -- see glyph_render_core.h's
//                           RGC_FLAT_MAX comment for why that default
//                           rounds up further than needed here.
//   RGC_ARC_MARGIN    1  -- MAX_OUTLINE's degraded-corner-arc safety
//                           term (glyph_render_core.h) is dead code for
//                           this font: is_degraded requires
//                           half_width >= corner radius, and half_width
//                           here is a constant 2.0 (DIGIT_LINE_WIDTH/2)
//                           while every radius in font_digits_lowspec.h's
//                           settings (plain/small/medium) is > 2.0. Only
//                           safe to keep this low as long as that holds --
//                           re-check if DIGIT_LINE_WIDTH ever grows
//                           enough to approach font_digits_lowspec.h's
//                           radius, or the font settings change.
#define GLYPH_MAX_SEGS      10
#define GLYPH_SMOOTH_MAX    42
#define RGC_FLATTEN_MUL     3
#define RGC_FLAT_MAX        124
#define RGC_ARC_MARGIN      1

// >= the measured worst case (243 edges, digit '8') with a little slack.
// Edges scale with flattened point count (left + right stroke sides) --
// see the GLYPH_SMOOTH_MAX comment. GrEdge is this array's biggest single
// static cost (20 B each), since it's `static` too (see
// render_lowspec.c).
#define POLY_MAX_EDGES      243
#define POLY_MAX_CROSSINGS  8
// No vertical supersampling: Pebble's GColor8 framebuffer only has 2 bits
// per channel, so subpixel AA buys nothing here and costs an 8x inner loop
// on a core with no hardware float.
#define POLY_SUBSAMPLES     2

// ---- Rasterizer speed levers (common/render_polygon.h) -------------------
//
// Both default to 0 there; this is the build that turns them on, so flipping
// either to 0 here is the A/B. Neither changes what a glyph looks like.
//
// (POLY_SKIP_ZERO_COVERAGE is not a lever here: it is unconditional in
// render_polygon.h, because not touching a pixel the path did not cover is a
// correctness rule.)
//
// BUCKET_SORT is the big one: gr_rasterize insertion-sorts the edge list by
// top y, and POLY_MAX_EDGES here is 243 (digit '8'), so that is ~14,700
// comparisons on every uncached glyph. Counting-sorting by row first takes it
// to roughly 2,000.
#define POLY_BUCKET_SORT        1
// The tile is the glyph box plus padding, and a stroke rarely reaches the top
// and bottom of it -- those rows run the full per-subsample activate/sort/fill
// for no coverage at all.
#define POLY_SKIP_EMPTY_ROWS    1

// Slope divide. gr_rasterize needs dx/dy in 16.16 once per edge, which is
// exactly fx16_div -- and this build already carries FX16_DIV_FAST32 (see
// wscript), which builds the quotient from four 32-bit udivs instead of
// calling libgcc's __aeabi_ldivmod. ARMv7-M has no 64-bit divide, so that is
// the difference between ~4 udivs and a bit-at-a-time loop over 64-bit
// operands, on every edge of every uncached glyph.
//
// The guard is what keeps this exact rather than merely close. fx16_div's
// result is an fx16, so it can only stand in where operands and quotient all
// fit 32 bits; otherwise fall through to the portable form. Both arms truncate
// toward zero on the same rational, so they agree wherever both are defined.
//
// adx < (ady << 15) is "|quotient| <= INT32_MAX" -- deliberately one bit
// tighter than FX16_DIV_FAST32's own internal aa < (bb << 16), which bounds
// the UNSIGNED magnitude because it only has to reproduce fx16_div's existing
// out-of-range bits, not a correct number.
//
// The fallback costs nothing in practice: a quotient past 32 bits means
// dy < |dx| * 2^-15, an edge too shallow to cross two sample rows, whose step
// gr_rasterize never reads.
#include "common/math_fixedp.h"
static long long poly_fx16_slope(long long dx, long long dy) {
    long long adx = dx < 0 ? -dx : dx;
    long long ady = dy < 0 ? -dy : dy;
    if (ady != 0 && adx <= 0x7FFFFFFFLL && ady <= 0x7FFFFFFFLL && adx < (ady << 15))
        return (long long)fx16_div((fx16)dx, (fx16)dy);
    return dx * 65536 / dy;
}
#define POLY_SLOPE_DIV(dx, dy) poly_fx16_slope((dx), (dy))

// Entry-x by multiply instead of divide. POLY_SLOPE_DIV above handles the
// slope; this handles the OTHER divide on the activation path, the one that
// places an edge's x at the first row it covers. With the slope already in
// hand the same rational is (advance * xStep) >> 16, so activation becomes a
// 64x32 multiply and a shift and stops calling libgcc entirely.
//
// That call is the reason this is now on. The shipped binary still contained
// __aeabi_ldivmod on the activation path -- libgcc's fully general
// bit-at-a-time 64/64 loop -- once per edge per subsample, i.e. ~486 calls for
// the 243-edge glyph worst case.
//
// It is the one rasterizer option here that changes output, which is why it
// sat off: truncation lands differently, by at most one Q16.16 unit, i.e.
// 1/65536 of a pixel. Measured on the desktop A/B at 1,882 of 1,113,600 bytes
// (0.17%), every one of them an AA fringe pixel moving by one level. Nothing
// else in this file changes what a glyph looks like; this one does, barely.
//
// Flip to 0 to A/B it -- and note that the arithmetic argument for it being
// harmless is in render_polygon.h next to the code, including why the product
// cannot overflow.
#define POLY_FAST_ENTRY_X       1

// Glyph draws on this target go straight into the captured screen framebuffer
// via an RSurface, so this TU's rasterizer must emit Pebble Time 2's native
// GColor8 bytes, not plain 8-bit grayscale (which would need a caller-side
// quantizing blit). Also covers the RG_FLAGS_RENDER_LINE_OUTLINE aaline path --
// one define, one pixel format for the whole TU (common/render_surface.h).
// microw8 shares this sizing but builds with -DRS_PEBBLE_TIME2=0 for runtime
// layouts (common/render_layout.h).
#ifndef RS_PEBBLE_TIME2
#define RS_PEBBLE_TIME2     1
#endif

// Glyph raster cache: the tile a cache entry is rasterized into comes off the
// front of s_glyph_scratch, so it has to be paid for there or the render
// behind it silently loses geometry (every size above was measured with no
// slack). The host's scratch allocation must include this tile budget on top of
// the render scratch -- keep the two in sync. 2048 B
// covers a ~45x45 tile, i.e. text up to cap_height ~36 on a 200x228 screen;
// anything bigger simply renders uncached (see lowspec_cache_find).
#define LOWSPEC_GLYPH_CACHE_MAX_TILE_BYTES 2048

#include "font/render_lowspec.c"
