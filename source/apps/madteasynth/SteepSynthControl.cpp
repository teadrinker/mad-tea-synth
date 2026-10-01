#include "SteepSynthControl.h"
#include "steepsynth.h"
#include "codesynth/CodeSynthParse.h"

#include "IGraphicsNanoVG.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "interactive_coding/interactive_coding.h"
}

#include "png2span.h"

#include "platform/platform.h"

bool PlatformFolderPicker_Choose(const std::string& initialUtf8, std::string& outUtf8);

void PlatformClipboard_SetGraphics(IGraphics* graphics);

extern "C" void PlatformKeyFocus_SetGraphics(void* graphics);
extern "C" void PlatformKeyFocus_SetEditorFocused(int focused);
extern "C" int  PlatformKeyFocus_WindowHasOsFocus(void);

#include "codesynth/CodeSynthVmCtx.h"

// A UI-side copy of the sample: the engine's is freed underneath it on recompile,
// so borrowing it would race.
struct CodeSynthPreviewSmp
{
  cSampleData*     sample = nullptr;
  song_smp_voice_t voice{};
  CodeSynthVmCtx   ctx{};
  std::string      key;               // dir/path/chain the cached sample came from
  ~CodeSynthPreviewSmp() { if (sample) sample_free(sample); }
};

SteepSynthControl::SteepSynthControl(const IRECT& bounds, SteepSynth* pPlugin)
  : IControl(bounds)
  , mPlugin(pPlugin)
  , mEngine(pPlugin ? &pPlugin->Engine() : nullptr)
{
  mSys.malloc  = &std::malloc;
  mSys.realloc = &std::realloc;
  mSys.free    = &std::free;
  mSys.memset  = &std::memset;
  mSys.memcpy  = &std::memcpy;
  mSys.print   = nullptr;
  mSys.error   = nullptr;

  get_font_assembly_line(&mSys, &mFontSettings);
  mFC = font_cache_create(&mSys, &mFontSettings);

  mTM = tm_create(&mSys, mFC, mCols, mRows, 0);
  mUI = ui_create(&mSys, mCols, mRows);

  mUI->global_scale = 1.0f;
  mUI->global_weight = 1.0f;
  mUI->global_line_height = mLineHeight;

  int w = (int)bounds.W();
  int h = (int)bounds.H();
  mBackW = w > 0 ? w : PLUG_WIDTH;
  mBackH = h > 0 ? h : PLUG_HEIGHT;
  mBackScale = 1.f;
  ReallocBackbuffer();
  RebuildUI(PLUG_FPS);

  mSoundEd.owner = this;  mSoundEd.isVisual = false;
  mVisualEd.owner = this; mVisualEd.isVisual = true;
  for (CodeEditor* ed : { &mSoundEd, &mVisualEd })
  {
    ed->ic = interactive_coding_create(&mSys);

    // Wrapped exactly as the real compile wraps it, or its params read as undefined.
    interactive_coding_set_freeform_wrap(ed->ic, kCodeSynthWrapName, kCodeSynthWrapParams, '{', '}');

    // Invoke the body once so the results box shows its value and print() runs. A
    // visual body also paints one frame at t = 0.
    interactive_coding_set_freeform_run_body(ed->ic, true);

    // Register the builtins playback will, per body kind, or the results box lies.
    interactive_coding_set_vm_setup_callback(ed->ic,
        [](VM* vm, void* user) {
          auto* e = static_cast<CodeEditor*>(user);
          // This instance's screen: `width`/`height` are folded and differ per instance.
          RenderCtx* screen = e->owner && e->owner->mPlugin ? e->owner->mEngine->ScreenCtx() : nullptr;
          if (e->isVisual) CodeSynthRegisterCFuncs(vm, screen);
          else             CodeSynthRegisterSmp(vm, screen);

          // Globals resolve through the bound layout only, not through the prelude text.
          if (e->owner)
            if (const cCodeSynthGlobals* g = e->owner->LiveGlobalsFor(e->isVisual))
              g->BindTo(vm);
        }, ed);

    interactive_coding_set_code_changed_callback(ed->ic, &SteepSynthControl::OnCodeChanged, ed);

    interactive_coding_set_wrap_args_callback(ed->ic, &SteepSynthControl::OnWrapArgs, ed);
  }

  // One-row fields: no gutter, no status line.
  for (UITextArea** ta : Fields())
  {
    *ta = ui_textarea_create(&mSys);
    ui_textarea_set_show_line_numbers(*ta, false);
    ui_textarea_set_show_bottom_status(*ta, false);
  }
}

SteepSynthControl::~SteepSynthControl()
{
  // The keyboard hook is thread-scoped and would outlive this control.
  PlatformKeyFocus_SetEditorFocused(0);
  PlatformKeyFocus_SetGraphics(nullptr);

  // Editors first: their VMs borrow the globals blocks.
  interactive_coding_destroy(mSoundEd.ic);
  interactive_coding_destroy(mVisualEd.ic);
  delete mLiveGlobals[0]; mLiveGlobals[0] = nullptr;
  delete mLiveGlobals[1]; mLiveGlobals[1] = nullptr;
  delete mPreviewSmp; mPreviewSmp = nullptr;

  for (UITextArea** ta : Fields())
    ui_textarea_destroy(*ta);

  if (mImageValid && mNVGImage)
  {
    NVGcontext* vg = (NVGcontext*)GetUI()->GetDrawContext();
    if (vg) nvgDeleteImage(vg, mNVGImage);
    mNVGImage = 0;
    mImageValid = false;
  }
  ReleaseScreenImage();
  ui_destroy(mUI);
  tm_destroy(mTM);
  font_cache_destroy(mFC);
  font_settings_free_glyphs(&mSys, &mFontSettings);
  std::free(mBackbuffer);
}

void SteepSynthControl::OnResize()
{
  IRECT r = mRECT;
  int w = (int)r.W();
  int h = (int)r.H();
  if (w != mBackW || h != mBackH)
  {
    mBackW = w; mBackH = h;
    ReallocBackbuffer();
    RebuildUI(PLUG_FPS);
    if (mImageValid && mNVGImage)
    {
      NVGcontext* vg = (NVGcontext*)GetUI()->GetDrawContext();
      if (vg) nvgDeleteImage(vg, mNVGImage);
      mNVGImage = 0; mImageValid = false;
    }
  }
}

void SteepSynthControl::ReallocBackbuffer()
{
  if (mBackbuffer) std::free(mBackbuffer);
  mBackPhysW = (int)(mBackW * mBackScale);
  mBackPhysH = (int)(mBackH * mBackScale);
  if (mBackPhysW < 8) mBackPhysW = 8;
  if (mBackPhysH < 8) mBackPhysH = 8;
  mBackbuffer = (unsigned int*)std::calloc(mBackPhysW * mBackPhysH, 4);
  mImageValid = false;
}

void SteepSynthControl::RebuildUI(float /* fps */)
{
  // Device pixels; only Auto reads the display scale. Everything that sizes a cell
  // must go through font_size, or grid and glyphs disagree.
  const float font_size = kBaseFontSize * mUiScale;

  // Before the cell height and tm_render read it.
  mUI->global_line_height = mLineHeight;

  // Truncated as tm_render does. mCellW/H divide back instead of rounding twice,
  // which would drift mouse rows apart down a tall editor.
  int cell_w = (int)((float)GLYPH_W * font_size);
  int cell_h = (int)((float)GLYPH_H * font_size * mUI->global_line_height);
  if (cell_w < 1) cell_w = 1;
  if (cell_h < 1) cell_h = 1;

  mScreenZoomDev = kScreenZoom < 1 ? 1 : kScreenZoom;

  // Per-instance live width. A floating preview reserves no strip.
  const int screenW = vscreen_width_ctx(mPlugin ? mEngine->ScreenCtx() : nullptr);
  mScreenPanelW = Ui().screenDocked
      ? (float)(screenW * mScreenZoomDev) / mBackScale + kScreenMargin * 2
      : 0.f;

  // Columns from what the screen panel leaves, so the UI never draws under it.
  int ui_w = mBackPhysW - (int)(mScreenPanelW * mBackScale + 0.5f);
  if (ui_w < 64) ui_w = 64;

  int new_cols = ui_w / cell_w;
  int new_rows = mBackPhysH / cell_h;
  if (new_cols < 8) new_cols = 8;
  if (new_rows < 4) new_rows = 4;

  mCols = new_cols;
  mRows = new_rows;
  mCellW = (float)cell_w / mBackScale;
  mCellH = (float)cell_h / mBackScale;

  font_cache_clear(mFC);
  tm_resize(mTM, mCols, mRows, ui_theme_color(mUI, UI_COL_DEFAULT));
  ui_resize(mUI, mCols, mRows);
  ui_set_cell_size(mUI, mCellW, mCellH);
}

CodeSynthUiState& SteepSynthControl::Ui() const
{
  static CodeSynthUiState sNone;
  return mPlugin ? mPlugin->UiState() : sNone;
}

static float ReadParam(SteepSynth* plugin, int paramIdx)
{
  if (!plugin) return 0.f;
  IParam* p = plugin->GetParam(paramIdx);
  return p ? (float)p->GetNormalized() : 0.f;
}

static void WriteParam(SteepSynth* plugin, IGraphics* gfx, int paramIdx, float normVal)
{
  if (!plugin || !gfx) return;
  IParam* p = plugin->GetParam(paramIdx);
  if (!p) return;
  if (normVal < 0.f) normVal = 0.f;
  if (normVal > 1.f) normVal = 1.f;
  gfx->GetDelegate()->SendParameterValueFromUI(paramIdx, (double)normVal);
}

// Same whitespace the entry splitter trims, so a round-trip doesn't look like an
// external edit.
static std::string Trimmed(const std::string& s)
{
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// `body` is the bare entry source, valid only for this call.
void SteepSynthControl::OnCodeChanged(const char* body, void* user)
{
  CodeEditor* ed = static_cast<CodeEditor*>(user);
  ed->owner->PushLiveEdit(body, *ed);
}

// Arguments for the wrapped body's live and hover runs: the visual pipeline's
// last values for the open entry, which run on the UI thread in step with the
// audio. Returning 0 keeps the zeros.
int SteepSynthControl::OnWrapArgs(const char* params, char* out, int outMax, void* user)
{
  CodeEditor* ed = static_cast<CodeEditor*>(user);
  if (!ed || !ed->owner || !ed->owner->mPlugin || !params || !out || outMax <= 0)
    return 0;

  // Only for the voice wrap's arity.
  int arity = params[0] ? 1 : 0;
  for (const char* p = params; *p; p++) if (*p == ',') arity++;
  if (arity != kCodeSynthWrapParamCount) return 0;

  double v[kCodeSynthWrapParamCount];
  if (!ed->owner->mEngine->CodeSynthLastVoiceArgs(v)) return 0;

  // %.10g: short for whole values, exact enough to rerun what the engine ran, and
  // accepted by every wrap domain.
  int off = 0;
  for (int i = 0; i < kCodeSynthWrapParamCount; i++)
  {
    // NaN/inf have no literal spelling.
    if (!std::isfinite(v[i])) return 0;
    int n = snprintf(out + off, (size_t)(outMax - off), "%s%.10g", i ? ", " : "", v[i]);
    if (n < 0 || off + n >= outMax) return 0;   // truncated: same fallback
    off += n;
  }
  return 1;
}

void SteepSynthControl::PushLiveEdit(const char* liveBody, CodeEditor& ed)
{
  if (!mPlugin || !ed.ic || !liveBody) return;
  if (mSelectedSound < 0 || mSelectedSound != ed.loadedSound) return;

  CodeSynthType type = mEngine->Entry(mSelectedSound).type;
  const bool isPrelude = CodeSynthEntryIsReserved(mEngine->Entry(mSelectedSound));
  std::string body = liveBody;

  ed.lastSyncedBody = Trimmed(body);
  mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { (ed.isVisual ? e.visualBody : e.body) = body; });

  if (!ed.isVisual && !isPrelude)
    RenderCodeSynthPreview(body, (int)type);
}

