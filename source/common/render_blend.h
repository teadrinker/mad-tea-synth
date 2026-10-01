#ifndef RENDER_BLEND_H
#define RENDER_BLEND_H

// ===================================================================
// The one implementation of compositing.
//
// Five rasterizers in this tree each grew their own answer to "combine this
// coverage with what is already in the destination byte": aaline_blend
// (render_aaline.h), gr_rasterize's blit (render_polygon.h), tri_span
// (render_triangle.h), lowspec_cache_blit and render_rect (render_lowspec.c).
// They disagreed about which modes exist, what the coverage denominator is,
// and whether the destination is one gray channel or three RGB ones. This
// header is the single place that knows, and everything under
// USE_UNIFIED_BLEND goes through it.
//
// THE SHAPE. The mode, the alpha and the channel mask are constant for a whole
// draw, so they are resolved ONCE into an RBlend and never looked at again.
// What is left in the per-pixel loop is a table lookup and a multiply -- no
// branch on the mode, no divide, no *85 decode, no shift/or encode chain. The
// combinatorics live in rb_make; the hot loop sees only their answer. Adding a
// mode costs the loop nothing, which is the whole point of the arrangement.
//
// TWO TIERS, because the call sites split in two:
//   Tier A -- coverage varies per pixel (aaline, gr_rasterize, cache blit).
//     rb_px(): base[]/reach[] indexed by the destination pixel's own 2-bit
//     level, 4 entries each.
//   Tier B -- coverage is constant for the draw (render_rect, tri_span).
//     The blend collapses to a pure dstbyte -> dstbyte function over the 64
//     reachable GColor8 states, so rb_lut_make() precomputes it and the inner
//     loop is one indexed load. This is also where per-channel COLOUR
//     blending is free, which is why render_rect (the only site that ever did
//     it) lives here.
//
// THE ARITHMETIC, one formula both tiers implement:
//     out = d + (col_eff - k*d) * cov * alpha / 255^2
//        screen   (no BLEND_INVERT): col_eff = col, k = 1 -> d + (col - d)*a
//        inverted (BLEND_INVERT):    col_eff = 255, k = 2 -> d + (255 - 2d)*a
// which is exactly what all five sites computed, with col == 255 everywhere
// except render_rect.
//
// NOT byte-identical to the legacy path -- see USE_UNIFIED_BLEND in
// render_surface.h for the list of deliberate differences.
//
// STILL OPEN: whether the extension seam should be per-span rather than
// per-pixel.
// ===================================================================

#include "common/render_surface.h"

// ===================================================================
// RB_HOT -- `static inline` is a request, and at -Os it gets refused.
//
// The two Tier A functions below are the innermost statement of every
// rasterizer in this tree, and the header above promises the per-pixel loop is
// "a table lookup and a multiply". That was true of aaline, which inlined
// them, and false of gr_rasterize, which did not: the shipped Pebble binary
// had `bl rb_px.constprop.0` in the blit loop, with a push/pop pair inside
// rb_px, for ~10-12 cycles of call overhead on a ~15 cycle body. Same
// compiler, same flags, same function -- gr_rasterize_scratch is just big
// enough (2,178 bytes) that -Os's inline budget was spent by the time it got
// there.
//
// So the hint is made binding. Only these two get it, and only because they
// are per-PIXEL: rb_make and rb_lut_make are per-draw and should stay
// out-of-line, where they cost one call and no code duplication.
#ifndef RB_HOT
#if defined(__GNUC__) || defined(__clang__)
#define RB_HOT __attribute__((always_inline)) static inline
#elif defined(_MSC_VER)
#define RB_HOT __forceinline static
#else
#define RB_HOT static inline
#endif
#endif

