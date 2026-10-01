#ifndef MATH_PURE_H
#define MATH_PURE_H

float  m_floorf(float value);
float  m_ceilf(float value);
float  m_roundf(float value);
double m_floor(double value);
double m_ceil(double value);
double m_round(double value);

float  m_fracf(float value);
double m_frac(double value);

float  m_expf(float x);
double m_exp(double value);

float  m_log2f(float x);
double m_log2(double value);
float  m_log10f(float x);
double m_log10(double value);
float  m_logf(float x);
double m_log(double value);

float  m_powf(float x, float y);
double m_pow(double x, double y);

float  m_sqrtf(float x);
double m_sqrt(double x);
float  m_cbrtf(float x);
double m_cbrt(double x);

float  m_sinf(float x);
double m_sin(double x);
float  m_cosf(float x);
double m_cos(double x);
float  m_tanf(float x);
double m_tan(double x);

float  m_asinf(float x);
double m_asin(double x);
float  m_acosf(float x);
double m_acos(double x);
float  m_atanf(float x);
double m_atan(double x);
float  m_atan2f(float y, float x);
double m_atan2(double y, double x);

float  m_fabsf(float x);
double m_fabs(double x);
float  m_fmodf(float x, float y);
double m_fmod(double x, double y);

float  m_fminf(float x, float y);
double m_fmin(double x, double y);
float  m_fmaxf(float x, float y);
double m_fmax(double x, double y);

int    m_iabs(int x);
int    m_imin(int x, int y);
int    m_imax(int x, int y);
// Integer clamp. Like m_iabs, this is binary-point agnostic -- it only
// compares and selects -- so it serves as both the raw-int and the
// fixed-point variant of `clamp` (see fx16_clamp in math_fixedp.h).
int    m_iclamp(int x, int lo, int hi);

// Integer power: base**e via exponentiation-by-squaring. Negative exponents
// (a fractional true result) collapse to the integer value: 1 for base==1,
// +/-1 for base==-1, else 0. e==0 yields 1.
int    m_ipow(int base, int e);

float  m_fmaxf(float x, float y);

// clamp / linearstep / smoothstep -- GLSL-style edge helpers.
// linearstep/smoothstep take (edge0, edge1, x) and return the interpolant in
// [0,1]; smoothstep applies the classic 3t^2-2t^3 ease. Registered as VM
// built-ins (clamp/linearstep/smoothstep) alongside the fx16 variants.
float  m_clampf(float x, float lo, float hi);
double m_clamp(double x, double lo, double hi);
float  m_linearstepf(float edge0, float edge1, float x);
double m_linearstep(double edge0, double edge1, double x);
float  m_smoothstepf(float edge0, float edge1, float x);
double m_smoothstep(double edge0, double edge1, double x);

// sin01 / cos01 -- sin/cos with the input measured in *turns* (0..1 is one
// full cycle) instead of radians (0..2*pi). Output range is unchanged
// ([-1,1]); only the input period is normalized. Handy for phase-driven
// synth code where phase already runs 0..1.
float  m_sin01f(float x);
double m_sin01(double x);
float  m_cos01f(float x);
double m_cos01(double x);

// linearstepa / smoothstepa / smootherstepa -- "alternative" variants that,
// instead of clamping to [0,1] for x>=b, continue with the linear tail
// x - 0.5*(b+a), so the function is C0-continuous across x=b and the return
// value lives in the same domain as x (not a unit interval).
// linearstepa  uses curve 0.5*t^2                (C1 at x=b: slope 1, value 0)
// smoothstepa  uses curve t^3 - 0.5*t^4          (C2 at x=b: slope 1, curvature 0)
// smootherstepa uses curve t^4*((t-3)*t + 2.5)   (C3 at x=b: slope 1, curvature 0, jerk 0)
// The clamped smootherstep (Perlin) is the standard 6th-order polynomial
// t^3*(t*(6t-15)+10) on [0,1].
float  m_linearstepaf(float edge0, float edge1, float x);
double m_linearstepa(double edge0, double edge1, double x);
float  m_smoothstepaf(float edge0, float edge1, float x);
double m_smoothstepa(double edge0, double edge1, double x);
float  m_smootherstepaf(float edge0, float edge1, float x);
double m_smootherstepa(double edge0, double edge1, double x);
float  m_smootherstepf(float edge0, float edge1, float x);
double m_smootherstep(double edge0, double edge1, double x);

// mix -- linear interpolation, GLSL's mix(a, b, t) = a + t*(b-a): a at t==0,
// b at t==1, extrapolating outside [0,1]. Only the float variants live here;
// the VM's fixed-point mix is expanded by the compiler (see compile_mix_fixed
// in vm/vm.c), and an all-int call promotes to f64 like sqrt(4).
float  m_mixf(float a, float b, float t);
double m_mix(double a, double b, double t);

// belong here?

static inline float clamp(float x, float mi, float ma) { return x < mi ? mi : (x > ma ? ma : x); }
static inline float mix(float a, float b, float t) { return a + t * (b - a); }
static inline float rsmul(float x, float a) { return a*x/(2.f*a*x-a-x+1.f); }


#endif // MATH_PURE_H
