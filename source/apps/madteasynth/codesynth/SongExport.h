// Turns a recorded event matrix plus its sounds into a standalone C renderer
// (song.c + song.h). Offline: data in, text out.
#pragma once

#include <string>
#include <vector>

#include "CodeSynthParse.h"
#include "SongSeq.h"

// Shape of song_render_audio_batch()'s inner loop. All render bit-identical audio.
enum SongSndLoopShape
{
  // One batch loop with the event switch inside: dispatch runs per output sample.
  // Slowest.
  kSongSndLoopShared = 0,

  // The switch hoisted out of the batch loop, one loop per case. Fastest and
  // largest, O(events).
  kSongSndLoopInlined = 1,

  // One batch loop per instrument; the per-(sound, note, velocity) scalars come
  // from a const table. O(instruments), which fits a Pebble's 64 KB image.
  kSongSndLoopGrouped = 2,

  // Shared below kSongSndLoopAutoSharedBelow distinct events, else grouped.
  kSongSndLoopAuto = 3,

  // Channel loop inside the batch loop, no accumulator array. Sounds run
  // sample-major, which only matters if bodies share mutable globals.
  kSongSndLoopPerSample = 4,
};

static const int kSongSndLoopAutoSharedBelow = 8;

// Shape of song_visual_render()'s switch. Measure before switching: grouping is
// not a size win by default (emery, RELEASE+LTO: per-event 56,368 bytes, grouped
// 56,600). A visual case is one call with immediates, so the table saves little
// while the ev -> (instrument, row) arrays cost 2 bytes per event row.
enum SongVisSwitchShape
{
  // One case per event; the default.
  kSongVisSwitchPerEvent = 0,

  // One case per visual instrument, with the per-(note, velocity) scalars and any
  // smp() sample in a const table.
  kSongVisSwitchGrouped = 1,
};

// Output channels of song_render_audio_batch(). A body reading `pan` renders
// once per channel; on mono it renders once at pan = 0.
enum SongChannelMode
{
  // Always 1: Pebble and Wav mono.
  kSongChannelsMono = 0,

  // 2 if any sound reads `pan`, else 1: win32 and microw8.
  kSongChannelsAuto,

  // Always 2: Wav stereo, where the channel count is the requested format.
  kSongChannelsStereo,
};

struct SongExportInput
{
  std::vector<CodeSynthEntry> entries;
  SongSeq                     seq;

  double sampleRate = 44100.0; // rate the song was recorded/authored at
  double bpm        = 120.0;   // tempo the matrix was quantized at

  // Resolves `// WAV` directives; must end in a separator.
  std::string workingDir;

  // What song.c #includes for the vscreen_* API. The default is repo-relative;
  // project exporters pass their layout's path.
  std::string vscreenInclude = "apps/madteasynth/vscreen.h";

  // The context every body compiles against, which fixes the folded
  // `width`/`height`/`stride`. The caller's own, never a global; non-null when
  // there are visual bodies.
  RenderCtx* screen = nullptr;

  // SteepSynthEngine::kBlockSize for the plugin.
  int batchSize = 32;

  // Grouped by default: Pebble can't fit O(events).
  SongSndLoopShape sndLoopShape = kSongSndLoopGrouped;

  // Per-event by default; see the measurement above.
  SongVisSwitchShape visSwitchShape = kSongVisSwitchPerEvent;

  // Emit `case N: break;` for events a switch renders nothing for, with a comment
  // saying why. Behaviour is identical; only size differs.
  bool addEmptyCases = false;

  // Mono by default.
  SongChannelMode channelMode = kSongChannelsMono;
};

// Generates song.c. A sound that fails to compile is emitted as a silent stub.
// `*outChannels` (1 or 2) must be passed to SongExportGenerateH:
// SONG_OUTPUT_CHANNELS sizes a buffer across translation units.
bool SongExportGenerateC(const SongExportInput& in, std::string& out, std::string& err,
                         int* outChannels);

// Generates song.h: format constants, channel counts, song_SongState and the
// entry points. song_SongState is sized by SONG_CHANNEL_COUNT, which changes
// with the song, so no TU may declare its own copy. `outputChannels` must come
// from SongExportGenerateC for the same input; deliberately no default.
bool SongExportGenerateH(const SongExportInput& in, std::string& out, std::string& err,
                         int outputChannels);

// Writes `path` and its "<stem>.h" companion.
bool SongExportWriteFile(const SongExportInput& in, const std::string& path, std::string& err);
