#include "textmode_ui.h"
#include "common/math_pure.h"
#include "common/string_pure.h"
#include "textmode_cell.h"

//#define UI_COL_BLACK   0xFF000000
#define UI_C8_GRAY0      25 // 0x19
#define UI_C8_GRAY1      51 // (39 + 1 * 12)
#define UI_C8_GRAY2      63 // (39 + 2 * 12)
//#define UI_C8_GRAY3      74 // (39 + 3 * 12)
#define UI_C8_GRAY4      87 // (39 + 4 * 12)
//#define UI_C8_GRAY5      99 
#define UI_C8_GRAY6     111 
//#define UI_C8_GRAY7     123 
//#define UI_C8_GRAY8     135 
//#define UI_C8_GRAY9     147 
//#define UI_C8_GRAY10    159 
#define UI_C8_ACCENT    160 // 0xA0
#define UI_C8_DIM_WHITE 160 // 0xA0 
// #define UI_C8_GRAY11    171 
// 183,195,207,219,231,243,255

#define UI_C8_WHITE      0xFF

static void ui_default_theme(UIContext *ui) {
    unsigned int base                 = TM_COLOR_PACK(UI_C8_DIM_WHITE, 0, UI_C8_GRAY0);
    ui->theme[UI_COL_DEFAULT]         = base;
    // A button body is not an authored colour of its own: it starts as the
    // panel background and theme_blend below lifts it a tenth of the way
    // towards the text it carries, so it stands off the panel by the same
    // amount on whatever ramp the theme is built on.
    ui->theme[UI_COL_BUTTON]          = base;
    ui->theme[UI_COL_BUTTON_HOVER]    = TM_COLOR_PACK(UI_C8_DIM_WHITE, 0, UI_C8_GRAY4);
    ui->theme[UI_COL_BUTTON_ACT]      = TM_COLOR_PACK(UI_C8_WHITE,     1, UI_C8_GRAY4);
    ui->theme[UI_COL_DRAGGABLE]       = TM_COLOR_PACK(UI_C8_DIM_WHITE, 1, UI_C8_GRAY1);
    ui->theme[UI_COL_DRAGGABLE_HOVER] = TM_COLOR_PACK(UI_C8_DIM_WHITE, 1, UI_C8_GRAY4);
    ui->theme[UI_COL_DIMMED]          = TM_COLOR_PACK(UI_C8_GRAY4,     0, UI_C8_GRAY1);
    ui->theme[UI_COL_DIMMED_BG]       = TM_COLOR_PACK(UI_C8_DIM_WHITE, 0, UI_C8_GRAY1);

    ui->theme[UI_COL_COLD_ACCENT]     = TM_COLOR_PACK(UI_C8_ACCENT,    1, UI_C8_GRAY0);
    ui->theme[UI_COL_WARM_ACCENT]     = TM_COLOR_PACK(UI_C8_ACCENT,    2, UI_C8_GRAY0);
    ui->theme[UI_COL_GREEN_ACCENT]    = TM_COLOR_PACK(UI_C8_ACCENT,    3, UI_C8_GRAY0);
    ui->theme[UI_COL_DIMMED_TEXT]     = TM_COLOR_PACK(UI_C8_GRAY4,     0, UI_C8_GRAY0);
    ui->theme[UI_COL_LABEL]           = TM_COLOR_PACK(UI_C8_GRAY6,     0, UI_C8_GRAY0);

    for (int i = 0; i < UI_NUM_COLORS; i++) ui->theme_blend[i] = 0.0f;
    ui->theme_blend[UI_COL_BUTTON] = 0.1f;
}

//static int ui_theme_grade_channel(float black, float gain, int v) {
//    float g = black + ((float)v - (float)UI_C8_GRAY0) * gain;
//    int   o = (int)(g + 0.5f);
//    if (o < 0) o = 0; else if (o > 255) o = 255;
//    return o;
//}

static int adjust(float diff, int v) {
    int  o = (int)(v + diff * 255.f); 
    if (o < 0) o = 0; else if (o > 255) o = 255;
    return o;
}
// Soft-clips the top of x into [0, 1 - 0.6*a]: [0, 1-a) passes through
// untouched, everything above it bends onto a smooth asymptote. C1 at the knee,
// monotone, and the identity at a == 0.
static float softcliptop(float x, float a) {
    if (a <= 0.0f) return x;
    float ofs = 0.9f * a;
    x += ofs;
    a  = 1.0f - 0.1f * a;
    if (x < a) return x - ofs;
    float one_over_1_minus_a = 1.0f / (1.0f - a);
    x = (x - a) * one_over_1_minus_a;
    x *= x + 1.0f;
    x = (a + x) / (x + 1.0f);
    return x - ofs;
}


static int adjust_text_intensity(float cutoff, int v) {
    int o = (int)(softcliptop((float)v * (1.0f / 255.0f), 1.0f - cutoff) * 255.0f + 0.5f);
    if (o < 0) o = 0; else if (o > 255) o = 255;
    return o;
}
unsigned int ui_theme_adjust(const UIContext *ui, unsigned int packed) {

    return TM_COLOR_PACK(adjust_text_intensity(ui->theme_text_intensity, (int)TM_COLOR_FG(packed)),
                         TM_COLOR_GRADIENT(packed),
                         adjust(ui->theme_black_point - UI_THEME_BLACK_POINT_DEFAULT, (int)TM_COLOR_BG(packed)));

//    if (ui->theme_black_point    == UI_THEME_BLACK_POINT_DEFAULT &&
//        ui->theme_text_intensity == UI_THEME_TEXT_INTENSITY_DEFAULT) return packed;
//    float black = ui->theme_black_point * 255.0f;
//    float gain  = ui->theme_text_intensity / UI_THEME_TEXT_INTENSITY_DEFAULT;
//    return TM_COLOR_PACK(ui_theme_grade_channel(black, gain, (int)TM_COLOR_FG(packed)),
//                         TM_COLOR_GRADIENT(packed),
//                         ui_theme_grade_channel(black, gain, (int)TM_COLOR_BG(packed)));
}

// Mix a slot's foreground into its background, staying on the ramp the
// background already sits on: fg and bg are two intensities on one gradient, so
// blending them is a step along that ramp and never a new hue. fg_amount is the
// foreground's share of the result -- 0.1f is 90% background, 10% foreground.
static unsigned int ui_blend_bg_towards_fg(unsigned int c, float fg_amount) {
    if (fg_amount <= 0.0f) return c;
    int bg = (int)TM_COLOR_BG(c), fg = (int)TM_COLOR_FG(c);
    int v  = (int)((float)bg + (float)(fg - bg) * fg_amount + 0.5f);
    if (v < 0) v = 0; else if (v > 255) v = 255;
    return TM_COLOR_PACK(TM_COLOR_FG(c), TM_COLOR_GRADIENT(c), v);
}

unsigned int ui_theme_color(const UIContext *ui, int slot) {
    if (slot < 0 || slot >= UI_NUM_COLORS) slot = UI_COL_DEFAULT;
    // Blend after grading, never before: the lift is a fraction of the distance
    // between the two ends the theme actually paints, so a slot keeps the same
    // relative separation from its own text once the knobs have moved them.
    return ui_blend_bg_towards_fg(ui_theme_adjust(ui, ui->theme[slot]),
                                  ui->theme_blend[slot]);
}


//static int ui_itoa(int v, char *buf) {
//    int n = 0;
//    if (v < 0) { buf[n++] = '-'; v = -v; }
//    char tmp[16]; int t = 0;
//    if (v == 0) tmp[t++] = '0';
//    while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
//    while (t > 0) buf[n++] = tmp[--t];
//    buf[n] = '\0';
//    return n;
//}

static void ui_ftoa(float f, char *buf, int n) {
    char tmp[S_FROM_NUMBER_MAX_CHARS];
    s_from_number(f, tmp);
    int i = 0;
    for(; i < n; i++) {
        buf[i] = tmp[i];
    }
    buf[i] = 0;
//    int neg = (f < 0);
//    if (neg) f = -f;
//    int whole = (int)f;
//    int frac = (int)((f - (float)whole) * 100.0f + 0.5f);
//    if (frac >= 100) { whole++; frac -= 100; }
//    int n = 0;
//    if (neg) buf[n++] = '-';
//    n += ui_itoa(whole, buf + n);
//    buf[n++] = '.';
//    buf[n++] = (char)('0' + frac / 10);
//    buf[n++] = (char)('0' + frac % 10);
//    buf[n] = '\0';
}

// Label text sitting inside a widget: UI_COL_LABEL's foreground on whatever
// background that widget already established, so a caption dims to the label
// ramp without punching a differently-coloured box into the control.
static unsigned int ui_label_color_on(UIContext *ui, unsigned int c) {
    return TM_COLOR_PACK(TM_COLOR_FG(ui_theme_color(ui, UI_COL_LABEL)),
                         TM_COLOR_GRADIENT(ui_theme_color(ui, UI_COL_LABEL)),
                         TM_COLOR_BG(c));
}

// ===== clip rect =====
// Cells outside it are dropped by ui_draw_cell_raw and hits inside it are the
// only ones ui_hit_raw reports, so a clipped-away widget is neither visible nor
// clickable. Reset to the whole screen whenever the screen changes size.
static void ui_clip_reset(UIContext *ui) {
    ui->clip_x0 = 0;
    ui->clip_y0 = 0;
    ui->clip_x1 = ui->cols;
    ui->clip_y1 = ui->rows;
    ui->clip_sp = 0;
}

void ui_push_clip_rect(UIContext *ui, float x, float y, float w, float h) {
    if (ui->clip_sp < 8) {
        ui->clip_stack[ui->clip_sp][0] = ui->clip_x0;
        ui->clip_stack[ui->clip_sp][1] = ui->clip_y0;
        ui->clip_stack[ui->clip_sp][2] = ui->clip_x1;
        ui->clip_stack[ui->clip_sp][3] = ui->clip_y1;
    }
    ui->clip_sp++; // counted even when the stack is full, so pop stays balanced
    if (ui->clip_sp > 8) return; // dropped: keep the tighter enclosing rect
    int x0 = (int)x, y0 = (int)y;
    int x1 = (int)(x + w), y1 = (int)(y + h);
    if (x0 > ui->clip_x0) ui->clip_x0 = x0; // intersect with the enclosing rect
    if (y0 > ui->clip_y0) ui->clip_y0 = y0;
    if (x1 < ui->clip_x1) ui->clip_x1 = x1;
    if (y1 < ui->clip_y1) ui->clip_y1 = y1;
}

void ui_pop_clip_rect(UIContext *ui) {
    if (ui->clip_sp <= 0) return;
    ui->clip_sp--;
    if (ui->clip_sp >= 8) return; // never pushed a saved rect for this level
    ui->clip_x0 = ui->clip_stack[ui->clip_sp][0];
    ui->clip_y0 = ui->clip_stack[ui->clip_sp][1];
    ui->clip_x1 = ui->clip_stack[ui->clip_sp][2];
    ui->clip_y1 = ui->clip_stack[ui->clip_sp][3];
}


unsigned int ui_adjust_color_additive(const UIContext *ui, unsigned int c,
                                      int fg_offset, int bg_offset) {
    int fg = (int)TM_COLOR_FG(c) + fg_offset;
    int bg = (int)TM_COLOR_BG(c) + bg_offset;
    if (fg < 0) fg = 0; else if (fg > 255) fg = 255;
    if (bg < 0) bg = 0; else if (bg > 255) bg = 255;
    return TM_COLOR_PACK(fg, TM_COLOR_GRADIENT(c), bg);
}