const cCodeSynthGlobals* SteepSynthControl::LiveGlobalsFor(bool forVisual)
{
  if (!mPlugin) return nullptr;
  const int i = forVisual ? 1 : 0;

  // Common carries the directives the declarations are read under, so both key the cache.
  std::string key = CodeSynthCommonBody(mEngine->Entries());
  key += "\n//--globals--\n";
  key += CodeSynthGlobalsBody(mEngine->Entries(), forVisual);

  if (mLiveGlobals[i] && mLiveGlobalsKey[i] == key)
    return mLiveGlobals[i]->Empty() ? nullptr : mLiveGlobals[i];

  if (!mLiveGlobals[i]) mLiveGlobals[i] = new cCodeSynthGlobals();
  mLiveGlobals[i]->Build(CodeSynthCommonBody(mEngine->Entries()),
                         CodeSynthGlobalsBody(mEngine->Entries(), forVisual),
                         mEngine->ScreenCtx());
  mLiveGlobalsKey[i] = key;
  return mLiveGlobals[i]->Empty() ? nullptr : mLiveGlobals[i];
}

// Cached on the WAV directive: a load decodes and recompresses the whole file.
// Always returns a ctx, since the natives read user_data unconditionally.
CodeSynthVmCtx* SteepSynthControl::PreviewSmpCtx(const std::string& body, double hostSr)
{
  if (!mPreviewSmp) mPreviewSmp = new CodeSynthPreviewSmp();

  std::string wavPath, wavChain;
  CodeSynthGetWavPath(body, wavPath, wavChain);
  const std::string dir = mPlugin ? mEngine->AssetDir() : std::string();
  const std::string key = dir + "\n" + wavPath + "\n" + wavChain;

  if (key != mPreviewSmp->key)
  {
    if (mPreviewSmp->sample) { sample_free(mPreviewSmp->sample); mPreviewSmp->sample = nullptr; }
    // malloc can reuse the freed address, and song_smp_bind only resets on a
    // changed pointer.
    mPreviewSmp->voice = song_smp_voice_t{};
    mPreviewSmp->sample = CodeSynthLoadBodySample(body, dir.c_str());
    mPreviewSmp->key = key;
  }

  smp_bind_voice(&mPreviewSmp->voice, mPreviewSmp->sample, hostSr);
  mPreviewSmp->ctx.smp = &mPreviewSmp->voice;
  // No screen: an audio body that draws must not paint while you type.
  mPreviewSmp->ctx.screen = nullptr;
  return &mPreviewSmp->ctx;
}

void SteepSynthControl::RenderCodeSynthPreview(const std::string& body, int codeSynthType)
{
  mPreviewValidCount = 0;
  mPreviewStereo = false;
  if (!Ui().previewEnabled) return;
  CodeSynthType type = (CodeSynthType)codeSynthType;

  VM* vm = nullptr;
  Parser* parser = nullptr;
  ParseResult* res = nullptr;
  std::string err;
  std::string prelude = mPlugin ? CodeSynthPreludeFor(mEngine->Entries(), /*forVisual=*/false) : std::string();

  // Reset: the body is run and may write to it.
  const cCodeSynthGlobals* previewGlobals = LiveGlobalsFor(/*forVisual=*/false);
  if (previewGlobals) const_cast<cCodeSynthGlobals*>(previewGlobals)->ResetValues();

  Func* fn = CodeSynthCompileForExport(body, CodeSynthWrapParamsForType(type), prelude.c_str(),
                                       previewGlobals, mPlugin ? mEngine->ScreenCtx() : nullptr,
                                       &vm, &parser, &res, err);
  if (!fn) return;

  unsigned char frame[kCodeSynthVoiceFrameBytes];
  if (func_frame_size(fn) > sizeof(frame)) { CodeSynthReleaseExport(vm, parser, res); return; }
  VMType rt = func_return_type(fn);

  // The host rate, so smp() reads what the audio thread would.
  const double previewSr = (mPlugin && mEngine->ExportSampleRate() > 0.0)
                         ? mEngine->ExportSampleRate() : 44100.0;
  CodeSynthVmCtx* smpCtx = PreviewSmpCtx(body, previewSr);
  vm_set_user_data(vm, smpCtx);

  const double hz = 440.0;  // A4
  const double bpm = (mPlugin && mEngine->GetTempo() > 0.0) ? mEngine->GetTempo() : 120.0;
  const double beatsPerSecond = bpm / 60.0;
  const double sixteenthSeconds = 60.0 / bpm / 4.0;
  float win16 = clamp(Ui().previewWindow16ths, kPreviewWindowMin16ths, kPreviewWindowMax16ths);
  double durationSeconds = sixteenthSeconds * (double)win16;

  // One sample per pixel column.
  int editorWCells = mCols - kLeftColumnWidth;
  if (editorWCells < 1) editorWCells = 1;
  int sampleCount = (int)((float)editorWCells * (float)mCellW * mBackScale + 0.5f);
  if (sampleCount < 2) sampleCount = 2;
  if (sampleCount > kMaxPreviewSamples) sampleCount = kMaxPreviewSamples;
  mPreviewSamples.resize(sampleCount);

  // Same test as cCodeSynthVoice::stereo.
  const bool stereo = func_param_used(fn, kCodeSynthPanParamIndex) != 0;
  mPreviewSamplesR.resize(stereo ? sampleCount : 0);

  Args a;
  args_bind(&a, fn, frame, sizeof(frame));
  for (int i = 0; i < sampleCount; i++)
  {
    double t = (double)i / (double)(sampleCount - 1) * durationSeconds;
    // smp() derives its position from note_sample, so decimated points still read
    // the right sample.
    if (smpCtx && smpCtx->smp) smpCtx->smp->note_sample = (int)llround(t * previewSr);
    // The root note, fully struck, on the beat. Each channel sets every argument,
    // as playback does.
    double args[kCodeSynthWrapParamCount] = { t, hz, t * hz, 69.0, t * beatsPerSecond, 1.0, 127.0, stereo ? -1.0 : 0.0 };
    CodeSynthSetArgs(&a, type, args);
    mPreviewSamples[i] = CodeSynthRunToFloat(fn, &a, rt);
    if (stereo)
    {
      args[kCodeSynthPanParamIndex] = 1.0;
      CodeSynthSetArgs(&a, type, args);
      mPreviewSamplesR[i] = CodeSynthRunToFloat(fn, &a, rt);
    }
  }
  mPreviewValidCount = sampleCount;

  // Only when the channels actually differ.
  mPreviewStereo = false;
  if (stereo)
  {
    for (int i = 0; i < sampleCount; i++)
      if (mPreviewSamples[i] != mPreviewSamplesR[i]) { mPreviewStereo = true; break; }
  }

  CodeSynthReleaseExport(vm, parser, res);
}

// Peak-normalized into the rect, in mTM's physical pixels.
void SteepSynthControl::PushPreviewWaveformPath(int col, int row, int w, int h, float alpha)
{
  if (mPreviewValidCount < 2) return;

  float pcw = (float)mCellW * mBackScale;
  float pch = (float)mCellH * mBackScale;
  float x0 = (float)col * pcw;
  float y0 = (float)row * pch;
  float w0 = (float)w * pcw;
  float h0 = (float)h * pch;
  const float marginY = 3.0f;
  float mid = y0 + h0 * 0.5f;
  float amp = h0 * 0.5f - marginY;
  if (amp < 1.0f) amp = 1.0f;

  // One peak for both channels, so relative levels stay readable.
  const bool stereo = mPreviewStereo && (int)mPreviewSamplesR.size() >= mPreviewValidCount;
  float peak = 1e-6f;
  for (int i = 0; i < mPreviewValidCount; i++)
  {
    float v = mPreviewSamples[i];
    if (v < 0.0f) v = -v;
    if (v > peak) peak = v;
    if (stereo)
    {
      float r = mPreviewSamplesR[i];
      if (r < 0.0f) r = -r;
      if (r > peak) peak = r;
    }
  }

  // `color` is a tm_line ramp: 0 gray, 1 cold, 2 warm/red, 3 green.
  auto pushTrace = [&](const std::vector<float>& samples, int color)
  {
    for (int i = 0; i < mPreviewValidCount; i++)
    {
      float t = (float)i / (float)(mPreviewValidCount - 1);
      float x = x0 + t * w0;
      float yNorm = clamp(samples[i] / peak, -1.0f, 1.0f);
      float y = mid - yNorm * amp;
      if (i == 0) tm_move_to(mTM, x, y, alpha, color);
      else        tm_line_to(mTM, x, y, alpha, color);
    }
  };

  if (stereo)
  {
    pushTrace(mPreviewSamples,  2); // left  -- red
    pushTrace(mPreviewSamplesR, 3); // right -- green
  }
  else
  {
    pushTrace(mPreviewSamples, 0);
  }

  tm_move_to(mTM, x0     , mid, alpha * 0.5f, 0);
  tm_line_to(mTM, x0 + w0, mid, alpha * 0.5f, 0);
}

// Polyphony in the exporter's sense (rows packed by FirstFitAssign), bucketed:
// up to 4 is band 0, then one band per channel up to 8.
static int MatrixDensityBand(int channels)
{
  if (channels >= 8) return 4;
  if (channels >= 5) return channels - 4;  // 5 -> 1, 6 -> 2, 7 -> 3
  return 0;
}

// Clamped: 3x a base alpha above 1/3 would saturate in tm_line_push.
static void MatrixDensityStyle(int band, float baseAlpha, int* color, float* outAlpha)
{
  switch (band)
  {
    case 1:  *color = 3; *outAlpha = baseAlpha * 2.0f; break;  // 5 channels: green, 2x
    case 2:  *color = 2; *outAlpha = baseAlpha * 3.0f; break;  // 6 channels: red, 3x
    case 3:  *color = 2; *outAlpha = baseAlpha * 3.0f; break;  // 7 channels: red, 3x
    case 4:  *color = 2; *outAlpha = 1.0f;             break;  // 8+ channels: red, full
    default: *color = 0; *outAlpha = baseAlpha;        break;  // <= 4 channels
  }
  *outAlpha = clamp(*outAlpha, 0.0f, 1.0f);
}

// Piano-roll of the recorded matrix. A run is split wherever its density band changes.
void SteepSynthControl::PushMatrixPath(int col, int row, int w, int h, float alpha)
{
  if (!mPlugin) return;
  int rows = mEngine->MatrixRowCount();
  int cols = mEngine->MatrixColCount();
  if (rows < 1 || cols < 1) return;

  std::vector<unsigned short> chans((size_t)cols, 0);
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      if (mEngine->MatrixCellActive(r, c)) chans[(size_t)c]++;

  float pcw = (float)mCellW * mBackScale;
  float pch = (float)mCellH * mBackScale;
  float x0 = (float)col * pcw;
  float y0 = (float)row * pch;
  float w0 = (float)w * pcw;
  float h0 = (float)h * pch;
  const float marginY = 2.0f;
  float usableH = h0 - 2.0f * marginY;
  if (usableH < 1.0f) usableH = 1.0f;
  float rowheight = usableH / rows;

  for (int r = 0; r < rows; r++)
  {
    float t = (rows > 1) ? ((float)r + 0.5f) / (float)rows : 0.5f;
    float y = y0 + marginY + t * usableH;

    int c = 0;
    while (c < cols)
    {
      if (!mEngine->MatrixCellActive(r, c)) { c++; continue; }
      // aaline takes its colour from the segment start, so each band change needs its
      // own segment.
      while (c < cols && mEngine->MatrixCellActive(r, c))
      {
        int segStart = c;
        int band = MatrixDensityBand(chans[(size_t)c]);
        while (++c < cols && mEngine->MatrixCellActive(r, c) &&
               MatrixDensityBand(chans[(size_t)c]) == band) {}
        int lineColor; float lineAlpha;
        MatrixDensityStyle(band, alpha, &lineColor, &lineAlpha);
        float xa = x0 + ((float)segStart / (float)cols) * w0;
        float xb = x0 + ((float)c        / (float)cols) * w0;
        if(lineAlpha > 0.6f) lineAlpha = 0.6f;
        tm_move_to(mTM, xa, y, lineAlpha, lineColor);
        tm_line_to(mTM, xb, y, lineAlpha, lineColor);
        if(band > 0) {
          tm_move_to(mTM, xa, y+1.f      , lineAlpha, lineColor);
          tm_line_to(mTM, xa, y+band*rowheight, lineAlpha, lineColor);
          tm_move_to(mTM, xb, y+1.f      , lineAlpha, lineColor);
          tm_line_to(mTM, xb, y+band*rowheight, lineAlpha, lineColor);
        }
      }
    }
  }
}

