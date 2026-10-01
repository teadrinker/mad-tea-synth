// One section per target, dispatched at the bottom.

#include "ExportProject.h"

extern "C" {
// Bodies compile against the screen this export is for.
#include "vscreen.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include "Utf8File.h"
#include <random>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

// Pebble and microw8 screens are fixed. A wav render draws nothing but still
// folds `width`/`height`, so it follows the page like win32.
void ExportTargetScreenSize(const ExportSettings& s, int& w, int& h)
{
  switch (s.target)
  {
    case kExportTargetMicrow8: w = 320; h = 240; break;
    case kExportTargetWin32:
    case kExportTargetWavStereo:
    case kExportTargetWavMono:
      w = s.width  > 0 ? s.width  : 200;
      h = s.height > 0 ? s.height : 228;
      break;
    case kExportTargetPebble:
    default:                   w = 200; h = 228; break;
  }
}

// Each target's song_config.h #defines SONG_SAMPLE_RATE from this.
int ExportTargetSampleRate(const ExportSettings& s)
{
  switch (s.target)
  {
    // A .wav can carry any rate; 16000 previews the watch.
    case kExportTargetWin32:
    case kExportTargetWavStereo:
    case kExportTargetWavMono: return s.sampleRate > 0 ? s.sampleRate : 44100;
    case kExportTargetMicrow8: return 44100;  // the cart ABI calls snd() at this
    case kExportTargetPebble:
    default:                   return 16000;  // the watch speaker
  }
}

SongChannelMode ExportTargetChannelMode(const ExportSettings& s)
{
  switch (s.target)
  {
    // The format the user picked, even when nothing pans.
    case kExportTargetWavStereo: return kSongChannelsStereo;

    // 2 only if some sound reads `pan`.
    case kExportTargetWin32:
    case kExportTargetMicrow8:   return kSongChannelsAuto;

    // One speaker, and no room in 64 KB for a second accumulator.
    case kExportTargetWavMono:
    case kExportTargetPebble:
    default:                     return kSongChannelsMono;
  }
}

bool ExportTargetIsWav(unsigned target)
{
  return target == kExportTargetWavStereo || target == kExportTargetWavMono;
}

// random_device, not a time seed: two exports in one second must not collide.
std::string ExportMakeUuid()
{
  std::random_device rd;
  std::uniform_int_distribution<unsigned> hex(0, 15);
  static const char* kHex = "0123456789abcdef";

  std::string s;
  for (int i = 0; i < 36; ++i)
  {
    if (i == 8 || i == 13 || i == 18 || i == 23) { s += '-'; continue; }
    if (i == 14) { s += '4'; continue; }                    // version 4
    if (i == 19) { s += kHex[8 + (hex(rd) & 3)]; continue; } // variant 10xx
    s += kHex[hex(rd)];
  }
  return s;
}

// Shape only (8-4-4-4-12 lowercase hex): a pasted id need not be v4.
bool ExportUuidLooksValid(const std::string& uuid)
{
  static const int kGroup[5] = { 8, 4, 4, 4, 12 };
  size_t at = 0;
  for (int g = 0; g < 5; ++g)
  {
    if (g > 0)
    {
      if (at >= uuid.size() || uuid[at] != '-') return false;
      ++at;
    }
    for (int i = 0; i < kGroup[g]; ++i, ++at)
    {
      if (at >= uuid.size()) return false;
      const char c = uuid[at];
      const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex) return false;
    }
  }
  return at == uuid.size();
}

// Rounded exactly as SongExportGenerateC does, so the page states the tempo the
// export produces.
int ExportSongTickFromBpm(double bpm, double sampleRate)
{
  const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
  const double b  = bpm > 0.0 ? bpm : 120.0;
  const int tick = (int)(sr * 60.0 / b / 4.0 + 0.5);
  return tick < 1 ? 1 : tick;
}

double ExportBpmFromSongTick(int tickSamples, double sampleRate)
{
  const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
  if (tickSamples < 1) tickSamples = 1;
  return sr * 60.0 / 4.0 / (double)tickSamples;
}

// n * bpm = (240 / div) * rate / bufferSize: round n and read the tempo back.
ExportBpmAlign ExportAlignBpm(double requestedBpm, int sampleRate, int bufferSize, int div)
{
  ExportBpmAlign a;
  a.requestedBpm = requestedBpm;
  a.bpm          = requestedBpm;
  if (requestedBpm <= 0.0 || sampleRate <= 0 || bufferSize <= 0 || div <= 0) return a;

  const double k = (240.0 / (double)div) * (double)sampleRate / (double)bufferSize;
  int n = (int)std::floor(k / requestedBpm + 0.5);
  if (n < 1) n = 1;

  a.buffers = n;
  a.samples = n * bufferSize;
  a.bpm     = k / (double)n;
  a.ms      = (double)a.samples * 1000.0 / (double)sampleRate;
  // Compare the sample count, not two doubles.
  a.changed = (ExportSongTickFromBpm(requestedBpm, (double)sampleRate) != a.samples);
  return a;
}