// ===================================================================
// RB_DITHER -- ordered (Bayer) dithering at the quantization step.
//
// A GColor8 channel is 2 bits. Four levels. Every gradient, every antialiased
// edge and every alpha-blended fill bands into those four steps, and no amount
// of precision earlier in the pipeline helps. An ordered matrix trades spatial
// noise for perceived depth and takes a 4-level channel to ~49 perceived
// levels.
//
// Ordered rather than the alternatives: error diffusion is better looking but
// carries a serial dependency and a row error buffer, which cannot live in a
// branchless per-pixel loop and is the wrong model for compositing anyway (we
// blend onto existing content, we do not convert an image). Random or
// blue-noise dither needs an RNG per pixel and changes every frame, which
// reads as shimmer on animated content. An ordered matrix is a pure function
// of screen position: stateless, two instructions, stable frame to frame.
//
// DEFAULTS OFF -- opt in with -DRB_DITHER=1. Perceived depth is not worth the
// cycles by default: measured on the real thing, the unified path with dither
// on is visibly slower, and the cost is not the two instructions the paragraph
// above advertises.
//
// Where it actually goes:
//   Tier A -- one flash load and an index per pixel (rb_bayer4[y&3][x&3]),
//     which is the honest "two instructions", in the hottest loop there is.
//   Tier B -- the expensive half. RB_LUT_PHASES goes 1 -> 4, so rb_lut_make
//     builds 256 entries instead of 64, each three rb_lut_chan calls with a
//     divide in them: ~768 divides per draw against ~192. That is a PER-DRAW
//     cost paid before a single pixel is written, so a small rect or a short
//     triangle fan can spend more time building its table than filling itself.
//     It also takes RBlendLut from 64 to 256 bytes of the caller's stack.
//
// So the flag is the whole feature's on/off switch, not a tuning knob, and off
// is the right default: the banding is a real cost but a static one, while the
// cycles come out of every frame.
// ===================================================================
#ifndef RB_DITHER
#define RB_DITHER 0
#endif

// Asking for dither off the unified path is a mistake worth stopping the build
// for -- there is no code there to dither with, so it would silently do nothing
// on a whole-build flag the caller went out of their way to set.
#if RB_DITHER && !USE_UNIFIED_BLEND
#error "RB_DITHER requires USE_UNIFIED_BLEND (dithering only exists on the unified path)"
#endif

// An 8-bit gray destination is different: 256 levels have nothing to dither, so
// the request is not wrong, it is just inapplicable. Fold it to 0 rather than
// erroring, because RS_PEBBLE_TIME2 is a PER-TU property (madteasynth sets it
// on render_lowspec.c alone) while -DRB_DITHER=1 is naturally passed to a whole
// target -- erroring would make the flag unusable for the TU that wants it.
// Folding here also puts RB_Q3_ROUND back in rb_make's base[] and drops the
// Bayer tables from a build that could never index them.
#if RB_DITHER && !RS_PEBBLE_TIME2
#undef RB_DITHER
#define RB_DITHER 0
#endif

// ===================================================================
// Quantization, and why it is a multiply rather than a shift.
//
// rs_px_of quantizes with `gray >> 6`, which bins 0..255 into four buckets at
// 64/128/192. But the four representable levels are 0/85/170/255, whose
// nearest-neighbour boundaries are at 42.5/127.5/212.5. So >>6 is not
// round-to-nearest: a gray of 84 -- visually level 1 -- becomes level 1 only
// by luck, and a gray of 50 becomes 0 when 1 is nearer. It systematically
// darkens, by up to 63/255 of full scale on a channel that only has four
// values. (render_rect already knew this and used `(v + 42) / 85`; nothing
// else did.)
//
// RB_Q3 does round(v * 3 / 255) exactly for v in 0..255, as one multiply and
// one shift, so the level index falls out of the high bits:
//     level = (v * RB_Q3_MUL + RB_Q3_ROUND) >> 16
// and because base[]/reach[] are pre-scaled by RB_Q3_MUL in rb_make, the hot
// loop pays nothing at all for it -- the shift it already had just moves from
// >>6 to >>16.
//
// RB_Q3_MUL is 3 * 257: 257/65536 is 1/255 to within the rounding, and the
// factor 3 turns "fraction of full scale" into "level index".
// ===================================================================
#define RB_Q3_MUL   771
#define RB_Q3_ROUND 32768