void SteepSynthControl::DrawTransportButtons()
{
  bool recording = mPlugin && mEngine->IsRecording();
  bool playing   = mPlugin && mEngine->IsPlaying();
  bool hasMatrix = mPlugin && mEngine->HasRecordedMatrix();
  // AssetDir(), not the path: a bare filename has no directory.
  bool haveDir   = mPlugin && !mEngine->AssetDir().empty();

  ui_label(mUI, "Sequence:");
  ui_same_line(mUI);

  // One label in both states: a wider one would shift later buttons and rehash the id.
  if (ui_toggle_button(mUI, "Record", recording) && mPlugin)
  {
    if (recording) mEngine->StopRecording();
    else mEngine->StartRecording();
  }

  ui_same_line(mUI);
  const char* playLabel = playing ? "Stop" : "Play";
  if (!hasMatrix) ui_begin_disabled(mUI);
  if (ui_button(mUI, playLabel) && mPlugin && hasMatrix)
  {
    if (playing) mEngine->StopPlayback();
    else mEngine->StartPlayback();
  }
  if (!hasMatrix) ui_end_disabled(mUI);

  // "##seq" keeps these ids apart from the File row's Save/Load.
  if (!haveDir) ui_begin_disabled(mUI);

  ui_same_line(mUI);
  if (ui_button(mUI, "Save##seq") && mPlugin && hasMatrix && haveDir)
    mEngine->SaveSeq();

  ui_same_line(mUI);
  if (ui_button(mUI, "Load##seq") && mPlugin && haveDir)
    mEngine->LoadSeq();

  if (!haveDir) ui_end_disabled(mUI);
}

// The selected entry's header fields as one row, in the text format's field order.
void SteepSynthControl::DrawCodeSynthHeaderRow(int col, int row, int w)
{
  if (!mPlugin || mSelectedSound < 0) return;
  int i = mSelectedSound;

  // The common block has none of these fields.
  if ((mEngine->Entry(i).kind == kEntryCommon))
  {
    ui_set_cursor(mUI, (float)col, (float)row);
    ui_label(mUI, "common -- shared code, compiled into every event below");
    (void)w;
    return;
  }

  constexpr int kTransRange = 24; // transpose spans +-24 semitones (+-2 octaves)
  constexpr int kOrderRange = 10; // order dropdown spans -10..10 (0 = unconstrained)
  static char        chanBuf[16][4];
  static const char* chanItems[16];
  static char        orderBuf[2 * kOrderRange + 1][8];
  static const char* orderItems[2 * kOrderRange + 1];
  static const char* typeItems[3] = { "f64", "f32", "fix" };
  static bool inited = false;
  if (!inited)
  {
    for (int k = 0; k < 16;  k++) { s_snprintf(chanBuf[k], sizeof(chanBuf[k]), "%d", k + 1); chanItems[k] = chanBuf[k]; }
    for (int k = 0; k < 2 * kOrderRange + 1; k++) { s_snprintf(orderBuf[k], sizeof(orderBuf[k]), "%d", k - kOrderRange); orderItems[k] = orderBuf[k]; }
    inited = true;
  }

  // Fixed columns, so a value changing width never shifts its neighbours.
  const int kColName = 0;   // name, padded to 9 chars
  const int kColChan = 10;  // "channel: NN v"
  const int kColNote = 25;  // "note: NNN v"
  const int kColType = 37;  // "type: fix v"
  const int kColTran = 51;  // "transpose: -NN v"
  const int kColOrder = 68; // "order: -NN v"

  DrawCodeSynthNameField(col + kColName, row, kColChan - kColName - 1);

  // "##h..." keeps the id distinct from the list's per-row dropdown.
  ui_set_cursor(mUI, (float)(col + kColChan), (float)row);
  int chanIdx = mEngine->Entry(i).channel - 1;
  if (chanIdx < 0) chanIdx = 0;
  if (chanIdx > 15) chanIdx = 15;
  if (ui_dropdown(mUI, "channel##hchan", &chanIdx, 16, chanItems, 0))
    mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.channel = chanIdx + 1; });

  ui_set_cursor(mUI, (float)(col + kColNote), (float)row);
  ui_label(mUI, "note:");
  int noteVal = mEngine->Entry(i).note;
  if (ui_textarea_int_absolute_pos(mUI, mNoteTA, &noteVal, 0, 127, col + kColNote + 6, row, 4))
    mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.note = noteVal; });

  ui_set_cursor(mUI, (float)(col + kColType), (float)row);
  int typeIdx = (int)mEngine->Entry(i).type;
  if (ui_dropdown(mUI, "type##htype", &typeIdx, 3, typeItems, 0))
  {
    mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.type = (CodeSynthType)typeIdx; });
    // Both bodies compile under the entry's one type.
    const char* params = CodeSynthWrapParamsForType((CodeSynthType)typeIdx);
    interactive_coding_set_freeform_wrap(mSoundEd.ic, kCodeSynthWrapName, params, '{', '}');
    interactive_coding_set_freeform_wrap(mVisualEd.ic, kCodeSynthWrapName, params, '{', '}');
  }

  // Out-of-range values clamp for display only.
  ui_set_cursor(mUI, (float)(col + kColTran), (float)row);
  ui_label(mUI, "transpose:");
  int transVal = mEngine->Entry(i).transpose;
  if (ui_textarea_int_absolute_pos(mUI, mTransposeTA, &transVal, -kTransRange, kTransRange, col + kColTran + 11, row, 5))
    mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.transpose = transVal; });

  // Of two events on one tick, the lower non-zero order plays first; 0 is unconstrained.
  ui_set_cursor(mUI, (float)(col + kColOrder), (float)row);
  int orderIdx = mEngine->Entry(i).order + kOrderRange;
  if (orderIdx < 0) orderIdx = 0;
  if (orderIdx > 2 * kOrderRange) orderIdx = 2 * kOrderRange;
  if (ui_dropdown(mUI, "order##horder", &orderIdx, 2 * kOrderRange + 1, orderItems, 0))
    mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.order = orderIdx - kOrderRange; });

  (void)w;
}

// Acts only when the plugin's revision moved.
void SteepSynthControl::RestoreUiState()
{
  if (!mPlugin) return;

  // Otherwise the name field grabs default focus on the first frame.
  if (mFirstCodeSynthFrame)
  {
    mFirstCodeSynthFrame = false;
    mPendingFocusPane = kFocusSoundEditor;
    mPendingCursorRow = 0;
    mPendingCursorCol = 0;
  }

  unsigned int rev = mPlugin->UiStateRevision();
  if (rev == mUiStateSeenRevision) return;

  const CodeSynthUiState& st = mPlugin->UiState();

  // A master file's entry list can arrive on a later idle tick, and a selection
  // clamped against an empty list would stick at -1. Stay pending until it's there.
  int n = mEngine->EntryCount();
  if (n == 0 && st.selectedEntry >= 0) return;

  mUiStateSeenRevision = rev;

  mSelectedSound = (st.selectedEntry >= 0 && st.selectedEntry < n) ? st.selectedEntry
                 : (n > 0 ? 0 : -1);

  if (mSoundEd.loadedSound  == mSelectedSound) ApplyEditorScroll(mSoundEd);
  if (mVisualEd.loadedSound == mSelectedSound) ApplyEditorScroll(mVisualEd);

  // Applied later: the editors still hold the previous selection's text.
  mPendingFocusPane = st.focusPane;
  mPendingCursorRow = st.cursorRow;
  mPendingCursorCol = st.cursorCol;
}

void SteepSynthControl::RecordUiState()
{
  if (!mPlugin) return;

  // A restore still pending must not be overwritten.
  if (mPlugin->UiStateRevision() != mUiStateSeenRevision) return;

  CodeSynthUiState& st = mPlugin->UiState();

  if (mPendingFocusPane >= 0)
  {
    // The saved pane may not be drawn for this entry.
    if (mPendingFocusPane == kFocusSoundEditor && mSoundEd.loadedSound < 0)
      mPendingFocusPane = kFocusNameField;
    if (mPendingFocusPane == kFocusVisualEditor && mVisualEd.loadedSound < 0)
      mPendingFocusPane = kFocusNameField;

    switch (mPendingFocusPane)
    {
      case kFocusNameField:
        if (mNameTA)
        {
          mUI->focus_id = ui_id_from_ptr(mNameTA);
          ui_textarea_set_cursor(mNameTA, mPendingCursorRow, mPendingCursorCol);
        }
        break;
      case kFocusVisualEditor:
        interactive_coding_set_editor_focus(mVisualEd.ic);
        interactive_coding_set_cursor(mVisualEd.ic, mPendingCursorRow, mPendingCursorCol);
        break;
      case kFocusSoundEditor:
      default:
        interactive_coding_set_editor_focus(mSoundEd.ic);
        interactive_coding_set_cursor(mSoundEd.ic, mPendingCursorRow, mPendingCursorCol);
        break;
    }
    mPendingFocusPane = -1;
  }

  st.selectedEntry = mSelectedSound;

  StashEditorScroll(mSoundEd);
  StashEditorScroll(mVisualEd);

  // Only while something holds the keyboard, so a button click doesn't save "no caret".
  if (mNameTA && mUI->focus_id == ui_id_from_ptr(mNameTA))
  {
    st.focusPane = kFocusNameField;
    ui_textarea_get_cursor(mNameTA, &st.cursorRow, &st.cursorCol);
  }
  else if (interactive_coding_has_editor_focus(mVisualEd.ic))
  {
    st.focusPane = kFocusVisualEditor;
    interactive_coding_get_cursor(mVisualEd.ic, &st.cursorRow, &st.cursorCol);
  }
  else if (interactive_coding_has_editor_focus(mSoundEd.ic))
  {
    st.focusPane = kFocusSoundEditor;
    interactive_coding_get_cursor(mSoundEd.ic, &st.cursorRow, &st.cursorCol);
  }
}

// A selection change starts a fresh undo history. SetEntryName's rewrites of
// header-breaking characters come back through the model on the next frame.
void SteepSynthControl::DrawCodeSynthNameField(int col, int row, int w)
{
  if (!mPlugin || mSelectedSound < 0 || w < 2) return;

  const char* model = mEngine->Entry(mSelectedSound).name.c_str();
  if (mNameLoadedSound != mSelectedSound)
  {
    ui_textarea_set_text(mNameTA, model, true);
    mNameLoadedSound = mSelectedSound;
  }
  if (const char* text = ui_textarea_str_absolute_pos(mUI, mNameTA, model, "\r\n", col, row, w))
    mEngine->SetEntryName(mSelectedSound, text);
}

// The checkbox only decides whether external changes to the file overwrite
// state. Save sits left of it: ticking the box discards a differing editor copy.
void SteepSynthControl::DrawCodeFileRow()
{
  if (!mPlugin || !mUI) return;

  const bool havePath = mEngine->GetCodeFilePath()[0] != '\0';
  
  if (!havePath) ui_begin_disabled(mUI);

  ui_label(mUI, "File:");

  // "##" keeps ids apart from the Sequence row's Save/Load.

  ui_same_line(mUI);
  if (ui_button(mUI, "Save##file") && havePath)
    mEngine->SaveCodeToFile();

  ui_same_line(mUI);
  if (ui_button(mUI, "Save as C") && havePath && mEngine->HasRecordedMatrix())
    mPlugin->SaveCSong();

  ui_same_line(mUI);
  if (ui_button(mUI, "Load##file") && havePath)
    ReloadAllEditorsFromModel();

  ui_same_line(mUI);
  int master = mEngine->GetFileIsMaster() ? 1 : 0;
  if (ui_check_box(mUI, "file is master", &master))
  {
    mEngine->SetFileIsMaster(master != 0);
  }

  if (!havePath) ui_end_disabled(mUI);

  ui_same_line(mUI);
  if (ui_button(mUI, "..."))
    PromptForCodeFile();

  ui_same_line(mUI);
  DrawCodeFilePathField();
}

