#include "CodeSynthParse.h"

extern "C" {
#include "vscreen.h"
#include "parser/tokens.h"
#include "vm/vm_emit_c.h"
}
// ctx-taking thunks; the plain vscreen_* names are what the emitter writes.
#include "vscreen_vm_bind.h"

#include <algorithm>
#include <climits>
#include <string>
#include <cstring>
#include <cstdlib>

// Declared here: its header #defines bool, which breaks C++.
extern "C" char* interactive_coding_wrap_func_body(Tsys* sys, const char* name, const char* params,
                                                     char open, char close, const char* body);

// vm_create()/parser_init() borrow this pointer, so it must outlive them.
static Tsys g_sys = { &std::malloc, &std::realloc, &std::free, &std::memset, &std::memcpy, nullptr, nullptr };

enum : int {
  TOK_PUTPIXEL = TOK_MATHS_LAST,
  TOK_POINT,
  TOK_LINE,
  TOK_ELLIPSE,
  TOK_CIRCLE,
  TOK_RECT,
  TOK_BACKGROUND,
  TOK_GLYPH,
  TOK_TEXT,
  TOK_SMP,
  // Append only: native_tok is serialized into hot-reload blobs.
  TOK_FONT,
  TOK_TEXT_ALIGN,
  TOK_IMAGE_ALLOC,
  TOK_IMAGE_GETPIXEL,
  TOK_PUSH_TARGET,
  TOK_POP_TARGET,
  TOK_IMAGE_SAMPLE,
  TOK_RGB,              // retired: rgb() is a vm_lib.h built-in now
  TOK_COLOR_RAMP_SETUP,
};

// `width`/`height`/`stride` as folded constants, on every VM since `common` is
// shared by both domains. Folded means stale after a resize: whoever resizes
// must recompile. `stride` is the row pitch in bytes, for `screen[x + y*stride]`.
void CodeSynthRegisterScreenConsts(VM* vm, RenderCtx* screen)
{
  if (!vm) return;
  // Off the given context: instances can target different sizes.
  vm_declare_const_num(vm, "width",  (double)vscreen_width_ctx(screen),  1);
  vm_declare_const_num(vm, "height", (double)vscreen_height_ctx(screen), 1);
  vm_declare_const_num(vm, "stride", (double)vscreen_stride_ctx(screen), 1);
}

// The C names are vscreen's, which every export target declares. `= {}`: VMType
// has more fields than these.
void CodeSynthRegisterScreenBuffers(VM* vm)
{
  if (!vm) return;

  // []u8, one GColor8 per pixel.
  VMType screenType = {};
  screenType.kind      = VMT_SLICE_I32;
  screenType.pack_bits = 8;

  // No elem_shift: a packed RGB word has no fixed-point meaning.
  VMType paletteType = {};
  paletteType.kind = VMT_SLICE_I32;

  vm_declare_host_buffer(vm, kCodeSynthHostBufScreen, "screen", screenType,
                         "vscreen_screen", "VSCREEN_SCREEN_LEN");
  vm_declare_host_buffer(vm, kCodeSynthHostBufPalette, "palette", paletteType,
                         "vscreen_palette", "VSCREEN_PALETTE_LEN");

  // The microw8 memory map, for the CurlyWas export: the 320x240 framebuffer at
  // 0x78 and the 256-entry palette at 0x13000, both owned by the platform.
  vm_place_host_buffer(vm, kCodeSynthHostBufScreen, 0x78, 320 * 240);
  vm_place_host_buffer(vm, kCodeSynthHostBufPalette, 0x13000, VSCREEN_PALETTE_LEN);
}

void CodeSynthBindScreenBuffers(VM* vm, RenderCtx* screen, bool bind)
{
  if (!vm) return;
  if (!screen) bind = false;   // nothing to point at
  // The live size: a buffer's length is read at run time, so a resize needs no recompile.
  vm_bind_host_buffer(vm, kCodeSynthHostBufScreen,
                      bind ? vscreen_pixels_ctx(screen) : nullptr,
                      bind ? vscreen_width_ctx(screen) * vscreen_height_ctx(screen) : 0);
  vm_bind_host_buffer(vm, kCodeSynthHostBufPalette,
                      bind ? vscreen_palette_ctx(screen) : nullptr,
                      bind ? vscreen_palette_len_ctx(screen) : 0);
}

// smp(rate, filter): the voice's WAV via user_data. An audio primitive.
void CodeSynthRegisterSmp(VM* vm, RenderCtx* screen)
{
  if (!vm) return;
  CodeSynthRegisterScreenConsts(vm, screen);
  CodeSynthRegisterScreenBuffers(vm);
  // No i32 variant: `rate` is a ratio, and an int slot would truncate the sample.
  // i32 NULL promotes to f64; fx22 args take smp_fx22.
  register_c_func_2arg_ctx(vm, TOK_SMP, "smp",
      "smp_f32",  smp_f32,
      "smp_f64",  smp_f64,
      NULL,       NULL,
      "smp_fx22", smp_fx22,
      22);
  register_c_func_param_names(vm, TOK_SMP, "rate, filter");
}