#if RB_DITHER
// 4x4 Bayer, pre-scaled to the >>16 fixed point above, so adding it in place
// of RB_Q3_ROUND replaces round-to-nearest with a position-dependent threshold
// OF THE SAME MEAN. Values fit an unsigned short; 32 bytes of flash total.
//
// entry = (index * 2 + 1) * 2048, i.e. (index + 0.5)/16 of a level step, NOT
// index/16. The half-step matters: index/16 has mean 7.5/16 rather than 0.5,
// which darkens every dithered pixel by 1/32 of a level -- uniformly, so it
// reads as a gamma shift rather than as noise and is easy to miss. Measured at
// -11.6 levels of 255 on the 2x2 table below without the half-step.
//
// Index in BUFFER space, never tile-local -- see RB_DITHER_AT.
static const unsigned short rb_bayer4[4][4] = {
    {  2048, 34816, 10240, 43008 },
    { 51200, 18432, 59392, 26624 },
    { 14336, 47104,  6144, 38912 },
    { 63488, 30720, 55296, 22528 },
};

// The dither term for a pixel, in the same fixed point base[]/reach[] use.
//
// x and y MUST be absolute destination coordinates. gr_rasterize and
// lowspec_cache_blit both work in tile-local coordinates with an org_x/org_y
// offset; feeding those in tile-local space gives a glyph a different pattern
// every time it moves a pixel, and it crawls. Pass org_x + x, org_y + y.
//
// Consequence worth knowing, and it is a choice rather than a defect: with the
// pattern nailed to the screen, smoothly moving content swims through a static
// texture. That beats per-frame noise, but it is visible. Anchoring the
// pattern to the shape instead trades it for shimmer under sub-pixel motion.
#define RB_DITHER_AT(x, y) ((int)rb_bayer4[(y) & 3][(x) & 3])
#else
#define RB_DITHER_AT(x, y) 0
#endif

// ===================================================================
// The per-draw context.
// ===================================================================
typedef struct {
    // Tier A, GColor8 destinations. Indexed by the destination pixel's own
    // 2-bit level; pre-scaled by RB_Q3_MUL so rb_px's >>16 yields a level
    // index directly. int, not short: base peaks near 255*771 + 32768 and
    // reach*cov near 256*771*255, both well past 16 bits.
    int  base[4];
    int  reach[4];               // (col_eff - k*level) * alpha, RB_Q3-scaled
    // Level index -> destination byte, ALREADY ANDed with `mask`. Folding the
    // channel mask in here rather than in rb_px takes a byte load and an AND
    // out of the per-pixel loop, at the cost of this table meaning "the bits
    // this draw may write for that level" rather than "that level's byte".
    // Nothing outside rb_px reads it, which is what makes that trade free --
    // check that before using it anywhere else.
    unsigned char enc[4];

    // 8-bit gray destinations: no table, closed form. Deliberately keeps the
    // legacy /255 arithmetic (see rb_px_gray).
    short col_eff;               // screen: col;  inverted: 255
    short k;                     // screen: 1;    inverted: 2
    short alpha8;                // 0..255 draw-wide coverage multiplier

    unsigned char col_raw;       // the raw destination-encoded colour byte,
                                 // for Tier B's per-channel targets
    unsigned char mask, nmask;   // channel write mask; nmask == ~mask
    unsigned char gray8;         // destination is 8-bit gray
} RBlend;

// Coverage multiplier as 0..255 from a Q16.16 alpha, clamped.
static inline int rb_alpha8(int alpha_fx) {
    if (alpha_fx <= 0)     return 0;
    if (alpha_fx >= 65536) return 255;
    return (alpha_fx * 255) >> 16;
}