ExportBpmAlign ExportPlanBpm(const ExportSettings& s, int authoredTick,
                             double authoredRate, int bufferSize)
{
  const int srAuthored = (int)((authoredRate > 0.0 ? authoredRate : 44100.0) + 0.5);
  const int srTarget   = ExportTargetSampleRate(s);
  const int batch      = bufferSize > 0 ? bufferSize : 32;
  if (authoredTick < 1) authoredTick = 1;

  // song.c's own rescale, as emitted.
  auto emitted = [&](long long tick) {
    return (tick * (long long)srTarget + (long long)srAuthored / 2) / (long long)srAuthored;
  };

  ExportBpmAlign plan;
  plan.requestedBpm = ExportBpmFromSongTick(authoredTick, (double)srAuthored);
  plan.bpm          = plan.requestedBpm;
  plan.songTick     = authoredTick;
  plan.samples      = (int)emitted(authoredTick);
  plan.buffers      = plan.samples / batch;   // truncates; only exact when aligned
  plan.ms           = (double)plan.samples * 1000.0 / (double)srTarget;
  plan.changed      = false;
  if (srAuthored <= 0 || srTarget <= 0) return plan;

  if (!s.alignBpmToBuffers || (plan.samples >= batch && plan.samples % batch == 0))
    return plan;

  // Invert the rescale for a starting tick, then walk ticks outward for one whose
  // rescaled length is whole buffers. Walking ticks, not buffer counts, finds a
  // reachable tempo when the target rate is above the authored one.
  const ExportBpmAlign ideal = ExportAlignBpm(plan.requestedBpm, srTarget, batch, 16);
  long long base = (long long)((double)ideal.samples * (double)srAuthored / (double)srTarget + 0.5);
  if (base < 1) base = 1;

  // Crosses a whole buffer at any sane rate pair; a miss means the grid is unalignable.
  const int kSearch = 4 * 32;
  long long chosen = 0;
  for (int d = 0; d <= kSearch && !chosen; d++)
    for (int sign = 1; sign >= -1; sign -= 2)
    {
      const long long cand = base + (long long)sign * d;
      const long long got  = (cand >= 1) ? emitted(cand) : 0;
      if (got >= batch && got % batch == 0) { chosen = cand; break; }
      if (d == 0) break;  // +0 and -0 are the same candidate
    }
  if (!chosen) return plan;

  plan.songTick = (int)chosen;
  plan.samples  = (int)emitted(chosen);
  plan.buffers  = plan.samples / batch;
  plan.bpm      = ExportBpmFromSongTick(plan.songTick, (double)srAuthored);
  plan.ms       = (double)plan.samples * 1000.0 / (double)srTarget;
  plan.changed  = (plan.songTick != authoredTick);
  return plan;
}

namespace {
// Sizes the exporting instance's screen to the target while alive, then restores
// it: `width`/`height` fold into the export's compile, and the editor's bodies
// are built against the preview size.
struct ScopedScreenSize
{
  RenderCtx* screen;
  int prev_w, prev_h;
  ScopedScreenSize(RenderCtx* c, const ExportSettings& s)
    : screen(c), prev_w(vscreen_width_ctx(c)), prev_h(vscreen_height_ctx(c))
  {
    int w = 0, h = 0;
    ExportTargetScreenSize(s, w, h);
    vscreen_set_size_ctx(screen, w, h);
  }
  ~ScopedScreenSize() { vscreen_set_size_ctx(screen, prev_w, prev_h); }
  ScopedScreenSize(const ScopedScreenSize&) = delete;
  ScopedScreenSize& operator=(const ScopedScreenSize&) = delete;
};

// The authored 1/16 grid, spelled as SongExportGenerateC does.
int AuthoredSongTick(const SongExportInput& in)
{
  const double sr = in.sampleRate > 0.0 ? in.sampleRate : 44100.0;
  if (in.seq.samplesPerStep > 0.0)
  {
    const int tick = (int)in.seq.samplesPerStep;
    return tick < 1 ? 1 : tick;
  }
  return ExportSongTickFromBpm(in.bpm, sr);
}

// Written into the sequence: once the matrix has a step length, SongExport
// ignores `bpm`.
void AlignSongToBuffers(const ExportSettings& s, SongExportInput& io)
{
  const double sr = io.sampleRate > 0.0 ? io.sampleRate : 44100.0;
  const ExportBpmAlign plan =
    ExportPlanBpm(s, AuthoredSongTick(io), sr, io.batchSize > 0 ? io.batchSize : 32);
  if (!plan.changed) return;

  io.seq.samplesPerStep = (double)plan.songTick;
  io.bpm = plan.bpm;
}

bool WriteFile(const fs::path& path, const std::string& text, std::string& err)
{
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) { err = "cannot create " + path.parent_path().u8string() + ": " + ec.message(); return false; }
  if (!WriteWholeFile(path, text)) { err = "cannot write " + path.u8string(); return false; }
  return true;
}

// For machine-owned files a re-export must not overwrite.
bool WriteFileIfMissing(const fs::path& path, const std::string& text, bool& wrote,
                        std::string& err)
{
  wrote = false;
  if (fs::exists(path)) return true;
  if (!WriteFile(path, text, err)) return false;
  wrote = true;
  return true;
}

// A string scan, not a JSON parse. Not found means not a project, and the
// caller refuses.
bool ExistingUuid(const fs::path& packageJson, std::string& uuid)
{
  std::string text;
  if (!ReadWholeFile(packageJson, text)) return false;

  size_t k = text.find("\"uuid\"");
  if (k == std::string::npos) return false;
  size_t colon = text.find(':', k);
  if (colon == std::string::npos) return false;
  size_t open = text.find('"', colon);
  if (open == std::string::npos) return false;
  size_t close = text.find('"', open + 1);
  if (close == std::string::npos) return false;

  uuid = text.substr(open + 1, close - open - 1);
  return uuid.size() == 36;
}

