#ifndef TEXTMODE_UI_H
#define TEXTMODE_UI_H

#include "common/array.h"
#include "common/tsys.h"
#include "textmode_cell.h"

#ifndef bool
    #define bool int
#endif
#ifndef true
    #define true 1
    #define false 0
#endif

#define UI_C_FLAGS_EDITING_DISABLED (1 << 0)
#define UI_C_FLAGS_BEING_DRAGGED    (1 << 1)
#define UI_C_FLAGS_DISABLED         (1 << 2)

// ui_dropdown_begin flags
#define UI_DROPDOWN_HIDE_NAME      (1 << 0)
#define UI_DROPDOWN_HIDE_SELECTION (1 << 1)
#define UI_DROPDOWN_HIDE_TRIANGLE  (1 << 2)
#define UI_DROPDOWN_HIDE_SEPARATOR (1 << 3)

#define UI_DRAGGABLE_VALUE_READOUT_LENGTH 15
#define UI_DRAGGABLE_HIDE_NAME          (1 << 4)
#define UI_DRAGGABLE_HIDE_SLIDER        (1 << 5)


// ===========================================================================
// FIRST-DRAW-WINS CONVENTION
// ===========================================================================
// The output canvas uses a first-draw-wins rule: once a cell has been drawn
// (CELL_FLAGS_DRAWN is set), subsequent draws to that same cell are silently
// ignored unless force_write=true is passed to ui_draw_cell_raw.
//
// CORRECT pattern (draw foreground FIRST, fill background gaps LAST):
//
//   ui_draw_text(ui, x, y, "hello", fg_color);  // foreground first
//   for (int cx = 0; cx < width; cx++)            // background last
//       ui_draw_cell(ui, (float)cx, (float)y, ' ', bg_color);
//
// WRONG (background first hides the text):
//
//   for (int cx = 0; cx < width; cx++)            // background first
//       ui_draw_cell(ui, (float)cx, (float)y, ' ', bg_color);
//   ui_draw_text(ui, x, y, "hello", fg_color);  // text ignored!
//

// Component types for dynamic controls
enum {
    UI_COMPONENT_BUTTON = 0,
    UI_COMPONENT_CHECKBOX,
    UI_COMPONENT_SLIDER,
    UI_COMPONENT_RADIO,
    UI_COMPONENT_LABEL,
    UI_COMPONENT_DROPDOWN,
    UI_COMPONENT_SELECTABLE,
    UI_COMPONENT_MENU_ITEM,
    UI_COMPONENT_MENU_TOGGLE,
    UI_COMPONENT_POPUP_BG
};

// Theme colour slots — each stores packed fg|(gradient<<8)|(bg<<16)
enum {
    UI_COL_DEFAULT = 0,
    UI_COL_BUTTON,
    UI_COL_BUTTON_HOVER,
    UI_COL_BUTTON_ACT,
    UI_COL_DRAGGABLE,
    UI_COL_DRAGGABLE_HOVER,
    UI_COL_DIMMED,
    UI_COL_DIMMED_BG,
    UI_COL_COLD_ACCENT,
    UI_COL_WARM_ACCENT,
    UI_COL_GREEN_ACCENT,
    UI_COL_DIMMED_TEXT,
    UI_COL_BUTTON_ON,
    UI_COL_BUTTON_ON_HOVER,
    UI_COL_LABEL,
    UI_NUM_COLORS
};

// flags for ui_option_bar / the _flags widget variants
#define UI_OPTION_VERTICAL   (1 << 0) // stack downward instead of across
#define UI_OPTION_NO_GAP     (1 << 1) // items abut; default leaves a 1-cell gap
#define UI_OPTION_ALIGN_LEFT (1 << 2) // left-align the label (default: centred, like ui_button)
#define UI_OPTION_FILL_WIDTH (1 << 3) // divide ui_avail_width() evenly instead of max(len)+2
#define UI_OPTION_DIM_WHEN_OFF (1 << 4)
#define UI_OPTION_NO_MARGIN  (1 << 5) // no padding cell either side: the label fills the button

