// codesynth_export: the standalone "Save C Song". Reads codesynth.txt and
// codesynth.seq and writes song.c + song.h through the plugin's generator.
//
// codesynth_export [dir] [-o out.c] [--sr HZ] [--bpm BPM]
//                  [--group-snd-loops | --inline-snd-loops |
//                   --no-inline-snd-loops | --auto-snd-loops |
//                   --per-sample-snd-loops] [--add-empty-cases]
//
// `dir` (default: current) holds codesynth.txt, codesynth.seq and any `// WAV`
// samples. --sr/--bpm override the authored rate/tempo.

#include <cstdio>
#include <cstring>
#include <string>

#include "vscreen.h"          // vscreen_ctx() -- this tool's single screen
#include "codesynth/CodeSynthParse.h"
#include "codesynth/ExportTemplate.h"
#include "codesynth/ExportProject.h"
#include "codesynth/SongExport.h"
#include "codesynth/SongSeq.h"
#include "codesynth/Utf8File.h"

namespace {
// workingDir is concatenated directly with each relative `// WAV` path.
std::string WithTrailingSlash(const std::string& dir)
{
  if (dir.empty()) return "./";
  char last = dir[dir.size() - 1];
  if (last == '/' || last == '\\') return dir;
  return dir + "/";
}

void Usage()
{
  fprintf(stderr,
    "usage: codesynth_export [dir] [-o out.c] [--sr HZ] [--bpm BPM]\n"
    "                        [--no-inline-snd-loops | --group-snd-loops | --auto-snd-loops |\n"
    "                         --per-sample-snd-loops]\n"
    "                        [--group-visual-cases | --per-event-visual-cases]\n"
    "                        [--add-empty-cases] [--align-bpm]\n"
    "                        [--export-project DIR --target TARGET]\n"
    "  dir     directory holding codesynth.txt + codesynth.seq (default: .)\n"
    "  --target  pebble | win32 | microw8 | wav | wav-mono\n"
    "          The two wav targets render an audio file instead of a program:\n"
    "          they write a source project (song + render_song.c + build.bat)\n"
    "          and then BUILD it with clang, which must be on PATH. `wav` is\n"
    "          stereo, `wav-mono` is one channel. A sound is stereo when its\n"
    "          body reads the `pan` argument (-1 left, +1 right, 0 centre);\n"
    "          under win32/microw8 the export is 2-channel only if some sound\n"
    "          actually does, while `wav` is always 2-channel\n"
    "  --wav-remove-source\n"
    "          (wav targets only) delete the source project once the .wav\n"
    "          exists, leaving just the audio file. Only ever runs after a\n"
    "          SUCCESSFUL render, and only removes what the export wrote\n"
    "  -o      output path (default: <dir>/song.c); the companion header is\n"
    "          written alongside it as the same path with a .h extension\n"
    "  --sr    export sample rate  (default: 44100)\n"
    "  --uuid  pebble app id for --export-project, as 8-4-4-4-12 hex.\n"
    "          Omitted keeps the behaviour that predates the settings\n"
    "          page field: reuse the id already in the destination\n"
    "          package.json, or mint a fresh one\n"
    "  --bpm   tempo the matrix was recorded at (default: 120)\n"
    "  --group-snd-loops   (DEFAULT)\n"
    "          one batch loop per INSTRUMENT rather than per event, with the\n"
    "          per-(note, velocity) constants moved into const tables. Code\n"
    "          size is O(instruments) + table instead of O(events), which is\n"
    "          what fits a big song in a Pebble's 64 KB app image\n"
    "  --inline-snd-loops\n"
    "          the older shape: one fully constant-folded batch loop per EVENT.\n"
    "          Marginally faster (the per-note constants are immediates rather\n"
    "          than table reads) and much larger. Also the reference to diff a\n"
    "          suspect grouped export against -- both must render identically\n"
    "  --no-inline-snd-loops\n"
    "          emit song_render_audio_batch's switch INSIDE one shared batch\n"
    "          loop (the compact shape). Smallest of the three per event, but\n"
    "          it dispatches once per output SAMPLE, so it is also the slowest\n"
    "  --auto-snd-loops\n"
    "          --no-inline-snd-loops below 8 sounding events, else\n"
    "          --group-snd-loops (the export page's default)\n"
    "  --per-sample-snd-loops\n"
    "          one batch loop with the channel loop inside it; each sample is\n"
    "          summed in a local and written straight out, no accumulator\n"
    "  --per-event-visual-cases   (DEFAULT)\n"
    "          song_visual_render gets one fully constant-folded switch case\n"
    "          per event\n"
    "  --group-visual-cases\n"
    "          one switch case per VISUAL INSTRUMENT instead, with the\n"
    "          per-(note, velocity) constants moved into const tables -- the\n"
    "          same trade as --group-snd-loops. MEASURE IT: a visual case is\n"
    "          only a call with a few immediate arguments, so unlike the sound\n"
    "          loops the tables can cost more than the cases they replace (on\n"
    "          the current song, +232 bytes). Both shapes draw identically\n"
    "  --add-empty-cases\n"
    "          keep a commented `case N: break;` for every event that renders\n"
    "          nothing in a given switch (no sound body / no visual body)\n"
    "          instead of omitting it. Same behaviour, more readable export\n"
    "  --align-bpm\n"
    "          (--export-project only) nudge the exported tempo to the nearest\n"
    "          one whose 1/16 note is a whole number of audio buffers at the\n"
    "          TARGET's rate. Every target ticks the sequencer once per buffer,\n"
    "          so an unaligned grid puts each step up to a buffer early or late\n");
}
} // namespace