// Resolve a draw. `blend` is the usual packed int (mode bits -- BLEND_INVERT,
// BLEND_LOCK_R/G/B -- plus BLEND_8BIT_GRAYSCALE and any BLEND_CHANNEL mask);
// `col` is the destination-encoded target colour byte,
// which for every coverage-style caller is "white" and can be passed as -1 to
// mean exactly that. `alpha_fx` is Q16.16, literal (0 draws nothing).
//
// Everything expensive happens here, once: the mode switch, the alpha fold,
// the channel mask, the four-entry tables. Nothing downstream branches on any
// of it.
static inline RBlend rb_make(int blend, int col, int alpha_fx) {
    RBlend b;
    int inverted = rs_blend_inverted(blend);
    int a8       = rb_alpha8(alpha_fx);
    int m;
    int i;

    b.gray8   = (unsigned char)(rs_blend_gray8(blend) ? 1 : 0);
    // The channel mask is a statement about a GColor8 byte's three 2-bit RGB
    // fields. An 8-bit gray destination has no such fields -- its byte IS the
    // level -- so applying the mask there would take bits 4-5 of a gray value
    // from the destination and hand back a number that means nothing. Such a
    // draw writes the whole byte, i.e. the locks are ignored rather than
    // half-honoured.
#if RS_PEBBLE_TIME2
    m = b.gray8 ? 0xFF : rs_blend_chanmask(blend);
#else
    m = 0xFF;
#endif
    b.alpha8  = (short)a8;
    b.k       = (short)(inverted ? 2 : 1);
    b.mask    = (unsigned char)m;
    b.nmask   = (unsigned char)(~m & 0xFF);

    // col < 0 means "toward white", which is what every site except
    // render_rect wants and what all of them did before `col` existed.
    if (col < 0) col = 0xFF;
    b.col_raw = (unsigned char)col;
    // The gray the colour byte stands for. Under RS_PEBBLE_TIME2 that is its
    // blue channel, matching rs_gray_of -- Tier A is a single-channel path by
    // design (see the plan's 3.7b); Tier B reads col_raw per channel instead.
    b.col_eff = (short)(inverted ? 255 : rs_gray_of((unsigned char)col));

    for (i = 0; i < 4; i++) {
        int lvl = i * 85;
        // Fold rounding into base when not dithering; when dithering, the
        // Bayer term supplies a threshold of the same mean instead, so the
        // round-to-nearest constant must NOT also be added or the two stack.
        b.base[i]  = lvl * RB_Q3_MUL;
#if !RB_DITHER
        b.base[i] += RB_Q3_ROUND;
#endif
        // *256 then >>8 at use, so full coverage really is full: a bare
        // (reach * cov) >> 8 with cov topping out at 255 would land 255/256 of
        // the way there and leave a permanently-dim maximum.
        b.reach[i] = (((b.col_eff - b.k * lvl) * a8 * 256) / (255 * 255)) * RB_Q3_MUL;
        // Pre-masked -- see the enc[] comment in RBlend. `m` is constant for
        // the draw, so the AND happens four times here instead of once per
        // pixel in rb_px.
        b.enc[i]   = (unsigned char)((0xC0 | (i << 4) | (i << 2) | i) & m);
    }
    return b;
}

// ===================================================================
// Tier A -- one pixel, coverage varying.
// ===================================================================

// GColor8 destination. `cov` is Q8 (0..255). `dth` is RB_DITHER_AT(x, y), or
// the literal 0 when dithering is off -- passing the literal lets it fold
// away, which is why this is a parameter rather than an #if in the body.
RB_HOT unsigned char rb_px(const RBlend *b, unsigned int cur, int cov, int dth) {
    unsigned int c = cur & 3;
    int lvl = (b->base[c] + ((b->reach[c] * cov) >> 8) + dth) >> 16;
    if (lvl < 0) lvl = 0; else if (lvl > 3) lvl = 3;
    // enc[] carries the mask already (see RBlend), so this is one load and
    // one OR rather than two loads, an AND and an OR.
    return (unsigned char)(b->enc[lvl] | (cur & b->nmask));
}

