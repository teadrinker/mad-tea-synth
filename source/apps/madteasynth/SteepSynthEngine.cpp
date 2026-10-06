// Bridges the DSP backends and iPlug2, with tdPluginVST.h's forced-block-size
// buffering.

#include "SteepSynthEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "codesynth/SongExport.h"
#include "codesynth/Utf8File.h"

// cTDAudioPluginBase, shared by both backends.
#include "tdPlugin.h"

#if STEEPSYNTH_ENABLE_INDEXYNT
#include "indexynt/NuXThreads.h"
#include "indexynt/INDEXYNT.cpp"
#endif

// Unity-built: CodeSynth.cpp relies on the includes above.
#include "codesynth/CodeSynth.cpp"

// tinywav's fopen() still takes an ANSI path, so a non-ASCII sample path fails.
namespace fs = std::filesystem;

#if STEEPSYNTH_ENABLE_INDEXYNT
#define kNumParams eINDEXYNTParam_count
#else
// cCodeSynth never reads fParam[]; only iPlug2's parameter list has to fit, with
// headroom for a stale host index.
#define kNumParams 32
#endif

enum { kMaxChans = 2 };

SteepSynthEngine::SteepSynthEngine()
  : mParams(nullptr)
  , mBatchPos(0)
  , mMidiCount(0)
  , mMidiReadPos(0)
  , mMidiVisualReadPos(0)
  , mHostSamplePos(0)
  , mEngineSamplePos(0)
  , mSampleRate(44100.0)
  , mBpm(120.0)
  , mSongPosSamples(0.0)
  // -1 is the standalone's "no transport", so start from a value no host reports.
  , mPrevSongPosSamples(-2.0)
  , mFreeBeatPos(0.0)
  , mRecording(false)
  , mRecordStartSamplePos(0)
  , mMatrixCols(0)
  , mMatrixSamplesPerStep(0.0)
  , mPlaybackReadPos(0)
  , mPlaybackVisualReadPos(0)
  , mPlaybackActive(false)
  , mPlaybackOriginSamplePos(0)
{
  mParams = new float[kNumParams];
  mParamChanged = new bool[kNumParams];
  for (int i = 0; i < kNumParams; i++) {
    mParams[i] = 0.5f;
    mParamChanged[i] = true;
  }

  for (int c = 0; c < kMaxChans; c++) {
    mBatchIn[c].resize(kBlockSize);
    mBatchOut[c].resize(kBlockSize);
  }
}

SteepSynthEngine::~SteepSynthEngine() {
#if STEEPSYNTH_ENABLE_INDEXYNT
  delete mIndexynt;
#endif
  delete mCodeSynth;
  delete[] mParams;
  delete[] mParamChanged;
}

void SteepSynthEngine::Init() {
#if STEEPSYNTH_ENABLE_INDEXYNT
  delete mIndexynt; mIndexynt = nullptr;
#endif
  delete mCodeSynth; mCodeSynth = nullptr;

#if STEEPSYNTH_ENABLE_INDEXYNT
  cINDEXYNT* eng = new cINDEXYNT();
  eng->fParam  = mParams;
  eng->fParamChanged = mParamChanged;
  eng->fSR     = 44100.0;
  eng->fOwner  = nullptr;
  eng->init();
  mIndexynt = eng;
#endif

  cCodeSynth* cs = new cCodeSynth();
  cs->fParam  = mParams;
  cs->fParamChanged = mParamChanged;
  cs->fSR     = 44100.0;
  cs->fOwner  = nullptr;
  cs->init();
  mCodeSynth = cs;

  mEntries.clear();
  cs->setAssetDir(AssetDir());

  CodeSynthEntry example;
  example.name = "Example";
  example.channel = 1;
  example.note = 36;
  example.type = kCodeSynthFix;
  example.body = kCodeSynthDefaultSoundBody;
  example.visualBody = kCodeSynthDefaultVisualBody;
  mEntries.push_back(example);
  PushCodeSynthLiveEdit();
  mWatchValid = false;

  mBatchPos = 0;
  mMidiCount = 0;
  mMidiReadPos = 0;
  mMidiVisualReadPos = 0;
  mHostSamplePos = 0;
  mEngineSamplePos = 0;

  mRecording = false;
  mRecordStartSamplePos = 0;
  mRecordedNotes.clear();
  mMatrixRows.clear();
  mMatrixCols = 0;
  mMatrixData.clear();
  mMatrixSamplesPerStep = 0.0;
  mPlaybackEvents.clear();
  mPlaybackReadPos = 0;
  mPlaybackVisualReadPos = 0;
  mPlaybackActive = false;
}

cTDAudioPluginBase* SteepSynthEngine::ActiveEngine() const {
#if STEEPSYNTH_ENABLE_INDEXYNT
  if (mBackend == SynthBackend::Indexynt) return mIndexynt;
#endif
  return mCodeSynth; // SetBackend() refuses Indexynt when it isn't built
}