// ui_popup_begin flags
#define UI_POPUP_BORDER       (1 << 0)
#define UI_POPUP_CAPTURE_KEYS (1 << 1) // keys go to the body only while the mouse is over it
#define UI_POPUP_STAY_OPEN    (1 << 2) // a press outside does not close it
#define UI_POPUP_PLAIN_BG     (1 << 3) // default background, not lifted one step
#define UI_POPUP_ALIGN_LEFT   (1 << 4) // anchored: left edge on the anchor's, not centred under it

typedef struct UIContext UIContext;
// OutputCell is typedef'd to TMCell in textmode_cell.h

// Input parked away from code that must not see it (a disabled scope, or
// everything outside an open popup's body), restored when it may again.
typedef struct UIInputSnapshot {
    float mouse_x, mouse_y, mouse_dx, mouse_dy;
    float wheel;
    int   mouse_down[3], mouse_pressed[3], mouse_released[3];
    int   num_key_events, num_char_events;
} UIInputSnapshot;

// The open ui_popup_begin popup. Rects are screen cells.
typedef struct UIPopup {
    unsigned int id;                   // 0 = none open
    int   x, y, w, h;                  // placed rect, written by ui_popup_end
    int   rect_valid;
    float anchor_x, anchor_y, anchor_w, anchor_h;
    int   anchored;
    float spawn_x, spawn_y;
    int   flags;
    int   opened_this_frame;
    int   begun_this_frame;
    int   owns_capture;                // active_id was taken inside the body
    int   layer_used;                  // the layer holds this frame's body
    int   src_x, src_y;                // where the body was drawn this frame
    float req_w, req_h;
    float inner_x, inner_y;
    float extent_x, extent_y;
    int   active_before;
    int   ctrl_edited;                 // ui_multi_dropdown: changed while Ctrl was held
    int   checkbox_view;               // ui_multi_dropdown: opened on a multi-selection
} UIPopup;

// Caller state ui_popup_begin sets aside for the body.
typedef struct UIPopupSaved {
    unsigned int body_id;
    OutputCell  *screen;
    float pen_x, pen_y, line_start_x, content_max_x, line_height;
    bool  same_line;
    float last_x, last_y, last_w, last_h;
    int   item_width_sp;
    int   label_width_sp;
    int   clip_x0, clip_y0, clip_x1, clip_y1, clip_sp;
    int   id_stack_sp;
    int   blind;                    // opening frame: the body is not where it will land
    UIInputSnapshot blind_parked;
} UIPopupSaved;

typedef struct UIControl {
    float x, y, w, h;
    char  text[256];
    int          component_type;
    int          flags;
    unsigned int widget_id;    // FNV-1a hash; stable cross-frame identity
    union {
        int   i;
        float f;
    } value;
    union {
        struct { float min, middle, max; } slider;
        // value is the shared group state (one int for the whole group, as in
        // ui_radio_button); NULL falls back to this control's own value.i
        struct { int which; int *value; } radio_button;
        // items[] is read every frame, not copied -- it must outlive the control
        struct { int *value; const char **items; int count; int flags; } dropdown;
        struct { int id; void *user_ptr; } user_data;
    } data;

    void (*action_callback)(UIContext *ui, int id, void *data);
    int   action_id;
    void *action_data;
} UIControl;

struct UIContext {
    Tsys       *sys;
    int         cols;
    int         rows;
    OutputCell *screen;   // cols*rows render target, copied to TextMode by caller

    bool        edit_mode_enabled;
    float       global_scale;
    float       global_weight;
    float       global_line_height;

    // theme colours, indexed by UI_COL_* — packed fg|(gradient<<8)|(bg<<16).
    // Authored values; read them through ui_theme_color(), never directly, so
    // the two knobs below get their say.
    unsigned int theme[UI_NUM_COLORS];

    // Per-slot background lift, indexed the same way: the share of the slot's
    // own foreground mixed into its background by ui_theme_color, after the
    // grading below. 0 leaves the authored background alone; UI_COL_BUTTON
    // carries 0.1 so a button body is 90% panel background and 10% of the text
    // it holds, which keeps it on the background's ramp instead of being a
    // separately authored colour that has to be re-picked for every theme.
    float theme_blend[UI_NUM_COLORS];

    // ---- on-the-fly theme grading (see ui_theme_adjust) ----
    float theme_black_point;    // darkest value the theme may produce, 0..1
    float theme_text_intensity; // where UI_COL_DEFAULT's text lands, 0..1