int main(int argc, char** argv)
{
  std::string dir = ".";
  std::string outPath;
  double sr = 44100.0;
  double bpm = 120.0;
  SongSndLoopShape sndLoopShape = kSongSndLoopGrouped;   // SongExportInput's default
  SongVisSwitchShape visSwitchShape = kSongVisSwitchPerEvent; // ditto
  bool addEmptyCases = false;
  bool sawDir = false;
  std::string dumpTemplateDir;
  std::string exportProjectDir;
  std::string projectName;
  std::string projectUuid;
  bool speakerFilter = true;
  bool wavRemoveSource = false;
  bool alignBpm = false;
  unsigned target = kExportTargetPebble;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) { fprintf(stderr, "codesynth_export: %s needs a value\n", what); exit(2); }
      return argv[++i];
    };
    if (a == "-o")          outPath = next("-o");
    else if (a == "--sr")   sr = atof(next("--sr"));
    else if (a == "--bpm")  bpm = atof(next("--bpm"));
    else if (a == "--no-inline-snd-loops") sndLoopShape = kSongSndLoopShared;
    else if (a == "--group-snd-loops")     sndLoopShape = kSongSndLoopGrouped;  // the default
    else if (a == "--inline-snd-loops")    sndLoopShape = kSongSndLoopInlined;
    else if (a == "--auto-snd-loops")      sndLoopShape = kSongSndLoopAuto;
    else if (a == "--per-sample-snd-loops") sndLoopShape = kSongSndLoopPerSample;
    else if (a == "--group-visual-cases")     visSwitchShape = kSongVisSwitchGrouped;
    else if (a == "--per-event-visual-cases") visSwitchShape = kSongVisSwitchPerEvent; // the default
    else if (a == "--add-empty-cases")     addEmptyCases = true;
    else if (a == "--dump-template")       dumpTemplateDir = next("--dump-template");
    else if (a == "--export-project")      exportProjectDir = next("--export-project");
    else if (a == "--project-name")        projectName = next("--project-name");
    else if (a == "--uuid")                projectUuid = next("--uuid");
    else if (a == "--no-speaker-filter")   speakerFilter = false;
    else if (a == "--align-bpm")           alignBpm = true;
    else if (a == "--wav-remove-source")   wavRemoveSource = true;
    else if (a == "--target") {
      std::string t = next("--target");
      if      (t == "pebble")   target = kExportTargetPebble;
      else if (t == "win32")    target = kExportTargetWin32;
      else if (t == "microw8")  target = kExportTargetMicrow8;
      // "wav" is stereo, as on the settings page.
      else if (t == "wav")      target = kExportTargetWavStereo;
      else if (t == "wav-mono") target = kExportTargetWavMono;
      else { fprintf(stderr, "codesynth_export: unknown target '%s'\n", t.c_str()); return 2; }
    }
    else if (a == "-h" || a == "--help") { Usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { fprintf(stderr, "codesynth_export: unknown option '%s'\n", a.c_str()); Usage(); return 2; }
    else if (!sawDir) { dir = a; sawDir = true; }
    else { fprintf(stderr, "codesynth_export: unexpected argument '%s'\n", a.c_str()); Usage(); return 2; }
  }

  // Template-only: needs no song.
  if (!dumpTemplateDir.empty()) {
    int written = 0;
    std::string terr;
    // Raw: byte-compared against the repo (tools/check_template_dump.py).
    if (!ExportTemplateWrite(target, dumpTemplateDir, written, terr, {},
                             kExportTemplateRaw)) {
      fprintf(stderr, "codesynth_export: template dump failed: %s\n", terr.c_str());
      return 1;
    }
    printf("codesynth_export: wrote %d template file%s to %s\n",
           written, written == 1 ? "" : "s", dumpTemplateDir.c_str());
    return 0;
  }

  const std::string root = WithTrailingSlash(dir);
  if (outPath.empty()) outPath = root + "song.c";

  std::string spec;
  if (!ReadWholeFile(root + "codesynth.txt", spec)) {
    fprintf(stderr, "codesynth_export: cannot read %scodesynth.txt\n", root.c_str());
    return 1;
  }

  SongExportInput in;
  if (!SongSeqReadFile(root + "codesynth.seq", in.seq)) {
    fprintf(stderr, "codesynth_export: cannot read %scodesynth.seq\n", root.c_str());
    return 1;
  }


  in.entries    = CodeSynthSplitEntries(spec);
  in.sampleRate = sr;
  in.bpm        = bpm;
  in.workingDir = root;
  // SteepSynthEngine::kBlockSize, so the output matches the plugin's byte for byte.
  in.batchSize  = 32;
  // One song per process: the file-scope context.
  in.screen     = vscreen_ctx();
  in.sndLoopShape   = sndLoopShape;
  in.visSwitchShape = visSwitchShape;
  in.addEmptyCases  = addEmptyCases;

  int numReserved = 0;
  for (const CodeSynthEntry& e : in.entries)
    if (CodeSynthEntryIsReserved(e)) numReserved++;
  const int numSounds = (int)in.entries.size() - numReserved;
  std::string blocks;
  if (CodeSynthCommonIndex(in.entries)  >= 0) blocks += " + common block";
  if (CodeSynthGlobalsIndex(in.entries) >= 0) blocks += " + globals block";
  printf("codesynth_export: %d sound%s%s, %d event row%s x %d steps\n",
         numSounds, numSounds == 1 ? "" : "s", blocks.c_str(),
         (int)in.seq.rows.size(), in.seq.rows.size() == 1 ? "" : "s", in.seq.cols);

  std::string err;

  // A whole project instead of the bare pair, from the same SongExportInput.
  if (!exportProjectDir.empty()) {
    ExportSettings settings;
    settings.destDir              = exportProjectDir;
    settings.target               = target;
    settings.name                 = projectName;
    settings.uuid                 = projectUuid;
    settings.speakerFilterDefault = speakerFilter;
    settings.wavRemoveSource      = wavRemoveSource;
    settings.alignBpmToBuffers    = alignBpm;
    settings.sndLoopShape         = sndLoopShape;

    ExportProjectResult res;
    if (!ExportProjectWrite(settings, in, res, err)) {
      fprintf(stderr, "codesynth_export: project export failed: %s\n", err.c_str());
      return 1;
    }
    printf("codesynth_export: wrote %d files to %s\n",
           res.filesWritten, res.projectDir.c_str());
    printf("codesynth_export: uuid %s (%s), song.c %ld bytes, %s\n",
           res.uuid.c_str(),
           res.setUuid ? "as set" : res.reusedUuid ? "reused" : "new",
           res.songBytes,
           res.outputChannels == 2 ? "stereo" : "mono");
    if (ExportTargetIsWav(settings.target)) {
      // A missing .wav is loud on stderr but exits 0: the source project is complete.
      if (res.wavPath.empty())
        fprintf(stderr, "codesynth_export: NO WAV: %s\n", res.wavError.c_str());
      else
        printf("codesynth_export: %s (%s)\n", res.wavPath.c_str(),
               wavRemoveSource ? "source removed" : "source kept alongside");
    }
    if (alignBpm) {
      // Recomputed: the writer plans on a copy.
      const int tick = in.seq.samplesPerStep > 0.0
                         ? (int)in.seq.samplesPerStep
                         : ExportSongTickFromBpm(in.bpm, in.sampleRate);
      const ExportBpmAlign plan = ExportPlanBpm(settings, tick, in.sampleRate, in.batchSize);
      printf("codesynth_export: exported bpm %g (%s)\n", plan.bpm,
             plan.changed ? "aligned to buffers" : "unchanged");
    }
    return 0;
  }

  if (!SongExportWriteFile(in, outPath, err)) {
    fprintf(stderr, "codesynth_export: export failed: %s\n", err.c_str());
    return 1;
  }

  // Name both outputs.
  size_t dot = outPath.find_last_of('.');
  size_t sep = outPath.find_last_of("/\\");
  std::string hPath = (dot == std::string::npos || (sep != std::string::npos && dot < sep))
                        ? outPath + ".h" : outPath.substr(0, dot) + ".h";
  printf("codesynth_export: wrote %s and %s\n", outPath.c_str(), hPath.c_str());
  return 0;
}