void SteepSynthEngine::SetSampleRate(double sr) {
  if (sr > 0.0) mSampleRate = sr;
#if STEEPSYNTH_ENABLE_INDEXYNT
  if (mIndexynt) {
    mIndexynt->fSR = sr;
    mIndexynt->sampleRateChanged();
  }
#endif
  if (mCodeSynth) {
    mCodeSynth->fSR = sr;
    mCodeSynth->sampleRateChanged();
  }
}

void SteepSynthEngine::SetParam(int idx, float value) {
  if (idx >= 0 && idx < kNumParams) mParams[idx] = value;
}
float SteepSynthEngine::GetParam(int idx) const {
  return (idx >= 0 && idx < kNumParams) ? mParams[idx] : 0.f;
}
int SteepSynthEngine::ParamCount() const { return kNumParams; }

void SteepSynthEngine::UiIdle() {
  if (!mCodeSynth) return;

  // Pulled whether or not it compiles, so a syntax error reaches the editor.
  if (mFileIsMaster) {
    std::string content;
    if (PollCodeFile(content))
      CodeSynthSetSourceText(content.data(), content.size());
  }

  // Also called from Draw(); this keeps visual MIDI draining with no editor open.
  TickVisualPipeline();
}

void SteepSynthEngine::TickVisualPipeline() {
  if (!mCodeSynth) return;
  DispatchVisualMidi();
  mCodeSynth->tickVisual();
}

RenderCtx* SteepSynthEngine::ScreenCtx() {
  return mCodeSynth ? mCodeSynth->screenCtx() : nullptr;
}

bool SteepSynthEngine::CodeSynthLastVoiceArgs(double* out) const {
  return mCodeSynth && mCodeSynth->lastVoiceArgs(out);
}

void SteepSynthEngine::SetCodeSynthVisualProbes(const CodeSynthProbes& probes) {
  if (mCodeSynth) mCodeSynth->setVisualProbes(probes);
}

void SteepSynthEngine::SetCodeSynthVisualProbeBody(const std::string& editorBody) {
  if (mCodeSynth) mCodeSynth->setVisualProbeBody(editorBody);
}

void SteepSynthEngine::SetCodeSynthInspectListener(const CodeSynthInspectListener& listener) {
  if (mCodeSynth) mCodeSynth->setInspectListener(listener);
}

const char* SteepSynthEngine::GetCodeSynthDebugMessage() const {
  return mCodeSynth ? mCodeSynth->GetLastDebugMessage() : "";
}

const CodeSynthEntry& SteepSynthEngine::Entry(int idx) const {
  static const CodeSynthEntry kNone;
  return (idx >= 0 && idx < EntryCount()) ? mEntries[(size_t)idx] : kNone;
}

void SteepSynthEngine::SetEntryName(int idx, const char* name) {
  if (!name || idx < 0 || idx >= (int)mEntries.size()) return;

  // A rename must not change what the entry is.
  if (CodeSynthEntryIsReserved(mEntries[idx])) return;
  if (std::string(name) == kCodeSynthCommonName) return;
  if (std::string(name) == kCodeSynthGlobalsName) return;

  // Header-breaking characters become '_'. Not trimmed: the name field compares
  // against this verbatim.
  std::string clean;
  for (const char* p = name; *p; p++)
    clean += (*p == ',' || *p == '\r' || *p == '\n' || *p == '\\') ? '_' : *p;

  if (mEntries[idx].name == clean) return;
  mEntries[idx].name = clean;
  PushCodeSynthLiveEdit();
}

int SteepSynthEngine::AddCodeSynthGlobalsEntry() {
  if (CodeSynthGlobalsIndex(mEntries) >= 0) return -1;

  CodeSynthEntry entry;
  entry.kind = kEntryGlobals;
  entry.name = kCodeSynthGlobalsName;
  entry.body = kCodeSynthDefaultGlobalsBody;
  entry.visualBody = kCodeSynthDefaultGlobalsVisualBody;

  // Where CodeSynthSplitEntries pins it, so a round-trip doesn't reshuffle the list.
  int at = CodeSynthCommonIndex(mEntries) >= 0 ? 1 : 0;
  mEntries.insert(mEntries.begin() + at, entry);
  PushCodeSynthLiveEdit();
  return at;
}

bool SteepSynthEngine::DeleteCodeSynthGlobalsEntry() {
  int idx = CodeSynthGlobalsIndex(mEntries);
  if (idx < 0) return false;
  mEntries.erase(mEntries.begin() + idx);
  PushCodeSynthLiveEdit();
  return true;
}

