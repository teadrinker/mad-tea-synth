// vm_lib_accel.c - C versions of the geometry built-ins in vm_lib.h, run by the
// interpreter in place of a library specialisation's body. Included from vm.c.
//
// Each has to give exactly what its source body gives, so the arithmetic follows
// the DSL text operation for operation, in the same precision and order, rather
// than the fastest formula. tests/test_vm_lib.c runs every one against its body
// (`#enable source_builtins` switches these off). Emitters never see them.
//
// Attached only to a specialisation whose parameters and result are all one
// float kind (lib_accel_kind); anything else -- fixed point, ints, mixed widths
// -- runs its body. Returning 0 declines at run time too, and the body runs.

#if VM_LIB_ACCEL_ON

// No fused multiply-add: the interpreter rounds every operation, so a C compiler
// targeting an FMA-capable CPU (zig cc does by default) would otherwise turn
// `a*b - c*d` into one rounding and disagree in the last bit.
// Included last in vm.c, and scoped where the compiler allows it, so nothing
// else in the translation unit is compiled under it.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("fp-contract=off")
#endif

#define ACC_MAX VM_VEC_MAX_LANES

// acc_dot_K is sum(a * b): the products materialise first, then accumulate from
// a typed 0, exactly as VM_LIB_SRC_SUM does.

// A parameter's data and element count: a fixed array lives in the frame, a
// slice points at the caller's storage.
static const unsigned char *acc_param(Func *sp, unsigned char *fr, int i, int *n) {
    VMSym *s = &sp->syms[sp->param_slot[i]];
    unsigned char *p = fr + s->offset;
    if (is_slice(s->type.kind)) {
        *n = read_i32(p + sizeof(void*));
        return (const unsigned char*)read_ptr(p);
    }
    *n = s->type.len;
    return p;
}

