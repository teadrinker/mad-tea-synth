// public domain, Martin Dvorak (fixscript wasm port / fixbrowser)

#include "math_pure.h"
#include <stdint.h>

// ---------- internal float helpers ----------

static float m_ftrunc(float value)
{
   union { float f; uint32_t i; } u;
   int e;
   u.f = value;
   e = ((u.i >> 23) & 0xFF) - 127;
   if (e < 0)       { u.i &= 0x80000000; }
   else if (e < 23) { u.i &= ~((1 << (23-e))-1); }
   return u.f;
}

static float m_ftrunc_up(float value)
{
   union { float f; uint32_t i; } u;
   uint32_t m;
   int e;
   u.f = value;
   e = ((u.i >> 23) & 0xFF) - 127;
   if (e < 0) {
      if (u.i & 0x7FFFFFFF) {
         return u.i & 0x80000000 ? -1.0f : 1.0f;
      }
      return u.i & 0x80000000 ? -0.0f : 0.0f;
   } else if (e < 23) {
      if (u.i & ((1 << (23-e))-1)) {
         m  = (u.i & ((1 << 23)-1)) | (1 << 23);
         m &= ~((1 << (23-e))-1);
         m += 1 << (23-e);
         if (m & (1 << 24)) { m >>= 1; e++; }
         u.i = (u.i & 0x80000000) | ((e+127) << 23) | (m & ((1 << 23)-1));
      }
   }
   return u.f;
}

// ---------- internal double helpers ----------

static double m_dtrunc(double value)
{
   union { double f; uint64_t i; } u;
   int e;
   u.f = value;
   e = ((u.i >> 52) & 0x7FF) - 1023;
   if (e < 0)       { u.i &= 0x8000000000000000ULL; }
   else if (e < 52) { u.i &= ~((1ULL << (52-e))-1); }
   return u.f;
}

static double m_dtrunc_up(double value)
{
   union { double f; uint64_t i; } u;
   uint64_t m;
   int e;
   u.f = value;
   e = ((u.i >> 52) & 0x7FF) - 1023;
   if (e < 0) {
      if (u.i & 0x7FFFFFFFFFFFFFFFULL) {
         return u.i & 0x8000000000000000ULL ? -1.0 : 1.0;
      }
      return u.i & 0x8000000000000000ULL ? -0.0 : 0.0;
   } else if (e < 52) {
      if (u.i & ((1ULL << (52-e))-1)) {
         m  = (u.i & ((1ULL << 52)-1)) | (1ULL << 52);
         m &= ~((1ULL << (52-e))-1);
         m += 1ULL << (52-e);
         if (m & (1ULL << 53)) { m >>= 1; e++; }
         u.i = (u.i & 0x8000000000000000ULL) | (((uint64_t)(e+1023)) << 52) | (m & ((1ULL << 52)-1));
      }
   }
   return u.f;
}

// ---------- floor / ceil / round ----------

float m_floorf(float value) { return value >= 0.0f ? m_ftrunc(value)    : m_ftrunc_up(value); }
float m_ceilf (float value) { return value >= 0.0f ? m_ftrunc_up(value) : m_ftrunc(value);    }
float m_roundf(float value) { return m_ftrunc(value >= 0.0f ? value + 0.5f : value - 0.5f);   }

double m_floor(double value) { return value >= 0.0 ? m_dtrunc(value)    : m_dtrunc_up(value); }
double m_ceil (double value) { return value >= 0.0 ? m_dtrunc_up(value) : m_dtrunc(value);    }
double m_round(double value) { return m_dtrunc(value >= 0.0 ? value + 0.5 : value - 0.5);     }

// x - floor(x): always in [0, 1).
float  m_fracf(float value)  { return value - m_floorf(value); }
double m_frac (double value) { return value - m_floor(value);  }

// ---------- exp ----------

// https://en.wikipedia.org/wiki/Exponentiation_by_squaring#With_constant_auxiliary_memory
static double m_exp_sqr(double x, int n)
{
   double y = 1.0;
   if (n == 0) return 1.0;
   while (n > 1) {
      if (n & 1) { y *= x; x *= x; } else { x *= x; }
      n >>= 1;
   }
   return x * y;
}