// Same text colour and hue, but sitting on the panel background instead of the
// slot's own: for glyphs drawn straight onto the panel (checkbox, radio), where
// a hover must brighten the mark without painting a button-coloured box.
static unsigned int ui_color_on_panel_bg(UIContext *ui, unsigned int c) {
    return TM_COLOR_PACK(TM_COLOR_FG(c), TM_COLOR_GRADIENT(c),
                         TM_COLOR_BG(ui_theme_color(ui, UI_COL_DEFAULT)));
}

// ===== disabled controls =====
// Flatten the cell background toward the panel background and fade the text
// into it, so a disabled control keeps its shape and its layout but loses the
// contrast that says "you can press this". The gradient goes to 0 as well --
// an accent colour reads as "live" no matter how dark it is.
static unsigned int ui_dim_color(UIContext *ui, unsigned int c) {
    int base = (int)TM_COLOR_BG(ui_theme_color(ui, UI_COL_DEFAULT));
    int bg   = (int)TM_COLOR_BG(c);
    int fg   = (int)TM_COLOR_FG(c);
    bg = base + ((bg - base) * 2) / 5;   // 40% of the way out from the panel bg
    fg = bg   + ((fg - bg)   * 9) / 20;  // 45% of the way from that bg to the text
    return TM_COLOR_PACK(fg, 0, bg);
}

// Blanking the input for the duration of the scope is what makes a disabled
// control inert, and it is deliberately not done through ui_hit(): the
// scrollbar and the textarea test ui->mouse_x / ui->wheel / the key queue
// themselves, so anything short of blanking the state would need every widget
// to opt in (and every future one to remember to).
static void ui_disable_park_input(UIContext *ui) {
    ui->saved_mouse_x  = ui->mouse_x;  ui->saved_mouse_y  = ui->mouse_y;
    ui->saved_mouse_dx = ui->mouse_dx; ui->saved_mouse_dy = ui->mouse_dy;
    ui->saved_wheel    = ui->wheel;
    for (int b = 0; b < 3; b++) {
        ui->saved_mouse_down[b]     = ui->mouse_down[b];
        ui->saved_mouse_pressed[b]  = ui->mouse_pressed[b];
        ui->saved_mouse_released[b] = ui->mouse_released[b];
        ui->mouse_down[b] = ui->mouse_pressed[b] = ui->mouse_released[b] = 0;
    }
    ui->saved_num_key_events  = ui->num_key_events;
    ui->saved_num_char_events = ui->num_char_events;
    ui->num_key_events = ui->num_char_events = 0;
    // Far off-screen rather than just outside the widget: every hit test in the
    // scope must fail, including the ones that never see the widget's rect.
    ui->mouse_x = ui->mouse_y = -1.0e9f;
    ui->mouse_dx = ui->mouse_dy = 0.0f;
    ui->wheel = 0.0f;
}
static void ui_disable_restore_input(UIContext *ui) {
    ui->mouse_x  = ui->saved_mouse_x;  ui->mouse_y  = ui->saved_mouse_y;
    ui->mouse_dx = ui->saved_mouse_dx; ui->mouse_dy = ui->saved_mouse_dy;
    ui->wheel    = ui->saved_wheel;
    for (int b = 0; b < 3; b++) {
        ui->mouse_down[b]     = ui->saved_mouse_down[b];
        ui->mouse_pressed[b]  = ui->saved_mouse_pressed[b];
        ui->mouse_released[b] = ui->saved_mouse_released[b];
    }
    ui->num_key_events  = ui->saved_num_key_events;
    ui->num_char_events = ui->saved_num_char_events;
}

void ui_disable_next(UIContext *ui) { ui->disable_next = true; }

void ui_begin_disabled(UIContext *ui) {
    // Only the outermost scope parks the input -- an inner one would "save" the
    // already-blanked state and restore that on the way out.
    if (ui->disabled_depth++ == 0) ui_disable_park_input(ui);
}
void ui_end_disabled(UIContext *ui) {
    if (ui->disabled_depth <= 0) return; // unbalanced end; nothing to restore
    if (--ui->disabled_depth == 0) ui_disable_restore_input(ui);
}
int ui_is_disabled(UIContext *ui) { return ui->disabled_depth > 0; }

int ui_item_disable_begin(UIContext *ui) {
    int disabled = (ui->disabled_depth > 0) || ui->disable_next;
    ui->disable_next = false; // this control consumes it, however it turns out
    if (disabled) ui_begin_disabled(ui);
    return disabled;
}
void ui_item_disable_end(UIContext *ui, int disabled) {
    if (disabled) ui_end_disabled(ui);
}

// ===== raw cell write (no CELL_FLAGS_DRAWN guard -- used for backgrounds) =====
static void ui_screen_set_cell(UIContext *ui, int col, int row,
                               unsigned char ch, unsigned int color) {
    if (col < 0 || col >= ui->cols || row < 0 || row >= ui->rows) return;
    OutputCell *c = &ui->screen[row * ui->cols + col];
    c->ch = ch; c->color = color;
    c->scale = 1.0f; c->weight = 1.0f; c->offset_x = 0.0f; c->offset_y = 0.0f;
    // deliberately does NOT touch c->flags
}



UIContext *ui_create(Tsys *sys, int cols, int rows) {
    UIContext *ui = (UIContext *)sys->malloc(sizeof(UIContext));
    sys->memset(ui, 0, sizeof(UIContext));
    ui->sys = sys;
    ui->cols = cols;
    ui->rows = rows;
    ui->screen = (OutputCell *)sys->malloc(sizeof(OutputCell) * cols * rows);
    ui->cell_w = 1.0f;
    ui->cell_h = 1.0f;
    ui->global_scale = 1.0f;
    ui->global_weight = 1.0f;
    ui->global_line_height = 1.5f;
    ui->window_has_focus = 1;
    array_init(&ui->components, sizeof(UIControl));
    array_init(&ui->overlay_components, sizeof(UIControl));
    ui->popup_bg_component_idx = -1;
    ui_clip_reset(ui);
    ui->theme_black_point    = UI_THEME_BLACK_POINT_DEFAULT;
    ui->theme_text_intensity = UI_THEME_TEXT_INTENSITY_DEFAULT;
    ui_default_theme(ui);
    return ui;
}

void ui_resize(UIContext *ui, int cols, int rows) {
    if (cols <= 0 || rows <= 0) return;
    if (cols == ui->cols && rows == ui->rows) return;
    ui->sys->free(ui->screen);
    ui->screen = (OutputCell *)ui->sys->malloc(sizeof(OutputCell) * cols * rows);
    ui->cols = cols;
    ui->rows = rows;
    ui_clip_reset(ui);
}

void ui_destroy(UIContext *ui) {
    if (!ui) return;
    Tsys *sys = ui->sys;
    array_free(&ui->components, sys);
    array_free(&ui->overlay_components, sys);
    sys->free(ui->screen);
    sys->free(ui);
}

OutputCell *ui_access(UIContext *ui) { return ui->screen; }

void ui_set_cell_size(UIContext *ui, float cell_w, float cell_h) {
    ui->cell_w = (cell_w > 0.0f) ? cell_w : 1.0f;
    ui->cell_h = (cell_h > 0.0f) ? cell_h : 1.0f;
}

bool ui_cell_was_drawn(UIContext *ui, int x, int y) {
    return ui->screen[y * ui->cols + x].flags & CELL_FLAGS_DRAWN;
}


// ===== screen cell helpers =====
// first-draw-wins: see FIRST-DRAW-WINS CONVENTION in textmode_ui.h
void ui_draw_cell_raw(UIContext *ui, int col, int row,
                       unsigned char ch, unsigned int color,
                       float scale, float weight, float offX, float offY, bool force_write, int flags) {
    if (col < 0 || col >= ui->cols || row < 0 || row >= ui->rows) return;
    if (col < ui->clip_x0 || col >= ui->clip_x1 ||
        row < ui->clip_y0 || row >= ui->clip_y1) return;
    OutputCell *c = &ui->screen[row * ui->cols + col];
    if (c->flags & CELL_FLAGS_DRAWN && !force_write) return;
    // Dimming lives here, at the one place every widget's cells pass through,
    // so a disabled widget dims whatever theme slots it happened to pick -- and
    // so does app drawing bracketed by ui_begin_disabled().
    if (ui->disabled_depth > 0) color = ui_dim_color(ui, color);
    c->ch = ch; c->color = color;
    c->flags = flags | CELL_FLAGS_DRAWN;
    c->scale = scale; c->weight = weight; c->offset_x = offX; c->offset_y = offY;
}

void ui_draw_cell_flags_weight(UIContext *ui, float col, float row,
                  unsigned char ch, unsigned int color, int flags, float weight) {
    int icol = (int)(col + 0.5f);
    int irow = (int)(row + 0.5f);
    col -= icol; row -= irow;
    ui_draw_cell_raw(ui, icol, irow, ch, color, ui->global_scale, weight, col, row, false, flags);
}
void ui_draw_cell_flags(UIContext *ui, float col, float row,
                  unsigned char ch, unsigned int color, int flags) {
    ui_draw_cell_flags_weight(ui, col, row, ch, color, flags, ui->global_weight);
}

void ui_draw_cell(UIContext *ui, float col, float row,
    unsigned char ch, unsigned int color) {
    ui_draw_cell_flags_weight(ui, col, row, ch, color, 0, ui->global_weight);
}

int ui_draw_text(UIContext *ui, float fcol, float frow, const char *s,
                 unsigned int color) {
    int col = (int)(fcol + 0.5f);
    int row = (int)(frow + 0.5f);
    fcol -= col; frow -= row;
    if (!s) return col;
    int c = col;
    for (int i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if(ch == '#' && s[i + 1] == '#')  // "##" starts the id-only suffix
            break;
        if (ch < 32 || ch > 126) ch = '?';
        ui_draw_cell_raw(ui, c, row, ch, color, ui->global_scale, ui->global_weight, fcol, frow, false, 0);
        c++;
    }
    return c;
}

int ui_get_cell(UIContext *ui, int col, int row, OutputCell *out) {
    if (col < 0 || col >= ui->cols || row < 0 || row >= ui->rows) return 0;
    *out = ui->screen[row * ui->cols + col];
    return 1;
}

void ui_fill_rect(UIContext *ui, float col, float row, int w, int h,
                  unsigned char ch, unsigned int color) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            ui_draw_cell(ui, (float)(col + c), (float)(row + r), ch, color);
}

void ui_helper_draw_round_rect(UIContext *ui, int col, int row, int width, int height,
                               unsigned int color) {
    if (width < 2 || height < 2) { ui_fill_rect(ui, (float)col, (float)row, width, height, ' ', color); return; }
    for (int r = 0; r < height; r++) {
        for (int c = 0; c < width; c++) {
            unsigned char ch = ' ';
            int left = (c == 0), right = (c == width - 1);
            int top = (r == 0), bot = (r == height - 1);
            if (top && left) ch = '.';
            else if (top && right) ch = '.';
            else if (bot && left) ch = '\'';
            else if (bot && right) ch = '\'';
            else if (top || bot) ch = '-';
            else if (left || right) ch = '|';
            ui_draw_cell(ui, (float)(col + c), (float)(row + r), ch, color);
        }
    }
}

// ===== layout =====
float ui_avail_width(UIContext *ui) {
    float w = ui->content_max_x - ui->pen_x;
    return w < 0.0f ? 0.0f : w;
}

float ui_take_width(UIContext *ui, float autow) {
    float w = autow;
    if (ui->item_width_sp > 0) {
        float iw = ui->item_width_stack[ui->item_width_sp - 1];
        if (iw < 0.0f) { // stretch: leave |iw| cells free on the right
            w = (ui->content_max_x - ui->pen_x) + iw;
        } else if (iw > 0.0f) {
            w = iw;
        }
    }
    // never let a widget extend past the content region right edge
    float avail = ui->content_max_x - ui->pen_x;
    if (w > avail) w = avail;
    if (w < 1.0f) w = 1.0f;
    return w;
}