    // dynamic components
    Array components; // UIControl

    // ---- immediate-mode layout ----
    float pen_x, pen_y;       // current insertion point (cell coords)
    float line_start_x;
    float content_max_x;      // right edge of the current content region (cells)
    float line_height;        // tallest widget on the current line
    bool  same_line;
    float last_x, last_y;     // last placed widget origin
    float last_w, last_h;     // last placed widget size
    float item_width_stack[16];
    int   item_width_sp;
    int   label_width_stack[16];
    int   label_width_sp;

    // ---- disabled controls ----
    int   disabled_depth; // disabled_depth > 0 means "everything drawn right now is dimmed
    bool  disable_next;
    UIInputSnapshot disabled_parked; // held while the outermost disabled scope is open

    // ---- immediate-mode interaction ----
    int   hot_id;             // hovered widget (0 = none)
    int   active_id;          // mouse-captured widget (0 = none)
    int   active_button;      // UI_MOUSE_BUTTON_* holding the capture
    void *active_state;       // per-control state (owned, freed on clear)
    int   focus_id;           // keyboard focus (0 = none, UI_FOCUS_RELEASED = none, and no textarea takes it by default)

    // per-widget interaction query state (set by widget, read by user after call)
    int   last_widget_activated;   // last widget was activated (pressed) this frame
    int   last_widget_active;      // last widget is currently held down
    int   last_widget_deactivated; // last widget was deactivated (released) this frame

    // ---- mouse ----
    float cell_w, cell_h;     // pixel size of a cell (for px->cell conversion)
    float mouse_x, mouse_y;   // cell coords
    float prev_mouse_x, prev_mouse_y;
    float mouse_dx, mouse_dy; // cell-space delta this frame
    int   mouse_down[3];
    int   mouse_pressed[3];
    int   mouse_released[3];
    float wheel;
    int   mouse_flags;
    int   prev_mouse_flags;

    // ---- window focus ----
    int   window_has_focus;

    // Set during a frame by a widget that needs the Escape key to reach the UI (an open
    // find dialog), cleared by ui_begin. A host that lets the OS or a plugin host act on
    // Escape reads it after ui_end and routes Escape to the UI while it is set.
    int   wants_escape;

    // slider drag anchor (relative-mode dragging)
    float slider_anchor_t;
    float slider_anchor_mouse_x;
    float slider_anchor_mouse_y;

    // scrollbar drag anchor (relative-mode dragging)
    float scrollbar_anchor_value;
    float scrollbar_anchor_mouse;

    // ---- buffered keyboard events (consumed by focused widgets) ----
    int   key_events[64];
    int   key_flags[64];
    int   num_key_events;
    int   char_events[64];
    int   char_flags[64];
    int   num_char_events;

    // time, for cursor blink etc.
    float time;

    // ---- ID hash stack ----
    unsigned int  id_stack[8];
    int           id_stack_sp;

    // ---- clip rect (cell coords, half-open [x0,x1) x [y0,y1)) ----
    int   clip_x0, clip_y0, clip_x1, clip_y1;
    int   clip_stack[8][4];
    int   clip_sp;

    // ---- popup / overlay ----
    unsigned int  popup_open_id;          // which popup is open (0 = none)
    unsigned int  overlay_activated_id;   // overlay item that fired this frame
    int           overlay_consumed_input; // true if mouse is over any overlay item
    Array         overlay_components;     // drawn first in ui_begin(); owns cells
    float         popup_spawn_x;          // mouse position when ui_open_popup fired
    float         popup_spawn_y;

    // ---- popup-building cursor (filled by dropdown_begin/selectable/menu_*) ----
    int           in_popup;
    float         popup_pen_x, popup_pen_y, popup_w;
    int           popup_bg_component_idx;

    // ---- ui_popup_begin popup ----
    // The body draws into popup_layer, which ui_end copies over the screen, so
    // it lands on top whatever was drawn before or after it. While the mouse is
    // over it, input is parked everywhere except inside the body.
    UIPopup         popup;
    OutputCell     *popup_layer;
    UIPopupSaved    popup_saved;
    int             in_popup_body;
    int             popup_press_outside;
    int             popup_input_parked;
    UIInputSnapshot popup_parked;
    int             theme_bg_lift;   // added to every slot's background inside a popup body
};

