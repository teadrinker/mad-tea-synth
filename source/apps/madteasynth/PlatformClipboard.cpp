// platform.h's clipboard functions for textmode_ui_textarea.c, routed through
// iPlug2's IGraphics, which SteepSynthControl sets every frame.

#include "IGraphics.h"
#include "wdlstring.h"
#include <cstdlib>
#include <cstring>

using namespace iplug;
using namespace igraphics;

static IGraphics* g_graphics = nullptr;

void PlatformClipboard_SetGraphics(IGraphics* graphics) {
  g_graphics = graphics;
}

extern "C" void platform_copy_to_clipboard(const char* text) {
  if (!text || !g_graphics) return;
  g_graphics->SetTextInClipboard(text);
}

extern "C" char* platform_paste_from_clipboard(void) {
  if (!g_graphics) return nullptr;
  WDL_String str;
  if (!g_graphics->GetTextFromClipboard(str) || str.GetLength() == 0) return nullptr;
  // The textarea frees this with plain free().
  size_t len = (size_t)str.GetLength();
  char* result = (char*)malloc(len + 1);
  if (result) memcpy(result, str.Get(), len + 1);
  return result;
}