// https://en.wikipedia.org/wiki/Exponential_function#Computation
static double m_exp_taylor(double x)
{
   double x2 = x*x, x3 = x2*x, x4 = x2*x2, x8 = x4*x4;
   return (
      1.0 + x +
      x2  * 0.5 +
      x3  * 0.16666666666666667 +
      x4  * 0.041666666666666667 +
      x4*x  * 0.0083333333333333333 +
      x4*x2 * 0.0013888888888888889 +
      x4*x3 * 0.00019841269841269841 +
      x8    * 0.000024801587301587302 +
      x8*x  * 0.0000027557319223985891 +
      x8*x2 * 0.00000027557319223985891 +
      x8*x3 * 0.000000025052108385441719
   );
}

float  m_expf(float x)      { return (float)m_exp((double)x); }
double m_exp(double value)
{
   union { double f; uint64_t i; } u;
   double n, frac, result, taylor;
   int neg = 0;
   if (value != value) return value;   // nan: (int)n below would be undefined
   if (value < 0.0) { value = -value; neg = 1; }
   n = m_dtrunc(value);
   if (n > 709.0) { u.i = 0x7FFULL << 52; return u.f; } // inf
   frac   = value - n;
   result = m_exp_sqr(2.7182818284590452, (int)n);
   taylor = m_exp_taylor(frac * 0.25);
   taylor *= taylor; taylor *= taylor;
   result *= taylor;
   return neg ? 1.0 / result : result;
}

// ---------- log ----------

// https://en.wikipedia.org/wiki/Binary_logarithm#Iterative_approximation
float  m_log2f(float x)     { return (float)m_log2((double)x); }
double m_log2(double value)
{
   union { double f; uint64_t i; } u;
   double m, tmp, result;
   int e, i, cnt;
   u.f = value;
   if (u.i >> 63)  { u.i = (0x7FFULL << 52) | (1ULL << 51); return u.f; } // nan
   if (u.i == 0)   { u.i = (0x7FFULL << 52) | (1ULL << 63); return u.f; } // -inf
   e = ((u.i >> 52) & 0x7FF) - 1023;
   u.i = (u.i & ((1ULL<<52)-1)) | (1023ULL << 52);
   m = u.f;
   result = e; e = 0;
   for (i = 0; i < 64; i++) {
      if (m == 1.0) break;
      cnt = 0;
      do { m *= m; cnt++; } while (m < 2.0);
      e -= cnt;
      if (e <= -1023) break;
      u.i = (uint64_t)(e + 1023) << 52;
      tmp = result + u.f;
      if (tmp == result) break;
      result = tmp;
      m *= 0.5;
   }
   return result;
}

float  m_log10f(float x)    { return (float)m_log10((double)x); }
double m_log10(double value) { return m_log2(value) * 0.30102999566398120; }

float  m_logf(float x)      { return (float)m_log((double)x); }
double m_log(double value)   { return m_log2(value) * 0.69314718055994531; }

// ---------- pow / sqrt / cbrt ----------

float  m_powf(float x, float y)  { return (float)m_pow((double)x, (double)y); }
double m_pow(double x, double y)
{
   if (x == 0.0) return x;
   if (y == 0.0) return 1.0;
   // An integer exponent by repeated squaring: exact wherever the result is
   // representable (so `x ** 2` agrees with the inlined x * x), and defined for a
   // negative base, where exp(log(x) * y) is nan.
   if (y >= -1024.0 && y <= 1024.0 && y == (double)(int)y) {
      int n = (int)y;
      unsigned int u = (unsigned int)(n < 0 ? -n : n);
      double r = 1.0, b = x;
      while (u) {
         if (u & 1u) r *= b;
         u >>= 1;
         if (u) b *= b;
      }
      return n < 0 ? 1.0 / r : r;
   }
   return m_exp(m_log(x) * y);
}

float  m_sqrtf(float x)  { return (float)m_sqrt((double)x); }
double m_sqrt(double x)   { return m_pow(x, 0.5); }

float  m_cbrtf(float x)  { return (float)m_cbrt((double)x); }
double m_cbrt(double x)   { return m_pow(x, 0.33333333333333333); }

// ---------- sin / cos / tan ----------

