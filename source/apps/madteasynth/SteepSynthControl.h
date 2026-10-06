#pragma once

#include <array>
#include <string>
#include <vector>

// IPlug headers first: MSVC's <xkeycheck.h> rejects a macroized bool. Then
// bool->int for the C UI headers, undone after them.
#include "IPlug_include_in_plug_hdr.h"

#if defined(__cplusplus) && !defined(INCLUDED_UI_C_GUARDS_H)
#define INCLUDED_UI_C_GUARDS_H
#define bool   int
#define true   1
#define false  0
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#include "ui/textmode.h"
#include "ui/textmode_ui.h"
#include "ui/textmode_ui_textarea.h"
#include "ui/ui_glue.h"
#include "vscreen.h"
#include "font/font_cache.h"
#include "font/gen_font.h"
#include "common/tsys.h"
#include "common/string_pure.h"
#include "common/array.h"
#include "common/math_pure.h"

#if defined(__cplusplus)
} // extern "C"
#endif

#if defined(__cplusplus) && defined(INCLUDED_UI_C_GUARDS_H)
#undef INCLUDED_UI_C_GUARDS_H
#undef bool
#undef true
#undef false
#endif

using namespace iplug;
using namespace igraphics;

#include "IControl.h"

constexpr float kBaseFontSize = 1.f;

// UI size (the Zoom dropdown): a multiplier on kBaseFontSize, counted in device
// pixels so a level means the same on every monitor.
constexpr float kUiScaleDefault = 1.0f;

// Snap rather than clamp: an off-preset value from a host chunk would leave the
// dropdown with no selection.
inline float SnapUiScale(float s) { return ui_glue_zoom_snap(s); }

// Line height scales the glyph height only: rows move, columns don't.
constexpr float kUiLineHeightMin     = 1.0f;
constexpr float kUiLineHeightMax     = 2.0f;
constexpr float kUiLineHeightDefault = 1.5f;

// Preview window, in 16th notes. Mid is the slider's half-travel value.
constexpr float kPreviewWindowMin16ths = 0.001f;
constexpr float kPreviewWindowMid16ths = 1.0f;
constexpr float kPreviewWindowMax16ths = 4.0f;

class SteepSynth;
class SteepSynthEngine;
struct CodeSynthUiState;

// Per-entry editor scroll, in visual lines. Persisted in the host chunk.
struct CodeSynthEditorScroll
{
  float sound  = 0.f;
  float visual = 0.f;
};

// A fixed array, not a vector: SerializeState reads it without a lock from the
// host's thread.
constexpr int kMaxCodeSynthScrollEntries = 512;

struct InteractiveCoding;

// Device pixels per preview pixel. Corner drags make it fractional; the bar's
// < > step to whole numbers.
constexpr int   kScreenZoom    = 1;
constexpr float kScreenZoomMin = 0.25f;
constexpr float kScreenZoomMax = 16.f;
constexpr int   kScreenMargin  = 8;
constexpr int   kScreenPanelW  = VSCREEN_W * kScreenZoom + kScreenMargin * 2;

// kScreenGrab: how far outside the image a press still grabs the frame.
// kScreenCornerGrab: how far along the ring from a corner a press resizes instead.
constexpr float kScreenBezel      = 5.f;
constexpr float kScreenGrab       = 10.f;
constexpr float kScreenCornerGrab = 16.f;
constexpr float kScreenDividerGrab = 4.f;
// A released resize within this fraction of a whole zoom lands on it.
constexpr float kScreenZoomSnap = 0.04f;
// How much of a dragged preview must stay inside the editor.
constexpr float kScreenKeepVisible = 28.f;

class cCodeSynthGlobals;

class SteepSynthControl : public IControl
{
public:
  SteepSynthControl(const IRECT& bounds, SteepSynth* pPlugin);
  ~SteepSynthControl();

  void Draw(IGraphics& g) override;
  void OnResize() override;
  void OnMouseDown(float x, float y, const IMouseMod& mod) override;
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override;
  void OnMouseUp(float x, float y, const IMouseMod& mod) override;
  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override;
  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override;
  void OnMouseOver(float x, float y, const IMouseMod& mod) override;
  void OnMouseOut() override;
  bool OnKeyDown(float x, float y, const IKeyPress& key) override;
  bool OnKeyUp(float x, float y, const IKeyPress& key) override;
  // A dropped PNG becomes scanline spans inserted as code.
  void OnDrop(const char* str) override;
  // A drop inside the path field sets the linked path; elsewhere falls through to OnDrop.
  void OnDropAt(const char* str, float x, float y) override;

private:
  void ReallocBackbuffer();
  // keepFonts: only the column split moved (a preview resize), so the glyphs still hold.
  void RebuildUI(float fps, bool keepFonts = false);

