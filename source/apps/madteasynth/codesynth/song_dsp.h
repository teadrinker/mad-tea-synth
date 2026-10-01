// Sample playback DSP shared by live playback (sample_loader.c) and the exported
// song.c, which inlines this text verbatim: a streaming QOA decoder plus smp()
// interpolation, in float and fx22. Each voice decodes through a 21-sample
// window as it plays, no malloc. #define NEED_SMP to instantiate the playback
// functions; otherwise only the types. No libc or project headers, so song.c
// stays portable.

#ifndef SONG_DSP_H
#define SONG_DSP_H

// Immutable, shared by every voice playing it.
typedef struct song_sample_t {
    const unsigned char *qoa_data;   // QOA-compressed bitstream (owned elsewhere)
    int                  qoa_len;    // bytes in qoa_data
    int                  num_samples;
    int                  rate_scale_fx;  // fx22 speed compensation for `degrade` (degraded_len / original_len)
    int                  sample_rate;  // original rate before degrade; pitch-corrects against host_sample_rate
} song_sample_t;

// Per-voice decode state; always visible so it can embed in a voice.
// Zero-initialise, then song_smp_bind().
#define SONG_QOA_LMS_LEN 4
#define SONG_SMP_WIN     21   // 20 (one QOA slice) + 1 interpolation lookahead

typedef struct { int history[SONG_QOA_LMS_LEN]; int weights[SONG_QOA_LMS_LEN]; } song_qoa_lms_t;

typedef struct song_smp_voice_t {
    int                  host_sample_rate;  // current output rate (0 = the sample's own)
    const song_sample_t *s;        // bound sample (compressed source)
    int                  note_sample;  // note-relative output index, stamped by the host; position = note_sample * rate
    // decoder state for the next sample:
    unsigned int         byte_pos; // read offset into s->qoa_data
    int                  frame_left; // samples left in the current frame
    int                  slice_left; // samples left in the current slice
    unsigned long long   slice;      // current slice bits, top-aligned
    int                  sf;          // current slice scalefactor index
    song_qoa_lms_t       lms;         // running predictor state
    int                  next_idx;    // absolute index of the next sample to emit
    // ring of recently decoded samples, for interpolation
    short                win[SONG_SMP_WIN];
    int                  win_base;    // absolute index of the oldest live sample; <0 = unseeked
} song_smp_voice_t;

#endif // SONG_DSP_H

// Playback: streaming QOA decode + smp()
#if defined(NEED_SMP) && !defined(SONG_DSP_SMP_IMPL)
#define SONG_DSP_SMP_IMPL

// QOA framing (lib/qoa/qoa.h). Samples are mono, so every frame but the last is
// SONG_QOA_MONO_FRAME_BYTES and seeking is O(1).
#define SONG_QOA_SLICE_LEN         20
#define SONG_QOA_SLICES_PER_FRAME  256
#define SONG_QOA_FRAME_LEN         (SONG_QOA_SLICES_PER_FRAME * SONG_QOA_SLICE_LEN)   // 5120
#define SONG_QOA_MONO_FRAME_BYTES  (8 + SONG_QOA_LMS_LEN * 4 + 8 * SONG_QOA_SLICES_PER_FRAME) // 2072

static const int song_qoa_dequant_tab[16][8] = {
    {   1,   -1,    3,   -3,    5,   -5,     7,    -7},
    {   5,   -5,   18,  -18,   32,  -32,    49,   -49},
    {  16,  -16,   53,  -53,   95,  -95,   147,  -147},
    {  34,  -34,  113, -113,  203, -203,   315,  -315},
    {  63,  -63,  210, -210,  378, -378,   588,  -588},
    { 104, -104,  345, -345,  621, -621,   966,  -966},
    { 158, -158,  528, -528,  950, -950,  1477, -1477},
    { 228, -228,  760, -760, 1368,-1368,  2128, -2128},
    { 316, -316, 1053,-1053, 1895,-1895,  2947, -2947},
    { 422, -422, 1405,-1405, 2529,-2529,  3934, -3934},
    { 548, -548, 1828,-1828, 3290,-3290,  5117, -5117},
    { 696, -696, 2320,-2320, 4176,-4176,  6496, -6496},
    { 868, -868, 2893,-2893, 5207,-5207,  8099, -8099},
    {1064,-1064, 3548,-3548, 6386,-6386,  9933, -9933},
    {1286,-1286, 4288,-4288, 7718,-7718, 12005,-12005},
    {1536,-1536, 5120,-5120, 9216,-9216, 14336,-14336},
};

