#ifndef RENDER_SURFACE_H
#define RENDER_SURFACE_H

// ===================================================================
// RSurface -- the destination descriptor shared by every rasterizer in
// common/ (render_aaline.h's Wu lines, render_polygon.h's scanline fill).
//
// It answers exactly two questions and nothing else:
//   where do the pixels live   -- pixels + stride
//   which of them may I write  -- clip_x/y/w/h
//
// Deliberately NOT here:
//   placement.  Where a tile-local path lands in the buffer is a property
//     of the draw, not of the destination -- gr_rasterize takes org_x/org_y
//     as arguments. (aaline needs no origin: its coords are already in
//     buffer space.) A surface is hoistable out of a draw loop; an origin
//     is not, which is the whole reason they are separate.
//   extent.  A `w`/`h` pair is redundant with the clip rect -- the old
//     AALineSurface carried both and never once read w/h.
//   blend policy.  Fill replaces, aaline blends over; unifying that is a
//     separate job (see the harmonization notes).
//
// Coords are plain buffer-space pixels; the clip rect need not sit at the
// origin, and may extend past the tile being drawn.
// ===================================================================

typedef struct {
    unsigned char *pixels;
    int stride;
    int clip_x, clip_y, clip_w, clip_h;
} RSurface;

// ===================================================================
// Pixel encoding -- what one byte of an RSurface actually means.
//
// Default: 8-bit gray. With RS_PEBBLE_TIME2, a native Pebble Time 2 GColor8
// (AARRGGBB, 2 bits/channel) so rasterizers can write straight into the
// captured framebuffer instead of blitting an offscreen tile.
//
// Every rasterizer in common/ goes through these, which is the point: with a
// separate answer per rasterizer, a TU using both fill and aaline would put two
// different encodings in one buffer. Define it once per TU, before including
// render_aaline.h / render_polygon.h.
// ===================================================================

#ifndef RS_PEBBLE_TIME2
#define RS_PEBBLE_TIME2 0
#endif

// Buffer byte -> 0-255 gray. Needed to blend against what is already there.
static inline int rs_gray_of(unsigned char px) {
#if RS_PEBBLE_TIME2
    return (int)(px & 3) * 85;   // 2-bit channel (0..3) back up to 0..255
#else
    return (int)px;
#endif
}

// 0-255 gray -> buffer byte. Not clamped: callers keep coverage in range,
// and the fill's hot loop has historically relied on skipping the branch.
static inline unsigned char rs_px_of(int gray) {
#if RS_PEBBLE_TIME2
    unsigned int c2 = (unsigned int)gray >> 6;   // 0..3
    return (unsigned char)((0x3u << 6) | (c2 << 4) | (c2 << 2) | c2);
#else
    return (unsigned char)gray;
#endif
}

// ===================================================================
// The blend argument -- what the `blend` / `blend_option` / `blending_flags`
// int every rasterizer here takes actually holds.
//
// It is two fields, not one number:
//
//   bits 0-15   the compositing MODE, itself a set of BITS (BLEND_*, below):
//                 1  BLEND_INVERT  lift each pixel toward its own inverse
//                                  instead of toward white / toward `col`
//                 2  BLEND_LOCK_R  leave the destination's red alone
//                 4  BLEND_LOCK_G  leave its green alone
//                 8  BLEND_LOCK_B  leave its blue alone
//                 16 BLEND_REPLACE store, don't blend (triangle backend only)
//               0 is plain screen, which is what it always was. Read them with
//               rs_blend_inverted() / rs_blend_chanmask(), never by comparing
//               the raw argument -- `blend == 1` stops being true the moment
//               any other bit rides along.
//   bit 16      BLEND_8BIT_GRAYSCALE, below.
//
// The lock bits live down here, in the low nibble, for one reason: this is the
// only part of `blend` a codesynth script can reach. Every draw builtin takes
// its arguments as Q16.16 and render_ctx shifts `blend` back down, so a value
// only survives the round trip if it fits a small integer -- BLEND_CHANNEL's
// bits 17-22 (which say the same thing, positively) do not. So a script asks
// for "red only" as blend = BLEND_LOCK_G | BLEND_LOCK_B == 4 | 8.
//
// The mode is read SIGNED, but every rasterizer here only ever sees a
// non-negative one. The sign is spare capacity that one public API layer above
// borrows: font/render_lowspec.h's render_line/render_ellipse take a
// negative `blend` to mean "use the triangle-fill backend", and strip it on the
// way in (their blend_take_backend) the same way they strip stroke_width's and
// radius_w's signs. Nothing below that boundary knows or cares.
//
// Everything above bit 15 is a flag; everything at or below it is the mode.
// A caller with no flags to set passes the bare mode exactly as before, so
// every pre-existing call site keeps its meaning.
// ===================================================================

