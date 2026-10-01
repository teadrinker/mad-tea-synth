#include "steepsynth.h"
#include "IPlug_include_in_plug_src.h"
#include "IControls.h"

SteepSynth::SteepSynth(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kSound)->InitDouble("Sound", 1.0 / kINDEXYNTNumSounds, 0., 1., 0.01, "");
  GetParam(kEnvelope)->InitDouble("Envelope", 0.9, 0., 1., 0.01, "");
  GetParam(kCutoff)->InitDouble("Cutoff", 1.0, 0., 1., 0.01, "");
  GetParam(kResonance)->InitDouble("Resonance", 0.8, 0., 1., 0.01, "");
  GetParam(kMovement)->InitDouble("Movement", 0.3, 0., 1., 0.01, "");
  GetParam(kUnison)->InitDouble("Unison", 0., 0., 1., 0.01, "");
  GetParam(kPortamento)->InitDouble("Portamento", 0.1, 0., 1., 0.01, "");
  GetParam(kDeform)->InitDouble("Deform", 0., 0., 1., 0.01, "");
  // The engine skips temp2 (index 8); init it so the DAW shows no blank name.
  GetParam(8)->InitDouble("", 0., 0., 1., 0.);
  GetParam(kExpression)->InitDouble("Expression", 0.3, 0., 1., 0.01, "");

  mEngine.Init();
  SyncParamsToEngine();

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    const IRECT b = pGraphics->GetBounds();

    // iPlug2 reruns this on resize but never moves existing controls, so re-target
    // them; OnResize reallocates the backbuffer.
    if (pGraphics->NControls())
    {
      pGraphics->GetBackgroundControl()->SetTargetAndDrawRECTs(b);
      pGraphics->GetControl(1)->SetTargetAndDrawRECTs(b);
      return;
    }

    pGraphics->AttachCornerResizer(EUIResizerMode::Size, true);
    pGraphics->AttachPanelBackground(COLOR_BLACK);
    // Off by default in iPlug2; hover states need MOVE events.
    pGraphics->EnableMouseOver(true);
    pGraphics->AttachControl(new SteepSynthControl(b, this));
  };
#endif
}

// Audio thread, before every block.
void SteepSynth::SyncParamsToEngine()
{
  for (int i = 0; i < kNumParams; i++)
  {
    if (i == 8) continue;  // temp2
    double norm = GetParam(i)->GetNormalized();
    mEngine.SetParam(i, (float)norm);
  }
}

#if IPLUG_DSP
void SteepSynth::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  SyncParamsToEngine();
  mEngine.SetTempo(GetTempo());
  mEngine.SetSongPos(GetSamplePos()); // host transport position, for the `b` (beats) sound arg

  static std::vector<float> floatBuf[2];
  for (int c = 0; c < 2; c++) {
    if ((int)floatBuf[c].size() < nFrames)
      floatBuf[c].resize(nFrames);
  }
  float* floatOut[2] = { floatBuf[0].data(), floatBuf[1].data() };
  mEngine.ProcessBlock(floatOut, nFrames);

  const bool clip = mClipOutput.load(std::memory_order_relaxed);
  for (int c = 0; c < 2; c++)
    for (int i = 0; i < nFrames; i++)
    {
      float v = floatBuf[c][i];
      if (clip) v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
      outputs[c][i] = (sample)v;
    }
}

void SteepSynth::ProcessMidiMsg(const IMidiMsg& msg)
{
  mEngine.HandleMidi(msg.mOffset, msg.mStatus, msg.mData1, msg.mData2);
}

void SteepSynth::OnReset()
{
  mEngine.SetSampleRate(GetSampleRate());
}

void SteepSynth::OnIdle()
{
  mEngine.UiIdle();
}
#endif

// Host state: iPlug2's parameter block, then this section. The reader is
// positional and everything after the version is optional: a short chunk keeps
// the defaults.
//
// int32   kStateVersion
// int32   selectedEntry
// int32   focusPane            (EFocusPane)
// int32   cursorRow
// int32   cursorCol
// double  previewWindow16ths
// double  visualDelayMs
// int32   seqBytes             (0 when nothing is recorded)
// uint8[seqBytes]              SerializeSeq's "CSQ1" blob
// double  uiScale
// double  scrollCount
// double[2*scrollCount]        sound, visual offset per entry
// v2:  int32 codeBytes + code text, int32 pathBytes + UTF-8 path ("" = no link), int32 fileIsMaster
// v3:  export page (folder, name, author, target, w, h, filter, rate)
// v4:  line height     v5: preview position   v6: exAlignBpm      v7: exUuid, length-prefixed
// v8:  previewEnabled  v9: exWavRemoveSrc     v10: uiScaleAuto    v11: clipOutput
// v12: exSndLoopShape
//
// New fields go at the end, in a new version-gated block. uiScale and
// scrollCount are unversioned, so they are doubles: the VST3 wrapper reads a
// 4-byte bypass flag right after this chunk, which an 8-byte field can't be
// mistaken for.
static const int kStateVersion = 12;

