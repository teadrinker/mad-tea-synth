// The microw8 player for an exported song: no entry point, just two exports.
//   void  upd()                 once per displayed frame (60 Hz nominal)
//   float snd(int sampleIndex)  once per sample
// Not generated; everything that varies per export is in song_config.h.

#include "song_config.h"   // SONG_SAMPLE_RATE, VSCREEN_W/H, the arena sizes
#include "song/song.h"
#include "vscreen.h"

// Framebuffer: 320x240 palette indices at 0x78 (song_config.h points
// VSCREEN_FRAMEBUFFER_ADDR here). Palette: 256 RGBA entries at 0x13000.
#define UW8_FRAMEBUFFER ((unsigned char *)0x78)
#define UW8_PALETTE     ((unsigned char *)0x13000)
#define UW8_SCREEN_W    320
#define UW8_SCREEN_H    240

// microw8's only clock: seconds since the cart started, a wasm import from "env".
__attribute__((import_module("env"), import_name("time")))
extern float uw8_time(void);

// `palette` IS the hardware table (VSCREEN_PALETTE_ADDR): a vscreen entry
// 0xAABBGGRR is these R,G,B,A bytes as a little-endian word. Program the Pebble
// colour cube once, so a GColor8 framebuffer byte is already the right index and
// the song looks the same on every target.
static void palette_init(void)
{
    for (int i = 0; i < 256; i++)
        vscreen_palette[i] = VSCREEN_PALETTE_DEFAULT(i);
}

// Audio: snd() pulls one sample; song_render_audio_batch makes a batch at a
// time. The index is stereo: snd() is called per channel, interleaved, so the
// frame index is sampleIndex >> 1 and the low bit picks the channel. Get this
// wrong and the song plays at double speed. The host never rewinds; a backward
// jump is clamped to the current batch.
static short          g_batch[SONG_BATCH_SAMPLES * SONG_OUTPUT_CHANNELS];
static int            g_batch_start = -1;   // sampleIndex of g_batch[0]
static int            g_song_sample_id;
static song_SongState g_song_state;

// Named exports: --export-all yields a cart that packs but fails to load.
__attribute__((export_name("snd")))
float snd(int sampleIndex)
{
    // Stereo index -> frame index.
    const int frame = sampleIndex >> 1;

    if (g_batch_start < 0)
    {
        g_song_sample_id = song_render_audio_batch(0, &g_song_state, g_batch);
        g_batch_start = 0;
    }

    while (frame >= g_batch_start + SONG_BATCH_SAMPLES)
    {
        g_batch_start += SONG_BATCH_SAMPLES;
        g_song_sample_id = song_render_audio_batch(g_song_sample_id, &g_song_state, g_batch);
    }

    int off = frame - g_batch_start;
    if (off < 0) off = 0;
    if (off >= SONG_BATCH_SAMPLES) off = SONG_BATCH_SAMPLES - 1;

    // On a stereo song the low bit picks the channel; mono reads the one sample.
#if SONG_OUTPUT_CHANNELS == 2
    return (float)g_batch[off * 2 + (sampleIndex & 1)] * (1.0f / 32768.0f);
#else
    return (float)g_batch[off] * (1.0f / 32768.0f);
#endif
}

// Video: the song position comes from the host clock, never a frame counter.
// upd() is skipped when frames run long while snd() is not, so a counter would
// drift behind the audio. Computed in double (f32 loses whole samples past
// 2^24), clamped monotonic since the visual sequencer only walks forward, and
// not rebased to zero: audio already runs ~1 s before the first upd().
static song_SongState g_visual_state;
static int            g_started;
static int            g_last_sample_id;

__attribute__((export_name("upd")))
void upd(void)
{
    if (!g_started) { palette_init(); g_started = 1; }

    double t = (double)uw8_time();        // seconds since the cart started
    int sample_id = (t > 0.0) ? (int)(t * (double)SONG_SAMPLE_RATE) : 0;

    if (sample_id < g_last_sample_id) sample_id = g_last_sample_id;
    g_last_sample_id = sample_id;

    if (sample_id >= song_total_samples()) return;  // song over; leave the last frame up

#ifdef SONG_VISUALS
    // No blit: bodies draw straight into microw8's framebuffer, whose palette
    // already maps GColor8 bytes.
    vscreen_images_reset();
    song_visual_render(sample_id, &g_visual_state);
#endif
}
