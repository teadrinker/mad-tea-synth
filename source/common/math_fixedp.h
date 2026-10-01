#ifndef MATH_FIXEDP_H
#define MATH_FIXEDP_H

// Fixed-point math (Q16.16), portable to both PC builds and Pebble
// firmware.
//
// sin/cos/atan2 ride on Pebble's own ROM lookup tables when built for real
// hardware (-DUSE_PEBBLE_MATH), and fall back to a portable
// reimplementation everywhere else (see math_fixedp.c) -- callers here
// don't need to care which one is active. That selection, and the header
// it pulls in either way, are kept entirely inside math_fixedp.c's own
// translation unit rather than exposed here: math_fixedp_trig.h's
// lookup-table functions have no include guard (by design, meant for
// exactly one translation unit), so if this header pulled it in too, any
// other .c file that included *this* header would define those same
// functions a second time, and collide with math_fixedp.c at link time
// the moment both ended up in the same binary.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// fx16 -- Q16.16 signed fixed point
// ---------------------------------------------------------------------------
typedef int32_t fx16;

#define FX16_SHIFT 16
#define FX16_ONE   ((fx16)1 << FX16_SHIFT)
#define FX16_HALF  (FX16_ONE >> 1)

#define fx16_from_int(i)     ((fx16)((int32_t)(i) << FX16_SHIFT))
#define fx16_to_int(f)       ((int32_t)((f) >> FX16_SHIFT))
#define fx16_round_to_int(f) ((int32_t)(((f) + FX16_HALF) >> FX16_SHIFT))

#define fx16_mul(a, b) ((fx16)(((int64_t)(a) * (int64_t)(b)) >> FX16_SHIFT))
#define fx16_abs(a)    ((a) < 0 ? -(a) : (a))

// fx16_div is a 64-bit division. Cortex-M3 has a 32-bit divide instruction but
// no 64-bit one, so each use expands to ~12 bytes of operand widening (sign-
// extend both sides to 64 bits, shift the numerator up) around a `bl` to
// libgcc's __aeabi_ldivmod. A device build has 60+ such sites, mostly RGC_DIV
// inside the glyph pipeline -- that inline setup costs more in total than the
// libgcc routine it calls into.
//
// FX16_DIV_OUTOFLINE (the exported Pebble project's wscript sets it) routes
// them all through one function in math_fixedp.c: the callee does the widening
// and the call site just passes two int32s. Results are bit-identical -- the
// same expression, compiled once instead of 60-odd times -- at the cost of a
// call/return on top of a division already running into the hundreds of cycles.
//
// Not the default, because it needs math_fixedp.c linked in; targets that use
// this header standalone keep the macro. Also note the macro form is usable in
// a constant expression and the function is not, so keep any such use (there
// are none today) on the macro side.
#ifdef FX16_DIV_OUTOFLINE
fx16 fx16_div(fx16 a, fx16 b);
#else
#define fx16_div(a, b) ((fx16)(((int64_t)(a) << FX16_SHIFT) / (int64_t)(b)))
#endif

#ifndef FX16_NO_FLOAT
#define fx16_from_float(v) ((fx16)((v) * (float)FX16_ONE))
#define fx16_to_float(f)   ((float)(f) / (float)FX16_ONE)
#endif

// ---------------------------------------------------------------------------
// sqrt
// ---------------------------------------------------------------------------

// Exact floor(sqrt(x)) for x >= 0 (returns 0 for x <= 0). Plain integer
// domain (not fixed point) -- used directly by fx16_sqrt() below.
int32_t integer_sqrt(int64_t x);

// sqrt of a Q16.16 value, result in Q16.16.
fx16 fx16_sqrt(fx16 x);

// Length of a (dx,dy) vector, Q16.16. NOT the same as
// fx16_sqrt(fx16_mul(dx,dx) + fx16_mul(dy,dy)): that form truncates each square
// back to int32 and overflows once |dx| or |dy| passes ~181.0, well before the
// length itself leaves fx16 range. This keeps dx^2+dy^2 in int64, so only the
// final length has to fit.
fx16 fx16_hypot(fx16 dx, fx16 dy);

// ---------------------------------------------------------------------------
// sin / cos / atan2
// ---------------------------------------------------------------------------

// Q16.16 radians in, approximately Q16.16 out over +/-1.0. The polynomial
// default peaks at exactly +/-FX16_ONE; the MATH_FIXEDP_USE_PEBBLE_TRIG
// lookup-table path caps at +/-TRIG_MAX_RATIO (65535).
fx16 fx16_sin(fx16 radians);
fx16 fx16_cos(fx16 radians);