void ui_advance(UIContext *ui, float x, float y, float w, float h) {
    ui->last_x = x; ui->last_y = y; ui->last_w = w; ui->last_h = h;
    ui->pen_x = ui->line_start_x;
    ui->pen_y = y + h;
}

void ui_same_line(UIContext *ui) {
    ui->pen_x = ui->last_x + ui->last_w + 1.0f;
    ui->pen_y = ui->last_y;
}

void ui_push_item_width(UIContext *ui, float w) {
    if (ui->item_width_sp < 16) ui->item_width_stack[ui->item_width_sp++] = w;
}
void ui_pop_item_width(UIContext *ui) {
    if (ui->item_width_sp > 0) ui->item_width_sp--;
}

void ui_set_cursor(UIContext *ui, float x, float y) {
    ui->pen_x = x;
    ui->pen_y = y;
    ui->line_start_x = x;
}

void ui_set_cursor_column(UIContext *ui, float x, float y, float width) {
    ui->pen_x = x;
    ui->pen_y = y;
    ui->line_start_x = x;
    ui->content_max_x = x + width;
}

void ui_dummy(UIContext *ui, float w, float h) {
    ui_advance(ui, ui->pen_x, ui->pen_y, w, h);
}
void ui_blank_row(UIContext *ui) { ui_dummy(ui, 0, 1); }
void ui_same_line_pad(UIContext *ui, int count)
{
    ui_same_line(ui);
    ui_dummy(ui, (float)count, 0);
    ui_same_line(ui);
}
void ui_indent(UIContext *ui, int count) {
    ui_dummy(ui, (float)(count - 1), 0);
    ui_same_line(ui);
}
void ui_same_line_col(UIContext *ui, int col) {
    ui_same_line(ui);
    float target = ui->line_start_x + (float)col;
    if (target > ui->pen_x) ui->pen_x = target;
}

// ===== interaction helpers =====
// raw: always tests geometry, ignores overlay state
int ui_hit_raw(UIContext *ui, float x, float y, float w, float h) {
    // The clipped-away part of a widget isn't drawn, so it must not be clickable
    // either -- otherwise a row overflowing its panel keeps reacting to clicks
    // landing on whatever is drawn over it.
    if (ui->mouse_x < (float)ui->clip_x0 || ui->mouse_x >= (float)ui->clip_x1 ||
        ui->mouse_y < (float)ui->clip_y0 || ui->mouse_y >= (float)ui->clip_y1) return 0;
    return (ui->mouse_x >= x && ui->mouse_x < x + w &&
            ui->mouse_y >= y && ui->mouse_y < y + h);
}
// interactive: returns 0 when an overlay has consumed mouse input
int ui_hit(UIContext *ui, float x, float y, float w, float h) {
    if (ui->overlay_consumed_input) return 0;
    if (ui->active_id != 0) return 0;
    return ui_hit_raw(ui, x, y, w, h);
}

// ===== ID helpers =====
static unsigned int ui_make_id(UIContext *ui, const char *label) {
    unsigned int seed = ui->id_stack_sp > 0 ? ui->id_stack[ui->id_stack_sp - 1] : 0;
    return ui_hash_fnv1a(label, (int)s_strlen(label), seed);
}
// Returns display length (chars before ##); writes stable hash to *out_id
static int ui_label_and_id(UIContext *ui, const char *label, unsigned int *out_id) {
    *out_id = ui_make_id(ui, label); // hash full string; ## suffix disambiguates
    return ui_display_len(label);
}


static void ui_push_id_num(UIContext *ui, unsigned int value) {
    unsigned int seed = ui->id_stack_sp > 0 ? ui->id_stack[ui->id_stack_sp - 1] : 0;
    if (ui->id_stack_sp < 8)
        ui->id_stack[ui->id_stack_sp++] =
            ui_hash_fnv1a((const char *)&value, (int)sizeof value, seed);
}

void ui_push_id(UIContext *ui, const char *str) {
    if (ui->id_stack_sp < 8)
        ui->id_stack[ui->id_stack_sp++] = ui_make_id(ui, str);
}
void ui_push_id_ptr(UIContext *ui, const void *ptr) {
    unsigned int seed = ui->id_stack_sp > 0 ? ui->id_stack[ui->id_stack_sp - 1] : 0;
    if (ui->id_stack_sp < 8)
        ui->id_stack[ui->id_stack_sp++] =
            ui_hash_fnv1a((const char *)&ptr, (int)sizeof ptr, seed);
}
void ui_pop_id(UIContext *ui) {
    if (ui->id_stack_sp > 0) ui->id_stack_sp--;
}

void ui_set_active_control_btn(UIContext *ui, int id, void *state, int button) {
    if (ui->active_state && ui->active_state != state) {
        ui->sys->free(ui->active_state);
    }
    ui->active_id = id;
    ui->active_button = button;
    ui->active_state = state;
}

void ui_set_active_control(UIContext *ui, int id, void *state) {
    ui_set_active_control_btn(ui, id, state, UI_MOUSE_BUTTON_LEFT);
}

// ===== last-widget interaction query =====
// Called by each widget at the end to record what happened to it.
static void ui_set_last_widget_state(UIContext *ui, int id) {
    ui->last_widget_activated   = (ui->active_id == id && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]);
    ui->last_widget_active      = (ui->active_id == id && ui->mouse_down[UI_MOUSE_BUTTON_LEFT]);
    ui->last_widget_deactivated = (ui->active_id == id && ui->mouse_released[UI_MOUSE_BUTTON_LEFT]);
}

int ui_is_item_activated(UIContext *ui) {
    return ui->last_widget_activated;
}

int ui_item_active(UIContext *ui) {
    return ui->last_widget_active;
}

int ui_is_item_deactivated(UIContext *ui) {
    return ui->last_widget_deactivated;
}

// ===== widgets =====
int ui_label(UIContext *ui, const char *text) {
    int disabled = ui_item_disable_begin(ui);
    int len = ui_display_len(text); // a "##suffix" is hashed, never drawn or measured
    ui_draw_text(ui, ui->pen_x, ui->pen_y, text, ui_theme_color(ui, UI_COL_LABEL));
    ui_advance(ui, ui->pen_x, ui->pen_y, (float)len, 1.0f);
    ui_item_disable_end(ui, disabled);
    return 0;
}

// Colour for a plain-button-look widget that is (or isn't) selected/on.
static unsigned int ui_option_color(UIContext *ui, int on, int hovered, int flags) {
    if (flags & UI_OPTION_DIM_WHEN_OFF) {
        // For toggles whose resting state is ON (a list filter that starts
        // enabled, say): on is the plain button look and off is dimmed back.
        // No accent -- accenting the default state would light up the row
        // permanently and carry no information. Hovering an off one lifts it to
        // the normal button colour, which is the "click to re-enable" cue.
        if (on) return ui_theme_color(ui, hovered ? UI_COL_BUTTON_HOVER : UI_COL_BUTTON);
        return ui_theme_color(ui, hovered ? UI_COL_BUTTON : UI_COL_DIMMED);
    }
    if (!on) return ui_theme_color(ui, hovered ? UI_COL_BUTTON_HOVER : UI_COL_BUTTON);

    // The sentinel test has to read the RAW slot: ui_theme_color grades the
    // background, and grading 0 with a black point above the default lands on a
    // non-zero colour -- an "unset" slot would then answer "set", and every
    // selected option button would paint itself fg 0 on a near-black bg.
    unsigned int c = ui->theme[hovered ? UI_COL_BUTTON_ON_HOVER : UI_COL_BUTTON_ON];
    if (c) return ui_theme_adjust(ui, c);
    // Unset (the usual case): use the dropdown's selection colour, so a
    // selected option button and a selected dropdown row match by construction
    // in every theme -- ui_dropdown paints both its open header and its
    // selected popup item with UI_COL_BUTTON_ACT (see ui_update_dynamic's
    // UI_COMPONENT_SELECTABLE case and ui_dropdown_begin).
    unsigned int act = ui_theme_color(ui, UI_COL_BUTTON_ACT);
    if (!hovered) return act;
    // Selected AND hovered: one step brighter, so hovering the selected item
    // doesn't read as deselecting it. (The palette ramps step by ~12.)
    return ui_adjust_color_additive(ui, act, 35, 12);
}

// Colour for one row of an open dropdown list. The whole list sits on the
// selection's blue ramp so it reads as a single block: unselected rows are the
// same hue stepped back, with the selected row the brightest cell in it.
// Selected-and-unhovered returns exactly UI_COL_BUTTON_ACT, so a dropdown row
// still matches the open header and a selected option button (see
// ui_option_color above). (The palette ramps step by ~12.)
static unsigned int ui_popup_row_color(UIContext *ui, int selected, int hovered) {
    unsigned int act = ui_theme_color(ui, UI_COL_BUTTON_ACT);
    unsigned int base = act;
    if (!selected) {
        // The selection's hue and ramp, but the panel's ordinary text weight
        // instead of its white -- then two steps darker.
        base = TM_COLOR_PACK(TM_COLOR_FG(ui_theme_color(ui, UI_COL_DEFAULT)),
                             TM_COLOR_GRADIENT(act), TM_COLOR_BG(act));
        base = ui_adjust_color_additive(ui, base, -30, -40);
    }
    // Hovered: one step brighter, so hovering the selected row doesn't read as
    // deselecting it.
    return hovered ? ui_adjust_color_additive(ui, base, 24, 12) : base;
}

// Shared body of ui_button and the plain-button-look selection widgets.
// disp_len is how many cells of `text` are drawn (callers honouring the "##"
// convention pass the display length; ui_draw_text stops at "##" by itself).
// `on` picks the selected colours; UI_OPTION_ALIGN_LEFT flips centring off.
// Returns 1 if pressed and released inside.
static int ui_button_core(UIContext *ui, const char *text, int disp_len,
                          unsigned int id, int on, int flags) {
    int disabled = ui_item_disable_begin(ui);
    float autow = (float)disp_len + 2.0f;
    float w = ui_take_width(ui, autow);
    float x = ui->pen_x, y = ui->pen_y, h = 1.0f;
    int iw = (int)w;

    int hovered = ui_hit(ui, x, y, w, h);
    if (hovered) ui->hot_id = id;
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) ui_set_active_control(ui, id, NULL);

    int clicked = 0;
    if (ui->active_id == id && ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) {
        if (ui_hit_raw(ui, x, y, w, h)) clicked = 1;
        ui_set_active_control(ui, 0, NULL);
    }
    ui_set_last_widget_state(ui, id);

    unsigned int col = (ui->active_id == id) ? ui_theme_color(ui, UI_COL_BUTTON_ACT)
                                             : ui_option_color(ui, on, hovered, flags);
    // Integer division on purpose: a half-cell text offset (odd padding, e.g. an
    // 8-char label in an 11-wide ui_option_bar item) reaches the renderer as
    // cell->offset_x, which shifts each glyph cell's BACKGROUND by half a cell
    // too (see render_cell_bg_8bit) -- leaving an unpainted half-cell seam
    // between the last glyph and the button's padding. Labels live on cell
    // boundaries; the odd cell goes to the right side.
    // The padding is integral, but it is added to the *float* x: a dynamic
    // control dragged to a fractional position then gives the label the same
    // sub-cell offset as the background fill below, instead of snapping the
    // text back onto the cell grid while the button body moves.
    int pad = (flags & UI_OPTION_ALIGN_LEFT) ? 1 : (iw - disp_len) / 2;
    if (pad < 1) pad = 1;
    float tx = x + (float)pad;
    ui_draw_text(ui, tx,  y, text, col);
    ui_fill_rect(ui, x, y, iw, 1, ' ', col); // fill padding around already-drawn text

    ui_advance(ui, x, y, w, h);
    ui_item_disable_end(ui, disabled);
    return clicked;
}

