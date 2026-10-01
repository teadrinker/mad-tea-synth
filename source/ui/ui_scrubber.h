#ifndef UI_SCRUBBER_H
#define UI_SCRUBBER_H

#include "textmode_ui.h"
#include "common/math_pure.h"
#include "common/string_pure.h"

// ===== Number Scrubber =====
// A free-floating slot-machine overlay for adjusting numeric literals
// by dragging with the mouse.  Right-click a number in a textarea to
// start scrubbing, then drag to change its value.
//
// Wheel model: wheel[SW(k)] = abs(number) / 10^k   (power-of-10 index k)
// Continuous wheels (k <= drag_wheel_id) spin freely; odometer wheels
// (k > drag_wheel_id) only move via carry from the wheel below.
// All wheel values are pre-computed at mousedown and incrementally
// updated each frame.
//
// Usage:
//   // On right-click press:
//   float sx = ui->mouse_x + (float)(lSide - hc);
//   float sy = ui->mouse_y;
//   ui_scrubber_init(ui, &scrubber, number, postfix, sx, sy);
//
//   // Each frame while dragging:
//   if (ui_scrubber(ui, &scrubber)) {
//       node->internal_number = scrubber.number;  // value changed
//   }

#define SCRUBBER_MAX_WHEELS 32
#define SCRUBBER_WHEEL_BASE 16          // wheel[k + BASE] for k in [-16, 15]
#define SW(k) ((k) + SCRUBBER_WHEEL_BASE)
#define SCRUBBER_EPS 1e-9

#define UI_SCRUBBER_FLAGS_INTEGER 1    // render / format as integer (never showing dot or decimals, even if used internally)
#define UI_SCRUBBER_FLAGS_FORCE_DOT 2  // render / format 1 as 1.0
#define UI_SCRUBBER_FLAGS_WARM_RAMP 4  // draw the wheels on the warm colour ramp; default is the cold (blue) one

// Accent the wheels (and the idle hover state) are drawn in.
static inline unsigned int ui_scrubber_ramp_color(UIContext *ui, int flags) {
    return ui_theme_color(ui, (flags & UI_SCRUBBER_FLAGS_WARM_RAMP) ? UI_COL_WARM_ACCENT
                                                                   : UI_COL_COLD_ACCENT);
}

typedef struct {
    double  original_number;
    double  internal_number;
    int     flags;
    int     init_flags;          // flags as passed to ui_scrubber_init (before shift promotion)
    int     promoted_to_double;  // shift was pressed mid-drag: an INTEGER scrub became fractional
    float   x, y;
    float   anchor_screen_x, anchor_screen_y;
    double  sign;                // 1.0 or -1.0
    int     num_target_chars_excluding_minus_and_postfix;
    int     dot_char_offset;     // position of '.' within target (0-indexed from first digit)
    int     has_dot;             // true if a decimal point should be shown
    int     had_dot_at_init;     // original has_dot from source format
    int     init_decimals;       // fractional digits in the source literal

    // Wheel state — indexed by power-of-10: wheel[SW(k)] = abs(number) / 10^k
    int     drag_wheel_id;       // k of the dragged wheel
    double  decimal_mul;         // = pow(10, drag_wheel_id)
    double  wheel[SCRUBBER_MAX_WHEELS];
    double  prev_base_dist;      // previous frame's quantized drag distance

    // Per-wheel alignment offset captured at mousedown so that every visible
    // wheel starts at an exact integer (scroll == 0).  Subtracted from the
    // continuous reading each frame; constant for the whole drag so the
    // dragged wheel still rolls smoothly relative to its aligned start.
    //
    // When the sign flips the snap shifts magnitudes (e.g. 444→…→4→6→16),
    // changing every wheel's fractional part.  We keep two offset sets —
    // one for positive and one for negative — and swap between them so
    // the drums remain aligned on both sides of zero.
    double  wheel_offset[SCRUBBER_MAX_WHEELS];          // active set
    double  wheel_offset_pos[SCRUBBER_MAX_WHEELS];      // positive-phase offsets (saved from init)
    double  wheel_offset_neg[SCRUBBER_MAX_WHEELS];      // negative-phase offsets (saved on first neg entry)
    int     has_wheel_offset_neg;  // true once wheel_offset_neg has been computed
    int     disp_neg;            // show a minus sign / reverse wheels this frame

    int     vis_top_wheel;       // highest visible wheel id (e.g. 2 for hundreds)
    int     vis_bot_wheel;       // lowest visible wheel id (e.g. -3)

    char    postfix[8];
} UIScrubber;


static inline int sw_floor_mod10(double a) {
    int r = (int)(a - 10.0 * m_floor(a / 10.0));
    return r < 0 ? r + 10 : r;
}