// The destination bytes are full 0-255 grayscale, one level per value, rather
// than whatever this TU was compiled for (GColor8 under RS_PEBBLE_TIME2, where
// only 4 levels survive a round trip). Set per DRAW, not per build: one target
// can be a GColor8 framebuffer and the next an 8-bit offscreen image in the
// same TU -- which is exactly what vscreen's image_alloc/push_target do (see
// apps/madteasynth/vscreen.h). 65536 == 1 << 16, i.e. the first bit above the
// mode field.
#define BLEND_8BIT_GRAYSCALE 65536
#define BLEND_MODE_MASK      0xFFFF

// The mode-field bits. 0 -- no bit set -- is screen over every channel, which
// is what a caller that never heard of any of this passes and always did.
#define BLEND_INVERT   1
#define BLEND_LOCK_R   2
#define BLEND_LOCK_G   4
#define BLEND_LOCK_B   8
#define BLEND_LOCK_BITS (BLEND_LOCK_R | BLEND_LOCK_G | BLEND_LOCK_B)
// Store the source instead of blending it. Only render_triangle.h implements
// this; it lives here because it is a mode-field bit like the rest, and having
// it here is what keeps it from colliding with the locks.
#define BLEND_REPLACE  16

// Channel write mask, bits 17-22: which of the destination byte's three 2-bit
// RGB channels this draw may modify, as RRGGBB (bit 5..4 red, 3..2 green,
// 1..0 blue -- the same bit positions they occupy in a GColor8 pixel).
//
// This is the POSITIVE form of the same control the BLEND_LOCK_* bits express
// negatively, and the two intersect: the locks subtract from whatever this
// field selects, so BLEND_CHANNEL(0x3F) | BLEND_LOCK_R and a bare
// BLEND_LOCK_R mean the same thing. It exists because "which channels" is
// naturally a mask, and the locks exist because only the low nibble survives
// the trip through a codesynth script (see the note above).
//
// 0 here means "every channel", so a caller that never heard of masking passes
// the bare mode exactly as before and keeps its meaning -- the same convention
// BLEND_8BIT_GRAYSCALE established one bit lower.
//
// Only the unified blend path (USE_UNIFIED_BLEND, common/render_blend.h)
// honours any of this. On the legacy path it is silently ignored, which
// degrades to today's behaviour rather than to something wrong.
#define BLEND_CHANNEL_SHIFT  17
#define BLEND_CHANNEL_BITS   0x7E0000
#define BLEND_CHANNEL(m)     (((m) & 0x3F) << BLEND_CHANNEL_SHIFT)

// The mode field as a magnitude, which is the only form its bits can be read
// out of: a bare negative mode (-1 and friends -- the backend-selecting sign
// render_line/render_ellipse consume) carries its flags in the magnitude, so
// -1 is BLEND_INVERT with the triangle backend asked for, not "every bit set".
// blend_take_backend folds the sign away for everything downstream of it; this
// covers the primitives that never had a backend choice to make.
static inline int rs_blend_flags(int blend) {
    int mode = (int)(short)(blend & BLEND_MODE_MASK);
    return mode < 0 ? -mode : mode;
}

// The one way to ask "is this draw inverted".
static inline int rs_blend_inverted(int blend) {
    return (rs_blend_flags(blend) & BLEND_INVERT) != 0;
}

// The destination byte-mask this draw may write, as a GColor8 AARRGGBB byte.
// 0xFF -- the whole byte, the historical behaviour -- unless a channel was
// masked out, either by BLEND_CHANNEL selecting a subset or by a BLEND_LOCK_*
// bit removing one. The two alpha bits are always writable: they are not a
// colour channel, and leaving them to come from the destination would let a
// masked draw land transparent on a buffer that was never initialized opaque.
static inline int rs_blend_chanmask(int blend) {
    int m    = (blend & BLEND_CHANNEL_BITS) >> BLEND_CHANNEL_SHIFT;
    int lock = rs_blend_flags(blend) & BLEND_LOCK_BITS;
    if (!m) m = 0x3F;
    if (lock & BLEND_LOCK_R) m &= ~0x30;
    if (lock & BLEND_LOCK_G) m &= ~0x0C;
    if (lock & BLEND_LOCK_B) m &= ~0x03;
    return m | 0xC0;
}

