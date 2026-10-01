// Windows only. Some hosts run their own accelerator table against every message
// on the thread, ahead of our IGraphics view, so Ctrl+C/X/V never reach a
// focused textarea. A thread-scoped WH_KEYBOARD hook runs inside
// GetMessage/PeekMessage, before the host's TranslateAccelerator, and handles
// the shortcuts itself while a textarea is focused. A native Edit ghost window
// also takes OS focus, which is enough for hosts that exempt Edit controls.

#include "IGraphics.h"

#ifdef OS_WIN

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace iplug;
using namespace igraphics;

static IGraphics* g_graphics = nullptr;
static HWND       g_ghostWnd = nullptr;
static HWND       g_mainWnd  = nullptr;
static HHOOK      g_keyboardHook = nullptr;

// ---- ghost Edit-control focus ----

static LRESULT CALLBACK GhostEditProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_KEYDOWN:
    case WM_KEYUP: {
      if (!g_graphics) return 0;

      BYTE keyboardState[256] = {};
      GetKeyboardState(keyboardState);
      const int keyboardScanCode = (lParam >> 16) & 0x00ff;
      WORD character = 0;
      const int len = ToAscii((UINT)wParam, keyboardScanCode, keyboardState, &character, 0);

      if (len == 0 || len == 1) {
        char str[2] = { (char)character, '\0' };
        IKeyPress keyPress{ str, (int)wParam,
                            (bool)(GetKeyState(VK_SHIFT)   & 0x8000),
                            (bool)(GetKeyState(VK_CONTROL) & 0x8000),
                            (bool)(GetKeyState(VK_MENU)    & 0x8000) };

        if (msg == WM_KEYDOWN)
          g_graphics->OnKeyDown(0.f, 0.f, keyPress);
        else
          g_graphics->OnKeyUp(0.f, 0.f, keyPress);
      }
      return 0;
    }
    case WM_CHAR:
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTALLKEYS;
    default:
      return DefWindowProcW(hWnd, msg, wParam, lParam);
  }
}

static void DestroyGhostWindow() {
  if (IsWindow(g_ghostWnd)) DestroyWindow(g_ghostWnd);
  g_ghostWnd = nullptr;
  g_mainWnd  = nullptr;
}

static void EnsureGhostWindow() {
  if (!g_graphics) return;

  HWND parent = (HWND)g_graphics->GetWindow();
  if (!parent) return;

  if (g_mainWnd != parent || !IsWindow(g_ghostWnd)) {
    if (IsWindow(g_ghostWnd)) DestroyWindow(g_ghostWnd);
    g_ghostWnd = nullptr;
    g_mainWnd  = parent;
  }
  if (g_ghostWnd) return;

  g_ghostWnd = CreateWindowExW(0, L"Edit", L"", WS_CHILD,
                                0, 0, 0, 0, parent, nullptr,
                                (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
  if (g_ghostWnd)
    SetWindowLongPtrW(g_ghostWnd, GWLP_WNDPROC, (LONG_PTR)GhostEditProc);
}

// ---- WH_KEYBOARD hook ----

// The shortcuts hosts bind for themselves; keep in sync with handle_key() in
// textmode_ui_textarea.c.
static bool IsInterceptedShortcut(WPARAM vk, bool ctrlDown) {
  return ctrlDown && (vk == 'C' || vk == 'X' || vk == 'V' || vk == 'Z' ||
                      vk == 'Y' || vk == 'A' || vk == VK_LEFT ||
                      vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN);
}

// Checked per key: our own focus_id can't see the user clicking into the host,
// so the hook could otherwise outlive the focus it was installed for.
static bool PluginWindowHasOsFocus() {
  // Re-resolved every call: the view changes when the editor is reopened.
  if (g_graphics) g_mainWnd = (HWND)g_graphics->GetWindow();
  if (!IsWindow(g_mainWnd)) return false;

  // NULL when another thread's window has focus, i.e. the user left the host.
  HWND focused = GetFocus();
  if (!focused) return false;
  if (focused == g_mainWnd || focused == g_ghostWnd) return true;
  return g_mainWnd && IsChild(g_mainWnd, focused);
}

extern "C" int PlatformKeyFocus_WindowHasOsFocus(void) {
  return PluginWindowHasOsFocus() ? 1 : 0;
}

static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION && g_graphics) {
    const bool isKeyUp   = (lParam & (1 << 31)) != 0;
    const bool ctrlDown  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

    if (IsInterceptedShortcut(wParam, ctrlDown) && PluginWindowHasOsFocus()) {
      char str[2] = { (char)wParam, '\0' };
      IKeyPress keyPress{ str, (int)wParam,
                          (bool)(GetKeyState(VK_SHIFT) & 0x8000),
                          true,
                          (bool)(GetKeyState(VK_MENU)  & 0x8000) };

      // Handle copy/cut/paste ourselves, then swallow the key.
      if (isKeyUp)
        g_graphics->OnKeyUp(0.f, 0.f, keyPress);
      else
        g_graphics->OnKeyDown(0.f, 0.f, keyPress);

      return 1;
    }
  }
  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static void InstallKeyboardHook() {
  if (g_keyboardHook) return;
  // Thread-scoped: the host's UI thread pumps messages for our child HWND.
  g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD, KeyboardHookProc, nullptr, GetCurrentThreadId());
}

static void RemoveKeyboardHook() {
  if (!g_keyboardHook) return;
  UnhookWindowsHookEx(g_keyboardHook);
  g_keyboardHook = nullptr;
}

extern "C" void PlatformKeyFocus_SetGraphics(void* graphics) {
  // Null means the control is going away: tear down the window and the hook.
  if (!graphics) {
    RemoveKeyboardHook();
    DestroyGhostWindow();
  }
  g_graphics = (IGraphics*)graphics;
}

extern "C" void PlatformKeyFocus_SetEditorFocused(int focused) {
  if (focused) {
    EnsureGhostWindow();
    if (g_ghostWnd) SetFocus(g_ghostWnd);
    InstallKeyboardHook();
  } else {
    RemoveKeyboardHook();
    // Hand focus back only if we hold it, or we'd fight the host for it.
    if (g_mainWnd && g_ghostWnd && GetFocus() == g_ghostWnd) SetFocus(g_mainWnd);
  }
}

#else // !OS_WIN

extern "C" void PlatformKeyFocus_SetGraphics(void* /* graphics */) {}
extern "C" void PlatformKeyFocus_SetEditorFocused(int /* focused */) {}
extern "C" int  PlatformKeyFocus_WindowHasOsFocus(void) { return 1; }

#endif