// Controls that don't apply to the selected target are disabled, not hidden, so
// the layout stays still.
void SteepSynthControl::DrawSettingsPage()
{
  if (!mPlugin || !mUI) return;

  ExportSettings ex = mPlugin->GetExportSettings();
  bool changed = false;

  ui_set_cursor(mUI, 3, (float)kActionRow);

  const int kValueCol = 12;

  if (ui_button(mUI, "< Back")) mShowSettings = false;

  ui_same_line(mUI);
  ui_label(mUI, "  SETTINGS & EXPORT");

  ui_blank_row(mUI);
  {
    int clip = mPlugin->GetClipOutput() ? 1 : 0;
    if (ui_check_box(mUI, "Clip Output", &clip))
      mPlugin->SetClipOutput(clip != 0);
  }

  // Not "Backend": that names the engine switch on the main screen.
  ui_blank_row(mUI);
  ui_label(mUI, "Target:");
  {
    static const char* const kTargetItems[5] = { "pebble", "win32", "microw8", "WAV", "WAV mono" };
    int idx = (ex.target == kExportTargetWin32)     ? 1
            : (ex.target == kExportTargetMicrow8)   ? 2
            : (ex.target == kExportTargetWavStereo) ? 3
            : (ex.target == kExportTargetWavMono)   ? 4 : 0;
    ui_same_line_col(mUI, kValueCol);
    if (ui_option_bar(mUI, "##target", &idx, 5, kTargetItems, 0))
    {
      ex.target = (idx == 1) ? kExportTargetWin32
                : (idx == 2) ? kExportTargetMicrow8
                : (idx == 3) ? kExportTargetWavStereo
                : (idx == 4) ? kExportTargetWavMono
                             : kExportTargetPebble;
      changed = true;
    }
  }

  const bool isPebble = (ex.target == kExportTargetPebble);
  const bool isWin32  = (ex.target == kExportTargetWin32);
  const bool isWav    = ExportTargetIsWav(ex.target);

  ui_blank_row(mUI);
  ui_label(mUI, "Folder:");
  ui_same_line_col(mUI, kValueCol);
  if (ui_button(mUI, "Browse")) PromptForExportFolder();

  // Full width on its own row: paths have no useful length bound. Too narrow,
  // it still takes the row, so nothing below shifts.
  const int dirW = (int)ui_avail_width(mUI);
  if (dirW <= 8)
    ui_blank_row(mUI);
  else if (const char* v = ui_textarea_str(mUI, mExportDirTA, ex.destDir.c_str(), "\r\n", dirW))
  { ex.destDir = v; changed = true; }

  ui_blank_row(mUI);
  ui_label(mUI, "Name:");
  ui_same_line_col(mUI, kValueCol);
  if (const char* v = ui_textarea_str(mUI, mExportNameTA, ex.name.c_str(), "\r\n", 28))
  { ex.name = v; changed = true; }

  // Pebble only; blank exports as "codesynth".
  if (!isPebble) ui_begin_disabled(mUI);
  ui_label(mUI, "Author:");
  ui_same_line_col(mUI, kValueCol);
  if (const char* v = ui_textarea_str(mUI, mExportAuthorTA, ex.author.c_str(), "\r\n", 28))
  { ex.author = v; changed = true; }
  if (!isPebble) ui_end_disabled(mUI);

  // The watch keys an app on package.json's uuid: keep it to update the installed
  // app, change it to get a second copy. Blank reuses the destination's id or mints one.
  if (!isPebble) ui_begin_disabled(mUI);

  // Measured before the label: ui_avail_width counts from the pen. Generate is
  // dropped when the row can't hold both.
  const int kUuidW = 38;
  const bool uuidBtnFits = ((int)ui_avail_width(mUI) >= kValueCol + kUuidW + 1 + 10);

  ui_label(mUI, "App id:");
  ui_same_line_col(mUI, kValueCol);
  // Pasted ids arrive with newlines and spaces that would fail validation.
  if (const char* v = ui_textarea_str(mUI, mExportUuidTA, ex.uuid.c_str(), "\r\n \t", kUuidW))
  { ex.uuid = v; changed = true; }
  if (uuidBtnFits)
  {
    ui_same_line(mUI);
    if (ui_button(mUI, "Generate"))
    {
      ex.uuid = ExportMakeUuid();
      changed = true;
    }
  }
  if (!isPebble) ui_end_disabled(mUI);

  // Drawn into the separating blank row so the layout doesn't jump while typing.
  if (isPebble && !ex.uuid.empty() && !ExportUuidLooksValid(ex.uuid))
  {
    ui_indent(mUI, kValueCol);
    ui_label(mUI, "not a uuid -- want 8-4-4-4-12 hex");
  }
  else
  {
    ui_blank_row(mUI);
  }

  if (!isWin32) ui_begin_disabled(mUI);
  ui_label(mUI, "Size:");
  if (mExportWTA && mExportHTA)
  {
    int w = 0, h = 0;
    ExportTargetScreenSize(ex, w, h);
    ui_same_line_col(mUI, kValueCol);
    if (ui_textarea_int(mUI, mExportWTA, &w, 16, VSCREEN_MAX_W, 5))
    { ex.width = w; changed = true; }
    ui_same_line(mUI);
    ui_label(mUI, "x");
    ui_same_line(mUI);
    if (ui_textarea_int(mUI, mExportHTA, &h, 16, VSCREEN_MAX_H, 5))
    { ex.height = h; changed = true; }
  }
  if (!isWin32) ui_end_disabled(mUI);

  if (!(isWin32 || isWav)) ui_begin_disabled(mUI);
  ui_label(mUI, "Rate:");
  if (mExportRateTA)
  {
    // A pinned target shows its pinned rate.
    int rate = ExportTargetSampleRate(ex);
    ui_same_line_col(mUI, kValueCol);
    if (ui_textarea_int(mUI, mExportRateTA, &rate, 4000, 192000, 7))
    { ex.sampleRate = rate; changed = true; }
  }
  if (!(isWin32 || isWav)) ui_end_disabled(mUI);

  ui_label(mUI, "Audio loop:");
  {
    static const char* kSndLoopItems[5] = { "auto", "per event", "per instrument", "combined", "sample by sample" };
    static const unsigned kSndLoopShapes[5] = { kSongSndLoopAuto, kSongSndLoopInlined, kSongSndLoopGrouped, kSongSndLoopShared, kSongSndLoopPerSample };
    int idx = 0;
    for (int k = 0; k < 5; k++) if (ex.sndLoopShape == kSndLoopShapes[k]) idx = k;
    ui_same_line_col(mUI, kValueCol);
    ui_push_item_width(mUI, 19);
    if (ui_dropdown(mUI, "##sndloop", &idx, 5, kSndLoopItems, UI_DROPDOWN_HIDE_NAME))
    {
      ex.sndLoopShape = kSndLoopShapes[idx];
      changed = true;
    }
    ui_pop_item_width(mUI);
  }

  ui_blank_row(mUI);
  {
    int align = ex.alignBpmToBuffers ? 1 : 0;
    if (ui_check_box(mUI, "Adjust export bpm to align buffers", &align))
    {
      ex.alignBpmToBuffers = (align != 0);
      changed = true;
    }
  }
  {
    ui_indent(mUI, 2);
    const ExportBpmAlign plan = ExportPlanBpm(ex, mEngine->ExportSongTick(),
                                              mEngine->ExportSampleRate(),
                                              SteepSynthEngine::kBlockSize);
    char line[96];
    if (plan.changed)
      snprintf(line, sizeof(line), "Exported bpm: %.6g (originally %.6g)",
               plan.bpm, plan.requestedBpm);
    else
      snprintf(line, sizeof(line), "Exported bpm: %.6g (unchanged)", plan.bpm);
    ui_label(mUI, line);
  }

  if (isPebble)
  {
    ui_blank_row(mUI);
    int filt = ex.speakerFilterDefault ? 1 : 0;
    if (ui_check_box(mUI, "Speaker filter on by default", &filt))
    {
      ex.speakerFilterDefault = (filt != 0);
      changed = true;
    }
  }

  if (isWav)
  {
    ui_blank_row(mUI);
    ui_label(mUI, "Wav export requires clang");
    int rm = ex.wavRemoveSource ? 1 : 0;
    if (ui_check_box(mUI, "Remove source code after wav is exported", &rm))
    {
      ex.wavRemoveSource = (rm != 0);
      changed = true;
    }
  }

  ui_blank_row(mUI);
  const bool canExport = !ex.destDir.empty() && mEngine->HasRecordedMatrix();
  if (ui_button(mUI, "EXPORT") && canExport)
  {
    if (changed) { mPlugin->SetExportSettings(ex); changed = false; }
    DoExportProject();
  }
  ui_same_line(mUI);
  if (!mEngine->HasRecordedMatrix())  ui_label(mUI, " nothing recorded to export");
  else if (ex.destDir.empty())        ui_label(mUI, " set a folder first");

  if (!mExportStatus.empty())
  {
    ui_blank_row(mUI);
    ui_label(mUI, mExportOk ? "OK:" : "FAILED:");
    ui_same_line_col(mUI, 8);
    ui_label(mUI, mExportStatus.c_str());
  }

  if (changed) mPlugin->SetExportSettings(ex);
}

void SteepSynthControl::PromptForExportFolder()
{
  if (!mPlugin) return;
  ExportSettings ex = mPlugin->GetExportSettings();
  std::string chosen;
  if (!PlatformFolderPicker_Choose(ex.destDir, chosen)) return; // cancelled
  ex.destDir = chosen;
  mPlugin->SetExportSettings(ex);
}

void SteepSynthControl::DoExportProject()
{
  if (!mPlugin) return;
  std::string msg;
  mExportOk = mPlugin->ExportProject(msg);
  mExportStatus = msg;
}

void SteepSynthControl::DrawCodeFilePathField()
{
  mPathFieldW = 0; // "not drawn" until proven otherwise -- see OnDropAt
  if (!mPlugin) return;

  // Leave a margin so the field can't run under the screen panel.
  int col = (int)(mUI->pen_x + 0.5f);
  int row = (int)(mUI->pen_y + 0.5f);
  int w   = (int)ui_avail_width(mUI);
  if (w < 8) return;

  mPathFieldCol = col; mPathFieldRow = row; mPathFieldW = w;
  if (const char* text = ui_textarea_str_absolute_pos(mUI, mPathTA, mEngine->GetCodeFilePath(), "\r\n", col, row, w))
    mEngine->SetCodeFilePath(text);
}

// iPlug2 returns UTF-8, which is what the engine stores.
void SteepSynthControl::PromptForCodeFile()
{
  IGraphics* g = GetUI();
  if (!g) return;

  WDL_String fileName, dir;
  fileName.Set(mPlugin ? mEngine->GetCodeFilePath() : "");
  g->PromptForFile(fileName, dir, EFileAction::Open, "txt",
    [this](const WDL_String& file, const WDL_String&) {
      if (file.GetLength() > 0 && mPlugin)
        mEngine->SetCodeFilePath(file.Get());
    });
}

// Mark the editors unloaded so they refill from the new model this frame instead
// of pushing stale buffers back.
void SteepSynthControl::ReloadAllEditorsFromModel()
{
  if (!mPlugin || !mEngine->LoadCodeFromFile()) return;
  int newN = mEngine->EntryCount();
  if (mSelectedSound >= newN) mSelectedSound = (newN > 0) ? newN - 1 : -1;
  mSoundEd.loadedSound = -1;
  mVisualEd.loadedSound = -1;
}

static const char* kDefaultSoundBody  = kCodeSynthDefaultSoundBody;
static const char* kDefaultVisualBody = kCodeSynthDefaultVisualBody;

// Empty means absent.
std::string SteepSynthControl::EditorModelBody(const CodeEditor& ed) const
{
  if (!mPlugin || mSelectedSound < 0) return std::string();
  return ed.isVisual ? mEngine->Entry(mSelectedSound).visualBody
                     : mEngine->Entry(mSelectedSound).body;
}

CodeSynthEditorScroll* SteepSynthControl::EntryScrollSlot(int idx)
{
  if (idx < 0 || idx >= kMaxCodeSynthScrollEntries) return nullptr;
  CodeSynthUiState& st = Ui();
  if (idx >= st.entryScrollCount) st.entryScrollCount = idx + 1;
  return &st.entryScroll[idx];
}

void SteepSynthControl::StashEditorScroll(const CodeEditor& ed)
{
  if (!ed.ic) return;
  CodeSynthEditorScroll* slot = EntryScrollSlot(ed.loadedSound);
  if (!slot) return;
  (ed.isVisual ? slot->visual : slot->sound) = interactive_coding_get_scroll(ed.ic);
}

// Always writes: set_text keeps the previous entry's offset.
void SteepSynthControl::ApplyEditorScroll(const CodeEditor& ed)
{
  if (!ed.ic) return;
  CodeSynthEditorScroll* slot = EntryScrollSlot(ed.loadedSound);
  if (!slot) return;
  interactive_coding_set_scroll(ed.ic, ed.isVisual ? slot->visual : slot->sound);
}

