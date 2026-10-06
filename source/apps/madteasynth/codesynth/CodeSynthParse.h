#pragma once

#include <assert.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include "sample_loader.h"
#include "CodeSynthVmCtx.h"
#ifdef __cplusplus
}
#endif

// vm.h/parser.h have no extern "C" guard of their own.
#ifdef __cplusplus
extern "C" {
#endif
#include "vm/vm.h"
#include "parser/parser.h"
#ifdef __cplusplus
}
#endif

// Numeric domain a sound compiles and runs in; the entry's 4th header field.
enum CodeSynthType : int { kCodeSynthF64 = 0, kCodeSynthF32, kCodeSynthFix };

const char*    CodeSynthTypeToString(CodeSynthType t);
CodeSynthType  CodeSynthTypeFromString(const std::string& s); // unknown/empty -> kCodeSynthF64

// The 8-arg body signature, in the domain's types:
//   t      seconds since note-on
//   hz     note frequency (A4 = 440)
//   phase  t * hz
//   note   MIDI note
//   beat   song position in beats, continuing across note-offs
//   rate   pitch ratio to the sound's root note
//   vel    raw velocity 0..127 (the voice gain already applies vel/127)
//   pan    -1 left, +1 right, 0 centre/mono
// A body naming `pan` renders twice per sample. `if (pan)` means "not centred";
// the right-channel test is `pan > 0`. In fix the values are fx22.
const char* CodeSynthWrapParamsForType(CodeSynthType t);

// `constPan` renames the 8th param so `pan` resolves to the constant that
// CodeSynthCompileForExport(constPan=true) declares; pass the same flag to both.
const char* CodeSynthExportWrapParamForType(CodeSynthType t, bool constPan = false);

class cCodeSynthVoice
{
public:
  Parser*       parser; // owns the interned tokens / AST referenced by vm below
  VM*           vm;     // owns fn
  Func*         fn;     // (t, hz, phase, note, beat, rate, vel, pan) => <expression>; NULL if this slot is unused
  CodeSynthType type;
  VMType        retType;  // cached for the audio thread
  // The body reads `pan`, so it renders once per channel. Always false on the
  // visual VM.
  bool          stereo;
  cSampleData  *sample;  // loaded via the // WAV directive; owned here

  cCodeSynthVoice() : parser(nullptr), vm(nullptr), fn(nullptr), type(kCodeSynthF64), retType{}, stereo(false), sample(nullptr) { }
  ~cCodeSynthVoice() { Reset(); }

  cCodeSynthVoice(const cCodeSynthVoice&) = delete;
  cCodeSynthVoice& operator=(const cCodeSynthVoice&) = delete;

  void Reset()
  {
    if (sample) { sample_free(sample); sample = nullptr; }
    if (vm) { vm_destroy(vm); vm = nullptr; }
    if (parser) { parser_deinit(parser); delete parser; parser = nullptr; }
    fn = nullptr;
    stereo = false;
  }
};

// One domain's shared variables. Every VM in the domain points into `block`, so
// it is sized once and must outlive those VMs.
class cCodeSynthGlobals
{
public:
  VMGlobalTable table{};
  std::vector<unsigned char> block;
  // `block` as the initialiser left it.
  std::vector<unsigned char> initBlock;
  bool        valid = false;   // true once Build() succeeded (or found nothing to do)
  std::string error;           // compile error from the declaration pass, if any

  cCodeSynthGlobals() { }
  ~cCodeSynthGlobals() { Reset(); }
  cCodeSynthGlobals(const cCodeSynthGlobals&) = delete;
  cCodeSynthGlobals& operator=(const cCodeSynthGlobals&) = delete;

  // `commonSrc` is compiled first only for its #directives, which must type these
  // variables as in the bodies.
  bool Build(const std::string& commonSrc, const std::string& globalsSrc, RenderCtx* screen);
  void Reset();

  // Re-zero and re-run the initialisers, so a preview starts every run alike.
  void ResetValues();

