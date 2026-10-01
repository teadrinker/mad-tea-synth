// WAV loading and the processing chain: tinywav for I/O, qoa.h to compress, a
// biquad for lp/hp, a cubic kernel for degrade. Playback lives in song_dsp.h.
#define NEED_SMP
#include "sample_loader.h"

#include "CodeSynthVmCtx.h"

#include "tinywavwrap.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// Encoding only; playback decodes via song_dsp.h.
#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include "../../lib/qoa/qoa.h"

// biquad, 12dB/oct

typedef struct {
    float a0, a1, a2, b0, b1, b2;
} Biquad;

static void biquad_init(Biquad *b) {
    b->a0 = 1.0f; b->a1 = 0.0f; b->a2 = 0.0f;
    b->b0 = 1.0f; b->b1 = 0.0f; b->b2 = 0.0f;
}

static void biquad_setup(Biquad *b, float hz, float sr, float q, float db, int is_peak, float mul) {
    float ra = powf(10.0f, db * 0.025f);
    float y  = cosf(6.283185307179586f * hz / sr);
    float x  = sinf(6.283185307179586f * hz / sr) * 0.5f / q;
    b->a1 = -2.0f * y;
    b->a0 =  1.0f + x * (1.0f / ra);
    b->a2 =  1.0f - x * (1.0f / ra);
    if (is_peak) {
        b->b1 = b->a1;
        b->b0 = 1.0f + (x * ra);
        b->b2 = 1.0f - (x * ra);
    } else {
        b->b0 = 0.5f * (1.0f - mul * y);
        b->b1 = mul * (1.0f - mul * y);
        b->b2 = b->b0;
    }
}

static void biquad_set_lp(Biquad *b, float hz, float sr, float q) {
    biquad_setup(b, hz, sr, q, 0.0f, 0, 1.0f);
}

static void biquad_set_hp(Biquad *b, float hz, float sr, float q) {
    biquad_setup(b, hz, sr, q, 0.0f, 0, -1.0f);
}

typedef struct {
    float x, y, px, py;
} BiquadState;

static float biquad_process(Biquad *b, BiquadState *s, float in) {
    float out = (b->b0 * in + b->b1 * s->x + b->b2 * s->px - b->a1 * s->y - b->a2 * s->py) / b->a0;
    s->px = s->x; s->py = s->y; s->x = in; s->y = out;
    return out;
}

// chain parsing

typedef enum { CH_DEGRADE, CH_LP, CH_HP, CH_Q,
               CH_TRIM_START, CH_TRIM_END, CH_FADE_START, CH_FADE_END } ChainOp;

// Trim steps carry three numbers; the rest only `val`.
typedef struct {
    ChainOp op;
    float   val;   // primary argument (percent / Hz / semitones / Q)
    float   fade;  // trim only: fadeout percent of the post-trim length
    float   bits;  // trim only: gate bit depth
} ChainStep;

#define CH_DEFAULT_BITS 12.0f

static int parse_args(const char *a, float *out, int max) {
    int n = 0;
    while (n < max) {
        while (*a == ' ') a++;
        if (!*a || *a == ',') break;
        out[n++] = (float)atof(a);
        while (*a && *a != ' ' && *a != ',') a++;
    }
    return n;
}

static void parse_trim(const char *a, ChainStep *s, ChainOp op) {
    float v[3] = { 0.0f, 0.0f, CH_DEFAULT_BITS };
    parse_args(a, v, 3);
    s->op = op; s->val = v[0]; s->fade = v[1]; s->bits = v[2];
}

static int parse_chain(const char *chain, ChainStep *steps, int max_steps) {
    if (!chain || !chain[0]) return 0;
    int n = 0;
    const char *p = chain;
    while (*p && n < max_steps) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (strncmp(p, "degrade=", 8) == 0) {
            steps[n].op = CH_DEGRADE;
            steps[n].val = (float)atof(p + 8);
            n++;
            p += 8; while (*p && *p != ',') p++;
        } else if (strncmp(p, "lp=", 3) == 0) {
            steps[n].op = CH_LP;
            steps[n].val = (float)atof(p + 3);
            n++;
            p += 3; while (*p && *p != ',') p++;
        } else if (strncmp(p, "hp=", 3) == 0) {
            steps[n].op = CH_HP;
            steps[n].val = (float)atof(p + 3);
            n++;
            p += 3; while (*p && *p != ',') p++;
        } else if (strncmp(p, "trim_start=", 11) == 0) {
            parse_trim(p + 11, &steps[n], CH_TRIM_START);
            n++;
            p += 11; while (*p && *p != ',') p++;
        } else if (strncmp(p, "trim_end=", 9) == 0) {
            parse_trim(p + 9, &steps[n], CH_TRIM_END);
            n++;
            p += 9; while (*p && *p != ',') p++;
        } else if (strncmp(p, "trim=", 5) == 0) {
            // The cut and fade apply to the end; the bit gate to both ends.
            parse_trim(p + 5, &steps[n], CH_TRIM_END);
            if (n + 1 < max_steps) {
                steps[n + 1].op   = CH_TRIM_START;
                steps[n + 1].val  = 0.0f;
                steps[n + 1].fade = 0.0f;
                steps[n + 1].bits = steps[n].bits;
                n++;
            }
            n++;
            p += 5; while (*p && *p != ',') p++;
        } else if (strncmp(p, "fade_start=", 11) == 0) {
            steps[n].op = CH_FADE_START;
            steps[n].val = (float)atof(p + 11);
            n++;
            p += 11; while (*p && *p != ',') p++;
        } else if (strncmp(p, "fade_end=", 9) == 0) {
            steps[n].op = CH_FADE_END;
            steps[n].val = (float)atof(p + 9);
            n++;
            p += 9; while (*p && *p != ',') p++;
        } else if (strncmp(p, "q=", 2) == 0) {
            steps[n].op = CH_Q;
            steps[n].val = (float)atof(p + 2);
            n++;
            p += 2; while (*p && *p != ',') p++;
        } else {
            while (*p && *p != ',') p++;
        }
    }
    return n;
}