void SteepSynthControl::ReloadCodeEditor(CodeEditor& ed)
{
  // Stash before set_text replaces the buffer.
  StashEditorScroll(ed);

  if (mSelectedSound >= 0)
  {
    ApplyEditorCompileContext(ed);
    std::string body = EditorModelBody(ed);
    interactive_coding_set_text(ed.ic, body.c_str());
    ed.lastSyncedBody = Trimmed(body);
    if (!ed.isVisual && !(mEngine->Entry(mSelectedSound).kind == kEntryCommon))
      RenderCodeSynthPreview(body, (int)mEngine->Entry(mSelectedSound).type);
  }
  else
  {
    ed.lastSyncedBody.clear();
    if (!ed.isVisual)
      mPreviewValidCount = 0;
  }
  ed.loadedSound = mSelectedSound;

  ApplyEditorScroll(ed);
}

// Bodies get their domain's wrap plus the prelude; a prelude block compiles as
// raw top-level script. Both setters no-op when unchanged.
void SteepSynthControl::ApplyEditorCompileContext(CodeEditor& ed)
{
  if (!ed.ic || !mPlugin || mSelectedSound < 0) return;

  if (CodeSynthEntryIsReserved(mEngine->Entry(mSelectedSound)))
  {
    interactive_coding_set_freeform_wrap(ed.ic, nullptr, nullptr, '{', '}');
    interactive_coding_set_freeform_prelude(ed.ic, nullptr);
    return;
  }

  CodeSynthType type = mEngine->Entry(mSelectedSound).type;
  interactive_coding_set_freeform_wrap(ed.ic, kCodeSynthWrapName,
      CodeSynthWrapParamsForType(type), '{', '}');
  // Visual bodies see the visual globals, sound bodies the sound ones.
  std::string prelude = CodeSynthPreludeFor(mEngine->Entries(), ed.isVisual);
  interactive_coding_set_freeform_prelude(ed.ic, prelude.c_str());
}

// Pull external model changes into the buffer, then push the live text back,
// even while it doesn't compile. No-op unless this editor holds the selection.
void SteepSynthControl::SyncCodeEditor(CodeEditor& ed)
{
  if (!ed.ic || mSelectedSound < 0 || mSelectedSound != ed.loadedSound) return;

  // Never mid-scrub: set_text rebuilds the parse arena under the scrub target.
  std::string modelBody = EditorModelBody(ed);
  if (!interactive_coding_is_scrubbing(ed.ic) && Trimmed(modelBody) != ed.lastSyncedBody)
  {
    interactive_coding_set_text(ed.ic, modelBody.c_str());
    ed.lastSyncedBody = Trimmed(modelBody);
  }

  // get_text() is scrub-aware, so this agrees with PushLiveEdit, and it also
  // carries non-compiling edits to the model.
  char* liveText = interactive_coding_get_text(ed.ic);
  if (liveText)
  {
    std::string trimmed = Trimmed(liveText);
    if (trimmed != ed.lastSyncedBody)
    {
      ed.lastSyncedBody = trimmed;
      mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { (ed.isVisual ? e.visualBody : e.body) = liveText; });
    }
    mSys.free(liveText);
  }
}

// By extension only; a file that isn't a WAV fails later at load.
static bool IsWavPath(const char* str)
{
  if (!str) return false;
  size_t n = strlen(str);
  if (n < 4) return false;
  const char* ext = str + n - 4;
  return (ext[0] == '.') &&
         (ext[1] == 'w' || ext[1] == 'W') &&
         (ext[2] == 'a' || ext[2] == 'A') &&
         (ext[3] == 'v' || ext[3] == 'V');
}

// Positionless: a WAV gets a new sound, an image becomes spans code. Both are
// staged for the next frame.
void SteepSynthControl::OnDrop(const char* str)
{
  if (!str || !*str) return;

  if (IsWavPath(str)) { StageWavDrop(str, /*ontoEditor=*/false); return; }

  P2SResult spans;
  if (!p2s_from_file(str, &spans)) return; // not an image we can read -- ignore

  const char* name = str + strlen(str);
  while (name > str && name[-1] != '/' && name[-1] != '\\') name--;

  char* code = p2s_emit_snippet(&spans, name);
  if (code)
  {
    mPendingDropCode = code;
    p2s_free_text(code);
  }
  p2s_free(&spans);
}

// This control covers the whole window, so every drop lands here. A drop on the
// path field sets the link whatever the file type.
void SteepSynthControl::OnDropAt(const char* str, float x, float y)
{
  if (!str || !*str) return;

  if (mPathFieldW > 0 && mCellW > 0.f && mCellH > 0.f)
  {
    int col = (int)(x / (float)mCellW);
    int row = (int)(y / (float)mCellH);
    if (row == mPathFieldRow && col >= mPathFieldCol && col < mPathFieldCol + mPathFieldW)
    {
      mEngine->SetCodeFilePath(str);
      return;
    }
  }

  // On the sound editor: rebind the edited sound. Elsewhere: a new sound.
  if (IsWavPath(str))
  {
    bool onEditor = false;
    if (mSoundEdW > 0 && mCellW > 0.f && mCellH > 0.f)
    {
      int col = (int)(x / (float)mCellW);
      int row = (int)(y / (float)mCellH);
      onEditor = col >= mSoundEdCol && col < mSoundEdCol + mSoundEdW &&
                 row >= mSoundEdRow && row < mSoundEdRow + mSoundEdH;
    }
    StageWavDrop(str, onEditor);
    return;
  }

  OnDrop(str);
}

void SteepSynthControl::StageWavDrop(const char* path, bool ontoEditor)
{
  mPendingDropWav = path ? path : "";
  mPendingDropWavOnEditor = ontoEditor;
}

// Relative to the code file's directory when under it, else absolute.
// Separators normalised to '/'.
std::string SteepSynthControl::WavDirectivePath(const char* absPath) const
{
  std::string wav = absPath ? absPath : "";
  for (char& c : wav) if (c == '\\') c = '/';
  if (wav.empty() || !mPlugin) return wav;

  std::string dir = mEngine->GetCodeFilePath();
  for (char& c : dir) if (c == '\\') c = '/';
  size_t sep = dir.find_last_of('/');
  if (sep == std::string::npos) return wav; // no linked file -- absolute it is
  dir.erase(sep + 1);

  if (wav.size() <= dir.size()) return wav;
  for (size_t i = 0; i < dir.size(); i++)
  {
    char a = wav[i], b = dir[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return wav;
  }
  return wav.substr(dir.size());
}

// Runs after both editors pushed this frame's text, so no sync overwrites it.
void SteepSynthControl::ApplyPendingWavDrop()
{
  if (mPendingDropWav.empty()) return;
  std::string wav;
  wav.swap(mPendingDropWav);
  const bool ontoEditor = mPendingDropWavOnEditor;
  if (!mPlugin) return;

  std::string rel = WavDirectivePath(wav.c_str());
  if (rel.empty()) return;

  // Prelude blocks have no voice to rebind.
  const bool canReplace = ontoEditor && mSelectedSound >= 0 &&
                          !CodeSynthEntryIsReserved(mEngine->Entry(mSelectedSound)) &&
                          !mEngine->Entry(mSelectedSound).body.empty();
  if (canReplace)
  {
    std::string body = mEngine->Entry(mSelectedSound).body;
    CodeSynthSetWavPath(body, rel);
    // The editor picks it up via SyncCodeEditor, which keeps undo history.
    mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { e.body = body; });
    return;
  }

  int added = mEngine->AddCodeSynthEntry(/*withSound=*/true, /*withVisual=*/false);
  if (added < 0) return;

  std::string stem = rel;
  size_t sep = stem.find_last_of('/');
  if (sep != std::string::npos) stem.erase(0, sep + 1);
  size_t dot = stem.find_last_of('.');
  if (dot != std::string::npos && dot > 0) stem.erase(dot);
  if (!stem.empty())
  {
    // Names identify sounds, so they must be unique.
    std::string name = stem;
    for (int n = 2; n < 1000; n++)
    {
      bool taken = false;
      for (int i = 0, cnt = mEngine->EntryCount(); i < cnt && !taken; i++)
        if (i != added && name == mEngine->Entry(i).name) taken = true;
      if (!taken) break;
      name = stem + "_" + std::to_string(n);
    }
    mEngine->SetEntryName(added, name.c_str());
  }

  std::string body = kCodeSynthDefaultWavBody;
  CodeSynthSetWavPath(body, rel);
  mEngine->EditEntry(added, [&](CodeSynthEntry& e) { e.body = body; });
  mSelectedSound = added;
}

// Pasted above the caret, into the focused editor, else the visual one; only an
// editor holding the selected entry.
void SteepSynthControl::ApplyPendingDrop()
{
  if (mPendingDropCode.empty()) return;
  std::string code;
  code.swap(mPendingDropCode);
  if (mSelectedSound < 0) return;

  CodeEditor* ed = nullptr;
  const bool visualLoaded = (mVisualEd.loadedSound == mSelectedSound);
  const bool soundLoaded  = (mSoundEd.loadedSound == mSelectedSound);
  if (visualLoaded && interactive_coding_has_editor_focus(mVisualEd.ic)) ed = &mVisualEd;
  else if (soundLoaded && interactive_coding_has_editor_focus(mSoundEd.ic)) ed = &mSoundEd;
  else if (visualLoaded) ed = &mVisualEd;
  else if (soundLoaded) ed = &mSoundEd;
  if (!ed || interactive_coding_is_scrubbing(ed->ic)) return;

  char* cur = interactive_coding_get_text(ed->ic);
  std::string text = cur ? cur : "";
  if (cur) mSys.free(cur);

  int row = 0, col = 0;
  interactive_coding_get_cursor(ed->ic, &row, &col);
  if (row < 0) row = 0;

  size_t pos = 0;
  for (int i = 0; i < row; i++)
  {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) { pos = text.size(); break; }
    pos = nl + 1;
  }
  if (pos == text.size() && !text.empty() && text[text.size() - 1] != '\n')
    text += '\n';

  text.insert(pos, code);
  interactive_coding_set_text(ed->ic, text.c_str());

  int lines = 0;
  for (size_t i = 0; i < code.size(); i++) if (code[i] == '\n') lines++;
  interactive_coding_set_cursor(ed->ic, row + lines, 0);
}

// Which entries the recorded sequence triggers, mirroring the parser's map: an
// unclaimed note plays the nearest claimed note on its channel, ties to the lower.
static void BuildSequenceUsage(SteepSynthEngine* plugin, std::vector<char>& used)
{
  int n = plugin ? plugin->EntryCount() : 0;
  used.assign((size_t)n, 0);
  if (n <= 0 || !plugin->HasRecordedMatrix()) return;

  std::vector<short> map((size_t)16 * 128, (short)-1);
  for (int i = 0; i < n; i++)
  {
    if (CodeSynthEntryIsReserved(plugin->Entry(i))) continue;
    int ch = plugin->Entry(i).channel - 1; // headers are 1-based
    int note = plugin->Entry(i).note;
    if (ch < 0 || ch > 15 || note < 0 || note > 127) continue;
    map[(size_t)ch * 128 + note] = (short)i;
  }

  int rows = plugin->MatrixRowCount();
  for (int r = 0; r < rows; r++)
  {
    int ch = plugin->MatrixRowChannel(r);
    int note = plugin->MatrixRowNote(r);
    if (ch < 0 || ch > 15 || note < 0 || note > 127) continue;
    const short* chanMap = &map[(size_t)ch * 128];
    int hit = chanMap[note];
    if (hit < 0)
    {
      int best = -1;
      for (int m = 0; m < 128; m++)
        if (chanMap[m] >= 0 && (best < 0 || abs(m - note) < abs(best - note))) best = m;
      if (best < 0) continue;
      hit = chanMap[best];
    }
    if (hit >= 0 && hit < n) used[(size_t)hit] = 1;
  }
}