int SteepSynthEngine::AddCodeSynthCommonEntry() {
  if (CodeSynthCommonIndex(mEntries) >= 0) return -1;

  CodeSynthEntry entry;
  entry.kind = kEntryCommon;
  entry.name = kCodeSynthCommonName;
  entry.body = kCodeSynthDefaultCommonBody;

  // Where CodeSynthSplitEntries pins it.
  mEntries.insert(mEntries.begin(), entry);
  PushCodeSynthLiveEdit();
  return 0;
}

bool SteepSynthEngine::DeleteCodeSynthCommonEntry() {
  int idx = CodeSynthCommonIndex(mEntries);
  if (idx < 0) return false;
  mEntries.erase(mEntries.begin() + idx);
  PushCodeSynthLiveEdit();
  return true;
}

bool SteepSynthEngine::DeleteCodeSynthEntry(int idx) {
  if (idx < 0 || idx >= (int)mEntries.size()) return false;
  if (CodeSynthEntryIsReserved(mEntries[idx])) return false;
  mEntries.erase(mEntries.begin() + idx);
  PushCodeSynthLiveEdit();
  return true;
}

int SteepSynthEngine::AddCodeSynthEntry(bool withSound, bool withVisual) {

  const char* prefix = (withVisual && !withSound) ? "visual" : "sound";
  std::string name;
  for (size_t n = 1; n <= mEntries.size() + 1; n++) {
    name = prefix + std::to_string(n);
    bool taken = false;
    for (const CodeSynthEntry& e : mEntries)
      if (e.name == name) { taken = true; break; }
    if (!taken) break;
  }

  // The next note after the last entry, else the first free (channel, note): a
  // shared slot is shadowed in the note map.
  int channel = 1, note = 0;
  bool found = false;
  if (!mEntries.empty()) {
    const CodeSynthEntry& last = mEntries.back();
    int ch = last.channel, nn = last.note + 1;
    if (ch >= 1 && ch <= 16 && nn >= 0 && nn < 128) {
      bool taken = false;
      for (const CodeSynthEntry& e : mEntries)
        if (e.channel == ch && e.note == nn) { taken = true; break; }
      if (!taken) { channel = ch; note = nn; found = true; }
    }
  }
  for (int ch = 1; ch <= 16 && !found; ch++) {
    for (int nn = 0; nn < 128 && !found; nn++) {
      bool taken = false;
      for (const CodeSynthEntry& e : mEntries)
        if (e.channel == ch && e.note == nn) { taken = true; break; }
      if (!taken) { channel = ch; note = nn; found = true; }
    }
  }

  CodeSynthEntry entry;
  entry.name = name;
  entry.channel = channel;
  entry.note = note;
  // Fixed-point: what the exported song runs.
  entry.type = kCodeSynthFix;
  entry.transpose = 0;
  if (withSound)  entry.body = kCodeSynthDefaultSoundBody;
  if (withVisual) entry.visualBody = kCodeSynthDefaultVisualBody;

  mEntries.push_back(entry);
  PushCodeSynthLiveEdit();
  return (int)mEntries.size() - 1;
}

std::string SteepSynthEngine::CodeSynthGetSourceText() const {
  return CodeSynthSerializeEntries(mEntries);
}

// Every replacement of the code enters here: state restore, file pull, Load.
void SteepSynthEngine::CodeSynthSetSourceText(const char* text, size_t len) {
  if (!text) return;
  mEntries = CodeSynthSplitEntries(std::string(text, len));
  PushCodeSynthLiveEdit();
}

std::string SteepSynthEngine::AssetDir() const {
  size_t sep = mCodeFilePath.find_last_of("/\\");
  if (sep == std::string::npos) return std::string();
  return mCodeFilePath.substr(0, sep + 1); // keeps the trailing separator
}

// Forgets the watch, so a master file is re-read next tick.
void SteepSynthEngine::SetCodeFilePath(const char* path) {
  std::string next = path ? path : "";
  if (next == mCodeFilePath) return;
  mCodeFilePath = next;
  mWatchValid = false;
  // Relative WAV directives resolve against the new directory.
  if (mCodeSynth) {
    mCodeSynth->setAssetDir(AssetDir());
    PushCodeSynthLiveEdit();
  }
}

void SteepSynthEngine::SetFileIsMaster(bool master) {
  if (master == mFileIsMaster) return;
  mFileIsMaster = master;
  mWatchValid = false; // newly master -> next tick pulls
}

bool SteepSynthEngine::PollCodeFile(std::string& out) {
  if (mCodeFilePath.empty()) return false;

  std::error_code ec;
  fs::path p = PathFromUtf8(mCodeFilePath);
  auto stamp = fs::last_write_time(p, ec);
  if (ec) return false; // missing or unreadable -- keep what is in state

  long long now = (long long)stamp.time_since_epoch().count();
  if (mWatchValid && mWatchedPath == mCodeFilePath && mWatchedStamp == now)
    return false;

  mWatchedPath  = mCodeFilePath;
  mWatchedStamp = now;
  mWatchValid   = true;

  return ReadWholeFile(p, out);
}

