#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include <atomic>

using namespace iplug;
using namespace igraphics;

#include "IControl.h"
#include "SteepSynthControl.h"
#include "SteepSynthEngine.h"

const int kINDEXYNTNumSounds = 14; // from INDEXYNT.cpp

// Exposed to the DAW; indices match eINDEXYNTParam.
enum ESteepParams
{
  kSound = 0,
  kEnvelope,
  kCutoff,
  kResonance,
  kMovement,
  kUnison,
  kPortamento,
  kDeform,
  // skip temp2 (8)
  kExpression = 9,
  kNumParams
};

const int kNumPresets = 1;

// Editor state saved with the session. Lives on the plugin since the host can
// save with no editor open.
struct CodeSynthUiState
{
  int   selectedEntry = -1;   // index into the CodeSynthEntry list, -1 = none
  int   focusPane     = 0;    // which pane holds the keyboard cursor -- see EFocusPane
  int   cursorRow     = 0;    // caret position within that pane, in logical
  int   cursorCol     = 0;    //   (source line, column) coordinates
  // Waveform preview window, in 16th notes.
  float previewWindow16ths = kPreviewWindowMid16ths;
  int   previewEnabled = 1;  // int: ui_check_box binds it directly
  float visualDelayMs = 0.f;  // visual editor's "VisDelay" slider (0..300 ms)
  // Backend-independent, so mirrored from Draw().
  float uiScale       = kUiScaleDefault;
  // uiScale stays the level in effect; this records that it follows the display.
  int   uiScaleAuto   = 0;
  float lineHeight    = kUiLineHeightDefault;
  bool  screenDocked  = true;
  // Fractions of the editor size; ignored while docked.
  float screenPosX    = 0.f;
  float screenPosY    = 0.f;
  // Only the first entryScrollCount are meaningful.
  CodeSynthEditorScroll entryScroll[kMaxCodeSynthScrollEntries];
  int   entryScrollCount = 0;
};

// CodeSynthUiState::focusPane values. Serialized, so only ever append.
enum EFocusPane
{
  kFocusSoundEditor = 0,
  kFocusVisualEditor,
  kFocusNameField
};

class SteepSynth final : public Plugin
{
public:
  SteepSynth(const InstanceInfo& info);

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnIdle() override; // UI thread: drives the visual pipeline + the file watch
#endif

  // Carries the sounds, sequence, file link and editor UI state, so a session
  // restores with no file on disk.
  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

  // The editor reads and writes this directly; mUiStateRevision tells it a restore
  // replaced the selection and caret.
  CodeSynthUiState& UiState() { return mUiState; }
  const CodeSynthUiState& UiState() const { return mUiState; }
  unsigned int UiStateRevision() const { return mUiStateRevision; }

  Plugin* GetPlugin() { return this; }

  SteepSynthEngine& Engine() { return mEngine; }
  const SteepSynthEngine& Engine() const { return mEngine; }

  bool SaveCSong() { return mEngine.SaveCSong(ExportTargetChannelMode(mExportSettings), (SongSndLoopShape)mExportSettings.sndLoopShape); }

  // Serialized with the state, so they survive the editor closing.
  const ExportSettings& GetExportSettings() const { return mExportSettings; }
  void SetExportSettings(const ExportSettings& s) { mExportSettings = s; }
  bool ExportProject(std::string& msg)
  {
    ExportProjectResult res;
    if (!mEngine.ExportProject(mExportSettings, msg, &res)) return false;
    // Adopt the uuid the export settled on, so later exports are deliberate.
    if (mExportSettings.target == kExportTargetPebble && !res.uuid.empty())
      mExportSettings.uuid = res.uuid;
    return true;
  }
  // Hard-clips the live output to [-1, 1]. Exports always clip: they write PCM.
  bool GetClipOutput() const { return mClipOutput.load(std::memory_order_relaxed); }
  void SetClipOutput(bool on) { mClipOutput.store(on, std::memory_order_relaxed); }

private:
  void SyncParamsToEngine();

  SteepSynthEngine mEngine;

  // Unsynchronised with SerializeState, like mUiState.
  ExportSettings mExportSettings;

  std::atomic<bool> mClipOutput{true};

  // Unsynchronised: independent scalars, so a race saves at worst a stale value.
  // Hence fixed arrays, never vectors.
  CodeSynthUiState mUiState;
  // Bumped by UnserializeState; 0 = nothing restored.
  unsigned int mUiStateRevision = 0;
};