  // Realtime-safe: no allocation, no VM run.
  // After a recompile: keeps `prev`'s running values where vm_globals_migrate allows.
  void TakeValuesFrom(const cCodeSynthGlobals& prev);
  void RestoreInitValues();

  bool   Empty() const { return table.count == 0; }
  void*  Data()        { return block.empty() ? nullptr : block.data(); }
  size_t Size()  const { return block.size(); }

  // Call before compiling anything that names one.
  void BindTo(VM* vm) const;

  // C declarations for these variables: a struct type plus one static instance.
  std::string EmitC(const char* typeName, const char* varName) const;

private:
  // Build retries the declaration pass; both attempts end here.
  bool FinishBuild(Func* init, ParseResult& res);

  // Kept alive: the layout's names and the initialiser live in them.
  Parser* mParser = nullptr;
  VM*     mVm     = nullptr;
  Func*   mInit   = nullptr;  // the compiled initialiser; arena-owned by mVm
};

class cCodeSynthReadonly
{
public:
  // Before synth[], so it outlives the VMs pointing into it.
  cCodeSynthGlobals globals;

  cCodeSynthVoice synth[128];
  unsigned char map[16 * 128]; // midi channel * 128 + note -> 1-based index into synth[], 0 = unmapped

  std::string   synthName[128];     // sound name as written in the header
  int           synthRootNote[128]; // mapping metadata only; pitch is A4-relative
  int           synthTranspose[128]; // semitones added to the played note before pitching
  int           synthOrder[128];     // process-order hint; offline exporter only
  unsigned      probeSerial = 0;     // the CodeSynthProbes::serial compiled in, 0 for none

  cCodeSynthReadonly() { memset(map, 0, sizeof(map)); memset(synthRootNote, 0, sizeof(synthRootNote)); memset(synthTranspose, 0, sizeof(synthTranspose)); memset(synthOrder, 0, sizeof(synthOrder)); }
};

// Every entry point below takes the RenderCtx the VM draws into; each plugin
// instance has its own. It is needed at registration, since `width`/`height`/
// `stride` are folded, and again at run time as CodeSynthVmCtx.screen.

// Drawing primitives (see vscreen.h). Never on the audio compile path.
void CodeSynthRegisterCFuncs(VM* vm, RenderCtx* screen);

// Just smp(). Audio compiles and the editor's audio live parse must use this, or
// a visual-only call compiles in the editor yet reads 0 at playback.
void CodeSynthRegisterSmp(VM* vm, RenderCtx* screen);

// `width`/`height` as folded constants: a screen resize must recompile.
void CodeSynthRegisterScreenConsts(VM* vm, RenderCtx* screen);

// Host-buffer ids. Append only: serialized VMSym.offset values refer to them.
enum : int {
  kCodeSynthHostBufScreen  = 0,
  kCodeSynthHostBufPalette = 1,
};

// `screen` ([]u8) and `palette` ([]i32, 0x00RRGGBB) as host buffers. Declared
// on audio VMs too, since `common` is shared; unbound there, so an access fails
// the run.
void CodeSynthRegisterScreenBuffers(VM* vm);

// Binds to the context's framebuffer and palette, or to nothing. Re-done per
// run, since a recompile builds a new VM.
void CodeSynthBindScreenBuffers(VM* vm, RenderCtx* screen, bool bind);

// Sets, replaces, or with an empty path removes the "// WAV <path>, <chain> \\"
// directive, keeping an existing chain.
void CodeSynthSetWavPath(std::string& body, const std::string& wavPath);

// False, with both outputs empty, when the body names no WAV.
bool CodeSynthGetWavPath(const std::string& body, std::string& wavPath, std::string& wavChain);

// An independently owned load, resolved as CodeSynthParse() does; the caller
// sample_free()s it. NULL when there is no WAV or it fails to load.
cSampleData* CodeSynthLoadBodySample(const std::string& body, const char* assetDir);

// Default body for a dropped WAV.
constexpr const char* kCodeSynthDefaultWavBody = "smp(rate, 0)";