void SteepSynthControl::DrawCodeSynthEditor()
{
  if (!mPlugin || !mSoundEd.ic || !mVisualEd.ic) return;

  RestoreUiState();

  int n = mEngine->EntryCount();
  if (mSelectedSound < 0 || mSelectedSound >= n)
    mSelectedSound = (n > 0) ? 0 : -1;

  // Sync both editors before the list can change the selection. Compile context
  // first: a common-block edit must reach the other editors' parse.
  ApplyEditorCompileContext(mVisualEd);
  ApplyEditorCompileContext(mSoundEd);
  SyncCodeEditor(mVisualEd);
  SyncCodeEditor(mSoundEd);

  // After the syncs, which would push the pre-drop buffer back, and before the
  // list, so a new sound shows this frame.
  ApplyPendingWavDrop();
  n = mEngine->EntryCount(); // a drop can have just added one

  // Show the edited entry on the screen while nothing plays.
  mEngine->SetVisualPreviewEntry(mSelectedSound);

  const int leftW = kLeftColumnWidth;
  const int listTop = kContentRow;
  const int addRows = ((CodeSynthCommonIndex(mEngine->Entries()) >= 0) &&
                       (CodeSynthGlobalsIndex(mEngine->Entries()) >= 0)) ? 2 : 3;
  const int maxListRows = (mRows - listTop > addRows) ? (mRows - listTop - addRows) : 1;

  // Fixed-width name field so the type dropdown never shifts: '[' + 9 chars + ']'.
  const int kNameCells = 11;

  // Pinned to 17 cells, one short of the header row. The header abbreviates; the
  // popup spells it out.
  static const char* kEventFilterShort[kEventFilterCount] =
    { "all", "sounds", "visuals", "both", "in seq", "not seq" };
  static const char* kEventFilterLong[kEventFilterCount] =
    { "all", "sounds", "visuals", "sound & visual", "in sequence", "not in sequence" };
  if (mEventFilter < 0 || mEventFilter >= kEventFilterCount) mEventFilter = kEventFilterAll;

  // Needs a matrix scan, so only when a sequence mode asks.
  const bool filterBySeq = (mEventFilter == kEventFilterInSeq ||
                            mEventFilter == kEventFilterNotInSeq);
  std::vector<char> inSeq;
  if (filterBySeq) BuildSequenceUsage(mEngine, inSeq);

  ui_set_cursor(mUI, 0, (float)listTop);
  ui_push_item_width(mUI, 17);
  if (ui_dropdown_begin(mUI, "Events", kEventFilterShort[mEventFilter], 0))
  {
    for (int k = 0; k < kEventFilterCount; k++)
      if (ui_selectable(mUI, kEventFilterLong[k], mEventFilter == k))
        mEventFilter = k;
    ui_dropdown_end(mUI);
  }
  ui_pop_item_width(mUI);

  int shown = 0;
  static const char* kCodeSynthTypeItems[3] = {"f64", "f32", "fix"};
  for (int i = 0; i < n && shown < maxListRows; i++)
  {
    // Prelude blocks are always shown, first.
    const bool isPrelude = CodeSynthEntryIsReserved(mEngine->Entry(i));
    if (!isPrelude)
    {
      bool keep = true;
      switch (mEventFilter)
      {
        case kEventFilterSounds:   keep = !mEngine->Entry(i).body.empty(); break;
        case kEventFilterVisuals:  keep = !mEngine->Entry(i).visualBody.empty(); break;
        case kEventFilterBoth:     keep = !mEngine->Entry(i).body.empty() &&
                                          !mEngine->Entry(i).visualBody.empty(); break;
        case kEventFilterInSeq:    keep = (i < (int)inSeq.size()) && inSeq[(size_t)i] != 0; break;
        case kEventFilterNotInSeq: keep = (i >= (int)inSeq.size()) || inSeq[(size_t)i] == 0; break;
        default: break; // kEventFilterAll
      }
      if (!keep) continue;
    }

    // "##e<idx>": names can repeat, and equal ids would select together.
    char label[64];
    s_snprintf(label, sizeof(label), "%s##e%d", mEngine->Entry(i).name.c_str(), i);
    ui_push_item_width(mUI, (float)kNameCells);
    ui_option_button_flags(mUI, label, &mSelectedSound, i, UI_OPTION_ALIGN_LEFT);
    ui_pop_item_width(mUI);

    // A prelude block compiles under each body's type, not its own.
    if (isPrelude) { shown++; continue; }

    ui_same_line_col(mUI, kNameCells + 1);
    ui_push_item_width(mUI, 5);
    int typeIdx = (int)mEngine->Entry(i).type;
    char dropdownId[16];
    s_snprintf(dropdownId, sizeof(dropdownId), "##type%d", i);
    if (ui_dropdown(mUI, dropdownId, &typeIdx, 3, kCodeSynthTypeItems, UI_DROPDOWN_HIDE_NAME))
    {
      mEngine->EditEntry(i, [&](CodeSynthEntry& e) { e.type = (CodeSynthType)typeIdx; });
      if (i == mSelectedSound)
      {
        const char* params = CodeSynthWrapParamsForType((CodeSynthType)typeIdx);
        interactive_coding_set_freeform_wrap(mSoundEd.ic, kCodeSynthWrapName, params, '{', '}');
        interactive_coding_set_freeform_wrap(mVisualEd.ic, kCodeSynthWrapName, params, '{', '}');
      }
    }
    ui_pop_item_width(mUI);
    shown++;
  }
  if (shown == 0)
  {
    ui_label(mUI, n == 0 ? "(none)" : "(filtered)");
  }

  // +snd/+vis add a single-half entry; the other half is added over the editor.
  {
    if (ui_button(mUI, "+snd"))
    {
      int added = mEngine->AddCodeSynthEntry(/*withSound=*/true, /*withVisual=*/false);
      if (added >= 0) mSelectedSound = added;
    }
    ui_same_line(mUI);
    if (ui_button(mUI, "+vis"))
    {
      int added = mEngine->AddCodeSynthEntry(/*withSound=*/false, /*withVisual=*/true);
      if (added >= 0) mSelectedSound = added;
    }

    // Both blocks land at the front of the list, so the selection follows.
    const bool offerCommon  = CodeSynthCommonIndex(mEngine->Entries()) < 0;
    const bool offerGlobals = CodeSynthGlobalsIndex(mEngine->Entries()) < 0;
    if (offerCommon)
    {
      if (ui_button(mUI, "+cmn"))
      {
        int added = mEngine->AddCodeSynthCommonEntry();
        if (added >= 0) mSelectedSound = added;
      }
    }
    if (offerGlobals)
    {
      // Column 7 either way, so +glb doesn't move.
      if (offerCommon) ui_same_line(mUI);
      else             ui_indent(mUI, 7);
      if (ui_button(mUI, "+glb"))
      {
        int added = mEngine->AddCodeSynthGlobalsEntry();
        if (added >= 0) mSelectedSound = added;
      }
    }
  }

  // Prelude blocks always show their editors, even emptied. common has one body;
  // globals is split per domain, so it shows both.
  const bool selIsCommon  = mSelectedSound >= 0 && (mEngine->Entry(mSelectedSound).kind == kEntryCommon);
  const bool selIsGlobals = mSelectedSound >= 0 && (mEngine->Entry(mSelectedSound).kind == kEntryGlobals);
  bool hasSound  = selIsCommon || selIsGlobals
                || (mSelectedSound >= 0 && !mEngine->Entry(mSelectedSound).body.empty());
  bool hasVisual = selIsGlobals
                || (!selIsCommon
                    && mSelectedSound >= 0 && !mEngine->Entry(mSelectedSound).visualBody.empty());

  // An absent body's editor is marked unloaded so a stale buffer can't resurrect it.
  if (hasSound) { if (mSoundEd.loadedSound != mSelectedSound) ReloadCodeEditor(mSoundEd); }
  else          { mSoundEd.loadedSound = -1; mSoundEd.lastSyncedBody.clear(); mPreviewValidCount = 0; }
  if (hasVisual){ if (mVisualEd.loadedSound != mSelectedSound) ReloadCodeEditor(mVisualEd); }
  else          { mVisualEd.loadedSound = -1; mVisualEd.lastSyncedBody.clear(); }

  // After the reload, so the paste lands in an editor holding the current entry.
  ApplyPendingDrop();

  // ui_button moves the pen to the next row; later widgets on a row need ui_same_line().
  ui_set_cursor(mUI, 0, (float)kActionRow);
  DrawCodeFileRow();

  ui_set_cursor(mUI, 0, (float)kSeqRow);
  DrawTransportButtons();

  int editorX = leftW;
  int editorY = listTop;
  int editorW = mCols - leftW;
  int editorH = mRows - listTop;
  if (editorH < 1) editorH = 1;

  // Header row, add/delete row, editors, slider row.
  const int headerRow = editorY;
  const int buttonRow = editorY + 1;
  const int edTop     = editorY + 2;
  const int sliderRow = editorY + editorH - 1;
  const int edH       = sliderRow - edTop; // rows available to the editor frame(s)

  if (mSelectedSound >= 0 && editorW > 4 && edH > 1)
  {
    DrawCodeSynthHeaderRow(editorX, headerRow, editorW);

    const int gap = 1; // 1-cell column between the editors -- the divider handle
    int soundX  = editorX, soundW  = 0;
    int visualX = editorX, visualW = 0;
    if (hasSound && hasVisual)
    {
      soundW  = ui_splitter(mUI, &mEditorSplit, editorX, edTop, editorW, edH, 6);
      visualW = editorW - gap - soundW;
      visualX = editorX + soundW + gap;
    }
    else if (hasSound)  { soundX  = editorX; soundW  = editorW; }
    else if (hasVisual) { visualX = editorX; visualW = editorW; }

    // Selected entry removed: drop both editors' loaded state so no stale buffer
    // lands on the neighbour that slid into its index.
    auto onEntryRemoved = [&]() {
      mSoundEd.loadedSound  = -1; mSoundEd.lastSyncedBody.clear(); mPreviewValidCount = 0;
      mVisualEd.loadedSound = -1; mVisualEd.lastSyncedBody.clear();
      int cnt = mEngine->EntryCount();
      if (mSelectedSound >= cnt) mSelectedSound = cnt - 1;
      hasSound = hasVisual = false;
    };

    // The side buttons add/remove a body; the whole entry goes only through the
    // middle button.
    bool showSndBtn = true;
    bool showVisBtn = true;
    const char* sndLabel = hasSound  ? "delete sound"  : "add sound";
    const char* visLabel = hasVisual ? "delete visual" : "add visual";

    // A prelude block only offers deleting itself.
    if (selIsCommon || selIsGlobals)
    {
      showSndBtn = showVisBtn = false;
      ui_set_cursor(mUI, (float)editorX, (float)buttonRow);
      const bool deleted = selIsCommon
          ? (ui_button(mUI, "delete common")  && mEngine->DeleteCodeSynthCommonEntry())
          : (ui_button(mUI, "delete globals") && mEngine->DeleteCodeSynthGlobalsEntry());
      if (deleted)
        onEntryRemoved();
    }

    if (showSndBtn)
    {
      ui_set_cursor(mUI, (float)editorX, (float)buttonRow);
      if (ui_button(mUI, sndLabel))
      {
        if (hasSound)
        {
          mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { e.body = ""; });
          mSoundEd.loadedSound = -1; mSoundEd.lastSyncedBody.clear(); mPreviewValidCount = 0;
        }
        else
          mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { e.body = kDefaultSoundBody; });
      }
    }
    if (showVisBtn)
    {
      int visBtnW = ui_display_len(visLabel) + 2; // ui_button auto-width (see ui_button)
      ui_set_cursor(mUI, (float)(editorX + editorW - visBtnW), (float)buttonRow);
      if (ui_button(mUI, visLabel))
      {
        if (hasVisual)
        {
          mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { e.visualBody = ""; });
          mVisualEd.loadedSound = -1; mVisualEd.lastSyncedBody.clear();
        }
        else
          mEngine->EditEntry(mSelectedSound, [&](CodeSynthEntry& e) { e.visualBody = kDefaultVisualBody; });
      }
    }

    if (showSndBtn && showVisBtn)
    {
      const char* delLabel = "delete event";
      int delBtnW = ui_display_len(delLabel) + 2;
      ui_set_cursor(mUI, (float)(editorX + (editorW - delBtnW) / 2), (float)buttonRow);
      if (ui_button(mUI, delLabel) && mEngine->DeleteCodeSynthEntry(mSelectedSound))
        onEntryRemoved();
    }

    if (hasSound && soundW > 4)
    {
      interactive_coding_frame_absolute_pos(mSoundEd.ic, mUI, soundX, edTop, soundW, edH);
      // Unconditional: a WAV dropped on a prelude block's editor still counts as on-editor.
      mSoundEdCol = soundX; mSoundEdRow = edTop;
      mSoundEdW   = soundW; mSoundEdH   = edH;

      if (!selIsCommon && !selIsGlobals)
      {
        // The slider is disabled, not hidden, while the preview is off.
        const char* kPreviewLabel = "Sample Preview";
        const int previewW = ui_display_len(kPreviewLabel) + 4; // see ui_check_box
        // The checkbox is label-wide and would spill into the visual column.
        ui_push_clip_rect(mUI, (float)soundX, (float)sliderRow, (float)soundW, 1.f);
        ui_set_cursor(mUI, (float)soundX, (float)sliderRow);
        if (ui_check_box(mUI, kPreviewLabel, &Ui().previewEnabled))
        {
          // Re-sample: rendering was skipped while off.
          if (Ui().previewEnabled)
            RenderCodeSynthPreview(mSoundEd.lastSyncedBody, (int)mEngine->Entry(mSelectedSound).type);
          else
            mPreviewValidCount = 0;
        }

        int zoomX = soundX + previewW + 1;
        int zoomW = soundW - (previewW + 1);
        if (zoomW > 4)
        {
          ui_set_cursor(mUI, (float)zoomX, (float)sliderRow);
          ui_push_item_width(mUI, (float)zoomW);
          if (!Ui().previewEnabled) ui_begin_disabled(mUI);
          if (ui_slider(mUI, "Zoom", &Ui().previewWindow16ths, kPreviewWindowMin16ths,
                        kPreviewWindowMid16ths, kPreviewWindowMax16ths))
            RenderCodeSynthPreview(mSoundEd.lastSyncedBody, (int)mEngine->Entry(mSelectedSound).type);
          if (!Ui().previewEnabled) ui_end_disabled(mUI);
          ui_pop_item_width(mUI);
        }
        ui_pop_clip_rect(mUI);

        mEditorCol = soundX;
        mEditorRow = edTop;
        mEditorW   = soundW;
        mEditorH   = edH;

        // Over the Results panel, whose split is draggable.
        if (!interactive_coding_get_results_rect(mSoundEd.ic, &mPreviewCol, &mPreviewRow,
                                                 &mPreviewW, &mPreviewH))
        {
          mPreviewCol = mEditorCol; mPreviewRow = mEditorRow;
          mPreviewW   = mEditorW;   mPreviewH   = mEditorH;
        }

        mPreviewCodeOk = interactive_coding_last_run_ok(mSoundEd.ic);
        mPreviewVisible = true;
      }
    }

    if (hasVisual && visualW > 4)
    {
      interactive_coding_frame_absolute_pos(mVisualEd.ic, mUI, visualX, edTop, visualW, edH);
      ui_set_cursor(mUI, (float)visualX, (float)sliderRow);
      ui_push_item_width(mUI, (float)visualW);
      float delayNorm = Ui().visualDelayMs / 300.f;
      if (ui_slider(mUI, "VisDelay", &delayNorm, 0.f, 0.f, 1.f))
      {
        Ui().visualDelayMs = delayNorm * 300.f;
        mEngine->SetVisualDelayMs(Ui().visualDelayMs);
      }
      ui_pop_item_width(mUI);
    }
  }

  RecordUiState();

  // Staged only: SetFocus/CreateWindowEx dispatch messages that can re-enter
  // Draw() mid-frame. Applied before the next ui_begin.
  mPendingOsFocusProxy = mUI->window_has_focus
                      && (interactive_coding_has_editor_focus(mSoundEd.ic)
                       || interactive_coding_has_editor_focus(mVisualEd.ic)
                       || (mNameTA && mUI->focus_id == ui_id_from_ptr(mNameTA)));
}