// ---- lifecycle ----
UIContext  *ui_create(Tsys *sys, int cols, int rows);
void        ui_resize(UIContext *ui, int cols, int rows);
void        ui_destroy(UIContext *ui);
OutputCell *ui_access(UIContext *ui); // this is the visual output buffer
void        ui_set_cell_size(UIContext *ui, float cell_w, float cell_h); // needed to scale mouse input

// ---- per-frame ----
void ui_begin(UIContext *ui, float time_in_seconds);
int  ui_update_dynamic(UIContext *ui); // run dynamic components
void ui_end(UIContext *ui);


void ui_disable_next(UIContext *ui);
void ui_begin_disabled(UIContext *ui);
void ui_end_disabled(UIContext *ui);
int  ui_is_disabled(UIContext *ui);   // true while inside a disabled scope

// ---- immediate-mode layout ----
/*

 Call ui_set_cursor() / ui_set_cursor_column() once per UI grouping, then to layout:

   * Downward: ui_blank_row() and ui_indent()
   * Rows: ui_same_line(), ui_same_line_pad() and ui_same_line_col().

*/

void ui_set_cursor(UIContext *ui, float x, float y);
void ui_set_cursor_column(UIContext *ui, float x, float y, float width);

void ui_blank_row(UIContext *ui);           
void ui_indent(UIContext *ui, int count);         // indent new row with space

void ui_same_line(UIContext *ui);
void ui_same_line_pad(UIContext *ui, int count);  // pad current row with space
void ui_same_line_col(UIContext *ui, int col);    // Same row, at a fixed column measured from the grouping's left edge

void ui_push_item_width(UIContext *ui, float w);
void ui_pop_item_width(UIContext *ui);
void ui_push_label_width(UIContext *ui, int chars);
void ui_pop_label_width(UIContext *ui);

float ui_avail_width(UIContext *ui); // space from pen to content_max_x

// ===== theme access =====
// The theme table is authored against these two settings, so at these values
// ui_theme_adjust is the identity for whatever table it is handed -- an app
// that overrides slots keeps exactly the colours it wrote until a knob moves.
#define UI_THEME_BLACK_POINT_DEFAULT    (25.0f  / 255.0f)
#define UI_THEME_TEXT_INTENSITY_DEFAULT (160.0f / 255.0f)

// Grade one packed colour by ui->theme_black_point / ui->theme_text_intensity:
// the authored black point slides to theme_black_point and everything above it
// is scaled so UI_COL_DEFAULT's text lands on theme_text_intensity. Use it for
// literal colours that have to sit alongside graded theme slots; slots
// themselves go through ui_theme_color.
unsigned int ui_theme_adjust(const UIContext *ui, unsigned int packed);

// The graded value of theme slot `slot` (UI_COL_*), with theme_blend[slot] of
// its own foreground mixed into its background afterwards. Every read of the
// theme goes through here -- reading ui->theme[] directly bypasses both steps
// and paints a cell that no longer matches its neighbours.
unsigned int ui_theme_color(const UIContext *ui, int slot);

// Step a colour's text and background along their own ramps, keeping the
// gradient (the hue) it already has. Offsets are steps on the authored palette
// (~12 per step) and are scaled by the theme's own gain before being applied,
// so an offset means the same thing at every grading; the result is clamped, so
// callers never have to reason about the ends of a ramp.
//
// This plus ui_theme_color is how a colour that is not a theme slot in its own
// right -- a syntax colour, a caret, a selection -- gets expressed: pick the
// nearest slot, then say how far off it sits. Never write a literal
// 0xAARRGGBB; it cannot follow the theme or its grading.
unsigned int ui_adjust_color_additive(const UIContext *ui, unsigned int c,
                                      int fg_offset, int bg_offset);

void ui_push_clip_rect(UIContext *ui, float x, float y, float w, float h);
void ui_pop_clip_rect(UIContext *ui);
void ui_dummy(UIContext *ui, float w, float h);



int  ui_item_disable_begin(UIContext *ui);
void ui_item_disable_end(UIContext *ui, int disabled);
float ui_take_width(UIContext *ui, float autow);
int   ui_hit(UIContext *ui, float x, float y, float w, float h);
int   ui_hit_raw(UIContext *ui, float x, float y, float w, float h);
void  ui_advance(UIContext *ui, float x, float y, float w, float h);

