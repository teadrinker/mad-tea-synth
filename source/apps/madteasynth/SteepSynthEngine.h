#pragma once

// The legacy Indexynt backend. Off by default; CMake -DSTEEPSYNTH_ENABLE_INDEXYNT=ON.
#ifndef STEEPSYNTH_ENABLE_INDEXYNT
#define STEEPSYNTH_ENABLE_INDEXYNT 0
#endif

#include <string>
#include <vector>

#include "codesynth/ExportProject.h"
#include "codesynth/SongSeq.h"

struct RenderCtx;  // opaque; see ScreenCtx()
struct CodeSynthProbes;
struct CodeSynthInspectListener;
class cCodeSynth;
class cINDEXYNT;
class cTDAudioPluginBase;

// Both backends stay alive, so switching allocates nothing on the audio thread.
enum class SynthBackend
{
  Indexynt = 0,  // only built with STEEPSYNTH_ENABLE_INDEXYNT
  CodeSynth = 1
};

class SteepSynthEngine {
public:
  SteepSynthEngine();
  ~SteepSynthEngine();

  void ProcessBlock(float** outputs, int nFrames);
  void HandleMidi(int frameOffset, int status, int data1, int data2);
  void SetSampleRate(double sr);
  void SetParam(int idx, float value);
  float GetParam(int idx) const;
  int ParamCount() const;
  void Init();

  void SetBackend(SynthBackend b) {
#if !STEEPSYNTH_ENABLE_INDEXYNT
    // Never constructed; selecting it would silence the plugin.
    if (b == SynthBackend::Indexynt) return;
#endif
    mBackend = b;
  }
  SynthBackend GetBackend() const { return mBackend; }

  void SetTempo(double bpm) { if (bpm > 0.0) mBpm = bpm; }
  // Always > 0.
  double GetTempo() const { return mBpm; }

  // The 1/16 grid an export starts from: the recorded matrix's grid, not the
  // current host tempo.
  int    ExportSongTick() const;
  double ExportSampleRate() const { return mSampleRate > 0.0 ? mSampleRate : 44100.0; }

  // Host transport position in samples at the start of this block.
  void SetSongPos(double samplePos) { mSongPosSamples = samplePos; }

  // Stopping quantizes the captured note-ons onto a 16th grid: one row per
  // distinct (channel, note, velocity).
  void StartRecording();
  void StopRecording();
  bool IsRecording() const { return mRecording; }
  bool HasRecordedMatrix() const { return !mMatrixRows.empty(); }

  int MatrixRowCount() const { return (int)mMatrixRows.size(); }
  int MatrixColCount() const { return mMatrixCols; }
  bool MatrixCellActive(int row, int col) const {
    if (row < 0 || row >= (int)mMatrixRows.size() || col < 0 || col >= mMatrixCols) return false;
    return mMatrixData[(size_t)row * (size_t)mMatrixCols + (size_t)col] != 0;
  }
  // Channel is 0-based here; entry headers are 1-based. -1 when out of range.
  int MatrixRowChannel(int row) const {
    return (row >= 0 && row < (int)mMatrixRows.size()) ? mMatrixRows[(size_t)row].channel : -1;
  }
  int MatrixRowNote(int row) const {
    return (row >= 0 && row < (int)mMatrixRows.size()) ? mMatrixRows[(size_t)row].note : -1;
  }

  // Auto-stops after the last note-off.
  void StartPlayback();
  void StopPlayback();
  bool IsPlaying() const { return mPlaybackActive; }

  // Writes song.c to the linked file's directory.
  bool SaveCSong(SongChannelMode channelMode, SongSndLoopShape sndLoopShape);

  // A self-contained project into settings.destDir. `result` includes the Pebble
  // uuid the export settled on.
  bool ExportProject(const ExportSettings& settings, std::string& msg,
                     ExportProjectResult* result = nullptr);

  // codesynth.seq in the linked file's directory.
  bool SaveSeq();
  bool LoadSeq();

  // The same payload for the host's plugin state. Deserialize leaves the matrix
  // untouched on a bad blob.
  bool SerializeSeq(std::string& out) const;
  bool DeserializeSeq(const char* data, size_t len);

  // UI thread. Also the one automatic disk read, when the linked file is master.
  void UiIdle();
  const char* GetCodeSynthDebugMessage() const;

  // Called from UiIdle and from Draw(); each call measures its own wall-clock delta.
  void TickVisualPipeline();

  // This instance's screen. Never the vscreen_* globals, which belong to an
  // exported song.
  RenderCtx* ScreenCtx();

  // The wrap arguments (kCodeSynthWrapParams order) the visual pipeline last ran
  // the open entry with. UI thread only.
  bool CodeSynthLastVoiceArgs(double* out) const;

  // Inspection of the open entry's visual body, UI thread only: probes compiled
  // into the visual pipeline (a change recompiles that half alone), the editor
  // text their spans are in, and where the readings go.
  void SetCodeSynthVisualProbes(const CodeSynthProbes& probes);
  void SetCodeSynthVisualProbeBody(const std::string& editorBody);
  void SetCodeSynthInspectListener(const CodeSynthInspectListener& listener);

