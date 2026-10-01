#include "math_fixedp.h"

// sin/cos default to a small polynomial (fx16_cos01_poly below) -- no lookup
// table, peaks at a clean +/-FX16_ONE. Defining MATH_FIXEDP_USE_PEBBLE_TRIG
// swaps them back to the lookup-table trig instead (output capped at
// TRIG_MAX_RATIO == 65535).
//
// atan2 always uses the lookup table (there is no polynomial variant). That
// table -- and the sin/cos lookup that MATH_FIXEDP_USE_PEBBLE_TRIG reaches
// for -- ride on Pebble's own ROM tables when built for real hardware
// (-DUSE_PEBBLE_MATH, which also requires pebble.h on the include path), and
// fall back to the portable reimplementation in math_fixedp_trig.h (ported
// from pebble-firmware's libutil/trig.c) everywhere else -- same angle units
// and output scale in both cases. This selection (and math_fixedp_trig.h's
// un-include-guarded lookup functions) stays private to this translation
// unit -- see math_fixedp.h's comment.
#ifdef USE_PEBBLE_MATH
  #include <pebble.h>
  #define fx_sin_lookup   sin_lookup
  #define fx_cos_lookup   cos_lookup
  #define fx_atan2_lookup atan2_lookup
#else
  #include "math_fixedp_trig.h"
  #define fx_sin_lookup   m_sin_lookup
  #define fx_cos_lookup   m_cos_lookup
  #define fx_atan2_lookup m_atan2_lookup
#endif

// The out-of-line half of fx16_div -- see math_fixedp.h for why.
#ifdef FX16_DIV_OUTOFLINE
fx16 fx16_div(fx16 a, fx16 b) {
#ifdef FX16_DIV_FAST32
  // (a << 16) / b is a 64/32 division, but written as int64/int64 the compiler
  // has no way to say so and calls libgcc's fully general __aeabi_ldivmod --
  // a bit-at-a-time loop over 64-bit operands. Cortex-M4 has a single-cycle-ish
  // 32-bit `udiv`, so the same quotient can be built from four 32-bit divides
  // by long division in hex digits: take 4 bits of the shift at a time,
  // carrying the remainder.
  //
  //     q = a/b, r = a%b;  repeat 4x:  q = q<<4 | (r<<4)/b,  r = (r<<4)%b
  //
  // Exact, not approximate -- it is ordinary long division, and the unsigned
  // magnitude + sign fixup reproduces C's truncate-toward-zero exactly.
  //
  // Two conditions have to hold or it falls back to the int64 form:
  //   * |b| <= 2^28, so r < |b| means r<<4 still fits a uint32.
  //   * the quotient fits 32 bits (|a| < |b|<<16, trivially true once
  //     |b| >= 2^16). Not a correctness dodge for in-range inputs -- it is
  //     what keeps an out-of-range one producing the same bits it did before.
  if (b != 0) {
    uint32_t aa = (uint32_t)(a < 0 ? -(int64_t)a : (int64_t)a);
    uint32_t bb = (uint32_t)(b < 0 ? -(int64_t)b : (int64_t)b);
    if (bb <= 0x0FFFFFFFu && (bb >= 0x10000u || aa < (bb << 16))) {
      uint32_t q = aa / bb;
      uint32_t r = aa % bb;
      int k;
      for (k = 0; k < 4; k++) {
        uint32_t rs = r << 4;
        q = (q << 4) | (rs / bb);
        r = rs % bb;
      }
      return (fx16)(((a < 0) != (b < 0)) ? (uint32_t)(-(int32_t)q) : q);
    }
  }
#endif
  return (fx16)(((int64_t)a << FX16_SHIFT) / (int64_t)b);
}
#endif