// ---- immediate-mode widgets ----
// All of these take a label that doubles as the widget id, and all honour the
// "##suffix" convention (see ui_display_len below): "Run##snd" draws "Run" but
// hashes differently from "Run##vis".
int ui_label(UIContext *ui, const char *text);
int ui_button(UIContext *ui, const char *text);
int ui_button_flags(UIContext *ui, const char *text, int flags);
int ui_radio_button(UIContext *ui, const char *text, int *value, int which);
int ui_check_box(UIContext *ui, const char *text, bool *value);

int ui_toggle_button(UIContext *ui, const char *text, bool on);
int ui_toggle_button_flags(UIContext *ui, const char *text, bool on, int flags);

int ui_option_button(UIContext *ui, const char *text, int *value, int which);
int ui_option_button_flags(UIContext *ui, const char *text, int *value, int which, int flags);

int ui_option_bar(UIContext *ui, const char *id, int *value,
                  int count, const char *const items[], int flags);
int ui_draggable_value(UIContext *ui, const char *text, float *value, float min, float middle, float max, int flags);
//int ui_scrubber(UIContext *ui, const char *text, float *value, float default_value, int num_chars);
int ui_slider(UIContext *ui, const char *text, float *value,
              float min, float middle, float max);
int ui_vscroll(UIContext *ui, float *value, int id, float handle_size, int x, int y, int h);
int ui_hscroll(UIContext *ui, float *value, int id, float handle_size, int x, int y, int w);
// A draggable 1-cell divider splitting the columns [x, x+w) into two panes over
// rows [y, y+h). *split is the left pane's share of w (clamped so each pane
// keeps min_w cells) and doubles as the id. Returns the left width: the divider
// sits at column x+left, the right pane starts at x+left+1, w-left-1 wide.
// Call it before drawing the panes so a press on the divider is captured first.
int ui_splitter(UIContext *ui, float *split, int x, int y, int w, int h, int min_w);
void ui_push_id(UIContext *ui, const char *str);
void ui_push_id_ptr(UIContext *ui, const void *ptr);
void ui_pop_id(UIContext *ui);
void ui_open_popup(UIContext *ui, const char *str_id);
int  ui_context_popup_begin(UIContext *ui, const char *str_id);
void ui_context_popup_end(UIContext *ui);
int  ui_dropdown_begin(UIContext *ui, const char *text, const char *cur_value, int flags);
void ui_dropdown_end(UIContext *ui);
bool ui_dropdown(UIContext *ui, const char *text, int *value, int num_items, const char *items[], int flags);
bool ui_selectable(UIContext *ui, const char *text, bool is_selected);
bool ui_menu_item(UIContext *ui, const char *text);
bool ui_menu_toggle(UIContext *ui, const char *text, bool *checked);
bool ui_hovering_enabled(UIContext *ui);

// ---- popups holding any widgets ----
// One open at a time; opening one closes the previous one and any legacy
// dropdown / context popup. Between begin and end the body is an ordinary
// layout grouping: lay widgets out from the pen as usual. After end the
// caller's layout continues as if the popup was not there.
// w/h: > 0 fixed, <= 0 measured from the body (measured popups need widgets
// that do not stretch to the content region).
void ui_popup_open(UIContext *ui, const char *str_id);                  // at the mouse
void ui_popup_open_at(UIContext *ui, const char *str_id, float x, float y);
void ui_popup_open_below_last(UIContext *ui, const char *str_id);       // under the last widget
void ui_popup_close(UIContext *ui);
int  ui_popup_is_open(UIContext *ui, const char *str_id);
int  ui_popup_any_open(UIContext *ui);
int  ui_popup_begin(UIContext *ui, const char *str_id, float w, float h, int flags);
void ui_popup_end(UIContext *ui);

// A dropdown-style header ("Show: C, Lua v") whose popup holds any widgets.
// header_flags are the UI_DROPDOWN_* ones, popup_flags the UI_POPUP_* ones.
int  ui_dropdown_panel_begin(UIContext *ui, const char *text, const char *cur_value,
                             int header_flags, float w, float h, int popup_flags);