bool SteepSynthEngine::LoadCodeFromFile() {
  if (mCodeFilePath.empty()) return false;

  std::string text;
  if (!ReadWholeFile(mCodeFilePath, text)) return false;

  CodeSynthSetSourceText(text.data(), text.size());
  // Another project must not inherit this one's values.
  ResetCodeSynthGlobals();
  // Mark the file seen, so the next master tick doesn't reread it.
  mWatchValid = false;
  std::string ignored;
  PollCodeFile(ignored);
  return true;
}

bool SteepSynthEngine::SaveCodeToFile() {
  if (mCodeFilePath.empty()) return false;
  std::string text = CodeSynthSerializeEntries(mEntries);

  bool ok = WriteWholeFile(mCodeFilePath, text);

  // Our own write must not read back as an external edit.
  if (ok) {
    mWatchValid = false;
    std::string ignored;
    PollCodeFile(ignored);
  }
  return ok;
}

void SteepSynthEngine::PushCodeSynthLiveEdit() {
  if (!mCodeSynth) return;
  std::string text = CodeSynthSerializeEntries(mEntries);
  mCodeSynth->LoadContent(text);
}

void SteepSynthEngine::ResetCodeSynthGlobals() {
  if (mCodeSynth) mCodeSynth->requestGlobalsReset();
}

void SteepSynthEngine::SetVisualDelayMs(float ms) {
  if (mCodeSynth) mCodeSynth->setVisualDelayMs(ms);
}

float SteepSynthEngine::GetVisualDelayMs() const {
  return mCodeSynth ? mCodeSynth->visualDelayMs() : 0.f;
}

void SteepSynthEngine::SetVisualPreviewEntry(int idx) {
  cCodeSynth* cs = mCodeSynth;
  if (!cs) return;

  // An entry without a visual body is still armed: its arguments feed the sound
  // editor's runs.
  if (idx < 0 || idx >= (int)mEntries.size()) {
    cs->setVisualPreviewNote(-1, -1);
    return;
  }

  // Entries store channels 1-based.
  cs->setVisualPreviewNote(mEntries[idx].channel - 1, mEntries[idx].note);
}

bool SteepSynthEngine::EntryVisuallyRendered(int idx) const {
  if (!mCodeSynth || idx < 0 || idx >= (int)mEntries.size()) return false;
  if (CodeSynthEntryIsReserved(mEntries[idx])) return false;
  return mCodeSynth->visualNoteRendered(mEntries[idx].channel - 1, mEntries[idx].note);
}

int SteepSynthEngine::ExportSongTick() const {
  // Truncated, as SongExportGenerateC does.
  if (mMatrixSamplesPerStep > 0.0) {
    const int tick = (int)mMatrixSamplesPerStep;
    return tick < 1 ? 1 : tick;
  }
  return ExportSongTickFromBpm(mBpm, ExportSampleRate());
}

SongSeq SteepSynthEngine::CurrentSeq() const {
  SongSeq seq;
  seq.rows = mMatrixRows;
  seq.cols = mMatrixCols;
  seq.matrix = mMatrixData;
  seq.samplesPerStep = mMatrixSamplesPerStep;
  return seq;
}

bool SteepSynthEngine::SaveCSong(SongChannelMode channelMode, SongSndLoopShape sndLoopShape) {
  const std::string dir = AssetDir();
  if (dir.empty()) return false;

  SongExportInput in;
  in.entries    = mEntries;
  in.seq        = CurrentSeq();
  in.sampleRate = mSampleRate > 0.0 ? mSampleRate : 44100.0;
  in.bpm        = mBpm;
  in.workingDir = dir;
  in.batchSize  = kBlockSize;
  // Screen size and channel mode follow the selected export target.
  in.screen      = ScreenCtx();
  in.channelMode = channelMode;
  in.sndLoopShape = sndLoopShape;

  std::string err;
  return SongExportWriteFile(in, dir + "song.c", err);
}

