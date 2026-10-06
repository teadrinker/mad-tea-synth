// The Pebble/emery player for an exported song. Not generated; per-export
// values are in song_config.h. The audio timing here was tuned on hardware.
//   TEATIME_SYNTH_VISUALS  drop for an audio-only build
//   PEBBLE_SYNC_DEBUG=1    audio/visual sync instrumentation
#include <pebble.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "song_config.h"   // generated per export -- rate, sizes, EQ constants
#include "teatime_log.h"   // TEATIME_NO_LOG: compiles APP_LOG out
#include "pebble_tsys.h"

static Tsys   s_sys;
static Window *s_window;

static void tsys_print_and_send(const char *s) {
  APP_LOG(APP_LOG_LEVEL_INFO, "%s", s);
}

// Audio: streams the song to the speaker in chunks, no malloc.
#include "song/song.h"
#include "svf.h"

#ifdef TEATIME_SYNTH_VISUALS
static void song_visual_start(void);
static void song_visual_tick(void);
static void song_visual_stop(void);
#endif

// Defaults; song_config.h wins.
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE       16000
#endif
#ifndef AUDIO_VOLUME
#define AUDIO_VOLUME            100
#endif
#ifndef AUDIO_TIMER_MS
#define AUDIO_TIMER_MS          10
#endif
#define AUDIO_SAMPLES_PER_CHUNK SONG_BATCH_SAMPLES

// Compiled and played at different rates would silently retune the song.
_Static_assert(SONG_SAMPLE_RATE == AUDIO_SAMPLE_RATE,
               "song_config.h: SONG_SAMPLE_RATE must equal AUDIO_SAMPLE_RATE -- "
               "the song is compiled at one and played at the other.");

static bool      s_audio_playing;
static uint32_t  s_audio_chunk_offset;      // 0..AUDIO_SAMPLES_PER_CHUNK consumed from s_audio_chunk
static uint32_t  s_audio_samples_sent;      // queued to stream
static uint32_t  s_audio_samples_generated; // generated total
static int16_t   s_audio_chunk[AUDIO_SAMPLES_PER_CHUNK];
static AppTimer *s_audio_timer;

// PEBBLE_SYNC_DEBUG: audio/visual sync instrumentation, off by default
// (SONG_DEFINES='PEBBLE_SYNC_DEBUG=1', read with `pebble logs`). It measures:
//   behind   how often and how far the song fell behind the wall clock
//   ring     the firmware ring capacity (~240 ms)
//   low      lowest queue estimate (120-180 ms on healthy runs)
//   maxgap   longest stretch with nothing feeding the speaker (35-86 ms)
//   stall    pump ticks where the firmware wasn't Playing (always 0)
// By these numbers the glitches are not app-side starvation; disconnecting the
// phone's developer connection removes most of them.
#ifndef PEBBLE_SYNC_DEBUG
#define PEBBLE_SYNC_DEBUG 0
#endif

#if PEBBLE_SYNC_DEBUG

// 16 samples per ms at 16 kHz: the reference for every measurement.
#define AUDIO_SAMPLES_PER_MS (AUDIO_SAMPLE_RATE / 1000)

// Start latency makes `elapsed` overstate playback; only a larger shortfall counts.
#ifndef AUDIO_UNDERRUN_SLACK_SAMPLES
#define AUDIO_UNDERRUN_SLACK_SAMPLES 1600  // 100 ms
#endif

// Past this, a shortfall is a wall-clock step, not a dropout.
#ifndef AUDIO_RESYNC_MAX_SAMPLES
#define AUDIO_RESYNC_MAX_SAMPLES (2 * AUDIO_SAMPLE_RATE)  // 2 s
#endif

// -DTEATIME_AUDIO_DIAG=1 logs the ring capacity and each dropout. Off: the
// strings cost virtual-size budget.
#ifndef TEATIME_AUDIO_DIAG
#define TEATIME_AUDIO_DIAG 0
#endif

static uint32_t s_audio_t0_ms;      // wall clock at the first accepted write
static bool     s_audio_clocked;    // s_audio_t0_ms valid
static uint32_t s_audio_dropouts;   // dropouts detected this playback
static uint32_t s_audio_skipped;    // samples skipped to catch back up

// Dropout gap log, dumped after playback: logging over Bluetooth mid-song could
// cause the stall it reports. Records how long the speaker went unfed and the
// queue estimate when feeding resumed.
#ifndef AUDIO_GAP_LOG_MS
#define AUDIO_GAP_LOG_MS 40  // a fill later than this is worth a line
#endif
#ifndef AUDIO_GAP_LOG_MAX
#define AUDIO_GAP_LOG_MAX 12
#endif
static uint32_t s_last_fill_ms;   // wall clock of the previous fill
static uint32_t s_gap_max_ms;     // longest stretch with no fill at all
static uint32_t s_gap_count;      // fills later than AUDIO_GAP_LOG_MS
static int32_t  s_ring_min;       // lowest queue estimate seen, in samples
static uint32_t s_gap_sample[AUDIO_GAP_LOG_MAX];  // sample id at the late fill
static uint16_t s_gap_ms[AUDIO_GAP_LOG_MAX];      // how late
static int16_t  s_gap_left[AUDIO_GAP_LOG_MAX];    // queue estimate on arrival, ms

