#include "CodeSynthParse.h"
#include "vscreen.h"

#include <string>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <algorithm>
#include <climits>
#include <cmath>

namespace fs = std::filesystem;

namespace {
// Not "cVoice": INDEXYNT.cpp, unity-built into this TU, declares one.
struct cCodeSynthLiveVoice
{
  int midiNoteId;
  int synthId;
  float gain;
  int velocity;  // raw 0..127, kept beside gain so vel == 127 compares exactly
  float freqMul;
  int timeStampTriggered;
  song_smp_voice_t smpState;  // per-voice smp() cursor, so overlapping notes stream independently
  CodeSynthVmCtx vmCtx;  // the VM keeps this pointer across func_run
  cCodeSynthLiveVoice() : midiNoteId(-1), synthId(-1), gain(0.0f), velocity(0), freqMul(1.0f), timeStampTriggered(0), smpState{}, vmCtx{} { }
};

// Stereo voices render at left and right; everything mono at centre.
constexpr double kPanLeft   = -1.0;
constexpr double kPanCentre =  0.0;
constexpr double kPanRight  =  1.0;

constexpr int kCodeSynthMaxVoices = 64;

// One compile-to-voices path: audio on the audio thread, visual on the UI thread.
// Each only ever touches its own pointers from its own thread.
struct cCodeSynthPipeline
{
  cCodeSynthLiveVoice voice[kCodeSynthMaxVoices];
  int time = 0;
  cCodeSynthReadonly* current = nullptr;
  std::atomic<cCodeSynthReadonly*> update{nullptr};
  std::atomic<cCodeSynthReadonly*> noLongerInUse{nullptr};
  std::atomic<unsigned> globalsResetRequests{0};
  unsigned globalsResetsSeen = 0;  // consumer thread only
  bool handedOver = false;         // producer thread only

  // Swaps in a fresh compile once the previous one was reclaimed. The globals'
  // running values are carried here, on the thread that writes them.
  bool valid()
  {
    if (noLongerInUse.load() == NULL)
      if (cCodeSynthReadonly* next = update.exchange(NULL))
      {
        if (current) next->globals.TakeValuesFrom(current->globals);
        noLongerInUse.store(current); // handed back to the UI to free
        current = next;
      }
    unsigned resets = globalsResetRequests.load();
    if (resets != globalsResetsSeen)
    {
      globalsResetsSeen = resets;
      if (current) current->globals.RestoreInitValues();
    }
    return current != NULL;
  }

  // A pending compile the consumer has not picked up is replaced: the consumer
  // only takes ownership via update.exchange(NULL), so what exchange returns
  // here was never seen and is safe to delete.
  void compile(const std::string& content, const std::string& assetDir, bool forVisual,
               char* debugMsg, int debugMsgMax, RenderCtx* screen,
               const CodeSynthProbes* probes = nullptr)
  {
    delete noLongerInUse.exchange(NULL);
    cCodeSynthReadonly* f = new cCodeSynthReadonly();
    if (!CodeSynthParse(content.c_str(), assetDir.c_str(), f, debugMsgMax, debugMsg, screen, forVisual, probes))
    {
      delete f;
      return;
    }
    // Mid-edit globals would drop every running value, so the last good program
    // keeps playing.
    if (!f->globals.valid && handedOver)
    {
      delete f;
      if (debugMsg && debugMsgMax > 0)
      {
        std::string msg = std::string(debugMsg) + "codesynth: globals do not compile -- still running the previous version\n";
        snprintf(debugMsg, (size_t)debugMsgMax, "%s", msg.c_str());
      }
      return;
    }
    delete update.exchange(f);
    handedOver = true;
  }

  // Only after valid().
  void midi(int status, int note, int velocity)
  {
    int type = status & 0xF0;
    int channel = status & 0x0F;
    if (type == 0x90 && velocity > 0)
      noteOn(channel, note, velocity);
    else if (type == 0x80 || (type == 0x90 && velocity == 0))
      noteOff(channel, note);
  }

