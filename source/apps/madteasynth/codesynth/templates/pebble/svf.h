#ifndef TEATIME_SVF_H
#define TEATIME_SVF_H

// Fixed-point TPT (trapezoidal) state-variable filter as a band/peak bell:
// cutoff + width + peak gain, SVF_STAGES sections in series. TPT is stable and
// accurate to Nyquist with independent cutoff and width, at one divide per
// parameter update. State is int64 with SVF_FX_SHIFT (20) fractional bits.
// Per stage: dry + gain * unity band-pass; gain -1.0 is the exact notch, where
// the cut side stops.

#include <stdint.h>

#define SVF_FX_SHIFT 20
#define SVF_FX_ONE   ((int64_t)1 << SVF_FX_SHIFT)          // 1.0
#define SVF_STATE_MAX ((int64_t)1 << 40)                   // state limiter rail

// Each stage has its own cutoff/width/gain.
#define SVF_STAGES 3

// Cutoff coefficient g = tan(pi*fc/fs), mapped linearly from the knob: ~30 Hz ..
// ~7 kHz at 16 kHz.
#define SVF_G_MIN (SVF_FX_ONE / 160)                       // ~0.006  (~30 Hz)
#define SVF_G_MAX (SVF_FX_ONE * 5)                         // 5.0     (~7 kHz)
// Damping k = 1/Q. K_MAX must stay < 8.0 given the 2^40 state clamp (7*2^40 < 2^63).
#define SVF_K_MIN (SVF_FX_ONE / 20)                        // 0.05    (Q ~= 20, narrow)
#define SVF_K_MAX (SVF_FX_ONE * 7)                         // 7.0     (Q ~= 0.14, very wide)
// Peak gain; knob 32768 = flat.
#define SVF_BOOST_MAX (SVF_FX_ONE * 4)                     // knob 65535 -> +4.0
#define SVF_CUT_MAX   (SVF_FX_ONE)                         // knob 0     -> -1.0 (notch)

typedef struct {
    int64_t ic1eq, ic2eq;   // trapezoidal integrator states (audio rate)
    int64_t a1, a2, a3;      // integrator coefficients (control rate)
    int64_t k;               // damping = 1/Q; also normalises the band-pass
    int64_t gain;            // signed peak gain: +boost / 0 flat / -1 notch
} Svf;

typedef struct {
    Svf stage[SVF_STAGES];
} SvfChain;

static inline int64_t svf_mul(int64_t a, int64_t b) {
    return (a * b) >> SVF_FX_SHIFT;
}

static inline void svf_reset(Svf* s) {
    s->ic1eq = 0;
    s->ic2eq = 0;
}

// Three 0..65535 knobs -> one section's coefficients. gain: 32768 flat, 0 full notch.
static inline void svf_set_params(Svf* s, uint16_t cutoff, uint16_t width,
                                  uint16_t gain) {
    // /65536 rather than /65535: inaudible, and no 64-bit divide.
    int64_t g = SVF_G_MIN + (((int64_t)cutoff * (SVF_G_MAX - SVF_G_MIN)) >> 16);
    int64_t k = SVF_K_MIN + (((int64_t)width  * (SVF_K_MAX - SVF_K_MIN)) >> 16);

    // The one divide, at control rate; denom >= 1.0.
    int64_t denom = SVF_FX_ONE + svf_mul(g, g + k);
    s->a1 = ((int64_t)SVF_FX_ONE << SVF_FX_SHIFT) / denom;   // 1/denom in Q20
    s->a2 = svf_mul(g, s->a1);
    s->a3 = svf_mul(g, s->a2);
    s->k  = k;

    // Piecewise around centre, so the knob is monotonic.
    int64_t d = (int64_t)gain - 32768;
    s->gain = (d >= 0) ? ((d * SVF_BOOST_MAX) >> 15)
                       : ((d * SVF_CUT_MAX)   >> 15);
}

static inline int16_t svf_process(Svf* s, int16_t in_i16) {
    int64_t v0 = (int64_t)in_i16 << SVF_FX_SHIFT;

    // TPT SVF core (Cytomic form).
    int64_t v3 = v0 - s->ic2eq;
    int64_t v1 = svf_mul(s->a1, s->ic1eq) + svf_mul(s->a2, v3);
    // Keeps the k*v1 multiplies inside int64 at extreme settings.
    if (v1 >  SVF_STATE_MAX) v1 =  SVF_STATE_MAX;
    if (v1 < -SVF_STATE_MAX) v1 = -SVF_STATE_MAX;
    int64_t v2 = s->ic2eq + svf_mul(s->a2, s->ic1eq) + svf_mul(s->a3, v3);
    s->ic1eq = 2 * v1 - s->ic1eq;
    s->ic2eq = 2 * v2 - s->ic2eq;

    // Limiter; normal audio never trips it.
    if (s->ic1eq >  SVF_STATE_MAX) s->ic1eq =  SVF_STATE_MAX;
    if (s->ic1eq < -SVF_STATE_MAX) s->ic1eq = -SVF_STATE_MAX;
    if (s->ic2eq >  SVF_STATE_MAX) s->ic2eq =  SVF_STATE_MAX;
    if (s->ic2eq < -SVF_STATE_MAX) s->ic2eq = -SVF_STATE_MAX;

    // k*v1 is the unity-peak band-pass; gain -1 gives the notch. Clamped so gain*bp
    // stays in int64.
    int64_t bp  = svf_mul(s->k, v1);
    if (bp >  SVF_STATE_MAX) bp =  SVF_STATE_MAX;
    if (bp < -SVF_STATE_MAX) bp = -SVF_STATE_MAX;
    int64_t out = v0 + svf_mul(s->gain, bp);

    int32_t o = (int32_t)(out >> SVF_FX_SHIFT);
    if (o >  32767) o =  32767;
    if (o < -32768) o = -32768;
    return (int16_t)o;
}

// ---- Cascade ----

static inline void svf_chain_reset(SvfChain* c) {
    for (int i = 0; i < SVF_STAGES; i++) svf_reset(&c->stage[i]);
}

static inline void svf_chain_set_params(SvfChain* c, uint16_t cutoff,
                                        uint16_t width, uint16_t gain) {
    for (int i = 0; i < SVF_STAGES; i++)
        svf_set_params(&c->stage[i], cutoff, width, gain);
}

static inline int16_t svf_chain_process(SvfChain* c, int16_t in) {
    for (int i = 0; i < SVF_STAGES; i++) in = svf_process(&c->stage[i], in);
    return in;
}

#endif // TEATIME_SVF_H