static int song_qoa_clamp_s16(int v) {
    if ((unsigned int)(v + 32768) > 65535) { if (v < -32768) return -32768; if (v > 32767) return 32767; }
    return v;
}

static unsigned long long song_qoa_read_u64(const unsigned char *b, unsigned int *p) {
    b += *p; *p += 8;
    return ((unsigned long long)b[0]<<56)|((unsigned long long)b[1]<<48)|
           ((unsigned long long)b[2]<<40)|((unsigned long long)b[3]<<32)|
           ((unsigned long long)b[4]<<24)|((unsigned long long)b[5]<<16)|
           ((unsigned long long)b[6]<<8)|(unsigned long long)b[7];
}

static int song_qoa_lms_predict(const song_qoa_lms_t *lms) {
    int p = 0;
    for (int i = 0; i < SONG_QOA_LMS_LEN; i++) p += lms->weights[i] * lms->history[i];
    return p >> 13;
}
static void song_qoa_lms_update(song_qoa_lms_t *lms, int sample, int residual) {
    int delta = residual >> 4;
    for (int i = 0; i < SONG_QOA_LMS_LEN; i++) lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
    for (int i = 0; i < SONG_QOA_LMS_LEN - 1; i++) lms->history[i] = lms->history[i+1];
    lms->history[SONG_QOA_LMS_LEN-1] = sample;
}

// ipf(a, b, x, y): 2-point interpolation shaped by filter y:
//   y in [0,1]  nearest (0) .. linear (1)
//   y in (1,2]  smoothstep blend; y > 2 smootherstep (float only)
//   y < 0       crossfade to silence

static float song_ipf(float a, float b, float x, float y) {
    float mu=1.0f,ad=0.0f,slide=1.0f;
    if(y>1.0f){
        x=x+(y-1.0f)*(x*x*(3.0f-2.0f*x)-x);
        if(y>2.0f) x=x+(y-2.0f)*(x*x*x*(x*(x*6.0f-15.0f)+10.0f)-x);
    }else if(y<0.0f){
        slide=(1.0f+y)*(1.0f+y); mu=slide-1.0f; ad=(x<0.5f)?a:b;
    }else{ slide=y*y; }
    float t=(x-0.5f)/slide+0.5f;
    if(t<0.0f)t=0.0f;
    if(t>1.0f)t=1.0f;
    return (a+t*(b-a))*mu+ad;
}

// fx22: 1.0 == 1 << 22.
#define SONG_FX22_SHIFT 22
#define SONG_FX22_ONE   (1<<SONG_FX22_SHIFT)
#define SONG_FX22_HALF  (1<<(SONG_FX22_SHIFT-1))
#define SONG_FX22_MUL(A,B) ((int)(((long long)(A)*(long long)(B))>>SONG_FX22_SHIFT))
#define SONG_FX22_DIV(A,B) ((int)(((long long)(A)<<SONG_FX22_SHIFT)/(long long)(B)))