void SteepSynthControl::Draw(IGraphics& g)
{
  // Win32 calls that pump messages let the host repaint us mid-frame; a nested
  // frame would corrupt the outer one.
  if (mInDraw) return;
  mInDraw = true;
  struct DrawGuard { bool* flag; ~DrawGuard() { *flag = false; } } drawGuard{ &mInDraw };

  PlatformClipboard_SetGraphics(&g);
  PlatformKeyFocus_SetGraphics(&g);

  if (mPendingOsFocusProxy != mEditorHadOsFocusProxy)
  {
    mEditorHadOsFocusProxy = mPendingOsFocusProxy;
    PlatformKeyFocus_SetEditorFocused(mPendingOsFocusProxy ? 1 : 0);
  }

  int w = (int)mRECT.W();
  int h = (int)mRECT.H();
  if (w < 8) w = 8;
  if (h < 8) h = 8;

  // Before the regrid check, so a restored size applies this frame.
  bool screenLayoutChanged = SyncScreenResolutionAndDock();

  float newScale = g.GetScreenScale() * g.GetDrawScale();
  bool uiScaleChanged = (Ui().uiScale != mUiScale) || (Ui().lineHeight != mLineHeight);
  if (w != mBackW || h != mBackH || newScale != mBackScale || uiScaleChanged ||
      screenLayoutChanged)
  {
    mBackW = w; mBackH = h; mBackScale = newScale;
    mUiScale = Ui().uiScale;
    mLineHeight = Ui().lineHeight;
    ReallocBackbuffer();
    RebuildUI(PLUG_FPS);
  }

  auto now = std::chrono::steady_clock::now();
  static auto start = now;
  float timeInSeconds = std::chrono::duration<float>(now - start).count();

  // We have the keyboard only while the host window has OS focus and the pointer
  // is over us.
  mUI->window_has_focus = (PlatformKeyFocus_WindowHasOsFocus() && mMouseInside) ? 1 : 0;

  ui_begin(mUI, timeInSeconds);

#if STEEPSYNTH_ENABLE_INDEXYNT
  struct SliderDef { const char* name; int paramIdx; };
  static const SliderDef sliders[] = {
    { "Sound",      kSound },
    { "Envelope",   kEnvelope },
    { "Cutoff",     kCutoff },
    { "Resonance",  kResonance },
    { "Movement",   kMovement },
    { "Unison",     kUnison },
    { "Portamento", kPortamento },
    { "Deform",     kDeform },
    { "Expression", kExpression },
  };
  const int numSliders = sizeof(sliders) / sizeof(sliders[0]);
#endif

  ui_set_cursor(mUI, 0, 0);
  ui_label(mUI, "=== MAD TEA SYNTH ===");

  {
    ui_same_line_pad(mUI, 2);
    // Display scale, not mBackScale, which includes the host's zoom.
    ui_glue_draw_zoom_auto(mUI, &Ui().uiScale, g.GetScreenScale(), &Ui().uiScaleAuto);

    ui_same_line_pad(mUI, 2);
    ui_glue_draw_grade_lineh(mUI, &Ui().lineHeight,
                              kUiLineHeightMin, kUiLineHeightDefault, kUiLineHeightMax);        

    ui_same_line_pad(mUI, 2);
    ui_glue_draw_theme(mUI);
  }

  bool isCodeSynth = mPlugin && mEngine->GetBackend() == SynthBackend::CodeSynth;
#if STEEPSYNTH_ENABLE_INDEXYNT
  // Drawn only when Indexynt is built.
  {
    static const char* const kBackendItems[2] = { "Indexynt", "CodeSynth" };
    int backendIdx = isCodeSynth ? 1 : 0;
    ui_same_line_pad(mUI, 2);
    ui_label(mUI, "Backend:");
    ui_same_line(mUI);
    if (ui_option_bar(mUI, "##backend", &backendIdx, 2, kBackendItems, 0) && mPlugin)
    {
      mEngine->SetBackend(backendIdx ? SynthBackend::CodeSynth : SynthBackend::Indexynt);
    }
  }
#endif

  {
    ui_same_line_pad(mUI, 2);
    if (ui_toggle_button(mUI, "Settings & Export", mShowSettings))
      mShowSettings = !mShowSettings;
  }

  // Cleared here: the settings page draws instead of DrawCodeSynthEditor.
  mPreviewVisible = false;
  mSoundEdW = 0;           // "not drawn" until proven otherwise -- see OnDropAt

  if (mShowSettings)
    DrawSettingsPage();
  else if (isCodeSynth)
    DrawCodeSynthEditor();
#if STEEPSYNTH_ENABLE_INDEXYNT
  else
  {
    ui_set_cursor(mUI, 0, (float)kActionRow);
    DrawTransportButtons();

    const int slidersTop = kActionRow + 2;
    ui_blank_row(mUI);
    for (int i = 0; i < numSliders; i++)
    {
      float val = ReadParam(mPlugin, sliders[i].paramIdx);
      if (ui_slider(mUI, sliders[i].name, &val, 0.f, 0.5f, 1.f))
      {
        WriteParam(mPlugin, &g, sliders[i].paramIdx, val);
      }
      ui_blank_row(mUI);
    }

    // `row` only bounds-checks against mRows.
    const int row = slidersTop + numSliders * 2;
    if (row + 1 < mRows)
    {
      char buf[256];
      int pos = 0;
      for (int i = 0; i < numSliders && pos < (int)sizeof(buf) - 20; i++)
      {
        float v = ReadParam(mPlugin, sliders[i].paramIdx);
        pos += s_snprintf(buf + pos, sizeof(buf) - pos, "%s:%.2f  ",
                         sliders[i].name, (double)v);
      }
      ui_label(mUI, buf);
    }
    if (row + 2 < mRows && mPlugin)
    {
      const char* msg = mEngine->GetCodeSynthDebugMessage();
      int line = 0;
      while (msg && msg[0] && row + 2 + line < mRows)
      {
        const char* nl = strchr(msg, '\n');
        char lineBuf[256];
        size_t len = nl ? (size_t)(nl - msg) : strlen(msg);
        if (len > sizeof(lineBuf) - 1) len = sizeof(lineBuf) - 1;
        memcpy(lineBuf, msg, len);
        lineBuf[len] = '\0';
        if (lineBuf[0])
        {
          ui_label(mUI, lineBuf);
          line++;
        }
        msg = nl ? nl + 1 : nullptr;
      }
    }
  }
#endif // STEEPSYNTH_ENABLE_INDEXYNT

  ui_end(mUI);

  int need_render = ui_blit_to_textmode(mUI, mTM);
  if (need_render || !mImageValid)
  {
    // Right before tm_render, which clears the path list, and under the same condition.
    if (isCodeSynth && mPreviewVisible)
    {
      bool showMatrix = mPlugin && mEngine->IsPlaying() && mEngine->HasRecordedMatrix();
      if (showMatrix)
        PushMatrixPath(mEditorCol, mEditorRow, mEditorW, mEditorH, 0.3f);
      else if (mPreviewValidCount > 1 && mPreviewCodeOk)
        PushPreviewWaveformPath(mPreviewCol, mPreviewRow, mPreviewW, mPreviewH, 0.3f);
    }

    tm_render(mTM, mBackPhysW, mBackPhysH, mBackPhysW,
              mBackbuffer, kBaseFontSize * mUiScale,
              mUI->global_line_height,
              ui_theme_color(mUI, UI_COL_DEFAULT), TM_FLAGS_SWAP_RED_BLUE_CHANNELS);
  }

  NVGcontext* vg = (NVGcontext*)g.GetDrawContext();
  if (!vg) return;

  if (mImageValid && mNVGImage)
  {
    if (need_render)
      nvgUpdateImage(vg, mNVGImage, (const unsigned char*)mBackbuffer);
  }
  else
  {
    mNVGImage = nvgCreateImageRGBA(vg, mBackPhysW, mBackPhysH, 0,
                                   (const unsigned char*)mBackbuffer);
    mImageValid = (mNVGImage != 0);
  }

  if (mNVGImage)
  {
    NVGpaint imgPaint = nvgImagePattern(vg,
      0.f, 0.f,
      (float)mBackW, (float)mBackH, 0.f, mNVGImage, 1.f);

    nvgBeginPath(vg);
    nvgRect(vg, mRECT.L, mRECT.T, mRECT.W(), mRECT.H());
    nvgFillPaint(vg, imgPaint);
    nvgFill(vg);
  }

  // At frame rate: UiIdle's 20Hz visibly judders animation.
  if (mPlugin) mEngine->TickVisualPipeline();

  DrawVirtualScreen(g);

  SetDirty(false);
}

void SteepSynthControl::ReleaseScreenImage()
{
  if (mScreenImageValid && mScreenNVGImage)
  {
    IGraphics* gfx = GetUI();
    NVGcontext* vg = gfx ? (NVGcontext*)gfx->GetDrawContext() : nullptr;
    if (vg) nvgDeleteImage(vg, mScreenNVGImage);
  }
  mScreenNVGImage = 0;
  mScreenImageValid = false;
}