int ui_button(UIContext *ui, const char *text) {
    unsigned int id;
    int len = ui_label_and_id(ui, text, &id);
    return ui_button_core(ui, text, len, id, 0, 0);
}

int ui_toggle_button_flags(UIContext *ui, const char *text, bool on, int flags) {
    unsigned int id;
    int len = ui_label_and_id(ui, text, &id);
    return ui_button_core(ui, text, len, id, on ? 1 : 0, flags);
}
int ui_toggle_button(UIContext *ui, const char *text, bool on) {
    return ui_toggle_button_flags(ui, text, on, 0);
}

int ui_option_button_flags(UIContext *ui, const char *text, int *value, int which, int flags) {
    int on = (value && *value == which);
    if (!ui_toggle_button_flags(ui, text, on, flags)) return 0;
    if (!value || *value == which) return 0; // clicking the selected one changes nothing
    *value = which;
    return 1;
}
int ui_option_button(UIContext *ui, const char *text, int *value, int which) {
    return ui_option_button_flags(ui, text, value, which, 0);
}

int ui_option_bar(UIContext *ui, const char *id, int *value,
                  int count, const char *const items[], int flags) {
    // The group, not its first item, is what a ui_disable_next() means here:
    // take the scope up front, so every item sees an enclosing one instead of
    // racing to consume the one-shot flag -- and so an empty group consumes it
    // rather than leaking it onto the next control.
    int disabled = ui_item_disable_begin(ui);
    if (count <= 0 || !items) { ui_item_disable_end(ui, disabled); return 0; }
    int vertical = (flags & UI_OPTION_VERTICAL) != 0;
    // 1-cell gap between items across (what ui_same_line would give); rows
    // always abut vertically, like consecutive ui_button calls do.
    float gap = (vertical || (flags & UI_OPTION_NO_GAP)) ? 0.0f : 1.0f;

    // Uniform item width, so the group's extent is set by the items[] table and
    // doesn't move when the selection does.
    int maxlen = 0;
    for (int i = 0; i < count; i++) {
        int n = ui_display_len(items[i]);
        if (n > maxlen) maxlen = n;
    }
    float item_w = (float)maxlen + 2.0f;
    int extra = 0; // cells to hand out one-per-item to the leftmost items
    if (!vertical && (flags & UI_OPTION_FILL_WIDTH)) {
        float avail = ui_avail_width(ui) - gap * (float)(count - 1);
        if (avail > item_w * (float)count) {
            int total = (int)avail;
            item_w = (float)(total / count);
            extra  = total % count; // spread the remainder rather than under-filling
        }
    }

    float x0 = ui->pen_x, y0 = ui->pen_y;
    float saved_line_start = ui->line_start_x; // each item's ui_advance rewrites the pen
    float total_w = 0.0f, total_h = 1.0f;
    int changed = 0;

    ui_push_id(ui, id);
    for (int i = 0; i < count; i++) {
        float w = item_w + (i < extra ? 1.0f : 0.0f);
        // Place the pen directly rather than via ui_set_cursor, which would
        // move line_start_x and leave the pen inside the group afterwards.
        ui->pen_x = vertical ? x0 : x0 + total_w;
        ui->pen_y = vertical ? y0 + (float)i : y0;
        ui_push_item_width(ui, w);
        if (ui_option_button_flags(ui, items[i], value, i, flags)) changed = 1;
        ui_pop_item_width(ui);
        w = ui->last_w; // ui_take_width clamps at content_max_x; follow what it gave
        if (vertical) { if (w > total_w) total_w = w; }
        else            total_w += w + (i + 1 < count ? gap : 0.0f);
    }
    ui_pop_id(ui);
    if (vertical) total_h = (float)count;

    ui->line_start_x = saved_line_start;
    // Report the whole group as the last widget, so ui_same_line() clears it.
    ui_advance(ui, x0, y0, total_w, total_h);
    ui_item_disable_end(ui, disabled);
    return changed;
}

int ui_check_box(UIContext *ui, const char *text, bool *value) {
    int disabled = ui_item_disable_begin(ui);
    unsigned int uid;
    int len = ui_label_and_id(ui, text, &uid);
    int id = (int)uid;
    float w = (float)(len + 4);
    float x = ui->pen_x, y = ui->pen_y, h = 1.0f;

    int hovered = ui_hit(ui, x, y, w, h);
    if (hovered) ui->hot_id = id;
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) ui_set_active_control(ui, id, NULL);
    int changed = 0;
    if (ui->active_id == id && ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) {
        if (ui_hit_raw(ui, x, y, w, h) && value) { *value = !*value; changed = 1; }
        ui_set_active_control(ui, 0, NULL);
    }
    ui_set_last_widget_state(ui, id);

    // Box and caption are one control, both on UI_COL_DEFAULT -- the caption is
    // not a label here. Drawn straight onto the panel, so hover steps the text
    // colour up its own ramp (ui_color_on_panel_bg keeps the panel background)
    // rather than swapping in UI_COL_BUTTON_HOVER, whose foreground matches
    // UI_COL_DEFAULT and so left hovering with nothing to show for it.
    unsigned int col = ui_color_on_panel_bg(ui, ui_theme_color(ui, UI_COL_DEFAULT));
    if (hovered) col = ui_adjust_color_additive(ui, col, 35, 0);
    int on = (value && *value);
    ui_draw_cell(ui, x, y, UI_CHAR_CHECKBOX_LEFT, col);
    ui_draw_cell(ui, x + 1.f, y, on ? UI_CHAR_CHECKBOX_MIDDLE_ENABLED : UI_CHAR_CHECKBOX_MIDDLE_DISABLED, col);
    ui_draw_cell(ui, x + 2.f, y, UI_CHAR_CHECKBOX_RIGHT, col);
    ui_draw_text(ui, x + 3.f, y, text, col);
    // Claim the whole reserved width, trailing filler cell included: an undrawn
    // cell keeps ui_begin's cleared offset_x, so a dynamic checkbox dragged to a
    // fractional x would leave that last cell behind on the cell grid as soon as
    // anything paints it (the drag highlight in ui_process_components_internal).
    ui_fill_rect(ui, x, y, (int)w, 1, ' ', col);
    ui_advance(ui, x, y, w, h);
    ui_item_disable_end(ui, disabled);
    return changed;
}

int ui_radio_button(UIContext *ui, const char *text, int *value, int which) {
    int disabled = ui_item_disable_begin(ui);
    int id = ui_make_id(ui, text);
    int len = ui_display_len(text);
    float w = (float)(len == 0 ? 2 : len + 3);
    float x = ui->pen_x, y = ui->pen_y, h = 1.0f;

    int hovered = ui_hit(ui, x, y, w, h);
    if (hovered) ui->hot_id = id;
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) ui_set_active_control(ui, id, NULL);
    int changed = 0;
    if (ui->active_id == id && ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) {
        // "changed", not "clicked" -- re-picking the selected option is a no-op,
        // same contract as ui_option_button.
        if (ui_hit_raw(ui, x, y, w, h) && value && *value != which) { *value = which; changed = 1; }
        ui_set_active_control(ui, 0, NULL);
    }
    ui_set_last_widget_state(ui, id);

    // Box and caption are one control, both on UI_COL_DEFAULT -- the caption is
    // not a label here. Drawn straight onto the panel, so hover steps the text
    // colour up its own ramp (ui_color_on_panel_bg keeps the panel background)
    // rather than swapping in UI_COL_BUTTON_HOVER, whose foreground matches
    // UI_COL_DEFAULT and so left hovering with nothing to show for it.
    unsigned int col = ui_color_on_panel_bg(ui, ui_theme_color(ui, UI_COL_DEFAULT));
    if (hovered) col = ui_adjust_color_additive(ui, col, 35, 0);
    int on = (value && *value == which);
    ui_draw_cell(ui, x, y, on ? UI_CHAR_BIG_CIRCLE_FILLED_L : UI_CHAR_BIG_CIRCLE_L, col);
    ui_draw_cell(ui, x + 1.f, y, on ? UI_CHAR_BIG_CIRCLE_FILLED_R : UI_CHAR_BIG_CIRCLE_R, col);
    //ui_draw_cell(ui, x, y, '(', col);
    //ui_draw_cell(ui, x + 1.f, y, on ? 'o' : ' ', on ? accent_col : col);
    //ui_draw_cell(ui, x + 2.f, y, ')', col);
    if(len > 0) {
        ui_draw_cell(ui, x + 2.f, y, ' ', col);
        ui_draw_text(ui, x + 3.f, y, text, col);
    }

    ui_advance(ui, x, y, w, h);
    ui_item_disable_end(ui, disabled);
    return changed;
}


static float ui_slider_t_to_value(float t, float min, float mid, float max) {
    return mix(min, max, rsmul(clamp(t, 0.f, 1.f), (mid - min) / (max - min)));
}
static float ui_slider_value_to_t(float v, float min, float mid, float max) {
    return rsmul((v - min)/(max-min), 1 -(mid - min) / (max - min));
}