// The firmware's view of the speaker, sampled per pump tick: the one signal
// that could show the output stalling while our ring still has data.
static uint32_t s_notplaying;     // pump ticks with status != Playing
static uint32_t s_notplaying_at;  // sample id of the first one

// Firmware ring capacity in samples, which the SDK doesn't expose: measured at
// the prefill's first short write. An upper bound (start latency).
static uint32_t s_audio_ring_cap;

static uint32_t audio_now_ms(void) {
  time_t   s  = 0;
  uint16_t ms = 0;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000u + ms;
}

// Samples that should have left the speaker by now; 0 until playback starts.
static uint32_t audio_elapsed_samples(void) {
  if (!s_audio_clocked) return 0;
  return (audio_now_ms() - s_audio_t0_ms) * AUDIO_SAMPLES_PER_MS;
}

// Estimated samples queued. Reads low by the start latency, so only compare it
// with itself or with the ring capacity.
static int32_t audio_queued_samples(void) {
  return (int32_t)s_audio_samples_sent - (int32_t)audio_elapsed_samples();
}

#endif // PEBBLE_SYNC_DEBUG (state + clock helpers)

// Playback cursor (16 kHz samples) and the audio's own song_SongState.
static int            s_song_sample_id;
static song_SongState s_song_audio_state;

// Post-song EQ: SVF_STAGES TPT sections in series (svf.h).
static SvfChain s_song_svf;

// Latest filter-slider values, 0..65535: cut1,wid1,gain1, cut2,wid2,gain2,
// cut3,wid3,gain3 (three per SVF stage).
static uint16_t s_filt[9];

// Two-segment gain knob split at 32768: below, a pre-filter gain into the SVF
// chain; above, the chain is bypassed and it drives a plain output gain. Each
// half ramps silence to unity (16384).
static uint16_t s_pregain;

// DOWN toggles the whole post-song stage for an A/B; on by default.
static bool s_audio_eq_enabled = SONG_SPEAKER_FILTER_DEFAULT;

// The speaker-EQ reference settings come from song_config.h: they belong to
// this target, not the song.

// Loads the nine EQ knobs, the compare switch and the pregain.
// The defaults fit the whole three-stage cascade to three RBJ peaking biquads at
// 16 kHz (+16 dB @ 150 Hz/2 oct, +6.6 dB @ 656 Hz/3 oct, -27 dB @ 2857 Hz/2 oct):
// 0.58 dB RMS error over 20 Hz..7.9 kHz. The stages don't map one-to-one onto
// those bands (the low boost is two overlapping bells), so refit rather than
// nudge a single knob.
static void read_filter_sliders(void) {
  // The FILT_REF_* reference: the compare switch's "A" side.
  s_filt[0]=FILT_REF_0; s_filt[1]=FILT_REF_1; s_filt[2]=FILT_REF_2;
  s_filt[3]=FILT_REF_3; s_filt[4]=FILT_REF_4; s_filt[5]=FILT_REF_5;
  s_filt[6]=FILT_REF_6; s_filt[7]=FILT_REF_7; s_filt[8]=FILT_REF_8;
  s_pregain=FILT_REF_PREGAIN;
}

// One speaker; the resync and the EQ chain assume one sample per frame.
#if SONG_OUTPUT_CHANNELS != 1
#error "the pebble target is mono; the exporter must not emit a stereo song here"
#endif

static void audio_generate_chunk(void) {
  s_song_sample_id = song_render_audio_batch(s_song_sample_id, &s_song_audio_state, s_audio_chunk);

  // Each gain half ramps 0..16384 over the same span. Above 32768 the chain is
  // skipped so its integrators don't drift. EQ off is deliberately louder:
  // FILT_REF_PREGAIN leaves headroom for the boost.
  if (!s_audio_eq_enabled) {
    s_audio_samples_generated += AUDIO_SAMPLES_PER_CHUNK;
    return;
  }

  int      bypass_filters = (s_pregain >= 32768);
  uint16_t gain;
  if (bypass_filters) {
    gain = (uint16_t)(((uint32_t)(s_pregain - 32768) * 16384u) / (65535u - 32768u));
  } else {
    gain = (uint16_t)(((uint32_t)s_pregain * 16384u) / 32767u);
    svf_set_params(&s_song_svf.stage[0], s_filt[0], s_filt[1], s_filt[2]);
    svf_set_params(&s_song_svf.stage[1], s_filt[3], s_filt[4], s_filt[5]);
    svf_set_params(&s_song_svf.stage[2], s_filt[6], s_filt[7], s_filt[8]);
  }
  for (int i = 0; i < AUDIO_SAMPLES_PER_CHUNK; i++) {
    // 16384 = unity
    int32_t g = ((int32_t)s_audio_chunk[i] * gain) >> 14;
    if (g >  32767) g =  32767;
    if (g < -32768) g = -32768;
    s_audio_chunk[i] = bypass_filters ? (int16_t)g : svf_chain_process(&s_song_svf, (int16_t)g);
  }

  s_audio_samples_generated += AUDIO_SAMPLES_PER_CHUNK;
}