std::string BaseName(const fs::path& dir)
{
  fs::path p = dir;
  if (p.has_filename()) return p.filename().u8string();
  return p.parent_path().filename().u8string();
}

// Pebble's package.json `name` must be npm-ish: lowercase, no spaces.
std::string SlugFor(const std::string& name)
{
  std::string s;
  for (char c : name)
  {
    if (c >= 'A' && c <= 'Z') s += (char)(c - 'A' + 'a');
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) s += c;
    else if (!s.empty() && s.back() != '-') s += '-';
  }
  while (!s.empty() && s.back() == '-') s.pop_back();
  return s.empty() ? "codesynth-song" : s;
}

// Placeholders the README templates use. One list for all targets: an unused
// value is free, a missing one is an export error.
std::vector<ExportTemplateVar> ReadmeVars(const ExportSettings& s, int authoredRate,
                                          const char* rateAsAuthored,
                                          const char* rateResampled)
{
  const int sr = ExportTargetSampleRate(s);
  int w = 0, h = 0;
  ExportTargetScreenSize(s, w, h);

  return {
    { "NAME",        s.name.empty() ? std::string("Exported codesynth song") : s.name },
    { "SLUG",        SlugFor(s.name) },
    { "SCREEN_W",    std::to_string(w) },
    { "SCREEN_H",    std::to_string(h) },
    { "SAMPLE_RATE", std::to_string(sr) },
    { "RATE_NOTE",   sr == authoredRate ? rateAsAuthored : rateResampled },
  };
}
} // namespace