// 10^m for m >= 0, computed iteratively (m_pow's exp/log is inaccurate).
static inline double sw_pow10(int m) {
    double r = 1.0;
    if (m > 99) m = 99; // 32 wheels -> max |k-dk| = 31; 99 is generous safety bound
    for (int i = 0; i < m; i++) r *= 10.0;
    return r;
}

// Continuous (un-floored) magnitude reading of wheel k:  au / 10^k.
static inline double sw_wheel_cont(double au, int k) {
    double c = au;
    int n = k > 0 ? k : -k;
    if (n > 99) n = 99; // 32 wheels -> max |k| = 16; 99 is generous safety bound
    if (k > 0) for (int i = 0; i < n; i++) c /= 10.0;
    else       for (int i = 0; i < n; i++) c *= 10.0;
    return c;
}

// Map a wheel's continuous reading to its displayed odometer position.
//   Free wheels (k <= dk) spin freely:            pos = c
//   Odometer wheels (k > dk) only roll via carry:  they stay centred on
//   floor(c) until the wheel below is going 9->0, i.e. until the continuous
//   reading enters the last 10^-(k-dk) of its digit cycle, then roll by 1.
static inline double sw_odometer_pos(double c, int k, int dk) {
    if (k <= dk) return c;
    int    m   = k - dk;
    double inv = sw_pow10(m);     // 10^m
    double thr = 1.0 - 1.0 / inv; // 1 - 10^-m  (start of carry window)
    double fc  = c - m_floor(c);
    double g   = (fc > thr) ? (fc - thr) * inv : 0.0; // carry progress 0..1
    return m_floor(c) + g;
}

// Given an already-resolved wheel position (odometer + alignment applied),
// compute the three visible digits (top/mid/bot) and sub-cell scroll.
// Sign-aware rounding: for positive numbers (gear_dir<0) exactly .5 rounds DOWN.
static inline void sw_digits_from_pos(double pos, double gear_dir,
                                      int *mid, int *top, int *bot, float *scroll) {
    double sp  = (gear_dir < 0.0) ? -pos : pos;
    double di  = m_floor(sp + 0.5);
    *scroll    = clamp((float)(sp - di), -0.49999f, 0.49999f);
    int    md  = sw_floor_mod10((gear_dir < 0.0) ? -di : di);
    int chsign = gear_dir < 0.0 ? 1 : -1;
    *mid = md;
    *top = sw_floor_mod10(md - chsign);
    *bot = sw_floor_mod10(md + chsign);
}

// Compute the three visible digits (top/mid/bot) and sub-cell scroll for a
// wheel, from its continuous magnitude reading.  Replicates the sign-aware
// rounding: for positive numbers (gear_dir<0) exactly .5 rounds DOWN.
static inline void sw_compute(double c, int k, int dk, double gear_dir,
                              int *mid, int *top, int *bot, float *scroll) {
    double pos = sw_odometer_pos(c, k, dk);
    double sp  = (gear_dir < 0.0) ? -pos : pos;
    double di  = m_floor(sp + 0.5);
    *scroll    = clamp((float)(sp - di), -0.49999f, 0.49999f);
    int    md  = sw_floor_mod10((gear_dir < 0.0) ? -di : di);
    int chsign = gear_dir < 0.0 ? 1 : -1;
    *mid = md;
    *top = sw_floor_mod10(md - chsign);
    *bot = sw_floor_mod10(md + chsign);
}


// Snap a value onto a decimal grid of `places` fractional digits.
//
// The scrubber is a decimal odometer, but it drives it with binary arithmetic,
// so the double it hands back is routinely a few ulp off the decimal the drums
// are showing -- and it is the drums the user is reading.  11119197.64 nudged
// one step on a fine wheel is 11119197.640000099; drop the top wheel and
// 11119197.64 - 1e7 lands 6 ulp under 1119197.64, because the same absolute
// error is ten times as many ulp at the smaller magnitude.  A formatter that
// round-trips faithfully then has to spell all of that out.  Rounding to the
// digits the odometer actually shows gives back the nearest double to what the
// user sees, so the committed literal reads "1119197.64".
//
// Left alone when the scaled value would leave the range where doubles hold
// every integer (2^53) -- there the snap could only make things worse.
static inline double sw_snap_decimal(double v, int places) {
    if (places < 0)  places = 0;
    if (places > 15) places = 15;
    double pw = 1.0;
    for (int i = 0; i < places; i++) pw *= 10.0;
    double scaled = v * pw;
    if (scaled > 9.0e15 || scaled < -9.0e15) return v;
    double r = (scaled < 0.0) ? -m_floor(-scaled + 0.5) : m_floor(scaled + 0.5);
    return r / pw;
}