// 8-bit gray destination (BLEND_8BIT_GRAYSCALE, or any !RS_PEBBLE_TIME2
// build). No table: 256 levels cannot be indexed by 2 bits, and a 256-entry
// table costs more to build than a glyph costs to draw.
//
// This arm keeps the legacy /255 arithmetic on purpose. It is the desktop and
// offscreen-image path, where the divides were never the bottleneck, and
// keeping it exact means a non-Pebble build's output is unchanged by
// USE_UNIFIED_BLEND -- which turns every existing desktop test into a
// regression test for this refactor for free. The compiler strength-reduces
// both divides to a multiply-high; neither is a real division.
//
// No dithering here either: 256 levels have nothing to dither.
RB_HOT unsigned char rb_px_gray(const RBlend *b, unsigned int cur, int cov) {
    int d = (int)cur;
    int a = (b->alpha8 == 255) ? cov : (cov * b->alpha8) / 255;
    int v = d + ((b->col_eff - b->k * d) * a) / 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return (unsigned char)(((unsigned)v & b->mask) | (cur & b->nmask));
}

// ===================================================================
// Tier B -- constant coverage, so the whole blend is a table.
// ===================================================================

#if RB_DITHER
// 2x2 rather than Tier A's 4x4: one LUT per phase, and 4 x 64 bytes is
// affordable where 16 x 64 (plus 1024 build iterations) is not. The quality
// drop is real -- ~13 perceived levels against ~49 -- but Tier B draws flat
// fills, whose source is a single constant, which is the case that needs
// dither least. If a large fill ever wants 4x4, gate it on w*h rather than
// making everyone pay the build.
#define RB_LUT_PHASES 4
#define RB_LUT_PHASE(x, y) ((((y) & 1) << 1) | ((x) & 1))
// Bayer indices {0,2,3,1} for phases (x,y) = (0,0),(1,0),(0,1),(1,1), as
// (index * 2 + 1) * 8192 -- the same half-step centering as rb_bayer4, and
// mean 32768 == RB_Q3_ROUND, which is what makes it brightness-neutral.
static const unsigned short rb_bayer2[4] = { 8192, 40960, 57344, 24576 };
#else
#define RB_LUT_PHASES 1
#define RB_LUT_PHASE(x, y) 0
#endif

// The dither term rb_lut_make uses for the phase at (x, y) -- i.e. what a
// caller must pass to rb_lut_chan to get the SAME answer the table holds.
//
// It exists for rasterizers that tabulate part of a draw and evaluate the rest
// inline (render_convex.h fills its constant-coverage core from the table and
// its varying-coverage fringe from rb_lut_chan). Feeding those two different
// dither terms would put a visible pattern seam exactly along the shape's edge,
// which is the one place nobody wants one. Note this is Tier B's 2x2, NOT
// RB_DITHER_AT's 4x4 -- the tiers deliberately dither at different resolutions
// (see RB_LUT_PHASES above), so a Tier A pixel and a Tier B pixel of the same
// draw are never directly comparable anyway.
#if RB_DITHER
#define RB_LUT_DITHER_AT(x, y) ((int)rb_bayer2[RB_LUT_PHASE((x), (y))])
#else
#define RB_LUT_DITHER_AT(x, y) RB_Q3_ROUND
#endif

typedef struct {
    unsigned char t[RB_LUT_PHASES][64];
} RBlendLut;

// Is there a Tier B table at all in this TU? Off the unified path there is no
// rb_lut_make to call, and on an 8-bit gray destination there are 256 states
// rather than 64 -- too many to be worth tabulating per draw (see rb_lut_get).
// Both call sites and RBlendCache below key off this rather than repeating the
// pair, which is also what keeps the cache from costing a byte in a build that
// could never index it.
#if USE_UNIFIED_BLEND && RS_PEBBLE_TIME2
#define RB_LUT_ENABLED 1
#else
#define RB_LUT_ENABLED 0
#endif

// One channel of a Tier B entry: 0..255 destination and target in, 2-bit
// level out. Per-channel by construction, which is what makes render_rect's
// colour blending free here.
static inline int rb_lut_chan(const RBlend *b, int dv, int cv, int a, int dth) {
    int reach = (b->k == 2) ? (255 - 2 * dv) : (cv - dv);
    int v = dv + (reach * a) / 255;
    if (v < 0) v = 0; else if (v > 255) v = 255;
    {
        int lvl = (v * RB_Q3_MUL + dth) >> 16;
        if (lvl < 0) lvl = 0; else if (lvl > 3) lvl = 3;
        return lvl;
    }
}