  // Its own NanoVG image rather than part of the textmode backbuffer: the two
  // change on unrelated schedules.
  void DrawVirtualScreen(IGraphics& g);
  void ReleaseScreenImage();
  // True when the textmode layout must regrid.
  bool SyncScreenResolutionAndDock();
  bool ScreenImageRect(float& x, float& y, float& w, float& h) const;
  // Drawn first in the frame, so its cells and presses win over a floating
  // preview's editor underneath.
  void DrawScreenBar();
  bool HitScreenBar(float x, float y) const;
  bool HitScreenFrame(float x, float y) const;
  // 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right; -1 for none.
  int  HitScreenCorner(float x, float y) const;
  // The docked panel's left edge; dragging it resizes the preview.
  bool HitScreenDivider(float x, float y) const;
  float MaxScreenZoom() const;
  void ScreenResizeGrowsLeftUp(bool& left, bool& top) const;
  void ResizeScreenTo(float x, float y, bool snap = false);
  void SetScreenPos(float x, float y);
  void DockScreen();
  void DrawCodeSynthEditor();
  void DrawCodeSynthHeaderRow(int col, int row, int w);

  void DrawCodeSynthNameField(int col, int row, int w);

  // Restore runs at the top of a frame and stages focus/caret; Record applies them
  // at the bottom, once the editors hold the restored text, then writes back.
  void RestoreUiState();
  void RecordUiState();

  void DrawTransportButtons();

  struct CodeEditor
  {
    InteractiveCoding* ic = nullptr;
    int         loadedSound = -1;    // entry whose body is loaded here, or -1
    std::string lastSyncedBody;      // trimmed body matching both ic + model
    bool        isVisual = false;
    SteepSynthControl* owner = nullptr;

    // The engine runs the bodies (host mode). codeSerial: the body last handed
    // over; resultSerial: the one whose results were last reported. The
    // editor's probe set, and whether the engine has it yet.
    struct Probe { int id; size_t lo, hi; };
    unsigned    codeSerial = 0;
    unsigned    resultSerial = 0;
    std::vector<Probe> probes;
    unsigned    probeSerial = 0;
    bool        probesDirty = false;
  };

  // Fired on every edit that parses, scrubbing included.
  static void OnCodeChanged(const char* body, unsigned serial, void* user);
  void PushLiveEdit(const char* liveBody, CodeEditor& ed);

  // Inspection. Visual: the probes go into the engine's visual pipeline, whose
  // readings and first-run report come back through its listener. Sound: a
  // probed copy of the open entry's body replayed here with the arguments the
  // visual pipeline last used (SoundReplay, in the .cpp).
  static void OnProbesChanged(const struct ICProbe* probes, int n, unsigned codeSerial,
                              unsigned probeSerial, void* user);
  void UpdateInspection();
  void OnVisualRan(bool ok, const char* print, int printLen);
  void ReplaySound();
  struct SoundReplay;
  SoundReplay* mSoundReplay = nullptr;

  // Supplies the sounding note's values as the wrapped body's arguments.
  static int OnWrapArgs(const char* params, char* out, int outMax, void* user);

  // Deferred to the frame: set_text rebuilds the parse arena.
  void ApplyPendingDrop();
  void StageWavDrop(const char* path, bool ontoEditor);
  void ApplyPendingWavDrop();
  std::string WavDirectivePath(const char* absPath) const;

  void DrawCodeFileRow();
  void DrawCodeFilePathField();
  void PromptForCodeFile();

  void DrawSettingsPage();
  void PromptForExportFolder();
  void DoExportProject();
  void ReloadAllEditorsFromModel();

  // Grows entryScrollCount to cover idx; NULL when out of range.
  CodeSynthEditorScroll* EntryScrollSlot(int idx);
  // No-ops when ed holds no entry.
  void StashEditorScroll(const CodeEditor& ed);
  void ApplyEditorScroll(const CodeEditor& ed);

  void SyncCodeEditor(CodeEditor& ed);
  void ReloadCodeEditor(CodeEditor& ed);
  void ApplyEditorCompileContext(CodeEditor& ed);
  std::string EditorModelBody(const CodeEditor& ed) const;

  // This control's own globals block for UI-thread compiles, never the engine's.
  // Rebuilt only when its source changes.
  const cCodeSynthGlobals* LiveGlobalsFor(bool forVisual);
  cCodeSynthGlobals* mLiveGlobals[2] = { nullptr, nullptr };  // [0] sound, [1] visual
  std::string        mLiveGlobalsKey[2];                      // source they were built from