// https://en.wikipedia.org/wiki/Sine_and_cosine#Series_definitions
static double m_sin_taylor(double x)
{
   double x2 = x*x, x3 = x2*x, x4 = x2*x2, x5 = x3*x2,
          x7 = x4*x3, x8 = x4*x4, x9 = x5*x4;
   return (
      x -
      x3  * 0.16666666666666667 +
      x5  * 0.0083333333333333333 -
      x7  * 0.00019841269841269841 +
      x9  * 0.0000027557319223985891 -
      x8*x3 * 0.000000025052108385441719 +
      x8*x5 * 0.00000000016059043836821615 -
      x8*x7 * 0.00000000000076471637318198165 +
      x9*x8 * 0.0000000000000028114572543455208 -
      x8*x8*x3 * 0.0000000000000000082206352466243297
   );
}

float  m_sinf(float x)   { return (float)m_sin((double)x); }
double m_sin(double x)
{
   double tmp;
   int neg = 0, quadrant;
   if (x - x != 0.0) return x - x;   // nan or inf: nan, and no undefined (int) below
   if (x < 0.0) { x = -x; neg = 1; }
   tmp = m_dtrunc(x * 0.63661977236758134); // 1/(pi/2)
   x   = x - tmp * 1.5707963267948966;
   tmp *= 0.25;
   quadrant = (int)((tmp - m_dtrunc(tmp)) * 4.0);
   if (quadrant == 1 || quadrant == 3) x = 1.5707963267948966 - x;
   x = m_sin_taylor(x);
   if (quadrant == 2 || quadrant == 3) x = -x;
   if (neg) x = -x;
   return x;
}

float  m_cosf(float x)   { return (float)m_cos((double)x); }
double m_cos(double x)    { return m_sin(x + 1.5707963267948966); }

float  m_tanf(float x)   { return (float)m_tan((double)x); }
double m_tan(double x)    { return m_sin(x) / m_cos(x); }

// ---------- asin / acos ----------

// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Infinite_series
static double m_asin_leibniz(double x)
{
   double x2 = x*x, x3 = x2*x, x4 = x2*x2, x5 = x3*x2,
          x7 = x4*x3, x8 = x4*x4, x9 = x5*x4,
          x11 = x8*x3, x13 = x8*x5, x15 = x8*x7,
          x16 = x8*x8, x17 = x9*x8, x24 = x16*x8;
   return (
      x +
      x3  * 0.16666666666666667 +
      x5  * 0.075 +
      x7  * 0.044642857142857144 +
      x9  * 0.030381944444444444 +
      x11 * 0.022372159090909091 +
      x13 * 0.017352764423076923 +
      x15 * 0.01396484375 +
      x17 * 0.011551800896139706 +
      x16*x3  * 0.0097616095291940789 +
      x16*x5  * 0.0083903358096168155 +
      x16*x7  * 0.0073125258735988451 +
      x16*x9  * 0.0064472103118896484 +
      x16*x11 * 0.0057400376708419235 +
      x16*x13 * 0.0051533096823199042 +
      x16*x15 * 0.0046601434869150962 +
      x24*x9  * 0.0042409070936793631 +
      x24*x11 * 0.0038809645588376692 +
      x24*x13 * 0.0035692053938259345 +
      x24*x15 * 0.0032970595034734847
   );
}

float  m_asinf(float x)  { return (float)m_asin((double)x); }
double m_asin(double x)
{
   union { double f; uint64_t i; } u;
   int neg = 0, invert = 0;
   if (x < 0.0) { x = -x; neg = 1; }
   if (x > 1.0) { u.i = (0x7FFULL << 52) | (1ULL << 51); return u.f; } // nan
   if (x > 0.5) { x = m_sqrt((1.0 - x) * 0.5); invert = 1; }
   x = m_asin_leibniz(x);
   if (invert) x = 1.5707963267948966 - x * 2.0;
   if (neg)    x = -x;
   return x;
}

float  m_acosf(float x)  { return (float)m_acos((double)x); }
double m_acos(double x)   { return 1.5707963267948966 - m_asin(x); }

// ---------- atan / atan2 ----------

// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Infinite_series
static double m_atan_leibniz(double x)
{
   double x2 = x*x, x3 = x2*x, x4 = x2*x2, x5 = x3*x2,
          x7 = x4*x3, x8 = x4*x4, x9 = x5*x4;
   return (
      x -
      x3  * 0.33333333333333333 +
      x5  * 0.2 -
      x7  * 0.14285714285714286 +
      x9  * 0.11111111111111111 -
      x8*x3 * 0.090909090909090909 +
      x8*x5 * 0.076923076923076923 -
      x8*x7 * 0.066666666666666667 +
      x9*x8 * 0.058823529411764706 -
      x8*x8*x3 * 0.052631578947368421 +
      x8*x8*x5 * 0.047619047619047619 -
      x8*x8*x7 * 0.043478260869565217
   );
}