int ui_draggable_value(UIContext *ui, const char *text, float *value, float min, float middle, float max, int flags) {
    int disabled = ui_item_disable_begin(ui);
    if(middle == min || middle == max)
        middle = (min + max) / 2.f;
    
    int value_w = flags&15;
    flags -= value_w;

    int id = ui_make_id(ui, text);
    float autow = ui_avail_width(ui); // stretch to fill region unless item_width pushed
    float w = ui_take_width(ui, autow);
    float x = ui->pen_x, y = ui->pen_y, h = 1.0f;
    int iw = (int)w;
    int label_w = 0;
    int track_w = 0;
    float track_x = x;

    if((flags & UI_DRAGGABLE_HIDE_NAME) && (flags & UI_DRAGGABLE_HIDE_SLIDER)) 
    {
        w = (float)value_w;
    }
    else
    {
        label_w = flags & UI_DRAGGABLE_HIDE_NAME ? 0 : (int)s_strlen(text) + 1;
        track_w = iw - label_w - value_w;
        if (track_w < 3) { track_w = 3; label_w = iw - value_w - track_w; if (label_w < 0) label_w = 0; }
        track_x += label_w;
    }

    int changed = 0;

    int hovered = ui_hit(ui, x, y, w, h);
    if (hovered) ui->hot_id = id;
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        if ((ui->mouse_flags & UI_FLAGS_DOUBLE_CLICK) && value) {
            *value = middle;
            changed = 1;
            ui_set_active_control(ui, 0, NULL); // don't enter drag
        } else {
            ui_set_active_control(ui, id, NULL);
            ui->slider_anchor_t = value ? ui_slider_value_to_t(*value, min, middle, max) : 0.0f;
            ui->slider_anchor_mouse_x = ui->mouse_x;
            ui->slider_anchor_mouse_y = ui->mouse_y;
        }
    }
    if (ui->active_id == id && ui->mouse_down[UI_MOUSE_BUTTON_LEFT] && value) {
        if((ui->mouse_flags & UI_FLAGS_SHIFT) != (ui->prev_mouse_flags & UI_FLAGS_SHIFT)){
            ui->slider_anchor_mouse_x = ui->mouse_x;
            ui->slider_anchor_mouse_y = ui->mouse_y;
            ui->slider_anchor_t = value ? ui_slider_value_to_t(*value, min, middle, max) : 0.0f;
        }
        float delta = (ui->mouse_x - ui->slider_anchor_mouse_x) / (float)(track_w > 1 ? track_w - 1 : 1);
        if(ui->mouse_flags & UI_FLAGS_SHIFT)
            delta *= 0.05f;
        float newt = ui->slider_anchor_t + delta;
        float nv = ui_slider_t_to_value(newt, min, middle, max);
        if (nv != *value) { *value = nv; changed = 1; }
    }
    if (ui->active_id == id && !ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) ui_set_active_control(ui, 0, NULL);

    // label 
    if(label_w > 0) {
        char lbl[64];
        s_strncpy(lbl, text, (size_t)(label_w > 0 ? label_w : 1));
        ui_draw_text(ui, x, y, lbl, ui_theme_color(ui, UI_COL_LABEL));
    }

    // slider track 
    if(track_w > 0) {
        float t = value ? ui_slider_value_to_t(*value, min, middle, max) : 0.0f;

        bool big = true; // only large knobs/handles for now

//        bool big = track_w > 5;
//        float handle_offset       = big ? -0.805f : -0.60f;
//        float handle_range_offset = big ? -0.47f  :  0.16f;
//        float handlef = t * (float)(track_w - 1 + handle_range_offset) + handle_offset;

        float weight = ui->global_weight;
        float big_gl_margin_w = (5.f - weight) / 14.f; // big circle glyph horizontal margin

        // extra tweaks, not entirely sure why these are needed, 
        // (it's not grade-box calculations since special glyphs are exempt)
        float extra_single_pixel = 1.f / ui->cell_w; 
        float extra_offset = 1.f / 128.f;

        float handle_size = 2.f - 2.f * big_gl_margin_w;
        float handlef = t * (float)(track_w - handle_size - extra_single_pixel) - 0.5f - big_gl_margin_w + extra_offset;
        int handlei = (int)m_floorf(handlef + 0.5f);
        handlef -= handlei;
        for(int i = -1; i < track_w; i++) {
            int flags = i < 0 ? CELL_FLAGS_CLIP_L : (i == track_w - 1 ? CELL_FLAGS_CLIP_R : 0);
            unsigned char ch = UI_CHAR_HORIZONTAL_LINE;
            if     (!big && handlei   == i) ch = UI_CHAR_CIRCLE_ON_LINE;
            else if( big && handlei   == i) ch = UI_CHAR_BIG_CIRCLE_ON_LINE_L;
            else if( big && handlei+1 == i) ch = UI_CHAR_BIG_CIRCLE_ON_LINE_R;
            ui_draw_cell_raw(ui, (int)(track_x+0.5f) + i, (int)(y+0.5f), ch, ui_theme_color(ui, UI_COL_DEFAULT), ui->global_scale, weight, handlef, y - m_floorf(y + 0.5f), false, flags);
        }
    }
    
    // value readout
    if(value_w > 0) {
        int pad = label_w + track_w > 0 ? 1 : 0;
        char vbuf[16];
        ui_ftoa(value ? *value : 0.0f, vbuf, value_w - pad);
        ui_draw_text(ui, track_x + (float)track_w + (float)pad, y, vbuf,ui_theme_color(ui, UI_COL_DEFAULT));
        ui_fill_rect(ui, track_x + (float)track_w, y, value_w, 1, ' ',ui_theme_color(ui, UI_COL_DEFAULT));
    }

    ui_set_last_widget_state(ui, id);
    ui_advance(ui, x, y, w, h);
    ui_item_disable_end(ui, disabled);
    return changed;
}

int ui_slider(UIContext *ui, const char *text, float *value,
              float min, float middle, float max) {
    return ui_draggable_value(ui, text, value, min, middle, max, 5);
}
//int ui_scrubber(UIContext *ui, const char *text, float *value, float default_value, int num_chars) {
//    return ui_draggable_value(ui, text, value, 0.f, default_value, default_value * 99999.f, num_chars | UI_DRAGGABLE_HIDE_NAME | UI_DRAGGABLE_HIDE_SLIDER);
//}

int ui_id_from_ptr(const void *ptr) {
    unsigned long long ptr_val = (unsigned long long)ptr;
    unsigned int low  = (unsigned int)(ptr_val & 0xFFFFFFFFULL);
    unsigned int high = (unsigned int)(ptr_val >> 32);
    return (int)(low ^ high);
}

// ===== splitter =====
static int ui_splitter_left(float *split, int w, int min_w) {
    float lo = (float)min_w / (float)w, hi = (float)(w - 1 - min_w) / (float)w;
    if (hi < lo) hi = lo;
    if (*split < lo) *split = lo;
    if (*split > hi) *split = hi;
    int left = (int)((float)w * *split + 0.5f);
    if (left > w - 1 - min_w) left = w - 1 - min_w;
    if (left < min_w) left = min_w;
    return left;
}

int ui_splitter(UIContext *ui, float *split, int x, int y, int w, int h, int min_w) {
    if (min_w < 1) min_w = 1;
    if (w < 2 * min_w + 1) return w > 1 ? (w - 1) / 2 : 0;
    int left = ui_splitter_left(split, w, min_w);
    int id = ui_id_from_ptr(split);
    if (ui_hit(ui, (float)(x + left), (float)y, 1, (float)h)) {
        ui->hot_id = id;
        if (ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
            ui_set_active_control(ui, id, NULL);
            // where inside the divider cell the grab started: without it a press
            // on the cell's right half rounds to the next column before any drag
            ui->slider_anchor_mouse_x = ui->mouse_x - (float)(x + left);
        }
    }
    if (ui->active_id == id) {
        if (ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) {
            *split = (ui->mouse_x - ui->slider_anchor_mouse_x - (float)x) / (float)w;
            left = ui_splitter_left(split, w, min_w);
        }
        if (ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) ui_set_active_control(ui, 0, NULL);
    }
    // the panel's own background, lifted 8 steps while hovered or dragged
    unsigned int col = ui_theme_color(ui, UI_COL_DEFAULT);
    if (ui->active_id == id || ui->hot_id == id) col = ui_adjust_color_additive(ui, col, 0, 8);
    ui_fill_rect(ui, (float)(x + left), (float)y, 1, h, ' ', col);
    return left;
}

// ===== scrollbar (shared impl for horizontal/vertical) =====
// handle_size  = fraction of total content currently visible (0..1)
// value        = scroll position (0..1), 0=top/left, 1=bottom/right
// x,y,h or w  = pixel rect in cell coords; the bar is always 1 cell thick
//
// Behaviour:
// - return 0 (no-op) if handle_size >= 1.0 (everything fits) or track < 3 cells
// - arrow buttons at each end (triangles) decrement/increment by a small step
// - click on track (not handle) jumps to that position
// - drag on handle uses delta-relative mode (identical to ui_slider)
static int ui_scroll_impl(UIContext *ui, float *value, int id, float handle_size,
                           int x, int y, int track_len, int vertical) {
    // Taken before the early-outs so a pending ui_disable_next() is consumed
    // even by a scrollbar that draws nothing this frame.
    int disabled = ui_item_disable_begin(ui);
    if (handle_size >= 1.0f || track_len < 3) { // need arrow + 1 track cell + arrow
        ui_item_disable_end(ui, disabled);
        return 0;
    }

    int changed = 0;

    // Layout: button (1 cell) | track (track_len-2 cells) | button (1 cell)
    int track_start = vertical ? (y + 1) : (x + 1);
    int track_cells = track_len - 2;

    // Handle size in cells (minimum 1)
    int handle_cells = (int)((float)track_cells * handle_size + 0.5f);
    if (handle_cells < 1) handle_cells = 1;
    if (handle_cells > track_cells) handle_cells = track_cells;
    int handle_max_off = track_cells - handle_cells;

    // Current handle offset (fractional for sub-cell precision)
    float handle_pos = *value * (float)handle_max_off;
    int   handle_off = (int)handle_pos;
    float handle_frac = handle_pos - (float)handle_off;
    if (handle_off < 0) { handle_off = 0; handle_frac = 0.0f; }
    if (handle_off > handle_max_off) { handle_off = handle_max_off; handle_frac = 0.0f; }

    // ---- per-axis helpers ----
    float mx, my;
    int btn1_hov, btn2_hov, track_hov, handle_hov, any_hov;
    int btn1_x, btn1_y, btn2_x, btn2_y;
    unsigned char ch1, ch2;

    if (vertical) {
        mx = ui->mouse_x; my = ui->mouse_y;
        btn1_x = x; btn1_y = y;
        btn2_x = x; btn2_y = y + track_len - 1;
        ch1 = UI_CHAR_TRIANGLE_UP;   ch2 = UI_CHAR_TRIANGLE_DOWN;

        btn1_hov = (my >= (float)y && my < (float)(y + 1));
        btn2_hov = (my >= (float)(y + track_len - 1) && my < (float)(y + track_len));
        track_hov = (my >= (float)track_start && my < (float)(track_start + track_cells));
    } else {
        mx = ui->mouse_x; my = ui->mouse_y;
        btn1_x = x; btn1_y = y;
        btn2_x = x + track_len - 1; btn2_y = y;
        ch1 = UI_CHAR_TRIANGLE_LEFT; ch2 = UI_CHAR_TRIANGLE_RIGHT;

        btn1_hov = (mx >= (float)x && mx < (float)(x + 1));
        btn2_hov = (mx >= (float)(x + track_len - 1) && mx < (float)(x + track_len));
        track_hov = (mx >= (float)track_start && mx < (float)(track_start + track_cells));
    }

    // Must also be within the 1-cell-thick strip
    if (vertical) {
        btn1_hov = btn1_hov && (mx >= (float)x && mx < (float)(x + 1));
        btn2_hov = btn2_hov && (mx >= (float)x && mx < (float)(x + 1));
        track_hov = track_hov && (mx >= (float)x && mx < (float)(x + 1));
    } else {
        btn1_hov = btn1_hov && (my >= (float)y && my < (float)(y + 1));
        btn2_hov = btn2_hov && (my >= (float)y && my < (float)(y + 1));
        track_hov = track_hov && (my >= (float)y && my < (float)(y + 1));
    }

    // Handle hover: mouse is within the handle sub-rect of track
    float mcoord  = vertical ? my : mx;
    float hstart  = (float)(track_start + handle_off) + handle_frac;
    float hend    = hstart + (float)handle_cells;
    handle_hov = track_hov && (mcoord >= hstart && mcoord < hend);

    any_hov = (btn1_hov || btn2_hov || track_hov) && (ui->active_id == 0);
    if (any_hov) ui->hot_id = id;

    // ---- button clicks (step) ----
    float step = 0.05f;
    if (ui->mouse_flags & UI_FLAGS_SHIFT) step *= 0.2f;

    if (ui->active_id == 0 && btn1_hov && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        *value -= step;
        if (*value < 0.0f) *value = 0.0f;
        changed = 1;
    }
    if (ui->active_id == 0 && btn2_hov && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        *value += step;
        if (*value > 1.0f) *value = 1.0f;
        changed = 1;
    }

    // ---- track/background click (jump) vs handle drag start ----
    if (ui->active_id == 0 && track_hov && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        if (handle_hov) {
            // Drag start: anchor like ui_slider
            ui_set_active_control(ui, id, NULL);
            ui->scrollbar_anchor_value = *value;
            ui->scrollbar_anchor_mouse = mcoord;
        } else {
            // Jump to click position (centre handle on click)
            float half_h = (float)handle_cells * 0.5f;
            float raw_t  = (mcoord - (float)track_start - half_h)
                           / (float)(track_cells - handle_cells);
            if (track_cells == handle_cells) raw_t = 0.0f;
            if (raw_t < 0.0f) raw_t = 0.0f;
            if (raw_t > 1.0f) raw_t = 1.0f;
            *value = raw_t;
            changed = 1;
        }
    }

    // ---- handle drag (delta-relative, same as ui_slider) ----
    if (ui->active_id == id && ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) {
        float delta = (mcoord - ui->scrollbar_anchor_mouse)
                      / (float)(track_cells - handle_cells);
        if (track_cells == handle_cells) delta = 0.0f;
        float new_val = ui->scrollbar_anchor_value + delta;
        if (new_val < 0.0f) new_val = 0.0f;
        if (new_val > 1.0f) new_val = 1.0f;
        if (new_val != *value) { *value = new_val; changed = 1; }
    }
    if (ui->active_id == id && !ui->mouse_down[UI_MOUSE_BUTTON_LEFT])
        ui_set_active_control(ui, 0, NULL);

    // ---- render ----
    // NOT UI_COL_DIMMED: its background (GRAY1, 51) sits between the track's
    // GRAY0 (25) and the textarea's own 0x21 (33) -- three near-identical
    // darks, leaving the handle effectively invisible. The button colours are
    // the palette's "raised, mouse target" steps and give the handle real
    // contrast plus hover/drag feedback.
    unsigned int track_bg  = ui_theme_color(ui, UI_COL_DEFAULT);
    unsigned int handle_bg = ui_theme_color(ui, UI_COL_BUTTON);
    if (handle_hov && ui->mouse_down[UI_MOUSE_BUTTON_LEFT] && ui->active_id == id)
        handle_bg = ui_theme_color(ui, UI_COL_BUTTON_ACT);
    else if (handle_hov || (ui->active_id == id && !btn1_hov && !btn2_hov))
        handle_bg = ui_theme_color(ui, UI_COL_BUTTON_HOVER);

    // Button 1
    unsigned int b1c = ui_theme_color(ui, UI_COL_BUTTON);
    if (btn1_hov && ui->mouse_down[UI_MOUSE_BUTTON_LEFT] && ui->active_id == id)
        b1c = ui_theme_color(ui, UI_COL_BUTTON_ACT);
    else if (btn1_hov)
        b1c = ui_theme_color(ui, UI_COL_BUTTON_HOVER);
    ui_draw_cell(ui, (float)btn1_x, (float)btn1_y, ch1, b1c);

    // Button 2
    unsigned int b2c = ui_theme_color(ui, UI_COL_BUTTON);
    if (btn2_hov && ui->mouse_down[UI_MOUSE_BUTTON_LEFT] && ui->active_id == id)
        b2c = ui_theme_color(ui, UI_COL_BUTTON_ACT);
    else if (btn2_hov)
        b2c = ui_theme_color(ui, UI_COL_BUTTON_HOVER);
    ui_draw_cell(ui, (float)btn2_x, (float)btn2_y, ch2, b2c);

    // Track + handle (handle cells use fractional offset for sub-cell precision)
    for (int i = 0; i < track_cells; i++) {
        int is_handle = (i >= handle_off && i < handle_off + handle_cells);

        float dynamic_pos = (float)(track_start + i) + (is_handle ? handle_frac : 0.0f);
        float fcx = vertical ? (float)x : dynamic_pos;
        float fcy = vertical ? dynamic_pos : (float)y;
        ui_draw_cell(ui, fcx, fcy, ' ', is_handle ? handle_bg : track_bg);
    }

    ui_item_disable_end(ui, disabled);
    return changed;
}