// ===== Low-level: compute wheel positions from number + format =====
// Populates sc->wheel[], sc->vis_top_wheel, sc->vis_bot_wheel, and sc->internal_number.
// The caller must have already set: sign, dot_char_offset, has_dot,
// num_target_chars_excluding_minus_and_postfix.
// drag_wheel_id controls which wheel is considered "dragged" (affects
// odometer-vs-continuous split; for undragged numbers any value works).
static inline void ui_scrubber_calc_wheels(UIScrubber *sc, double number, int drag_wheel_id) {
    double au = m_fabs(number);

    // Clamp drag_wheel_id to valid wheel-array range so we never
    // read or write sc->wheel[] out of bounds (index 0..31).
    if (drag_wheel_id < -SCRUBBER_WHEEL_BASE)
        drag_wheel_id = -SCRUBBER_WHEEL_BASE;
    if (drag_wheel_id > SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE - 1)
        drag_wheel_id = SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE - 1;

    // Compute every wheel as an integer — no fractional part, so scroll starts
    // at zero.  Use iterative *10.0 / /10.0 with the literal constant (never
    // m_pow — its exp/log approximation is inaccurate even for 10^1).
    double val = au;
    sc->wheel[SW(0)] = m_floor(val + SCRUBBER_EPS);
    for (int k = -1; k >= -SCRUBBER_WHEEL_BASE; k--) {
        val *= 10.0;
        sc->wheel[SW(k)] = m_floor(val + SCRUBBER_EPS);
    }
    val = au;
    for (int k = 1; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
        val /= 10.0;
        sc->wheel[SW(k)] = m_floor(val + SCRUBBER_EPS);
    }

    sc->drag_wheel_id = drag_wheel_id;
    // Compute decimal_mul iteratively (avoid m_pow — its exp/log is inaccurate).
    // drag_wheel_id is now clamped so the loop count is bounded.
    sc->decimal_mul = 1.0;
    if (drag_wheel_id >= 0) for (int i = 0; i < drag_wheel_id; i++) sc->decimal_mul *= 10.0;
    else                    for (int i = 0; i < -drag_wheel_id; i++) sc->decimal_mul /= 10.0;

    // Recompute odometer wheels (k > drag_wheel_id) — keep them integer too.
    // Guard the starting index so we never read wheel[] below index 0.
    {
        int kstart = drag_wheel_id + 1;
        if (kstart < -SCRUBBER_WHEEL_BASE) kstart = -SCRUBBER_WHEEL_BASE;
        for (int k = kstart; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
            sc->wheel[SW(k)] = m_floor(sc->wheel[SW(k - 1)] / 10.0 + SCRUBBER_EPS);
        }
    }

    // Store the exact number as-is — do NOT reconstruct it from the
    // deepest wheel by iterative division by 10.0.  That chain accumulates
    // floating-point error, corrupts internal_number, and creates spurious
    // fractional digits that suddenly expand vis_bot_wheel ("lots of decimals").
    sc->internal_number = number;

    // Determine visible wheel range
    int n = sc->num_target_chars_excluding_minus_and_postfix;
    int dot = sc->dot_char_offset;
    int has_dot = sc->has_dot;

    int target_top = has_dot ? dot - 1 : n - 1;
    int target_bot = has_dot ? dot - n + 1 : 0;

    // `n` is the character count of the ORIGINAL token, so it only carries the
    // dot and the fractional digits when the source literal actually had them.
    // When has_dot gets turned on later -- by FORCE_DOT, by a shift promotion,
    // or by the "drag created decimals" rule in ui_scrubber() -- `dot - n + 1`
    // lands at or above the units wheel ("1f": n=1, dot=1 -> target_bot=1), the
    // extension loop below then breaks on wheel 0 and vis_bot_wheel never goes
    // negative.  has_dot is recomputed from vis_bot_wheel every frame, so that
    // fed straight back into has_dot=0 and the dot flickered on and off each
    // frame.  A dot always implies at least one fractional wheel.
    if (has_dot && target_bot > -1) target_bot = -1;
    if (target_top < 0) target_top = 0;

    int top = target_top;
    for (int k = target_top + 1; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
        double w = sc->wheel[SW(k)];
        if (sw_floor_mod10(m_floor(w + 0.5)) != 0 || (w - m_floor(w)) > 1e-12) top = k; else break;
    }
    // Shrink top when leading integer wheels are zero so they render as
    // spaces instead of '0'.  Never shrink past wheel 0: the digit
    // immediately left of the decimal point (or the units digit when
    // there is no dot) must stay visible as an anchor.
    while (top > 0) {
        double w = sc->wheel[SW(top)];
        if (sw_floor_mod10(m_floor(w + 0.5)) == 0 && (w - m_floor(w)) <= 1e-12) top--; else break;
    }
    int bot = target_bot;
    for (int k = target_bot - 1; k >= -SCRUBBER_WHEEL_BASE; k--) {
        if (sw_floor_mod10(m_floor(sc->wheel[SW(k)] + 0.5)) != 0) bot = k; else break;
    }
    sc->vis_top_wheel = top;
    sc->vis_bot_wheel = bot;
}