// ---- Pebble / emery ----
namespace {
std::string PebblePackageJson(const std::string& displayName, const std::string& author,
                              const std::string& uuid)
{
  std::ostringstream o;
  o << "{\n"
    << "  \"name\": \"" << SlugFor(displayName) << "\",\n"
    << "  \"author\": \"" << (author.empty() ? std::string("codesynth") : author) << "\",\n"
    << "  \"version\": \"1.0.0\",\n"
    << "  \"keywords\": [\"pebble-app\"],\n"
    << "  \"private\": true,\n"
    << "  \"dependencies\": {},\n"
    << "  \"pebble\": {\n"
    << "    \"displayName\": \"" << displayName << "\",\n"
    << "    \"uuid\": \"" << uuid << "\",\n"
    << "    \"sdkVersion\": \"3\",\n"
    << "    \"enableMultiJS\": true,\n"
    << "    \"targetPlatforms\": [\n"
    << "      \"emery\"\n"
    << "    ],\n"
    << "    \"watchapp\": {\n"
    << "      \"watchface\": false\n"
    << "    },\n"
    // No messageKeys: nothing talks to the phone. The menu icon's `file` must match
    // the `binary` entry in templates/manifest.txt. Resources don't count against
    // the 64 KB cap.
    << "    \"messageKeys\": [],\n"
    << "    \"resources\": {\n"
    << "      \"media\": [\n"
    << "        { \"type\": \"bitmap\", \"name\": \"IMAGE_MENU_ICON\","
       " \"file\": \"cup icon 25x25.png\", \"menuIcon\": true }\n"
    << "      ]\n"
    << "    }\n"
    << "  }\n"
    << "}\n";
  return o.str();
}

// The phone IP, written once (WriteFileIfMissing); build_and_install.bat reads
// the first line.
std::string PebblePhoneIpSeed()
{
  return "192.168.1.128\n";
}

std::string PebbleSongConfigH(const ExportSettings& s, int sampleRate)
{
  std::ostringstream o;
  o << "// song_config.h -- GENERATED by the codesynth project exporter.\n"
    << "//\n"
    << "// Everything about this build that varies with the song or the export\n"
    << "// settings. main.c is the same bytes for every export and reads its\n"
    << "// values from here, which is what lets it stay a reviewable source file\n"
    << "// rather than a string in the exporter.\n"
    << "#ifndef SONG_CONFIG_H\n"
    << "#define SONG_CONFIG_H\n"
    << "\n"
    << "// ---- rates --------------------------------------------------------\n"
    << "// ONE rate used twice: the song is COMPILED at SONG_SAMPLE_RATE (see\n"
    << "// src/c/song/song.c) and PLAYED at AUDIO_SAMPLE_RATE (the\n"
    << "// SpeakerPcmFormat rate). They agree by construction here, and main.c\n"
    << "// static-asserts it, so overriding just one via SONG_DEFINES fails the\n"
    << "// build instead of silently retuning and retiming the whole song.\n"
    << "//\n"
    << "// The song was authored at " << sampleRate << " Hz; every rate-dependent\n"
    << "// constant in song.c is expressed over SONG_SAMPLE_RATE, so lowering it\n"
    << "// to the watch speaker's rate resamples tempo AND pitch together.\n"
    << "#ifndef SONG_SAMPLE_RATE\n"
    << "#define SONG_SAMPLE_RATE " << ExportTargetSampleRate(s) << "\n"
    << "#endif\n"
    << "#ifndef AUDIO_SAMPLE_RATE\n"
    << "#define AUDIO_SAMPLE_RATE SONG_SAMPLE_RATE\n"
    << "#endif\n"
    << "\n"
    << "// ---- playback -----------------------------------------------------\n"
    << "#define AUDIO_VOLUME   100\n"
    << "#define AUDIO_TIMER_MS 10\n"
    << "\n"
    << "// ---- speaker EQ ---------------------------------------------------\n"
    << "// Whether the speaker-compensation EQ starts enabled. It stays a runtime\n"
    << "// toggle (DOWN button), so this is the initial state, not a compile-out.\n"
    << "#define SONG_SPEAKER_FILTER_DEFAULT " << (s.speakerFilterDefault ? 1 : 0) << "\n"
    << "\n"
    << "// The coefficients themselves are fixed constants -- see speaker_eq.h.\n"
    << "#include \"speaker_eq.h\"\n"
    << "\n"
    << "#endif // SONG_CONFIG_H\n";
  return o.str();
}

// The generated pair lives outside src/c, so waf's src/c/**/*.c glob doesn't
// compile song.c twice.
std::string PebbleSongShimC()
{
  return
    "// Pebble build of the song: the generated player, compiled at the watch\n"
    "// speaker's sample rate.\n"
    "//\n"
    "// ../../../song/song.c is generated at the synth's authoring rate, but every\n"
    "// rate-dependent constant in it (the song tick, the t/b time bases, each\n"
    "// note's XRATE, each sample's playback scale) is expressed over\n"
    "// SONG_SAMPLE_RATE. So this whole file is one #include plus the rate that\n"
    "// song_config.h chose: no hand-rescaled copy, nothing to re-derive when the\n"
    "// song is re-exported.\n"
    "//\n"
    "// It lives here, in src/c, while the generated pair lives in song/ at the\n"
    "// project root -- because waf globs src/c/**/*.c, and a generated song.c\n"
    "// inside that glob would be compiled twice, once directly and once through\n"
    "// this file.\n"
    "#include \"song_config.h\"   // SONG_SAMPLE_RATE\n"
    "#include \"../../../song/song.c\"\n"
    "#include \"song.h\"\n";
}

std::string PebbleSongShimH()
{
  return
    "#ifndef SONG_PROJECT_SONG_H\n"
    "#define SONG_PROJECT_SONG_H\n"
    "\n"
    "// The generated header, reached from src/c so main.c can say\n"
    "// #include \"song/song.h\".\n"
    "//\n"
    "// Do NOT copy anything out of it into another header. song_SongState is\n"
    "// sized by SONG_CHANNEL_COUNT, which the exporter re-derives from the\n"
    "// recorded sequence on every export, so it changes when the song does. A\n"
    "// translation unit carrying its own copy silently under-allocates the state\n"
    "// and the player writes past its end -- which shows up only on the watch,\n"
    "// where .bss is packed tightly enough for the stray word to land on\n"
    "// something live. Include, never copy.\n"
    "#include \"../../../song/song.h\"\n"
    "\n"
    "#endif // SONG_PROJECT_SONG_H\n";
}

bool WritePebbleProject(const ExportSettings& settings, const SongExportInput& songIn,
                        const fs::path& dest, const std::string& displayName,
                        const std::string& uuid, ExportProjectResult& result,
                        std::string& err)
{
  // The include makes song.c portable; the loop shape comes from the export page.
  SongExportInput in = songIn;
  in.channelMode    = ExportTargetChannelMode(settings);
  in.vscreenInclude = "song/vscreen.h";

  std::string songC, songH;
  // Channels come from the C pass: SONG_OUTPUT_CHANNELS sizes buffers across TUs.
  int songChannels = 1;
  if (!SongExportGenerateC(in, songC, err, &songChannels)) return false;
  if (!SongExportGenerateH(in, songH, err, songChannels)) return false;
  result.outputChannels = songChannels;

  if (!WriteFile(dest / "song" / "song.c", songC, err)) return false;
  if (!WriteFile(dest / "song" / "song.h", songH, err)) return false;
  result.filesWritten += 2;
  result.songBytes = (long)songC.size();

  // ---- the template --------------------------------------------------------
  int templateFiles = 0;
  if (!ExportTemplateWrite(kExportTargetPebble, dest.u8string(), templateFiles, err)) return false;
  result.filesWritten += templateFiles;

  // ---- the project --------------------------------------------------------
  const int sr = (int)(in.sampleRate > 0.0 ? in.sampleRate : 44100.0);

  struct { const char* path; std::string text; } generated[] = {
    { "package.json",           PebblePackageJson(displayName, settings.author, uuid) },
    { "src/c/song_config.h",    PebbleSongConfigH(settings, sr) },
    { "src/c/song/song.c",      PebbleSongShimC() },
    { "src/c/song/song.h",      PebbleSongShimH() },
  };
  for (const auto& g : generated)
  {
    if (!WriteFile(dest / g.path, g.text, err)) return false;
    ++result.filesWritten;
  }

  // Seeded on the first export and left alone after: the IP belongs to whoever
  // installs, not to the project.
  bool wrotePhoneIp = false;
  if (!WriteFileIfMissing(dest / "pebble_phone_ip.txt", PebblePhoneIpSeed(), wrotePhoneIp, err))
    return false;
  if (wrotePhoneIp) ++result.filesWritten;

  return true;
}
} // namespace