// One entry's inspection probes (vm_set_inspect_probes), as byte spans of
// `editorBody`: the editor's text, inside which the compiled body is found.
struct CodeSynthProbe { int id; size_t lo, hi; };
struct CodeSynthProbes {
  std::string entryName;
  std::string editorBody;
  std::vector<CodeSynthProbe> probes;
  unsigned serial = 0;
  bool inPrelude = false;
};

// Where the visual pipeline's inspection goes, on the UI thread: one pass per
// visual tick (begin, the values, end), quoting the probe serial the running
// pipeline was compiled with; and `ran`, once after each new pipeline, with
// what the open entry's first run printed.
struct CodeSynthInspectListener {
  void* user = nullptr;
  void (*begin)(void* user, unsigned probeSerial) = nullptr;
  void (*value)(void* user, int id, int kind, int shift, double v) = nullptr;
  void (*array)(void* user, int id, int kind, int shift, int total, const double* vals, int n) = nullptr;
  void (*end)(void* user) = nullptr;
  void (*ran)(void* user, bool ok, const char* print, int printLen) = nullptr;
};

// `forVisual` picks which body of each entry compiles. Empty bodies are skipped.
// `probes` names the entry whose body they are compiled into.
bool CodeSynthParse(const char* specification, const char* workingDirectoryPath, cCodeSynthReadonly* dest, int debugMessagesMaxSize, const char* debugMessages, RenderCtx* screen, bool forVisual = false,
                    const CodeSynthProbes* probes = nullptr);

// One body compiled as the pipelines compile it, with `probes` in, for the
// editor's replay of the open entry. Release with CodeSynthReleaseExport(vm,
// parser, nullptr). NULL on failure, with `err` set.
Func* CodeSynthCompileProbed(const std::string& body, CodeSynthType type, const std::string& prelude,
                             bool forVisual, const cCodeSynthGlobals* globals, RenderCtx* screen,
                             const CodeSynthProbes& probes, VM** outVm, Parser** outParser, std::string& err);

// Compiles one body standalone, for the exporter and the editor's preview.
// `globals` must be built from the same source as `prelude`. The VM, parser and
// parse result must outlive the Func, including any C emission, which reads
// names from the parse result; release them with CodeSynthReleaseExport().
// `constPan` folds `pan` to 0 for a body only ever rendered centred.
// NULL on failure, with `err` set.
Func* CodeSynthCompileForExport(const std::string& body, const char* wrapParams,
                                  const char* prelude,
                                  const cCodeSynthGlobals* globals, RenderCtx* screen,
                                  VM** outVm, Parser** outParser, ParseResult** outRes,
                                  std::string& err, bool constPan = false);

// Safe with any argument NULL.
void CodeSynthReleaseExport(VM* vm, Parser* parser, ParseResult* res);

// A body is wrapped as `f = (params) => {body}`; the editor uses the same values.
constexpr const char* kCodeSynthWrapName = "f";
constexpr const char* kCodeSynthWrapParams = "t:f64, hz:f64, phase:f64, note:f64, beat:f64, rate:f64, vel:f64, pan:f64";

// FindWrappedVoiceFn matches on the arity. Adding a wrap arg means changing
// every string above.
constexpr int kCodeSynthWrapParamCount = 8;
constexpr int kCodeSynthPanParamIndex  = 7;

// Stack frame for the voice function; a body needing more is skipped.
constexpr size_t kCodeSynthVoiceFrameBytes = 16384;

// Op budget per audio run: once per sample and channel.
constexpr long long kCodeSynthAudioOpBudget = 200000;

// Op budget per visual run: once per frame, whole scenes.
constexpr long long kCodeSynthVisualOpBudget = 3200000;

// Fractional bits of the "fix" domain.
constexpr int kCodeSynthFxShift = 22;

// Clamped to int32: `t` grows unboundedly on a held note.
int CodeSynthFxFromDouble(double v, int fracBits = kCodeSynthFxShift);

// The kCodeSynthWrapParamCount wrap arguments, in kCodeSynthWrapParams order.
void CodeSynthSetArgs(Args* a, CodeSynthType type, const double* v);
void CodeSynthSetArg(Args* a, CodeSynthType type, int i, double v);