// ===== Low-level: convert wheel state to displayed text =====
// Writes the displayed number (minus if negative, digits, dot if has_dot,
// trailing fractional zeros) into dst.  No padding spaces.
// Returns number of characters written (excluding null terminator),
// or -1 if dst_size is too small.
static inline int ui_scrubber_get_as_text(const UIScrubber *sc, char *dst, int dst_size) {
    int pos = 0;

    // minus sign
    if (sc->sign < 0.0) {
        if (pos >= dst_size) return -1;
        dst[pos++] = '-';
    }

    int top = sc->vis_top_wheel;
    int bot = sc->vis_bot_wheel;
    int has_dot = sc->has_dot;

    if (sc->flags & UI_SCRUBBER_FLAGS_INTEGER) { has_dot = 0; if (bot < 0) bot = 0; }
    if (sc->flags & UI_SCRUBBER_FLAGS_FORCE_DOT) { has_dot = 1; if (bot > -1) bot = -1; }

    // integer part: wheels from top down to 0
    for (int k = top; k >= 0; k--) {
        if (pos >= dst_size) return -1;
        double w = sc->wheel[SW(k)];
        int digit = sw_floor_mod10(m_floor(w + SCRUBBER_EPS));
        dst[pos++] = (char)('0' + digit);
    }

    // dot (between integer and fractional wheels)
    if (has_dot) {
        if (pos >= dst_size) return -1;
        dst[pos++] = '.';
    }

    // fractional part: wheels from -1 down to bot
    for (int k = -1; k >= bot; k--) {
        if (pos >= dst_size) return -1;
        double w = sc->wheel[SW(k)];
        int digit = sw_floor_mod10(m_floor(w + SCRUBBER_EPS));
        dst[pos++] = (char)('0' + digit);
    }

    dst[pos] = '\0';
    return pos;
}


static inline double ui_scrubber_get_value(UIScrubber *sc) {
    return sc->flags & UI_SCRUBBER_FLAGS_INTEGER ? m_floor(sc->internal_number + 0.5) : sc->internal_number;
}


// ===== Low-level: draw a static character (minus sign, dot, postfix, spacer) =====
static inline void ui_scrubber_draw_char(UIContext *ui, int col, int row, int *draw_offset,
                                          char ch, unsigned int color, float weight) {
    float fx = (float)(col + *draw_offset);
    ui_draw_cell_flags_weight(ui, fx, (float)(row - 1), ' ',               color, CELL_FLAGS_CLIP_TOP,    weight);
    ui_draw_cell_flags_weight(ui, fx, (float)(row + 0), (unsigned char)ch, color, 0,                      weight);
    ui_draw_cell_flags_weight(ui, fx, (float)(row + 1), ' ',               color, CELL_FLAGS_CLIP_BOTTOM, weight);
    (*draw_offset)++;
}

// ===== Low-level: draw one digit column with sub-cell scroll =====
static inline void snapui_scrubber_draw_digit(UIContext *ui, int col, int row, int *draw_offset,
                                           unsigned int color, float weight,
                                           float scroll, int mid_digit, int top_digit, int bot_digit) {
    unsigned int c0 = TM_COLOR_FADE_F(color, m_fmaxf(0.0f, 1.0f - 0.6f * m_fabsf(-1.0f + scroll)));
    unsigned int c1 = TM_COLOR_FADE_F(color, m_fmaxf(0.0f, 1.0f - 0.6f * m_fabsf( 0.0f + scroll)));
    unsigned int c2 = TM_COLOR_FADE_F(color, m_fmaxf(0.0f, 1.0f - 0.6f * m_fabsf( 1.0f + scroll)));
    int cx = col + *draw_offset;
    ui_draw_cell_raw(ui, cx, row - 1, (unsigned char)('0' + top_digit), c0, 1.0f, weight, 0.0f, scroll, false, CELL_FLAGS_CLIP_TOP);
    ui_draw_cell_raw(ui, cx, row + 0, (unsigned char)('0' + mid_digit), c1, 1.0f, weight, 0.0f, scroll, false, 0);
    ui_draw_cell_raw(ui, cx, row + 1, (unsigned char)('0' + bot_digit), c2, 1.0f, weight, 0.0f, scroll, false, CELL_FLAGS_CLIP_BOTTOM);
    (*draw_offset)++;
}