  void noteOn(int channel, int note, int velocity)
  {
    int midiIndex = channel * 128 + note;
    if (midiIndex < 0 || midiIndex >= 16 * 128 || current->map[midiIndex] == 0)
      return;
    int synthId = current->map[midiIndex] - 1;

    // A free voice, else the oldest.
    int idx = 0;
    for (int i = 0; i < kCodeSynthMaxVoices; i++)
    {
      if (voice[i].gain == 0) { idx = i; break; }
      if (voice[i].timeStampTriggered < voice[idx].timeStampTriggered) idx = i;
    }

    cCodeSynthLiveVoice& v = voice[idx];
    v.synthId = synthId;
    v.midiNoteId = note;
    v.gain = velocity / 127.0f;
    v.velocity = velocity;
    v.timeStampTriggered = time;
    v.freqMul = (float)CodeSynthSemitoneRatio(note + current->synthTranspose[synthId] - 69);
  }

  void noteOff(int channel, int note)
  {
    int midiIndex = channel * 128 + note;
    if (midiIndex < 0 || midiIndex >= 16 * 128)
      return;
    for (cCodeSynthLiveVoice& v : voice)
      if (v.midiNoteId == note && current->map[midiIndex] - 1 == v.synthId && v.gain > 0)
        v.gain = 0;
  }

  // The playing voice's slot, or null.
  const cCodeSynthVoice* slot(const cCodeSynthLiveVoice& v) const
  {
    if (v.gain <= 0 || v.synthId < 0 || v.synthId >= 128) return nullptr;
    return &current->synth[v.synthId];
  }
};
} // namespace

class cCodeSynth : public cTDAudioPluginBase
{
public:
  cCodeSynthPipeline fAudio;
  // A UI-thread-only mirror of fAudio.
  cCodeSynthPipeline fVisual;
  std::chrono::steady_clock::time_point fVisualLastTick;
  bool fVisualLastTickValid = false;

  float fVisualDelayMs = 0.f;
  struct VisualDelayEntry {
    std::chrono::steady_clock::time_point arrived;
    int status; int data1; int data2;
  };
  std::vector<VisualDelayEntry> fVisualDelayQueue;

  // The wrap arguments the open entry last ran with, for the editors' live and
  // hover runs. From the visual pipeline, which runs on the UI thread in step with
  // the audio.
  double fLastVoiceArgs[kCodeSynthWrapParamCount];
  bool   fLastVoiceArgsValid = false;
  // First capture wins, so a playing voice beats the idle preview.
  bool   fVoiceArgsCapturedThisTick = false;

  // Idle preview: the open entry, rendered while no visual voice plays. Resolved
  // through the current map each frame, since a recompile renumbers slots. -1 = none.
  int fVisualPreviewChannel = -1;
  int fVisualPreviewNote = -1;
  // Re-armed only when the entry changes, so edits don't restart the animation.
  int fVisualPreviewStartTime = 0;

  // The synth slots whose visual body ran in the last tick, indexed like synth[].
  bool fVisualRendered[128] = {};

  // This instance's screen and its storage. Members, not the vscreen_* globals,
  // which belong to an exported song. Never reallocated: the framebuffer must
  // outlive every VM that drew into it.
  RenderCtx     fRenderCtx;
  unsigned char fScreenPixels[VSCREEN_MAX_W * VSCREEN_MAX_H];
  int           fPalette[VSCREEN_PALETTE_LEN];
  RLayout       fLayout;
  char          fGlyphScratch[VSCREEN_GLYPH_SCRATCH_BYTES];
  char          fImageArena[VSCREEN_IMAGE_ARENA_BYTES];
  RenderImage   fImages[VSCREEN_MAX_IMAGES];
  RSurface      fTargetStack[VSCREEN_TARGET_STACK_MAX];
  int           fTargetFlags[VSCREEN_TARGET_STACK_MAX];
  VScreenFontCache fFontCache;
  char             fFontCacheMem[VSCREEN_FONT_CACHE_BYTES];

  CodeSynthVmCtx fVisualVmCtx;

  // Inspection, UI thread: the visual editor's probes, compiled into the visual
  // pipeline; where its readings go; the content last compiled, for a probe
  // change to recompile the visual half alone; and the pipeline the first-run
  // report was last made for.
  CodeSynthProbes fVisualProbes;
  CodeSynthInspectListener fInspect;
  std::string fLastContent;
  const cCodeSynthReadonly* fReportedPipeline = nullptr;
  bool fInspecting = false;
  bool fReporting = false;
  bool fReportOk = true;
  std::string fReportPrint;

  void setInspectListener(const CodeSynthInspectListener& l) { fInspect = l; }

