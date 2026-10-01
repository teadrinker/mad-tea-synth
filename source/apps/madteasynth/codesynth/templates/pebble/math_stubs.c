// Stubs satisfying the linker for vm_register_builtins; never called unless a VM
// blob uses them.

float  m_sinf(float x)           { (void)x; return 0.0f; }
float  m_cosf(float x)           { (void)x; return 0.0f; }
float  m_expf(float x)           { (void)x; return 0.0f; }
float  m_floorf(float x)         { (void)x; return 0.0f; }
float  m_ceilf(float x)          { (void)x; return 0.0f; }
float  m_roundf(float x)         { (void)x; return 0.0f; }
float  m_powf(float x, float y)  { (void)x; (void)y; return 0.0f; }
float  m_fabsf(float x)          { (void)x; return 0.0f; }
float  m_fmodf(float x, float y) { (void)x; (void)y; return 0.0f; }
float  m_fminf(float x, float y) { (void)x; (void)y; return 0.0f; }
float  m_fmaxf(float x, float y) { (void)x; (void)y; return 0.0f; }

double m_sin(double x)           { (void)x; return 0.0; }
double m_cos(double x)           { (void)x; return 0.0; }
float  m_sin01f(float x)         { (void)x; return 0.0f; }
double m_sin01(double x)         { (void)x; return 0.0; }
float  m_cos01f(float x)         { (void)x; return 0.0f; }
double m_cos01(double x)         { (void)x; return 0.0; }

float m_linearstepf(float edge0, float edge1, float x)
{
   return 0.0f;
}
double m_linearstep(double edge0, double edge1, double x)
{
   return 0.0;
}
double m_exp(double x)           { (void)x; return 0.0; }
double m_floor(double x)         { (void)x; return 0.0; }
double m_ceil(double x)          { (void)x; return 0.0; }
double m_round(double x)         { (void)x; return 0.0; }
double m_pow(double x, double y) { (void)x; (void)y; return 0.0; }
double m_fabs(double x)          { (void)x; return 0.0; }
double m_fmod(double x, double y){ (void)x; (void)y; return 0.0; }
double m_fmin(double x, double y){ (void)x; (void)y; return 0.0; }
double m_fmax(double x, double y){ (void)x; (void)y; return 0.0; }

int    m_iabs(int x)             { (void)x; return 0; }
int    m_imin(int x, int y)      { (void)x; (void)y; return 0; }
int    m_imax(int x, int y)      { (void)x; (void)y; return 0; }
// Real: an int clamp() calls this.
int    m_iclamp(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// These are in math_pure.h but not in the builtins table; define just in case.
float  m_sqrtf(float x)          { (void)x; return 0.0f; }
float  m_cbrtf(float x)          { (void)x; return 0.0f; }
float  m_logf(float x)           { (void)x; return 0.0f; }
float  m_log2f(float x)          { (void)x; return 0.0f; }
float  m_log10f(float x)         { (void)x; return 0.0f; }
float  m_tanf(float x)           { (void)x; return 0.0f; }
float  m_asinf(float x)          { (void)x; return 0.0f; }
float  m_acosf(float x)          { (void)x; return 0.0f; }
float  m_atanf(float x)          { (void)x; return 0.0f; }
float  m_atan2f(float y, float x){ (void)x; (void)y; return 0.0f; }
double m_sqrt(double x)          { (void)x; return 0.0; }
double m_cbrt(double x)          { (void)x; return 0.0; }
double m_log(double x)           { (void)x; return 0.0; }
double m_log2(double x)          { (void)x; return 0.0; }
double m_log10(double x)         { (void)x; return 0.0; }
double m_tan(double x)           { (void)x; return 0.0; }
double m_asin(double x)          { (void)x; return 0.0; }
double m_acos(double x)          { (void)x; return 0.0; }
double m_atan(double x)          { (void)x; return 0.0; }
double m_atan2(double y, double x){ (void)x; (void)y; return 0.0; }