// Counts the song falling behind the wall clock and, only with
// AUDIO_RESYNC_ENABLE, skips forward to catch up. Off by default: the shortfall
// is sustained (the app can't render audio and heavy visuals at once), and
// skipping turns lateness into repeated audible gouges. Must run before anything
// refills the ring, or the shortfall is gone.
#ifndef AUDIO_RESYNC_ENABLE
#define AUDIO_RESYNC_ENABLE 0
#endif

#if PEBBLE_SYNC_DEBUG
static void audio_resync(void) {
  if (!s_audio_clocked) return;

  uint32_t elapsed = audio_elapsed_samples();
  if (s_audio_samples_sent + AUDIO_UNDERRUN_SLACK_SAMPLES >= elapsed) return;

  uint32_t deficit = elapsed - s_audio_samples_sent;

  // A time sync mid-song looks like a huge dropout: re-anchor t0 and leave the
  // song alone.
  if (deficit > AUDIO_RESYNC_MAX_SAMPLES) {
    s_audio_t0_ms = audio_now_ms() - s_audio_samples_sent / AUDIO_SAMPLES_PER_MS;
    return;
  }

  s_audio_dropouts++;
  s_audio_skipped += deficit;

#if AUDIO_RESYNC_ENABLE
  // Keep s_song_sample_id == s_audio_samples_generated; the half-consumed chunk
  // belongs to the skipped stretch.
  s_audio_samples_sent      = elapsed;
  s_audio_samples_generated = elapsed;
  s_song_sample_id          = (int)elapsed;
  s_audio_chunk_offset      = AUDIO_SAMPLES_PER_CHUNK;
#else
  // Measuring only: re-anchor so the same shortfall isn't reported again.
  s_audio_t0_ms = audio_now_ms() - s_audio_samples_sent / AUDIO_SAMPLES_PER_MS;
#endif

#if TEATIME_AUDIO_DIAG
  APP_LOG(APP_LOG_LEVEL_WARNING, "audio: behind %ums (#%u, %ums total)",
          (unsigned)(deficit / AUDIO_SAMPLES_PER_MS),
          (unsigned)s_audio_dropouts,
          (unsigned)(s_audio_skipped / AUDIO_SAMPLES_PER_MS));
#endif
}

// The calibration-free check: how long since the last fill, and how low the
// queue got. A gap longer than the ring's playtime is a dropout whatever the
// clock bias.
static void audio_observe(void) {
  if (!s_audio_clocked) return;

  int32_t q = audio_queued_samples();
  if (q < s_ring_min) s_ring_min = q;

  uint32_t now = audio_now_ms();
  uint32_t gap = now - s_last_fill_ms;
  s_last_fill_ms = now;
  if (gap > s_gap_max_ms) s_gap_max_ms = gap;

  if (gap >= AUDIO_GAP_LOG_MS) {
    if (s_gap_count < AUDIO_GAP_LOG_MAX) {
      int32_t q_ms = q / (int32_t)AUDIO_SAMPLES_PER_MS;
      if (q_ms >  32767) q_ms =  32767;
      if (q_ms < -32768) q_ms = -32768;
      s_gap_sample[s_gap_count] = s_audio_samples_sent;
      s_gap_ms[s_gap_count]     = (uint16_t)(gap > 65535u ? 65535u : gap);
      s_gap_left[s_gap_count]   = (int16_t)q_ms;
    }
    s_gap_count++;
  }
}

#else  // !PEBBLE_SYNC_DEBUG -- no clock, no counters, no syscalls

#define audio_observe()      ((void)0)
#define audio_resync()       ((void)0)

#endif // PEBBLE_SYNC_DEBUG (observe + resync)

// Every top-up goes through here, so observe/resync at the top sees a drained
// ring first.
static bool audio_fill_stream(void) {
  audio_observe();
  audio_resync();

  for (;;) {
    if (s_audio_chunk_offset < AUDIO_SAMPLES_PER_CHUNK) {
      uint32_t remaining = AUDIO_SAMPLES_PER_CHUNK - s_audio_chunk_offset;
      uint32_t written   = speaker_stream_write(
          (const uint8_t *)(s_audio_chunk + s_audio_chunk_offset),
          remaining * sizeof(int16_t));
      s_audio_chunk_offset += written / sizeof(int16_t);
      s_audio_samples_sent += written / sizeof(int16_t);
#if PEBBLE_SYNC_DEBUG
      // The first accepted write starts the wall clock.
      if (written && !s_audio_clocked) {
        s_audio_clocked = true;
        s_audio_t0_ms   = audio_now_ms();
        // Seed the gap clock, or the first observe reports the whole epoch.
        s_last_fill_ms  = s_audio_t0_ms;
      }
#endif
    }

    // Not fully consumed: the ring is full.
    if (s_audio_chunk_offset < AUDIO_SAMPLES_PER_CHUNK) {
#if PEBBLE_SYNC_DEBUG
      // The first saturation is the prefill reaching the ring's end.
      if (!s_audio_ring_cap) {
        int32_t q = audio_queued_samples();
        s_audio_ring_cap = q > 0 ? (uint32_t)q : 1;
      }
#endif
      return false;
    }

    // Chunk done; all generated means playback is complete.
    if (s_audio_samples_generated >= (uint32_t)song_total_samples()) {
      return true;
    }

    audio_generate_chunk();
    s_audio_chunk_offset = 0;
  }
}