void CodeSynthRegisterCFuncs(VM* vm, RenderCtx* screen)
{
  if (!vm) return;

  CodeSynthRegisterScreenConsts(vm, screen);
  CodeSynthRegisterScreenBuffers(vm);

  // Each primitive registers one numeric variant; compile_call coerces every
  // argument to it. The fn pointer is a *_ctx thunk reaching the screen through
  // user_data (a CodeSynthVmCtx, which must be set before func_run); the name is
  // the flat vscreen_* function song.c calls.
  register_c_func_3arg_ctx(vm, TOK_PUTPIXEL, "putpixel",
      NULL, NULL, NULL, NULL,
      "vscreen_putpixel_i32", vscreen_putpixel_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_PUTPIXEL, "x, y, color");
  register_c_func_no_value(vm, TOK_PUTPIXEL);

  register_c_func_4arg_ctx(vm, TOK_POINT, "point",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_point_fx16", vscreen_point_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_POINT, "x, y, amount, blend");
  register_c_func_no_value(vm, TOK_POINT);

  // alpha is literal coverage (0 draws nothing), so an omitted alpha defaults to
  // 1.0. The index is 0-based over script-visible params.

  // line(x1,y1,x2,y2,stroke_width[,alpha,blend])
  register_c_func_7arg_ctx(vm, TOK_LINE, "line",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_line_fx16", vscreen_line_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_LINE, "x1, y1, x2, y2, stroke_width, alpha, blend");
  register_c_func_no_value(vm, TOK_LINE);
  register_c_func_defaults(vm, TOK_LINE, 2);
  register_c_func_default_one(vm, TOK_LINE, 5);

  // ellipse(x,y,radius_w,radius_h[,alpha,blend])
  register_c_func_6arg_ctx(vm, TOK_ELLIPSE, "ellipse",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_ellipse_fx16", vscreen_ellipse_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_ELLIPSE, "x, y, radius_w, radius_h, alpha, blend");
  register_c_func_no_value(vm, TOK_ELLIPSE);
  register_c_func_defaults(vm, TOK_ELLIPSE, 2);
  register_c_func_default_one(vm, TOK_ELLIPSE, 4);

  // circle(x,y,radius[,alpha,blend])
  register_c_func_5arg_ctx(vm, TOK_CIRCLE, "circle",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_circle_fx16", vscreen_circle_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_CIRCLE, "x, y, radius, alpha, blend");
  register_c_func_no_value(vm, TOK_CIRCLE);
  register_c_func_defaults(vm, TOK_CIRCLE, 2);
  register_c_func_default_one(vm, TOK_CIRCLE, 3);

  // rect(x,y,w,h,col[,alpha,blend])
  register_c_func_7arg_ctx(vm, TOK_RECT, "rect",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_rect_fx16", vscreen_rect_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_RECT, "x, y, w, h, color, alpha, blend");
  register_c_func_no_value(vm, TOK_RECT);
  register_c_func_defaults(vm, TOK_RECT, 2);
  register_c_func_default_one(vm, TOK_RECT, 5);

  // background(col[,transparency]); fx16, with col shifted back down.
  register_c_func_2arg_ctx(vm, TOK_BACKGROUND, "background",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_background_fx16", vscreen_background_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_BACKGROUND, "color, transparency");
  register_c_func_no_value(vm, TOK_BACKGROUND);
  register_c_func_defaults(vm, TOK_BACKGROUND, 1);

  // glyph(x,y,size,stroke_width,ascii[,alpha,blend])
  register_c_func_7arg_ctx(vm, TOK_GLYPH, "glyph",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_glyph_fx16", vscreen_glyph_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_GLYPH, "x, y, size, stroke_width, ascii, alpha, blend");
  register_c_func_no_value(vm, TOK_GLYPH);
  register_c_func_defaults(vm, TOK_GLYPH, 2);
  register_c_func_default_one(vm, TOK_GLYPH, 5);

  // text(x,y,size,stroke_width,"str"[,alpha,blend,letter_spacing,line_height]):
  // the mixed-signature ABI; the slice arrives as packed bytes.
  {
    VTKind text_sig[9] = { VMT_I32, VMT_I32, VMT_I32, VMT_I32,
                           VMT_SLICE_I32, VMT_I32, VMT_I32, VMT_I32, VMT_I32 };
    register_c_func_sig_ctx(vm, TOK_TEXT, "text",
        "vscreen_text", (void*)vscreen_text_ctx,
        9, text_sig, VMT_VOID, FX16_SHIFT);
    register_c_func_arg_bytes(vm, TOK_TEXT, 4);
    register_c_func_defaults(vm, TOK_TEXT, 4);
    register_c_func_default_one(vm, TOK_TEXT, 5);
    register_c_func_param_names(vm, TOK_TEXT,
        "x, y, size, stroke_width, str, alpha, blend, letter_spacing, line_height");
  }

  // font(id), raw int.
  register_c_func_1arg_ctx(vm, TOK_FONT, "font",
      NULL, NULL, NULL, NULL,
      "vscreen_font_i32", vscreen_font_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_FONT, "id");
  register_c_func_no_value(vm, TOK_FONT);

  // text_align(id), raw int.
  register_c_func_1arg_ctx(vm, TOK_TEXT_ALIGN, "text_align",
      NULL, NULL, NULL, NULL,
      "vscreen_text_align_i32", vscreen_text_align_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_TEXT_ALIGN, "id");
  register_c_func_no_value(vm, TOK_TEXT_ALIGN);

  // Off-screen images: image_alloc(w,h), image_getpixel(id,x,y), push_target(id),
  // pop_target(). Raw ints.
  register_c_func_2arg_ctx(vm, TOK_IMAGE_ALLOC, "image_alloc",
      NULL, NULL, NULL, NULL,
      "vscreen_image_alloc_i32", vscreen_image_alloc_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_IMAGE_ALLOC, "w, h");

  register_c_func_3arg_ctx(vm, TOK_IMAGE_GETPIXEL, "image_getpixel",
      NULL, NULL, NULL, NULL,
      "vscreen_image_getpixel_i32", vscreen_image_getpixel_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_IMAGE_GETPIXEL, "image_id, x, y");

  // image_sample(id,x,y): bilinear, so fx16; `id` is shifted back down.
  register_c_func_3arg_ctx(vm, TOK_IMAGE_SAMPLE, "image_sample",
      NULL, NULL, NULL, NULL, NULL, NULL,
      "vscreen_image_sample_fx16", vscreen_image_sample_fx16_ctx,
      FX16_SHIFT);
  register_c_func_param_names(vm, TOK_IMAGE_SAMPLE, "image_id, x, y");

  register_c_func_1arg_ctx(vm, TOK_PUSH_TARGET, "push_target",
      NULL, NULL, NULL, NULL,
      "vscreen_push_target_i32", vscreen_push_target_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_PUSH_TARGET, "image_id");

  // No zero-arg registration: one arg, defaulted.
  register_c_func_1arg_ctx(vm, TOK_POP_TARGET, "pop_target",
      NULL, NULL, NULL, NULL,
      "vscreen_pop_target_i32", vscreen_pop_target_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_POP_TARGET, "");
  register_c_func_no_value(vm, TOK_POP_TARGET);
  register_c_func_defaults(vm, TOK_POP_TARGET, 1);

  // color_ramp_setup(add,bit_offset,low[,mid,high]), raw ints.
  register_c_func_5arg_ctx(vm, TOK_COLOR_RAMP_SETUP, "color_ramp_setup",
      NULL, NULL, NULL, NULL,
      "vscreen_color_ramp_setup_i32", vscreen_color_ramp_setup_i32_ctx,
      NULL, NULL, 0);
  register_c_func_param_names(vm, TOK_COLOR_RAMP_SETUP, "add, bit_offset, low, mid, high");
  register_c_func_defaults(vm, TOK_COLOR_RAMP_SETUP, 2);

  CodeSynthRegisterSmp(vm, screen);
}

const char* CodeSynthTypeToString(CodeSynthType t)
{
  switch (t)
  {
    case kCodeSynthF32: return "f32";
    case kCodeSynthFix: return "fix";
    case kCodeSynthF64: default: return "f64";
  }
}

CodeSynthType CodeSynthTypeFromString(const std::string& s)
{
  if (s == "f32") return kCodeSynthF32;
  if (s == "fix") return kCodeSynthFix;
  return kCodeSynthF64;  // unknown/empty -> f64
}

// Every body compiles under all eight names; func_param_used() drops unused ones
// from the C signature.
const char* CodeSynthWrapParamsForType(CodeSynthType t)
{
  switch (t)
  {
    case kCodeSynthF32: return "t:f32, hz:f32, phase:f32, note:f32, beat:f32, rate:f32, vel:f32, pan:f32";
    case kCodeSynthFix: return "t:fx22, hz:fx22, phase:fx22, note:fx22, beat:fx22, rate:fx22, vel:fx22, pan:fx22";
    case kCodeSynthF64: default: return "t:f64, hz:f64, phase:f64, note:f64, beat:f64, rate:f64, vel:f64, pan:f64";
  }
}

const char* CodeSynthExportWrapParamForType(CodeSynthType t, bool constPan)
{
  if (!constPan) return CodeSynthWrapParamsForType(t);
  // Arity kept, since FindWrappedVoiceFn matches on it.
  switch (t)
  {
    case kCodeSynthF32: return "t:f32, hz:f32, phase:f32, note:f32, beat:f32, rate:f32, vel:f32, pan_centred_:f32";
    case kCodeSynthFix: return "t:fx22, hz:fx22, phase:fx22, note:fx22, beat:fx22, rate:fx22, vel:fx22, pan_centred_:fx22";
    case kCodeSynthF64: default: return "t:f64, hz:f64, phase:f64, note:f64, beat:f64, rate:f64, vel:f64, pan_centred_:f64";
  }
}

int CodeSynthFxFromDouble(double v, int fracBits)
{
  double scaled = v * (double)(1LL << fracBits);
  if (scaled != scaled) scaled = 0.0; // NaN
  if (scaled > (double)INT_MAX) scaled = (double)INT_MAX;
  if (scaled < (double)INT_MIN) scaled = (double)INT_MIN;
  return (int)std::llround(scaled);
}

void CodeSynthSetArg(Args* a, CodeSynthType type, int i, double v)
{
  switch (type)
  {
    case kCodeSynthF32: args_set_f32(a, i, (float)v); break;
    case kCodeSynthFix: args_set_i32(a, i, CodeSynthFxFromDouble(v)); break;
    case kCodeSynthF64: default: args_set_f64(a, i, v); break;
  }
}

void CodeSynthSetArgs(Args* a, CodeSynthType type, const double* v)
{
  switch (type)
  {
    case kCodeSynthF32: for (int i = 0; i < kCodeSynthWrapParamCount; i++) args_set_f32(a, i, (float)v[i]); break;
    case kCodeSynthFix: for (int i = 0; i < kCodeSynthWrapParamCount; i++) args_set_i32(a, i, CodeSynthFxFromDouble(v[i])); break;
    case kCodeSynthF64: default: for (int i = 0; i < kCodeSynthWrapParamCount; i++) args_set_f64(a, i, v[i]); break;
  }
}

float CodeSynthResultToFloat(VMType rt, Args* a)
{
  switch (rt.kind)
  {
    case VMT_I32:
    {
      long long raw = args_get_i32_result(a);
      int shift = rt.len;
      return shift ? (float)((double)raw / (double)(1LL << shift)) : (float)raw;
    }
    case VMT_F32: return args_get_f32_result(a);
    case VMT_F64: default: return (float)args_get_f64_result(a);
  }
}

float CodeSynthRunToFloat(Func* fn, Args* a, VMType rt)
{
  return func_run(fn, a, kCodeSynthAudioOpBudget) == VM_OK ? CodeSynthResultToFloat(rt, a) : 0.0f;
}

namespace {
const char* kSoundSep = "//---- ";
const char* kHeaderSep = " ----\\\\"; // header lines end with two literal backslashes
// Splits a body into its sound and optional visual halves.
const char* kVisualSep = "// visual \\\\";
const char* kWavPrefix = "// WAV ";
const char* kWavSuffix = " \\\\";

std::string trim(const std::string& s)
{
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool splitFirst(const std::string& s, const char* sep, std::string& head, std::string& tail)
{
  size_t pos = s.find(sep);
  if (pos == std::string::npos) return false;
  head = s.substr(0, pos);
  tail = s.substr(pos + strlen(sep));
  return true;
}

// POSIX, drive or UNC path. Only relative WAV paths join the working directory.
static bool wavIsAbsolute(const std::string& p)
{
  if (!p.empty() && (p[0] == '/' || p[0] == '\\')) return true;
  return p.size() >= 2 && p[1] == ':';
}

bool extractWavDirective(std::string& body, std::string& wavPath, std::string& wavChain)
{
  size_t pos = body.find(kWavPrefix);
  if (pos == std::string::npos) return false;
  size_t start = pos + strlen(kWavPrefix);
  size_t end = body.find(kWavSuffix, start);
  if (end == std::string::npos) return false;

  std::string directive = body.substr(start, end - start);

  size_t comma = directive.find(',');
  if (comma != std::string::npos) {
    wavPath = trim(directive.substr(0, comma));
    wavChain = trim(directive.substr(comma + 1));
  } else {
    wavPath = trim(directive);
    wavChain.clear();
  }

  body.erase(pos, end - pos + strlen(kWavSuffix));
  return true;
}

// Fields after the name are positional (channel, note, type, transpose, order)
// or key=value, mixed freely:
//   pwm, 1, 36, fix, 12, 3
//   pwm, channel=1, note=36, type=fix, transpose=12, order=3
// Empty type -> f64; empty transpose/order -> 0.
bool splitHeaderFields(const std::string& header, std::string& name, std::string& chStr, std::string& noteStr, std::string& typeStr, std::string& transposeStr, std::string& orderStr)
{
  std::vector<std::string> fields;
  {
    std::string rest = header, head, tail;
    while (splitFirst(rest, ",", head, tail)) { fields.push_back(trim(head)); rest = tail; }
    fields.push_back(trim(rest));
  }
  if (fields.empty()) return false;

  name = fields[0];

  std::string* positional[] = { &chStr, &noteStr, &typeStr, &transposeStr, &orderStr };
  const int kNumPositional = (int)(sizeof(positional) / sizeof(positional[0]));
  int posCursor = 0;

  for (size_t i = 1; i < fields.size(); i++)
  {
    const std::string& f = fields[i];
    if (f.empty()) continue;

    // Named only for recognised keys; anything else stays positional.
    size_t eq = f.find('=');
    if (eq != std::string::npos)
    {
      std::string key = trim(f.substr(0, eq));
      std::string val = trim(f.substr(eq + 1));
      if      (key == "channel")   { chStr = val;        continue; }
      else if (key == "note")      { noteStr = val;      continue; }
      else if (key == "type")      { typeStr = val;      continue; }
      else if (key == "transpose") { transposeStr = val; continue; }
      else if (key == "order")     { orderStr = val;     continue; }
    }

    if (posCursor < kNumPositional)
      *positional[posCursor++] = f;
  }

  return true;
}

// Shared by CodeSynthParse and CodeSynthSplitEntries. channel/note are kept as
// parsed, even out of range, so the editor can show them.
std::vector<CodeSynthEntry> splitRawSounds(const std::string& content, std::string& log)
{
  std::vector<CodeSynthEntry> out;
  int commonIdx  = -1;  // first common block in `out`
  int globalsIdx = -1; // ditto for the globals block

  size_t pos = content.find(kSoundSep);
  while (pos != std::string::npos)
  {
    size_t soundStart = pos + strlen(kSoundSep);
    size_t next = content.find(kSoundSep, soundStart);
    std::string chunk = content.substr(soundStart, next == std::string::npos ? std::string::npos : next - soundStart);
    pos = next;

    std::string header, body;
    if (!splitFirst(chunk, kHeaderSep, header, body))
    {
      log += "codesynth: missing '" + std::string(kHeaderSep) + "' after '//---- '\n";
      continue;
    }

    CodeSynthEntry entry;
    std::string chStr, noteStr, typeStr, transposeStr, orderStr;
    if (!splitHeaderFields(trim(header), entry.name, chStr, noteStr, typeStr, transposeStr, orderStr))
    {
      log += "codesynth: bad header '" + trim(header) + "' (expected 'name, channel, note')\n";
      continue;
    }

    // Recognised by name alone, and not split on the visual divider.
    if (trim(entry.name) == kCodeSynthCommonName)
    {
      entry.kind = kEntryCommon;
      entry.name = kCodeSynthCommonName; // canonical, so serialize round-trips
      entry.body = trim(body);
      if (commonIdx < 0)
      {
        commonIdx = (int)out.size();
        out.push_back(entry);
      }
      else if (!entry.body.empty())
      {
        // Appended rather than dropped, so no code is lost.
        log += "codesynth: more than one 'common' block -- appended to the first\n";
        if (!out[commonIdx].body.empty()) out[commonIdx].body += "\n";
        out[commonIdx].body += entry.body;
      }
      continue;
    }

    // Recognised the same way, but split on the visual divider: the halves are
    // separate storage.
    if (trim(entry.name) == kCodeSynthGlobalsName)
    {
      entry.kind = kEntryGlobals;
      entry.name = kCodeSynthGlobalsName; // canonical, so serialize round-trips
      std::string gSound, gVisual;
      if (splitFirst(body, kVisualSep, gSound, gVisual))
      {
        entry.body = trim(gSound);
        entry.visualBody = trim(gVisual);
      }
      else
      {
        entry.body = trim(body);
      }
      if (globalsIdx < 0)
      {
        globalsIdx = (int)out.size();
        out.push_back(entry);
      }
      else
      {
        // Appended: dropping one would drop declarations.
        log += "codesynth: more than one 'globals' block -- appended to the first\n";
        if (!entry.body.empty())
        {
          if (!out[globalsIdx].body.empty()) out[globalsIdx].body += "\n";
          out[globalsIdx].body += entry.body;
        }
        if (!entry.visualBody.empty())
        {
          if (!out[globalsIdx].visualBody.empty()) out[globalsIdx].visualBody += "\n";
          out[globalsIdx].visualBody += entry.visualBody;
        }
      }
      continue;
    }

    std::string soundPart, visualPart;
    if (splitFirst(body, kVisualSep, soundPart, visualPart))
    {
      entry.body = trim(soundPart);
      entry.visualBody = trim(visualPart);
    }
    else
    {
      entry.body = trim(body);
    }
    entry.channel = atoi(chStr.c_str());
    entry.note = atoi(noteStr.c_str());
    entry.type = CodeSynthTypeFromString(typeStr);
    entry.transpose = transposeStr.empty() ? 0 : atoi(transposeStr.c_str());
    entry.order = orderStr.empty() ? 0 : atoi(orderStr.c_str());

    out.push_back(entry);
  }

  // Pin reserved blocks to the front (common, then globals); stable, so sound
  // order is untouched.
  std::stable_sort(out.begin(), out.end(),
                   [](const CodeSynthEntry& a, const CodeSynthEntry& b) {
                     auto rank = [](const CodeSynthEntry& e) {
                       return e.kind == kEntryCommon ? 0 : (e.kind == kEntryGlobals ? 1 : 2);
                     };
                     return rank(a) < rank(b);
                   });

  return out;
}

// common goes before the wrap: it is a sibling of the voice function. The
// emitter's content-hash dedup collapses the per-body copies.
std::string PrependCommon(const std::string& commonBody, const std::string& wrapped)
{
  if (commonBody.empty()) return wrapped;
  return commonBody + "\n" + wrapped;
}

int CommonPreludeLines(const std::string& commonBody)
{
  if (commonBody.empty()) return 0;
  int n = 1; // the separating "\n" PrependCommon adds
  for (char c : commonBody) if (c == '\n') n++;
  return n;
}

// Re-based on the body; positions inside the common block are labelled.
static std::string ErrorPosPrefix(int row, int col, int headerLines)
{
  char buf[64];
  if (row > headerLines) snprintf(buf, sizeof(buf), "%d:%d: ", row - headerLines, col);
  else                   snprintf(buf, sizeof(buf), "common block %d:%d: ", row, col);
  return std::string(buf);
}

static std::string ShiftVmErrorRow(const char* msg, int headerLines)
{
  if (!msg) return "?";
  const char* s = msg;
  int row = 0, col = 0, nd = 0;
  while (*s >= '0' && *s <= '9') { row = row * 10 + (*s++ - '0'); nd++; }
  if (!nd || *s != ':') return msg;
  s++; nd = 0;
  while (*s >= '0' && *s <= '9') { col = col * 10 + (*s++ - '0'); nd++; }
  if (!nd || s[0] != ':' || s[1] != ' ') return msg;
  return ErrorPosPrefix(row, col, headerLines) + (s + 2);
}

static std::string ParseErrorWithRow(const ParseResult& res, const char* perr, int headerLines)
{
  if (!perr) perr = "?";
  if (res.error_row < 0 || res.error_col < 0) return perr;
  return ErrorPosPrefix(res.error_row + 1, res.error_col + 1, headerLines) + perr;
}

// Name and arity: an 8-arg helper, or one named "f", must not pass for the voice.
Func* FindWrappedVoiceFn(VM* vm)
{
  for (Func* g = vm->run.funcs; g; g = g->next)
  {
    if (g->native_tok != 0 || g->name == 0 || g->is_template) continue;
    if (g->n_params != kCodeSynthWrapParamCount) continue;
    const char* n = intern_get_cstr(vm->intern, g->name);
    if (n && strcmp(n, kCodeSynthWrapName) == 0) return g;
  }
  return nullptr;
}

// Probes are spans of the editor's text; the body compiled is found inside it
// (it may have been trimmed, or lost a WAV directive), and `bodyOff` is where
// it starts in `src`. A span outside the body names nothing.
void ApplyProbes(VM* vm, const ParseResult& res, const std::string& src, size_t bodyOff,
                 const std::string& body, const std::string& commonBody, const CodeSynthProbes* probes)
{
  if (!probes || probes->probes.empty()) return;

  // A prelude editor's text sits inside the common+globals text that opens `src`.
  size_t k, len, off;
  if (probes->inPrelude)
  {
    size_t at = commonBody.rfind(probes->editorBody);
    if (probes->editorBody.empty() || at == std::string::npos) return;
    k = 0; len = probes->editorBody.size(); off = at;
  }
  else
  {
    k = probes->editorBody.find(body);
    if (k == std::string::npos) return;
    len = body.size(); off = bodyOff;
  }
  VMProbe vp[VM_MAX_PROBES];
  int n = 0;
  for (const CodeSynthProbe& p : probes->probes)
  {
    if (p.lo < k || p.hi > k + len || p.hi <= p.lo || n >= VM_MAX_PROBES) continue;
    ASTNode* node = vm_probe_node_for_span(&res, src.c_str(), src.size(), off + p.lo - k, off + p.hi - k);
    if (!node) continue;
    vp[n].node = node;
    vp[n].id = p.id;
    n++;
  }
  vm_set_inspect_probes(vm, vp, n);
}

// `registerPutpixel` is false for the audio body, which keeps graphics off the
// audio thread.
Func* CompileVoiceFn(const char* wrapParams, const std::string& body, const std::string& commonBody,
                      bool registerPutpixel, const cCodeSynthGlobals* globals, RenderCtx* screen,
                      VM** outVm, Parser** outParser, std::string& err,
                      const CodeSynthProbes* probes = nullptr)
{
  *outVm = nullptr;
  *outParser = nullptr;

  char* wrapped = interactive_coding_wrap_func_body(&g_sys, kCodeSynthWrapName, wrapParams, '{', '}', body.c_str());
  if (!wrapped)
  {
    err = "failed to wrap function body";
    return nullptr;
  }
  // Newline and '}' follow the body in the wrap.
  size_t bodyOff = strlen(wrapped) - body.size() - 2 + (commonBody.empty() ? 0 : commonBody.size() + 1);
  std::string src = PrependCommon(commonBody, wrapped);
  g_sys.free(wrapped);

  const int headerLines = CommonPreludeLines(commonBody) + 1;

  Parser* parser = new Parser();
  parser_init(parser, &g_sys);
  ParseResult res = parse_to_asts(parser, src.c_str(), PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
  const char* perr = parser_get_error(&res);
  if (perr)
  {
    err = std::string("parse error: ") + ParseErrorWithRow(res, perr, headerLines);
    free_parse_result(parser, &res);
    parser_deinit(parser);
    delete parser;
    return nullptr;
  }

  VM* vm = vm_create(&g_sys, parser);
  // Audio bodies get smp only.
  if (registerPutpixel) CodeSynthRegisterCFuncs(vm, screen);
  else                  CodeSynthRegisterSmp(vm, screen);
  // Before the body compiles, or an assignment to a shared variable declares a local.
  if (globals) globals->BindTo(vm);
  ApplyProbes(vm, res, src, bodyOff, body, commonBody, probes);
  Func* main_f = func_create(vm, res.code_tree, &res);
  if (!main_f)
  {
    err = std::string("compile error: ") + ShiftVmErrorRow(vm_last_error(vm), headerLines);
    vm_destroy(vm);
    free_parse_result(parser, &res);
    parser_deinit(parser);
    delete parser;
    return nullptr;
  }

  Func* voiceFn = FindWrappedVoiceFn(vm);

  // Params are typed, so the AST isn't needed after compile.
  free_parse_result(parser, &res);

  if (!voiceFn)
  {
    err = "did not produce a usable (t, hz, phase, note, beat, rate, vel, pan) function";
    vm_destroy(vm);
    parser_deinit(parser);
    delete parser;
    return nullptr;
  }

  *outVm = vm;
  *outParser = parser;
  return voiceFn;
}
} // namespace

// An existing directive keeps its chain; an empty path removes it.
void CodeSynthSetWavPath(std::string& body, const std::string& wavPath)
{
  size_t pos = body.find(kWavPrefix);
  size_t end = (pos == std::string::npos)
             ? std::string::npos
             : body.find(kWavSuffix, pos + strlen(kWavPrefix));

  if (wavPath.empty()) {
    if (pos != std::string::npos && end != std::string::npos) {
      body.erase(pos, end - pos + strlen(kWavSuffix));
      body = trim(body);
    }
    return;
  }

  std::string chain;
  if (pos != std::string::npos && end != std::string::npos) {
    std::string directive = body.substr(pos + strlen(kWavPrefix), end - pos - strlen(kWavPrefix));
    size_t comma = directive.find(',');
    if (comma != std::string::npos) chain = trim(directive.substr(comma + 1));
  }

  std::string line = std::string(kWavPrefix) + wavPath;
  if (!chain.empty()) line += ", " + chain;
  line += kWavSuffix;

  if (pos != std::string::npos && end != std::string::npos) {
    body.replace(pos, end - pos + strlen(kWavSuffix), line);
    return;
  }
  body = line + "\n\n" + body;
}

// Outside the anonymous namespace: a header-declared function defined in there
// would fail to link.
bool CodeSynthGetWavPath(const std::string& body, std::string& wavPath, std::string& wavChain)
{
  wavPath.clear();
  wavChain.clear();
  std::string scratch = body; // extractWavDirective erases the directive it finds
  return extractWavDirective(scratch, wavPath, wavChain);
}

cSampleData* CodeSynthLoadBodySample(const std::string& body, const char* assetDir)
{
  std::string wavPath, wavChain;
  if (!CodeSynthGetWavPath(body, wavPath, wavChain) || wavPath.empty()) return nullptr;
  std::string fullPath = wavIsAbsolute(wavPath)
                       ? wavPath
                       : std::string(assetDir ? assetDir : "") + "/" + wavPath;
  return sample_load_wav(fullPath.c_str(), wavChain.empty() ? nullptr : wavChain.c_str());
}

std::vector<CodeSynthEntry> CodeSynthSplitEntries(const std::string& specification)
{
  std::string log; // discarded: callers wanting diagnostics use CodeSynthParse
  return splitRawSounds(specification, log);
}

int CodeSynthCommonIndex(const std::vector<CodeSynthEntry>& entries)
{
  for (size_t i = 0; i < entries.size(); i++)
    if (entries[i].kind == kEntryCommon) return (int)i;
  return -1;
}

int CodeSynthGlobalsIndex(const std::vector<CodeSynthEntry>& entries)
{
  for (size_t i = 0; i < entries.size(); i++)
    if (entries[i].kind == kEntryGlobals) return (int)i;
  return -1;
}

std::string CodeSynthCommonBody(const std::vector<CodeSynthEntry>& entries)
{
  int i = CodeSynthCommonIndex(entries);
  return i < 0 ? std::string() : entries[(size_t)i].body;
}

std::string CodeSynthGlobalsBody(const std::vector<CodeSynthEntry>& entries, bool forVisual)
{
  int i = CodeSynthGlobalsIndex(entries);
  if (i < 0) return std::string();
  return forVisual ? entries[(size_t)i].visualBody : entries[(size_t)i].body;
}

std::string CodeSynthPreludeFor(const std::vector<CodeSynthEntry>& entries, bool forVisual)
{
  std::string common  = CodeSynthCommonBody(entries);
  std::string globals = CodeSynthGlobalsBody(entries, forVisual);
  // common first: globals may call its helpers.
  if (globals.empty()) return common;
  if (common.empty())  return globals;
  return common + "\n" + globals;
}

std::string CodeSynthSerializeEntries(const std::vector<CodeSynthEntry>& entries)
{
  std::string out;
  for (const CodeSynthEntry& e : entries)
  {
    // Reserved blocks keep their body even when empty.
    if (e.kind == kEntryCommon || e.kind == kEntryGlobals)
    {
      out += "//---- ";
      out += (e.kind == kEntryCommon ? kCodeSynthCommonName : kCodeSynthGlobalsName);
      out += " ----\\\\\r\n\r\n";
      out += e.body;
      out += "\r\n\r\n";
      // Written whenever non-empty, even under an empty sound half.
      if (e.kind == kEntryGlobals && !e.visualBody.empty())
      {
        out += "// visual \\\\\r\n\r\n";
        out += e.visualBody;
        out += "\r\n\r\n";
      }
      continue;
    }

    out += "//---- ";
    out += e.name;
    out += ", ";
    out += std::to_string(e.channel);
    out += ", ";
    out += std::to_string(e.note);
    out += ", ";
    out += CodeSynthTypeToString(e.type);
    if (e.transpose != 0)
    {
      out += ", ";
      out += std::to_string(e.transpose);
    }
    // Named, so it round-trips with or without a transpose.
    if (e.order != 0)
    {
      out += ", order=";
      out += std::to_string(e.order);
    }
    out += " ----\\\\\r\n\r\n";
    out += e.body;
    out += "\r\n\r\n";
    if (!e.visualBody.empty())
    {
      out += "// visual \\\\\r\n\r\n";
      out += e.visualBody;
      out += "\r\n\r\n";
    }
  }
  return out;
}

// Compiles `common` alone, only to diagnose two mistakes: a helper naming a
// shared variable (it compiles in one domain and breaks every body in the
// other), and a top-level variable, which is a dead local.
static void DiagnoseCommonBlock(const std::string& commonBody,
                                const cCodeSynthGlobals& globals,
                                bool forVisual, RenderCtx* screen, std::string& log)
{
  if (commonBody.find_first_not_of(" \t\r\n") == std::string::npos) return;

  Parser* parser = new Parser();
  parser_init(parser, &g_sys);
  ParseResult res = parse_to_asts(parser, commonBody.c_str(), PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
  if (parser_get_error(&res))
  {
    // Syntax errors are reported per body with usable positions.
    free_parse_result(parser, &res);
    parser_deinit(parser); delete parser;
    return;
  }

  VM* vm = vm_create(&g_sys, parser);
  CodeSynthRegisterCFuncs(vm, screen);
  globals.BindTo(vm);
  Func* f = func_create(vm, res.code_tree, &res);

  const char* domain = forVisual ? "visual" : "sound";
  if (!f)
  {
    const char* verr = vm_last_error(vm);
    log += std::string("codesynth: the common block does not compile for the ") + domain +
           " domain: " + (verr ? verr : "?") + "\n";
    log += "codesynth:   helpers in 'common' are shared by BOTH domains, so they can only use\n";
    log += "codesynth:   shared variables declared in both halves of 'globals' -- otherwise move\n";
    log += "codesynth:   the helper into the 'globals' half that owns the variable\n";
  }
  else
  {
    for (int i = 0; i < f->n_syms; i++)
    {
      const VMSym& s = f->syms[i];
      if (s.is_func || s.is_global || s.name == 0 || s.type.kind == VMT_VOID) continue;
      const char* nm = intern_get_cstr(vm->intern, s.name);
      if (!nm) continue;
      log += std::string("codesynth: '") + nm + "' is a variable in the common block, which holds "
             "helpers only -- move it to the 'globals' block to share it\n";
    }
  }

  vm_destroy(vm);
  free_parse_result(parser, &res);
  parser_deinit(parser); delete parser;
}

bool CodeSynthParse(const char* specification, const char* workingDirectoryPath, cCodeSynthReadonly* dest, int debugMessagesMaxSize, const char* debugMessages, RenderCtx* screen, bool forVisual,
                    const CodeSynthProbes* probes)
{
  (void)workingDirectoryPath;

  if (!specification || !dest)
  {
    if (debugMessages && debugMessagesMaxSize > 0)
    {
      strncpy((char*)debugMessages, "Invalid parameters", debugMessagesMaxSize - 1);
      ((char*)debugMessages)[debugMessagesMaxSize - 1] = '\0';
    }
    return false;
  }

  std::string log;
  int count = 0;

  std::vector<CodeSynthEntry> raw = splitRawSounds(specification, log);

  // common, then this domain's globals half.
  const std::string commonBody = CodeSynthPreludeFor(raw, forVisual);

  // Layout fixed once before any body compiles, so a body's #directive can't
  // retype the shared declarations.
  if (!dest->globals.Build(CodeSynthCommonBody(raw), CodeSynthGlobalsBody(raw, forVisual), screen))
    log += "codesynth: " + dest->globals.error + "\n";

  DiagnoseCommonBlock(CodeSynthCommonBody(raw), dest->globals, forVisual, screen, log);

  for (const CodeSynthEntry& entry : raw)
  {
    if (CodeSynthEntryIsReserved(entry)) continue;

    const std::string& name = entry.name;
    const std::string& body = forVisual ? entry.visualBody : entry.body;

    if (count >= 128)
    {
      log += "codesynth: '" + name + "' ignored, too many sounds (max 128)\n";
      continue;
    }
    if (name.empty())
    {
      log += "codesynth: entry has an empty name\n";
      continue;
    }

    // Headers are 1-based. Resolved before the empty check: a missing half still
    // claims its slot.
    int channel = entry.channel - 1;
    int note = entry.note;
    if (channel < 0 || channel > 15 || note < 0 || note > 127)
    {
      log += "codesynth: '" + name + "' channel/note out of range (" + std::to_string(entry.channel) + ", " + std::to_string(note) + ")\n";
      continue;
    }

    if (body.empty())
    {
      // A missing half still claims its (channel, note) with a NULL-fn slot, or the
      // nearest-neighbour fill below would give this note another entry's half. Both
      // maps thus agree on ownership.
      cCodeSynthVoice& voice = dest->synth[count];
      voice.Reset();
      voice.type = entry.type;
      dest->synthName[count] = name;
      dest->synthRootNote[count] = note;
      dest->synthTranspose[count] = entry.transpose;
      dest->synthOrder[count] = entry.order;
      dest->map[channel * 128 + note] = (unsigned char)(count + 1);
      count++;
      if (entry.body.empty() && entry.visualBody.empty())
        log += "codesynth: '" + name + "' has no sound and no visual body\n";
      continue;
    }

    // Same '{'/'}' wrap as the editor's live parse.

    std::string cleanBody = body;
    std::string wavPath, wavChain;
    bool hasWav = extractWavDirective(cleanBody, wavPath, wavChain);
    if (hasWav)
      cleanBody = trim(cleanBody);

    const char* wrapParams = CodeSynthWrapParamsForType(entry.type);
    VM* vm = nullptr;
    Parser* parser = nullptr;
    std::string cerr;
    const CodeSynthProbes* entryProbes = probes && probes->entryName == name ? probes : nullptr;
    Func* voiceFn = CompileVoiceFn(wrapParams, cleanBody, commonBody, forVisual, &dest->globals, screen, &vm, &parser, cerr,
                                   entryProbes);
    if (voiceFn && entryProbes) dest->probeSerial = entryProbes->serial;
    if (!voiceFn)
    {
      // Doesn't compile (often mid-edit): fall back to a silent stub so the note stays
      // mapped to silence rather than to a neighbour's sound.
      log += "codesynth: '" + name + "' " + cerr + " -- silent until fixed\n";
      std::string fallbackErr;
      // No prelude or globals, in case they are what's broken.
      voiceFn = CompileVoiceFn(wrapParams, "0", std::string(), forVisual, nullptr, screen, &vm, &parser, fallbackErr);
      if (!voiceFn)
      {
        log += "codesynth: '" + name + "' silent fallback also failed to compile: " + fallbackErr + "\n";
        continue;
      }
    }

    cSampleData* sample = nullptr;
    if (hasWav && !forVisual && !wavPath.empty())
    {
      // Absolute paths stand alone.
      std::string fullPath = wavIsAbsolute(wavPath)
                           ? wavPath
                           : std::string(workingDirectoryPath) + "/" + wavPath;
      sample = sample_load_wav(fullPath.c_str(), wavChain.empty() ? nullptr : wavChain.c_str());
      if (!sample)
        log += "codesynth: '" + name + "' failed to load WAV '" + wavPath + "'\n";
      // No user_data here: smp()'s cursor is bound per voice at render time.
    }

    cCodeSynthVoice& voice = dest->synth[count];
    voice.Reset();
    voice.parser = parser;
    voice.vm = vm;
    voice.fn = voiceFn;
    voice.type = entry.type;
    voice.retType = func_return_type(voiceFn);
    // Never for visuals: they draw once per frame.
    voice.stereo = !forVisual && func_param_used(voiceFn, kCodeSynthPanParamIndex) != 0;
    voice.sample = sample; // owned here; the render binds each voice's smp stream to it
    dest->synthName[count] = name;
    dest->synthRootNote[count] = note;
    dest->synthTranspose[count] = entry.transpose;
    dest->synthOrder[count] = entry.order;
    dest->map[channel * 128 + note] = (unsigned char)(count + 1);

    count++;
  }

  // An unmapped note plays the nearest mapped note's sound, pitched by the played note.
  for (int c = 0; c < 16; c++)
  {
    unsigned char* chanMap = dest->map + c * 128;
    std::vector<int> mappedNotes;
    for (int n = 0; n < 128; n++)
      if (chanMap[n] > 0) mappedNotes.push_back(n);
    if (mappedNotes.empty())
      continue;

    for (int n = 0; n < 128; n++)
    {
      if (chanMap[n] > 0) continue;
      int nearest = mappedNotes[0];
      for (int m : mappedNotes)
        if (abs(m - n) < abs(nearest - n)) nearest = m;
      chanMap[n] = chanMap[nearest];
    }
  }

  if (count == 0)
    log += "codesynth: no sounds compiled\n";

  if (debugMessages && debugMessagesMaxSize > 0)
  {
    strncpy((char*)debugMessages, log.c_str(), debugMessagesMaxSize - 1);
    ((char*)debugMessages)[debugMessagesMaxSize - 1] = '\0';
  }

  return count > 0;
}

// Does not free the parse result: see the header.
Func* CodeSynthCompileForExport(const std::string& body, const char* wrapParams,
                                  const char* prelude,
                                  const cCodeSynthGlobals* globals, RenderCtx* screen,
                                  VM** outVm, Parser** outParser, ParseResult** outRes,
                                  std::string& err, bool constPan)
{
  *outVm = nullptr;
  *outParser = nullptr;
  *outRes = nullptr;

  char* wrapped = interactive_coding_wrap_func_body(&g_sys, kCodeSynthWrapName, wrapParams, '{', '}', body.c_str());
  if (!wrapped)
  {
    err = "failed to wrap function body";
    return nullptr;
  }
  std::string src = PrependCommon(prelude ? prelude : "", wrapped);
  g_sys.free(wrapped);

  const int headerLines = CommonPreludeLines(prelude ? prelude : "") + 1;

  Parser* parser = new Parser();
  parser_init(parser, &g_sys);
  ParseResult* res = new ParseResult();
  *res = parse_to_asts(parser, src.c_str(), PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
  const char* perr = parser_get_error(res);
  if (perr)
  {
    err = std::string("parse error: ") + ParseErrorWithRow(*res, perr, headerLines);
    CodeSynthReleaseExport(nullptr, parser, res);
    return nullptr;
  }

  VM* vm = vm_create(&g_sys, parser);
  CodeSynthRegisterCFuncs(vm, screen);
  if (constPan) vm_declare_const_num(vm, "pan", 0.0, 0);
  // Before func_create, as in CompileVoiceFn.
  if (globals) globals->BindTo(vm);
  Func* main_f = func_create(vm, res->code_tree, res);
  if (!main_f)
  {
    err = std::string("compile error: ") + ShiftVmErrorRow(vm_last_error(vm), headerLines);
    CodeSynthReleaseExport(vm, parser, res);
    return nullptr;
  }

  Func* voiceFn = FindWrappedVoiceFn(vm);
  if (!voiceFn)
  {
    err = "did not produce a usable function";
    CodeSynthReleaseExport(vm, parser, res);
    return nullptr;
  }

  *outVm = vm;
  *outParser = parser;
  *outRes = res;
  return voiceFn;
}

void cCodeSynthGlobals::Reset()
{
  if (mVm) { vm_destroy(mVm); mVm = nullptr; }
  if (mParser) { parser_deinit(mParser); delete mParser; mParser = nullptr; }
  mInit = nullptr;
  block.clear();
  initBlock.clear();
  table = VMGlobalTable{};
  valid = false;
  error.clear();
}

namespace {
std::vector<std::string> TopLevelVarNames(const std::string& src, RenderCtx* screen)
{
  std::vector<std::string> names;
  if (src.find_first_not_of(" \t\r\n") == std::string::npos) return names;

  Parser* parser = new Parser();
  parser_init(parser, &g_sys);
  ParseResult res = parse_to_asts(parser, src.c_str(), 0);
  if (!parser_get_error(&res))
  {
    VM* vm = vm_create(&g_sys, parser);
    CodeSynthRegisterCFuncs(vm, screen);
    const char* found[VM_MAX_GLOBALS];
    int n = vm_collect_top_level_vars(vm, res.code_tree, &res, found, VM_MAX_GLOBALS);
    for (int i = 0; i < n; i++) names.push_back(found[i]);
    vm_destroy(vm);
  }
  free_parse_result(parser, &res);
  parser_deinit(parser); delete parser;
  return names;
}
} // namespace

bool cCodeSynthGlobals::Build(const std::string& commonSrc, const std::string& globalsSrc, RenderCtx* screen)
{
  Reset();

  if (globalsSrc.find_first_not_of(" \t\r\n") == std::string::npos)
  {
    valid = true;
    return true;
  }

  // common's top-level variables are dead there and must stay dead here.
  std::vector<std::string> commonVars = TopLevelVarNames(commonSrc, screen);
  std::vector<const char*> exclude;
  for (const std::string& s : commonVars) exclude.push_back(s.c_str());

  // Compile common ahead for its directives; fall back to the declarations alone
  // when a common helper references a variable declared only here.
  std::string combined = commonSrc.empty() ? globalsSrc : (commonSrc + "\n" + globalsSrc);
  for (int attempt = 0; attempt < 2; attempt++)
  {
    const std::string& src = (attempt == 0) ? combined : globalsSrc;
    const char* const* ex  = (attempt == 0 && !exclude.empty()) ? exclude.data() : nullptr;
    int nEx                = (attempt == 0) ? (int)exclude.size() : 0;
    const int headerLines  = (attempt == 0) ? CommonPreludeLines(commonSrc) : 0;

    mParser = new Parser();
    parser_init(mParser, &g_sys);
    ParseResult res = parse_to_asts(mParser, src.c_str(), PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
    const char* perr = parser_get_error(&res);
    if (perr)
    {
      error = std::string("globals parse error: ") + ParseErrorWithRow(res, perr, headerLines);
      free_parse_result(mParser, &res);
      parser_deinit(mParser); delete mParser; mParser = nullptr;
      if (attempt == 0) continue;
      return false;
    }

    mVm = vm_create(&g_sys, mParser);
    // Helpers may call drawing primitives; this VM only runs the initialiser, on the
    // UI thread.
    CodeSynthRegisterCFuncs(mVm, screen);

    Func* init = vm_declare_globals(mVm, res.code_tree, &res, ex, nEx, &table);
    if (!init)
    {
      error = std::string("globals compile error: ") + ShiftVmErrorRow(vm_last_error(mVm), headerLines);
      vm_destroy(mVm); mVm = nullptr;
      free_parse_result(mParser, &res);
      parser_deinit(mParser); delete mParser; mParser = nullptr;
      if (attempt == 0) continue;
      return false;
    }
    return FinishBuild(init, res);
  }
  return false;
}

bool cCodeSynthGlobals::FinishBuild(Func* init, ParseResult& res)
{
  // Exactly once: later VMs hold raw pointers into it.
  block.assign(table.size ? table.size : 1, 0);
  mInit = init;
  vm_set_globals(mVm, block.data(), block.size(), table.hash);

  ResetValues();
  initBlock = block;

  free_parse_result(mParser, &res);
  valid = true;
  return true;
}

void cCodeSynthGlobals::TakeValuesFrom(const cCodeSynthGlobals& prev)
{
  if (block.empty() || prev.block.empty()) return;
  vm_globals_migrate(&g_sys, &prev.table, prev.block.data(), prev.initBlock.data(),
                     &table, block.data(), initBlock.data());
}

void cCodeSynthGlobals::RestoreInitValues()
{
  if (!block.empty() && initBlock.size() == block.size())
    std::copy(initBlock.begin(), initBlock.end(), block.begin());
}

void cCodeSynthGlobals::ResetValues()
{
  if (block.empty()) return;
  std::fill(block.begin(), block.end(), (unsigned char)0);
  if (!mInit || !mVm) return;

  std::vector<unsigned char> frame(func_frame_size(mInit) + 64, 0);
  Args a;
  args_bind(&a, mInit, frame.data(), frame.size());
  if (func_run(mInit, &a, 2000000) != VM_OK)
  {
    const char* verr = vm_last_error(mVm);
    error = std::string("globals initialiser failed: ") + (verr ? verr : "?");
    // Bodies still run; only initial values are missing.
  }
}

void cCodeSynthGlobals::BindTo(VM* vm) const
{
  if (!vm || table.count == 0) return;
  vm_import_globals(vm, &table);
  vm_set_globals(vm, const_cast<unsigned char*>(block.data()), block.size(), table.hash);
}

std::string cCodeSynthGlobals::EmitC(const char* typeName, const char* varName) const
{
  if (!mVm || table.count == 0) return std::string();
  char* c = vm_emit_c_globals(mVm, typeName, varName);
  if (!c) return std::string();
  std::string out(c);
  mVm->run.sys->free(c);
  return out;
}

Func* CodeSynthCompileProbed(const std::string& body, CodeSynthType type, const std::string& prelude,
                             bool forVisual, const cCodeSynthGlobals* globals, RenderCtx* screen,
                             const CodeSynthProbes& probes, VM** outVm, Parser** outParser, std::string& err)
{
  return CompileVoiceFn(CodeSynthWrapParamsForType(type), body, prelude, forVisual, globals, screen,
                        outVm, outParser, err, &probes);
}

void CodeSynthReleaseExport(VM* vm, Parser* parser, ParseResult* res)
{
  if (vm) vm_destroy(vm);
  if (parser && res) free_parse_result(parser, res);
  if (parser) { parser_deinit(parser); delete parser; }
  delete res;
}
