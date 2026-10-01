// Turns a recorded song plus a target into a standalone project directory: the
// song (SongExport.h), the renderer template (ExportTemplate.h), and the build
// files and shims written here.
#pragma once

#include <string>

#include "ExportTemplate.h"
#include "SongExport.h"

// Persisted in the plugin's state: append, never reorder.
struct ExportSettings
{
  // Created if missing; an existing project is overwritten in place.
  std::string destDir;

  unsigned target = kExportTargetPebble;

  // Empty uses the destination folder's name.
  std::string name;

  // Empty falls back to "codesynth".
  std::string author;

  // win32 only; emery and microw8 have fixed screens.
  int width  = 200;
  int height = 228;

  // win32 only; emery is pinned to 16 kHz, microw8 to 44.1 kHz. Everything
  // rate-dependent in song.c is over SONG_SAMPLE_RATE, so 16000 previews the watch.
  int sampleRate = 44100;

  // pebble only: the initial state of the speaker EQ toggle.
  bool speakerFilterDefault = true;

  // Nudge the tempo to a whole number of buffers per 1/16, since steps can only
  // start on buffer boundaries. Off by default: it changes the speed.
  bool alignBpmToBuffers = false;

  // pebble only: package.json's uuid. The same id replaces the installed app, a
  // new one adds a second. Empty reuses the destination's or mints one; a
  // successful export writes the used id back. Must be 8-4-4-4-12 when set.
  std::string uuid;

  // WAV targets only: delete the source this export wrote once a .wav exists. A
  // failed build keeps everything; nothing the export didn't write is touched.
  bool wavRemoveSource = false;

  // Audio is identical in every shape; only size and speed differ.
  unsigned sndLoopShape = kSongSndLoopAuto;
};

struct ExportProjectResult
{
  int         filesWritten = 0;
  std::string projectDir;      // absolute, as written
  std::string uuid;            // the app id used (see ExportSettings::uuid)
  // set: the page asked for it; reused: read from an existing package.json;
  // neither: minted.
  bool        setUuid    = false;
  bool        reusedUuid = false;
  long        songBytes  = 0;  // generated song.c, a rough size proxy

  // Under kSongChannelsAuto this depends on whether a sound reads `pan`.
  int         outputChannels = 1;

  // WAV targets: the rendered file, or empty if none (not an export failure).
  std::string wavPath;
  std::string wavError;
};

// The target's actual screen; only win32 reads width/height. The plugin sizes
// its preview from this too.
void ExportTargetScreenSize(const ExportSettings& s, int& w, int& h);

// The target's actual rate; only win32 reads sampleRate.
int ExportTargetSampleRate(const ExportSettings& s);

// Pebble and Wav mono: 1. win32 and microw8: 2 if a sound reads `pan`. Wav stereo: 2.
SongChannelMode ExportTargetChannelMode(const ExportSettings& s);

bool ExportTargetIsWav(unsigned target);

// Converted exactly as SongExport does, so the page and the exporter agree.
int    ExportSongTickFromBpm(double bpm, double sampleRate);
double ExportBpmFromSongTick(int tickSamples, double sampleRate);

// What "align to buffers" works out to, for the page to show before exporting.
struct ExportBpmAlign
{
  double requestedBpm = 0.0;   // tempo the song is on now
  double bpm          = 0.0;   // tempo the export will run at
  int    buffers      = 0;     // whole audio buffers per subdivision
  int    samples      = 0;     // samples per subdivision, at the TARGET rate
  double ms           = 0.0;   // ...as a duration
  bool   changed      = false; // false when the tempo is left alone
  int    songTick     = 0;     // grid to store, in samples at the AUTHORED rate
};

// The nearest tempo whose subdivision is a whole number of buffers. `div` 16 =
// 1/16 notes.
ExportBpmAlign ExportAlignBpm(double requestedBpm, int sampleRate, int bufferSize,
                              int div = 16);

// What this export will do to the tempo. The grid is authored at one rate but
// aligned at the target's, and song.c converts with
//   SONG_SAMPLES_PER_SONGTICK = (songTick * SONG_SAMPLE_RATE + rate/2) / rate
// so the plan needs a songTick that comes out of that as whole buffers. When
// none is near (target rate well above the authored one), `changed` is false.
ExportBpmAlign ExportPlanBpm(const ExportSettings& s, int authoredTick,
                             double authoredRate, int bufferSize);

// A fresh v4 UUID in package.json's 8-4-4-4-12 form.
std::string ExportMakeUuid();
bool        ExportUuidLooksValid(const std::string& uuid);

// Re-exporting keeps an existing project's uuid unless settings.uuid names one.
// A destination package.json that isn't a Pebble project is refused. Tempo
// alignment applies to a copy of `song`.
bool ExportProjectWrite(const ExportSettings& settings, const SongExportInput& song,
                        ExportProjectResult& result, std::string& err);