bool SteepSynth::SerializeState(IByteChunk& chunk) const
{
  if (!SerializeParams(chunk))
    return false;

  int32_t ver = kStateVersion;
  chunk.Put(&ver);

  const CodeSynthUiState& ui = mUiState;
  int32_t selected = (int32_t)ui.selectedEntry;
  int32_t focus    = (int32_t)ui.focusPane;
  int32_t curRow   = (int32_t)ui.cursorRow;
  int32_t curCol   = (int32_t)ui.cursorCol;
  double  zoom     = (double)ui.previewWindow16ths;
  double  delayMs  = (double)mEngine.GetVisualDelayMs();
  chunk.Put(&selected);
  chunk.Put(&focus);
  chunk.Put(&curRow);
  chunk.Put(&curCol);
  chunk.Put(&zoom);
  chunk.Put(&delayMs);

  std::string seq;
  mEngine.SerializeSeq(seq); // leaves seq empty when nothing is recorded
  int32_t seqBytes = (int32_t)seq.size();
  chunk.Put(&seqBytes);
  if (seqBytes > 0)
    chunk.PutBytes(seq.data(), seqBytes);

  double uiScale = (double)ui.uiScale;
  chunk.Put(&uiScale);

  // The delay comes from the engine, which an editor-less session keeps current.
  // Scroll count and offsets are doubles for the bypass-flag reason above.
  int n = ui.entryScrollCount;
  if (n < 0) n = 0;
  if (n > kMaxCodeSynthScrollEntries) n = kMaxCodeSynthScrollEntries;
  double scrollCount = (double)n;
  chunk.Put(&scrollCount);
  for (int i = 0; i < n; i++)
  {
    double s = (double)ui.entryScroll[i].sound;
    double v = (double)ui.entryScroll[i].visual;
    chunk.Put(&s);
    chunk.Put(&v);
  }

  // v2: code, linked path, master flag.
  std::string code = mEngine.CodeSynthGetSourceText();
  int32_t codeBytes = (int32_t)code.size();
  chunk.Put(&codeBytes);
  if (codeBytes > 0)
    chunk.PutBytes(code.data(), codeBytes);

  std::string path = mEngine.GetCodeFilePath();
  int32_t pathBytes = (int32_t)path.size();
  chunk.Put(&pathBytes);
  if (pathBytes > 0)
    chunk.PutBytes(path.data(), pathBytes);

  int32_t fileIsMaster = mEngine.GetFileIsMaster() ? 1 : 0;
  chunk.Put(&fileIsMaster);

  // v3: export settings.
  const ExportSettings& ex = mExportSettings;
  int32_t exDirBytes = (int32_t)ex.destDir.size();
  chunk.Put(&exDirBytes);
  if (exDirBytes > 0) chunk.PutBytes(ex.destDir.data(), exDirBytes);

  int32_t exNameBytes = (int32_t)ex.name.size();
  chunk.Put(&exNameBytes);
  if (exNameBytes > 0) chunk.PutBytes(ex.name.data(), exNameBytes);

  int32_t exAuthorBytes = (int32_t)ex.author.size();
  chunk.Put(&exAuthorBytes);
  if (exAuthorBytes > 0) chunk.PutBytes(ex.author.data(), exAuthorBytes);

  int32_t exTarget = (int32_t)ex.target;
  int32_t exWidth  = (int32_t)ex.width;
  int32_t exHeight = (int32_t)ex.height;
  int32_t exFilter = ex.speakerFilterDefault ? 1 : 0;
  int32_t exRate   = (int32_t)ex.sampleRate;
  chunk.Put(&exTarget);
  chunk.Put(&exWidth);
  chunk.Put(&exHeight);
  chunk.Put(&exFilter);
  chunk.Put(&exRate);

  // v4: line height, in tenths.
  int32_t lineHeightTenths = (int32_t)(ui.lineHeight * 10.f + 0.5f);
  chunk.Put(&lineHeightTenths);

  // v5: preview position.
  int32_t screenDocked = ui.screenDocked ? 1 : 0;
  double  screenPosX   = (double)ui.screenPosX;
  double  screenPosY   = (double)ui.screenPosY;
  chunk.Put(&screenDocked);
  chunk.Put(&screenPosX);
  chunk.Put(&screenPosY);

  // v6: buffer-aligned export tempo.
  int32_t exAlignBpm = mExportSettings.alignBpmToBuffers ? 1 : 0;
  chunk.Put(&exAlignBpm);

  // v7: Pebble app id; empty lets the exporter pick.
  const std::string& exUuid = mExportSettings.uuid;
  int32_t exUuidBytes = (int32_t)exUuid.size();
  chunk.Put(&exUuidBytes);
  if (exUuidBytes > 0) chunk.PutBytes(exUuid.data(), exUuidBytes);

  // v8: preview on/off.
  int32_t previewEnabled = ui.previewEnabled ? 1 : 0;
  chunk.Put(&previewEnabled);

  // v9: remove source after wav.
  int32_t exWavRemoveSrc = mExportSettings.wavRemoveSource ? 1 : 0;
  chunk.Put(&exWavRemoveSrc);

  // v10: Zoom Auto.
  int32_t uiScaleAuto = ui.uiScaleAuto ? 1 : 0;
  chunk.Put(&uiScaleAuto);

  // v11: Clip Output.
  int32_t clipOutput = mClipOutput.load(std::memory_order_relaxed) ? 1 : 0;
  chunk.Put(&clipOutput);

  // v12: Audio loop.
  int32_t exSndLoopShape = (int32_t)mExportSettings.sndLoopShape;
  chunk.Put(&exSndLoopShape);

  return true;
}