#if PEBBLE_SYNC_DEBUG
// Dumped only once the speaker is done: logging mid-song could cause stalls.
// Always logs the summary, so all zeros is visible.
static void audio_log_dropouts(void) {
  // With resync off, "behind" is lateness, not lost audio.
  APP_LOG(APP_LOG_LEVEL_INFO, "audio: behind %ux, %ums total, %u samples",
          (unsigned)s_audio_dropouts,
          (unsigned)(s_audio_skipped / AUDIO_SAMPLES_PER_MS),
          (unsigned)s_audio_samples_sent);

  // ring and low in the same units: low near ring is comfortable, near 0
  // marginal, negative empty. maxgap near ring is a dropout.
  APP_LOG(APP_LOG_LEVEL_INFO, "audio: ring %ums low %dms maxgap %ums late %u stall %u@%u",
          (unsigned)(s_audio_ring_cap / AUDIO_SAMPLES_PER_MS),
          (int)(s_ring_min / (int32_t)AUDIO_SAMPLES_PER_MS),
          (unsigned)s_gap_max_ms,
          (unsigned)s_gap_count,
          (unsigned)s_notplaying,
          (unsigned)s_notplaying_at);

  uint32_t n = s_gap_count;
  if (n > AUDIO_GAP_LOG_MAX) n = AUDIO_GAP_LOG_MAX;  // rest went unrecorded
  for (uint32_t i = 0; i < n; i++) {
    APP_LOG(APP_LOG_LEVEL_INFO, "audio: gap %ums @%u (%us) left %dms",
            (unsigned)s_gap_ms[i],
            (unsigned)s_gap_sample[i],
            (unsigned)(s_gap_sample[i] / AUDIO_SAMPLE_RATE),
            (int)s_gap_left[i]);
  }
}
#else
#define audio_log_dropouts() ((void)0)
#endif // PEBBLE_SYNC_DEBUG (report)

static void audio_timer_callback(void *data) {
  (void)data;
  if (!s_audio_playing) return;

  // Re-arm first, so the period doesn't stretch by the callback's own run time.
  s_audio_timer = app_timer_register(AUDIO_TIMER_MS, audio_timer_callback, NULL);

  // Once per pump: these feed a filter, not an envelope.
  read_filter_sliders();

#if PEBBLE_SYNC_DEBUG
  // Once per pump: a syscall. Meaningful only after the first write.
  if (s_audio_clocked && speaker_get_status() != SpeakerStatusPlaying) {
    if (!s_notplaying) s_notplaying_at = s_audio_samples_sent;
    s_notplaying++;
  }
#endif

  if (audio_fill_stream()) {
    s_audio_playing = false;
    if (s_audio_timer) {
      app_timer_cancel(s_audio_timer);
      s_audio_timer = NULL;
    }
    speaker_stream_close();
#ifdef TEATIME_SYNTH_VISUALS
    song_visual_stop();
#endif
    audio_log_dropouts();
    return;
  }

#ifdef TEATIME_SYNTH_VISUALS
  // Repaint in step with playback; update_proc reads the play cursor.
  song_visual_tick();
#endif
}

static void audio_start(void) {
  if (s_audio_playing) return;

  if (!speaker_stream_open(SpeakerPcmFormat_16kHz_16bit, AUDIO_VOLUME)) {
    return;
  }

  s_audio_samples_sent      = 0;
  s_audio_samples_generated = 0;
  s_audio_chunk_offset      = AUDIO_SAMPLES_PER_CHUNK; // force fresh chunk on first fill
  s_song_sample_id          = 0;
#if PEBBLE_SYNC_DEBUG
  s_audio_clocked           = false;  // t0 is set by the first accepted write
  s_audio_t0_ms             = 0;
  s_audio_dropouts          = 0;
  s_audio_skipped           = 0;
  s_audio_ring_cap          = 0;  // re-measured by the prefill below
  s_last_fill_ms            = 0;
  s_gap_max_ms              = 0;
  s_gap_count               = 0;
  s_ring_min                = 0x7fffffff;
  s_notplaying              = 0;
  s_notplaying_at           = 0;
#endif
  memset(&s_song_audio_state, 0, sizeof(s_song_audio_state));  // no notes held, latch tick 0 first
  svf_chain_reset(&s_song_svf);    // clear both stages' integrators
  s_audio_playing           = true;

  // Seed the knobs before the prefill, or a zero pregain renders it silent.
  read_filter_sliders();

#ifdef TEATIME_SYNTH_VISUALS
  song_visual_start();
#endif

  if (audio_fill_stream()) {
    s_audio_playing = false;
    speaker_stream_close();
#ifdef TEATIME_SYNTH_VISUALS
    song_visual_stop();
#endif
    return;
  }

  s_audio_timer = app_timer_register(AUDIO_TIMER_MS, audio_timer_callback, NULL);
}