int ui_vscroll(UIContext *ui, float *value, int id, float handle_size, int x, int y, int h) {
    return ui_scroll_impl(ui, value, id, handle_size, x, y, h, 1);
}

int ui_hscroll(UIContext *ui, float *value, int id, float handle_size, int x, int y, int w) {
    return ui_scroll_impl(ui, value, id, handle_size, x, y, w, 0);
}

// ===== dynamic components =====
static UIControl *ui_add_component(UIContext *ui, int type, float x, float y, const char *text) {
    UIControl c;
    ui->sys->memset(&c, 0, sizeof(c));
    c.component_type = type;
    c.x = x; c.y = y; c.w = 0; c.h = 1;
    s_strncpy(c.text, text, sizeof(c.text));
    array_push(&ui->components, &c, ui->sys);
    return (UIControl *)array_get(&ui->components, ui->components.size - 1);
}

UIControl *ui_add_button(UIContext *ui, float x, float y, const char *text,
                         void (*cb)(UIContext *, int, void *), int id, void *data) {
    UIControl *c = ui_add_component(ui, UI_COMPONENT_BUTTON, x, y, text);
    c->action_callback = cb;
    c->action_id = id;
    c->action_data = data;
    return c;
}

UIControl *ui_add_checkbox(UIContext *ui, float x, float y, const char *text, int value) {
    UIControl *c = ui_add_component(ui, UI_COMPONENT_CHECKBOX, x, y, text);
    c->value.i = value;
    return c;
}

UIControl *ui_add_slider(UIContext *ui, float x, float y, const char *text,
                         float value, float min, float middle, float max) {
    UIControl *c = ui_add_component(ui, UI_COMPONENT_SLIDER, x, y, text);
    c->value.f = value;
    c->data.slider.min = min;
    c->data.slider.middle = middle;
    c->data.slider.max = max;
    return c;
}

UIControl *ui_add_radio(UIContext *ui, float x, float y, const char *text,
                        int *value, int which) {
    UIControl *c = ui_add_component(ui, UI_COMPONENT_RADIO, x, y, text);
    c->data.radio_button.which = which;
    c->data.radio_button.value = value;
    c->value.i = value ? *value : which; // standalone: start on
    return c;
}

UIControl *ui_add_label(UIContext *ui, float x, float y, const char *text) {
    return ui_add_component(ui, UI_COMPONENT_LABEL, x, y, text);
}

UIControl *ui_add_dropdown(UIContext *ui, float x, float y, const char *text,
                           int *value, int num_items, const char **items,
                           int flags, float w) {
    UIControl *c = ui_add_component(ui, UI_COMPONENT_DROPDOWN, x, y, text);
    c->data.dropdown.value = value;
    c->data.dropdown.items = items;
    c->data.dropdown.count = num_items;
    c->data.dropdown.flags = flags;
    c->value.i = value ? *value : 0;
    c->w = w; // 0 = size to the content, like an unpushed ui_dropdown
    return c;
}

void ui_set_component_text(UIControl *c, const char *text) {
    if (c) s_strncpy(c->text, text, sizeof(c->text));
}

// The dropdown's natural width, shared by the widget and by the dynamic
// component's hit box / drag highlight so the two can't drift apart.
static float ui_dropdown_autow(const char *text, const char *cur_value, int flags) {
    int has_name = !(flags & UI_DROPDOWN_HIDE_NAME);
    int has_sel  = !(flags & UI_DROPDOWN_HIDE_SELECTION);
    int has_tri  = !(flags & UI_DROPDOWN_HIDE_TRIANGLE);
    int show_sep = has_name && has_sel && !(flags & UI_DROPDOWN_HIDE_SEPARATOR);

    float autow = 1.0f; // left padding
    if (has_name)  autow += (float)ui_display_len(text);
    if (show_sep)  autow += 2.0f;  // ": "
    if (has_sel && cur_value) autow += (float)s_strlen(cur_value);
    if (has_tri)   autow += 2.0f;  // space + triangle
    return autow < 8.0f ? 8.0f : autow;
}

static float ui_component_width(UIControl *c) {
    // Display length, matching what the widgets themselves measure: a "##suffix"
    // is hashed into the id, never drawn -- counting it here would stretch the
    // hit box and the drag highlight past the cells the widget actually paints.
    int len = ui_display_len(c->text);
    switch (c->component_type) {
    case UI_COMPONENT_BUTTON:   return (float)(len + 2);
    case UI_COMPONENT_CHECKBOX: return (float)(len + 4);
    case UI_COMPONENT_RADIO:    return (float)(len == 0 ? 2 : len + 3); // == ui_radio_button
    // label + a track wide enough to read, the way a pushed item width sizes
    // the static sliders; 0 keeps the caller from having to pick one
    case UI_COMPONENT_SLIDER:   return c->w > 0.0f ? c->w : (float)(len + 16);
    case UI_COMPONENT_DROPDOWN: {
        if (c->w > 0.0f) return c->w;
        const char **items = c->data.dropdown.items;
        const char *cur = (const char *)0;
        if (items && c->data.dropdown.count > 0) {
            int v = c->data.dropdown.value ? *c->data.dropdown.value : c->value.i;
            if (v < 0) v = 0;
            if (v >= c->data.dropdown.count) v = c->data.dropdown.count - 1;
            cur = items[v];
        }
        return ui_dropdown_autow(c->text, cur, c->data.dropdown.flags);
    }
    default:                    return (float)len;
    }
}

static UIControl *ui_add_overlay_control(UIContext *ui, int type,
                                          float x, float y, const char *text) {
    UIControl c;
    ui->sys->memset(&c, 0, sizeof(c));
    c.component_type = type;
    c.x = x; c.y = y;
    s_strncpy(c.text, text, sizeof(c.text));
    array_push(&ui->overlay_components, &c, ui->sys);
    return (UIControl *)array_get(&ui->overlay_components,
                                   ui->overlay_components.size - 1);
}

// ===== per-frame =====