// https://en.wikipedia.org/wiki/Inverse_trigonometric_functions#Arctangent_addition_formula
float  m_atanf(float x)  { return (float)m_atan((double)x); }
double m_atan(double x)
{
   double add;
   int neg = 0, invert = 0, adjust1 = 0, adjust2 = 0;
   if (x < 0.0) { x = -x; neg = 1; }
   if (x > 1.0) { x = 1.0 / x; invert = 1; }
   if (x > 0.5) {
      add = -0.54630248984379051; // tan(-0.5)
      x = (x + add) / (1.0 - x * add);
      adjust1 = 1;
   }
   if (x > 0.25) {
      add = -0.25534192122103627; // tan(-0.25)
      x = (x + add) / (1.0 - x * add);
      adjust2 = 1;
   }
   x = m_atan_leibniz(x);
   if (adjust2) x += 0.25;
   if (adjust1) x += 0.5;
   if (invert)  x  = 1.5707963267948966 - x;
   if (neg)     x  = -x;
   return x;
}

float  m_atan2f(float y, float x)  { return (float)m_atan2((double)y, (double)x); }
double m_atan2(double y, double x)
{
   union { double f; uint64_t i; } u;
   double angle;
   int ys, xs;
   u.f = y; ys = (int)(u.i >> 63);
   u.f = x; xs = (int)(u.i >> 63);
   if (x == 0.0 && y == 0.0) {
      return xs ? (ys ? -3.1415926535897932 : 3.1415926535897932)
                : (ys ? -0.0 : 0.0);
   }
   if (ys) y = -y;
   angle = m_atan(y / x);
   if (xs) angle = 3.1415926535897932 + angle;
   if (ys) angle = -angle;
   return angle;
}

// ---------- fabs / fmin / fmax ----------

float  m_fabsf(float x)
{
   union { float f; uint32_t i; } u;
   u.f = x; u.i &= 0x7FFFFFFF;
   return u.f;
}

double m_fabs(double x)
{
   union { double f; uint64_t i; } u;
   u.f = x; u.i &= 0x7FFFFFFFFFFFFFFFULL;
   return u.f;
}

float  m_fmodf(float x, float y)  { return x - m_floorf(x / y) * y; }
double m_fmod (double x, double y) { return x - m_floor(x / y) * y; }

float  m_fminf(float  x, float  y) { return x < y ? x : y; }
double m_fmin (double x, double y) { return x < y ? x : y; }
float  m_fmaxf(float  x, float  y) { return x > y ? x : y; }
double m_fmax (double x, double y) { return x > y ? x : y; }

// ---------- integer abs / min / max ----------