  void setVisualProbes(const CodeSynthProbes& probes)
  {
    fVisualProbes = probes;
    if (fLastContent.empty()) return;
    char msg[1024] = {0};
    fVisual.compile(fLastContent, fAssetDir, true, msg, 1023, &fRenderCtx, &fVisualProbes);
  }

  // The editor's text moved with no change to the spans: they still hold
  // against the new text, which the next compile finds the body in.
  void setVisualProbeBody(const std::string& body) { fVisualProbes.editorBody = body; }

  static void inspectSink(void* user, int id, int kind, int shift, double v)
  {
    cCodeSynth* cs = static_cast<cCodeSynth*>(user);
    if (id >= 0 && cs->fInspecting && cs->fInspect.value) cs->fInspect.value(cs->fInspect.user, id, kind, shift, v);
  }
  static void inspectArrSink(void* user, int id, int kind, int shift, int total, const double* vals, int n)
  {
    cCodeSynth* cs = static_cast<cCodeSynth*>(user);
    if (id >= 0 && cs->fInspecting && cs->fInspect.array) cs->fInspect.array(cs->fInspect.user, id, kind, shift, total, vals, n);
  }
  static void reportPrint(void* user, const unsigned char* s, int n)
  {
    cCodeSynth* cs = static_cast<cCodeSynth*>(user);
    if (cs->fReportPrint.size() + (size_t)n + 1 > 65536) return;
    cs->fReportPrint.append(reinterpret_cast<const char*>(s), (size_t)n);
    cs->fReportPrint.push_back('\n');
  }

  virtual void handleMidi(int frameOffset, int status, int data1, int data2) override
  {
    if (fAudio.valid())
      fAudio.midi(status, data1, data2);
  }

  // UI thread, not sample-accurate.
  void handleVisualMidi(int status, int data1, int data2)
  {
    if (!fVisual.valid())
      return;
    if (fVisualDelayMs <= 0.f)
      fVisual.midi(status, data1, data2);
    else
      fVisualDelayQueue.push_back({ std::chrono::steady_clock::now(), status, data1, data2 });
  }

  // For a force-stopped playback, whose note-offs never arrive.
  void clearVisualVoices()
  {
    for (cCodeSynthLiveVoice& v : fVisual.voice)
      v.gain = 0;
    fVisualDelayQueue.clear();
  }

  void setVisualDelayMs(float ms) { fVisualDelayMs = ms < 0.f ? 0.f : (ms > 300.f ? 300.f : ms); }
  float visualDelayMs() const { return fVisualDelayMs; }

  // Called every frame; re-arms t only when the target changes.
  void setVisualPreviewNote(int midiChannel, int midiNote)
  {
    if (midiChannel == fVisualPreviewChannel && midiNote == fVisualPreviewNote)
      return;
    fVisualPreviewChannel = midiChannel;
    fVisualPreviewNote = midiNote;
    fVisualPreviewStartTime = fVisual.time;
  }