// By the inferred return type: a fix body that calls pow() returns f64.
float CodeSynthResultToFloat(VMType rt, Args* a);
// 0 when the run fails.
float CodeSynthRunToFloat(Func* fn, Args* a, VMType rt);

// 2^(semitones/12).
inline double CodeSynthSemitoneRatio(int semitones) { return std::pow(2.0, semitones / 12.0); }

// An empty body reads back as absent.
constexpr const char* kCodeSynthDefaultSoundBody =
  "// right click line nr to enable/disable\n"
  "0 \n"
  "\n"
  "// pulse-width modulation\n"
  "+ ((phase % 1 < (beat/4) % 1) - 0.5) / 4\n"
  "\n"
  "// stereo detuned marimba\n"
  "// + sin01(phase * (1 + pan/100)) * exp(-30*t)\n"
  "\n"
  "// kick drum\n"
  "// + sin(sqrt(t) * 173) * exp(-16*t)";
constexpr const char* kCodeSynthDefaultVisualBody =
  "background 0\n"
  "\n"
  "r = linearstep(1/4, 0, t) * vel/2 + 4\n"
  "text width/2, 28, 16 + r/2, 2, 'HELLO!'\n"
  "circle width/2, height/2, r\n"
  "rect width/2 - r, 170, r * 2, 10, r\n"
  "for i in 0..r\n"
  "    for sign in [-1, 1]\n"
  "        putpixel width/2 - sign*i, 192, i";

// Reserved name for the shared-code block, prepended to every body.
constexpr const char* kCodeSynthCommonName = "common";

constexpr const char* kCodeSynthDefaultCommonBody =
  "// shared helpers -- callable both sound & visuals\n";

// The shared-variable block. Split on "// visual \\" into halves, each prepended
// only to its own domain, so the thread boundary is lexical. Visual bodies run
// once per frame in `order`; audio bodies run voice-major per block, so audio
// globals are block-granular.
constexpr const char* kCodeSynthGlobalsName = "globals";

constexpr const char* kCodeSynthDefaultGlobalsBody =
  "// shared state for sounds\n"
  "your_sound_data = 1.0;";
constexpr const char* kCodeSynthDefaultGlobalsVisualBody =
  "// shared state for visuals\n"
  "your_visual_data = 1.0;";

enum CodeSynthEntryKind : int {
  kEntrySound = 0,  // an ordinary (channel, note) sound/visual entry
  kEntryCommon,     // the shared-helper prelude   -- kCodeSynthCommonName
  kEntryGlobals     // the shared-variable prelude -- kCodeSynthGlobalsName
};

struct CodeSynthEntry
{
  std::string name;
  // Zero-initialised: reserved blocks never assign them.
  int channel = 0; // 1-based, as written in the header
  int note = 0;
  std::string body;
  // Split off by "// visual \\"; runs on the UI-thread visual VM.
  std::string visualBody;
  CodeSynthType type = kCodeSynthF64;
  int transpose = 0;  // optional 5th header field, in semitones
  int order = 0;  // optional: on one tick the lower non-zero order goes first; exporter only

  // Reserved blocks use only `body` (globals also `visualBody`) and are pinned to
  // the front of the list.
  CodeSynthEntryKind kind = kEntrySound;
};

inline bool CodeSynthEntryIsReserved(const CodeSynthEntry& e) { return e.kind != kEntrySound; }

std::string CodeSynthCommonBody(const std::vector<CodeSynthEntry>& entries);

std::string CodeSynthGlobalsBody(const std::vector<CodeSynthEntry>& entries, bool forVisual);

// common, then this domain's globals half. Every body compile must pass it.
std::string CodeSynthPreludeFor(const std::vector<CodeSynthEntry>& entries, bool forVisual);

int CodeSynthCommonIndex(const std::vector<CodeSynthEntry>& entries);
int CodeSynthGlobalsIndex(const std::vector<CodeSynthEntry>& entries);

std::vector<CodeSynthEntry> CodeSynthSplitEntries(const std::string& specification);

std::string CodeSynthSerializeEntries(const std::vector<CodeSynthEntry>& entries);