int m_iabs(int x)            { return x < 0 ? -x : x; }
int m_imin(int x, int y)     { return x < y ? x : y; }
int m_imax(int x, int y)     { return x > y ? x : y; }
int m_iclamp(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

int m_ipow(int base, int e) {
    if (e < 0) return (base == 1) ? 1 : (base == -1 ? ((-e) & 1 ? -1 : 1) : 0);
    int result = 1;
    while (e > 0) {
        if (e & 1) result *= base;
        e >>= 1;
        if (e) base *= base;
    }
    return result;
}

// ---------- clamp / linearstep / smoothstep ----------

float  m_clampf(float x, float lo, float hi)  { return x < lo ? lo : (x > hi ? hi : x); }
double m_clamp (double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

// (x - edge0) / (edge1 - edge0), clamped to [0,1]. A zero-width edge
// (edge0 == edge1) degenerates to a hard step at that edge.
float m_linearstepf(float edge0, float edge1, float x)
{
   float d = edge1 - edge0;
   float t = d != 0.0f ? (x - edge0) / d : (x < edge0 ? 0.0f : 1.0f);
   return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}
double m_linearstep(double edge0, double edge1, double x)
{
   double d = edge1 - edge0;
   double t = d != 0.0 ? (x - edge0) / d : (x < edge0 ? 0.0 : 1.0);
   return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
}

// linearstep followed by the smoothstep ease t*t*(3 - 2t).
float m_smoothstepf(float edge0, float edge1, float x)
{
   float t = m_linearstepf(edge0, edge1, x);
   return t * t * (3.0f - 2.0f * t);
}
double m_smoothstep(double edge0, double edge1, double x)
{
   double t = m_linearstep(edge0, edge1, x);
   return t * t * (3.0 - 2.0 * t);
}

// ---------- linearstepa / smoothstepa / smootherstepa / smootherstep ----------
// "a" variants: instead of clamping to [0,1] for x>=b, continue with the
// linear tail x - 0.5*(b+a) so the return value is in the same domain as x.
// Each curve has value 0 at t=0, value 0.5*(b-a) at t=1, slope 1 at t=1,
// and increasing orders of continuity at the join point.

float m_linearstepaf(float edge0, float edge1, float x)
{
   if (x < edge0) return 0.0f;
   float d = edge1 - edge0;
   if (d == 0.0f) return x <= edge0 ? 0.0f : x - edge0;
   if (x > edge1) return x - 0.5f * (edge1 + edge0);
   float t = (x - edge0) / d;
   return 0.5f * t * t * d;
}
double m_linearstepa(double edge0, double edge1, double x)
{
   if (x < edge0) return 0.0;
   double d = edge1 - edge0;
   if (d == 0.0) return x <= edge0 ? 0.0 : x - edge0;
   if (x > edge1) return x - 0.5 * (edge1 + edge0);
   double t = (x - edge0) / d;
   return 0.5 * t * t * d;
}

float m_smoothstepaf(float edge0, float edge1, float x)
{
   if (x < edge0) return 0.0f;
   float d = edge1 - edge0;
   if (d == 0.0f) return x <= edge0 ? 0.0f : x - edge0;
   if (x > edge1) return x - 0.5f * (edge1 + edge0);
   float t = (x - edge0) / d;
   float t2 = t * t;
   return (t2 * t - 0.5f * t2 * t2) * d;
}
double m_smoothstepa(double edge0, double edge1, double x)
{
   if (x < edge0) return 0.0;
   double d = edge1 - edge0;
   if (d == 0.0) return x <= edge0 ? 0.0 : x - edge0;
   if (x > edge1) return x - 0.5 * (edge1 + edge0);
   double t = (x - edge0) / d;
   double t2 = t * t;
   return (t2 * t - 0.5 * t2 * t2) * d;
}

float m_smootherstepaf(float edge0, float edge1, float x)
{
   if (x < edge0) return 0.0f;
   float d = edge1 - edge0;
   if (d == 0.0f) return x <= edge0 ? 0.0f : x - edge0;
   if (x > edge1) return x - 0.5f * (edge1 + edge0);
   float t = (x - edge0) / d;
   float t2 = t * t;
   return t2 * t2 * ((t - 3.0f) * t + 2.5f) * d;
}
double m_smootherstepa(double edge0, double edge1, double x)
{
   if (x < edge0) return 0.0;
   double d = edge1 - edge0;
   if (d == 0.0) return x <= edge0 ? 0.0 : x - edge0;
   if (x > edge1) return x - 0.5 * (edge1 + edge0);
   double t = (x - edge0) / d;
   double t2 = t * t;
   return t2 * t2 * ((t - 3.0) * t + 2.5) * d;
}

// Perlin's smootherstep: 6th-order polynomial clamped to [0,1]
float m_smootherstepf(float edge0, float edge1, float x)
{
   float t = m_linearstepf(edge0, edge1, x);
   return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
double m_smootherstep(double edge0, double edge1, double x)
{
   double t = m_linearstep(edge0, edge1, x);
   return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// ---------- mix (linear interpolation) ----------
// a + t*(b-a) rather than a*(1-t) + t*b: one multiply, and it returns exactly
// `a` at t==0 and exactly `b` at t==1 in floating point.

float  m_mixf(float  a, float  b, float  t) { return a + t * (b - a); }
double m_mix (double a, double b, double t) { return a + t * (b - a); }

// ---------- sin01 / cos01 (input in turns, not radians) ----------

float  m_sin01f(float x)  { return (float)m_sin((double)x * 6.2831853071795865); }
double m_sin01 (double x) { return m_sin(x * 6.2831853071795865); }
float  m_cos01f(float x)  { return (float)m_cos((double)x * 6.2831853071795865); }
double m_cos01 (double x) { return m_cos(x * 6.2831853071795865); }