  // UI thread: advances visual time by the wall clock (in samples) and runs each
  // active visual voice once.
  void tickVisual()
  {
    auto now = std::chrono::steady_clock::now();
    memset(fVisualRendered, 0, sizeof(fVisualRendered));

    bool haveSynth = fVisual.valid();
    if (fVisualDelayMs > 0.f && !fVisualDelayQueue.empty())
    {
      std::chrono::duration<double, std::milli> delay(fVisualDelayMs);
      size_t keepFrom = 0;
      while (keepFrom < fVisualDelayQueue.size() && (now - fVisualDelayQueue[keepFrom].arrived) >= delay)
        keepFrom++;
      if (haveSynth)
      {
        for (size_t i = 0; i < keepFrom; i++)
          fVisual.midi(fVisualDelayQueue[i].status, fVisualDelayQueue[i].data1, fVisualDelayQueue[i].data2);
        fVisualDelayQueue.erase(fVisualDelayQueue.begin(), fVisualDelayQueue.begin() + keepFrom);
      }
    }

    if (!haveSynth)
    {
      // No synth yet: keep the queue, capped.
      if (fVisualDelayQueue.size() > 4096)
        fVisualDelayQueue.erase(fVisualDelayQueue.begin(), fVisualDelayQueue.begin() + (fVisualDelayQueue.size() - 4096));
      return;
    }

    double dtSeconds = fVisualLastTickValid
        ? std::chrono::duration<double>(now - fVisualLastTick).count() : 0.0;
    fVisualLastTick = now;
    fVisualLastTickValid = true;
    fVisual.time += (int)(dtSeconds * fSR);

    // Lag beat by the same delay as the MIDI.
    double beat = fSongBeatPos;
    if (fVisualDelayMs > 0.f)
      beat -= (double)fVisualDelayMs * 0.001 * fSongBeatPerSample * fSR;

    // Lower order draws first (behind); voice index breaks ties.
    struct Active { int voiceIdx; int order; } sorted[kCodeSynthMaxVoices];
    int numActive = 0;
    for (int v = 0; v < kCodeSynthMaxVoices; v++)
      if (fVisual.slot(fVisual.voice[v]))
        sorted[numActive++] = { v, fVisual.current->synthOrder[fVisual.voice[v].synthId] };
    std::sort(sorted, sorted + numActive, [](const Active& a, const Active& b) {
      return a.order != b.order ? a.order < b.order : a.voiceIdx < b.voiceIdx;
    });

    fVoiceArgsCapturedThisTick = false;
    // One inspection pass per tick, from the pipeline compiled with the probes.
    fInspecting = fInspect.begin && !fVisualProbes.probes.empty() &&
                  fVisual.current->probeSerial != 0 && fVisual.current->probeSerial == fVisualProbes.serial;
    if (fInspecting) fInspect.begin(fInspect.user, fVisual.current->probeSerial);
    // A new pipeline reports how the open entry's first run went.
    fReporting = fInspect.ran && fReportedPipeline != fVisual.current;
    fReportOk = true;
    fReportPrint.clear();
    bool reported = false;

    int rendered = 0;
    for (int i = 0; i < numActive; i++)
    {
      const cCodeSynthLiveVoice& v = fVisual.voice[sorted[i].voiceIdx];
      if (runVisualVoice(v.synthId, v.midiNoteId, v.freqMul, v.timeStampTriggered, beat, v.velocity, &reported))
        rendered++;
    }

    if (rendered == 0)
      runVisualPreview(beat, &reported);
    else if (!fVoiceArgsCapturedThisTick)
      captureVisualPreviewArgs(beat);

    if (fInspecting) fInspect.end(fInspect.user);
    fInspecting = false;
    if (reported)
    {
      fReportedPipeline = fVisual.current;
      fInspect.ran(fInspect.user, fReportOk, fReportPrint.data(), (int)fReportPrint.size());
    }
    fReporting = false;

    // Nothing spoke for the open entry: drop the reading rather than show another
    // entry's.
    if (!fVoiceArgsCapturedThisTick)
      fLastVoiceArgsValid = false;
  }

  // -1 when nothing is armed or the note maps to no entry.
  int visualPreviewSynthId(float* outFreqMul) const
  {
    if (fVisualPreviewChannel < 0 || fVisualPreviewChannel > 15 ||
        fVisualPreviewNote < 0 || fVisualPreviewNote > 127)
      return -1;
    unsigned char slot = fVisual.current->map[fVisualPreviewChannel * 128 + fVisualPreviewNote];
    if (slot == 0)
      return -1;
    int synthId = slot - 1;
    if (outFreqMul)
      *outFreqMul = (float)CodeSynthSemitoneRatio(fVisualPreviewNote + fVisual.current->synthTranspose[synthId] - 69);
    return synthId;
  }

  void voiceArgs(int synthId, int midiNote, float freqMul, int startTime, double beat,
                 double vel, double* out) const
  {
    double hz = 440.0 * (double)freqMul;
    double t = (double)(fVisual.time - startTime) / fSR;
    out[0] = t;
    out[1] = hz;
    out[2] = t * hz;                                    // phase
    out[3] = (double)midiNote;
    out[4] = beat;
    out[5] = CodeSynthSemitoneRatio(midiNote - fVisual.current->synthRootNote[synthId]);
    out[6] = vel;
    out[7] = kPanCentre;
  }

  void captureLastVoiceArgs(const double* args)
  {
    memcpy(fLastVoiceArgs, args, sizeof(fLastVoiceArgs));
    fLastVoiceArgsValid = true;
    fVoiceArgsCapturedThisTick = true;
  }

  void captureVisualPreviewArgs(double beat)
  {
    float freqMul = 1.0f;
    int synthId = visualPreviewSynthId(&freqMul);
    if (synthId < 0) return;
    double args[kCodeSynthWrapParamCount];
    voiceArgs(synthId, fVisualPreviewNote, freqMul, fVisualPreviewStartTime, beat, 127.0, args);
    captureLastVoiceArgs(args);
  }