// Precompute the whole dstbyte -> dstbyte map for a draw whose coverage is
// constant. 64 entries per phase; the caller keeps the RBlendLut on its stack
// (64 or 256 bytes) for the duration of the draw.
//
// `cov` is Q8 and is combined with the RBlend's own alpha, so a caller with
// coverage already folded into alpha passes 255.
static inline void rb_lut_make(RBlendLut *lut, const RBlend *b, int cov) {
    int a  = (b->alpha8 == 255) ? cov : (cov * b->alpha8) / 255;
    int cr = ((b->col_raw >> 4) & 3) * 85;
    int cg = ((b->col_raw >> 2) & 3) * 85;
    int cb = ( b->col_raw       & 3) * 85;
    int p, d;
    for (p = 0; p < RB_LUT_PHASES; p++) {
#if RB_DITHER
        int dth = (int)rb_bayer2[p];
#else
        int dth = RB_Q3_ROUND;
#endif
        for (d = 0; d < 64; d++) {
            int r = rb_lut_chan(b, ((d >> 4) & 3) * 85, cr, a, dth);
            int g = rb_lut_chan(b, ((d >> 2) & 3) * 85, cg, a, dth);
            int u = rb_lut_chan(b, ( d       & 3) * 85, cb, a, dth);
            unsigned char out = (unsigned char)(0xC0 | (r << 4) | (g << 2) | u);
            unsigned char cur = (unsigned char)(0xC0 | d);
            lut->t[p][d] = (unsigned char)((out & b->mask) | (cur & b->nmask));
        }
    }
}

// ===================================================================
// RBlendCache -- memoizing the table, because building it dwarfs small draws.
//
// rb_lut_make is 64 entries x 3 rb_lut_chan, each a multiply, a /255 (constant,
// so strength-reduced, but still ~4 cycles), two clamps and the RB_Q3 quantize:
// ~3.5k cycles on Cortex-M3, ~14k under RB_DITHER's four phases. A tri_span
// pixel is a load, an AND, an indexed load and a store -- call it 5. So the
// table pays for itself at ~700 filled pixels (~2800 dithered).
//
// A full-screen background is 45,600 px and the table is obviously right. A
// line(x1,y1,x2,y2,3.0) is a quad of 60-150 px, and the table costs EIGHT TIMES
// the fill. Codesynth bodies draw hundreds of those a frame.
//
// WHY A CACHE NEEDS NO INVALIDATION. rb_lut_make reads exactly b->k, b->col_raw,
// b->mask, b->nmask and `cov` -- every one of which rb_make derived from
// (blend, col, alpha) alone. It never reads the surface, the clip rect, the
// pixels, or anything else mutable. So the table is a pure function of three
// ints and THE KEY IS COMPLETE. Consequences, all of which kill an objection
// people reach for first:
//   - a push_target into an 8-bit grayscale image ORs BLEND_8BIT_GRAYSCALE into
//     the blend flags, so it changes the KEY. There is nothing to hook.
//   - rebinding the screen, moving the clip, or nesting targets cannot make an
//     entry wrong.
//   - render_ctx_images_reset() does not need to touch this.
//   - an entry may be arbitrarily old and still be exactly right.
// The only failure mode is a miss, which costs precisely what every draw costs
// today. That is what makes this safe to add: the worst case is the status quo.
//
// ONE ENTRY. Enough for "a body draws 200 lines at one alpha and one blend",
// which is the common shape -- for the triangle path `col` is always -1, so the
// key collapses to (blend, coverage). It is NOT enough for a body that fades per
// element (alpha = i/n), which misses every time and gains nothing. Build
// RB_CACHE_STATS=1 to find out which you have before paying for more; the fix
// if you need one is a quantized key (round `cov` to 5-6 bits -- the display
// resolves ~4 levels a channel, ~13 with Tier B dither, so 32 buckets is
// already finer than it can show) rather than more entries, which cost 64 B
// each here and 256 B each under RB_DITHER.
//
// NOT THREAD-SAFE, and deliberately not made so. A torn table would be wrong
// pixels for one draw, not a crash, and every caller in this tree renders from
// one thread. Give a second renderer its own cache rather than locking this.
// ===================================================================
#ifndef RB_CACHE_STATS
#define RB_CACHE_STATS 0
#endif