bool SteepSynthEngine::ExportProject(const ExportSettings& settings, std::string& msg,
                                     ExportProjectResult* result) {
  msg.clear();
  if (settings.destDir.empty()) { msg = "no export folder set"; return false; }

  // An input for resolving `// WAV` directives, not the destination.
  const std::string dir = AssetDir();
  if (dir.empty()) { msg = "set the code file path first (samples resolve against it)"; return false; }

  SongExportInput in;
  in.entries    = mEntries;
  in.seq        = CurrentSeq();
  in.sampleRate = mSampleRate > 0.0 ? mSampleRate : 44100.0;
  in.bpm        = mBpm;
  in.workingDir = dir;
  in.batchSize  = kBlockSize;
  in.screen     = ScreenCtx();

  ExportProjectResult res;
  std::string err;
  if (!ExportProjectWrite(settings, in, res, err)) { msg = err; return false; }
  if (result) *result = res;

  char buf[768];
  if (ExportTargetIsWav(settings.target))
  {
    // A failed .wav build is reported, not returned as an error: the source project
    // is complete.
    if (res.wavPath.empty())
      snprintf(buf, sizeof(buf), "source -> %s, but NO WAV: %s",
               res.projectDir.c_str(), res.wavError.c_str());
    else
      snprintf(buf, sizeof(buf), "%s (%s) -> %s",
               res.wavPath.c_str(),
               res.outputChannels == 2 ? "stereo" : "mono",
               settings.wavRemoveSource ? "source removed" : "source kept alongside");
  }
  else
  {
    snprintf(buf, sizeof(buf), "%d files -> %s (uuid %s, song %ld KB, %s)",
             res.filesWritten, res.projectDir.c_str(),
             res.setUuid ? "as set" : res.reusedUuid ? "reused" : "new",
             res.songBytes / 1024,
             res.outputChannels == 2 ? "stereo" : "mono");
  }
  msg = buf;
  return true;
}

bool SteepSynthEngine::SerializeSeq(std::string& out) const {
  return SongSeqEncode(CurrentSeq(), out);
}

void SteepSynthEngine::AdoptSeq(SongSeq&& seq) {
  StopPlayback();
  mRecording = false;
  mRecordedNotes.clear();

  mMatrixRows = std::move(seq.rows);
  mMatrixCols = seq.cols;
  mMatrixData = std::move(seq.matrix);
  mMatrixSamplesPerStep = seq.samplesPerStep;
}

bool SteepSynthEngine::DeserializeSeq(const char* data, size_t len) {
  SongSeq seq;
  if (!SongSeqDecode(data, len, seq)) return false;
  AdoptSeq(std::move(seq));
  return true;
}

bool SteepSynthEngine::SaveSeq() {
  const std::string dir = AssetDir();
  if (dir.empty()) return false;
  return SongSeqWriteFile(CurrentSeq(), dir + "codesynth.seq");
}

bool SteepSynthEngine::LoadSeq() {
  SongSeq seq;
  const std::string dir = AssetDir();
  if (dir.empty()) return false;
  if (!SongSeqReadFile(dir + "codesynth.seq", seq)) return false;
  AdoptSeq(std::move(seq));
  return true;
}

// Engines take fixed kBlockSize blocks, so host samples are batched. Both
// backends are synths, so input is zeros.
void SteepSynthEngine::ProcessBlock(float** outputs, int nFrames) {
  cTDAudioPluginBase* eng = ActiveEngine();
  if (!eng) { mHostSamplePos += nFrames; return; }

  // `b` (beats) has two clocks. During sequence playback it counts from the
  // sequence's step 0, as the exported renderer does. Otherwise it follows the host
  // transport while that advances and free-runs from there when it stops (the
  // standalone has no transport), so modulation never freezes.
  const double beatsPerSample = (mBpm > 0.0 && mSampleRate > 0.0) ? mBpm / 60.0 / mSampleRate : 0.0;

  // Wraps: fix sounds get b as fx22, which overflows past 512 beats.
  const double kFreeRunWrapBeats = 360.0;

  // The same position twice means stopped; negative means no transport.
  const bool hostAdvancing = (mSongPosSamples >= 0.0 && mSongPosSamples != mPrevSongPosSamples);
  if (hostAdvancing)
    mFreeBeatPos = mSongPosSamples * beatsPerSample;
  else if (mFreeBeatPos >= kFreeRunWrapBeats)
    mFreeBeatPos = fmod(mFreeBeatPos, kFreeRunWrapBeats);

  // Captured before the clocks advance.
  const double blockStartBeats = mPlaybackActive
    ? (double)(mHostSamplePos - mPlaybackOriginSamplePos) * beatsPerSample
    : mFreeBeatPos;
  mPrevSongPosSamples = mSongPosSamples;
  mFreeBeatPos += (double)nFrames * beatsPerSample;

  auto setSongBeatPos = [&]() {
    eng->fSongBeatPos = blockStartBeats + (double)(mEngineSamplePos - mHostSamplePos) * beatsPerSample;
    eng->fSongBeatPerSample = beatsPerSample;
  };

  int pos = 0;

  // 1. Drain the batch tail computed by a previous internal block.
  if (mBatchPos > 0 && mBatchPos < kBlockSize) {
    int avail = kBlockSize - mBatchPos;
    int copy  = (std::min)(avail, nFrames);
    for (int c = 0; c < kMaxChans; c++)
      for (int i = 0; i < copy; i++)
        outputs[c][pos + i] = mBatchOut[c][mBatchPos + i];
    mBatchPos += copy;
    pos += copy;
    if (mBatchPos < kBlockSize) { mHostSamplePos += nFrames; return; }
    mBatchPos = 0;
  }

  // 2. Full blocks
  while (pos + kBlockSize <= nFrames) {
    float* in[2]  = { mBatchIn[0].data(), mBatchIn[1].data() };
    float* out[2] = { mBatchOut[0].data(), mBatchOut[1].data() };

    for (int c = 0; c < kMaxChans; c++) {
      std::fill(mBatchIn[c].begin(), mBatchIn[c].end(), 0.f);
      std::fill(mBatchOut[c].begin(), mBatchOut[c].end(), 0.f);
    }

    DispatchMidiUpTo(mEngineSamplePos + kBlockSize);
    DispatchPlaybackUpTo(mEngineSamplePos + kBlockSize);
    setSongBeatPos();
    eng->handleSamples(in, out, kBlockSize);
    mEngineSamplePos += kBlockSize;

    for (int c = 0; c < kMaxChans; c++)
      for (int i = 0; i < kBlockSize; i++)
        outputs[c][pos + i] = mBatchOut[c][i];

    pos += kBlockSize;
  }

  // 3. Partial block
  int remaining = nFrames - pos;
  if (remaining > 0) {
    float* in2[2]  = { mBatchIn[0].data(), mBatchIn[1].data() };
    float* out2[2] = { mBatchOut[0].data(), mBatchOut[1].data() };

    for (int c = 0; c < kMaxChans; c++) {
      std::fill(mBatchIn[c].begin(), mBatchIn[c].end(), 0.f);
      std::fill(mBatchOut[c].begin(), mBatchOut[c].end(), 0.f);
    }

    // No DispatchPlaybackUpTo here: a sequencer event in this tail waits for the
    // next ProcessBlock, up to kBlockSize samples late.
    DispatchMidiUpTo(mEngineSamplePos + kBlockSize);
    setSongBeatPos();
    eng->handleSamples(in2, out2, kBlockSize);
    mEngineSamplePos += kBlockSize;

    for (int c = 0; c < kMaxChans; c++)
      for (int i = 0; i < remaining; i++)
        outputs[c][pos + i] = mBatchOut[c][i];

    mBatchPos = remaining;
  }

  mHostSamplePos += nFrames;
}