// ===== Low-level: compute and draw one wheel column =====
static inline void ui_scrubber_draw_wheel(UIContext *ui, int col, int row, int *draw_offset,
                                           unsigned int color, float weight,
                                           double au, int k, int dk, double gear_dir,
                                           const double *wheel_offset) {
    int mid, top, bot; float scroll;
    double pos = sw_odometer_pos(sw_wheel_cont(au, k), k, dk) - wheel_offset[SW(k)];
    sw_digits_from_pos(pos, gear_dir, &mid, &top, &bot, &scroll);
    snapui_scrubber_draw_digit(ui, col, row, draw_offset, color, weight, scroll, mid, top, bot);
}

// ===== Low-level: draw a zero padding column =====
static inline void ui_scrubber_draw_pad(UIContext *ui, int col, int row, int *draw_offset,
                                         unsigned int color, float weight,
                                         int dk, double gear_dir) {
    int mid, top, bot; float scroll;
    sw_compute(0.0, dk, dk, gear_dir, &mid, &top, &bot, &scroll);
    snapui_scrubber_draw_digit(ui, col, row, draw_offset, color, weight, scroll, mid, top, bot);
}

// ===== Low-level: render the scrubber overlay =====
static inline void ui_scrubber_render(UIContext *ui, UIScrubber *sc) {
    int dk = sc->drag_wheel_id;
    // Reverse the wheels once the value is (display-)negative so the drag
    // direction stays consistent across the zero crossing.
    double gear_dir = sc->disp_neg ? 1.0 : -1.0;
    double au = m_fabs(sc->internal_number);

    int col = (int)m_floor(sc->x);
    int row = (int)m_floor(sc->y);
    float weight = ui->global_weight + 1.0f;
    // Colour ramp: cold (blue) by default, warm when the caller asks for it.
    // Read from init_flags -- shift promotion rewrites flags mid-drag, and the
    // ramp is a caller's styling choice, not part of the number's format.
    unsigned int scratch_col = ui_scrubber_ramp_color(ui, sc->init_flags);

    int has_minus = sc->disp_neg;
    int draw_offset = 0;


    int bot = sc->vis_bot_wheel;
    int has_dot = sc->has_dot;
    if (sc->flags & UI_SCRUBBER_FLAGS_INTEGER) { has_dot = 0; if (bot < 0) bot = 0; }
    if (sc->flags & UI_SCRUBBER_FLAGS_FORCE_DOT) { has_dot = 1; if (bot > -1) bot = -1; }

    int dot_screen_pos = has_dot ? sc->dot_char_offset : 999;

    int n = sc->num_target_chars_excluding_minus_and_postfix;
    int dot = sc->dot_char_offset;
    int target_top = has_dot ? dot - 1 : n - 1;
    int top = sc->vis_top_wheel;

    int dot_drawn = 0;
    int extra_space_for_minus = 0;

    // Pad left if top < target_top (fewer integer digits than target)
    if(top < target_top) {
        for (int k = target_top - has_minus; k > top; k--) {
            ui_scrubber_draw_char(ui, col, row, &draw_offset, ' ', scratch_col, weight);
        }        
    } else if(has_minus) { // we need extra space for minus
        extra_space_for_minus = 1;
        dot_screen_pos++; 
    }


    // Minus sign
    if (has_minus) {
        ui_scrubber_draw_char(ui, col, row, &draw_offset, '-', scratch_col, weight);
    }

    // Integer wheels (k >= 0), left of dot.  The dot always follows wheel 0 —
    // it is NOT pinned to dot_screen_pos, because the integer part can grow a
    // digit mid-drag (9 -> 10) and a pinned dot would render "1.0" for 10.0.
    // dot_screen_pos only sets the *idle* column; the left-padding above keeps
    // the dot there whenever there are fewer integer digits than the target.
    for (int k = top; k >= 0; k--) {
        ui_scrubber_draw_wheel(ui, col, row, &draw_offset, scratch_col, weight, au, k, dk, gear_dir, sc->wheel_offset);
        if (has_dot && !dot_drawn && k == 0) {
            ui_scrubber_draw_char(ui, col, row, &draw_offset, '.', scratch_col, weight);
            dot_drawn = 1;
        }
    }

    // Place dot between integer and fractional parts (if not already placed)
    if (has_dot && !dot_drawn) {
        while (draw_offset < dot_screen_pos) {
            ui_scrubber_draw_char(ui, col, row, &draw_offset, ' ', scratch_col, weight);
        }
        ui_scrubber_draw_char(ui, col, row, &draw_offset, '.', scratch_col, weight);
        dot_drawn = 1;
    }

    // Fractional wheels (k < 0), right of dot
    for (int k = -1; k >= bot; k--) {
        ui_scrubber_draw_wheel(ui, col, row, &draw_offset, scratch_col, weight, au, k, dk, gear_dir, sc->wheel_offset);
    }

    // Pad right if fewer fractional digits drawn than target
    while (draw_offset - extra_space_for_minus < n) {
        ui_scrubber_draw_pad(ui, col, row, &draw_offset, scratch_col, weight, dk, gear_dir);
    }

    // Postfix
    for (int j = 0; sc->postfix[j]; j++) {
        ui_scrubber_draw_char(ui, col, row, &draw_offset, sc->postfix[j], scratch_col, weight);
    }
}