// Fixed-point song_ipf; y > 2 clamps to 2, since smootherstep is float-only.
static int song_ipf_fx(int a, int b, int x, int y) {
    int t, mu=SONG_FX22_ONE, ad=0;
    if(y<0){
        int slide=SONG_FX22_MUL(SONG_FX22_ONE+y,SONG_FX22_ONE+y); // (1+y)^2
        mu=slide-SONG_FX22_ONE;
        ad=(x<SONG_FX22_HALF)?a:b;
        if(slide<=0) t=(x<SONG_FX22_HALF)?0:SONG_FX22_ONE;
        else         t=SONG_FX22_DIV(x-SONG_FX22_HALF,slide)+SONG_FX22_HALF;
    }else if(y<=2048){ // div issues in the range 0 to 2048 (0.03125)
        t=(x<SONG_FX22_HALF)?0:SONG_FX22_ONE;            // nearest
    }else if(y<=SONG_FX22_ONE){
        int slide=SONG_FX22_MUL(y,y);                    // y^2
        if(slide<=0) t=(x<SONG_FX22_HALF)?0:SONG_FX22_ONE;   // y ~ 0 -> nearest
        else         t=SONG_FX22_DIV(x-SONG_FX22_HALF,slide)+SONG_FX22_HALF;
    }else{
        if(y>2*SONG_FX22_ONE) y=2*SONG_FX22_ONE;         // clamp exotic tail
        int ss=SONG_FX22_MUL(SONG_FX22_MUL(x,x),(3*SONG_FX22_ONE-2*x)); // x^2*(3-2x)
        int w=y-SONG_FX22_ONE;                           // blend factor in [0,1]
        t=x+SONG_FX22_MUL(w,ss-x);
    }
    if(t<0)t=0;
    if(t>SONG_FX22_ONE)t=SONG_FX22_ONE;
    return SONG_FX22_MUL(a+SONG_FX22_MUL(t,b-a),mu)+ad;
}

// Per-voice streaming decoder: advances with the play position and re-seeks in
// O(1) on a jump.

// Keeps the stream position for the same sample; invalidates it for another.
static void song_smp_bind(song_smp_voice_t *v, const song_sample_t *s, int host_sr) {
    if (v->s != s) { v->s = s; v->win_base = -1; v->next_idx = 0; }
    v->host_sample_rate = host_sr;
}

// Load the frame header + LMS state at v->byte_pos (mono: one channel).
static void song_smp_frame_start(song_smp_voice_t *v) {
    const unsigned char *d = v->s->qoa_data;
    unsigned int p = v->byte_pos;
    unsigned long long fh = song_qoa_read_u64(d, &p);
    unsigned int fsamples = (unsigned int)((fh >> 16) & 0xffff);
    unsigned long long hist = song_qoa_read_u64(d, &p);
    unsigned long long wts  = song_qoa_read_u64(d, &p);
    for (int i = 0; i < SONG_QOA_LMS_LEN; i++) {
        v->lms.history[i] = (int)(signed short)(hist >> 48); hist <<= 16;
        v->lms.weights[i] = (int)(signed short)(wts  >> 48); wts  <<= 16;
    }
    v->byte_pos   = p;
    v->frame_left = (int)fsamples;
    v->slice_left = 0;
}

// Decode and return the next sample, appending it to the window ring.
static int song_smp_emit(song_smp_voice_t *v) {
    if (v->frame_left <= 0) song_smp_frame_start(v);
    if (v->slice_left <= 0) {
        unsigned int p = v->byte_pos;
        v->slice = song_qoa_read_u64(v->s->qoa_data, &p);
        v->byte_pos = p;
        v->sf = (int)((v->slice >> 60) & 0xf);
        v->slice <<= 4;
        v->slice_left = v->frame_left < SONG_QOA_SLICE_LEN ? v->frame_left : SONG_QOA_SLICE_LEN;
    }
    int pred = song_qoa_lms_predict(&v->lms);
    int q  = (int)((v->slice >> 61) & 0x7);
    int dq = song_qoa_dequant_tab[v->sf][q];
    int rc = song_qoa_clamp_s16(pred + dq);
    song_qoa_lms_update(&v->lms, rc, dq);
    v->slice <<= 3;
    v->slice_left--;
    v->frame_left--;
    v->win[v->next_idx % SONG_SMP_WIN] = (short)rc;
    v->next_idx++;
    if (v->next_idx - v->win_base > SONG_SMP_WIN) v->win_base = v->next_idx - SONG_SMP_WIN;
    return rc;
}