// Named struct, not just a typedef: font/render_lowspec.h forward-declares it
// and hands out a pointer, so RenderCtx can hold one WITHOUT agreeing on its
// size. That matters -- sizeof(RBlendLut) depends on RB_DITHER, and RB_DITHER
// depends on RS_PEBBLE_TIME2, which is a PER-TU property
// (render_lowspec_lowmem.c sets it, render_ctx.c does not). Embedding this by
// value in a struct two TUs share would give them different layouts. Keep it
// behind the pointer.
typedef struct RBlendCache {
#if RB_LUT_ENABLED
    RBlendLut lut;
    int  key_blend, key_col, key_cov;
    unsigned char valid;
#if RB_CACHE_STATS
    unsigned long hits, misses;
#endif
#else
    // No table exists in this build; keep the type non-empty (a zero-size
    // struct is not C) so sizeof and the pointer arithmetic stay legal.
    unsigned char unused;
#endif
} RBlendCache;

// Empties the cache. The ONLY thing a caller must do before first use -- and
// only because `valid` cannot be trusted to be zero on a stack instance.
// Nothing else ever needs to call this (see the invalidation note above); it
// exists for initialisation, not for invalidation.
static inline void rb_cache_reset(RBlendCache *c) {
#if RB_LUT_ENABLED
    c->valid = 0;
#if RB_CACHE_STATS
    c->hits = 0; c->misses = 0;
#endif
#else
    (void)c;
#endif
}

// The Tier B table for this draw, built on a miss, or 0 when there is nothing
// to tabulate:
//   BLEND_REPLACE     stores the byte; there is no blend to collapse
//   8-bit gray dest   256 reachable states, not 64 -- a 256-entry table costs
//                     more to build than most draws cost to fill
//   !RB_LUT_ENABLED   no Tier B in this build at all
// A 0 return is not a failure. It is the caller's signal to take its
// closed-form arm.
//
// `cov` is the draw's EFFECTIVE 0-255 coverage, with the caller's alpha already
// folded in. Both historical call sites reduce to that: tri_lut_for passed
// alpha 1.0 to rb_make and the coverage as `cov`; render_rect passed alpha to
// rb_make and 255 as `cov`. rb_lut_make computes `a` the same either way
// ((cov * alpha8) / 255 with one of them 255), and `a` is the only thing the
// table depends on, so folding here loses nothing and gives the cache ONE key
// shape instead of two.
//
// `c` must be non-NULL -- a null cache cannot mean "no table", because on a
// GColor8 destination the callers' 0-arm is the 8-bit-gray closed form and
// would blend against the wrong encoding. Callers with nowhere to keep a cache
// put one on their stack; it is 12 bytes more than the bare RBlendLut they
// already had there.
static inline const RBlendLut *rb_lut_get(RBlendCache *c, int blend, int col, int cov) {
#if RB_LUT_ENABLED
    if (rs_blend_mode(blend) & BLEND_REPLACE) return 0;
    if (rs_blend_gray8(blend))                return 0;

    if (c->valid && c->key_blend == blend && c->key_col == col && c->key_cov == cov) {
#if RB_CACHE_STATS
        c->hits++;
#endif
        return &c->lut;
    }
#if RB_CACHE_STATS
    c->misses++;
#endif
    {
        // alpha 1.0 into rb_make, coverage into rb_lut_make -- see the `cov`
        // note above for why that is the canonical half of the two forms.
        RBlend rb = rb_make(blend, col, 65536);
        rb_lut_make(&c->lut, &rb, cov);
    }
    c->key_blend = blend; c->key_col = col; c->key_cov = cov;
    c->valid = 1;
    return &c->lut;
#else
    (void)c; (void)blend; (void)col; (void)cov;
    return 0;
#endif
}

#endif // RENDER_BLEND_H