static inline void ui_scrubber_init(UIContext *ui, UIScrubber *sc,
                                     double number, const char *postfix,
                                     int num_target_chars_excluding_postfix,
                                     float screen_x, float screen_y, int flags) {
    (void)ui;
    sc->original_number = number;
    sc->sign            = number < 0.0 ? -1.0 : 1.0;
    sc->x               = screen_x;
    sc->y               = screen_y;
    sc->flags           = flags;
    sc->init_flags      = flags;
    sc->promoted_to_double = 0;
    sc->anchor_screen_x = ui->mouse_x;
    sc->anchor_screen_y = ui->mouse_y;
    sc->num_target_chars_excluding_minus_and_postfix = num_target_chars_excluding_postfix + (number < 0.f ? -1 : 0);

    s_strncpy(sc->postfix, postfix ? postfix : "", sizeof(sc->postfix) - 1);

    // Compute dot position from the printed number string
    char numstr[S_FROM_NUMBER_MAX_CHARS];
    int numstr_len = s_from_number_flags(m_fabs(number), numstr, S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM);
    sc->dot_char_offset = numstr_len;
    for (int i = 0; i < numstr_len; i++) {
        if (numstr[i] == '.') { sc->dot_char_offset = i; break; }
    }

    sc->has_dot = sc->dot_char_offset < sc->num_target_chars_excluding_minus_and_postfix ? 1 : 0;
    sc->had_dot_at_init = sc->has_dot;
    // Fractional digits the literal was written with -- the drag may add more
    // (a finer wheel), never fewer.
    sc->init_decimals = sc->has_dot
        ? sc->num_target_chars_excluding_minus_and_postfix - sc->dot_char_offset - 1
        : 0;

    // Determine which wheel is being dragged from mouse click position
    int drag_char_offset = (int)m_floor(ui->mouse_x - sc->x) - (number < 0.f ? 1 : 0);
    if (drag_char_offset == sc->dot_char_offset)
        drag_char_offset--; // dragging the decimal point drags the digit to its left

    // Map char offset to wheel id (power-of-10 index)
    int drag_wheel_id = sc->dot_char_offset - drag_char_offset - (drag_char_offset < sc->dot_char_offset ? 1 : 0);

    sc->has_wheel_offset_neg = 0;
    sc->prev_base_dist = 0.0;
    sc->disp_neg       = number < 0.0 ? 1 : 0;

    // Populate wheel array, visible range, and number
    ui_scrubber_calc_wheels(sc, number, drag_wheel_id);

    // Capture each wheel's initial fractional offset so that every visible
    // wheel renders at an exact integer (scroll == 0) at the start of the
    // drag.  Use floor (not round) so the offset matches the displayed digit:
    // e.g. tens wheel of 595 reads 59.5 -> offset 0.5 -> shows '9', scroll 0.
    {
        double mag0 = m_fabs(number);
        for (int k = -SCRUBBER_WHEEL_BASE; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
            double pos0 = sw_odometer_pos(sw_wheel_cont(mag0, k), k, drag_wheel_id);
            double o = pos0 - m_floor(pos0 + SCRUBBER_EPS);
            sc->wheel_offset[SW(k)]     = o;
            sc->wheel_offset_pos[SW(k)] = o;
        }
    }
}