// ---- win32 ----
namespace {
std::string Win32SongConfigH(const ExportSettings& s, int authoredRate)
{
  int w = 0, h = 0;
  ExportTargetScreenSize(s, w, h);
  const int sr = ExportTargetSampleRate(s);

  std::ostringstream o;
  o << "// song_config.h -- GENERATED by the codesynth project exporter.\n"
    << "//\n"
    << "// Everything about this build that varies with the song or the export\n"
    << "// settings. main.c is the same bytes for every export.\n"
    << "#ifndef SONG_CONFIG_H\n"
    << "#define SONG_CONFIG_H\n"
    << "\n"
    << "// ---- rate ---------------------------------------------------------\n"
    << "// The song was authored at " << authoredRate << " Hz. Every rate-dependent\n"
    << "// constant in song.c is expressed over SONG_SAMPLE_RATE, so changing it\n"
    << "// resamples tempo AND pitch together rather than just retiming.\n"
    << "//\n"
    << (sr == authoredRate
          ? "// This build plays the song as authored.\n"
          : "// This build deliberately does NOT play it as authored -- at this\n"
            "// rate it is a preview of what the same song sounds like on a\n"
            "// target pinned to it (the watch speaker runs at 16 kHz).\n")
    << "#ifndef SONG_SAMPLE_RATE\n"
    << "#define SONG_SAMPLE_RATE " << sr << "\n"
    << "#endif\n"
    << "\n"
    << "// ---- screen -------------------------------------------------------\n"
    << "// These size STATIC arrays in vscreen.c, so the framebuffer is fixed at\n"
    << "// build time and the window is sized to match it -- the blit is 1:1 and\n"
    << "// nothing is scaled.\n"
    << "#define VSCREEN_W " << w << "\n"
    << "#define VSCREEN_H " << h << "\n"
    << "\n"
    << "// Drop this to build without the visual half.\n"
    << "#define SONG_VISUALS 1\n"
    << "\n"
    << "#define SONG_WINDOW_TITLE \"" << s.name << "\"\n"
    << "\n"
    << "#endif // SONG_CONFIG_H\n";
  return o.str();
}

// As on Pebble, the generated pair lives outside src/c.
std::string Win32SongShimC()
{
  return
    "// The generated player, compiled at this build's sample rate.\n"
    "//\n"
    "// ../../../song/song.c is generated at the synth's authoring rate, but every\n"
    "// rate-dependent constant in it is expressed over SONG_SAMPLE_RATE -- so this\n"
    "// whole file is one #include plus the rate song_config.h chose.\n"
    "#include \"song_config.h\"   // SONG_SAMPLE_RATE\n"
    "#include \"../../../song/song.c\"\n";
}

std::string Win32SongShimH()
{
  return
    "#ifndef SONG_PROJECT_SONG_H\n"
    "#define SONG_PROJECT_SONG_H\n"
    "\n"
    "// The generated header, reached from src/c so main.c can say\n"
    "// #include \"song/song.h\".\n"
    "//\n"
    "// Do NOT copy anything out of it into another header: song_SongState is\n"
    "// sized by SONG_CHANNEL_COUNT, which the exporter re-derives from the\n"
    "// recorded sequence on every export.\n"
    "#include \"../../../song/song.h\"\n"
    "\n"
    "#endif // SONG_PROJECT_SONG_H\n";
}

bool WriteWin32Project(const ExportSettings& settings, const SongExportInput& songIn,
                       const fs::path& dest, ExportProjectResult& result,
                       std::string& err)
{
  SongExportInput in = songIn;
  in.channelMode    = ExportTargetChannelMode(settings);
  in.vscreenInclude = "vscreen.h";

  std::string songC, songH;
  // Channels come from the C pass.
  int songChannels = 1;
  if (!SongExportGenerateC(in, songC, err, &songChannels)) return false;
  if (!SongExportGenerateH(in, songH, err, songChannels)) return false;
  result.outputChannels = songChannels;

  if (!WriteFile(dest / "song" / "song.c", songC, err)) return false;
  if (!WriteFile(dest / "song" / "song.h", songH, err)) return false;
  result.filesWritten += 2;
  result.songBytes = (long)songC.size();

  const int authoredRate = (int)(in.sampleRate > 0.0 ? in.sampleRate : 44100.0);

  // README.md and build.bat are `subst` templates; build.bat takes {{SLUG}}, the
  // .exe's name.
  int templateFiles = 0;
  if (!ExportTemplateWrite(kExportTargetWin32, dest.u8string(), templateFiles, err,
                           ReadmeVars(settings, authoredRate,
                                      "(as authored)",
                                      "(resampled from the authored rate)")))
    return false;
  result.filesWritten += templateFiles;

  struct { const char* path; std::string text; } generated[] = {
    { "src/c/song_config.h", Win32SongConfigH(settings, authoredRate) },
    { "src/c/song/song.c",   Win32SongShimC() },
    { "src/c/song/song.h",   Win32SongShimH() },
  };
  for (const auto& g : generated)
  {
    if (!WriteFile(dest / g.path, g.text, err)) return false;
    ++result.filesWritten;
  }
  return true;
}
} // namespace