// The resolution is the export target's. A change forces a full recompile,
// since `width`/`height` are folded.
bool SteepSynthControl::SyncScreenResolutionAndDock()
{
  bool layoutChanged = (Ui().screenDocked != mScreenDockedLaidOut);
  mScreenDockedLaidOut = Ui().screenDocked;

  if (!mPlugin) return layoutChanged;

  int w = 0, h = 0;
  ExportTargetScreenSize(mPlugin->GetExportSettings(), w, h);
  if (w == mScreenResW && h == mScreenResH) return layoutChanged;

  // Read back: vscreen clamps to VSCREEN_MAX_W/H.
  RenderCtx* screen = mEngine->ScreenCtx();
  vscreen_set_size_ctx(screen, w, h);
  mScreenResW = vscreen_width_ctx(screen);
  mScreenResH = vscreen_height_ctx(screen);

  ReleaseScreenImage();
  mScreenRGBA.clear();
  mScreenGeneration = 0;

  mEngine->PushCodeSynthLiveEdit();
  return true;
}

bool SteepSynthControl::HitScreenFrame(float x, float y) const
{
  if (!mScreenDrawValid) return false;
  const float g = kScreenGrab;
  const bool inOuter = x >= mScreenDrawX - g && x <= mScreenDrawX + mScreenDrawW + g &&
                       y >= mScreenDrawY - g && y <= mScreenDrawY + mScreenDrawH + g;
  if (!inOuter) return false;
  const bool inImage = x >= mScreenDrawX && x < mScreenDrawX + mScreenDrawW &&
                       y >= mScreenDrawY && y < mScreenDrawY + mScreenDrawH;
  return !inImage;
}

void SteepSynthControl::SetScreenPos(float x, float y)
{
  const float w = mRECT.W() > 1.f ? mRECT.W() : 1.f;
  const float h = mRECT.H() > 1.f ? mRECT.H() : 1.f;

  const float keep = kScreenKeepVisible;
  const float minX = -(mScreenDrawW - keep), maxX = w - keep;
  const float minY = -(mScreenDrawH - keep), maxY = h - keep;
  if (x < minX) x = minX;
  if (x > maxX) x = maxX;
  if (y < minY) y = minY;
  if (y > maxY) y = maxY;

  Ui().screenPosX = x / w;
  Ui().screenPosY = y / h;
  Ui().screenDocked = false;
}

void SteepSynthControl::DockScreen()
{
  Ui().screenDocked = true;
  mScreenDragging = false;
}

void SteepSynthControl::DrawVirtualScreen(IGraphics& g)
{
  NVGcontext* vg = (NVGcontext*)g.GetDrawContext();
  if (!vg) return;

  RenderCtx* screen = mPlugin ? mEngine->ScreenCtx() : nullptr;
  const int srcW = vscreen_width_ctx(screen);
  const int srcH = vscreen_height_ctx(screen);
  if (srcW <= 0 || srcH <= 0) return;

  // Integer zoom, with the origin snapped to the device-pixel grid.
  const float scale = mBackScale > 0.f ? mBackScale : 1.f;
  const float sw = (float)(srcW * mScreenZoomDev) / scale;
  const float sh = (float)(srcH * mScreenZoomDev) / scale;

  float px, py;
  if (Ui().screenDocked)
  {
    px = mRECT.R - mScreenPanelW + kScreenMargin;
    py = mRECT.T + kScreenMargin;
  }
  else
  {
    px = mRECT.L + Ui().screenPosX * mRECT.W();
    py = mRECT.T + Ui().screenPosY * mRECT.H();
  }
  const float sx = std::floor(px * scale + 0.5f) / scale;
  const float sy = std::floor(py * scale + 0.5f) / scale;

  mScreenDrawX = sx; mScreenDrawY = sy;
  mScreenDrawW = sw; mScreenDrawH = sh;
  mScreenDrawValid = true;

  unsigned int gen = vscreen_generation_ctx(screen);
  const bool pebblePal = mPlugin->GetExportSettings().target == kExportTargetPebble;
  if (!mScreenImageValid || gen != mScreenGeneration || pebblePal != mScreenPebblePal)
  {
    if (mScreenRGBA.size() != (size_t)srcW * (size_t)srcH)
      mScreenRGBA.resize((size_t)srcW * (size_t)srcH);

    // Pebble has no palette: restore the default cube so `palette` writes don't show.
    if (pebblePal)
    {
      int* pal = vscreen_palette_ctx(screen);
      const int palLen = vscreen_palette_len_ctx(screen);
      if (pal)
        for (int i = 0; i < palLen; i++) pal[i] = (int)VSCREEN_PALETTE_DEFAULT(i);
    }
    // Already RGBA order, no channel swap.
    vscreen_expand_rgba_ctx(screen, mScreenRGBA.data(), srcW);
    mScreenGeneration = gen;
    mScreenPebblePal = pebblePal;

    if (mScreenImageValid && mScreenNVGImage)
    {
      nvgUpdateImage(vg, mScreenNVGImage, (const unsigned char*)mScreenRGBA.data());
    }
    else
    {
      mScreenNVGImage = nvgCreateImageRGBA(vg, srcW, srcH, NVG_IMAGE_NEAREST,
                                           (const unsigned char*)mScreenRGBA.data());
      mScreenImageValid = (mScreenNVGImage != 0);
    }
  }

  if (!mScreenImageValid) return;

  const float b = kScreenBezel;
  if (!Ui().screenDocked)
  {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, sx - b, sy - b, sw + b * 2, sh + b * 2, 6.f);
    nvgFillColor(vg, nvgRGBA(24, 24, 24, 235));
    nvgFill(vg);
  }

  nvgBeginPath(vg);
  nvgRoundedRect(vg, sx - b, sy - b, sw + b * 2, sh + b * 2, 6.f);
  nvgStrokeColor(vg, mScreenDragging ? nvgRGBA(170, 170, 170, 255)
                                     : nvgRGBA(90, 90, 90, 255));
  nvgStrokeWidth(vg, 2.f);
  nvgStroke(vg);

  NVGpaint paint = nvgImagePattern(vg, sx, sy, sw, sh, 0.f, mScreenNVGImage, 1.f);
  nvgBeginPath(vg);
  nvgRect(vg, sx, sy, sw, sh);
  nvgFillPaint(vg, paint);
  nvgFill(vg);
}

static int ModFlags(const IMouseMod& mod)
{
  int f = 0;
  if (mod.C) f |= UI_FLAGS_CTRL;
  if (mod.S) f |= UI_FLAGS_SHIFT;
  if (mod.A) f |= UI_FLAGS_ALT;
  return f;
}

// The frame is checked before the UI, since a floating preview overlaps the
// editor. Only the ring, never the picture.
void SteepSynthControl::OnMouseDown(float x, float y, const IMouseMod& mod)
{
  mMouseInside = true;
  mMouseButton = mod.L ? 0 : (mod.R ? 2 : 1);

  if (mod.L && HitScreenFrame(x, y))
  {
    mScreenDragging = true;
    mScreenDragOffX = mScreenDrawX - x;
    mScreenDragOffY = mScreenDrawY - y;
    SetDirty(false);
    return;
  }

  ui_os_mouse_event(mUI, x, y, 0.f, UI_MOUSE_TYPE_DOWN, mMouseButton, ModFlags(mod));
  SetDirty(false);
}
// The default forwards to OnMouseDown; the UI wants a DOWN carrying UI_FLAGS_DOUBLE_CLICK.
void SteepSynthControl::OnMouseDblClick(float x, float y, const IMouseMod& mod)
{
  mMouseInside = true;
  mMouseButton = mod.L ? 0 : (mod.R ? 2 : 1);

  if (mod.L && HitScreenFrame(x, y))
  {
    DockScreen();
    SetDirty(false);
    return;
  }

  ui_os_mouse_event(mUI, x, y, 0.f, UI_MOUSE_TYPE_DOWN, mMouseButton,
                    ModFlags(mod) | UI_FLAGS_DOUBLE_CLICK);
  SetDirty(false);
}
void SteepSynthControl::OnMouseUp(float x, float y, const IMouseMod& mod)
{
  mMouseInside = true;

  // Swallowed: the UI never saw the matching DOWN.
  if (mScreenDragging)
  {
    mScreenDragging = false;
    mMouseButton = -1;
    SetDirty(false);
    return;
  }

  ui_os_mouse_event(mUI, x, y, 0.f, UI_MOUSE_TYPE_UP, mMouseButton, ModFlags(mod));
  mMouseButton = -1; SetDirty(false);
}
void SteepSynthControl::OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod)
{
  mMouseInside = true;

  if (mScreenDragging)
  {
    // Absolute rather than accumulated, so the clamp can't make it lag the cursor.
    SetScreenPos(x + mScreenDragOffX, y + mScreenDragOffY);
    SetDirty(false);
    return;
  }

  ui_os_mouse_event(mUI, x, y, 0.f, UI_MOUSE_TYPE_DRAG, mMouseButton, ModFlags(mod));
  SetDirty(false);
}
void SteepSynthControl::OnMouseWheel(float x, float y, const IMouseMod& mod, float d)
{
  mMouseInside = true;
  ui_os_mouse_event(mUI, x, y, d, UI_MOUSE_TYPE_WHEEL, -1, ModFlags(mod));
  SetDirty(false);
}
void SteepSynthControl::OnMouseOver(float x, float y, const IMouseMod& mod)
{
  mMouseInside = true;
  ui_os_mouse_event(mUI, x, y, 0.f, UI_MOUSE_TYPE_MOVE, -1, ModFlags(mod));
  SetDirty(false);
}
void SteepSynthControl::OnMouseOut()
{
  mMouseInside = false;
  ui_os_mouse_event(mUI, -1.f, -1.f, 0.f, UI_MOUSE_TYPE_MOVE, -1, 0);
  SetDirty(false);
}

// IKeyPress.VK uses win32 VK numbering on every platform; mirrors platform_win32.c.
static int MapKeycode(int vk)
{
  if (vk >= 'A' && vk <= 'Z') return vk + 0x20; // lowercase ASCII, matches PLATFORM_KEY_A..Z
  if (vk >= '0' && vk <= '9') return vk;
  switch (vk)
  {
    case kVK_LEFT:   return PLATFORM_KEY_LEFT;
    case kVK_RIGHT:  return PLATFORM_KEY_RIGHT;
    case kVK_UP:     return PLATFORM_KEY_UP;
    case kVK_DOWN:   return PLATFORM_KEY_DOWN;
    case kVK_HOME:   return PLATFORM_KEY_HOME;
    case kVK_END:    return PLATFORM_KEY_END;
    case kVK_PRIOR:  return PLATFORM_KEY_PAGEUP;
    case kVK_NEXT:   return PLATFORM_KEY_PAGEDOWN;
    case kVK_DELETE: return PLATFORM_KEY_DELETE;
    case kVK_BACK:   return PLATFORM_KEY_BACKSPACE;
    case kVK_RETURN: return PLATFORM_KEY_ENTER;
    case kVK_TAB:    return PLATFORM_KEY_TAB;
    case kVK_ESCAPE: return PLATFORM_KEY_ESCAPE;
    case kVK_INSERT: return PLATFORM_KEY_INSERT;
    default:         return vk;
  }
}

static int ModFlags(const IKeyPress& key)
{
  int f = 0;
  if (key.C) f |= UI_FLAGS_CTRL;
  if (key.S) f |= UI_FLAGS_SHIFT;
  if (key.A) f |= UI_FLAGS_ALT;
  return f;
}

bool SteepSynthControl::OnKeyDown(float /* x */, float /* y */, const IKeyPress& key)
{
  if (!mUI) return false;
  int flags = ModFlags(key);
  ui_os_key_event(mUI, MapKeycode(key.VK), 1, flags);

  // One IKeyPress per keypress, so fire the char event here too.
  if (key.utf8[0] && !key.C && !key.A)
  {
    unsigned char ch = (unsigned char)key.utf8[0];
    if (ch >= 32 && ch < 127)
      ui_os_char_event(mUI, ch, flags);
  }

  SetDirty(false);
  return true;
}

bool SteepSynthControl::OnKeyUp(float /* x */, float /* y */, const IKeyPress& key)
{
  if (!mUI) return false;
  ui_os_key_event(mUI, MapKeycode(key.VK), 0, ModFlags(key));
  SetDirty(false);
  return true;
}