// ===== High-level: per-frame update + render =====
// Returns true when the value changes.
static inline bool ui_scrubber(UIContext *ui, UIScrubber *sc) {
    float dy = ui->mouse_y - sc->anchor_screen_y;
    float dist = dy;

    // ===== Shift promotes an integer scrub to a fractional one, mid-drag.
    // The drag is never interrupted: internal_number has been tracking the
    // fractional drag position all along (INTEGER only hides/rounds it on
    // output), so we just stop hiding it and force a decimal point.  The
    // promotion latches for the rest of the drag — releasing shift keeps the
    // decimals you just dialled in, so you can let go of shift before the
    // mouse button.
    bool promoted_now = false;
    if ((sc->init_flags & UI_SCRUBBER_FLAGS_INTEGER) && !sc->promoted_to_double &&
        (ui->mouse_flags & UI_FLAGS_SHIFT)) {
        sc->promoted_to_double = 1;
        sc->flags = (sc->init_flags & ~UI_SCRUBBER_FLAGS_INTEGER) | UI_SCRUBBER_FLAGS_FORCE_DOT;
        promoted_now = true;
    }

    // ===== Update: map drag -> number monotonically, including zero crossing.
    // `lin` is the naive linear value (dragging down always decreases it).
    // A snap (width = decimal_mul) is inserted exactly at the
    // zero crossing so the wheels visibly stop and reverse, e.g. dragging 3
    // down past zero shows magnitudes 3 2 1 0 0 1 2 with a minus from the
    // second zero.  The snap only applies when the drag actually crosses
    // zero (sign differs from the start), so an already-negative number keeps
    // decreasing smoothly when dragged further down.
    double rounding = 10.0;
    double cur_dist = m_floor((double)dist * rounding) / rounding;
    double dm       = sc->decimal_mul;
    double lin      = sc->original_number - cur_dist * dm;
    double dmd      = 0.1 * dm; // dm snap

    double new_number;
    int    neg;
    if (sc->original_number >= 0.0) {
        // started non-negative; crossing happens dragging down (lin -> negative)
        if (lin >= 0.0)      { new_number = lin;         neg = 0; } // positive incl. first zero
        else if (lin > -dmd) { new_number = 0.0;         neg = 0; } // snap: first zero lingers
        else                  { new_number = lin + dmd;  neg = 1; } // negative incl. second zero (-0)
    } else {
        // started negative; crossing happens dragging up (lin -> positive)
        if (lin <= 0.0)      { new_number = lin;         neg = 1; } // negative incl. first zero (-0)
        else if (lin < dmd)  { new_number = 0.0;         neg = 1; } // snap: -0 lingers
        else                 { new_number = lin - dmd;   neg = 0; } // positive incl. second zero (0)
    }

    // The drag moves in steps of 0.1 * decimal_mul, so it reaches one digit
    // deeper than the wheel being dragged; the literal's own digits are the
    // floor.  Snapping to that grid keeps the value equal to the reading.
    {
        int drag_decimals = 1 - sc->drag_wheel_id;
        int places = drag_decimals > sc->init_decimals ? drag_decimals : sc->init_decimals;
        new_number = sw_snap_decimal(new_number, places);
    }

    bool changed = new_number != sc->internal_number || neg != sc->disp_neg || promoted_now;
    bool sign_flipped = neg != sc->disp_neg;

    sc->internal_number   = new_number;
    sc->sign     = neg ? -1.0 : 1.0;
    sc->disp_neg = neg;

    // When the sign flips (crossing zero), gear_dir reverses and the snap
    // shifts magnitudes so the initial offsets no longer cancel the
    // fractional parts.  Swap between the two offset sets — positive
    // (saved from init) and negative (computed on first non-zero entry
    // to the negative phase).
    if (sign_flipped) {
        if (neg) {
            // entering negative phase — swap to negative offsets if ready
            if (sc->has_wheel_offset_neg) {
                for (int k = -SCRUBBER_WHEEL_BASE; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
                    sc->wheel_offset[SW(k)] = sc->wheel_offset_neg[SW(k)];
                }
            }
        } else {
            // returning to positive — restore the init offsets
            for (int k = -SCRUBBER_WHEEL_BASE; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
                sc->wheel_offset[SW(k)] = sc->wheel_offset_pos[SW(k)];
            }
        }
    }

    // On the first non-zero frame after entering the negative phase,
    // compute the negative-offset set from the current magnitude so
    // every drum starts at scroll=0 in the new gear direction.
    // (We wait for mag>0 because the snap can produce new_number==0
    //  on the flip frame, e.g. when original_number mod dmd == 0.)
    if (neg && !sc->has_wheel_offset_neg) {
        double mag = m_fabs(new_number);
        if (mag > 0.0) {
            for (int k = -SCRUBBER_WHEEL_BASE; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
                double pos0 = sw_odometer_pos(sw_wheel_cont(mag, k), k, sc->drag_wheel_id);
                sc->wheel_offset_neg[SW(k)] = pos0 - m_floor(pos0 + SCRUBBER_EPS);
            }
            sc->has_wheel_offset_neg = 1;
            // also update the active set (we are currently negative)
            for (int k = -SCRUBBER_WHEEL_BASE; k < SCRUBBER_MAX_WHEELS - SCRUBBER_WHEEL_BASE; k++) {
                sc->wheel_offset[SW(k)] = sc->wheel_offset_neg[SW(k)];
            }
        }
    }

    // Rebuild all wheels and visible range from the new number.
    // This is simpler and more robust than incremental wheel updates —
    // it avoids wheels going negative and sign-bookkeeping issues.
    ui_scrubber_calc_wheels(sc, new_number, sc->drag_wheel_id);

    // If dragging created fractional digits that weren't in the source
    // format, show a decimal point so the output is mathematically correct
    // (e.g. dragging "888888" creates wheel[-1]!=0 -> show "888888.1")
    if (sc->flags & UI_SCRUBBER_FLAGS_INTEGER)
        sc->has_dot = 0;
    else if (sc->flags & UI_SCRUBBER_FLAGS_FORCE_DOT)
        sc->has_dot = 1;
    else if (!sc->had_dot_at_init && sc->vis_bot_wheel < 0)
        sc->has_dot = 1;
    else
        sc->has_dot = sc->had_dot_at_init;

    // Render
    ui_scrubber_render(ui, sc);

    return changed;
}


