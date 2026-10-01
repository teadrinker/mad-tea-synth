#ifndef PLATFORM_H
#define PLATFORM_H


#define FLAGS_DOUBLE_CLICK                (1 << 0) // NOTE: these are aligned with the textmode_ui.h flags and mouse event types, keep them in sync
#define FLAGS_CTRL                        (1 << 1)
#define FLAGS_SHIFT                       (1 << 2)
#define FLAGS_ALT                         (1 << 3)

#define FLAGS_NEED_FULL_REDRAW            (1 << 16) // if this is not set on each_frame flags arg, the color buffer data is the same as last frame 
#define FLAGS_SWAP_RED_BLUE_CHANNELS      (1 << 17)
#define FLAGS_WINDOW_HAS_FOCUS            (1 << 18) // set on each_frame flags when this window has keyboard focus
#define FLAGS_SECONDARY_WINDOW            (1 << 24) // only SDL3 backend supports this

#define EACH_FRAME_RETURN_EXIT            (1 << 3) 
#define EACH_FRAME_RETURN_ARGB_UNCHANGED  (1 << 4) 
#define EACH_FRAME_RETURN_CLOSE_WINDOW    (1 << 5) 

// app implements
int  each_frame(float time_in_seconds, int width, int height, int *argb, float dpi, int flags);
void on_mouse_event(float x, float y, float wheel, int type, int which_button, int flags);
void on_key_event(int keycode, int is_down, int flags);
void on_char_event(int ch, int flags);

void platform_init(void);
void platform_shutdown(void);

// app can call  
// Seconds on a monotonic high-resolution clock, from an arbitrary origin: for
// measuring intervals (profiling), not for wall-clock time.
double platform_time_seconds(void);
void platform_request_size(int w, int h, int flags);
void platform_copy_to_clipboard(const char *text);
char *platform_paste_from_clipboard(void);
// Startup argument: --name=value / --name value on the command line, ?name=value
// in the page URL on wasm. Returns 1 if present (dst may be "" for a bare flag),
// 0 if absent. dst is always null-terminated, truncated to dst_len-1 chars.
int  platform_get_arg(const char *name, int dst_len, char *dst);

// SDL3 backend entry point (for apps defining PLATFORM_NO_MAIN)
void platform_sdl3_run(void);
void platform_win32_run(void);
void platform_cli_run(void);


// ---------------------------------------------------------------------------
// Platform-neutral keycodes (derived from SDL3 SDLK_* values).
// Each platform backend converts its native format to these.
// Printable characters use their lowercase ASCII where applicable.
// ---------------------------------------------------------------------------

// Navigation & editing keys
#define PLATFORM_KEY_LEFT       0x40000050u  // SDLK_LEFT
#define PLATFORM_KEY_RIGHT      0x4000004Fu  // SDLK_RIGHT
#define PLATFORM_KEY_UP         0x40000052u  // SDLK_UP
#define PLATFORM_KEY_DOWN       0x40000051u  // SDLK_DOWN
#define PLATFORM_KEY_HOME       0x4000004Au  // SDLK_HOME
#define PLATFORM_KEY_END        0x4000004Du  // SDLK_END
#define PLATFORM_KEY_PAGEUP     0x4000004Bu  // SDLK_PAGEUP
#define PLATFORM_KEY_PAGEDOWN   0x4000004Eu  // SDLK_PAGEDOWN
#define PLATFORM_KEY_DELETE     0x0000007Fu  // SDLK_DELETE
#define PLATFORM_KEY_BACKSPACE  0x00000008u  // SDLK_BACKSPACE
#define PLATFORM_KEY_ENTER      0x0000000Du  // SDLK_RETURN
#define PLATFORM_KEY_TAB        0x00000009u  // SDLK_TAB
#define PLATFORM_KEY_ESCAPE     0x0000001Bu  // SDLK_ESCAPE
#define PLATFORM_KEY_INSERT     0x40000049u  // SDLK_INSERT

// Alpha keys – lowercase ASCII (SDL3 convention)
#define PLATFORM_KEY_A          0x00000061u  // 'a'
#define PLATFORM_KEY_B          0x00000062u  // 'b'
#define PLATFORM_KEY_C          0x00000063u  // 'c'
#define PLATFORM_KEY_D          0x00000064u  // 'd'
#define PLATFORM_KEY_E          0x00000065u  // 'e'
#define PLATFORM_KEY_F          0x00000066u  // 'f'
#define PLATFORM_KEY_G          0x00000067u  // 'g'
#define PLATFORM_KEY_H          0x00000068u  // 'h'
#define PLATFORM_KEY_I          0x00000069u  // 'i'
#define PLATFORM_KEY_J          0x0000006Au  // 'j'
#define PLATFORM_KEY_K          0x0000006Bu  // 'k'
#define PLATFORM_KEY_L          0x0000006Cu  // 'l'
#define PLATFORM_KEY_M          0x0000006Du  // 'm'
#define PLATFORM_KEY_N          0x0000006Eu  // 'n'
#define PLATFORM_KEY_O          0x0000006Fu  // 'o'
#define PLATFORM_KEY_P          0x00000070u  // 'p'
#define PLATFORM_KEY_Q          0x00000071u  // 'q'
#define PLATFORM_KEY_R          0x00000072u  // 'r'
#define PLATFORM_KEY_S          0x00000073u  // 's'
#define PLATFORM_KEY_T          0x00000074u  // 't'
#define PLATFORM_KEY_U          0x00000075u  // 'u'
#define PLATFORM_KEY_V          0x00000076u  // 'v'
#define PLATFORM_KEY_W          0x00000077u  // 'w'
#define PLATFORM_KEY_X          0x00000078u  // 'x'
#define PLATFORM_KEY_Y          0x00000079u  // 'y'
#define PLATFORM_KEY_Z          0x0000007Au  // 'z'
#define PLATFORM_KEY_F1         0x4000003Au  // SDLK_F1
#define PLATFORM_KEY_F2         0x4000003Bu  // SDLK_F2
#define PLATFORM_KEY_F3         0x4000003Cu  // SDLK_F3
#define PLATFORM_KEY_F4         0x4000003Du  // SDLK_F4
#define PLATFORM_KEY_F5         0x4000003Eu  // SDLK_F5
#define PLATFORM_KEY_F6         0x4000003Fu  // SDLK_F6
#define PLATFORM_KEY_F7         0x40000040u  // SDLK_F7
#define PLATFORM_KEY_F8         0x40000041u  // SDLK_F8
#define PLATFORM_KEY_F9         0x40000042u  // SDLK_F9
#define PLATFORM_KEY_F10        0x40000043u  // SDLK_F10
#define PLATFORM_KEY_F11        0x40000044u  // SDLK_F11
#define PLATFORM_KEY_F12        0x40000045u  // SDLK_F12

// ---------------------------------------------------------------------------
// Mouse event types
#define MOUSE_TYPE_MOVE        0
#define MOUSE_TYPE_DOWN        1
#define MOUSE_TYPE_UP          2
#define MOUSE_TYPE_DRAG        3
#define MOUSE_TYPE_WHEEL       4

// Mouse buttons
#define MOUSE_BUTTON_NONE     -1
#define MOUSE_BUTTON_LEFT      0
#define MOUSE_BUTTON_MIDDLE    1
#define MOUSE_BUTTON_RIGHT     2


#endif 