void ui_dropdown_panel_end(UIContext *ui);

// "Show: C, Lua v" over count on/off values. A click picks one item (or
// none_label, which clears all) and closes, like a plain dropdown; while Ctrl
// is held the rows are checkboxes and none_label a button, and it stays open
// until Ctrl is released (only if something was changed while it was held).
// Opened on more than one selected item, it shows the checkbox view until
// none_label is clicked without Ctrl.
// Returns a bitmask of the items that changed (count <= 31).
int  ui_multi_dropdown(UIContext *ui, const char *text, int count, const char *const items[],
                       bool *values, const char *none_label);
// With every item on (count > 1) the header says "All" instead of listing them.
// UI_MULTI_CHECKBOXES: always the checkbox view, no Ctrl needed, stays open,
// and a NULL none_label means no clear-all button (the header still says "None").
// disabled (optional, count entries): greyed-out items that cannot change.
#define UI_MULTI_CHECKBOXES (1 << 0)
int  ui_multi_dropdown_ex(UIContext *ui, const char *text, int count, const char *const items[],
                          bool *values, const char *none_label, int flags, const bool *disabled);

// Swallow this frame's press edge so nothing drawn later can claim the same
// click. For a press handled outside the widget/capture system.
void ui_consume_mouse_press(UIContext *ui, int button);

// ---- last-widget interaction query (call immediately after a widget call) ----
int  ui_is_item_activated(UIContext *ui);   // fires for exactly one frame when user first clicks it
int  ui_item_active(UIContext *ui);          // fires every frame the user keeps holding it down
int  ui_is_item_deactivated(UIContext *ui); // fires for exactly one frame when user lets go


// ---- dynamic / draggable components ----
UIControl *ui_add_button(UIContext *ui, float x, float y, const char *text,
                         void (*cb)(UIContext *, int, void *), int id, void *data);
UIControl *ui_add_checkbox(UIContext *ui, float x, float y, const char *text, int value);
UIControl *ui_add_slider(UIContext *ui, float x, float y, const char *text,
                         float value, float min, float middle, float max);
UIControl *ui_add_radio(UIContext *ui, float x, float y, const char *text,
                        int *value, int which);
UIControl *ui_add_label(UIContext *ui, float x, float y, const char *text);

UIControl *ui_add_dropdown(UIContext *ui, float x, float y, const char *text,
                           int *value, int num_items, const char **items,
                           int flags, float w); // items[] is not copied -- it is read every frame
void ui_set_component_text(UIControl *c, const char *text);

// ---- low-level screen helpers (used by widgets and the textarea) ----
bool ui_cell_was_drawn(UIContext *ui, int x, int y);
void ui_draw_cell(UIContext *ui, float col, float row,
                  unsigned char ch, unsigned int color);
void ui_draw_cell_raw(UIContext *ui, int col, int row,
                       unsigned char ch, unsigned int color,
                       float scale, float weight, float offX, float offY, bool force_write, int flags);
void ui_draw_cell_flags(UIContext *ui, float col, float row,
                  unsigned char ch, unsigned int color, int flags);
void ui_draw_cell_flags_weight(UIContext *ui, float col, float row,
                  unsigned char ch, unsigned int color, int flags, float weight);
int ui_draw_text(UIContext *ui, float fcol, float frow, const char *s,
                 unsigned int color);
int  ui_get_cell(UIContext *ui, int col, int row, OutputCell *out);
void ui_fill_rect(UIContext *ui, float col, float row, int w, int h,
                  unsigned char ch, unsigned int color);

// ---- os event input ----
void ui_os_mouse_event(UIContext *ui, float x, float y, float wheel,
                       int type, int which_button, int flags);
void ui_os_key_event(UIContext *ui, int keycode, int is_down, int flags);
void ui_os_char_event(UIContext *ui, int ch, int flags);

int ui_id_from_ptr(const void *ptr);
void ui_set_active_control(UIContext *ui, int id, void *state);
// Same, but the capture is held by `button` rather than the left mouse button.
// ui_end() drops a capture whose button is no longer down, so a right-button
// drag (the number scrubber) has to say so or it is released after one frame.
void ui_set_active_control_btn(UIContext *ui, int id, void *state, int button);