// Buffered; frameOffset becomes absolute via mHostSamplePos.
void SteepSynthEngine::HandleMidi(int frameOffset, int status,
                                   int data1, int data2) {
  // Compact only once both readers have drained.
  if (mMidiReadPos == mMidiCount && mMidiVisualReadPos == mMidiCount) {
    mMidiReadPos = 0;
    mMidiVisualReadPos = 0;
    mMidiCount = 0;
  }
  if (mMidiCount >= kMidiBufferMax) return; // drop if flooded

  MidiEvent& ev = mMidiBuffer[mMidiCount++];
  ev.pos    = mHostSamplePos + frameOffset;
  ev.status = status;
  ev.data1  = data1;
  ev.data2  = data2;

  // A velocity-0 note-on is a note-off.
  if (mRecording && (status & 0xf0) == 0x90 && data2 != 0) {
    RecordedNote rn;
    rn.samplePos    = ev.pos - mRecordStartSamplePos;
    rn.channel      = status & 0x0f;
    rn.note         = data1;
    rn.velocity     = data2;
    rn.offSamplePos = -1;
    mRecordedNotes.push_back(rn);
  } else if (mRecording &&
             ((status & 0xf0) == 0x80 || ((status & 0xf0) == 0x90 && data2 == 0))) {
    // Latch onto the most recent open note for this (channel, note).
    int channel = status & 0x0f;
    for (size_t i = mRecordedNotes.size(); i-- > 0; ) {
      RecordedNote& rn = mRecordedNotes[i];
      if (rn.channel == channel && rn.note == data1 && rn.offSamplePos < 0) {
        rn.offSamplePos = ev.pos - mRecordStartSamplePos;
        break;
      }
    }
  }
}

void SteepSynthEngine::DispatchMidiUpTo(int blockEndPos) {
  cTDAudioPluginBase* eng = ActiveEngine();
  if (!eng) return;
  while (mMidiReadPos < mMidiCount && mMidiBuffer[mMidiReadPos].pos < blockEndPos) {
    const MidiEvent& ev = mMidiBuffer[mMidiReadPos];
    int deltaFrames = ev.pos - mEngineSamplePos;
    if (deltaFrames < 0) deltaFrames = 0;
    eng->handleMidi(deltaFrames, ev.status, ev.data1, ev.data2);
    mMidiReadPos++;
  }
}