// ---- microw8 ----
namespace {
std::string Microw8SongConfigH(const ExportSettings& s, int authoredRate)
{
  std::ostringstream o;
  o << "// song_config.h -- GENERATED by the codesynth project exporter.\n"
    << "#ifndef SONG_CONFIG_H\n"
    << "#define SONG_CONFIG_H\n"
    << "\n"
    << "// ---- rate ---------------------------------------------------------\n"
    << "// Fixed by the platform: microw8 calls snd() at 44100 Hz. The song was\n"
    << "// authored at " << authoredRate << " Hz"
    << (authoredRate == 44100 ? ", so it plays exactly as authored.\n"
                              : ", so it is resampled -- every rate-dependent\n"
                                "// constant in song.c is expressed over SONG_SAMPLE_RATE, which moves\n"
                                "// tempo and pitch together.\n")
    << "#define SONG_SAMPLE_RATE " << ExportTargetSampleRate(s) << "\n"
    << "\n"
    << "// ---- screen -------------------------------------------------------\n"
    << "// Also fixed by the platform: 320x240, 8-bit palette. vscreen matches it\n"
    << "// exactly -- see cart.c's palette_init, which programs the palette to the\n"
    << "// Pebble GColor8 cube precisely so a vscreen byte IS a valid palette\n"
    << "// index, and no conversion is needed anywhere.\n"
    << "#define VSCREEN_W 320\n"
    << "#define VSCREEN_H 240\n"
    << "\n"
    << "// Draw STRAIGHT INTO microw8's framebuffer rather than keeping a private\n"
    << "// copy. Without this vscreen declares its own 320x240 buffer -- 76,800\n"
    << "// bytes duplicating the one the platform already provides at 0x78 -- and\n"
    << "// that duplicate is most of what pushed this cart past the static-memory\n"
    << "// ceiling above 0x14000. It also removes a full-screen copy per\n"
    << "// frame. VSCREEN_W must equal the host stride (320) for this to be sound.\n"
    << "#define VSCREEN_FRAMEBUFFER_ADDR 0x78\n"
    << "\n"
    << "// The same arrangement for the PALETTE, and for the same reason: with\n"
    << "// this, a script's `palette[id] = rgb(r,g,b)` stores directly into the\n"
    << "// hardware table at 0x13000 and the cart carries no 1,024-byte shadow of\n"
    << "// it and no per-frame upload. It works because a vscreen palette entry is\n"
    << "// 0xAABBGGRR -- red in the LOW byte -- which is exactly microw8's four\n"
    << "// R,G,B,A bytes read as a little-endian word (see vscreen.h). cart.c\n"
    << "// programs the default cube into it once, before the first frame.\n"
    << "#define VSCREEN_PALETTE_ADDR 0x13000\n"
    << "\n"
    << "// ---- memory -------------------------------------------------------\n"
    << "// The cart has 256 KB TOTAL and the framebuffer is 76,800 of it, so the\n"
    << "// renderer gets Pebble-sized arenas rather than the desktop's roomy\n"
    << "// defaults (768 KB of glyph scratch would not fit twice over). The glyph\n"
    << "// pipeline is compiled through the same low-memory shim the watch uses\n"
    << "// (font_lowspec_pebble.c), where a glyph fits in ~14 KB.\n"
    << "#define VSCREEN_GLYPH_SCRATCH_BYTES 15600\n"
    << "#define VSCREEN_IMAGE_ARENA_BYTES   2048\n"
    << "#define VSCREEN_MAX_IMAGES          2\n"
    << "#define VSCREEN_TARGET_STACK_MAX    2\n"
    << "// No glyph cache: its ~29 KB block does not fit beside the rest. Text\n"
    << "// draws the same, uncached.\n"
    << "#define VSCREEN_FONT_CACHE_BYTES    0\n"
    << "\n"
    << "// Drop this to build without the visual half.\n"
    << "#define SONG_VISUALS 1\n"
    << "\n"
    << "#endif // SONG_CONFIG_H\n";
  return o.str();
}

std::string Microw8SongShimC()
{
  return
    "// The generated player, compiled at microw8's fixed 44100 Hz.\n"
    "#include \"song_config.h\"   // SONG_SAMPLE_RATE\n"
    "#include \"../../../song/song.c\"\n";
}

std::string Microw8SongShimH()
{
  return
    "#ifndef SONG_PROJECT_SONG_H\n"
    "#define SONG_PROJECT_SONG_H\n"
    "\n"
    "// The generated header, reached from src/c so cart.c can say\n"
    "// #include \"song/song.h\". Do NOT copy anything out of it: song_SongState\n"
    "// is sized by SONG_CHANNEL_COUNT, which changes when the song does.\n"
    "#include \"../../../song/song.h\"\n"
    "\n"
    "#endif // SONG_PROJECT_SONG_H\n";
}

bool WriteMicrow8Project(const ExportSettings& settings, const SongExportInput& songIn,
                         const fs::path& dest, ExportProjectResult& result,
                         std::string& err)
{
  SongExportInput in = songIn;
  in.channelMode    = ExportTargetChannelMode(settings);
  in.vscreenInclude = "vscreen.h";

  std::string songC, songH;
  // Channels come from the C pass.
  int songChannels = 1;
  if (!SongExportGenerateC(in, songC, err, &songChannels)) return false;
  if (!SongExportGenerateH(in, songH, err, songChannels)) return false;
  result.outputChannels = songChannels;

  if (!WriteFile(dest / "song" / "song.c", songC, err)) return false;
  if (!WriteFile(dest / "song" / "song.h", songH, err)) return false;
  result.filesWritten += 2;
  result.songBytes = (long)songC.size();

  const int authoredRate = (int)(in.sampleRate > 0.0 ? in.sampleRate : 44100.0);

  // README.md, build.bat and run.bat all take {{SLUG}}.
  int templateFiles = 0;
  if (!ExportTemplateWrite(kExportTargetMicrow8, dest.u8string(), templateFiles, err,
                           ReadmeVars(settings, authoredRate,
                                      "(the song is played as authored)",
                                      "(the song was authored at a different rate and is resampled)")))
    return false;
  result.filesWritten += templateFiles;

  struct { const char* path; std::string text; } generated[] = {
    { "src/c/song_config.h", Microw8SongConfigH(settings, authoredRate) },
    { "src/c/song/song.c",   Microw8SongShimC() },
    { "src/c/song/song.h",   Microw8SongShimH() },
  };
  for (const auto& g : generated)
  {
    if (!WriteFile(dest / g.path, g.text, err)) return false;
    ++result.filesWritten;
  }
  return true;
}
} // namespace