  // Runs the body standalone over a short window for the waveform preview.
  void RenderCodeSynthPreview(const std::string& body, int codeSynthType);

  // The preview VM's smp() context. The WAV is loaded UI-side and cached.
  struct CodeSynthVmCtx* PreviewSmpCtx(const std::string& body, double hostSr);

  // Stereo draws two traces sharing one normalisation.
  void PushPreviewWaveformPath(int col, int row, int w, int h, float alpha);

  void PushMatrixPath(int col, int row, int w, int h, float alpha);

  SteepSynth* mPlugin = nullptr;
  SteepSynthEngine* mEngine = nullptr;  // null exactly when mPlugin is
  // The plugin's saved editor state, read and written in place.
  CodeSynthUiState& Ui() const;

  Tsys         mSys;
  FontCache*   mFC = nullptr;
  FONT_SETTINGS mFontSettings;
  int          mFontIndex = 0;
  int          mFontApplied = 0;
  TextMode*    mTM = nullptr;
  UIContext*   mUI = nullptr;

  int           mBackW = 0;          // logical width  (control pixels)
  int           mBackH = 0;          // logical height
  int           mBackPhysW = 0;       // physical backbuffer width  (mBackW * mBackScale)
  int           mBackPhysH = 0;       // physical backbuffer height
  float         mBackScale = 1.f;     // backing-pixel scale factor
  unsigned int* mBackbuffer = nullptr; // RGBA pixel buffer (physical size)

  // The size in effect; UiState() holds the picked one, adopted at the top of the
  // next frame since it is picked inside ui_begin/ui_end.
  float         mUiScale = kUiScaleDefault;
  float         mLineHeight = kUiLineHeightDefault;

  int           mNVGImage = 0;
  bool          mImageValid = false;

  std::vector<unsigned int> mScreenRGBA;
  int           mScreenNVGImage = 0;
  bool          mScreenImageValid = false;
  // 0 is never returned by vscreen_generation(), so the first frame uploads.
  unsigned int  mScreenGeneration = 0;
  bool          mScreenPebblePal = false;
  // mScreenPanelW is 0 while the preview floats.
  float         mScreenZoomDev = (float)kScreenZoom;
  float         mScreenPanelW = (float)kScreenPanelW;

  // `width`/`height` are folded, so a resize reaches bodies only by a recompile.
  int           mScreenResW = VSCREEN_W;
  int           mScreenResH = VSCREEN_H;

  // The docking the textmode grid was last laid out for.
  bool          mScreenDockedLaidOut = true;

  // Where the image was drawn last frame; hit tests read this.
  float         mScreenDrawX = 0.f, mScreenDrawY = 0.f;
  float         mScreenDrawW = 0.f, mScreenDrawH = 0.f;
  bool          mScreenDrawValid = false;

  // A corner drag keeps the opposite corner fixed; docked, the top-right one.
  int           mScreenResizeCorner = -1;
  float         mScreenResizeAnchorX = 0.f, mScreenResizeAnchorY = 0.f;
  // A divider drag is a corner drag (kScreenDividerCorner) that reads x only.
  static constexpr int kScreenDividerCorner = 4;
  bool          mScreenDividerHover = false;
  bool          mScreenCursorSet = false;

  // The fullscreen / zoom bar under the preview, in cells; 0 rows when not drawn.
  float         mScreenBarCol = 0.f, mScreenBarRow = 0.f;
  float         mScreenBarCols = 0.f;
  int           mScreenBarRows = 0;

  // Every mouse event is swallowed while dragging.
  bool          mScreenDragging = false;
  float         mScreenDragOffX = 0.f, mScreenDragOffY = 0.f;

  // The preview covers the whole control; any click ends it, and that click's
  // drag/up are swallowed too.
  bool          mScreenFullscreen = false;
  bool          mScreenFullscreenExitHeld = false;
  // Hosts like REAPER close the plugin window on Escape; it is ours while fullscreen.
  bool          mEscapeCaptured = false;

  // mCols stops short of a docked preview; mGridCols runs under it, for its bar.
  int           mCols = 80;
  int           mGridCols = 80;
  int           mRows = 25;
  float         mCellW = 21.f;        // logical cell width  (for mouse coord conversion)
  float         mCellH = 43.f;        // logical cell height

  int           mMouseButton = -1;
  // The editor shares its window with the host: take the keyboard only while the
  // pointer is over it.
  bool          mMouseInside = false;