static void audio_stop(void) {
  if (s_audio_playing) {
    s_audio_playing = false;
    if (s_audio_timer) {
      app_timer_cancel(s_audio_timer);
      s_audio_timer = NULL;
    }
    speaker_stop();
#ifdef TEATIME_SYNTH_VISUALS
    song_visual_stop();
#endif
  }

  // Unconditional, so DOWN re-dumps the stats when no log viewer was attached at
  // the song's end.
  audio_log_dropouts();
}

// The glyph infrastructure below serves both the POLY_TEST star demo and the
// song visuals.
#if defined(POLY_TEST) || defined(TEATIME_SYNTH_VISUALS)

// The demo uses render_lowspec's compiled fill/stroke rather than a second
// static copy of render_polygon.h/render_aaline.h.

// Digit glyph data; the Pebble-sized renderer is font_lowspec_pebble.c's own TU.
#include "font_digits_lowspec.h"

// Sidestack for render_glyph_lowspec, malloc'd once in init(). Heap, not
// static: a static array can land misaligned on hardware and blank every digit.
// Pebble's heap is only 4-aligned; scratch_bump rounds to 8, which the slack
// absorbs.
// Sized for font_lowspec_pebble.c's GLYPH_MAX_SEGS(10)/GLYPH_SMOOTH_MAX(42)/
// RGC_FLAT_MAX(124)/RGC_ARC_MARGIN(1); keep in sync. Edge-loop peak:
//   GrPath (GrEdge*243 + 12)                                  = 4872 B
//   contour (RGC_OUTLINE_MAX = 2*(124+10) + 16 = 284 verts)   = 2272 B
//   smooth_pts/ctrl/degraded/orig_idx (survive build_smoothed) = 840 B
//   + larger of build_smoothed temps (520) or
//     flat*/sd*/sn*/right_out (992+992+2480+1072)             = 5536 B
//   = 13520 B, which also covers rasterize (~1784 B).
// Plus PEBBLE_GLYPH_CACHE_TILE_BYTES: a cache tile is carved off the front, so
// without it the render would run short and the cache would memorize a
// truncated glyph. Must equal LOWSPEC_GLYPH_CACHE_MAX_TILE_BYTES.
#define PEBBLE_GLYPH_CACHE_TILE_BYTES 2048
// Plus PEBBLE_VSCREEN_IMAGE_BYTES for image_alloc(), also carved off the front.
// A fixed reservation: the renderer always starts past it, so its budget is
// unaffected. 512 B is one 25x20 image. Heap, so no .bss cost; keep it a
// multiple of 8.
#define PEBBLE_VSCREEN_IMAGE_BYTES 512

// Plus PEBBLE_POLY_BUCKET_BYTES for POLY_BUCKET_SORT's (height + 2) int table.
// Optional: allocated last, and a null table falls back to insertion sort.
// 928 B covers height 230.
#define PEBBLE_POLY_BUCKET_BYTES 928
#define PEBBLE_GLYPH_SCRATCH_BYTES (13520 + PEBBLE_GLYPH_CACHE_TILE_BYTES \
                                    + PEBBLE_VSCREEN_IMAGE_BYTES \
                                    + PEBBLE_POLY_BUCKET_BYTES)
static char *s_glyph_scratch;

// ---- Star / digit-ladder demo proper (POLY_TEST only) --------------------

// Song visuals: song.c's *_visual bodies draw through vscreen_*
// (song/vscreen.h), backed here by the glyph infrastructure above and the
// framebuffer captured per render. Without TEATIME_SYNTH_VISUALS the whole path
// and the visual bodies drop out.
#ifdef TEATIME_SYNTH_VISUALS

#include "song/vscreen.h"
// Compiled into this app by src/c/render_ctx_pebble.c.
#include "font/render_ctx.h"

// Idealist Hacker Mono for font() ids 0/1. WARNING: the scratch sizes above
// were measured against Assembly Line's glyphs only; re-measure in pebblesim
// before shipping Idealist Hacker Mono text on hardware, or glyphs may silently
// truncate.
#include "font_idealist_hacker_mono_lowspec.h"

// Provisioning for font/render_ctx.c, which holds all behaviour. The screen is
// bound only during one song_visual_render pass; unbound, every draw drops.
// Images come off the front of s_glyph_scratch at a fixed split:
//   s_glyph_scratch [ images: PEBBLE_VSCREEN_IMAGE_BYTES ][ renderer: 13520 + tile ]
//                     ^ ctx.image_arena                     ^ ctx.scratch
// image_alloc stops at the boundary (returns 0).
#define PEBBLE_VSCREEN_IMAGE_MAX_DIM 1024   // guards the w*h multiply; the split above is the real ceiling
#define PEBBLE_VSCREEN_MAX_IMAGES        2
#define PEBBLE_VSCREEN_TARGET_STACK_MAX  2

static RenderImage s_vscreen_images[PEBBLE_VSCREEN_MAX_IMAGES];
static RSurface    s_vscreen_target_stack[PEBBLE_VSCREEN_TARGET_STACK_MAX];
static int         s_vscreen_target_flags[PEBBLE_VSCREEN_TARGET_STACK_MAX];