static inline void ui_memmove(void *dst, const void *src, int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { for (int i = 0; i < n; i++) d[i] = s[i]; }
    else if (d > s) { for (int i = n; i > 0; i--) d[i - 1] = s[i - 1]; }
}
static inline void ui_memcpy(void *dst, const void *src, int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}

// ---- label "##suffix" convention (from imgui) ----
// Everything from the first "##" is id-only: hashed into the widget id but
// never drawn and never measured. It is how two widgets that must READ the
// same ("Run##snd" / "Run##vis") get distinct ids. Distinct ids matter because
// a collision breaks BOTH widgets: the first one drawn claims the shared id on
// press, then fails its own hit test on release and clears active_id, so the
// one actually clicked never fires. For a whole duplicated component, prefer
// ui_push_id_ptr over sprinkling "##" through every label.
static inline int ui_display_len(const char *s) {
    if (!s) return 0;
    int n = 0;
    while (s[n] && !(s[n] == '#' && s[n + 1] == '#')) n++;
    return n;
}

// ---- FNV-1a 32-bit hash (stable widget IDs) ----
#define UI_FNV_PRIME   16777619U
#define UI_FNV_OFFSET  2166136261U
static inline unsigned int ui_hash_fnv1a(const char *data, int len, unsigned int seed) {
    const unsigned char *p = (const unsigned char *)data;
    unsigned int h = seed ? seed : UI_FNV_OFFSET;
    while (len-- > 0) { h ^= *p++; h *= UI_FNV_PRIME; }
    return h;
}

// ---- special drawing glyph slots (reserved for future custom font) ----
#define UI_CHAR_CHECKBOX_MIDDLE_DISABLED  127
#define UI_CHAR_CHECKBOX_LEFT             128
#define UI_CHAR_CHECKBOX_MIDDLE_ENABLED   129
#define UI_CHAR_CHECKBOX_RIGHT            130
#define UI_CHAR_VERTICAL_LINE             131
#define UI_CHAR_HORIZONTAL_LINE           132
#define UI_CHAR_ROUND_CORNER_TOP_LEFT     133
#define UI_CHAR_ROUND_CORNER_TOP_RIGHT    134
#define UI_CHAR_ROUND_CORNER_BOTTOM_LEFT  135
#define UI_CHAR_ROUND_CORNER_BOTTOM_RIGHT 136
#define UI_CHAR_TRIANGLE_UP               137
#define UI_CHAR_TRIANGLE_DOWN             138
#define UI_CHAR_TRIANGLE_RIGHT            139
#define UI_CHAR_TRIANGLE_LEFT             140
#define UI_CHAR_CIRCLE_ON_LINE            141
#define UI_CHAR_BIG_CIRCLE_ON_LINE_L      142
#define UI_CHAR_BIG_CIRCLE_ON_LINE_R      143
#define UI_CHAR_BIG_CIRCLE_L              144
#define UI_CHAR_BIG_CIRCLE_R              145
#define UI_CHAR_BIG_CIRCLE_FILLED_L       146
#define UI_CHAR_BIG_CIRCLE_FILLED_R       147
#define UI_CHAR_HORIZONTAL_STRIPES        148


void ui_helper_draw_round_rect(UIContext *ui, int col, int row, int width, int height,
                               unsigned int color);

// NOTE: these are aligned with platform.h flags & mouse event types, keep in sync.
#define UI_FLAGS_DOUBLE_CLICK (1 << 0)
#define UI_FLAGS_CTRL         (1 << 1)
#define UI_FLAGS_SHIFT        (1 << 2)
#define UI_FLAGS_ALT          (1 << 3)

#define UI_MOUSE_TYPE_MOVE   0
#define UI_MOUSE_TYPE_DOWN   1
#define UI_MOUSE_TYPE_UP     2
#define UI_MOUSE_TYPE_DRAG   3
#define UI_MOUSE_TYPE_WHEEL  4

#define UI_KEY_ESCAPE 0x1B

#define UI_FOCUS_RELEASED (-1)

#define UI_MOUSE_BUTTON_NONE   -1
#define UI_MOUSE_BUTTON_LEFT    0
#define UI_MOUSE_BUTTON_MIDDLE  1
#define UI_MOUSE_BUTTON_RIGHT   2

#endif // TEXTMODE_UI_H