// ===================================================================
// USE_UNIFIED_BLEND -- selects the centralized compositing path in
// common/render_blend.h over the five hand-written blend loops that predate
// it (aaline_blend, gr_rasterize's blit, tri_span, lowspec_cache_blit,
// render_rect). Both are compiled from the same sources; this picks which one
// each site uses, so the two can be A/B'd on hardware by a build switch.
//
// Default 0 -- nothing changes for an existing consumer until it opts in.
//
// This is SCAFFOLDING, not a configuration axis. The intended end state is
// default 1, then deletion of the flag and every #else arm: two
// implementations of one thing is the disease this was meant to cure, and
// carrying both indefinitely makes the tree worse than leaving it alone.
// Note render_aaline.h's USE_LB_CLIPPER 160 lines below, which is `#define
// USE_LB_CLIPPER 1` directly above `#ifndef USE_LB_CLIPPER`, i.e. an A/B flag
// that rotted into 62 lines of permanently unreachable code.
//
// NOT byte-identical to the legacy path. Known differences, all deliberate:
//   * aaline_blend honours the compositing mode; the legacy arm ignores it.
//   * quantization to a 2-bit channel rounds to the nearest representable
//     level instead of binning by >>6, which darkens every value by up to
//     63/255.
//   * gr_rasterize's coverage is normalized 0..2040 -> 0..255 before blending.
// ===================================================================
#ifndef USE_UNIFIED_BLEND
#define USE_UNIFIED_BLEND 1
#endif

// The signed compositing mode carried in `blend`. Raw: use rs_blend_flags()
// (or rs_blend_inverted() / rs_blend_chanmask()) to read a bit out of it --
// this is for the two things that need the number itself, the backend-
// selecting sign and render_triangle.h's BLEND_REPLACE test.
static inline int rs_blend_mode(int blend) {
    return (int)(short)(blend & BLEND_MODE_MASK);
}

// True when this draw's destination is 8-bit grayscale.
static inline int rs_blend_gray8(int blend) {
    return (blend & BLEND_8BIT_GRAYSCALE) != 0;
}

// Folds a bare negative mode (-1 and friends, whose sign extension sets EVERY
// high bit including the flags) down into the mode field, so the bits above it
// mean what they say. A value that already carries flags is non-negative and
// passes through untouched. The one public layer that accepts a negative mode
// calls this on the way in, before splitting off the sign (see
// render_lowspec.c's blend_take_backend); after that the two fields are
// independent and can be read directly.
static inline int rs_blend_norm(int blend) {
    return blend < 0 ? (blend & BLEND_MODE_MASK) : blend;
}

// rs_gray_of / rs_px_of for one draw, honouring BLEND_8BIT_GRAYSCALE. Under
// the flag a byte IS its gray level, so both directions are the identity --
// which is the whole point: no 2-bit quantization anywhere in the loop.
static inline int rs_gray_of_b(unsigned char px, int blend) {
    return rs_blend_gray8(blend) ? (int)px : rs_gray_of(px);
}

// Clamped, unlike rs_px_of: the GColor8 path's >>6 hides a slightly out-of-
// range coverage in the quantization, but the identity path would wrap it
// (256 -> 0, a black speck in the middle of a white fill). One branch on a
// loop that already does a multiply and a divide.
static inline unsigned char rs_px_of_b(int gray, int blend) {
    if (!rs_blend_gray8(blend)) return rs_px_of(gray);
    if (gray < 0)   gray = 0;
    if (gray > 255) gray = 255;
    return (unsigned char)gray;
}

// A packed w*h buffer that is entirely writable -- the common "render into
// a tile I just allocated" case.
static inline RSurface rs_tile(unsigned char *pixels, int w, int h) {
    RSurface s;
    s.pixels = pixels;
    s.stride = w;
    s.clip_x = 0;
    s.clip_y = 0;
    s.clip_w = w;
    s.clip_h = h;
    return s;
}

// Last, deliberately: render_blend.h is written against everything above and
// includes this header back (harmlessly -- its own guard is already set by the
// time it does). Every rasterizer here already includes render_surface.h, so
// pulling it in from this one point means no site needs a new include line.
#include "common/render_blend.h"

#endif // RENDER_SURFACE_H