static const FONT_LOWSPEC *vscreen_font_resolve(void *user, int id);

// Zero-initialised, not link-time initialised: a non-zero initialiser would move
// it from .bss into .data and count twice against the 64 KB cap. Until
// vscreen_ctx_init() every draw and image_alloc is a no-op.
static RenderCtx s_vscreen_ctx;

// `screen`/`palette`, borrowed for one render pass. vscreen_palette is 1 KB
// nothing reads, kept so a body is the same source on every target.
unsigned char *vscreen_screen = NULL;
int            vscreen_screen_len = 0;
int            vscreen_palette[256];

// Called from init() after s_glyph_scratch; a null scratch leaves both ranges empty.
static void vscreen_ctx_init(void) {
  s_vscreen_ctx.images      = s_vscreen_images;
  s_vscreen_ctx.image_cap   = PEBBLE_VSCREEN_MAX_IMAGES;
  s_vscreen_ctx.image_max_w = PEBBLE_VSCREEN_IMAGE_MAX_DIM;
  s_vscreen_ctx.image_max_h = PEBBLE_VSCREEN_IMAGE_MAX_DIM;

  s_vscreen_ctx.target_stack = s_vscreen_target_stack;
  s_vscreen_ctx.target_flags = s_vscreen_target_flags;
  s_vscreen_ctx.target_cap   = PEBBLE_VSCREEN_TARGET_STACK_MAX;

  s_vscreen_ctx.font_resolve = vscreen_font_resolve;

  if (!s_glyph_scratch) return;
  s_vscreen_ctx.image_arena     = s_glyph_scratch;
  s_vscreen_ctx.image_arena_end = s_glyph_scratch + PEBBLE_VSCREEN_IMAGE_BYTES;
  s_vscreen_ctx.image_cursor    = s_glyph_scratch;
  s_vscreen_ctx.scratch         = s_glyph_scratch + PEBBLE_VSCREEN_IMAGE_BYTES;
  s_vscreen_ctx.scratch_end     = s_glyph_scratch + PEBBLE_GLYPH_SCRATCH_BYTES;
}

// font() id -> FONT_LOWSPEC. Idealist Hacker Mono (0, 1) draws through a cached
// twin, lossless under RS_PEBBLE_TIME2; Assembly Line (2) is uncached. Heap,
// built on first use, ~12 KB.
static const FONT_LOWSPEC *vscreen_cached_font(const FONT_LOWSPEC *src) {
  static const FONT_LOWSPEC *built;
  if (!built) {
    int need = render_ctx_font_cache_bytes();
    void *mem = s_sys.malloc((size_t)need);
    if (!mem) APP_LOG(APP_LOG_LEVEL_WARNING, "glyph cache alloc failed (%d B)", need);
    // No free: the twin outlives every draw. On a failed malloc, the plain font.
    built = render_ctx_build_font_cache(src, mem, need);
  }
  return built;
}

// Assembly Line (id 2) is only reachable from a hot-reloaded body, so release
// builds drop the arm and --gc-sections removes the font (5424 bytes).
static const FONT_LOWSPEC *vscreen_font_resolve(void *user, int id) {
  (void)user;
  switch (id) {
    case 0:
    case 1:  return vscreen_cached_font(&font_idealist_hacker_mono_lowspec_font);
    default: return NULL;   // unrecognized id: leave the current font selected
  }
}

// vscreen_* API for song.c's visual bodies, bound to the context above. Fix
// behaviour in font/render_ctx.c, not here.
// In-render audio pump: the repaint owns the event loop, so no AppTimer fires
// during it. With VSCREEN_AUDIO_PUMP_INTERVAL > 0 the shape primitives top up
// the speaker every that many draws. Safe mid-render: audio and visual state are
// separate. Off (0) by default: measured maxgap stayed far below the ring depth,
// so the shortfall is throughput, not scheduling, and each pump is two
// syscalls. Enable only if maxgap approaches the ring depth.
#ifndef VSCREEN_AUDIO_PUMP_INTERVAL
#define VSCREEN_AUDIO_PUMP_INTERVAL 0
#endif

#if defined(TEATIME_AUDIO) && VSCREEN_AUDIO_PUMP_INTERVAL > 0
static void vscreen_pump_audio(void) {
  static unsigned char n;
  if (!s_audio_playing) return;
  if (++n < VSCREEN_AUDIO_PUMP_INTERVAL) return;
  n = 0;
  (void)audio_fill_stream();
}
#else
#define vscreen_pump_audio() ((void)0)
#endif

int vscreen_putpixel_i32(int x, int y, int c) {
  return render_ctx_putpixel_i32(&s_vscreen_ctx, x, y, c);
}

int vscreen_point_fx16(int x, int y, int amount, int blend) {
  return render_ctx_point_fx16(&s_vscreen_ctx, x, y, amount, blend);
}

int vscreen_line_fx16(int x1, int y1, int x2, int y2, int stroke_width, int alpha, int blend) {
  vscreen_pump_audio();
  return render_ctx_line_fx16(&s_vscreen_ctx, x1, y1, x2, y2, stroke_width, alpha, blend);
}

