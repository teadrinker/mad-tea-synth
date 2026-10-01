#pragma once

// Only the type unless the includer defines NEED_QOA/NEED_SMP.
#include "song_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Same layout the export emits.
typedef struct song_sample_t cSampleData;

// Loads a WAV (left channel), applies a processing chain and compresses to QOA.
// The chain is comma-separated, applied in order:
//   degrade=X              resample by X semitones (cubic filter)
//   lp=X / hp=X            12dB/oct lowpass/highpass biquad at X Hz
//   q=X                    Q for all lp/hp steps (default 0.7071)
//   trim_end=X [Y] [B]     cut X% off the end, then gate inward past anything
//                          below a B-bit LSB (default 12); Y fades the last Y%
//   trim_start=X [Y] [B]   the same from the start
//   trim=X [Y] [B]         trim_start=0 0 B, trim_end=X Y B
//   fade_end=Y             fade out over the last Y% of the trimmed length
//   fade_start=Y           fade in over the first Y%
// NULL on failure; the caller owns the result.
cSampleData *sample_load_wav(const char *filename, const char *chain);

// Bind a voice's decode state to a sample (or NULL) before running its sound.
void smp_bind_voice(song_smp_voice_t *v, const cSampleData *s, double host_sr);

void sample_free(cSampleData *s);

// smp(rate, filter), ctx-registered: ctx is the VM's user_data. rate 1.0 =
// native pitch; filter 0 nearest, 1 linear, 2 smoothstep. One-shot.

float  smp_f32(void *ctx, float rate, float filter);
double smp_f64(void *ctx, double rate, double filter);
int    smp_fx22(void *ctx, int rate, int filter);  // fx22 in and out

#ifdef __cplusplus
}
#endif