// Seek to `target`'s frame, then decode forward: at most one frame of replay.
static void song_smp_seek(song_smp_voice_t *v, int target) {
    int f = target / SONG_QOA_FRAME_LEN;
    v->byte_pos   = 8u + (unsigned int)f * SONG_QOA_MONO_FRAME_BYTES;
    v->frame_left = 0;
    v->slice_left = 0;
    v->next_idx   = f * SONG_QOA_FRAME_LEN;
    v->win_base   = v->next_idx;
    while (v->next_idx < target) song_smp_emit(v);
    v->win_base = v->next_idx;
}

// Far forward jumps re-seek too: a body reading two positions (smp(rate,0) +
// smp(rate*2,0)) would otherwise decode ever more per output sample. Exact
// either way, since each frame header carries its LMS state.
static int song_smp_fetch(song_smp_voice_t *v, int idx) {
    if (v->win_base < 0 || idx < v->win_base ||
        idx >= v->next_idx + SONG_QOA_FRAME_LEN) song_smp_seek(v, idx);
    while (v->next_idx <= idx) song_smp_emit(v);
    return v->win[idx % SONG_SMP_WIN];
}

// Float read. Position is note_sample * rate, so any number of smp() calls per
// sample agree. One-shot: silence past either end.
static inline float song_smp_stream(song_smp_voice_t *v, float rate, float filter) {
    if (!v) return 0.0f;
    const song_sample_t *s = v->s;
    if (!s || !s->qoa_data || s->num_samples < 1) return 0.0f;
    double sr_ratio = (s->sample_rate > 0 && v->host_sample_rate > 0)
                    ? (double)s->sample_rate / (double)v->host_sample_rate : 1.0;
    double pos = (double)v->note_sample * (double)rate
               * ((double)s->rate_scale_fx / (double)SONG_FX22_ONE) // degrade speed comp
               * sr_ratio;
    int n = s->num_samples;
    if (!(pos == pos) || pos < 0.0 || pos >= (double)n) return 0.0f; // NaN / off the ends
    int i0 = (int)pos; float frac = (float)(pos - (double)i0);
    int i1 = i0 + 1;
    int a = song_smp_fetch(v, i0);
    int b = (i1 < n) ? song_smp_fetch(v, i1) : a;        // hold the final sample
    return song_ipf((float)a/32768.0f, (float)b/32768.0f, frac, filter);
}

// fx22 read; the position product is 64-bit.
static inline int song_smp_stream_fx(song_smp_voice_t *v, int rate, int filter) {
    if (!v) return 0;
    const song_sample_t *s = v->s;
    if (!s || !s->qoa_data || s->num_samples < 1) return 0;
    // Fold degrade compensation into rate first, so the 64-bit position product
    // can't overflow.
    rate = (int)(((long long)rate * (long long)s->rate_scale_fx) >> SONG_FX22_SHIFT);
    if (s->sample_rate > 0 && v->host_sample_rate > 0)
        rate = (int)(((long long)rate * (long long)s->sample_rate) / (long long)v->host_sample_rate);
    long long posfx = (long long)v->note_sample * (long long)rate;   // fx22 position
    long long idx = posfx >> SONG_FX22_SHIFT;
    int n = s->num_samples;
    if (idx < 0 || idx >= n) return 0;                   // one-shot: silence off the ends
    int frac = (int)(posfx & (SONG_FX22_ONE - 1));
    int i0 = (int)idx; int i1 = i0 + 1;
    int a = (int)(short)song_smp_fetch(v, i0) << (SONG_FX22_SHIFT - 15);   // int16 (Q15) -> fx22
    int b = (i1 < n) ? ((int)(short)song_smp_fetch(v, i1) << (SONG_FX22_SHIFT - 15)) : a;
    return song_ipf_fx(a, b, frac, filter);
}

#endif // NEED_SMP