int vscreen_ellipse_fx16(int x, int y, int radius_w, int radius_h, int alpha, int blend) {
  vscreen_pump_audio();
  return render_ctx_ellipse_fx16(&s_vscreen_ctx, x, y, radius_w, radius_h, alpha, blend);
}

int vscreen_circle_fx16(int x, int y, int radius, int alpha, int blend) {
  vscreen_pump_audio();
  return render_ctx_circle_fx16(&s_vscreen_ctx, x, y, radius, alpha, blend);
}

int vscreen_rect_fx16(int x, int y, int w, int h, int col, int alpha, int blend) {
  vscreen_pump_audio();
  return render_ctx_rect_fx16(&s_vscreen_ctx, x, y, w, h, col, alpha, blend);
}

int vscreen_background_fx16(int col, int transparency) {
  vscreen_pump_audio();
  return render_ctx_background_fx16(&s_vscreen_ctx, col, transparency);
}

int vscreen_glyph_fx16(int x, int y, int size, int stroke_width, int ascii, int alpha, int blend) {
  vscreen_pump_audio();
  return render_ctx_glyph_fx16(&s_vscreen_ctx, x, y, size, stroke_width, ascii, alpha, blend);
}

void vscreen_text(int x, int y, int size, int stroke_width,
                  const unsigned char *str, int len, int alpha, int blend,
                  int letter_spacing, int line_height) {
  vscreen_pump_audio();
  render_ctx_text(&s_vscreen_ctx, x, y, size, stroke_width, str, len, alpha, blend,
                  letter_spacing, line_height);
}

int vscreen_font_i32(int id)       { return render_ctx_font_i32(&s_vscreen_ctx, id); }
int vscreen_text_align_i32(int id) { return render_ctx_text_align_i32(&s_vscreen_ctx, id); }

int vscreen_image_alloc_i32(int w, int h) {
  return render_ctx_image_alloc_i32(&s_vscreen_ctx, w, h);
}

int vscreen_image_getpixel_i32(int image_id, int x, int y) {
  return render_ctx_image_getpixel_i32(&s_vscreen_ctx, image_id, x, y);
}

int vscreen_image_sample_fx16(int image_id, int x, int y) {
  // Pumped here too: on a dark frame this per-cell sampling loop is what runs.
  // Per cell, not per pixel; each pump is a syscall.
  vscreen_pump_audio();
  return render_ctx_image_sample_fx16(&s_vscreen_ctx, image_id, x, y);
}

int vscreen_push_target_i32(int image_id) {
  return render_ctx_push_target_i32(&s_vscreen_ctx, image_id);
}

int vscreen_pop_target_i32(int unused) {
  return render_ctx_pop_target_i32(&s_vscreen_ctx, unused);
}

void vscreen_images_reset(void) { render_ctx_images_reset(&s_vscreen_ctx); }

// Samples the visuals lag the audio: the firmware ring and codec sit between
// the write and the speaker. 1024 @ 16 kHz = 64 ms; 0 disables.
#ifndef TEATIME_DELAY_VISUALS_SAMPLES
#define TEATIME_DELAY_VISUALS_SAMPLES 4096
#endif

static Layer         *s_song_visual_layer;
static bool           s_song_visual_active;
static song_SongState s_song_visual_state;      // own sequencer state (UI thread)

// -DTEATIME_RENDER_PROFILE=1 logs `render: 32 frames in NNNN ms` every 32nd
// frame. Wall clock, so it includes the audio timer.
#ifndef TEATIME_RENDER_PROFILE
#define TEATIME_RENDER_PROFILE 0
#endif

#if TEATIME_RENDER_PROFILE
#define RENDER_PROFILE_WINDOW 32
static uint32_t render_profile_now_ms(void) {
  time_t s = 0; uint16_t ms = 0;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000u + ms;
}
#endif

static void song_visual_layer_update_proc(Layer *layer, GContext *ctx) {
  (void)layer;
  if (!s_song_visual_active) return;
#if TEATIME_RENDER_PROFILE
  static uint32_t prof_start_ms;
  static int      prof_frames;
  if (prof_frames == 0) prof_start_ms = render_profile_now_ms();
#endif

  // Bracket the render with fills: no timer fires inside it.
  if (s_audio_playing) (void)audio_fill_stream();

  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect fb_bounds = gbitmap_get_bounds(fb);

  RSurface surf;
  surf.pixels = gbitmap_get_data(fb);
  surf.stride = gbitmap_get_bytes_per_row(fb);
  surf.clip_x = 0;
  surf.clip_y = 0;
  surf.clip_w = fb_bounds.size.w;
  surf.clip_h = fb_bounds.size.h;
  surf.layout = 0;
  render_ctx_bind_screen(&s_vscreen_ctx, &surf);
  // Scripts index `screen[x + y*width]`, assuming packed rows; a padded bitmap
  // shears the picture (still in bounds) and is logged once.
  vscreen_screen     = surf.pixels;
  vscreen_screen_len = fb_bounds.size.w * fb_bounds.size.h;
  if (surf.stride != fb_bounds.size.w) {
    static int warned;
    if (!warned) {
      warned = 1;
      APP_LOG(APP_LOG_LEVEL_WARNING,
              "framebuffer stride %d != width %d: screen[] writes will shear",
              (int)surf.stride, (int)fb_bounds.size.w);
    }
  }

  // Driven off the samples handed to the speaker, minus output latency, clamped at 0.
  int visual_sample_id = (int)s_audio_samples_sent - TEATIME_DELAY_VISUALS_SAMPLES;
  if (visual_sample_id < 0) visual_sample_id = 0;
  song_visual_render(visual_sample_id, &s_song_visual_state);

  // Unbind both before the framebuffer is released.
  render_ctx_bind_screen(&s_vscreen_ctx, NULL);
  vscreen_screen     = NULL;
  vscreen_screen_len = 0;
  graphics_release_frame_buffer(ctx, fb);

  if (s_audio_playing) (void)audio_fill_stream();

#if TEATIME_RENDER_PROFILE
  if (++prof_frames >= RENDER_PROFILE_WINDOW) {
    uint32_t elapsed = render_profile_now_ms() - prof_start_ms;
    APP_LOG(APP_LOG_LEVEL_INFO, "render: %d frames in %u ms (%u.%u ms/frame)",
            prof_frames, (unsigned)elapsed,
            (unsigned)(elapsed / prof_frames),
            (unsigned)((elapsed * 10u / prof_frames) % 10u));
    prof_frames = 0;
  }
#endif
}