  // Returns whether a body ran. `vel` is raw MIDI; the idle preview passes 127.
  bool runVisualVoice(int synthId, int midiNote, float freqMul, int startTime, double beat, double vel,
                      bool* reported = nullptr)
  {
    const cCodeSynthVoice& voice = fVisual.current->synth[synthId];

    // Visuals run once per frame, at centre.
    double args[kCodeSynthWrapParamCount];
    voiceArgs(synthId, midiNote, freqMul, startTime, beat, vel, args);

    // Before the body check: a sound without a visual half still wants its
    // arguments shown.
    if (!fVoiceArgsCapturedThisTick && synthId == visualPreviewSynthId(NULL))
      captureLastVoiceArgs(args);

    Func* fn = voice.fn;
    if (fn == NULL)
      return false;

    unsigned char frame[kCodeSynthVoiceFrameBytes];
    if (func_frame_size(fn) > sizeof(frame))
      return false;
    Args a;
    args_bind(&a, fn, frame, sizeof(frame));
    CodeSynthSetArgs(&a, voice.type, args);
    // Re-set per run: a recompile builds a new VM.
    fVisualVmCtx.smp    = nullptr;
    fVisualVmCtx.screen = &fRenderCtx;
    vm_set_user_data(voice.vm, &fVisualVmCtx);
    CodeSynthBindScreenBuffers(voice.vm, &fRenderCtx, true);

    // Frame-temporary images and an empty target stack per body, as the exported
    // song does.
    render_ctx_images_reset(&fRenderCtx);
    // Every visual VM may carry an inspect(); only the open entry's has probes.
    vm_set_inspect_sink(voice.vm, fInspecting ? inspectSink : nullptr, this);
    vm_set_inspect_array_sink(voice.vm, fInspecting ? inspectArrSink : nullptr, this);
    bool report = fReporting && reported && !*reported && synthId == visualPreviewSynthId(NULL);
    vm_set_print_sink(voice.vm, report ? reportPrint : nullptr, this);
    VMStatus st = func_run(fn, &a, kCodeSynthVisualOpBudget);
    if (report) { *reported = true; fReportOk = st == VM_OK; vm_set_print_sink(voice.vm, nullptr, nullptr); }
    // `screen[i] = c` bypasses the primitives' generation bump.
    vscreen_touch_ctx(&fRenderCtx);
    fVisualRendered[synthId] = true;
    return true;
  }

  // Whether the body (channel, note) plays had its visual half run in the last tick.
  bool visualNoteRendered(int midiChannel, int midiNote) const
  {
    if (!fVisual.current || midiChannel < 0 || midiChannel > 15 || midiNote < 0 || midiNote > 127)
      return false;
    unsigned char slot = fVisual.current->map[midiChannel * 128 + midiNote];
    return slot != 0 && fVisualRendered[slot - 1];
  }

  void runVisualPreview(double beat, bool* reported = nullptr)
  {
    float freqMul = 1.0f;
    int synthId = visualPreviewSynthId(&freqMul);
    if (synthId < 0)
      return;
    // No clear for a body without a visual half; it would blank the last frame.
    if (fVisual.current->synth[synthId].fn != NULL)
      vscreen_clear_ctx(&fRenderCtx, 0);
    runVisualVoice(synthId, fVisualPreviewNote, freqMul, fVisualPreviewStartTime, beat, 127.0, reported);
  }