// ---- wav ----
// An ordinary project export plus the offline renderer, built and run here.
// Building the export, rather than mixing in-process, makes the .wav the export
// by construction instead of a third sequencer implementation. Needs clang;
// without it the source project is still complete.
namespace {
std::string WavSongConfigH(const ExportSettings& s, int authoredRate)
{
  int w = 0, h = 0;
  ExportTargetScreenSize(s, w, h);
  const int sr = ExportTargetSampleRate(s);

  std::ostringstream o;
  o << "// song_config.h -- GENERATED by the codesynth project exporter.\n"
    << "//\n"
    << "// Everything about this render that varies with the song or the export\n"
    << "// settings. render_song.c is the same bytes for every export.\n"
    << "#ifndef SONG_CONFIG_H\n"
    << "#define SONG_CONFIG_H\n"
    << "\n"
    << "// ---- rate ---------------------------------------------------------\n"
    << "// The song was authored at " << authoredRate << " Hz. Every rate-dependent\n"
    << "// constant in song.c is expressed over SONG_SAMPLE_RATE, so changing it\n"
    << "// resamples tempo AND pitch together rather than just retiming -- and\n"
    << "// the .wav is written at whatever this says.\n"
    << "//\n"
    << (sr == authoredRate
          ? "// This render plays the song as authored.\n"
          : "// This render deliberately does NOT play it as authored -- at this\n"
            "// rate it is a preview of what the same song sounds like on a\n"
            "// target pinned to it (the watch speaker runs at 16 kHz).\n")
    << "#ifndef SONG_SAMPLE_RATE\n"
    << "#define SONG_SAMPLE_RATE " << sr << "\n"
    << "#endif\n"
    << "\n"
    << "// ---- screen -------------------------------------------------------\n"
    << "// A .wav render draws nothing, but song.c still DEFINES the song's\n"
    << "// visual half and vscreen.c is linked to resolve it -- so these must\n"
    << "// match what the bodies were compiled against, exactly as they do for\n"
    << "// the win32 target. They size static arrays in vscreen.c.\n"
    << "#define VSCREEN_W " << w << "\n"
    << "#define VSCREEN_H " << h << "\n"
    << "\n"
    << "#endif // SONG_CONFIG_H\n";
  return o.str();
}

bool WriteWavProject(const ExportSettings& settings, const SongExportInput& songIn,
                     const fs::path& dest, ExportProjectResult& result,
                     std::string& err)
{
  // Same layout as win32, sharing its shims and include paths.
  SongExportInput in = songIn;
  in.channelMode    = ExportTargetChannelMode(settings);
  in.vscreenInclude = "vscreen.h";

  std::string songC, songH;
  int songChannels = 1;
  if (!SongExportGenerateC(in, songC, err, &songChannels)) return false;
  if (!SongExportGenerateH(in, songH, err, songChannels)) return false;
  result.outputChannels = songChannels;

  if (!WriteFile(dest / "song" / "song.c", songC, err)) return false;
  if (!WriteFile(dest / "song" / "song.h", songH, err)) return false;
  result.filesWritten += 2;
  result.songBytes = (long)songC.size();

  const int authoredRate = (int)(in.sampleRate > 0.0 ? in.sampleRate : 44100.0);

  int templateFiles = 0;
  if (!ExportTemplateWrite(kExportTemplateWav, dest.u8string(), templateFiles, err,
                           ReadmeVars(settings, authoredRate,
                                      "(as authored)",
                                      "(resampled from the authored rate)")))
    return false;
  result.filesWritten += templateFiles;

  struct { const char* path; std::string text; } generated[] = {
    { "src/c/song_config.h", WavSongConfigH(settings, authoredRate) },
    { "src/c/song/song.c",   Win32SongShimC() },
    { "src/c/song/song.h",   Win32SongShimH() },
  };
  for (const auto& g : generated)
  {
    if (!WriteFile(dest / g.path, g.text, err)) return false;
    ++result.filesWritten;
  }
  return true;
}

// Files WriteWavProject generated; the template's come from ExportTemplateAll().
const char* const kWavGeneratedFiles[] = {
  "song/song.c",
  "song/song.h",
  "src/c/song_config.h",
  "src/c/song/song.c",
  "src/c/song/song.h",
};

// Build output goes to build.log. False means no .wav, which is not an export failure.
bool RunWavBuild(const fs::path& dest, const std::string& slug, std::string& why)
{
  const fs::path bat = dest / "build.bat";
  const fs::path log = dest / "build.log";
  const fs::path wav = dest / (slug + ".wav");

  std::error_code ec;
  fs::remove(wav, ec);  // so a stale file can't pass for this one

  // The path may contain spaces; cmd needs the extra outer quotes.
#ifdef _WIN32
  std::wstring cmd = L"cmd /c \"\"" + bat.wstring() + L"\" > \"" + log.wstring() + L"\" 2>&1\"";
  int rc = _wsystem(cmd.c_str());
#else
  std::string cmd = "cmd /c \"\"" + bat.string() + "\" > \"" + log.string() + "\" 2>&1\"";
  int rc = std::system(cmd.c_str());
#endif

  if (rc != 0)
  {
    why = "build failed (see " + log.u8string() + ") -- the WAV targets need clang on PATH";
    return false;
  }
  if (!fs::exists(wav))
  {
    why = "the build reported success but wrote no " + wav.filename().u8string()
        + " (see " + log.u8string() + ")";
    return false;
  }
  return true;
}

// Deletes what this export wrote, leaving the .wav and the user's own files.
// Template files are enumerated from ExportTemplateAll(), since most arrive via
// #include and are named nowhere else. Best-effort; remove(), not remove_all(),
// on directories.
void RemoveWavSource(const fs::path& dest, const std::string& slug)
{
  std::error_code ec;
  std::vector<fs::path> dirs;   // parents of everything removed, for the sweep below

  auto drop = [&](const fs::path& rel) {
    fs::remove(dest / rel, ec);
    for (fs::path d = rel.parent_path(); !d.empty(); d = d.parent_path())
      dirs.push_back(d);
  };

  size_t count = 0;
  const ExportTemplateFile* all = ExportTemplateAll(count);
  for (size_t i = 0; i < count; i++)
    if (all[i].targets & kExportTemplateWav)
      drop(fs::u8path(all[i].path));

  for (const char* rel : kWavGeneratedFiles)
    drop(fs::u8path(rel));

  // Build artefacts go too.
  drop(fs::u8path("build.log"));
  drop(fs::u8path(slug + "_render.exe"));

  // Deepest first.
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
  });
  for (const fs::path& d : dirs)
    fs::remove(dest / d, ec);
}
} // namespace