// Top-most layer; draws nothing while inactive.
static void song_visual_attach(Layer *root) {
  s_song_visual_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_song_visual_layer, song_visual_layer_update_proc);
  layer_add_child(root, s_song_visual_layer);
  s_song_visual_active    = false;
}

static void song_visual_detach(void) {
  if (s_song_visual_layer) {
    layer_destroy(s_song_visual_layer);
    s_song_visual_layer = NULL;
  }
  s_song_visual_active = false;
}

static void song_visual_start(void) {
  memset(&s_song_visual_state, 0, sizeof(s_song_visual_state));  // latch tick 0 on the first render
  s_song_visual_active    = true;
  if (s_song_visual_layer) layer_mark_dirty(s_song_visual_layer);
}

// Repaint every Nth audio tick: every tick (~100 fps) saturates the UI thread.
// The rate doesn't affect the audio glitches; 2 is ~50 fps.
#ifndef SONG_VISUAL_TICK_DIVISOR
#define SONG_VISUAL_TICK_DIVISOR 2
#endif
static void song_visual_tick(void) {
  static int accum;
  if (!(s_song_visual_active && s_song_visual_layer)) return;
  if (++accum < SONG_VISUAL_TICK_DIVISOR) return;
  accum = 0;
  layer_mark_dirty(s_song_visual_layer);
}

// No "buffer low -> skip frame" guard here: audio_queued_samples() reads low by
// the start latency, so any watermark near the ring level latches and freezes
// the display for good.

static void song_visual_stop(void) {
  s_song_visual_active = false;
  // Repaint so the song overlay clears back to the layers underneath.
  if (s_song_visual_layer) layer_mark_dirty(s_song_visual_layer);
}

#endif // TEATIME_SYNTH_VISUALS

#endif // POLY_TEST || TEATIME_SYNTH_VISUALS

// Release boots straight into the song: one deferred kick starts audio, which
// drives the visuals.
static void release_start_tick(void *data) {
  (void)data;
  audio_start();
}

// SELECT toggles the backlight; the SDK exposes only on/off.
static bool s_light_on = false;

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  s_light_on = !s_light_on;
  light_enable(s_light_on);
}

static void audio_up_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  audio_start();
}

// DOWN: in release, toggles the post-song EQ + gain stage; in debug, stops
// audio and steps the star demo.
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer; (void)context;
  s_audio_eq_enabled = !s_audio_eq_enabled;
  // Start from rest, or re-entering the chain thumps.
  if (s_audio_eq_enabled) svf_chain_reset(&s_song_svf);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  // UP replays the song.
  window_single_click_subscribe(BUTTON_ID_UP, audio_up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

// ---- Window ----
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);

#ifdef TEATIME_SYNTH_VISUALS
  // Added last so the song visuals composite on top.
  song_visual_attach(root);
#endif
}

static void window_unload(Window *window) {
#ifdef TEATIME_SYNTH_VISUALS
  song_visual_detach();
#endif
}

// ---- App lifecycle ----
static void init(void) {
  s_sys = pebble_tsys_init();
  s_sys.print = tsys_print_and_send;

#if defined(POLY_TEST) || defined(TEATIME_SYNTH_VISUALS)
  // Allocated once and never freed; shared by the star demo and the song visuals,
  // which never draw concurrently.
  s_glyph_scratch = (char*)s_sys.malloc(PEBBLE_GLYPH_SCRATCH_BYTES);
#endif

#ifdef TEATIME_SYNTH_VISUALS
  // After the malloc: both halves are offsets into it.
  vscreen_ctx_init();
#endif

  s_window = window_create();
  window_set_background_color(s_window, GColorClear);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load   = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  // Boot straight into the song + visuals a beat after the window loads.
  app_timer_register(300, release_start_tick, NULL);
}

static void deinit(void) {
  audio_stop();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