  virtual void handleSamples(float** inputs, float** outputs, int sampleFrames) override
  {
    float* outL = outputs[0];
    float* outR = outputs[1];

    for (int i = 0; i < sampleFrames; i++)
    {
      outL[i] = 0.0f;
      outR[i] = 0.0f;
    }

    if (!fAudio.valid())
      return;

    for (cCodeSynthLiveVoice& lv : fAudio.voice)
    {
      const cCodeSynthVoice* voice = fAudio.slot(lv);
      if (!voice || !voice->fn)
        continue;
      Func* fn = voice->fn;

      float gain = lv.gain;
      double hz = 440.0 * (double)lv.freqMul;
      double rate = CodeSynthSemitoneRatio(lv.midiNoteId - fAudio.current->synthRootNote[lv.synthId]);
      int timestamp = lv.timeStampTriggered;

      unsigned char frame[kCodeSynthVoiceFrameBytes];
      if (func_frame_size(fn) > sizeof(frame))
        continue;
      Args a;
      args_bind(&a, fn, frame, sizeof(frame));

      // The VM keeps this ctx pointer. Audio VMs get no screen.
      smp_bind_voice(&lv.smpState, voice->sample, fSR);
      lv.vmCtx.smp    = &lv.smpState;
      lv.vmCtx.screen = nullptr;
      vm_set_user_data(voice->vm, &lv.vmCtx);
      // Unbound: a sound body touching `screen` fails its run instead of writing from
      // the audio thread.
      CodeSynthBindScreenBuffers(voice->vm, &fRenderCtx, false);

      for (int i = 0; i < sampleFrames; i++)
      {
        double t = (double)(fAudio.time + i - timestamp) / fSR;
        lv.smpState.note_sample = fAudio.time + i - timestamp;

        // Mono voices pass centre too, matching the exporter and the preview. All
        // arguments again for the right channel: a body may assign to them.
        double args[kCodeSynthWrapParamCount] = {
          t, hz, t * hz, (double)lv.midiNoteId, fSongBeatPos + (double)i * fSongBeatPerSample,
          rate, (double)lv.velocity, voice->stereo ? kPanLeft : kPanCentre };
        CodeSynthSetArgs(&a, voice->type, args);
        float left = CodeSynthRunToFloat(fn, &a, voice->retType);

        float right = left;
        if (voice->stereo)
        {
          args[kCodeSynthPanParamIndex] = kPanRight;
          CodeSynthSetArgs(&a, voice->type, args);
          right = CodeSynthRunToFloat(fn, &a, voice->retType);
        }

        outL[i] += left  * gain;
        outR[i] += right * gain;
      }
    }

    fAudio.time += sampleFrames;
  }

  cCodeSynth()
  {
    // Cleared: the UI can upload before the first body runs.
    vscreen_ctx_provision(&fRenderCtx,
                          fScreenPixels, VSCREEN_W, VSCREEN_H,
                          fPalette, VSCREEN_PALETTE_LEN,
                          &fLayout,
                          fGlyphScratch, VSCREEN_GLYPH_SCRATCH_BYTES,
                          fImageArena, VSCREEN_IMAGE_ARENA_BYTES,
                          fImages, VSCREEN_MAX_IMAGES,
                          fTargetStack, fTargetFlags, VSCREEN_TARGET_STACK_MAX,
                          &fFontCache, fFontCacheMem, VSCREEN_FONT_CACHE_BYTES);
    vscreen_clear_ctx(&fRenderCtx, 0);
  }

  char fLastDebugMsg[1024] = {0};
  const char* GetLastDebugMessage() const { return fLastDebugMsg; }

  // With a trailing separator.
  std::string fAssetDir;
  void setAssetDir(const std::string& dir) { fAssetDir = dir; }

  // Applied by each pipeline on its own thread, at its next valid().
  void requestGlobalsReset()
  {
    fAudio.globalsResetRequests.fetch_add(1);
    fVisual.globalsResetRequests.fetch_add(1);
  }

  // Anything outside this class wanting the screen must come through here, never
  // vscreen_ctx().
  RenderCtx* screenCtx() { return &fRenderCtx; }

  bool lastVoiceArgs(double* out) const
  {
    if (!fLastVoiceArgsValid || !out) return false;
    memcpy(out, fLastVoiceArgs, sizeof(fLastVoiceArgs));
    return true;
  }

  void LoadContent(const std::string& content)
  {
    char debugMsg[1024] = {0};
    char visualDebugMsg[1024] = {0};
    // A body that dropped its color_ramp_setup gets the default back.
    render_ctx_color_ramp_setup_i32(&fRenderCtx, 192, 0, 2, 2, 2);
    fLastContent = content;
    fAudio.compile(content, fAssetDir, false, debugMsg, 1023, &fRenderCtx);
    fVisual.compile(content, fAssetDir, true, visualDebugMsg, 1023, &fRenderCtx, &fVisualProbes);
    snprintf(fLastDebugMsg, sizeof(fLastDebugMsg), "%s%s", debugMsg, visualDebugMsg);
  }
};

cTDAudioPluginBase* CreatePluginInstance() {
  return new cCodeSynth();
}