bool ExportProjectWrite(const ExportSettings& settings, const SongExportInput& songIn,
                        ExportProjectResult& result, std::string& err)
{
  result = ExportProjectResult();
  err.clear();

  if (settings.destDir.empty()) { err = "no export folder set"; return false; }
  if (settings.target != kExportTargetPebble &&
      settings.target != kExportTargetWin32 &&
      settings.target != kExportTargetMicrow8 &&
      !ExportTargetIsWav(settings.target))
  {
    err = "unknown export target";
    return false;
  }

  fs::path dest = fs::u8path(settings.destDir);
  std::error_code ec;
  fs::create_directories(dest, ec);
  if (ec) { err = "cannot create " + dest.u8string() + ": " + ec.message(); return false; }

  std::string displayName = settings.name.empty() ? BaseName(dest) : settings.name;
  if (displayName.empty()) displayName = "codesynth song";

  ExportSettings s = settings;
  s.name = displayName;

  // Every compile below sees the target's screen.
  ScopedScreenSize screen(songIn.screen, s);

  // A copy, so the live sequence keeps its recorded tempo.
  SongExportInput song = songIn;
  if (s.alignBpmToBuffers) AlignSongToBuffers(s, song);
  if (s.sndLoopShape <= kSongSndLoopPerSample) song.sndLoopShape = (SongSndLoopShape)s.sndLoopShape;

  switch (settings.target)
  {
    case kExportTargetPebble:
    {
      // Pebble only: the watch keys an app on its uuid. In order: settings.uuid, an
      // existing package.json's, or a fresh one. The not-a-project guard is separate:
      // an explicit uuid doesn't license overwriting an unreadable package.json.
      const fs::path pkg = dest / "package.json";
      std::string existing;
      const bool haveExisting = fs::exists(pkg);
      if (haveExisting && !ExistingUuid(pkg, existing))
      {
        err = "refusing to overwrite: " + pkg.u8string() +
              " exists but has no readable Pebble uuid, so it is probably not an "
              "exported project";
        return false;
      }

      if (!s.uuid.empty())
      {
        if (!ExportUuidLooksValid(s.uuid))
        {
          err = "export uuid '" + s.uuid + "' is not a uuid (want 8-4-4-4-12 hex, "
                "e.g. 1a2b3c4d-5e6f-4a8b-9c0d-1e2f3a4b5c6d)";
          return false;
        }
        result.uuid    = s.uuid;
        result.setUuid = true;
      }
      else if (haveExisting)
      {
        result.uuid       = existing;
        result.reusedUuid = true;
      }
      else
      {
        result.uuid = ExportMakeUuid();
      }

      if (!WritePebbleProject(s, song, dest, displayName, result.uuid, result, err))
        return false;
      break;
    }

    case kExportTargetWin32:
      if (!WriteWin32Project(s, song, dest, result, err)) return false;
      break;

    case kExportTargetMicrow8:
      if (!WriteMicrow8Project(s, song, dest, result, err)) return false;
      break;

    case kExportTargetWavStereo:
    case kExportTargetWavMono:
    {
      if (!WriteWavProject(s, song, dest, result, err)) return false;

      // A failed build is reported, not an export failure.
      const std::string slug = SlugFor(s.name);
      std::string why;
      if (RunWavBuild(dest, slug, why))
      {
        result.wavPath = (dest / (slug + ".wav")).u8string();
        // Only now is removing the source safe; a failed build keeps it for diagnosis.
        if (s.wavRemoveSource)
        {
          RemoveWavSource(dest, slug);
          result.filesWritten = 1;   // what is left is the .wav
        }
      }
      else
      {
        result.wavError = why;
      }
      break;
    }

    default:
      err = "unknown export target";
      return false;
  }

  result.projectDir = fs::absolute(dest, ec).u8string();
  return true;
}