  static constexpr int kActionRow = 1;   // "File:"     ...
  static constexpr int kSeqRow    = 2;   // "Sequence:" ...
  static constexpr int kContentRow = 4;  // kSeqRow + 1 is left blank

  // Also used to size the preview before the editor is laid out.
  // Column 0 is the event list's marker column.
  static constexpr int kLeftColumnWidth = 19;
  CodeEditor    mVisualEd;
  CodeEditor    mSoundEd;
  int           mSelectedSound = -1; // index into SteepSynth::CodeSynthEntry*, or -1

  // The last selected event. A selected prelude block keeps it, so the screen keeps
  // showing that event; the name re-finds it when inserts or deletes shift the index.
  int           mContextEntry = -1;
  std::string   mContextEntryName;
  int ResolveContextEntry();

  std::string   mPendingDropCode;

  // Staged: the drop arrives outside the frame. mPendingDropWavOnEditor true
  // replaces the selected sound's WAV, false adds a sound.
  std::string   mPendingDropWav;
  bool          mPendingDropWavOnEditor = false;

  // The prelude blocks ignore the filter.
  enum EventFilter
  {
    kEventFilterAll = 0,
    kEventFilterSounds,   // has a sound body
    kEventFilterVisuals,  // has a visual body
    kEventFilterBoth,     // has both halves
    kEventFilterInSeq,    // triggered by the recorded sequence
    kEventFilterNotInSeq, // ...and its complement
    kEventFilterCount
  };
  int           mEventFilter = kEventFilterAll;

  UITextArea*   mNameTA = nullptr;
  int           mNameLoadedSound = -1;

  UITextArea*   mPathTA = nullptr;

  // Export settings live in the plugin so they are saved with its state.
  bool          mShowSettings = false;
  UITextArea*   mExportDirTA  = nullptr;
  UITextArea*   mExportNameTA = nullptr;
  UITextArea*   mExportAuthorTA = nullptr;
  UITextArea*   mExportRateTA = nullptr;
  UITextArea*   mExportUuidTA = nullptr;
  UITextArea*   mExportWTA    = nullptr;
  UITextArea*   mExportHTA    = nullptr;
  std::string   mExportStatus;
  bool          mExportOk = false;
  // Width 0 means not drawn this frame.
  int           mPathFieldCol = 0, mPathFieldRow = 0, mPathFieldW = 0;

  // The sound editor frame, for WAV drop hit tests. W is 0 when not drawn.
  int           mSoundEdCol = 0, mSoundEdRow = 0, mSoundEdW = 0, mSoundEdH = 0;

  UITextArea*   mNoteTA = nullptr;
  UITextArea*   mTransposeTA = nullptr;

  std::array<UITextArea**, 11> Fields()
  {
    return { &mNameTA, &mPathTA, &mNoteTA, &mTransposeTA, &mExportDirTA, &mExportNameTA,
             &mExportAuthorTA, &mExportRateTA, &mExportUuidTA, &mExportWTA, &mExportHTA };
  }

  // 0 matches the plugin's initial revision.
  unsigned int  mUiStateSeenRevision = 0;
  bool          mFirstCodeSynthFrame = true;
  // -1 = nothing pending.
  int           mPendingFocusPane = -1;
  int           mPendingCursorRow = 0;
  int           mPendingCursorCol = 0;
  float         mEditorSplit = 0.5f;

  // Edge-detects editor focus for PlatformKeyFocus, which gives a native Edit the
  // OS focus so hosts don't swallow Ctrl+C/X/V.
  bool          mEditorHadOsFocusProxy = false;
  // Applied before the next ui_begin: SetFocus/CreateWindowEx dispatch messages
  // synchronously.
  bool          mPendingOsFocusProxy = false;
  bool          mInDraw = false;

  // One preview sample per pixel column, capped.
  static constexpr int kMaxPreviewSamples = 2048;
  std::vector<float> mPreviewSamples;   // left channel (pan = -1), or the mono trace
  std::vector<float> mPreviewSamplesR;
  int           mPreviewValidCount = 0;
  // True only when the two channels actually differ.
  bool          mPreviewStereo = false;
  bool          mPreviewVisible = false;

  // A UI-owned load of the sample; the engine's copy is freed under us on recompile.
  struct CodeSynthPreviewSmp* mPreviewSmp = nullptr;

  // False while the buffer doesn't compile, hiding the stale trace.
  bool          mPreviewCodeOk = true;

  int           mPreviewCol = 0, mPreviewRow = 0, mPreviewW = 0, mPreviewH = 0;
};