// UI thread, not sample-accurate.
void SteepSynthEngine::DispatchVisualMidi() {
  cCodeSynth* cs = mCodeSynth;
  if (!cs) return;

  // (1) Live host MIDI.
  while (mMidiVisualReadPos < mMidiCount) {
    const MidiEvent& ev = mMidiBuffer[mMidiVisualReadPos];
    cs->handleVisualMidi(ev.status, ev.data1, ev.data2);
    mMidiVisualReadPos++;
  }

  // (2) Sequencer playback bypasses mMidiBuffer, so walk its events here, gated on
  // the audio clock (read unsynchronized; off by at most a UI frame). Not gated on
  // mPlaybackActive, so the final note-offs still arrive.
  int playbackPos = mEngineSamplePos;
  while (mPlaybackVisualReadPos < (int)mPlaybackEvents.size() &&
         mPlaybackEvents[mPlaybackVisualReadPos].pos < playbackPos) {
    const MidiEvent& ev = mPlaybackEvents[mPlaybackVisualReadPos];
    cs->handleVisualMidi(ev.status, ev.data1, ev.data2);
    mPlaybackVisualReadPos++;
  }
}

void SteepSynthEngine::StartRecording() {
  StopPlayback();
  mRecording = true;
  mRecordStartSamplePos = mHostSamplePos;
  mRecordedNotes.clear();
}

void SteepSynthEngine::StopRecording() {
  mRecording = false;
  BuildMatrixFromRecording();
}

// Quantizes recorded notes onto a 16th grid. One row per (channel, note,
// velocity), plus an adjacent duplicate row for back-to-back notes: the exported
// player only re-strikes when the event id changes.
void SteepSynthEngine::BuildMatrixFromRecording() {
  mMatrixRows.clear();
  mMatrixCols = 0;
  mMatrixData.clear();
  mMatrixSamplesPerStep = 0.0;
  if (mRecordedNotes.empty()) return;

  double samplesPerStep = mSampleRate * 60.0 / mBpm / 4.0;
  if (samplesPerStep < 1.0) samplesPerStep = 1.0;
  mMatrixSamplesPerStep = samplesPerStep;

  // Rebase on the first note, dropping leading silence.
  int firstSamplePos = mRecordedNotes[0].samplePos;
  for (size_t i = 1; i < mRecordedNotes.size(); i++) {
    if (mRecordedNotes[i].samplePos < firstSamplePos) firstSamplePos = mRecordedNotes[i].samplePos;
  }

  // [onCol, endCol); a note still held at stop gets one step.
  struct NoteSpan { int onCol; int endCol; };
  std::vector<NoteSpan> span(mRecordedNotes.size());
  int maxCol = 0;
  for (size_t i = 0; i < mRecordedNotes.size(); i++) {
    const RecordedNote& n = mRecordedNotes[i];
    int onCol  = (int)((n.samplePos - firstSamplePos) / samplesPerStep + 0.5);
    int endCol;
    if (n.offSamplePos >= 0) {
      endCol = (int)((n.offSamplePos - firstSamplePos) / samplesPerStep + 0.5);
      if (endCol <= onCol) endCol = onCol + 1;
    } else {
      endCol = onCol + 1;
    }
    span[i] = { onCol, endCol };
    if (endCol > maxCol) maxCol = endCol;
  }
  mMatrixCols = maxCol;

  struct Identity { int channel, note, velocity; std::vector<int> notes; };
  std::vector<Identity> ids;
  for (size_t i = 0; i < mRecordedNotes.size(); i++) {
    const RecordedNote& n = mRecordedNotes[i];
    int idx = -1;
    for (size_t k = 0; k < ids.size(); k++)
      if (ids[k].channel == n.channel && ids[k].note == n.note && ids[k].velocity == n.velocity)
        { idx = (int)k; break; }
    if (idx < 0) { idx = (int)ids.size(); ids.push_back({ n.channel, n.note, n.velocity, {} }); }
    ids[idx].notes.push_back((int)i);
  }

  // Back-to-back notes of one identity alternate colours so their runs never
  // merge; a gap resets to 0.
  std::vector<int> colorOfNote(mRecordedNotes.size(), 0);
  std::vector<bool> usesDuplicate(ids.size(), false);
  for (size_t k = 0; k < ids.size(); k++) {
    std::vector<int>& ns = ids[k].notes;
    std::sort(ns.begin(), ns.end(),
              [&](int a, int b) { return span[a].onCol < span[b].onCol; });
    for (size_t j = 0; j < ns.size(); j++) {
      if (j + 1 < ns.size() && span[ns[j]].endCol > span[ns[j + 1]].onCol)
        span[ns[j]].endCol = span[ns[j + 1]].onCol;   // clamp to next onset
      if (span[ns[j]].endCol <= span[ns[j]].onCol)
        span[ns[j]].endCol = span[ns[j]].onCol + 1;
      if (j == 0) { colorOfNote[ns[j]] = 0; continue; }
      bool touching = span[ns[j]].onCol <= span[ns[j - 1]].endCol;
      int color = touching ? (1 - colorOfNote[ns[j - 1]]) : 0;
      colorOfNote[ns[j]] = color;
      if (color == 1) usesDuplicate[k] = true;
    }
  }

  std::vector<int> primaryRow(ids.size()), duplicateRow(ids.size(), -1);
  for (size_t k = 0; k < ids.size(); k++) {
    primaryRow[k] = (int)mMatrixRows.size();
    mMatrixRows.push_back({ ids[k].channel, ids[k].note, ids[k].velocity });
    if (usesDuplicate[k]) {
      duplicateRow[k] = (int)mMatrixRows.size();
      mMatrixRows.push_back({ ids[k].channel, ids[k].note, ids[k].velocity });
    }
  }

  mMatrixData.assign((size_t)mMatrixRows.size() * (size_t)mMatrixCols, 0);
  for (size_t k = 0; k < ids.size(); k++) {
    for (int noteIdx : ids[k].notes) {
      int row = (colorOfNote[noteIdx] == 1) ? duplicateRow[k] : primaryRow[k];
      for (int c = span[noteIdx].onCol; c < span[noteIdx].endCol && c < mMatrixCols; c++)
        mMatrixData[(size_t)row * (size_t)mMatrixCols + (size_t)c] = 1;
    }
  }
}