#define ACC_KIND(T, K, RD, WR, SQRT)                                                        \
static int acc_load_##K(Func *sp, unsigned char *fr, int i, T *out) {                       \
    int n;                                                                                  \
    const unsigned char *p = acc_param(sp, fr, i, &n);                                      \
    if (n < 0 || n > ACC_MAX) return -1;                                                    \
    for (int k = 0; k < n; k++) out[k] = RD(p + k * (int)sizeof(T));                        \
    return n;                                                                               \
}                                                                                           \
static void acc_ret_##K(Func *sp, unsigned char *fr, const T *v, int n) {                  \
    for (int k = 0; k < n; k++) WR(fr + sp->ret_offset + k * (int)sizeof(T), v[k]);        \
}                                                                                           \
static T acc_dot_##K(const T *a, const T *b, int n) {                                       \
    T t = 0;                                                                                \
    for (int k = 0; k < n; k++) { T m = a[k] * b[k]; t = t + m; }                           \
    return t;                                                                               \
}                                                                                           \
static int acc_geo_##K(int op, Func *sp, unsigned char *fr) {                               \
    T a[ACC_MAX], b[ACC_MAX], c[ACC_MAX], r[ACC_MAX];                                       \
    int n = acc_load_##K(sp, fr, 0, a);                                                     \
    if (n < 0) return 0;                                                                    \
    int nb = sp->n_params > 1 ? acc_load_##K(sp, fr, 1, b) : n;                             \
    if (nb != n) return 0;                                                                  \
    switch (op) {                                                                           \
    case ACC_DOT:    WR(fr + sp->ret_offset, acc_dot_##K(a, b, n)); return 1;               \
    case ACC_LENGTH: WR(fr + sp->ret_offset, SQRT(acc_dot_##K(a, a, n))); return 1;         \
    case ACC_DISTANCE:                                                                      \
        for (int k = 0; k < n; k++) c[k] = a[k] - b[k];                                     \
        WR(fr + sp->ret_offset, SQRT(acc_dot_##K(c, c, n)));                                \
        return 1;                                                                           \
    case ACC_NORMALIZE: {                                                                   \
        T l = SQRT(acc_dot_##K(a, a, n));                                                   \
        int guard = (sp->flags & VM_FLAG_CHECK_DIV_ZERO) != 0;                              \
        for (int k = 0; k < n; k++) r[k] = (guard && l == 0) ? (T)0 : a[k] / l;             \
        break;                                                                              \
    }                                                                                       \
    case ACC_REFLECT: {                                                                     \
        T d = acc_dot_##K(b, a, n);                                                         \
        T d2 = d + d;                                                                       \
        for (int k = 0; k < n; k++) { T m = d2 * b[k]; r[k] = a[k] - m; }                   \
        break;                                                                              \
    }                                                                                       \
    case ACC_REFRACT: {                                                                     \
        T eta = RD(fr + sp->syms[sp->param_slot[2]].offset);                                \
        T d = acc_dot_##K(b, a, n);                                                         \
        T dd = d * d, in = (T)1 - dd, ee = eta * eta, kk = (T)1 - ee * in;                  \
        T s = SQRT(kk > 0 ? kk : (T)0);                                                     \
        T e = eta * d + s;                                                                  \
        T on = kk < 0 ? (T)0 : (T)1;                                                        \
        for (int k = 0; k < n; k++) { T p = eta * a[k]; T q = e * b[k]; r[k] = (p - q) * on; } \
        break;                                                                              \
    }                                                                                       \
    case ACC_CROSS:                                                                         \
        if (n != 3) return 0;                                                               \
        r[0] = a[1] * b[2] - a[2] * b[1];                                                   \
        r[1] = a[2] * b[0] - a[0] * b[2];                                                   \
        r[2] = a[0] * b[1] - a[1] * b[0];                                                   \
        break;                                                                              \
    case ACC_FACEFORWARD: {                                                                 \
        if (acc_load_##K(sp, fr, 2, c) != n) return 0;                                      \
        T s = acc_dot_##K(c, b, n) < 0 ? (T)1 : (T)-1;                                      \
        for (int k = 0; k < n; k++) r[k] = a[k] * s;                                        \
        break;                                                                              \
    }                                                                                       \
    default: return 0;                                                                      \
    }                                                                                       \
    acc_ret_##K(sp, fr, r, n);                                                              \
    return 1;                                                                               \
}

enum { ACC_DOT, ACC_LENGTH, ACC_DISTANCE, ACC_NORMALIZE, ACC_REFLECT, ACC_REFRACT,
       ACC_CROSS, ACC_FACEFORWARD };

ACC_KIND(float,  f32, read_f32, write_f32, m_sqrtf)
ACC_KIND(double, f64, read_f64, write_f64, m_sqrt)

static int acc_geo(int op, Func *sp, unsigned char *fr) {
    VMSym *s = &sp->syms[sp->param_slot[0]];
    VTKind ek = is_array(s->type.kind) || is_slice(s->type.kind) ? arr_elem(s->type.kind) : s->type.kind;
    if (ek == VMT_F32) return acc_geo_f32(op, sp, fr);
    if (ek == VMT_F64) return acc_geo_f64(op, sp, fr);
    return 0;
}

static int acc_dot(Func *sp, unsigned char *fr)         { return acc_geo(ACC_DOT, sp, fr); }
static int acc_length(Func *sp, unsigned char *fr)      { return acc_geo(ACC_LENGTH, sp, fr); }
static int acc_distance(Func *sp, unsigned char *fr)    { return acc_geo(ACC_DISTANCE, sp, fr); }
static int acc_normalize(Func *sp, unsigned char *fr)   { return acc_geo(ACC_NORMALIZE, sp, fr); }
static int acc_reflect(Func *sp, unsigned char *fr)     { return acc_geo(ACC_REFLECT, sp, fr); }
static int acc_refract(Func *sp, unsigned char *fr)     { return acc_geo(ACC_REFRACT, sp, fr); }
static int acc_cross(Func *sp, unsigned char *fr)       { return acc_geo(ACC_CROSS, sp, fr); }
static int acc_faceforward(Func *sp, unsigned char *fr) { return acc_geo(ACC_FACEFORWARD, sp, fr); }

#if defined(__clang__)
#pragma STDC FP_CONTRACT DEFAULT
#elif defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC pop_options
#endif

#endif