// Exact floor(sqrt(x)) for x >= 0, by the restoring ("digit-by-digit") binary
// method: one bit of the root per iteration, and every operation is a shift,
// a compare and a subtract. NO DIVISION AT ALL, which is the entire point.
//
// What this replaces, and why it mattered: the previous implementation was
// Newton's method seeded with `res = x`, i.e. the worst available initial
// guess -- it halves once per iteration all the way down from x to sqrt(x)
// before the quadratic convergence has anything to work with. Measured over
// the segment lengths the glyph stroker actually produces (0.25px .. 40px,
// through fx16_hypot) that is 20-26 iterations, EACH containing an int64
// division. ARMv7-M has no 64-bit divide, so each of those was a call to
// libgcc's __aeabi_ldivmod: roughly 5,000-10,000 cycles for one hypot, and
// glyph_render_core.h calls hypot once per flattened segment (up to
// RGC_FLAT_MAX = 124 of them) plus three per rounded corner. That single
// function dominated the cost of every uncached glyph.
//
// This form runs one iteration per two bits of x -- ~18 for the values above,
// ~31 worst case -- with no calls and no division.
//
// Bit-exact with the Newton version: both return floor(sqrt(x)). Define
// FX16_SQRT_LEGACY_NEWTON to build the old one back for an A/B.
int32_t integer_sqrt(int64_t x) {
  if (x <= 0) return 0;
#ifdef FX16_SQRT_LEGACY_NEWTON
  int64_t res = x;
  int64_t last;
  do {
    last = res;
    res = (res + x / res) / 2;
  } while (res < last);
  return (int32_t)last;
#else
  uint64_t num = (uint64_t)x;
  uint64_t res = 0;
  uint64_t bit;

  // Start at the highest even bit position <= the top set bit, so the loop
  // runs only over x's real width rather than all 32 possible root bits.
#if defined(__GNUC__)
  bit = (uint64_t)1 << ((63 - __builtin_clzll(num)) & ~1);
#else
  bit = (uint64_t)1 << 62;
  while (bit > num) bit >>= 2;
#endif

  while (bit != 0) {
    uint64_t trial = res + bit;
    if (num >= trial) {
      num -= trial;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return (int32_t)res;
#endif
}

fx16 fx16_sqrt(fx16 x) {
  if (x <= 0) return 0;
  // sqrt(x / 2^16) * 2^16 == sqrt(x * 2^16)
  return (fx16)integer_sqrt((int64_t)x << FX16_SHIFT);
}

fx16 fx16_hypot(fx16 dx, fx16 dy) {
  // dx^2 + dy^2 kept as an int64 Q16.16 value (dx*dy scaled back down by
  // FX16_SHIFT, same as fx16_mul, but never narrowed to fx16/int32 --
  // see the header comment for why that narrowing is what overflows).
  int64_t sumsq = (((int64_t)dx * dx) + ((int64_t)dy * dy)) >> FX16_SHIFT;
  return (fx16)integer_sqrt(sumsq << FX16_SHIFT);
}

// 10430 (as a Q16.16 value, i.e. 10430/65536 = 0.159155...) == 1/(2*pi)
// scaled by 65536. fx16_mul(radians, 10430) converts Q16.16 radians
// straight into TRIG_MAX_ANGLE units (a full turn == 0x10000) -- which,
// since a full turn is exactly FX16_ONE, is also a Q16.16 *turns* value.
#define FX16_RAD_TO_TRIGANGLE 10430

#ifndef MATH_FIXEDP_USE_PEBBLE_TRIG
// ---------------------------------------------------------------------------
// Default sin/cos: a small even polynomial over the folded quarter-wave,
// evaluated in turns (one full cycle == FX16_ONE). No lookup table. Direct
// fixed-point port of:
//
//   cos01 = x => {
//     let s = -1
//     x = frac(x + 0.25) - 0.25
//     if (x > 0.25) { s = 1; x -= 0.5 }
//     let x2 = x*x, x4 = x2*x2
//     return s * mix(x2*16, x4*256, -0.2) - s   // mix == lerp(a,b,t)
//   }
//
// mix(a,b,t) = a + (b-a)*t, so with a=16*x2, b=256*x4, t=-0.2 the polynomial
// collapses to  poly = 19.2*x2 - 51.2*x4,  which runs 0..1 across the folded
// quarter-wave (x in [-0.25, 0.25] turns, i.e. x2 in [0, 1/16]); the result
// is then s*(poly - 1) with s = +/-1. Peaks land at exactly +/-FX16_ONE
// (0.25*0.25 = 1/16 hits poly == 1 exactly), so cos(0) == FX16_ONE (65536)
// and sin(0) == 0 exactly -- a raw fx16 unit above the lookup path's 65535,
// but a cleaner Q16.16 1.0.
#define FX16_COS_C1 1258291   // round(19.2 * 65536)
#define FX16_COS_C2 3355443   // round(51.2 * 65536)

static fx16 fx16_cos01_poly(fx16 turns) {
  int32_t s = -1;
  // frac(turns + 0.25) - 0.25 folds any turns value into [-0.25, 0.25].
  fx16 x = fx16_frac(turns + (FX16_ONE >> 2)) - (FX16_ONE >> 2);
  if (x > (FX16_ONE >> 2)) { s = 1; x -= (FX16_ONE >> 1); }
  fx16 x2 = fx16_mul(x, x);
  fx16 x4 = fx16_mul(x2, x2);
  fx16 poly = fx16_mul(x2, FX16_COS_C1) - fx16_mul(x4, FX16_COS_C2);
  return (fx16)(s * (poly - FX16_ONE));
}

// sin(t) == cos(t - quarter turn); cos01_poly folds internally so the
// negative offset needs no separate wrapping.
fx16 fx16_sin(fx16 radians) {
  return fx16_cos01_poly(fx16_mul(radians, FX16_RAD_TO_TRIGANGLE) - (FX16_ONE >> 2));
}

fx16 fx16_cos(fx16 radians) {
  return fx16_cos01_poly(fx16_mul(radians, FX16_RAD_TO_TRIGANGLE));
}
#else
// MATH_FIXEDP_USE_PEBBLE_TRIG: the original lookup-table trig, riding on
// whichever table fx_sin_lookup/fx_cos_lookup select above (Pebble ROM under
// USE_PEBBLE_MATH, else the ported m_sin_lookup/m_cos_lookup). Output tops
// out at TRIG_MAX_RATIO (65535).
fx16 fx16_sin(fx16 radians) {
  return (fx16)fx_sin_lookup(fx16_mul(radians, FX16_RAD_TO_TRIGANGLE));
}

fx16 fx16_cos(fx16 radians) {
  return (fx16)fx_cos_lookup(fx16_mul(radians, FX16_RAD_TO_TRIGANGLE));
}
#endif

// A minimax-polynomial atan2 (evaluating atan(z) directly via Horner on
// z^2 after octant-folding to z=min(|x|,|y|)/max(|x|,|y|) in [0,1]) was
// tried here instead of the lookup below. Its coefficients aren't accurate
// right at z=1 (the fold boundary, i.e. x==y / 45 degrees): evaluating the
// polynomial at z=65536 (1.0 in Q16.16) gives 50106, not the true pi/4
// (51472) -- a systematic ~1.2 degree error concentrated exactly at the
// fold line, which is what showed up as a visible discontinuity there.
// Fixing that needs a properly re-derived (Remez/Chebyshev) polynomial
// constrained to be accurate at the domain edge, not a quick coefficient
// tweak, so reverted to the lookup-based version below instead.
// 411775 == round(2*pi * 65536): converts a signed TRIG_MAX_ANGLE-turn
// count into fx16 radians (see fx16_atan2 below). Inverse of
// FX16_RAD_TO_TRIGANGLE (10430 ~= 65536 / (2*pi)) above.
#define FX16_TRIGANGLE_TO_RAD 411775

// Small-angle fast path for fx16_atan2: the ROM-derived lookup table
// (math_fixedp_trig.h, ATAN_LUT_STRIDE=255, "DO NOT CHANGE") resolves the
// tan(0..45deg) ratio into only ~256 buckets, so any angle under about a
// quarter of a degree falls into bucket 0 and comes back as *exactly* 0 --
// not just imprecise, entirely lost. That's fatal for
// glyph_render_core.h's degraded round-join code, which calls
// atan2(cross, dot) once per flattened-curve vertex: cross/dot is exactly
// this "small ratio, x near +FX16_ONE" shape for nearly-but-not-exactly
// collinear segments, and a curve flattened into many tiny segments hits
// it repeatedly, collapsing each join's arc to duplicate points and
// producing a visibly jagged/stairstepped stroke edge. atan(r) ~= r -
// r^3/3 is accurate to a fraction of a percent well past where this
// kicks in (ratio < 1/8, i.e. under ~7.1 degrees), so use it there
// instead of the coarse table. Only handles the x>0 (near-0-degree) case,
// since that's what a "turning" angle between two roughly-forward-
// pointing segment directions always is; the symmetric x<0 (near-180-
// degree) case doesn't arise from that call site.
static fx16 fx16_atan2_small_angle(fx16 y, fx16 x) {
  fx16 r  = fx16_div(y, x);
  fx16 r3 = fx16_mul(fx16_mul(r, r), r);
  return r - r3 / 3;
}

fx16 fx16_atan2(fx16 y, fx16 x) {
  int64_t ay = (y < 0) ? -(int64_t)y : (int64_t)y;
  if (x > 0 && ay * 8 < (int64_t)x) return fx16_atan2_small_angle(y, x);

  // atan2 only depends on the ratio y/x, so scale both down by a shared
  // shift until they fit the lookup's native int16 domain -- this keeps
  // more precision than truncating straight from Q16.16 to int.
  int64_t ax = (x < 0) ? -(int64_t)x : (int64_t)x;
  int64_t m  = (ay > ax) ? ay : ax;
  int shift = 0;
  while ((m >> shift) > 0x7fff) shift++;
  int32_t turn = fx_atan2_lookup((int16_t)(y >> shift), (int16_t)(x >> shift));

  // fx_atan2_lookup follows Pebble's own convention: an *unsigned* fraction
  // of a turn in [0, TRIG_MAX_ANGLE) (a full turn == 2*pi). Fold the upper
  // half back to negative, then convert to radians -- so this returns the
  // same signed (-pi, pi] range as the f32/f64 atan2 variants, matching
  // standard atan2() semantics instead of Pebble's angle convention.
  if (turn > TRIG_MAX_ANGLE / 2) turn -= TRIG_MAX_ANGLE;
  return (fx16)(((int64_t)turn * FX16_TRIGANGLE_TO_RAD) >> FX16_SHIFT);
}

fx16 fx16_acos(fx16 x) {
  fx16 y = fx16_sqrt(FX16_ONE - fx16_mul(x, x));
  return fx16_atan2(y, x);
}

// ---------------------------------------------------------------------------
// floor / frac / min / max
// ---------------------------------------------------------------------------
// mask = (1<<FX16_SHIFT)-1 clears exactly the fractional bits; ANDing with
// its complement rounds toward -infinity in two's complement (an arithmetic
// right-shift-then-left-shift by FX16_SHIFT would do the same thing less
// directly), and the fractional bits alone (mask, not ~mask) are exactly
// x - floor(x) in the same Q16.16 representation, always non-negative.
fx16 fx16_floor(fx16 x) { return x & ~(fx16)(FX16_ONE - 1); }
fx16 fx16_frac(fx16 x)  { return x &  (fx16)(FX16_ONE - 1); }

// Fixed-point comparison is just raw integer comparison -- Q16.16 ordering
// matches the underlying int32 ordering, so no scaling is needed.
fx16 fx16_min(fx16 a, fx16 b) { return a < b ? a : b; }
fx16 fx16_max(fx16 a, fx16 b) { return a > b ? a : b; }

// ---------------------------------------------------------------------------
// clamp / linearstep / smoothstep
// ---------------------------------------------------------------------------

fx16 fx16_clamp(fx16 x, fx16 lo, fx16 hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// (x - edge0) / (edge1 - edge0), clamped to [0, FX16_ONE]. A zero-width edge
// (edge0 == edge1) would divide by zero, so it degenerates to a hard step at
// that edge instead -- matching m_linearstep's guard.
fx16 fx16_linearstep(fx16 edge0, fx16 edge1, fx16 x) {
  fx16 d = edge1 - edge0;
  fx16 t;
  if (d == 0) return x < edge0 ? 0 : FX16_ONE;
  t = fx16_div(x - edge0, d);
  if (t < 0)        return 0;
  if (t > FX16_ONE) return FX16_ONE;
  return t;
}

// linearstep followed by the smoothstep ease t*t*(3 - 2t). t is in
// [0, FX16_ONE], so 3 - 2t stays in [FX16_ONE, 3*FX16_ONE] and every
// intermediate stays comfortably within fx16 range.
fx16 fx16_smoothstep(fx16 edge0, fx16 edge1, fx16 x) {
  fx16 t    = fx16_linearstep(edge0, edge1, x);
  fx16 t2   = fx16_mul(t, t);
  fx16 poly = fx16_from_int(3) - fx16_mul(fx16_from_int(2), t);
  return fx16_mul(t2, poly);
}

// ---------------------------------------------------------------------------
// linearstepa / smoothstepa / smootherstepa / smootherstep
// ---------------------------------------------------------------------------
// "a" variants: for x>=edge1 return x - (edge0+edge1)/2 instead of clamping
// to FX16_ONE, so the return value lives in the same domain as x. Each curve
// has value 0 at t=0, value d/2 at t=1, slope 1 at t=1 (in real units),
// and increasing orders of continuity.

fx16 fx16_linearstepa(fx16 edge0, fx16 edge1, fx16 x) {
  if (x < edge0) return 0;
  fx16 d = edge1 - edge0;
  if (d == 0) return x <= edge0 ? 0 : x - edge0;
  if (x > edge1) return x - ((edge0 + edge1) >> 1);
  fx16 t = fx16_div(x - edge0, d);
  // 0.5 * t^2 * d = (t^2 * d) >> 17  (since fx16_mul already shifts by 16)
  fx16 t2 = fx16_mul(t, t);
  return fx16_mul(t2, d) >> 1;
}

fx16 fx16_smoothstepa(fx16 edge0, fx16 edge1, fx16 x) {
  if (x < edge0) return 0;
  fx16 d = edge1 - edge0;
  if (d == 0) return x <= edge0 ? 0 : x - edge0;
  if (x > edge1) return x - ((edge0 + edge1) >> 1);
  fx16 t = fx16_div(x - edge0, d);
  // curve: (t^3 - 0.5*t^4) * d
  fx16 t2 = fx16_mul(t, t);
  fx16 t3 = fx16_mul(t2, t);
  fx16 t4 = fx16_mul(t2, t2);
  // t3 - t4/2 = t3 - (t4>>1)
  return fx16_mul(t3 - (t4 >> 1), d);
}

fx16 fx16_smootherstepa(fx16 edge0, fx16 edge1, fx16 x) {
  if (x < edge0) return 0;
  fx16 d = edge1 - edge0;
  if (d == 0) return x <= edge0 ? 0 : x - edge0;
  if (x > edge1) return x - ((edge0 + edge1) >> 1);
  fx16 t = fx16_div(x - edge0, d);
  // curve: t^4 * ((t-3)*t + 2.5) * d
  // (t-3)*t + 2.5 = t^2 - 3*t + 2.5
  fx16 t2 = fx16_mul(t, t);
  fx16 t4 = fx16_mul(t2, t2);
  // t^2 - 3*t + 5/2: 5/2 in Q16.16 = (5<<16)/2 = 5<<15
  fx16 poly = t2 - fx16_mul(fx16_from_int(3), t) + (fx16_from_int(5) >> 1);
  return fx16_mul(fx16_mul(t4, poly), d);
}

// Perlin's smootherstep: 6th-order polynomial clamped to [0, FX16_ONE].
// t^3*(t*(6t-15)+10) = t^3*(6t^2 - 15t + 10)
fx16 fx16_smootherstep(fx16 edge0, fx16 edge1, fx16 x) {
  fx16 t  = fx16_linearstep(edge0, edge1, x);
  fx16 t2 = fx16_mul(t, t);
  fx16 t3 = fx16_mul(t2, t);
  // 6*t^2 - 15*t + 10
  fx16 poly = fx16_mul(fx16_from_int(6), t2) - fx16_mul(fx16_from_int(15), t) + fx16_from_int(10);
  return fx16_mul(t3, poly);
}

// ---------------------------------------------------------------------------
// sin01 / cos01
// ---------------------------------------------------------------------------
// A Q16.16 turns value (one full cycle == FX16_ONE) is already the native
// argument for both paths: the polynomial default measures its input in turns
// directly, and the lookup path's angle unit is TRIG_MAX_ANGLE (0x10000) per
// full turn == FX16_ONE. Either way out-of-range and negative turns fold
// automatically (fx16_cos01_poly via its frac step, the lookups internally).

#ifndef MATH_FIXEDP_USE_PEBBLE_TRIG
fx16 fx16_sin01(fx16 turns) { return fx16_cos01_poly(turns - (FX16_ONE >> 2)); }
fx16 fx16_cos01(fx16 turns) { return fx16_cos01_poly(turns); }
#else
fx16 fx16_sin01(fx16 turns) { return (fx16)fx_sin_lookup(turns); }
fx16 fx16_cos01(fx16 turns) { return (fx16)fx_cos_lookup(turns); }
#endif

// ---------------------------------------------------------------------------
// exp2 / log2 / exp / ln / pow
// ---------------------------------------------------------------------------

// gFX16_POW2_FRAC[i] = round(2^(i/64) * 65536) for i in [0,64] -- 2^f for f
// in [0,1] sampled every 1/64, for interpolating fx16_exp2's fractional
// part. gFX16_LOG2_MANT[i] = round(log2(1 + i/64) * 65536), used the same
// way for fx16_log2's mantissa. Same construction as fr_math's
// gFR_POW2_FRAC_TAB / gFR_LOG2_MANT_TAB (see header comment for links).
static const int32_t gFX16_POW2_FRAC[65] = {
    65536, 66250, 66971, 67700, 68438, 69183, 69936, 70698,
    71468, 72246, 73032, 73828, 74632, 75444, 76266, 77096,
    77936, 78785, 79642, 80510, 81386, 82273, 83169, 84074,
    84990, 85915, 86851, 87796, 88752, 89719, 90696, 91684,
    92682, 93691, 94711, 95743, 96785, 97839, 98905, 99982,
    101070, 102171, 103283, 104408, 105545, 106694, 107856, 109031,
    110218, 111418, 112631, 113858, 115098, 116351, 117618, 118899,
    120194, 121502, 122825, 124163, 125515, 126882, 128263, 129660,
    131072,
};

static const int32_t gFX16_LOG2_MANT[65] = {
    0, 1466, 2909, 4331, 5732, 7112, 8473, 9814,
    11136, 12440, 13727, 14996, 16248, 17484, 18704, 19909,
    21098, 22272, 23433, 24579, 25711, 26830, 27936, 29029,
    30109, 31178, 32234, 33279, 34312, 35334, 36346, 37346,
    38336, 39316, 40286, 41246, 42196, 43137, 44068, 44990,
    45904, 46809, 47705, 48593, 49472, 50344, 51207, 52063,
    52911, 53751, 54584, 55410, 56229, 57040, 57845, 58643,
    59434, 60219, 60997, 61769, 62534, 63294, 64047, 64794,
    65536,
};

// round(1/ln(2) * 65536) and round(ln(2) * 65536) -- the fixed constants
// tying exp2/log2 to exp/ln.
#define FX16_LOG2E 94548
#define FX16_LN2   45426

fx16 fx16_exp2(fx16 x) {
  // n = floor(x / 65536) via arithmetic shift (rounds toward -infinity for
  // negative x too). frac = x - n*65536, guaranteed in [0, 65536) -- taken
  // as x's low FX16_SHIFT bits directly (same trick as fx16_frac) rather
  // than "x - (n << FX16_SHIFT)", since n can be negative and left-shifting
  // a negative value is undefined behaviour in C.
  int32_t n    = x >> FX16_SHIFT;
  int32_t frac = x & (FX16_ONE - 1);
  int     idx  = (int)(frac >> 10);   // 65536/64 = 1024 = 1<<10 per table step
  int32_t r    = frac & 1023;
  int32_t lo = gFX16_POW2_FRAC[idx], hi = gFX16_POW2_FRAC[idx + 1];
  // val = 2^frac * 65536, in [65536, 131072).
  int32_t val = lo + (int32_t)((((int64_t)(hi - lo)) * r) >> 10);
  // val << n / val >> n applies the integer octave (2^n). fx16's usable
  // magnitude tops out well under 2^15, so n outside roughly [-16, 14]
  // over/underflows -- saturate instead of shifting by an out-of-range
  // amount (undefined behaviour in C) or silently wrapping.
  if (n >= 15)  return 0x7FFFFFFF;
  if (n <= -17) return 0;
  return (fx16)(n >= 0 ? (val << n) : (val >> (-n)));
}

fx16 fx16_log2(fx16 x) {
  if (x <= 0) return 0;  // domain error -> 0, matching fx16_sqrt's convention
  uint32_t ux = (uint32_t)x;
  // Find the position of the leading set bit (0-based) via binary search --
  // e.g. msb=16 for x==FX16_ONE (2^16), msb=0 for x==1.
  int msb = 0;
  if (ux >= (1u << 16)) { ux >>= 16; msb += 16; }
  if (ux >= (1u << 8))  { ux >>= 8;  msb += 8;  }
  if (ux >= (1u << 4))  { ux >>= 4;  msb += 4;  }
  if (ux >= (1u << 2))  { ux >>= 2;  msb += 2;  }
  if (ux >= (1u << 1))  {            msb += 1;  }
  // Normalize the original x so its leading bit sits at bit FX16_SHIFT,
  // giving a Q16.16 mantissa m in [FX16_ONE, 2*FX16_ONE) -- i.e. [1,2).
  int32_t m = (msb >= FX16_SHIFT) ? (int32_t)((uint32_t)x >> (msb - FX16_SHIFT))
                                  : (int32_t)((uint32_t)x << (FX16_SHIFT - msb));
  int32_t frac = m - FX16_ONE;              // in [0, FX16_ONE)
  int     idx  = (int)(frac >> 10);
  int32_t r    = frac & 1023;
  int32_t lo = gFX16_LOG2_MANT[idx], hi = gFX16_LOG2_MANT[idx + 1];
  int32_t interp = lo + (int32_t)((((int64_t)(hi - lo)) * r) >> 10);
  // log2(x) = (msb - FX16_SHIFT) + log2(1+frac) -- the (msb - FX16_SHIFT)
  // term accounts for x itself being a Q16.16 value (raw integer x equals
  // 2^FX16_SHIFT times the real value), not a plain integer.
  return (fx16)(((msb - FX16_SHIFT) << FX16_SHIFT) + interp);
}

fx16 fx16_exp(fx16 x) { return fx16_exp2(fx16_mul(x, FX16_LOG2E)); }
fx16 fx16_log(fx16 x) { return fx16_mul(fx16_log2(x), FX16_LN2); }

fx16 fx16_pow(fx16 base, fx16 exp) {
  if (base <= 0) return 0;  // domain error -> 0, matching fx16_sqrt/fx16_log2
  return fx16_exp2(fx16_mul(exp, fx16_log2(base)));
}