// is_overlay=1 : overlay pass -- use ui_hit_raw, claim cells via CELL_FLAGS_DRAWN, no drag
// is_overlay=0 : regular dynamic pass -- ui_hit (respects overlay), drag support
static void ui_process_components_internal(UIContext *ui, Array *arr, int is_overlay) {
    for (size_t i = 0; i < arr->size; i++) {
        UIControl *c = (UIControl *)array_get(arr, i);
        float fx = c->x, fy = c->y;
        int iw = (int)c->w;
        float cw = is_overlay ? c->w : ui_component_width(c);

        int draggable = 0;
        if (!is_overlay) {
            int comp_id = 100000 + (int)i;
            draggable = ui->edit_mode_enabled && !(c->flags & UI_C_FLAGS_EDITING_DISABLED);
            if (draggable) {
                int hov = ui_hit(ui, c->x, c->y, cw, c->h > 0.0f ? c->h : 1.0f);
                // Initiate drag on press
                if (hov && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
                    ui_set_active_control(ui, comp_id, NULL);
                    c->flags |= UI_C_FLAGS_BEING_DRAGGED;
                }
                // Update/release: check flag directly since the widget call below
// may overwrite ui->active_id with its own widget_seq id
                if (c->flags & UI_C_FLAGS_BEING_DRAGGED) {
                    if (ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) {
                        c->x += ui->mouse_dx; c->y += ui->mouse_dy;
                        if (c->x < 0) c->x = 0;
                        if (c->y < 0) c->y = 0;
                    } else {
                        c->flags &= ~UI_C_FLAGS_BEING_DRAGGED;
                        ui_set_active_control(ui, 0, NULL);
                    }
                }
            }
        }

        ui_set_cursor(ui, c->x, c->y);

        // Suppress widget interaction (press/release) while being dragged,
// so the widget does not toggle state, capture active_id, or draw
// itself as "active" (blue) -- the drag highlight handles the tint.
        int saved_mouse_pressed[3], saved_mouse_released[3];
        int drag_suppress = !is_overlay && (c->flags & UI_C_FLAGS_BEING_DRAGGED);
        if (drag_suppress) {
            for (int b = 0; b < 3; b++) {
                saved_mouse_pressed[b] = ui->mouse_pressed[b];
                saved_mouse_released[b] = ui->mouse_released[b];
                ui->mouse_pressed[b] = 0;
                ui->mouse_released[b] = 0;
            }
        }

        // A dynamic component carries its own disabled state; the widget call
        // below is an ordinary widget call, so a scope is all it takes. It goes
        // inside the drag-suppress pair on purpose -- opening it first would
        // make that pair save the blanked events and restore them afterwards.
        int comp_disabled = (c->flags & UI_C_FLAGS_DISABLED) != 0;
        if (comp_disabled) ui_begin_disabled(ui);

        // Own id space per component, so two dynamic controls with the same
        // label -- or a dynamic one sharing a label with an immediate-mode
        // widget elsewhere in the frame -- stay independent. Overlay items are
        // excluded: they carry the widget_id their popup body hashed.
        if (!is_overlay) ui_push_id_num(ui, (unsigned int)i);

        switch (c->component_type) {
        case UI_COMPONENT_BUTTON:
            if (ui_button(ui, c->text) && !draggable && c->action_callback)
                c->action_callback(ui, c->action_id, c->action_data);
            break;
        case UI_COMPONENT_CHECKBOX:
            ui_check_box(ui, c->text, (bool *)&c->value.i);
            break;
        case UI_COMPONENT_SLIDER:
            // Same reason as the dropdown below: an unpushed ui_slider stretches
            // to the content region, which would not match the width the hit box
            // and the drag highlight use.
            ui_push_item_width(ui, cw);
            ui_slider(ui, c->text, &c->value.f,
                      c->data.slider.min, c->data.slider.middle, c->data.slider.max);
            ui_pop_item_width(ui);
            break;
        case UI_COMPONENT_RADIO: {
            int *v = c->data.radio_button.value ? c->data.radio_button.value
                                                : &c->value.i;
            ui_radio_button(ui, c->text, v, c->data.radio_button.which);
            break;
        }
        case UI_COMPONENT_LABEL:
            ui_label(ui, c->text);
            break;
        case UI_COMPONENT_DROPDOWN: {
            int *v = c->data.dropdown.value ? c->data.dropdown.value : &c->value.i;
            // An explicit width has to be pushed, or the widget would size
            // itself to its content and stop matching cw (the hit box and the
            // drag highlight); with w == 0 both sides use ui_dropdown_autow.
            if (c->w > 0.0f) ui_push_item_width(ui, c->w);
            if (c->data.dropdown.items && c->data.dropdown.count > 0)
                ui_dropdown(ui, c->text, v, c->data.dropdown.count,
                            c->data.dropdown.items, c->data.dropdown.flags);
            if (c->w > 0.0f) ui_pop_item_width(ui);
            break;
        }

        case UI_COMPONENT_POPUP_BG: {
            // fill without claiming cells so items above can draw over them
            int ih = (int)c->h;
            int iix = (int)(fx + 0.5f), iiy = (int)(fy + 0.5f);
            for (int r = 0; r < ih; r++)
                for (int cc = 0; cc < iw; cc++)
                    ui_screen_set_cell(ui, iix + cc, iiy + r, ' ',
                        ui_theme_color(ui, UI_COL_DEFAULT));
            // consume input for the whole popup area
            if (ui_hit_raw(ui, c->x, c->y, c->w, c->h))
                ui->overlay_consumed_input = 1;
            break;
        }
        case UI_COMPONENT_SELECTABLE:
        case UI_COMPONENT_MENU_ITEM:
        case UI_COMPONENT_MENU_TOGGLE: {
            int item_disabled = (c->flags & UI_C_FLAGS_DISABLED) != 0;
            // A disabled item still swallows the click -- it is part of the
            // popup, and letting it fall through to whatever sits underneath
            // would be worse than doing nothing.
            int over = ui_hit_raw(ui, c->x, c->y, c->w, 1.0f);
            if (over) ui->overlay_consumed_input = 1;
            int hov = over && !item_disabled;

            unsigned int item_col;
            if (c->component_type == UI_COMPONENT_SELECTABLE) {
                // A dropdown list is painted as one blue block (see
                // ui_popup_row_color); menu popups keep the plain look below.
                item_col = ui_popup_row_color(ui, c->value.i != 0, hov);
            } else {
                item_col = hov ? ui_theme_color(ui, UI_COL_BUTTON_HOVER)
                               : ui_theme_color(ui, UI_COL_DEFAULT);
            }
            if (item_disabled)
                item_col = ui_dim_color(ui, item_col);

            // gutter col 1: checkmark for toggle, blank for others
            float text_x = fx + 2.f;
            if (c->component_type == UI_COMPONENT_MENU_TOGGLE) {
                unsigned char mark = c->value.i ? UI_CHAR_CHECKBOX_MIDDLE_ENABLED
                                                : UI_CHAR_CHECKBOX_MIDDLE_DISABLED;
                ui_draw_cell(ui, fx + 1.f, fy, mark, item_col);
            }
            // display text, stopping at ##
            for (int k = 0; c->text[k]; k++) {
                if (c->text[k] == '#' && c->text[k + 1] == '#') break;
                ui_draw_cell(ui, text_x + (float)k, fy, (unsigned char)c->text[k], item_col);
            }
            // fill padding around already-drawn content
            for (int cc = 0; cc < iw; cc++)
                ui_draw_cell(ui, fx + (float)cc, fy, ' ', item_col);

            if (hov && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
                ui->overlay_activated_id = c->widget_id;
                // NOTE: do NOT clear popup_open_id here. The popup is still
// logically open this frame so ui_dropdown_begin() returns 1
// and the user's ui_selectable() call can detect the activation.
// The selectable/menu functions close the popup themselves.
                ui_set_active_control(ui, 0, NULL); // release any widget that held capture
            }
            break;
        }
        default: break;
        }

        if (!is_overlay) ui_pop_id(ui);

        if (comp_disabled) ui_end_disabled(ui);

        // restore suppressed input events
        if (drag_suppress) {
            for (int b = 0; b < 3; b++) {
                ui->mouse_pressed[b] = saved_mouse_pressed[b];
                ui->mouse_released[b] = saved_mouse_released[b];
            }
        }

        // drag highlight -- tint bg only, preserve char/scale; raw write skips CELL_FLAGS_DRAWN
        if (!is_overlay && draggable && (c->flags & UI_C_FLAGS_BEING_DRAGGED)) {
            int base_col = (int)(c->x + 0.5f), base_row = (int)(c->y + 0.5f);
            for (int k = 0; k < (int)cw; k++) {
                int col = base_col + k, row = base_row;
                // One step above UI_COL_DRAGGABLE_HOVER's background, so a
                // widget actually being dragged still reads as hotter than one
                // merely hovered. Text keeps UI_COL_DEFAULT's colour and hue --
                // only the background is tinted.
                unsigned int def  = ui_theme_color(ui, UI_COL_DEFAULT);
                unsigned int drag = ui_adjust_color_additive(ui, 
                    ui_theme_color(ui, UI_COL_DRAGGABLE_HOVER), 0, 19);
                if (col >= 0 && col < ui->cols && row >= 0 && row < ui->rows)
                    ui->screen[row * ui->cols + col].color =
                        TM_COLOR_PACK(TM_COLOR_FG(def), TM_COLOR_GRADIENT(def),
                                      TM_COLOR_BG(drag));
            }
        }
    }
}

void ui_begin(UIContext *ui, float time_in_seconds) {
    ui->time = time_in_seconds;
    ui->mouse_dx = ui->mouse_x - ui->prev_mouse_x;
    ui->mouse_dy = ui->mouse_y - ui->prev_mouse_y;
    ui->hot_id = 0;
    ui->last_widget_activated = 0;
    ui->last_widget_active = 0;
    ui->last_widget_deactivated = 0;
    ui->id_stack_sp = 0;
    ui_clip_reset(ui); // full screen again, whatever last frame left pushed
    ui->line_start_x = 0.0f;
    ui->content_max_x = (float)ui->cols;
    ui->pen_x = 0.0f;
    ui->pen_y = 0.0f;
    ui->last_x = ui->last_y = 0.0f;
    ui->last_w = ui->last_h = 0.0f;

    // clear screen; flags=0 resets CELL_FLAGS_DRAWN so overlay can reclaim cells
    for (int i = 0; i < ui->cols * ui->rows; i++) {
        OutputCell *c = &ui->screen[i];
        c->ch = ' '; c->color = ui_theme_color(ui, UI_COL_DEFAULT);
        c->scale = 1.0f; c->weight = 1.0f; c->offset_x = 0.0f; c->offset_y = 0.0f;
        c->flags = 0;
    }

    // draw overlay first -- these cells are claimed before any other drawing
    ui->overlay_activated_id   = 0;
    ui->overlay_consumed_input = 0;
    ui_process_components_internal(ui, &ui->overlay_components, 1);

    // A press on a popup belongs to the popup. ui_hit already refuses it, but
    // raw hit-testers underneath (the textarea) would still take it and turn
    // the hand's drift, while the button is still down, into a selection.
    if (ui->overlay_consumed_input)
        for (int b = 0; b < 3; b++) ui_consume_mouse_press(ui, b);

    // discard overlay list; this frame's Begin/selectable calls will rebuild it
    ui->overlay_components.size = 0;

    // reset popup-building state for this frame
    ui->in_popup = 0;
    ui->popup_bg_component_idx = -1;
}

int ui_update_dynamic(UIContext *ui) {
    ui_process_components_internal(ui, &ui->components, 0);
    return 0;
}

void ui_end(UIContext *ui) {
    // An unbalanced ui_begin_disabled (or a ui_disable_next with no control
    // after it) must not leak into the next frame -- and the parked input has
    // to come back before the bookkeeping below latches prev_mouse_* from it.
    while (ui->disabled_depth > 0) ui_end_disabled(ui);
    ui->disable_next = false;

    // fix active_id when a control is removed while dragging
    if (ui->active_id != 0 && !ui->mouse_down[ui->active_button]) {
        ui_set_active_control(ui, 0, NULL);
    }

    ui->mouse_pressed[0] = ui->mouse_pressed[1] = ui->mouse_pressed[2] = 0;
    ui->mouse_released[0] = ui->mouse_released[1] = ui->mouse_released[2] = 0;
    ui->wheel = 0.0f;

    ui->prev_mouse_x = ui->mouse_x;
    ui->prev_mouse_y = ui->mouse_y;    
    ui->prev_mouse_flags = ui->mouse_flags;

    ui->num_key_events = 0;
    ui->num_char_events = 0;
}



// ===== popup / overlay API =====

void ui_open_popup(UIContext *ui, const char *str_id) {
    unsigned int id = ui_make_id(ui, str_id);
    ui->popup_open_id  = (ui->popup_open_id == id) ? 0 : id;
    ui->popup_spawn_x  = ui->mouse_x;
    ui->popup_spawn_y  = ui->mouse_y;
}

int ui_context_popup_begin(UIContext *ui, const char *str_id) {
    unsigned int id = ui_make_id(ui, str_id);
    if (ui->popup_open_id != id) return 0;
    // click outside any overlay area closes the popup
    if (ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT] && !ui->overlay_consumed_input) {
        ui->popup_open_id = 0;
        return 0;
    }
    ui->popup_pen_x = ui->popup_spawn_x;
    ui->popup_pen_y = ui->popup_spawn_y;
    ui->popup_w     = 16.0f;
    ui->in_popup    = 1;
    ui->popup_bg_component_idx = (int)ui->overlay_components.size;
    UIControl *bg = ui_add_overlay_control(ui, UI_COMPONENT_POPUP_BG,
                                            ui->popup_pen_x, ui->popup_pen_y, "");
    bg->w = ui->popup_w; bg->h = 0.0f;
    return 1;
}