void SteepSynthEngine::StartPlayback() {
  mPlaybackEvents.clear();
  mPlaybackReadPos = 0;
  mPlaybackVisualReadPos = 0;
  mPlaybackActive = false;
  if (mMatrixRows.empty() || mMatrixCols <= 0) return;

  double step = mMatrixSamplesPerStep > 0.0 ? mMatrixSamplesPerStep : (mSampleRate * 60.0 / mBpm / 4.0);
  if (step < 1.0) step = 1.0;

  // Also beat 0 for `b`.
  mPlaybackOriginSamplePos = mHostSamplePos;

  // Each run of 1s is one note, so durations come from the recorded holds.
  for (size_t r = 0; r < mMatrixRows.size(); r++) {
    const MatrixRow& row = mMatrixRows[r];
    int c = 0;
    while (c < mMatrixCols) {
      if (!mMatrixData[r * (size_t)mMatrixCols + (size_t)c]) { c++; continue; }
      int runStart = c;
      while (c < mMatrixCols && mMatrixData[r * (size_t)mMatrixCols + (size_t)c]) c++;
      int runEnd = c; // one past the last active column
      int onPos  = mHostSamplePos + (int)(runStart * step);
      int offPos = mHostSamplePos + (int)(runEnd * step);
      MidiEvent on;  on.pos  = onPos;  on.status  = 0x90 | (row.channel & 0x0f); on.data1 = row.note; on.data2 = row.velocity;
      MidiEvent off; off.pos = offPos; off.status = 0x80 | (row.channel & 0x0f); off.data1 = row.note; off.data2 = 0;
      mPlaybackEvents.push_back(on);
      mPlaybackEvents.push_back(off);
    }
  }
  // Note-offs before note-ons at equal positions, so an abutting note re-triggers.
  std::sort(mPlaybackEvents.begin(), mPlaybackEvents.end(),
            [](const MidiEvent& a, const MidiEvent& b) {
              if (a.pos != b.pos) return a.pos < b.pos;
              return (a.status & 0xf0) < (b.status & 0xf0);
            });
  mPlaybackActive = !mPlaybackEvents.empty();
}

void SteepSynthEngine::StopPlayback() {
  mPlaybackActive = false;
  mPlaybackEvents.clear();
  mPlaybackReadPos = 0;
  mPlaybackVisualReadPos = 0;
  // Pending note-offs are dropped, so release the visual voices explicitly.
  if (mCodeSynth)
    mCodeSynth->clearVisualVoices();
}

void SteepSynthEngine::DispatchPlaybackUpTo(int blockEndPos) {
  cTDAudioPluginBase* eng = ActiveEngine();
  if (!mPlaybackActive || !eng) return;
  while (mPlaybackReadPos < (int)mPlaybackEvents.size() &&
         mPlaybackEvents[mPlaybackReadPos].pos < blockEndPos) {
    const MidiEvent& ev = mPlaybackEvents[mPlaybackReadPos];
    int deltaFrames = ev.pos - mEngineSamplePos;
    if (deltaFrames < 0) deltaFrames = 0;
    eng->handleMidi(deltaFrames, ev.status, ev.data1, ev.data2);
    mPlaybackReadPos++;
  }
  // Stop once the block holding the last event has run, keeping `b` on the
  // sequence clock until then.
  if (mPlaybackReadPos >= (int)mPlaybackEvents.size() &&
      mEngineSamplePos >= mPlaybackEvents.back().pos)
    mPlaybackActive = false;
}