  const std::vector<CodeSynthEntry>& Entries() const { return mEntries; }
  int EntryCount() const { return (int)mEntries.size(); }
  // A default entry when idx is out of range.
  const CodeSynthEntry& Entry(int idx) const;
  // Applies `edit`, then recompiles. No-op when idx is out of range.
  template <class F> void EditEntry(int idx, F&& edit)
  {
    if (idx < 0 || idx >= EntryCount()) return;
    edit(mEntries[(size_t)idx]);
    PushCodeSynthLiveEdit();
  }
  // Header-breaking characters become '_'; otherwise stored verbatim.
  void SetEntryName(int idx, const char* name);
  // Unique name, fixed-point type, and the next free (channel, note) after the
  // last entry.
  int AddCodeSynthEntry(bool withSound, bool withVisual);

  // The prelude blocks, pinned to the front of the list (common, then globals).
  // -1 if one exists already.
  int AddCodeSynthCommonEntry();
  int AddCodeSynthGlobalsEntry();
  bool DeleteCodeSynthCommonEntry();
  bool DeleteCodeSynthGlobalsEntry();
  // Refuses the prelude blocks.
  bool DeleteCodeSynthEntry(int idx);
  void PushCodeSynthLiveEdit();
  // Recompiles keep the globals' running values; this puts back the initial ones.
  void ResetCodeSynthGlobals();

  // The path is always live: Load/Save, exports and WAV directives use it.
  // FileIsMaster only governs automatic pulls on UiIdle. Setting either re-reads a
  // master file on the next tick.
  void SetCodeFilePath(const char* path);
  const char* GetCodeFilePath() const { return mCodeFilePath.c_str(); }
  void SetFileIsMaster(bool master);
  bool GetFileIsMaster() const { return mFileIsMaster; }
  // With a trailing separator; "" when unset.
  std::string AssetDir() const;

  bool LoadCodeFromFile();
  bool SaveCodeToFile();

  std::string CodeSynthGetSourceText() const;
  void CodeSynthSetSourceText(const char* text, size_t len);

  void SetVisualDelayMs(float ms);
  float GetVisualDelayMs() const;

  // The entry the screen shows while no visual voice plays; idx < 0 = none.
  void SetVisualPreviewEntry(int idx);
  // Whether idx's visual body was drawn in the last visual tick. UI thread only.
  bool EntryVisuallyRendered(int idx) const;

  static const int kBlockSize = 32; // VSTPLUGIN_FORCEBLOCKSIZE

private:
  SynthBackend mBackend = SynthBackend::CodeSynth;

  cINDEXYNT* mIndexynt = nullptr;
  cCodeSynth* mCodeSynth = nullptr;
  cTDAudioPluginBase* ActiveEngine() const;
  float* mParams = nullptr;
  bool* mParamChanged = nullptr;

  std::vector<CodeSynthEntry> mEntries;

  std::string mCodeFilePath;
  bool        mFileIsMaster = false;

  // mWatchValid false means not yet seen, so the next tick pulls.
  std::string  mWatchedPath;
  long long    mWatchedStamp = 0;
  bool         mWatchValid = false;
  bool PollCodeFile(std::string& out);

  std::vector<float> mBatchIn[2];
  std::vector<float> mBatchOut[2];
  int mBatchPos;

  // Handed to the engine along kBlockSize boundaries.
  struct MidiEvent { int pos; int status; int data1; int data2; };
  static const int kMidiBufferMax = 4096;
  MidiEvent mMidiBuffer[kMidiBufferMax];
  int mMidiCount;
  int mMidiReadPos;
  // The UI-thread visual dispatch's own cursor; the buffer compacts once both catch up.
  int mMidiVisualReadPos;
  int mHostSamplePos;   // absolute sample position at the start of the current ProcessBlock call
  int mEngineSamplePos; // absolute sample position of the next block the engine will process
  double mSampleRate;   // last value passed to SetSampleRate(), used for matrix/playback timing
  double mBpm;          // last value passed to SetTempo(), used for matrix/playback timing
  double mSongPosSamples; // last value passed to SetSongPos(): host transport pos (samples)
  double mPrevSongPosSamples;  // unchanged means the host transport is stopped
  double mFreeBeatPos;  // free-running `b`: follows the transport, free-runs while stopped

  void DispatchMidiUpTo(int blockEndPos);

  void DispatchVisualMidi();

  // Positions are relative to record start; offSamplePos is -1 until the note-off.
  struct RecordedNote { int samplePos; int channel; int note; int velocity; int offSamplePos; };
  bool mRecording;
  int mRecordStartSamplePos;
  std::vector<RecordedNote> mRecordedNotes;

  // A duplicate row splits back-to-back same-sound notes across two event ids,
  // since the player only retriggers on an id change.
  using MatrixRow = SongEventRow;
  std::vector<MatrixRow> mMatrixRows;
  int mMatrixCols;
  // Row-major; a held note is a run of 1s.
  std::vector<unsigned char> mMatrixData;
  double mMatrixSamplesPerStep; // 16th-note length (in samples) the matrix was built with

  SongSeq CurrentSeq() const;
  void AdoptSeq(SongSeq&& seq);

  void BuildMatrixFromRecording();

  std::vector<MidiEvent> mPlaybackEvents; // sorted ascending by pos
  int mPlaybackReadPos;
  // The UI thread's own cursor over the playback events, gated on the audio clock.
  int mPlaybackVisualReadPos;
  bool mPlaybackActive;
  int mPlaybackOriginSamplePos;  // absolute sample pos where `b` reads 0 during playback

  void DispatchPlaybackUpTo(int blockEndPos);
};