void ui_context_popup_end(UIContext *ui) {
    if (!ui->in_popup) return;
    if (ui->popup_bg_component_idx >= 0) {
        UIControl *bg = (UIControl *)array_get(&ui->overlay_components,
                                               (size_t)ui->popup_bg_component_idx);
        if (bg) { bg->h = ui->popup_pen_y - bg->y; bg->w = ui->popup_w; }
        for (size_t i = (size_t)ui->popup_bg_component_idx + 1;
             i < ui->overlay_components.size; i++)
            ((UIControl *)array_get(&ui->overlay_components, i))->w = ui->popup_w;
    }
    if (ui->popup_open_id == 0 && ui->popup_bg_component_idx >= 0)
        ui->overlay_components.size = (size_t)ui->popup_bg_component_idx;
    ui->in_popup = 0;
}

int ui_dropdown_begin(UIContext *ui, const char *text, const char *cur_value, int flags) {
    int disabled = ui_item_disable_begin(ui);
    unsigned int id;
    int label_len = ui_label_and_id(ui, text, &id);
    // A popup that was open when the control got disabled would otherwise stay
    // open and clickable -- its items live in the overlay list, which is drawn
    // and hit-tested next frame, outside this scope.
    if (disabled && ui->popup_open_id == id) ui->popup_open_id = 0;

    float x = ui->pen_x, y = ui->pen_y;

    int has_name = !(flags & UI_DROPDOWN_HIDE_NAME);
    int has_sel  = !(flags & UI_DROPDOWN_HIDE_SELECTION);
    int has_tri  = !(flags & UI_DROPDOWN_HIDE_TRIANGLE);
    int show_sep = has_name && has_sel && !(flags & UI_DROPDOWN_HIDE_SEPARATOR);

    float w = ui_take_width(ui, ui_dropdown_autow(text, cur_value, flags));
    int iw = (int)w;

    int is_open = (ui->popup_open_id == id);
    int hovered = ui_hit(ui, x, y, w, 1.0f);
    if (hovered) ui->hot_id = (int)(id & 0x7fffffff);

    // close on click outside (not on header, not on an overlay item)
    if (is_open && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]
        && !ui->overlay_consumed_input && !hovered) {
        ui->popup_open_id = 0;
        is_open = 0;
    }
    // toggle on header click
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        ui->popup_open_id = is_open ? 0 : id;
        is_open = !is_open;
    }

    // draw header

    unsigned int col = ui_theme_color(ui, UI_COL_DEFAULT);
    unsigned int tri_col;
    if (is_open || hovered) {
        col = TM_COLOR_PACK(TM_COLOR_FG(ui_theme_color(ui, UI_COL_COLD_ACCENT)),
                            TM_COLOR_GRADIENT(ui_theme_color(ui, UI_COL_COLD_ACCENT)),
                            TM_COLOR_BG(ui_theme_color(ui, UI_COL_DIMMED_BG)));
        tri_col = is_open ? TM_COLOR_FADE_F(col, 0.5f) : col;
    } else {
        tri_col = TM_COLOR_FADE_F(col, 0.5f);
    }

    float cx = x + 1.f;
    // The name and its ":" are a label, not part of the value -- they take
    // UI_COL_LABEL while the control rests. Hover/open still recolours the
    // whole header, so the accent doesn't arrive split in two.
    unsigned int name_col = (is_open || hovered) ? col : ui_label_color_on(ui, col);
    // Text stops one cell short of the box when there's a triangle, so the
    // triangle always has a cell to land in.
    float header_end = (float)((int)x + iw - (has_tri ? 1 : 0));
    if (has_name) {
        for (int k = 0; k < label_len && text[k] && cx < header_end; k++)
            ui_draw_cell(ui, cx++, y, (unsigned char)text[k], name_col);
    }
    if (show_sep && cx < header_end) ui_draw_cell(ui, cx++, y, ':', name_col);
    if (show_sep && cx < header_end) ui_draw_cell(ui, cx++, y, ' ', name_col);
    if (has_sel && cur_value) {
        for (int k = 0; cur_value[k] && cx < header_end; k++)
            ui_draw_cell(ui, cx++, y, (unsigned char)cur_value[k], col);
    }
    if (has_tri) {
        // Tight against the selection text, not pinned to the right edge: the
        // triangle belongs to the value it opens, so any slack in the box (a
        // pushed item width, or the 8-cell minimum) reads as trailing padding
        // rather than as a gap splitting the header in two.
        float tri_x = cx;
        float tri_max = (float)((int)x + iw - 1);
        if (tri_x > tri_max) tri_x = tri_max;
        ui_draw_cell(ui, tri_x, y, UI_CHAR_TRIANGLE_DOWN, tri_col);
    }
    ui_fill_rect(ui, x, y, iw, 1, ' ', col); // fill padding around already-drawn content

    ui_advance(ui, x, y, w, 1.0f);

    if (is_open) {
        ui->popup_pen_x = x;
        ui->popup_pen_y = y + 1.0f;
        ui->popup_w     = w;
        ui->in_popup    = 1;
        ui->popup_bg_component_idx = (int)ui->overlay_components.size;
        UIControl *bgc = ui_add_overlay_control(ui, UI_COMPONENT_POPUP_BG,
                                                 x, y + 1.0f, "");
        bgc->w = w; bgc->h = 0.0f;
    }
    // Closed before the caller's ui_selectable() calls run, so the popup body
    // is built (and dimmed) by its own scope, not by the header's.
    ui_item_disable_end(ui, disabled);
    return is_open;
}

void ui_dropdown_end(UIContext *ui) {
    if (!ui->in_popup) return;
    if (ui->popup_bg_component_idx >= 0) {
        UIControl *bg = (UIControl *)array_get(&ui->overlay_components,
                                               (size_t)ui->popup_bg_component_idx);
        if (bg) { bg->h = ui->popup_pen_y - bg->y; bg->w = ui->popup_w; }
        // backfill final width into all item rows
        for (size_t i = (size_t)ui->popup_bg_component_idx + 1;
             i < ui->overlay_components.size; i++)
            ((UIControl *)array_get(&ui->overlay_components, i))->w = ui->popup_w;
    }
    // an item fired this frame (selectable closed popup_open_id) -- discard the
// overlay items we just built so the popup vanishes immediately, not next frame
    if (ui->popup_open_id == 0 && ui->popup_bg_component_idx >= 0)
        ui->overlay_components.size = (size_t)ui->popup_bg_component_idx;
    ui->in_popup = 0;
}

// convenience wrapper: dropdown that binds to an int index
bool ui_dropdown(UIContext *ui, const char *text, int *value, int num_items, const char *items[], int flags) {
    bool changed = false;
    if (ui_dropdown_begin(ui, text, items[*value], flags)) {
        for (int i = 0; i < num_items; i++) {
            if (ui_selectable(ui, items[i], *value == i)) {
                *value = i;
                changed = true;
            }
        }
        ui_dropdown_end(ui);
    }
    return changed;
}

// shared registration for selectable / menu_item / menu_toggle
static UIControl *ui_register_popup_item(UIContext *ui, int type,
                                          const char *text, unsigned int id, int value) {
    if (!ui->in_popup) return (UIControl *)0;
    // grow popup_w to fit this item's display text
    int disp = 0;
    while (text[disp] && !(text[disp] == '#' && text[disp + 1] == '#')) disp++;
    int prefix = (type == UI_COMPONENT_MENU_TOGGLE) ? 3 : 2; // gutter + leading space
    float needed = (float)(disp + prefix + 1);
    if (needed > ui->popup_w) ui->popup_w = needed;

    UIControl *c = ui_add_overlay_control(ui, type,
                                           ui->popup_pen_x, ui->popup_pen_y, text);
    c->widget_id = id;
    c->value.i   = value;
    c->w         = ui->popup_w; // _end will backfill if a later item is wider
    // Input blanking can't reach this one: the item is drawn and hit-tested by
    // ui_begin() NEXT frame, long after the scope closed. Carry the state on
    // the control instead.
    if (ui_is_disabled(ui)) c->flags |= UI_C_FLAGS_DISABLED;
    ui->popup_pen_y += 1.0f;
    return c;
}

bool ui_selectable(UIContext *ui, const char *text, bool is_selected) {
    int disabled = ui_item_disable_begin(ui);
    unsigned int id;
    ui_label_and_id(ui, text, &id);
    ui_register_popup_item(ui, UI_COMPONENT_SELECTABLE, text, id, is_selected);
    bool activated = !disabled && (ui->overlay_activated_id == id);
    if (activated) ui->popup_open_id = 0; // close popup; _end will discard overlay
    ui_item_disable_end(ui, disabled);
    return activated;
}

bool ui_menu_item(UIContext *ui, const char *text) {
    int disabled = ui_item_disable_begin(ui);
    unsigned int id;
    ui_label_and_id(ui, text, &id);
    ui_register_popup_item(ui, UI_COMPONENT_MENU_ITEM, text, id, 0);
    bool activated = !disabled && (ui->overlay_activated_id == id);
    if (activated) ui->popup_open_id = 0;
    ui_item_disable_end(ui, disabled);
    return activated;
}

bool ui_menu_toggle(UIContext *ui, const char *text, bool *checked) {
    int disabled = ui_item_disable_begin(ui);
    unsigned int id;
    ui_label_and_id(ui, text, &id);
    ui_register_popup_item(ui, UI_COMPONENT_MENU_TOGGLE, text, id,
                            checked ? *checked : 0);
    bool fired = !disabled && (ui->overlay_activated_id == id);
    if (fired) { ui->popup_open_id = 0; if (checked) *checked = !(*checked); }
    ui_item_disable_end(ui, disabled);
    return fired;
}
bool ui_hovering_enabled(UIContext *ui) { return ui->active_id == 0; }

void ui_consume_mouse_press(UIContext *ui, int button) {
    if (button >= 0 && button < 3) ui->mouse_pressed[button] = 0;
}

// ===== os events =====
void ui_os_mouse_event(UIContext *ui, float x, float y, float wheel,
                       int type, int which_button, int flags) {
    ui->mouse_x = x / ui->cell_w;
    ui->mouse_y = y / ui->cell_h;
    ui->mouse_flags = flags;
                        
    if (type == UI_MOUSE_TYPE_WHEEL) {
        ui->wheel += wheel;
        return;
    }
    if (which_button < 0 || which_button > 2) return;
    if (type == UI_MOUSE_TYPE_DOWN) {
        ui->mouse_down[which_button] = 1;
        ui->mouse_pressed[which_button] = 1;
    } else if (type == UI_MOUSE_TYPE_UP) {
        ui->mouse_down[which_button] = 0;
        ui->mouse_released[which_button] = 1;
    }
}

void ui_os_key_event(UIContext *ui, int keycode, int is_down, int flags) {
    // Refresh the modifier bits from the key event so holding/releasing a
    // modifier is seen even when the mouse doesn't move (mouse_flags is
    // otherwise only updated by ui_os_mouse_event).  Non-modifier bits
    // (e.g. UI_FLAGS_DOUBLE_CLICK) are left alone.
    {
        int mods = UI_FLAGS_CTRL | UI_FLAGS_SHIFT | UI_FLAGS_ALT;
        ui->mouse_flags = (ui->mouse_flags & ~mods) | (flags & mods);
    }
    if (!is_down) return;
    if (ui->num_key_events < 64) {
        ui->key_events[ui->num_key_events] = keycode;
        ui->key_flags[ui->num_key_events] = flags;
        ui->num_key_events++;
    }
}

void ui_os_char_event(UIContext *ui, int ch, int flags) {
    if (ui->num_char_events < 64) {
        ui->char_events[ui->num_char_events] = ch;
        ui->char_flags[ui->num_char_events] = flags;
        ui->num_char_events++;
    }
}