int SteepSynth::UnserializeState(const IByteChunk& chunk, int startPos)
{
  int pos = UnserializeParams(chunk, startPos);
  if (pos < 0) return pos;

  int32_t ver = 0;
  int next = chunk.Get(&ver, pos);
  if (next < 0 || ver < 1) return pos; // no section of ours (pre-v1 chunk)
  pos = next;

  CodeSynthUiState ui; // starts at the defaults any missing field keeps
  int32_t selected = 0, focus = 0, curRow = 0, curCol = 0, seqBytes = 0;
  double zoom = 0.0, delayMs = 0.0;

  // A short chunk leaves the remaining fields at their defaults.
  #define STEEP_GET_OR_RETURN(field)                       \
    if ((next = chunk.Get(&(field), pos)) < 0) return pos; \
    pos = next;

  STEEP_GET_OR_RETURN(selected)
  STEEP_GET_OR_RETURN(focus)
  STEEP_GET_OR_RETURN(curRow)
  STEEP_GET_OR_RETURN(curCol)
  STEEP_GET_OR_RETURN(zoom)
  STEEP_GET_OR_RETURN(delayMs)

  ui.selectedEntry = (int)selected;
  ui.focusPane     = (focus >= kFocusSoundEditor && focus <= kFocusNameField)
                       ? (int)focus : kFocusSoundEditor;
  ui.cursorRow     = curRow > 0 ? (int)curRow : 0;
  ui.cursorCol     = curCol > 0 ? (int)curCol : 0;

  ui.previewWindow16ths = (float)(zoom < (double)kPreviewWindowMin16ths ? (double)kPreviewWindowMin16ths
                                : (zoom > (double)kPreviewWindowMax16ths ? (double)kPreviewWindowMax16ths : zoom));
  ui.visualDelayMs = (float)(delayMs < 0.0 ? 0.0 : (delayMs > 300.0 ? 300.0 : delayMs));

  mUiState = ui;
  mUiStateRevision++; // tells an already-open editor to re-read the above
  mEngine.SetVisualDelayMs(ui.visualDelayMs);

  STEEP_GET_OR_RETURN(seqBytes)
  #undef STEEP_GET_OR_RETURN

  if (seqBytes > 0)
  {
    std::vector<char> seq((size_t)seqBytes);
    if ((next = chunk.GetBytes(seq.data(), seqBytes, pos)) < 0) return pos;
    pos = next;
    mEngine.DeserializeSeq(seq.data(), seq.size());
  }

  // Trailing field: bump the revision again so an open editor picks it up.
  double uiScale = 0.0;
  if ((next = chunk.Get(&uiScale, pos)) >= 0)
  {
    pos = next;
    mUiState.uiScale = SnapUiScale((float)uiScale);
    mUiStateRevision++;
  }

  // Read every declared pair so `pos` lands where the VST3 wrapper expects its
  // bypass flag; keep only those that fit.
  double scrollCount = 0.0;
  if ((next = chunk.Get(&scrollCount, pos)) >= 0)
  {
    pos = next;
    int count = (scrollCount > 0.0) ? (int)scrollCount : 0;
    int got = 0;
    for (int i = 0; i < count; i++)
    {
      double s = 0.0, v = 0.0;
      if ((next = chunk.Get(&s, pos)) < 0) break;
      pos = next;
      if ((next = chunk.Get(&v, pos)) < 0) break;
      pos = next;
      if (i >= kMaxCodeSynthScrollEntries) continue; // consumed, not stored
      // The textarea clamps the upper end on draw.
      mUiState.entryScroll[i].sound  = (float)(s > 0.0 ? s : 0.0);
      mUiState.entryScroll[i].visual = (float)(v > 0.0 ? v : 0.0);
      got = i + 1;
    }
    mUiState.entryScrollCount = got;
    mUiStateRevision++;  // trailing field again
  }

  // v2. Path and flag first: setting the path re-pushes the entries, which must
  // not push the pre-restore ones. A master file then replaces the code next tick.
  if (ver >= 2)
  {
    int32_t pathBytes = 0, fileIsMaster = 0, codeBytes = 0;
    std::string code, path;

    if ((next = chunk.Get(&codeBytes, pos)) < 0) return pos;
    pos = next;
    if (codeBytes > 0)
    {
      code.resize((size_t)codeBytes);
      if ((next = chunk.GetBytes(&code[0], codeBytes, pos)) < 0) return pos;
      pos = next;
    }

    if ((next = chunk.Get(&pathBytes, pos)) < 0) return pos;
    pos = next;
    if (pathBytes > 0)
    {
      path.resize((size_t)pathBytes);
      if ((next = chunk.GetBytes(&path[0], pathBytes, pos)) < 0) return pos;
      pos = next;
    }

    if ((next = chunk.Get(&fileIsMaster, pos)) < 0) return pos;
    pos = next;

    mEngine.SetCodeFilePath(path.c_str());
    mEngine.SetFileIsMaster(fileIsMaster != 0);
    // Unconditional, so a saved-empty project doesn't come back with the Example entry.
    mEngine.CodeSynthSetSourceText(code.data(), code.size());
    mUiStateRevision++; // the editor has to re-read the entry list it just got
  }

  // v3: export settings.
  if (ver >= 3)
  {
    ExportSettings ex;
    int32_t exDirBytes = 0, exNameBytes = 0;
    int32_t exTarget = 0, exWidth = 0, exHeight = 0, exFilter = 0;

    if ((next = chunk.Get(&exDirBytes, pos)) < 0) return pos;
    pos = next;
    if (exDirBytes > 0)
    {
      ex.destDir.resize((size_t)exDirBytes);
      if ((next = chunk.GetBytes(&ex.destDir[0], exDirBytes, pos)) < 0) return pos;
      pos = next;
    }

    if ((next = chunk.Get(&exNameBytes, pos)) < 0) return pos;
    pos = next;
    if (exNameBytes > 0)
    {
      ex.name.resize((size_t)exNameBytes);
      if ((next = chunk.GetBytes(&ex.name[0], exNameBytes, pos)) < 0) return pos;
      pos = next;
    }

    int32_t exAuthorBytes = 0;
    if ((next = chunk.Get(&exAuthorBytes, pos)) < 0) return pos;
    pos = next;
    if (exAuthorBytes > 0)
    {
      ex.author.resize((size_t)exAuthorBytes);
      if ((next = chunk.GetBytes(&ex.author[0], exAuthorBytes, pos)) < 0) return pos;
      pos = next;
    }

    if ((next = chunk.Get(&exTarget, pos)) < 0) return pos;
    pos = next;
    if ((next = chunk.Get(&exWidth, pos)) < 0) return pos;
    pos = next;
    if ((next = chunk.Get(&exHeight, pos)) < 0) return pos;
    pos = next;
    if ((next = chunk.Get(&exFilter, pos)) < 0) return pos;
    pos = next;
    int32_t exRate = 0;
    if ((next = chunk.Get(&exRate, pos)) < 0) return pos;
    pos = next;

    // Only a target this build knows.
    if (exTarget == kExportTargetPebble || exTarget == kExportTargetWin32 ||
        exTarget == kExportTargetMicrow8 || ExportTargetIsWav((unsigned)exTarget))
      ex.target = (unsigned)exTarget;
    if (exWidth  > 0) ex.width  = exWidth;
    if (exHeight > 0) ex.height = exHeight;
    ex.speakerFilterDefault = (exFilter != 0);
    if (exRate > 0) ex.sampleRate = exRate;

    mExportSettings = ex;
  }

  // v4: line height, clamped.
  if (ver >= 4)
  {
    int32_t lineHeightTenths = 0;
    if ((next = chunk.Get(&lineHeightTenths, pos)) < 0) return pos;
    pos = next;
    float lh = (float)lineHeightTenths * 0.1f;
    mUiState.lineHeight = (lh < kUiLineHeightMin) ? kUiLineHeightMin
                         : (lh > kUiLineHeightMax) ? kUiLineHeightMax : lh;
    mUiStateRevision++;
  }

  // v5: clamped, or the preview could park off the window.
  if (ver >= 5)
  {
    int32_t screenDocked = 1;
    double  screenPosX = 0.0, screenPosY = 0.0;
    if ((next = chunk.Get(&screenDocked, pos)) < 0) return pos;
    pos = next;
    if ((next = chunk.Get(&screenPosX, pos)) < 0) return pos;
    pos = next;
    if ((next = chunk.Get(&screenPosY, pos)) < 0) return pos;
    pos = next;

    mUiState.screenDocked = (screenDocked != 0);
    mUiState.screenPosX = (float)(screenPosX < 0.0 ? 0.0 : (screenPosX > 1.0 ? 1.0 : screenPosX));
    mUiState.screenPosY = (float)(screenPosY < 0.0 ? 0.0 : (screenPosY > 1.0 ? 1.0 : screenPosY));
    mUiStateRevision++;
  }

  // v6. Absent means off: the setting retimes the song.
  if (ver >= 6)
  {
    int32_t exAlignBpm = 0;
    if ((next = chunk.Get(&exAlignBpm, pos)) < 0) return pos;
    pos = next;
    mExportSettings.alignBpmToBuffers = (exAlignBpm != 0);
  }

  // v7. Absent means empty: reuse or mint.
  if (ver >= 7)
  {
    int32_t exUuidBytes = 0;
    if ((next = chunk.Get(&exUuidBytes, pos)) < 0) return pos;
    pos = next;
    mExportSettings.uuid.clear();
    if (exUuidBytes > 0)
    {
      // A uuid is 36 bytes; bound the length before trusting it.
      if (exUuidBytes > 64) return pos;
      mExportSettings.uuid.resize((size_t)exUuidBytes);
      if ((next = chunk.GetBytes(&mExportSettings.uuid[0], exUuidBytes, pos)) < 0) return pos;
      pos = next;
    }
  }

  // v8. Absent means on.
  if (ver >= 8)
  {
    int32_t previewEnabled = 1;
    if ((next = chunk.Get(&previewEnabled, pos)) < 0) return pos;
    pos = next;
    mUiState.previewEnabled = (previewEnabled != 0);
    mUiStateRevision++;
  }

  // v9. Absent means false, keep the source.
  if (ver >= 9)
  {
    int32_t exWavRemoveSrc = 0;
    if ((next = chunk.Get(&exWavRemoveSrc, pos)) < 0) return pos;
    pos = next;
    mExportSettings.wavRemoveSource = (exWavRemoveSrc != 0);
  }

  // v10. Absent means off.
  if (ver >= 10)
  {
    int32_t uiScaleAuto = 0;
    if ((next = chunk.Get(&uiScaleAuto, pos)) < 0) return pos;
    pos = next;
    mUiState.uiScaleAuto = (uiScaleAuto != 0);
    mUiStateRevision++;
  }

  // v11. Absent means on.
  if (ver >= 11)
  {
    int32_t clipOutput = 1;
    if ((next = chunk.Get(&clipOutput, pos)) < 0) return pos;
    pos = next;
    mClipOutput.store(clipOutput != 0, std::memory_order_relaxed);
  }

  // v12. Absent or unknown means auto.
  if (ver >= 12)
  {
    int32_t exSndLoopShape = kSongSndLoopAuto;
    if ((next = chunk.Get(&exSndLoopShape, pos)) < 0) return pos;
    pos = next;
    if (exSndLoopShape >= 0 && exSndLoopShape <= kSongSndLoopPerSample)
      mExportSettings.sndLoopShape = (unsigned)exSndLoopShape;
  }

  return pos;
}