// Q16.16 radians, range (-pi, pi] -- the same units and range as the standard
// f32/f64 atan2(), so the VM's polymorphic `atan2` behaves the same whichever
// variant it dispatches to.
fx16 fx16_atan2(fx16 y, fx16 x);

// x in [-1, 1], result Q16.16 radians in [0, pi]. Built as
// atan2(sqrt(1-x^2), x) so it needs no table of its own: the sqrt term is
// always >= 0, which pins atan2's result to exactly acos's range.
fx16 fx16_acos(fx16 x);

// ---------------------------------------------------------------------------
// floor / frac / min / max
// ---------------------------------------------------------------------------
// Bitwise on the raw Q16.16 representation. The VM compiler inlines these same
// formulas at whatever shift a script variable carries (vm/vm.c); these exist
// for callers outside the VM.

fx16 fx16_floor(fx16 x);
fx16 fx16_frac(fx16 x);
fx16 fx16_min(fx16 a, fx16 b);
fx16 fx16_max(fx16 a, fx16 b);

// ---------------------------------------------------------------------------
// clamp / linearstep / smoothstep
// ---------------------------------------------------------------------------
// GLSL-style edge helpers over Q16.16, matching the float m_clamp/
// m_linearstep/m_smoothstep semantics (and the VM's clamp/linearstep/
// smoothstep built-ins). linearstep/smoothstep take (edge0, edge1, x) and
// return the interpolant in [0, FX16_ONE].

fx16 fx16_clamp(fx16 x, fx16 lo, fx16 hi);
fx16 fx16_linearstep(fx16 edge0, fx16 edge1, fx16 x);
fx16 fx16_smoothstep(fx16 edge0, fx16 edge1, fx16 x);

// "a" variants (linearstepa/smoothstepa/smootherstepa): instead of clamping
// to [0, FX16_ONE] for x>=edge1, continue with the linear tail
// x - (edge0+edge1)/2, so the return value is in the same domain as x.
// smootherstep is the clamped 6th-order Perlin variant.
fx16 fx16_linearstepa(fx16 edge0, fx16 edge1, fx16 x);
fx16 fx16_smoothstepa(fx16 edge0, fx16 edge1, fx16 x);
fx16 fx16_smootherstepa(fx16 edge0, fx16 edge1, fx16 x);
fx16 fx16_smootherstep(fx16 edge0, fx16 edge1, fx16 x);

// ---------------------------------------------------------------------------
// sin01 / cos01
// ---------------------------------------------------------------------------
// sin/cos with the input measured in *turns* (0..1 == one full cycle,
// i.e. FX16_ONE per revolution) instead of radians. Output is the same
// range as fx16_sin/fx16_cos (peaks at +/-FX16_ONE by default, +/-65535
// under MATH_FIXEDP_USE_PEBBLE_TRIG).

fx16 fx16_sin01(fx16 turns);
fx16 fx16_cos01(fx16 turns);

// ---------------------------------------------------------------------------
// exp2 / log2 / exp / ln / pow
// ---------------------------------------------------------------------------
// Table-driven (65-entry LUT + linear interpolation between entries, ~1e-4
// relative precision), integer-only -- no libm dependency, same portability
// goal as sqrt/sin/cos/atan2 above. Method follows the standard fixed-point
// technique of range-reducing to a single interpolated octave/decade:
//   exp2:  split x = n + f (n integer, f in [0,1) via arithmetic floor),
//          2^x = 2^n * 2^f -- 2^n is a bit shift, 2^f is a table lookup.
//   log2:  find the input's leading-bit position e (so x/2^e is in [1,2)),
//          log2(x) = e + log2(1 + frac) -- frac's log2 is a table lookup.
// (see https://github.com/deftio/fr_math and
//  https://www.dsprelated.com/showcode/40.php for the general approach).
// Domain errors (x <= 0 for log2/log, or the pow base) return 0, matching
// fx16_sqrt's convention above rather than signalling NaN/-inf.

// 2^x for x in Q16.16 (any sign). Saturates rather than overflowing when
// the result would exceed fx16's representable range.
fx16 fx16_exp2(fx16 x);

// log2(x) for x > 0 in Q16.16, result in Q16.16.
fx16 fx16_log2(fx16 x);

// e^x, via fx16_exp2(x * log2(e)).
fx16 fx16_exp(fx16 x);

// ln(x), x > 0, via fx16_log2(x) * ln(2).
fx16 fx16_log(fx16 x);

// base^exp, via fx16_exp2(exp * log2(base)). base must be > 0 (no general
// support for negative bases / fractional-exponent branch cuts).
fx16 fx16_pow(fx16 base, fx16 exp);

#ifdef __cplusplus
}
#endif

#endif // MATH_FIXEDP_H