static int fade_pct_to_len(float pct, int ns) {
    if (pct <= 0.0f) return 0;
    if (pct > 100.0f) pct = 100.0f;
    int len = (int)((float)ns * pct * 0.01f + 0.5f);
    return len > ns ? ns : len;
}

// Catmull-Rom through p0..p1 at t; m1/p2 are the outer neighbours.
static float cubicint(float t, float m1, float p0, float p1, float p2) {
    return p0 + 0.5f * t * (p1 - m1 +
                     t * (2.0f*m1 - 5.0f*p0 + 4.0f*p1 - p2 +
                     t * (p2 - 3.0f*p1 + 3.0f*p0 - m1)));
}

cSampleData *sample_load_wav(const char *filename, const char *chain) {
    int sr = 0, ns = 0;
    float *L = NULL, *R = NULL;
    if (wav_load(filename, &sr, &ns, &L, &R) != 0) return NULL;
    if (!L || ns <= 0) { free(L); free(R); return NULL; }

    ChainStep steps[16];
    int nsteps = parse_chain(chain, steps, 16);

    float q = 0.7071f;

    for (int i = 0; i < nsteps; i++)
        if (steps[i].op == CH_Q) { q = steps[i].val; break; }

    float degrade_semitones = 0.0f;
    for (int i = 0; i < nsteps; i++)
        if (steps[i].op == CH_DEGRADE) { degrade_semitones = steps[i].val; break; }

    Biquad bq[8];
    BiquadState bqs[8];
    int nbq = 0;
    for (int i = 0; i < nsteps && nbq < 8; i++) {
        if (steps[i].op == CH_LP) {
            biquad_init(&bq[nbq]);
            biquad_set_lp(&bq[nbq], steps[i].val, (float)sr, q);
            memset(&bqs[nbq], 0, sizeof(BiquadState));
            nbq++;
        } else if (steps[i].op == CH_HP) {
            biquad_init(&bq[nbq]);
            biquad_set_hp(&bq[nbq], steps[i].val, (float)sr, q);
            memset(&bqs[nbq], 0, sizeof(BiquadState));
            nbq++;
        }
    }

    float *mono = (float*)malloc((size_t)ns * sizeof(float));
    if (!mono) { free(L); free(R); return NULL; }

    // left channel only
    memcpy(mono, L, (size_t)ns * sizeof(float));
    free(L); free(R);

    // Trim first, so filters, fades and degrade work on the shortened sample.
    int start = 0, end = ns;
    float fade_in_pct = 0.0f, fade_out_pct = 0.0f;
    for (int i = 0; i < nsteps; i++) {
        ChainOp op = steps[i].op;
        if (op == CH_FADE_START) { fade_in_pct  = steps[i].val; continue; }
        if (op == CH_FADE_END)   { fade_out_pct = steps[i].val; continue; }
        if (op != CH_TRIM_START && op != CH_TRIM_END) continue;

        float pct = steps[i].val;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        int cut = (int)((float)ns * pct * 0.01f + 0.5f);

        int bits = (int)steps[i].bits;
        if (bits < 1)  bits = 1;
        if (bits > 16) bits = 16;
        float thr = (float)(1 << (16 - bits)) / 32768.0f;

        if (op == CH_TRIM_START) {
            start += cut;
            if (start > end) start = end;
            while (start < end && fabsf(mono[start]) < thr) start++;
            if (steps[i].fade > 0.0f) fade_in_pct = steps[i].fade;
        } else {
            end -= cut;
            if (end < start) end = start;
            while (end > start && fabsf(mono[end - 1]) < thr) end--;
            if (steps[i].fade > 0.0f) fade_out_pct = steps[i].fade;
        }
    }
    // An all-silent sample keeps one frame for QOA.
    if (end <= start) { if (start >= ns) start = ns - 1; end = start + 1; }
    if (start > 0) memmove(mono, mono + start, (size_t)(end - start) * sizeof(float));
    ns = end - start;

    if (nbq > 0) {
        for (int i = 0; i < ns; i++) {
            float v = mono[i];
            for (int j = 0; j < nbq; j++)
                v = biquad_process(&bq[j], &bqs[j], v);
            mono[i] = v;
        }
    }

    // Fades after the biquads, so the ramps end on exact zeros.
    {
        int flen = fade_pct_to_len(fade_in_pct, ns);
        for (int k = 0; k < flen; k++)
            mono[k] *= (float)(k + 1) / (float)flen;
        flen = fade_pct_to_len(fade_out_pct, ns);
        for (int k = 0; k < flen; k++)
            mono[ns - flen + k] *= 1.0f - (float)(k + 1) / (float)flen;
    }

    int out_sr = sr;
    int out_ns = ns;
    float *out_pcm = mono;
    if (degrade_semitones != 0.0f) {
        // -12 = one octave down = half the rate
        float ratio = powf(2.0f, degrade_semitones / 12.0f);
        int new_sr = (int)((float)sr * ratio + 0.5f);
        int new_ns = (int)((float)ns * ratio + 0.5f);
        if (new_ns < 1) new_ns = 1;
        if (new_sr < 1) new_sr = 1;
        float *resampled = (float*)malloc((size_t)new_ns * sizeof(float));
        // Commit the dimensions only once the buffer exists.
        if (resampled) {
            out_sr = new_sr;
            out_ns = new_ns;
            for (int i = 0; i < out_ns; i++) {
                float src_pos = (float)i / ratio;
                int   si = (int)src_pos;
                float frac = src_pos - (float)si;
                float m1 = (si - 1 >= 0)    ? mono[si - 1] : mono[0];
                float p0 = (si < ns)        ? mono[si]     : mono[ns - 1];
                float p1 = (si + 1 < ns)    ? mono[si + 1] : mono[ns - 1];
                float p2 = (si + 2 < ns)    ? mono[si + 2] : mono[ns - 1];
                resampled[i] = cubicint(frac, m1, p0, p1, p2);
            }
            free(mono);
            out_pcm = resampled;
        }
    }

    unsigned char *qoa_bitstream = NULL;
    int qoa_bitstream_len = 0;
    {
        short *i16 = (short*)malloc((size_t)out_ns * sizeof(short));
        if (i16) {
            for (int i = 0; i < out_ns; i++) {
                float v = out_pcm[i];
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                i16[i] = (short)(v * 32767.0f);
            }
            qoa_desc qoa;
            qoa.channels = 1;
            qoa.samplerate = out_sr;
            qoa.samples = out_ns;
            unsigned int encoded_len = 0;
            void *encoded = qoa_encode(i16, &qoa, &encoded_len);
            if (encoded) {
                qoa_bitstream = (unsigned char*)malloc(encoded_len);
                if (qoa_bitstream) {
                    memcpy(qoa_bitstream, encoded, encoded_len);
                    qoa_bitstream_len = (int)encoded_len;
                }
                QOA_FREE(encoded);
            }
            free(i16);
        }
    }
    free(out_pcm);

    // The sample owns only the compressed bytes and metadata.
    cSampleData *s = (cSampleData*)malloc(sizeof(cSampleData));
    if (!s) { free(qoa_bitstream); return NULL; }
    s->qoa_data    = qoa_bitstream;
    s->qoa_len     = qoa_bitstream_len;
    s->num_samples = out_ns;
    // A shrunk buffer plays slower to span the same duration; exactly SONG_FX22_ONE
    // without degrade.
    s->rate_scale_fx = (int)((double)out_ns / (double)ns * (double)SONG_FX22_ONE + 0.5);
    s->sample_rate = sr;  // original rate, before degrade
    return s;
}

void sample_free(cSampleData *s) {
    if (s) { free((void*)s->qoa_data); free(s); }
}

void smp_bind_voice(song_smp_voice_t *v, const cSampleData *s, double host_sr) {
    song_smp_bind(v, s, (int)host_sr);
}

// ctx is a CodeSynthVmCtx; its `smp` is the playing voice's cursor, or null,
// which song_smp_stream reads as silence.

static song_smp_voice_t *smp_voice(void *ctx) {
    CodeSynthVmCtx *c = (CodeSynthVmCtx*)ctx;
    return c ? c->smp : 0;
}

float smp_f32(void *ctx, float rate, float filter) {
    return song_smp_stream(smp_voice(ctx), rate, filter);
}

double smp_f64(void *ctx, double rate, double filter) {
    return (double)song_smp_stream(smp_voice(ctx), (float)rate, (float)filter);
}

int smp_fx22(void *ctx, int rate, int filter) {
    return song_smp_stream_fx(smp_voice(ctx), rate, filter);
}