// ===== Immediate-mode widget: ui_scrubber_f =====
// Drop-in replacement for ui_slider / ui_button layout-wise.
// Displays the float value as text; on click enters slot-machine scrubbing.
// Returns 1 when the value changed this frame.
static inline int ui_scrubber_f(UIContext *ui, float *value, int flags) {
    int disabled = ui_item_disable_begin(ui);
    int id = ui_id_from_ptr((const void *)value);
    // Disabled mid-drag: drop the capture instead of running the scrubber for
    // another frame. Unlike the other draggables, its update below is gated on
    // active_id alone, not on the mouse button, so it would read the parked
    // mouse position as an enormous drag delta and trash the value.
    if (disabled && ui->active_id == id) ui_set_active_control(ui, 0, NULL);
    double dv = (double)(*value);

    // ---- display text ----
    char buf[S_FROM_NUMBER_MAX_CHARS];
    int num_len;
    if (flags & UI_SCRUBBER_FLAGS_INTEGER) {
        int iv = (int)(dv + (dv >= 0.0 ? 0.5 : -0.5));
        char *p = buf;
        int tmp = iv < 0 ? -iv : iv;
        if (iv < 0) *p++ = '-';
        if (tmp == 0) *p++ = '0';
        else {
            char t2[16]; int t = 0;
            while (tmp) { t2[t++] = (char)('0' + (tmp % 10)); tmp /= 10; }
            while (t) *p++ = t2[--t];
        }
        *p = '\0';
        num_len = (int)(p - buf);
    } else {
        int fm = (flags & UI_SCRUBBER_FLAGS_FORCE_DOT) ? S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM : 0;
        num_len = s_from_number_flags(dv, buf, fm);
    }

    float autow = (float)num_len + 2.0f;
    float w = ui_take_width(ui, autow);
    float x = ui->pen_x, y = ui->pen_y;
    int iw = (int)w, ix = (int)x;

    int hovered = ui_hit(ui, x, y, w, 1.0f);
    if (hovered) ui->hot_id = id;

    int changed = 0;

    // ---- enter scrubbing on left-click ----
    if (hovered && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        UIScrubber *sc = (UIScrubber *)ui->sys->malloc(sizeof(UIScrubber));
        ui->sys->memset(sc, 0, sizeof(UIScrubber));
        char ns[S_FROM_NUMBER_MAX_CHARS];
        int nsl = s_from_number_flags(m_fabs(dv), ns, S_FROM_NUMBER_FLAG_FORCE_DECIMAL_FORM);
        int nc = nsl + (dv < 0.0 ? 1 : 0);
        ui_scrubber_init(ui, sc, dv, "", nc, x + 1.0f, y, flags);
        ui_set_active_control(ui, id, sc);
    }

    // ---- per-frame scrubber update + render ----
    if (ui->active_id == id) {
        UIScrubber *sc = (UIScrubber *)ui->active_state;
        if (ui_scrubber(ui, sc)) { changed = 1; if (value) *value = (float)ui_scrubber_get_value(sc); }
    }

    // ---- release ----
    if (ui->active_id == id && ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) {
        if (ui->active_state && value)
            *value = (float)ui_scrubber_get_value((UIScrubber *)ui->active_state);
        ui_set_active_control(ui, 0, NULL);
        changed = 1;
    }

    // ---- draw: text (idle) or scrubber already rendered ----
    if (ui->active_id != id) {
        // Hover lights the number itself up -- accent colour on the scrubber's
        // own ramp, drawn at the same bold weight the wheels use -- rather than
        // swapping in a button background that would vanish again the moment
        // the scrub overlay takes over.
        unsigned int col    = ui_theme_color(ui, UI_COL_DEFAULT);
        unsigned int txt    = hovered ? ui_scrubber_ramp_color(ui, flags) : col;
        float        weight = hovered ? ui->global_weight + 1.0f : ui->global_weight;
        float tx = x + (float)(iw - num_len) / 2.0f;
        if (tx < (float)ix + 1.0f) tx = (float)ix + 1.0f;
        for (int i = 0; i < num_len; i++)
            ui_draw_cell_flags_weight(ui, tx + (float)i, y, (unsigned char)buf[i], txt, 0, weight);
        ui_fill_rect(ui, x, y, iw, 1, ' ', col);
    }

    ui_advance(ui, x, y, w, 1.0f);
    ui_item_disable_end(ui, disabled);
    return changed;
}

#endif // UI_SCRUBBER_H
