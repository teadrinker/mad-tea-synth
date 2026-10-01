#include "textmode_ui_textarea.h"
#include "common/string_pure.h"
#include "platform/platform.h"

// ===== colours =====
// The editor has no theme slots of its own: every colour here is the nearest
// slot plus an offset along its own ramp (ui_adjust_color_additive), so the
// whole textarea follows ui_theme_color's grading. The offsets reproduce the
// 0xAARRGGBB literals these replaced exactly at the default theme.

// Body text on the panel, a touch above UI_COL_DEFAULT's text and background:
// the editor is a well sunk into the panel, not a widget sitting on it.
static unsigned int ta_col_text(const UIContext *ui) {
    return ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT), 48, 8);
}

// Alternating row: a third of a palette step of background above ta_col_text.
// Deliberately far less than the ~12 a step is worth -- any more and the
// banding competes with the text for attention.
static unsigned int ta_col_text_alt(const UIContext *ui) {
    return ui_adjust_color_additive(ui, ta_col_text(ui), 0, 4);
}

// Line numbers and the empty rows past the end of the content: the theme's
// dim-text ramp, two steps further down, on the editor background.
static unsigned int ta_col_gutter(const UIContext *ui) {
    return ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DIMMED_TEXT), -24, 8);
}

// Selection: body text lifted onto UI_COL_BUTTON_HOVER's background, so the
// highlight tracks whatever the theme calls a raised surface.
static unsigned int ta_col_selection(const UIContext *ui) {
    return ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_BUTTON_HOVER), 48, -8);
}

// The caret is an inverted cell -- the panel's text pushed down to near-black
// and its background up to near-white. It has to stay the brightest thing in
// the editor at any grading, which is why it is not simply the text colour
// with its two channels swapped.
static unsigned int ta_col_cursor(const UIContext *ui) {
    return ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT), -130, 199);
}

// ===== keycodes (now platform-neutral; see platform/platform.h) =====
#define TA_KEY_LEFT       PLATFORM_KEY_LEFT
#define TA_KEY_RIGHT      PLATFORM_KEY_RIGHT
#define TA_KEY_UP         PLATFORM_KEY_UP
#define TA_KEY_DOWN       PLATFORM_KEY_DOWN
#define TA_KEY_HOME       PLATFORM_KEY_HOME
#define TA_KEY_END        PLATFORM_KEY_END
#define TA_KEY_PAGEUP     PLATFORM_KEY_PAGEUP
#define TA_KEY_PAGEDOWN   PLATFORM_KEY_PAGEDOWN
#define TA_KEY_DELETE     PLATFORM_KEY_DELETE
#define TA_KEY_BACKSPACE  PLATFORM_KEY_BACKSPACE
#define TA_KEY_ENTER      PLATFORM_KEY_ENTER
#define TA_KEY_TAB        PLATFORM_KEY_TAB

#define TA_KEY_C  PLATFORM_KEY_C
#define TA_KEY_X  PLATFORM_KEY_X
#define TA_KEY_V  PLATFORM_KEY_V
#define TA_KEY_A  PLATFORM_KEY_A
#define TA_KEY_Z  PLATFORM_KEY_Z
#define TA_KEY_Y  PLATFORM_KEY_Y

// ===== internal types =====

// Each character in the text optionally carries a 32-bit tag.
// When tags==NULL every character on this line has tag 0 (the default).
// lineTag is always present (a plain struct field, no allocation needed).
typedef struct {
    int           length;
    int           capacity;
    char         *text;
    unsigned int *tags;    // NULL => all characters on this line have tag 0
    unsigned int  lineTag; // one tag per logical line
} Line;

typedef struct {
    int logical_row;
    int char_offset;
    int length;
    int is_wrapped;
} VisualLine;

// ===== diff-based undo types =====
typedef enum { UNDO_INS, UNDO_DEL } UndoDiffKind;
typedef struct { UndoDiffKind kind; int row, col; char *text; int len; } UndoDiff;
typedef struct {
    UndoDiff *diffs; int count, cap;
    int old_crow, old_ccol, old_srow, old_scol; float old_scroll;
    int new_crow, new_ccol, new_srow, new_scol; float new_scroll;
} UndoGroup;

struct UITextArea {
    Tsys *sys;

    UITextAreaCallbacks *callbacks;

    Line  *lines;
    int    num_lines;
    int    cap_lines;

    int cur_row, cur_col;
    float scroll;
    int sel_row, sel_col;

    bool show_line_numbers;
    bool show_bottom_status;

    VisualLine *vis;
    int         num_vis;
    int         cap_vis;

    // last laid-out rectangle (cell coords) for mouse mapping
    int rx, ry, rw, rh, gutter;
    int text_width;  // effective text width after scrollbar accommodation

    // set after double-click select_word_at, used to extend by words on drag
    bool drag_by_word;
    int  word_anchor_row;
    int  word_anchor_start;
    int  word_anchor_end;

    // set when gutter-click selected a line; drag then extends by whole lines
    bool gutter_select;
    int  gutter_anchor_row;

    UndoGroup *undo_stack;
    int undo_cap, undo_pos, undo_count;
    int undo_low;   // lowest undo_pos since the last undo_mark
    bool undo_saving;
    UndoGroup current_group;
    bool group_active;

    bool enable_fractional_scroll;

    // drag-and-drop state
    bool mousedown_on_selection; // mouse was pressed on existing selection
    bool drag_active;            // drag is active (mouse moved while down on selected text)
    int  drag_orig_sel_row, drag_orig_sel_col;  // original selection anchor (normalised)
    int  drag_orig_cur_row, drag_orig_cur_col;  // original cursor position (normalised)
    int  drag_target_row, drag_target_col;      // where text would be inserted (during drag)
    int  drag_press_row, drag_press_col;        // where mouse was pressed (logical coords)

    char *bound;  // ui_textarea_str: the text last shown from or handed to the model


};

// ===== helpers =====

static void undo_record_diff(UITextArea *ta, UndoDiffKind kind, int row, int col, const char *text, int len);
static char *get_selection_text(UITextArea *ta);

static void ui_memset_bytes(void *dst, int val, int n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v  = (unsigned char)val;
    for (int i = 0; i < n; i++) d[i] = v;
}

// ===== tag helpers =====

// Allocate (and zero) the tags array for a line that previously had none.
// Must only be called when row is valid and the line exists.
static void ensure_tags(UITextArea *ta, int row) {
    Line *l = &ta->lines[row];
    if (!l->tags) {
        l->tags = (unsigned int *)ta->sys->malloc(sizeof(unsigned int) * l->capacity);
        ui_memset_bytes(l->tags, 0, sizeof(unsigned int) * l->capacity);
    }
}

// ===== buffer helpers =====

static Line ta_make_line(UITextArea *ta, const char *s) {
    int   n = (int)s_strlen(s);
    char *p = (char *)ta->sys->malloc(n + 1);
    ui_memcpy(p, s, n);
    p[n] = '\0';
    Line line;
    line.text     = p;
    line.length   = n;
    line.capacity = n + 1;
    line.tags     = NULL;
    line.lineTag  = 0;
    return line;
}

static void ensure_lines(UITextArea *ta, int needed) {
    if (needed >= ta->cap_lines) {
        int new_cap = needed * 2 + 64;
        ta->lines = (Line *)ta->sys->realloc(ta->lines, sizeof(Line) * new_cap);
        for (int i = ta->cap_lines; i < new_cap; i++) {
            ta->lines[i].text     = NULL;
            ta->lines[i].length   = 0;
            ta->lines[i].capacity = 0;
            ta->lines[i].tags     = NULL;
            ta->lines[i].lineTag  = 0;
        }
        ta->cap_lines = new_cap;
    }
}

static void ensure_line_exists(UITextArea *ta, int row) {
    ensure_lines(ta, row + 1);
    while (ta->num_lines <= row) {
        ta->lines[ta->num_lines] = ta_make_line(ta, "");
        ta->num_lines++;
    }
}

// Grow line's text (and tags, if present) to hold at least min_len chars.
static void grow_line(UITextArea *ta, int row, int min_len) {
    ensure_line_exists(ta, row);
    if (ta->lines[row].capacity < min_len + 1) {
        int old_cap = ta->lines[row].capacity;
        int new_sz  = min_len * 2 + 32;
        ta->lines[row].text = (char *)ta->sys->realloc(ta->lines[row].text, new_sz);
        if (ta->lines[row].tags) {
            ta->lines[row].tags = (unsigned int *)ta->sys->realloc(
                ta->lines[row].tags, sizeof(unsigned int) * new_sz);
            for (int i = old_cap; i < new_sz; i++) ta->lines[row].tags[i] = 0;
        }
        ta->lines[row].capacity = new_sz;
    }
}

static void insert_char(UITextArea *ta, int row, int col, char ch) {
    grow_line(ta, row, ta->lines[row].length + 1);
    int len = ta->lines[row].length;
    ui_memmove(ta->lines[row].text + col + 1, ta->lines[row].text + col, len - col + 1);
    ta->lines[row].text[col] = ch;
    if (ta->lines[row].tags) {
        ui_memmove((void *)(ta->lines[row].tags + col + 1),
                   (void *)(ta->lines[row].tags + col),
                   (len - col) * (int)sizeof(unsigned int));
        ta->lines[row].tags[col] = 0;
    }
    ta->lines[row].length++;
    undo_record_diff(ta, UNDO_INS, row, col, &ch, 1);
}

// Insert len non-newline chars at (row,col) in a single memmove + single undo record.
static void insert_chars_bulk(UITextArea *ta, int row, int col, const char *chars, int len) {
    if (len <= 0) return;
    ensure_line_exists(ta, row);
    int old_len = ta->lines[row].length;
    grow_line(ta, row, old_len + len);
    ui_memmove(ta->lines[row].text + col + len, ta->lines[row].text + col, old_len - col + 1);
    ui_memcpy(ta->lines[row].text + col, chars, len);
    if (ta->lines[row].tags) {
        ui_memmove((void *)(ta->lines[row].tags + col + len),
                   (void *)(ta->lines[row].tags + col),
                   (old_len - col) * (int)sizeof(unsigned int));
        for (int i = 0; i < len; i++) ta->lines[row].tags[col + i] = 0;
    }
    ta->lines[row].length = old_len + len;
    undo_record_diff(ta, UNDO_INS, row, col, chars, len);
}

static int delete_char(UITextArea *ta, int row, int col) {
    ensure_line_exists(ta, row);
    int len = ta->lines[row].length;
    if (col < len) {
        char ch = ta->lines[row].text[col];
        ui_memmove(ta->lines[row].text + col,
                   ta->lines[row].text + col + 1, len - col);
        if (ta->lines[row].tags) {
            ui_memmove((void *)(ta->lines[row].tags + col),
                       (void *)(ta->lines[row].tags + col + 1),
                       (len - col - 1) * (int)sizeof(unsigned int));
        }
        ta->lines[row].length--;
        undo_record_diff(ta, UNDO_DEL, row, col, &ch, 1);
        return 0;
    }
    return -1;
}

static void join_next_line(UITextArea *ta, int row) {
    if (row + 1 >= ta->num_lines) return;
    int len1 = ta->lines[row].length;
    int len2 = ta->lines[row + 1].length;
    undo_record_diff(ta, UNDO_DEL, row, len1, "\n", 1);
    grow_line(ta, row, len1 + len2);
    ui_memcpy(ta->lines[row].text + len1, ta->lines[row + 1].text, len2 + 1);
    ta->lines[row].length = len1 + len2;

    // Merge tag arrays
    if (ta->lines[row + 1].tags && len2 > 0) {
        ensure_tags(ta, row);  // ensures row's tags array exists
        ui_memcpy((void *)(ta->lines[row].tags + len1),
                  (void *)(ta->lines[row + 1].tags),
                  len2 * (int)sizeof(unsigned int));
    } else if (ta->lines[row].tags && len2 > 0) {
        // row+1 has no tags (all implicit 0); zero those positions in row's array
        for (int i = len1; i < len1 + len2; i++) ta->lines[row].tags[i] = 0;
    }
    // lineTag: keep row's own lineTag; discard row+1's

    ta->sys->free(ta->lines[row + 1].text);
    if (ta->lines[row + 1].tags) ta->sys->free(ta->lines[row + 1].tags);

    for (int i = row + 1; i < ta->num_lines - 1; i++)
        ta->lines[i] = ta->lines[i + 1];

    ta->lines[ta->num_lines - 1].text     = NULL;
    ta->lines[ta->num_lines - 1].length   = 0;
    ta->lines[ta->num_lines - 1].capacity = 0;
    ta->lines[ta->num_lines - 1].tags     = NULL;
    ta->lines[ta->num_lines - 1].lineTag  = 0;
    ta->num_lines--;
}

static void insert_newline(UITextArea *ta, int row, int col) {
    ensure_line_exists(ta, row);
    undo_record_diff(ta, UNDO_INS, row, col, "\n", 1);
    int len      = ta->lines[row].length;
    int tail_len = len - col;

    char         *tail_text = NULL;
    unsigned int *tail_tags = NULL;

    if (tail_len > 0) {
        tail_text = (char *)ta->sys->malloc(tail_len + 1);
        ui_memcpy(tail_text, ta->lines[row].text + col, tail_len + 1);
        if (ta->lines[row].tags) {
            tail_tags = (unsigned int *)ta->sys->malloc(
                sizeof(unsigned int) * (tail_len + 1));
            ui_memcpy((void *)tail_tags,
                      (void *)(ta->lines[row].tags + col),
                      tail_len * (int)sizeof(unsigned int));
        }
    }

    ta->lines[row].text[col] = '\0';
    ta->lines[row].length    = col;
    // lineTag stays on the original row

    ensure_lines(ta, ta->num_lines + 1);
    ui_memmove(ta->lines + row + 2, ta->lines + row + 1,
               (int)sizeof(Line) * (ta->num_lines - row - 1));

    if (tail_len > 0) {
        ta->lines[row + 1].text     = tail_text;
        ta->lines[row + 1].length   = tail_len;
        ta->lines[row + 1].capacity = tail_len + 1;
        ta->lines[row + 1].tags     = tail_tags;
        ta->lines[row + 1].lineTag  = 0;
    } else {
        ta->lines[row + 1] = ta_make_line(ta, "");
    }
    ta->num_lines++;
}

static void clamp_cursor(UITextArea *ta) {
    if (ta->cur_row < 0) ta->cur_row = 0;
    if (ta->cur_row >= ta->num_lines) ta->cur_row = ta->num_lines - 1;
    ensure_line_exists(ta, ta->cur_row);
    if (ta->cur_col < 0) ta->cur_col = 0;
    int len = ta->lines[ta->cur_row].length;
    if (ta->cur_col > len) ta->cur_col = len;
}

// ===== diff-based undo/redo =====

static void undo_free_group(UITextArea *ta, UndoGroup *g) {
    for (int i = 0; i < g->count; i++)
        if (g->diffs[i].text) ta->sys->free(g->diffs[i].text);
    if (g->diffs) { ta->sys->free(g->diffs); g->diffs = NULL; }
    g->count = g->cap = 0;
}

static void undo_init(UITextArea *ta) {
    ta->undo_cap = 64;
    ta->undo_stack = (UndoGroup *)ta->sys->malloc(sizeof(UndoGroup) * ta->undo_cap);
    for (int i = 0; i < ta->undo_cap; i++)
        ta->undo_stack[i].diffs = NULL, ta->undo_stack[i].count = ta->undo_stack[i].cap = 0;
    ta->undo_pos = ta->undo_count = ta->undo_low = 0;
    ta->undo_saving = false;
    ta->current_group.diffs = NULL;
    ta->current_group.count = ta->current_group.cap = 0;
    ta->group_active = false;
}

static void undo_destroy(UITextArea *ta) {
    if (ta->undo_stack) {
        for (int i = 0; i < ta->undo_count; i++) undo_free_group(ta, &ta->undo_stack[i]);
        ta->sys->free(ta->undo_stack);
        ta->undo_stack = NULL;
    }
    undo_free_group(ta, &ta->current_group);
}

static void undo_reset(UITextArea *ta) {
    if (ta->undo_saving) return;
    undo_destroy(ta);
    undo_init(ta);
}

static void undo_begin_edit(UITextArea *ta) {
    if (ta->undo_saving || ta->group_active) return;
    ta->group_active = true;
    for (int i = 0; i < ta->current_group.count; i++)
        if (ta->current_group.diffs[i].text) ta->sys->free(ta->current_group.diffs[i].text);
    ta->current_group.count = 0;
    ta->current_group.old_crow = ta->cur_row;
    ta->current_group.old_ccol = ta->cur_col;
    ta->current_group.old_srow = ta->sel_row;
    ta->current_group.old_scol = ta->sel_col;
    ta->current_group.old_scroll = ta->scroll;
}

static void undo_record_diff(UITextArea *ta, UndoDiffKind kind, int row, int col,
                              const char *text, int len) {
    if (!ta->group_active || !text || len <= 0) return;
    UndoGroup *g = &ta->current_group;
    if (g->count >= g->cap) {
        int new_cap = g->cap * 2 + 8;
        g->diffs = (UndoDiff *)ta->sys->realloc(g->diffs, sizeof(UndoDiff) * new_cap);
        for (int i = g->cap; i < new_cap; i++) g->diffs[i].text = NULL;
        g->cap = new_cap;
    }
    UndoDiff *d = &g->diffs[g->count];
    d->kind = kind;
    d->row = row; d->col = col;
    d->text = (char *)ta->sys->malloc(len + 1);
    ui_memcpy(d->text, text, len);
    d->text[len] = '\0';
    d->len = len;
    g->count++;
}

static void undo_end_edit(UITextArea *ta) {
    if (ta->undo_saving || !ta->group_active) return;
    ta->group_active = false;
    UndoGroup *g = &ta->current_group;
    if (g->count == 0) return;
    g->new_crow = ta->cur_row; g->new_ccol = ta->cur_col;
    g->new_srow = ta->sel_row; g->new_scol = ta->sel_col;
    g->new_scroll = ta->scroll;
    // discard redo
    for (int i = ta->undo_pos; i < ta->undo_count; i++) undo_free_group(ta, &ta->undo_stack[i]);
    ta->undo_count = ta->undo_pos;
    if (ta->undo_count >= ta->undo_cap) {
        int new_cap = ta->undo_cap * 2 + 8;
        ta->undo_stack = (UndoGroup *)ta->sys->realloc(ta->undo_stack, sizeof(UndoGroup) * new_cap);
        for (int i = ta->undo_cap; i < new_cap; i++)
            ta->undo_stack[i].diffs = NULL, ta->undo_stack[i].count = ta->undo_stack[i].cap = 0;
        ta->undo_cap = new_cap;
    }
    ta->undo_stack[ta->undo_count] = *g;
    g->diffs = NULL; g->count = g->cap = 0;
    ta->undo_count++;
    ta->undo_pos = ta->undo_count;
}

static void undo_abort_edit(UITextArea *ta) {
    if (!ta->group_active) return;
    ta->group_active = false;
    UndoGroup *g = &ta->current_group;
    for (int i = 0; i < g->count; i++)
        if (g->diffs[i].text) ta->sys->free(g->diffs[i].text);
    g->count = 0;
}

// apply a single diff; apply_kind is the direction to execute (INS inserts, DEL deletes)
static void undo_apply_diff(UITextArea *ta, UndoDiff *d, UndoDiffKind apply_kind) {
    if (apply_kind == UNDO_INS) {
        const char *s = d->text;
        int r = d->row, c = d->col;
        int run_pos = 0;
        char buf[1024];
        for (int i = 0; i < d->len; i++) {
            if (s[i] == '\r') continue;
            if (s[i] == '\n') {
                if (run_pos > 0) { insert_chars_bulk(ta, r, c, buf, run_pos); c += run_pos; run_pos = 0; }
                insert_newline(ta, r, c); r++; c = 0;
            } else {
                buf[run_pos++] = s[i];
                if (run_pos >= 1024) { insert_chars_bulk(ta, r, c, buf, run_pos); c += run_pos; run_pos = 0; }
            }
        }
        if (run_pos > 0) insert_chars_bulk(ta, r, c, buf, run_pos);
    } else {
        int r = d->row, c = d->col, rem = d->len;
        while (rem > 0) {
            ensure_line_exists(ta, r);
            int line_len = ta->lines[r].length;
            if (c < line_len) {
                int to_del = line_len - c;
                if (to_del > rem) to_del = rem;
                for (int i = 0; i < to_del; i++) delete_char(ta, r, c);
                rem -= to_del;
            }
            if (rem > 0 && r + 1 < ta->num_lines) {
                join_next_line(ta, r);
                rem--;
            } else if (rem > 0) break;
        }
    }
}

static void undo_apply_group(UITextArea *ta, UndoGroup *g, bool reverse) {
    ta->undo_saving = true;
    int start = reverse ? g->count - 1 : 0;
    int end   = reverse ? -1 : g->count;
    int step  = reverse ? -1 : 1;
    for (int i = start; i != end; i += step) {
        UndoDiff *d = &g->diffs[i];
        UndoDiffKind k = reverse ? (d->kind == UNDO_INS ? UNDO_DEL : UNDO_INS) : d->kind;
        undo_apply_diff(ta, d, k);
    }
    ta->undo_saving = false;
}

static void undo_undo(UITextArea *ta) {
    if (ta->undo_pos <= 0) return;
    bool was_active = ta->group_active;
    ta->group_active = false;
    ta->undo_pos--;
    if (ta->undo_pos < ta->undo_low) ta->undo_low = ta->undo_pos;
    UndoGroup *g = &ta->undo_stack[ta->undo_pos];
    undo_apply_group(ta, g, true);
    ta->cur_row = g->old_crow; ta->cur_col = g->old_ccol;
    ta->sel_row = g->old_srow; ta->sel_col = g->old_scol;
    ta->scroll  = g->old_scroll;
    clamp_cursor(ta);
    ta->group_active = was_active;
}

static void undo_redo(UITextArea *ta) {
    if (ta->undo_pos >= ta->undo_count) return;
    bool was_active = ta->group_active;
    ta->group_active = false;
    UndoGroup *g = &ta->undo_stack[ta->undo_pos];
    undo_apply_group(ta, g, false);
    ta->cur_row = g->new_crow; ta->cur_col = g->new_ccol;
    ta->sel_row = g->new_srow; ta->sel_col = g->new_scol;
    ta->scroll  = g->new_scroll;
    clamp_cursor(ta);
    ta->undo_pos++;
    ta->group_active = was_active;
}

void ui_textarea_undo(UITextArea *ta) { undo_undo(ta); }
void ui_textarea_redo(UITextArea *ta) { undo_redo(ta); }

void ui_textarea_undo_begin_batch(UITextArea *ta) {
    undo_begin_edit(ta);
    ta->undo_saving = true;
}

void ui_textarea_undo_end_batch(UITextArea *ta) {
    ta->undo_saving = false;
    undo_end_edit(ta);
}

int ui_textarea_undo_mark(UITextArea *ta) {
    ta->undo_low = ta->undo_pos;
    return ta->undo_pos;
}

// Concatenate the groups pushed since the mark: diffs in order, the caret and
// scroll from before the first and after the last. Skipped when an undo went
// below the mark (the groups above it are no longer the ones marked) or when
// redo groups sit above undo_pos.
void ui_textarea_undo_merge_since(UITextArea *ta, int mark) {
    if (mark < 0 || ta->undo_low < mark || ta->undo_pos != ta->undo_count) return;
    if (ta->undo_pos - mark < 2) return;
    UndoGroup *dst = &ta->undo_stack[mark];
    for (int i = mark + 1; i < ta->undo_pos; i++) {
        UndoGroup *src = &ta->undo_stack[i];
        if (dst->count + src->count > dst->cap) {
            int new_cap = dst->cap * 2 + src->count + 8;
            dst->diffs = (UndoDiff *)ta->sys->realloc(dst->diffs, sizeof(UndoDiff) * new_cap);
            dst->cap = new_cap;
        }
        ui_memcpy(dst->diffs + dst->count, src->diffs, sizeof(UndoDiff) * src->count);
        dst->count += src->count;
        dst->new_crow = src->new_crow; dst->new_ccol = src->new_ccol;
        dst->new_srow = src->new_srow; dst->new_scol = src->new_scol;
        dst->new_scroll = src->new_scroll;
        // the texts moved to dst: free only the array
        if (src->diffs) ta->sys->free(src->diffs);
        src->diffs = NULL; src->count = src->cap = 0;
    }
    ta->undo_count = ta->undo_pos = mark + 1;
}

// ===== lifecycle =====

UITextArea *ui_textarea_create(Tsys *sys) {
    UITextArea *ta = (UITextArea *)sys->malloc(sizeof(UITextArea));
    ta->sys       = sys;
    ta->callbacks = NULL;
    ta->lines     = NULL;
    ta->num_lines = 0;
    ta->cap_lines = 0;
    ta->cur_row   = 0;
    ta->cur_col   = 0;
    ta->scroll    = 0.0f;
    ta->sel_row   = -1;
    ta->sel_col   = -1;
    ta->show_line_numbers         = true;
    ta->show_bottom_status        = false;
    ta->enable_fractional_scroll  = false;
    ta->drag_by_word            = false;
    ta->word_anchor_row    = 0;
    ta->word_anchor_start  = 0;
    ta->word_anchor_end    = 0;
    ta->gutter_select      = false;
    ta->gutter_anchor_row  = 0;
    ta->vis     = NULL;
    ta->num_vis = 0;
    ta->cap_vis = 0;
    ta->rx = ta->ry = ta->rw = ta->rh = ta->gutter = 0;
    ta->text_width = 0;

    ta->mousedown_on_selection = false;
    ta->drag_active            = false;
    ta->drag_orig_sel_row = ta->drag_orig_sel_col = 0;
    ta->drag_orig_cur_row = ta->drag_orig_cur_col = 0;
    ta->drag_target_row   = ta->drag_target_col   = 0;
    ta->drag_press_row    = ta->drag_press_col    = 0;
    ta->bound = NULL;

    ta->cap_lines = 64;
    ta->lines = (Line *)sys->malloc(sizeof(Line) * ta->cap_lines);
    for (int i = 0; i < ta->cap_lines; i++) {
        ta->lines[i].text     = NULL;
        ta->lines[i].length   = 0;
        ta->lines[i].capacity = 0;
        ta->lines[i].tags     = NULL;
        ta->lines[i].lineTag  = 0;
    }
    ta->lines[0] = ta_make_line(ta, "");
    ta->num_lines = 1;
    undo_init(ta);
    return ta;
}

void ui_textarea_destroy(UITextArea *ta) {
    if (!ta) return;
    Tsys *sys = ta->sys;
    for (int i = 0; i < ta->num_lines; i++) {
        if (ta->lines[i].text) sys->free(ta->lines[i].text);
        if (ta->lines[i].tags) sys->free(ta->lines[i].tags);
    }
    if (ta->lines) sys->free(ta->lines);
    if (ta->vis)   sys->free(ta->vis);
    if (ta->bound) sys->free(ta->bound);
    undo_destroy(ta);
    sys->free(ta);
}

void ui_textarea_set_callbacks(UITextArea *ta, UITextAreaCallbacks *callbacks) {
    ta->callbacks = callbacks;
}

void ui_textarea_set_show_line_numbers(UITextArea *ta, bool show) {
    ta->show_line_numbers = show;
}

void ui_textarea_set_show_bottom_status(UITextArea *ta, bool show) {
    ta->show_bottom_status = show;
}

void ui_textarea_set_enable_fractional_scroll(UITextArea *ta, bool enable) {
    ta->enable_fractional_scroll = enable;
}

float ui_textarea_get_scroll(UITextArea *ta) {
    return ta->scroll;
}

// Only the lower bound is enforced here: the upper one depends on the visual
// line count, and callers restoring a saved offset (see the plugin-state
// restore in apps/madteasynth) typically set the text and the scroll together,
// before any layout for that text exists. The draw call clamps against the
// live layout on the next draw.
void ui_textarea_set_scroll(UITextArea *ta, float scroll) {
    if (scroll < 0.0f) scroll = 0.0f;
    ta->scroll = scroll;
}

// ===== scroll -> screen row mapping =====
// ta->scroll is fractional (trackpad sub-notch deltas accumulate into it, and a
// scrollbar drag sets an arbitrary float).  The render loop draws visual line
// scroll_int+k at sy = ry - scroll_frac + k, so every hit test has to derive its
// row from the *same* split. Truncating in the mouse code while the renderer
// rounds puts clicks one line above the drawn text as soon as the scroll offset
// has a fractional part >= 0.5.

static int floor_to_int(float v) {
    int i = (int)v;
    return ((float)i > v) ? i - 1 : i;
}

static void scroll_split(UITextArea *ta, int *out_int, float *out_frac) {
    if (ta->enable_fractional_scroll) {
        float s  = ta->scroll - 0.4999f;  // we clip at 0.5 so we want lines to start there
        int   si = (int)(s + 0.5f);
        *out_int  = si;
        *out_frac = s - (float)si;
    } else {
        *out_int  = (int)(ta->scroll + 0.5f);
        *out_frac = 0.0f;
    }
}

// Visual-line index drawn `rel_y` rows below the top of the text area.
static int vis_idx_at_rel_row(UITextArea *ta, float rel_y) {
    int   scroll_int;
    float scroll_frac;
    scroll_split(ta, &scroll_int, &scroll_frac);
    return scroll_int + floor_to_int(rel_y + scroll_frac);
}

// ===== public introspection =====

int  ui_textarea_line_count(UITextArea *ta)           { return ta->num_lines; }

int  ui_textarea_line_length(UITextArea *ta, int lrow) {
    if (lrow < 0 || lrow >= ta->num_lines) return 0;
    return ta->lines[lrow].length;
}

void ui_textarea_get_cursor(UITextArea *ta, int *row, int *col) {
    if (row) *row = ta->cur_row;
    if (col) *col = ta->cur_col;
}

void ui_textarea_set_cursor(UITextArea *ta, int row, int col) {
    ta->cur_row = row;
    ta->cur_col = col;
    ta->sel_row = -1;
    ta->sel_col = -1;
    clamp_cursor(ta);
}

bool ui_textarea_get_selection(UITextArea *ta, int *r1, int *c1, int *r2, int *c2) {
    if (ta->sel_row == -1) return false;
    int _r1 = ta->sel_row, _c1 = ta->sel_col;
    int _r2 = ta->cur_row, _c2 = ta->cur_col;
    if (_r1 > _r2 || (_r1 == _r2 && _c1 > _c2)) {
        int t = _r1; _r1 = _r2; _r2 = t; t = _c1; _c1 = _c2; _c2 = t;
    }
    if (_r1 == _r2 && _c1 == _c2) return false;
    if (r1) *r1 = _r1; if (c1) *c1 = _c1;
    if (r2) *r2 = _r2; if (c2) *c2 = _c2;
    return true;
}

char ui_textarea_get_char_at(UITextArea *ta, int row, int col) {
    if (row < 0 || row >= ta->num_lines) return '\0';
    if (col < 0 || col >= ta->lines[row].length) return '\0';
    return ta->lines[row].text[col];
}

bool ui_textarea_screen_to_pos(UITextArea *ta, float screen_x, float screen_y,
                                int *out_row, int *out_col) {
    // Check if coordinates are within the textarea's bounding rect
    if (screen_x < (float)ta->rx || screen_x >= (float)(ta->rx + ta->rw))
        return false;
    if (screen_y < (float)ta->ry || screen_y >= (float)(ta->ry + ta->rh))
        return false;

    // Check if within gutter
    if (screen_x < (float)(ta->rx + ta->gutter))
        return false;

    // Exclude scrollbar column (text_width < rw - gutter means scrollbar is present)
    if (ta->text_width > 0 && ta->text_width < ta->rw - ta->gutter) {
        if (screen_x >= (float)(ta->rx + ta->gutter + ta->text_width))
            return false;
    }

    // Layout must be valid (rebuilt by the draw call before this is called)
    if (ta->num_vis == 0) return false;

    // Convert to visual row index
    int vis_idx = vis_idx_at_rel_row(ta, screen_y - (float)ta->ry);
    if (vis_idx < 0 || vis_idx >= ta->num_vis)
        return false;

    VisualLine *vl = &ta->vis[vis_idx];

    // Convert screen x to column offset within the visual segment.
// Use truncation (cell boundary) for hover, not midpoint rounding like click selection.
    int rel_col = (int)(screen_x - (float)(ta->rx + ta->gutter));
    if (rel_col < 0) rel_col = 0;

    int col = rel_col + vl->char_offset;
    if (col < vl->char_offset) col = vl->char_offset;
    if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;

    *out_row = vl->logical_row;
    *out_col = col;
    return true;
}

bool ui_textarea_screen_past_end(UITextArea *ta, float screen_x, float screen_y) {
    if (ta->num_vis == 0) return false;
    int content_h = ta->rh - (ta->show_bottom_status ? 1 : 0);
    if (screen_y < (float)ta->ry || screen_y >= (float)(ta->ry + content_h)) return false;
    if (screen_x < (float)(ta->rx + ta->gutter) || screen_x >= (float)(ta->rx + ta->rw)) return false;
    if (ta->text_width > 0 && screen_x >= (float)(ta->rx + ta->gutter + ta->text_width)) return false;
    return vis_idx_at_rel_row(ta, screen_y - (float)ta->ry) >= ta->num_vis;
}

bool ui_textarea_pos_to_screen(UITextArea *ta, int row, int col,
                               float *out_x, float *out_y) {
    // Layout must be valid (rebuilt by the draw call before this is called),
    // exactly as for screen_to_pos.
    if (!ta || ta->num_vis == 0) return false;

    // Find the visual segment this logical position falls in. A wrapped line
    // spans several, so the column picks which one; the last segment of a row
    // also owns the position one past its end, which is where a caret sits.
    int vis_idx = -1;
    for (int i = 0; i < ta->num_vis; i++) {
        VisualLine *vl = &ta->vis[i];
        if (vl->logical_row != row) continue;
        int lo = vl->char_offset, hi = vl->char_offset + vl->length;
        int last = (i + 1 >= ta->num_vis) || (ta->vis[i + 1].logical_row != row);
        if (col >= lo && (col < hi || (last && col <= hi))) { vis_idx = i; break; }
    }
    if (vis_idx < 0) return false;

    int   scroll_int;
    float scroll_frac;
    scroll_split(ta, &scroll_int, &scroll_frac);

    float y = (float)ta->ry + (float)(vis_idx - scroll_int) - scroll_frac;
    float x = (float)(ta->rx + ta->gutter) + (float)(col - ta->vis[vis_idx].char_offset);

    // Scrolled out of view, or behind the scrollbar column. Reported as "no
    // screen position" rather than as an off-rect one, so a caller drawing at
    // the result cannot paint outside the widget.
    if (y < (float)ta->ry || y >= (float)(ta->ry + ta->rh)) return false;
    if (x < (float)(ta->rx + ta->gutter) || x >= (float)(ta->rx + ta->rw)) return false;
    if (ta->text_width > 0 && ta->text_width < ta->rw - ta->gutter) {
        if (x >= (float)(ta->rx + ta->gutter + ta->text_width)) return false;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    return true;
}

bool ui_textarea_screen_to_gutter_row(UITextArea *ta, float screen_x, float screen_y,
                                       int *out_row) {
    // Must be inside the textarea's bounding rect
    if (screen_x < (float)ta->rx || screen_x >= (float)(ta->rx + ta->rw))
        return false;
    if (screen_y < (float)ta->ry || screen_y >= (float)(ta->ry + ta->rh))
        return false;

    // Must be inside the gutter
    if (screen_x >= (float)(ta->rx + ta->gutter))
        return false;
    if (ta->gutter <= 0)
        return false;

    // Layout must be valid
    if (ta->num_vis == 0) return false;

    int vis_idx = vis_idx_at_rel_row(ta, screen_y - (float)ta->ry);
    if (vis_idx < 0 || vis_idx >= ta->num_vis)
        return false;

    VisualLine *vl = &ta->vis[vis_idx];
    *out_row = vl->logical_row;
    return true;
}

// ===== tag API =====

unsigned int ui_textarea_get_tag(UITextArea *ta, int row, int col) {
    if (row < 0 || row >= ta->num_lines) return 0;
    if (col < 0 || col >= ta->lines[row].length) return 0;
    if (!ta->lines[row].tags) return 0;
    return ta->lines[row].tags[col];
}

void ui_textarea_set_tag(UITextArea *ta, int row, int col, unsigned int tag) {
    if (row < 0 || row >= ta->num_lines) return;
    if (col < 0 || col >= ta->lines[row].length) return;
    if (tag == 0 && !ta->lines[row].tags) return; // already 0 implicitly
    ensure_tags(ta, row);
    ta->lines[row].tags[col] = tag;
}

unsigned int ui_textarea_get_line_tag(UITextArea *ta, int row) {
    if (row < 0 || row >= ta->num_lines) return 0;
    return ta->lines[row].lineTag;
}

void ui_textarea_set_line_tag(UITextArea *ta, int row, unsigned int tag) {
    if (row < 0 || row >= ta->num_lines) return;
    ta->lines[row].lineTag = tag;
}

// forward declaration for internal helper used by edit primitives
static void delete_selection(UITextArea *ta);

// ===== programmatic editing primitives =====

void ui_textarea_delete_range(UITextArea *ta, int r1, int c1, int r2, int c2) {
    // Normalise so r1/c1 <= r2/c2
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
    }
    if (r1 < 0) r1 = 0;
    if (r2 >= ta->num_lines) {
        // r2 one-past-the-end (ui_textarea_set_text's "delete through the
        // end of the buffer" sentinel) or beyond: clamp to the last real
        // row, but AT ITS END, not its start. Leaving c2 as given (usually
        // 0) would silently turn "delete through EOF" into "delete through
        // the start of the last row", never removing that row's own text --
        // then the caller's follow-up insert lands in front of it, pasting
        // the new content ahead of the untouched old last line instead of
        // replacing it.
        r2 = ta->num_lines - 1;
        c2 = ta->lines[r2].length;
    }
    if (r1 == r2 && c1 >= c2) return;

    undo_begin_edit(ta);

    // Use the existing delete_selection logic by temporarily setting
    // the selection to the desired range.
    ta->sel_row = r1; ta->sel_col = c1;
    ta->cur_row = r2; ta->cur_col = c2;
    delete_selection(ta);
    // delete_selection leaves cursor at (r1,c1) and sel_row = -1.

    if (ta->callbacks && ta->callbacks->on_content_updated)
        ta->callbacks->on_content_updated(ta->callbacks->user, ta);
    undo_end_edit(ta);
}

void ui_textarea_insert_string(UITextArea *ta, int row, int col, const char *s) {
    if (!s || !*s) return;
    ensure_line_exists(ta, row);

    undo_begin_edit(ta);

    int r = row, c = col;
    int run_start_r = r, run_start_c = c, run_len = 0;
    int run_pos     = 0;

    for (int i = 0; ; i++) {
        char ch = s[i];
        if (ch == '\n' || ch == '\0') {
            if (run_pos > 0) {
                insert_chars_bulk(ta, r, c, s + i - run_pos, run_pos);
                c += run_pos;
                run_len += run_pos;
                if (ta->callbacks && ta->callbacks->on_insert_text)
                    ta->callbacks->on_insert_text(ta->callbacks->user, ta,
                                                   run_start_r, run_start_c, run_len);
                run_pos = 0;
                run_len = 0;
            }
            if (ch == '\n') {
                int new_row = r + 1;
                insert_newline(ta, r, c);
                if (ta->callbacks && ta->callbacks->on_insert_line)
                    ta->callbacks->on_insert_line(ta->callbacks->user, ta, new_row);
                r++;
                c = 0;
                run_start_r = r;
                run_start_c = 0;
            }
            if (ch == '\0') break;
        } else {
            if (run_pos == 0) {
                run_start_r = r;
                run_start_c = c;
            }
            run_pos++;
        }
    }

    if (ta->callbacks && ta->callbacks->on_content_updated)
        ta->callbacks->on_content_updated(ta->callbacks->user, ta);
    undo_end_edit(ta);
}

// ===== public text get/set =====

// Full rebuild: free all existing lines and parse text into fresh lines.
// If clear_history is true, also resets undo history.
static void set_text_full_rebuild(UITextArea *ta, const char *text, bool clear_history) {
    char *old_text = clear_history ? NULL : ui_textarea_get_text(ta);
    for (int i = 0; i < ta->num_lines; i++) {
        if (ta->lines[i].text) { ta->sys->free(ta->lines[i].text); ta->lines[i].text = NULL; }
        if (ta->lines[i].tags) { ta->sys->free(ta->lines[i].tags); ta->lines[i].tags = NULL; }
        ta->lines[i].length   = 0;
        ta->lines[i].capacity = 0;
        ta->lines[i].lineTag  = 0;
    }
    ta->num_lines = 0;

    int start = 0;
    for (int pos = 0; ; pos++) {
        char c = text[pos];
        if (c == '\n' || c == '\0') {
            int len = pos - start;
            if (len > 0 && text[start + len - 1] == '\r') len--;
            ensure_lines(ta, ta->num_lines + 1);
            char *txt = (char *)ta->sys->malloc(len + 1);
            ui_memcpy(txt, text + start, len);
            txt[len] = '\0';
            ta->lines[ta->num_lines].text     = txt;
            ta->lines[ta->num_lines].length   = len;
            ta->lines[ta->num_lines].capacity = len + 1;
            ta->lines[ta->num_lines].tags     = NULL;
            ta->lines[ta->num_lines].lineTag  = 0;
            ta->num_lines++;
            start = pos + 1;
            if (c == '\0') break;
        }
    }
    if (ta->num_lines == 0) {
        ensure_lines(ta, 1);
        ta->lines[0] = ta_make_line(ta, "");
        ta->num_lines = 1;
    }
    if (clear_history) {
        ta->cur_row = ta->cur_col = 0; ta->scroll = 0.0f;
        ta->sel_row = ta->sel_col = -1;
        undo_reset(ta);
    } else {
        int save_row = ta->cur_row, save_col = ta->cur_col;
        float save_scroll = ta->scroll;
        undo_begin_edit(ta);
        undo_record_diff(ta, UNDO_DEL, 0, 0, old_text, s_strlen(old_text));
        undo_record_diff(ta, UNDO_INS, 0, 0, text, s_strlen(text));
        undo_end_edit(ta);
        ta->sys->free(old_text);
        ta->cur_row = save_row; ta->cur_col = save_col;
        ta->scroll  = save_scroll;
        clamp_cursor(ta);
    }
}

void ui_textarea_set_text(UITextArea *ta, const char *text, bool clear_history) {
    if (!text) text = "";

    // When clear_history is set, just rebuild everything from scratch.
    if (clear_history) { set_text_full_rebuild(ta, text, true); return; }

    // Parse new text into line boundaries. Use stack arrays for the common
    // case (up to 256 lines); fall back to the simple rebuild for huge texts.
    #define SET_TEXT_MAX_LINES 256
    int  new_starts[SET_TEXT_MAX_LINES];
    int  new_lens[SET_TEXT_MAX_LINES];
    int  new_count = 0;
    int  start = 0;
    bool too_many = false;
    for (int pos = 0; ; pos++) {
        char c = text[pos];
        if (c == '\n' || c == '\0') {
            if (new_count >= SET_TEXT_MAX_LINES) { too_many = true; break; }
            int len = pos - start;
            if (len > 0 && text[start + len - 1] == '\r') len--;
            new_starts[new_count] = start;
            new_lens[new_count]   = len;
            new_count++;
            start = pos + 1;
            if (c == '\0') break;
        }
    }
    if (too_many) { set_text_full_rebuild(ta, text, false); return; }
    if (new_count == 0) {
        new_starts[0] = 0; new_lens[0] = 0; new_count = 1;
    }

    // Find common prefix: lines that are identical at the top.
    int old_num = ta->num_lines;
    int prefix  = 0;
    while (prefix < old_num && prefix < new_count) {
        Line *ol = &ta->lines[prefix];
        if (ol->length != new_lens[prefix]) break;
        if (s_strncmp(ol->text, text + new_starts[prefix], (size_t)new_lens[prefix]) != 0) break;
        prefix++;
    }

    // Find common suffix: lines that are identical at the bottom
    // (non-overlapping with prefix).
    int max_suffix = old_num - prefix;
    { int t = new_count - prefix; if (t < max_suffix) max_suffix = t; }
    int suffix = 0;
    while (suffix < max_suffix) {
        Line *ol = &ta->lines[old_num - 1 - suffix];
        int   ni = new_count - 1 - suffix;
        if (ol->length != new_lens[ni]) break;
        if (s_strncmp(ol->text, text + new_starts[ni], (size_t)new_lens[ni]) != 0) break;
        suffix++;
    }

    // If nothing changed, just return (cursor/scroll stay as-is).
    if (prefix == old_num && prefix == new_count) {
        return;
    }

    // Full replacement (no lines shared at top or bottom)
    if (prefix == 0 && suffix == 0) { set_text_full_rebuild(ta, text, false); return; }

    // Batch delete + insert into a single undo step so the intermediate
    // state (first line removed) is never visible to the user.
    bool prev_saving = ta->undo_saving;
    int save_row = ta->cur_row, save_col = ta->cur_col;
    float save_scroll = ta->scroll;

    undo_begin_edit(ta);
    ta->undo_saving = true;

    // Delete the old middle portion: remove lines [prefix .. old_num-suffix)
    // (i.e. up to, but not including, the first suffix line).
    // This removes both the line content and the line breaks, so the first
    // suffix line moves up to row prefix.
    int del_r2 = old_num - suffix;
    if (del_r2 > prefix) {
        ui_textarea_delete_range(ta, prefix, 0, del_r2, 0);
    }

    // Build the new middle portion and insert it.
    // Since the first suffix line now sits at the insertion row, a trailing
    // '\n' is always needed when suffix > 0 to push it back down.
    int mid_lines = new_count - suffix - prefix;
    if (mid_lines > 0) {
        int mid_len = 0;
        for (int i = 0; i < mid_lines; i++) {
            if (i > 0) mid_len++; // '\n' separator
            mid_len += new_lens[prefix + i];
        }
        if (suffix > 0) mid_len++;
        char *mid = (char *)ta->sys->malloc(mid_len + 1);
        int   mp  = 0;
        for (int i = 0; i < mid_lines; i++) {
            if (i > 0) mid[mp++] = '\n';
            int nl = new_lens[prefix + i];
            ui_memcpy(mid + mp, text + new_starts[prefix + i], nl);
            mp += nl;
        }
        if (suffix > 0) mid[mp++] = '\n';
        mid[mp] = '\0';
        ui_textarea_insert_string(ta, prefix, 0, mid);
        ta->sys->free(mid);
    }

    ta->undo_saving = prev_saving;
    undo_end_edit(ta);

    // Restore cursor/scroll from before the edit; clamp to valid range.
    ta->cur_row = save_row; ta->cur_col = save_col;
    ta->scroll  = save_scroll;
    clamp_cursor(ta);
    ta->sel_row = ta->sel_col = -1;
}

char *ui_textarea_get_text(UITextArea *ta) {
    int total = 1;
    for (int i = 0; i < ta->num_lines; i++)
        total += ta->lines[i].length + 1;
    char *buf = (char *)ta->sys->malloc(total);
    int pos = 0;
    for (int i = 0; i < ta->num_lines; i++) {
        int len = ta->lines[i].length;
        ui_memcpy(buf + pos, ta->lines[i].text, len);
        pos += len;
        if (i != ta->num_lines - 1) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return buf;
}

// One-row numeric text entry built on a UITextArea. While the field doesn't
// hold keyboard focus its text is resynced to *value every frame -- cheap,
// since ui_textarea_set_text no-ops when the text already matches -- so an
// external change to *value (e.g. a different list row selected) shows up
// immediately without any extra "which entry is loaded" bookkeeping at the
// call site. While focused, only digits and a single leading '-' survive each
// keystroke (anything else, including a pasted or Enter-inserted newline that
// would otherwise sit invisibly below this one-row view, is stripped), and
// *value updates live from the parsed result, clamped to [min_value,
// max_value]. Returns true the frame *value actually changes.
bool ui_textarea_int_absolute_pos(UIContext *ui, UITextArea *ta, int *value,
                                  int min_value, int max_value,
                                  int x, int y, int w) {
    bool focused = (ui->focus_id == ui_id_from_ptr(ta));

    if (!focused) {
        char buf[24];
        s_from_number((double)*value, buf);
        ui_textarea_set_text(ta, buf, false);
        ui_textarea_absolute_pos(ui, ta, x, y, w, 1);
        return false;
    }

    int text_changed = ui_textarea_absolute_pos(ui, ta, x, y, w, 1);
    if (!text_changed) return false;

    char *raw = ui_textarea_get_text(ta);
    if (!raw) return false;

    char clean[24];
    int  n = 0;
    for (const char *p = raw; *p && n < (int)sizeof(clean) - 1; p++) {
        if (*p == '-' && n == 0) clean[n++] = *p;
        else if (*p >= '0' && *p <= '9') clean[n++] = *p;
    }
    clean[n] = '\0';
    bool sanitized = (s_strcmp(clean, raw) != 0);
    ta->sys->free(raw);
    if (sanitized) ui_textarea_set_text(ta, clean, false);

    int parsed = (int)s_to_number(clean);
    if (parsed < min_value) parsed = min_value;
    if (parsed > max_value) parsed = max_value;
    if (parsed == *value) return false;
    *value = parsed;
    return true;
}

const char *ui_textarea_str_absolute_pos(UIContext *ui, UITextArea *ta, const char *model,
                                         const char *strip, int x, int y, int w) {
    if (!model) model = "";
    if (!ta->bound || s_strcmp(ta->bound, model) != 0) {
        ui_textarea_set_text(ta, model, false);
        if (ta->bound) ta->sys->free(ta->bound);
        int n = (int)s_strlen(model);
        ta->bound = (char *)ta->sys->malloc(n + 1);
        ta->sys->memcpy(ta->bound, model, n + 1);
    }

    ui_textarea_absolute_pos(ui, ta, x, y, w, 1);

    char *raw = ui_textarea_get_text(ta);
    if (!raw) return NULL;
    if (strip) {
        int n = 0;
        for (const char *p = raw; *p; p++)
            if (!s_strchr(strip, *p)) raw[n++] = *p;
        if (raw[n]) {
            raw[n] = '\0';
            ui_textarea_set_text(ta, raw, false);
        }
    }
    if (s_strcmp(raw, ta->bound) == 0) {
        ta->sys->free(raw);
        return NULL;
    }
    ta->sys->free(ta->bound);
    ta->bound = raw;
    return raw;
}

// ===== visual layout =====

static void rebuild_layout(UITextArea *ta, int text_width) {
    if (text_width < 1) text_width = 1;
    ta->num_vis = 0;
    for (int r = 0; r < ta->num_lines; r++) {
        ensure_line_exists(ta, r);
        int L    = ta->lines[r].text ? ta->lines[r].length : 0;
        int segs = (L == 0) ? 1 : ((L + text_width - 1) / text_width);
        for (int i = 0; i < segs; i++) {
            if (ta->num_vis >= ta->cap_vis) {
                ta->cap_vis = ta->num_vis * 2 + 64;
                ta->vis = (VisualLine *)ta->sys->realloc(
                    ta->vis, sizeof(VisualLine) * ta->cap_vis);
            }
            VisualLine vl;
            vl.logical_row = r;
            vl.char_offset = i * text_width;
            vl.length      = (L == 0) ? 0 : ((i == segs - 1) ? (L - i * text_width) : text_width);
            vl.is_wrapped  = (i > 0) ? 1 : 0;
            ta->vis[ta->num_vis++] = vl;
        }
    }
}

static void get_visual_coords(UITextArea *ta, int row, int col, int text_width,
                              int *out_idx, int *out_col) {
    if (text_width < 1) text_width = 1;
    int start = -1;
    for (int i = 0; i < ta->num_vis; i++)
        if (ta->vis[i].logical_row == row) { start = i; break; }
    if (start == -1) { *out_idx = 0; *out_col = 0; return; }

    int rel_line = col / text_width;
    int rel_col  = col % text_width;
    int L    = (row < ta->num_lines && ta->lines[row].text) ? ta->lines[row].length : 0;
    int segs = (L == 0) ? 1 : ((L + text_width - 1) / text_width);
    if (rel_line >= segs) { rel_line = segs - 1; rel_col = L - rel_line * text_width; }
    *out_idx = start + rel_line;
    *out_col = rel_col;
}

static void move_by_visual_lines(UITextArea *ta, int text_width, int dir) {
    rebuild_layout(ta, text_width);
    int idx, vcol;
    get_visual_coords(ta, ta->cur_row, ta->cur_col, text_width, &idx, &vcol);
    int target = idx + dir;
    if (target < 0) { ta->cur_row = 0; ta->cur_col = 0; return; }
    if (target >= ta->num_vis) {
        ta->cur_row = ta->num_lines - 1;
        ensure_line_exists(ta, ta->cur_row);
        ta->cur_col = ta->lines[ta->cur_row].length;
        return;
    }
    VisualLine *vl = &ta->vis[target];
    ta->cur_row = vl->logical_row;
    int tcol = vcol;
    if (tcol > vl->length) tcol = vl->length;
    ta->cur_col = vl->char_offset + tcol;
    clamp_cursor(ta);
}

// ===== selection =====

static int is_selected(UITextArea *ta, int r, int c) {
    if (ta->sel_row == -1) return 0;
    int r1 = ta->sel_row, c1 = ta->sel_col;
    int r2 = ta->cur_row, c2 = ta->cur_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
    }
    if (r < r1 || r > r2) return 0;
    if (r > r1 && r < r2) return 1;
    if (r1 == r2) return (c >= c1 && c < c2);
    if (r == r1)  return (c >= c1);
    if (r == r2)  return (c < c2);
    return 0;
}

static void update_selection_before_move(UITextArea *ta, int shift) {
    if (shift) {
        if (ta->sel_row == -1) { ta->sel_row = ta->cur_row; ta->sel_col = ta->cur_col; }
    } else {
        ta->sel_row = -1; ta->sel_col = -1;
    }
}

static void delete_selection(UITextArea *ta) {
    if (ta->sel_row == -1) return;
    int r1 = ta->sel_row, c1 = ta->sel_col;
    int r2 = ta->cur_row, c2 = ta->cur_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
    }
    if (r1 == r2 && c1 == c2) { ta->sel_row = -1; return; }
    // record diff before deletion
    if (ta->group_active) {
        char *deleted = get_selection_text(ta);
        if (deleted) {
            int dlen = s_strlen(deleted);
            int w = 0;
            for (int i = 0; i < dlen; i++) if (deleted[i] != '\r') deleted[w++] = deleted[i];
            undo_record_diff(ta, UNDO_DEL, r1, c1, deleted, w);
            ta->sys->free(deleted);
        }
    }

    if (r1 == r2) {
        int len = ta->lines[r1].length;
        ui_memmove(ta->lines[r1].text + c1, ta->lines[r1].text + c2, len - c2 + 1);
        if (ta->lines[r1].tags) {
            ui_memmove((void *)(ta->lines[r1].tags + c1),
                       (void *)(ta->lines[r1].tags + c2),
                       (len - c2) * (int)sizeof(unsigned int));
        }
        ta->lines[r1].length -= (c2 - c1);
    } else {
        int len2 = ta->lines[r2].length;
        grow_line(ta, r1, c1 + len2 - c2);
        s_strcpy(ta->lines[r1].text + c1, ta->lines[r2].text + c2);
        ta->lines[r1].length = c1 + (len2 - c2);

        // Merge tags: copy tail of r2 into r1 from position c1
        if (ta->lines[r2].tags && (len2 - c2) > 0) {
            ensure_tags(ta, r1);
            ui_memcpy((void *)(ta->lines[r1].tags + c1),
                      (void *)(ta->lines[r2].tags + c2),
                      (len2 - c2) * (int)sizeof(unsigned int));
        } else if (ta->lines[r1].tags && (len2 - c2) > 0) {
            // r2 has no tags; zero those positions in r1
            for (int i = c1; i < c1 + (len2 - c2); i++) ta->lines[r1].tags[i] = 0;
        }

        int removed = r2 - r1;
        for (int i = r1 + 1; i <= r2; i++) {
            ta->sys->free(ta->lines[i].text);
            if (ta->lines[i].tags) ta->sys->free(ta->lines[i].tags);
        }
        for (int i = r1 + 1; i < ta->num_lines - removed; i++)
            ta->lines[i] = ta->lines[i + removed];
        ta->num_lines -= removed;
        for (int i = ta->num_lines; i < ta->num_lines + removed; i++) {
            ta->lines[i].text     = NULL;
            ta->lines[i].length   = 0;
            ta->lines[i].capacity = 0;
            ta->lines[i].tags     = NULL;
            ta->lines[i].lineTag  = 0;
        }
    }
    ta->cur_row = r1; ta->cur_col = c1; ta->sel_row = -1;
}

static char *get_selection_text(UITextArea *ta) {
    if (ta->sel_row == -1) return NULL;
    int r1 = ta->sel_row, c1 = ta->sel_col;
    int r2 = ta->cur_row, c2 = ta->cur_col;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
    }
    if (r1 == r2 && c1 == c2) return NULL;
    int total = 0;
    for (int r = r1; r <= r2; r++) {
        int s = (r == r1) ? c1 : 0;
        int e = (r == r2) ? c2 : ta->lines[r].length;
        total += (e - s) + 2;
    }
    char *buf = (char *)ta->sys->malloc(total + 1);
    int pos = 0;
    for (int r = r1; r <= r2; r++) {
        int s = (r == r1) ? c1 : 0;
        int e = (r == r2) ? c2 : ta->lines[r].length;
        ui_memcpy(buf + pos, ta->lines[r].text + s, e - s);
        pos += e - s;
        if (r != r2) { buf[pos++] = '\r'; buf[pos++] = '\n'; }
    }
    buf[pos] = '\0';
    return buf;
}

static void copy_to_clipboard(UITextArea *ta) {
    char *t = get_selection_text(ta);
    if (!t) return;
    platform_copy_to_clipboard(t);
    ta->sys->free(t);
}

static int paste_from_clipboard(UITextArea *ta) {
    char *t = platform_paste_from_clipboard();
    if (!t) return 0;
    if (ta->sel_row != -1) delete_selection(ta);

    int run_start_row = ta->cur_row;
    int run_start_col = ta->cur_col;
    int run_len       = 0;
    int run_pos       = 0;
    char run_buf[1024];

    for (int i = 0; ; i++) {
        char ch = t[i];
        if (ch == '\r') continue;
        if (ch == '\n' || ch == '\0') {
            if (run_pos > 0) {
                insert_chars_bulk(ta, ta->cur_row, ta->cur_col, run_buf, run_pos);
                ta->cur_col += run_pos;
                if (ta->callbacks && ta->callbacks->on_insert_text)
                    ta->callbacks->on_insert_text(ta->callbacks->user, ta,
                                                   run_start_row, run_start_col, run_len);
                run_pos = 0;
                run_len = 0;
            }
            if (ch == '\n') {
                int new_row = ta->cur_row + 1;
                insert_newline(ta, ta->cur_row, ta->cur_col);
                ta->cur_row++; ta->cur_col = 0;
                if (ta->callbacks && ta->callbacks->on_insert_line)
                    ta->callbacks->on_insert_line(ta->callbacks->user, ta, new_row);
                run_start_row = ta->cur_row;
                run_start_col = 0;
            }
            if (ch == '\0') break;
        } else {
            if (run_pos == 0) {
                run_start_row = ta->cur_row;
                run_start_col = ta->cur_col;
            }
            run_buf[run_pos++] = ch;
            run_len++;
            if (run_pos >= 1024) {
                insert_chars_bulk(ta, ta->cur_row, ta->cur_col, run_buf, run_pos);
                ta->cur_col += run_pos;
                if (ta->callbacks && ta->callbacks->on_insert_text)
                    ta->callbacks->on_insert_text(ta->callbacks->user, ta,
                                                   run_start_row, run_start_col, run_len);
                run_start_row = ta->cur_row;
                run_start_col = ta->cur_col;
                run_pos = 0;
                run_len = 0;
            }
        }
    }
    ta->sys->free(t);
    return 1;
}

static void ensure_visible(UITextArea *ta, int text_width, int usable) {
    rebuild_layout(ta, text_width);
    int idx, vcol;
    get_visual_coords(ta, ta->cur_row, ta->cur_col, text_width, &idx, &vcol);
    if (usable < 1) usable = 1;
    int   si;
    float sf;
    scroll_split(ta, &si, &sf);   // same top line the renderer will use
    if (idx < si) ta->scroll = (float)idx;
    else if (idx >= si + usable) ta->scroll = (float)(idx - usable + 1);
    if (ta->scroll < 0.0f) ta->scroll = 0.0f;
}

// ===== input =====

static int handle_key(UITextArea *ta, int keycode, int flags, int text_width) {
    int ctrl    = (flags & UI_FLAGS_CTRL)  != 0;
    int shift   = (flags & UI_FLAGS_SHIFT) != 0;
    int changed = 0;

    if (ctrl && keycode == TA_KEY_C) { copy_to_clipboard(ta); return 0; }
    if (ctrl && keycode == TA_KEY_X) { copy_to_clipboard(ta); delete_selection(ta); return 1; }
    if (ctrl && keycode == TA_KEY_V) { return paste_from_clipboard(ta); }
    if (ctrl && keycode == TA_KEY_A) {
        ta->sel_row = 0; ta->sel_col = 0;
        ta->cur_row = ta->num_lines - 1;
        ensure_line_exists(ta, ta->cur_row);
        ta->cur_col = ta->lines[ta->cur_row].length;
        return 0;
    }
    // Ctrl+Shift+Z is accepted as a redo alias alongside Ctrl+Y (common
    // convention in editors that don't use Ctrl+Y for redo at all).
    if (ctrl && shift && keycode == TA_KEY_Z) { undo_redo(ta); return 1; }
    if (ctrl && keycode == TA_KEY_Z) { undo_undo(ta); return 1; }
    if (ctrl && keycode == TA_KEY_Y) { undo_redo(ta); return 1; }

    switch (keycode) {
    case TA_KEY_LEFT:
        update_selection_before_move(ta, shift);
        if (ctrl) {
            if (ta->cur_col == 0 && ta->cur_row > 0) {
                ta->cur_row--; ta->cur_col = ta->lines[ta->cur_row].length;
            } else {
                while (ta->cur_col > 0 && ta->lines[ta->cur_row].text[ta->cur_col-1] == ' ') ta->cur_col--;
                while (ta->cur_col > 0 && ta->lines[ta->cur_row].text[ta->cur_col-1] != ' ') ta->cur_col--;
            }
        } else {
            ta->cur_col--;
            if (ta->cur_col < 0) {
                if (ta->cur_row > 0) {
                    ta->cur_row--; ensure_line_exists(ta, ta->cur_row);
                    ta->cur_col = ta->lines[ta->cur_row].length;
                } else ta->cur_col = 0;
            }
        }
        break;
    case TA_KEY_RIGHT: {
        update_selection_before_move(ta, shift);
        ensure_line_exists(ta, ta->cur_row);
        int len = ta->lines[ta->cur_row].length;
        if (ctrl) {
            if (ta->cur_col == len && ta->cur_row < ta->num_lines - 1) {
                ta->cur_row++; ta->cur_col = 0;
            } else {
                while (ta->cur_col < len && ta->lines[ta->cur_row].text[ta->cur_col] != ' ') ta->cur_col++;
                while (ta->cur_col < len && ta->lines[ta->cur_row].text[ta->cur_col] == ' ')  ta->cur_col++;
            }
        } else {
            if (ta->cur_col < len) ta->cur_col++;
            else if (ta->cur_row < ta->num_lines - 1) { ta->cur_row++; ta->cur_col = 0; }
        }
        break; }
    case TA_KEY_UP: update_selection_before_move(ta, shift); move_by_visual_lines(ta, text_width, -1); break;
    case TA_KEY_DOWN: update_selection_before_move(ta, shift); move_by_visual_lines(ta, text_width,  1); break;
    case TA_KEY_HOME: update_selection_before_move(ta, shift); ta->cur_col = 0; break;
    case TA_KEY_END: update_selection_before_move(ta, shift);
        ensure_line_exists(ta, ta->cur_row);
        ta->cur_col = ta->lines[ta->cur_row].length; break;
    case TA_KEY_PAGEUP: { update_selection_before_move(ta, shift);
        int steps = ta->rh - 1; if (steps < 1) steps = 1;
        move_by_visual_lines(ta, text_width, -steps); break; }
    case TA_KEY_PAGEDOWN: { update_selection_before_move(ta, shift);
        int steps = ta->rh - 1; if (steps < 1) steps = 1;
        move_by_visual_lines(ta, text_width, steps); break; }

    case TA_KEY_DELETE:
        if (ta->sel_row != -1) {
            delete_selection(ta);
        } else {
            ensure_line_exists(ta, ta->cur_row);
            int cur_len = ta->lines[ta->cur_row].length;
            if (ta->cur_col < cur_len) {
                if (ta->callbacks && ta->callbacks->on_delete_text)
                    ta->callbacks->on_delete_text(ta->callbacks->user, ta,
                                                  ta->cur_row, ta->cur_col, 1);
                delete_char(ta, ta->cur_row, ta->cur_col);
            } else if (ta->cur_row < ta->num_lines - 1) {
                if (ta->callbacks && ta->callbacks->on_delete_line)
                    ta->callbacks->on_delete_line(ta->callbacks->user, ta,
                                                  ta->cur_row + 1);
                join_next_line(ta, ta->cur_row);
            }
        }
        changed = 1; break;

    case TA_KEY_BACKSPACE:
        if (ta->sel_row != -1) {
            delete_selection(ta);
        } else if (ta->cur_col > 0) {
            if (ta->callbacks && ta->callbacks->on_delete_text)
                ta->callbacks->on_delete_text(ta->callbacks->user, ta,
                                              ta->cur_row, ta->cur_col - 1, 1);
            ta->cur_col--;
            delete_char(ta, ta->cur_row, ta->cur_col);
        } else if (ta->cur_row > 0) {
            if (ta->callbacks && ta->callbacks->on_delete_line)
                ta->callbacks->on_delete_line(ta->callbacks->user, ta, ta->cur_row);
            ta->cur_row--;
            ta->cur_col = ta->lines[ta->cur_row].length;
            join_next_line(ta, ta->cur_row);
        }
        changed = 1; break;

    case TA_KEY_ENTER:
        if (ta->sel_row != -1) delete_selection(ta);
        { int new_row = ta->cur_row + 1;
        insert_newline(ta, ta->cur_row, ta->cur_col);
        ta->cur_row++; ta->cur_col = 0; clamp_cursor(ta);
        if (ta->callbacks && ta->callbacks->on_insert_line)
            ta->callbacks->on_insert_line(ta->callbacks->user, ta, new_row); }
        changed = 1; break;

    case TA_KEY_TAB: {
        if (ta->sel_row != -1) {
            int r1 = ta->sel_row, c1 = ta->sel_col;
            int r2 = ta->cur_row, c2 = ta->cur_col;
            int swapped = (r1 > r2 || (r1 == r2 && c1 > c2));
            if (swapped) {
                int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
            }
            // Cursor at column 0 of a later line means that line is NOT
            // part of the selection (e.g. gutter-click places cursor at
            // the start of the line after the selection).  Skip it.
            int r2_orig = r2;
            if (r2 > r1 && c2 == 0) r2--;
            if (shift) {
                ensure_line_exists(ta, r1);
                int n1 = ta->lines[r1].length;
                int sp1 = 0;
                while (sp1 < 4 && sp1 < n1 && ta->lines[r1].text[sp1] == ' ') sp1++;
                int sp2 = 0;
                if (r2 >= r1) {
                    ensure_line_exists(ta, r2);
                    int n2 = ta->lines[r2].length;
                    while (sp2 < 4 && sp2 < n2 && ta->lines[r2].text[sp2] == ' ') sp2++;
                }
                for (int r = r1; r <= r2; r++) {
                    ensure_line_exists(ta, r);
                    int n = ta->lines[r].length;
                    int spaces = 0;
                    while (spaces < 4 && spaces < n &&
                           ta->lines[r].text[spaces] == ' ') spaces++;
                    if (spaces > 0) {
                        if (ta->callbacks && ta->callbacks->on_delete_text)
                            ta->callbacks->on_delete_text(
                                ta->callbacks->user, ta, r, 0, spaces);
                        for (int i = 0; i < spaces; i++) delete_char(ta, r, 0);
                    }
                }
                // Adjust sel/cur columns by the dedent amount on each
                // side, respecting the original left/right ordering.
                if (swapped) {
                    ta->sel_col -= sp2; if (ta->sel_col < 0) ta->sel_col = 0;
                    ta->cur_col -= sp1; if (ta->cur_col < 0) ta->cur_col = 0;
                } else {
                    ta->sel_col -= sp1; if (ta->sel_col < 0) ta->sel_col = 0;
                    ta->cur_col -= sp2; if (ta->cur_col < 0) ta->cur_col = 0;
                }
                // If the trailing line was skipped, cursor wasn't dedented.
                if (r2 != r2_orig) ta->cur_col = c2;
                clamp_cursor(ta);
            } else {
                for (int r = r1; r <= r2; r++) {
                    for (int i = 0; i < 4; i++) insert_char(ta, r, 0, ' ');
                    if (ta->callbacks && ta->callbacks->on_insert_text)
                        ta->callbacks->on_insert_text(
                            ta->callbacks->user, ta, r, 0, 4);
                }
                // Set sel_col / cur_col preserving which was left/right.
                // If the left edge was at column 0, keep it at 0 so the
                // newly-inserted indentation spaces stay selected.
                int new_left  = (c1 == 0) ? 0 : c1 + 4;
                int new_right = c2 + 4;
                if (swapped) {
                    // sel was on right side, cur on left
                    ta->sel_col = (r2 == r2_orig) ? new_right : c2;
                    ta->cur_col = new_left;
                } else {
                    // sel on left, cur on right
                    ta->sel_col = new_left;
                    ta->cur_col = (r2 == r2_orig) ? new_right : c2;
                }
                clamp_cursor(ta);
            }
        } else if (shift) {
            ensure_line_exists(ta, ta->cur_row);
            int n = ta->lines[ta->cur_row].length;
            int spaces = 0;
            while (spaces < 4 && spaces < n &&
                   ta->lines[ta->cur_row].text[spaces] == ' ') spaces++;
            if (spaces > 0) {
                if (ta->callbacks && ta->callbacks->on_delete_text)
                    ta->callbacks->on_delete_text(
                        ta->callbacks->user, ta, ta->cur_row, 0, spaces);
                for (int i = 0; i < spaces; i++)
                    delete_char(ta, ta->cur_row, 0);
                ta->cur_col -= spaces;
                if (ta->cur_col < 0) ta->cur_col = 0;
            }
            clamp_cursor(ta);
        } else {
            if (ta->sel_row != -1) delete_selection(ta);
            int r0 = ta->cur_row, c0 = ta->cur_col;
            for (int i = 0; i < 4; i++) {
                insert_char(ta, ta->cur_row, ta->cur_col, ' ');
                ta->cur_col++;
            }
            clamp_cursor(ta);
            if (ta->callbacks && ta->callbacks->on_insert_text)
                ta->callbacks->on_insert_text(ta->callbacks->user, ta, r0, c0, 4);
        }
        changed = 1; break;
    }

    default: break;
    }
    clamp_cursor(ta);
    return changed;
}

static int handle_char(UITextArea *ta, int ch, int flags) {
    int ctrl = (flags & UI_FLAGS_CTRL) != 0;

    // Note: TA_KEY_ENTER and TA_KEY_TAB are now handled in handle_key(),
    // since SDL3 may not deliver text-input events for these keys on all
    // platforms (e.g. Windows).  The key-down event is always delivered.

    if (ch >= 32 && ch < 127 && !ctrl) {
        if (ta->sel_row != -1) delete_selection(ta);
        int r0 = ta->cur_row, c0 = ta->cur_col;
        insert_char(ta, ta->cur_row, ta->cur_col, (char)ch);
        ta->cur_col++; clamp_cursor(ta);
        if (ta->callbacks && ta->callbacks->on_insert_text)
            ta->callbacks->on_insert_text(ta->callbacks->user, ta, r0, c0, 1);
        return 1;
    }
    return 0;
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void select_word_at(UITextArea *ta, int row, int col) {
    if (row < 0 || row >= ta->num_lines) return;
    char *line = ta->lines[row].text;
    int len = line ? ta->lines[row].length : 0;
    if (len == 0 || col < 0 || col >= len) { ta->sel_row = -1; ta->sel_col = -1; return; }
    if (!is_word_char(line[col])) { ta->sel_row = -1; ta->sel_col = -1; return; }
    int start = col, end = col;
    while (start > 0 && is_word_char(line[start-1])) start--;
    while (end < len && is_word_char(line[end])) end++;
    ta->word_anchor_row   = row;
    ta->word_anchor_start = start;
    ta->word_anchor_end   = end;
    ta->sel_row = row; ta->sel_col = start;
    ta->cur_row = row; ta->cur_col = end;
    ta->drag_by_word = true;
}

static void place_cursor_from_mouse(UITextArea *ta, int text_width,
                                    float mcol_f, float mrow_f, int extend) {
    rebuild_layout(ta, text_width);
    if (ta->num_vis <= 0) return;
    int vis_idx = vis_idx_at_rel_row(ta, mrow_f);
    // Clicking below the last line puts the cursor at the very end of the text
    // (and above the first line snaps to the first line), rather than doing nothing.
    int past_end = 0;
    if (vis_idx < 0) vis_idx = 0;
    if (vis_idx >= ta->num_vis) { vis_idx = ta->num_vis - 1; past_end = 1; }
    VisualLine *vl = &ta->vis[vis_idx];
    int col = past_end ? vl->char_offset + vl->length
                       : (int)(mcol_f + 0.5f) + vl->char_offset;
    if (col < vl->char_offset) col = vl->char_offset;
    if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;
    if (extend) {
        if (ta->sel_row == -1) { ta->sel_row = ta->cur_row; ta->sel_col = ta->cur_col; }
    } else {
        ta->sel_row = -1; ta->sel_col = -1;
    }
    ta->cur_row = vl->logical_row;
    ta->cur_col = col;
    clamp_cursor(ta);
}

static void cursor_in_viz(UITextArea *ta, int x, int y, int w, int h,
                          int text_width, int *out_vis_row, int *out_vis_col) {
    int idx, vcol;
    get_visual_coords(ta, ta->cur_row, ta->cur_col, text_width, &idx, &vcol);
    int   scroll_int;
    float scroll_frac;
    scroll_split(ta, &scroll_int, &scroll_frac);
    // Mirror the render loop exactly: line scroll_int+k is drawn at
    // sy = y - frac + k, and ui_draw_cell_flags rounds that to a cell row.
    float sy_line = (float)y + (float)(idx - scroll_int) - scroll_frac;
    int   crow    = (int)(sy_line + 0.5f) - y;
    int ccol = ta->gutter + vcol;
    if (crow >= 0 && crow < h && ccol >= 0 && ccol < w) {
        *out_vis_col = x + ccol;
        *out_vis_row = y + crow;
    } else {
        *out_vis_col = -1;
        *out_vis_row = -1;
    }
}

// ===== render + widget =====

// The two layout-participating entry points, thin over the pinned forms below.
// Those forms' own clamps (w>=2, h>=1) are repeated here rather than guessed at:
// the pen has to advance by the rectangle that was really drawn, or a 1-wide
// field would leave the next row one cell high on paper and two on screen.
int ui_textarea(UIContext *ui, UITextArea *ta, int w, int h) {
    if (w < 2) w = 2;
    if (h < 1) h = 1;
    float x = ui->pen_x, y = ui->pen_y;
    int changed = ui_textarea_absolute_pos(ui, ta, (int)x, (int)y, w, h);
    ui_advance(ui, x, y, (float)w, (float)h);
    return changed;
}

const char *ui_textarea_str(UIContext *ui, UITextArea *ta, const char *model,
                            const char *strip, int w) {
    if (w < 2) w = 2;
    float x = ui->pen_x, y = ui->pen_y;
    const char *edited = ui_textarea_str_absolute_pos(ui, ta, model, strip, (int)x, (int)y, w);
    ui_advance(ui, x, y, (float)w, 1.0f);
    return edited;
}

bool ui_textarea_int(UIContext *ui, UITextArea *ta, int *value,
                     int min_value, int max_value, int w) {
    if (w < 2) w = 2;
    float x = ui->pen_x, y = ui->pen_y;
    bool changed = ui_textarea_int_absolute_pos(ui, ta, value, min_value, max_value,
                                                (int)x, (int)y, w);
    ui_advance(ui, x, y, (float)w, 1.0f);
    return changed;
}

int ui_textarea_absolute_pos(UIContext *ui, UITextArea *ta, int x, int y, int w, int h) {
    // The scope blanks the mouse and the key queue, which is all this widget
    // needs: it reads them directly rather than going through ui_hit(), so
    // every mouse and keyboard path below goes quiet on its own. Only the two
    // things that don't read input -- the default focus grab and the caret --
    // need saying explicitly.
    int disabled = ui_item_disable_begin(ui);
    if (w < 2) w = 2;
    if (h < 1) h = 1;
    int id = ui_id_from_ptr(ta);
    if (!disabled && ui->focus_id == 0) ui->focus_id = id;
    ta->gutter = ta->show_line_numbers ? 4 : 0;
    ta->rx = x; ta->ry = y; ta->rw = w; ta->rh = h;

    int content_h = h - (ta->show_bottom_status ? 1 : 0);
    if (content_h < 1) content_h = 1;
    int content_h_draw = content_h;//ta->enable_fractional_scroll ? content_h - 1 : content_h;
    if (content_h_draw < 1) content_h_draw = 1;

    // First-pass layout to determine if scrollbar is needed.
// Show scrollbar when content overflows OR the view is scrolled away from top.
    int text_width = w - ta->gutter;
    if (text_width < 1) text_width = 1;
    rebuild_layout(ta, text_width);
    int has_scrollbar = (ta->num_vis > content_h) || (ta->scroll > 0);
    int scrollbar_w = has_scrollbar ? 1 : 0;

    // If scrollbar is needed, recompute layout with reduced width for word-wrap
    if (has_scrollbar) {
        text_width = w - ta->gutter - scrollbar_w;
        if (text_width < 1) text_width = 1;
        rebuild_layout(ta, text_width);
    }
    ta->text_width = text_width;

    // A scroll offset past the last visual line draws nothing at all -- the
    // render loop below simply finds no line at scroll_int+k. The wheel and
    // scrollbar paths clamp themselves, but two others can leave it out of
    // range: ui_textarea_set_scroll (no layout to clamp against yet) and
    // ui_textarea_set_text (which preserves the offset over text that may now
    // be shorter). Clamp once here, against the layout everything below uses.
    {
        float max_scroll = (float)(ta->num_vis - 1);
        if (max_scroll < 0.0f) max_scroll = 0.0f;
        if (ta->scroll > max_scroll) ta->scroll = max_scroll;
        if (ta->scroll < 0.0f)       ta->scroll = 0.0f;
    }

    // Pre-check: is the cursor cell free (no overlay drawn there yet)?
    // (no caret when the window itself doesn't have keyboard focus)
    bool cursor_visible = false;
    if (!disabled && ui->focus_id == id && ui->window_has_focus) {
        int viz_cursor_row, viz_cursor_col;
        cursor_in_viz(ta, x, y, w, h, text_width, &viz_cursor_row, &viz_cursor_col);
        if(viz_cursor_col >= 0 && viz_cursor_row >=0)
            cursor_visible = !ui_cell_was_drawn(ui, viz_cursor_col, viz_cursor_row);
    }

    int changed = 0;

    // ---- mouse ----
    int mc  = (int)ui->mouse_x - x;
    int mr  = (int)ui->mouse_y - y;
    float mcol_f = ui->mouse_x - (float)(x + ta->gutter);
    // Row lookups go through the float offset (not mr) so a fractional scroll
    // position resolves to the same visual line the renderer drew there.
    float mrow_f = ui->mouse_y - (float)y;
    // Exclude scrollbar column from text-area mouse handling.
// Wheel events still work over the full area (see below).
    int inside   = (mc >= 0 && mc < w - scrollbar_w && mr >= 0 && mr < content_h);

    // A press only reaches the textarea if no other control already captured the
    // mouse this frame. Widgets drawn before us grab on the press edge (ui_hit
    // -> ui_set_active_control), and the textarea is drawn late, so without this
    // a click on something sitting right against our rect -- the editor-split
    // divider, say -- would both drive that control AND steal focus in here.
    int can_take_press = (ui->active_id == 0 || ui->active_id == id);
    if (ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT] && inside && can_take_press) {
        ui->focus_id  = id;
        ui_set_active_control(ui, id, NULL);
        if (ui->mouse_flags & UI_FLAGS_DOUBLE_CLICK) {
            rebuild_layout(ta, text_width);
            int vis_idx = vis_idx_at_rel_row(ta, mrow_f);
            if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                VisualLine *vl = &ta->vis[vis_idx];
                int col = (int)(mcol_f + 0.5f) + vl->char_offset;
                if (col < vl->char_offset) col = vl->char_offset;
                if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;
                select_word_at(ta, vl->logical_row, col);
            }
        } else if (mc < ta->gutter) {
            ta->drag_by_word  = false;
            ta->gutter_select = true;
            rebuild_layout(ta, text_width);
            int vis_idx = vis_idx_at_rel_row(ta, mrow_f);
            if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                VisualLine *vl = &ta->vis[vis_idx];
                int lrow = vl->logical_row;
                int len  = (lrow < ta->num_lines && ta->lines[lrow].text) ? ta->lines[lrow].length : 0;
                ta->gutter_anchor_row = lrow;
                ta->sel_row = lrow; ta->sel_col = 0;
                if (lrow + 1 < ta->num_lines) {
                    ta->cur_row = lrow + 1; ta->cur_col = 0;
                } else {
                    ta->cur_row = lrow; ta->cur_col = len;
                }
            }
        } else {
            ta->drag_by_word  = false;
            ta->gutter_select = false;
            // Check if click is on existing selection (for drag-and-drop)
            bool shift_held = (ui->mouse_flags & UI_FLAGS_SHIFT) != 0;
            if (!shift_held && ta->sel_row != -1) {
                rebuild_layout(ta, text_width);
                int vis_idx = vis_idx_at_rel_row(ta, mrow_f);
                int click_on_sel = 0;
                if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                    VisualLine *vl = &ta->vis[vis_idx];
                    int col = (int)(mcol_f + 0.5f) + vl->char_offset;
                    if (col < vl->char_offset) col = vl->char_offset;
                    if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;
                    if (is_selected(ta, vl->logical_row, col)) {
                        click_on_sel = 1;
                        ta->mousedown_on_selection = true;
                        ta->drag_active = false;
                        ta->drag_press_row = vl->logical_row;
                        ta->drag_press_col = col;
                    }
                }
                if (!click_on_sel) {
                    place_cursor_from_mouse(ta, text_width, mcol_f, mrow_f, 0);
                }
            } else {
                place_cursor_from_mouse(ta, text_width, mcol_f, mrow_f,
                                        (ui->mouse_flags & UI_FLAGS_SHIFT));
            }
        }
    } else if (ui->active_id == id && ui->mouse_down[UI_MOUSE_BUTTON_LEFT] &&
               (ui->mouse_dx != 0.0f || ui->mouse_dy != 0.0f)) {
        float cmrow_f = mrow_f;
        if (cmrow_f < 0.0f) cmrow_f = 0.0f;
        if (cmrow_f > (float)h - 0.001f) cmrow_f = (float)h - 0.001f;

        if (ta->mousedown_on_selection) {
            // Drag starting from selected text
            if (!ta->drag_active) {
                // First drag frame: save original selection (normalised)
                ta->drag_active = true;
                int r1 = ta->sel_row, c1 = ta->sel_col;
                int r2 = ta->cur_row, c2 = ta->cur_col;
                if (r1 > r2 || (r1 == r2 && c1 > c2)) {
                    int t = r1; r1 = r2; r2 = t; t = c1; c1 = c2; c2 = t;
                }
                ta->drag_orig_sel_row = r1; ta->drag_orig_sel_col = c1;
                ta->drag_orig_cur_row = r2; ta->drag_orig_cur_col = c2;
            }
            // Update target position from mouse (don't alter actual cursor/selection yet)
            {
                float clamped_col_f = ui->mouse_x - (float)(x + ta->gutter);
                if (clamped_col_f < 0.0f) clamped_col_f = 0.0f;
                if (clamped_col_f > (float)(text_width-1)+0.99f)
                    clamped_col_f = (float)(text_width-1)+0.99f;
                rebuild_layout(ta, text_width);
                int vis_idx = vis_idx_at_rel_row(ta, cmrow_f);
                if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                    VisualLine *vl = &ta->vis[vis_idx];
                    int col = (int)(clamped_col_f + 0.5f) + vl->char_offset;
                    if (col < vl->char_offset) col = vl->char_offset;
                    if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;
                    ta->drag_target_row = vl->logical_row;
                    ta->drag_target_col = col;
                }
            }
        } else {
            if (ta->sel_row == -1) { ta->sel_row = ta->cur_row; ta->sel_col = ta->cur_col; }
            if (ta->drag_by_word) {
                rebuild_layout(ta, text_width);
                int vis_idx = vis_idx_at_rel_row(ta, cmrow_f);
                if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                    VisualLine *vl = &ta->vis[vis_idx];
                    int lrow = vl->logical_row;
                    float cmcol_f = ui->mouse_x - (float)(x + ta->gutter);
                    int col = (int)(cmcol_f + 0.5f) + vl->char_offset;
                    if (col < vl->char_offset) col = vl->char_offset;
                    if (col > vl->char_offset + vl->length) col = vl->char_offset + vl->length;
                    if (lrow < ta->num_lines && ta->lines[lrow].text) {
                        char *line = ta->lines[lrow].text;
                        int len = ta->lines[lrow].length;
                        if (col > len) col = len;
                        int wstart = col, wend = col;
                        if (len > 0 && col >= 0 && col < len) {
                            if (is_word_char(line[col])) {
                                while (wstart > 0 && is_word_char(line[wstart-1])) wstart--;
                                while (wend < len && is_word_char(line[wend])) wend++;
                            } else {
                                while (wstart > 0 && !is_word_char(line[wstart-1])) wstart--;
                                while (wend < len && !is_word_char(line[wend])) wend++;
                            }
                        }
                        int is_before = (lrow < ta->word_anchor_row ||
                                         (lrow == ta->word_anchor_row && col < ta->word_anchor_start));
                        if (is_before) {
                            ta->sel_row = ta->word_anchor_row; ta->sel_col = ta->word_anchor_end;
                            ta->cur_row = lrow;                ta->cur_col = wstart;
                        } else {
                            ta->sel_row = ta->word_anchor_row; ta->sel_col = ta->word_anchor_start;
                            ta->cur_row = lrow;                ta->cur_col = wend;
                        }
                    } else {
                        ta->cur_row = lrow; ta->cur_col = col;
                    }
                    clamp_cursor(ta);
                }
            } else if (ta->gutter_select) {
                rebuild_layout(ta, text_width);
                int vis_idx = vis_idx_at_rel_row(ta, cmrow_f);
                if (vis_idx >= 0 && vis_idx < ta->num_vis) {
                    VisualLine *vl = &ta->vis[vis_idx];
                    int lrow   = vl->logical_row;
                    int anchor = ta->gutter_anchor_row;
                    int anchor_len = (anchor < ta->num_lines && ta->lines[anchor].text)
                                     ? ta->lines[anchor].length : 0;
                    int drag_len   = (lrow   < ta->num_lines && ta->lines[lrow].text)
                                     ? ta->lines[lrow].length : 0;
                    if (lrow <= anchor) {
                        ta->cur_row = lrow;   ta->cur_col = 0;
                        if (anchor + 1 < ta->num_lines) {
                            ta->sel_row = anchor + 1; ta->sel_col = 0;
                        } else {
                            ta->sel_row = anchor; ta->sel_col = anchor_len;
                        }
                    } else {
                        ta->sel_row = anchor; ta->sel_col = 0;
                        if (lrow + 1 < ta->num_lines) {
                            ta->cur_row = lrow + 1; ta->cur_col = 0;
                        } else {
                            ta->cur_row = lrow; ta->cur_col = drag_len;
                        }
                    }
                    clamp_cursor(ta);
                }
            } else {
                float clamped_col_f = ui->mouse_x - (float)(x + ta->gutter);
                if (clamped_col_f < 0.0f) clamped_col_f = 0.0f;
                if (clamped_col_f > (float)(text_width-1)+0.99f)
                    clamped_col_f = (float)(text_width-1)+0.99f;
                place_cursor_from_mouse(ta, text_width, clamped_col_f, cmrow_f, 1);
            }
        }
    }
    // ---- mouse release handling ----
    if (ui->mouse_released[UI_MOUSE_BUTTON_LEFT]) {
        if (ui->active_id == id) {
            if (ta->drag_active) {
                // Finished dragging: move the selected text to the target
                int r1 = ta->drag_orig_sel_row, c1 = ta->drag_orig_sel_col;
                int r2 = ta->drag_orig_cur_row, c2 = ta->drag_orig_cur_col;
                int tr = ta->drag_target_row, tc = ta->drag_target_col;

                // Get the selected text
                char *moved_text = get_selection_text(ta);
                if (moved_text) {
                    // Check if target is inside the original selection -> no-op
                    bool inside = false;
                    if (r1 == r2) {
                        inside = (tr == r1 && tc > c1 && tc < c2);
                    } else {
                        inside = (tr > r1 && tr < r2) ||
                                 (tr == r1 && tc >= c1) ||
                                 (tr == r2 && tc < c2);
                    }
                    if (!inside) {
                        undo_begin_edit(ta);
                        ta->undo_saving = true;

                        // Delete the original selection
                        ta->sel_row = r1; ta->sel_col = c1;
                        ta->cur_row = r2; ta->cur_col = c2;
                        delete_selection(ta);

                        // Adjust target after deletion
                        if (r1 == r2) {
                            // Single-line deletion: if target is on same line and after deletion point, shift left
                            if (tr == r1 && tc > c1) {
                                if (tc >= c2)
                                    tc -= (c2 - c1);
                                else
                                    tc = c1; // target was inside deleted range; clamp to start
                            }
                        } else {
                            // Multi-line deletion
                            if (tr > r2) {
                                tr -= (r2 - r1);
                            } else if (tr == r2 && tc >= c2) {
                                // Tail of r2 merged into r1 at c1
                                tc = c1 + (tc - c2);
                                tr = r1;
                            }
                            // else: before selection -> no adjustment
                        }

                        // Insert moved text at adjusted target
                        // Use direct operations (suppress undo since we manage it externally)
                        int ins_start_row = tr;
                        int ins_start_col = tc;
                        int run_pos = 0;
                        char buf[1024];
                        for (int i = 0; ; i++) {
                            char c = moved_text[i];
                            if (c == '\r') continue;
                            if (c == '\n' || c == '\0') {
                                if (run_pos > 0) { insert_chars_bulk(ta, tr, tc, buf, run_pos); tc += run_pos; run_pos = 0; }
                                if (c == '\n') { insert_newline(ta, tr, tc); tr++; tc = 0; }
                                if (c == '\0') break;
                            } else {
                                buf[run_pos++] = c;
                                if (run_pos >= 1024) { insert_chars_bulk(ta, tr, tc, buf, run_pos); tc += run_pos; run_pos = 0; }
                            }
                        }

                        ta->undo_saving = false;
                        undo_end_edit(ta);

                        // Select the moved/inserted text
                        ta->sel_row = ins_start_row;
                        ta->sel_col = ins_start_col;
                        ta->cur_row = tr;
                        ta->cur_col = tc;
                        clamp_cursor(ta);

                        if (ta->callbacks && ta->callbacks->on_content_updated)
                            ta->callbacks->on_content_updated(ta->callbacks->user, ta);
                    }
                    ta->sys->free(moved_text);
                }
            } else if (ta->mousedown_on_selection) {
                // Click on selected text without drag: deselect and place cursor
                place_cursor_from_mouse(ta, text_width, mcol_f, mrow_f, 0);
            }
            ui_set_active_control(ui, 0, NULL);
        }
        ta->gutter_select = false;
        ta->mousedown_on_selection = false;
        ta->drag_active = false;
    }
    // Cleanup when button is not down (continuous, not just on release)
    if (!ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) {
        if (ui->active_id == id) ui_set_active_control(ui, 0, NULL);
        ta->gutter_select = false;
        ta->mousedown_on_selection = false;
        ta->drag_active = false;
    }

    {
        int inside_wheel = (mc >= 0 && mc < w && mr >= 0 && mr < h);
        if (inside_wheel && ui->wheel != 0.0f) {
            float old_scroll = ta->scroll;
            rebuild_layout(ta, text_width);
            // Proportional, not a fixed step per event: one mouse notch is
            // wheel == +/-1 and still moves exactly 3 lines, while a trackpad's
            // sub-notch deltas move a fraction of a line and accumulate in
            // ta->scroll instead of jumping 3 lines per event.
            ta->scroll -= ui->wheel * 3.0f;
            if (ta->scroll < 0.0f) ta->scroll = 0.0f;
            int maxs = ta->num_vis - 1; if (maxs < 0) maxs = 0;
            if (ta->scroll > (float)maxs) ta->scroll = (float)maxs;
            if (old_scroll != ta->scroll && ta->callbacks && ta->callbacks->on_scroll)
                ta->callbacks->on_scroll(ta->callbacks->user, ta);
        }
    }

    // ---- keyboard ----
    if (ui->focus_id == id) {
        bool has_events = (ui->num_key_events > 0 || ui->num_char_events > 0);
        if (has_events)
            undo_begin_edit(ta);

        for (int i = 0; i < ui->num_key_events; i++)
            changed |= handle_key(ta, ui->key_events[i], ui->key_flags[i], text_width);
        for (int i = 0; i < ui->num_char_events; i++)
            changed |= handle_char(ta, ui->char_events[i], ui->char_flags[i]);
        if (changed) {
            if (ta->callbacks && ta->callbacks->on_content_updated)
                ta->callbacks->on_content_updated(ta->callbacks->user, ta);
            undo_end_edit(ta);
            ensure_visible(ta, text_width, content_h_draw);
        } else {
            undo_abort_edit(ta);
            if (ui->num_key_events) ensure_visible(ta, text_width, content_h_draw);
        }
    }

    // ---- render ----
    rebuild_layout(ta, text_width);
    ta->text_width = text_width;

    // ---- scrollbar (drawn first so content blank rows don't claim its cells) ----
    if (has_scrollbar) {
        int max_scroll = ta->num_vis - 1;
        if (max_scroll < 0) max_scroll = 0;
        float scroll_val = max_scroll > 0
            ? (float)ta->scroll / (float)max_scroll : 0.0f;
        float old_scroll = ta->scroll;
        float hs = (float)content_h / (float)(ta->num_vis + content_h - 1);
        if (ui_vscroll(ui, &scroll_val, ui_id_from_ptr(ta) ^ 999, hs, x + w - 1, y, content_h)) {
            ta->scroll = scroll_val * (float)max_scroll;
            if (ta->scroll < 0.0f) ta->scroll = 0.0f;
            if (ta->scroll > (float)max_scroll) ta->scroll = (float)max_scroll;
        }
        if (old_scroll != ta->scroll && ta->callbacks && ta->callbacks->on_scroll)
            ta->callbacks->on_scroll(ta->callbacks->user, ta);
    }

    // Track the cursor's float position during the render loop
    int   cursor_vis_idx = -1, cursor_vis_col_off = 0;
    float cursor_draw_sx = -1.0f, cursor_draw_sy = -1.0f;
    if (cursor_visible && ui->focus_id == id)
        get_visual_coords(ta, ta->cur_row, ta->cur_col, text_width,
                          &cursor_vis_idx, &cursor_vis_col_off);

    // Same split the mouse hit tests use (see scroll_split) -- the two must
    // agree or clicks land on a different line than the one drawn there.
    int   scroll_int;
    float scroll_frac;
    scroll_split(ta, &scroll_int, &scroll_frac);
    if (ta->enable_fractional_scroll && scroll_frac >= 0.f) content_h_draw--;
    
    // ---- drag drop text target indicator ----
    if (ta->drag_active) {
        ui_draw_cell(ui, ui->mouse_x-0.5f, ui->mouse_y-0.5f, ' ',
            ta_col_cursor(ui));
    }

    // sy starts one row BEFORE the first line; on_draw_line_begin advances it.
    // This allows the callback to control line height.
    float sy = (float)y - 1.0f - scroll_frac;  
    for (int row = 0; sy + 1.0f < (float)(y + content_h_draw); row++) {
        int vis_idx = scroll_int + row;

        if (vis_idx < ta->num_vis) {
            VisualLine *vl   = &ta->vis[vis_idx];
            int lrow         = vl->logical_row;
            unsigned int rowcol = (lrow % 2 == 0) ? ta_col_text_alt(ui) : ta_col_text(ui);
            float sx = (float)(x + ta->gutter);

            // --- advance sy (line-begin callback or default +1) ---
            if (ta->callbacks && ta->callbacks->on_draw_line_begin) {
                sy += ta->callbacks->on_draw_line_begin(
                    ta->callbacks->user, ui, ta, lrow, vl->char_offset, sx, sy);
            } else {
                sy += 1.0f;
            }

            // Record cursor position if this is the cursor's visual row
            if (vis_idx == cursor_vis_idx) {
                cursor_draw_sy = sy;
                cursor_draw_sx = (float)(x + ta->gutter + cursor_vis_col_off);
            }
            
            unsigned int clipflags = 0;

            if(ta->enable_fractional_scroll) {
                if(row == 0) 
                    clipflags = CELL_FLAGS_CLIP_TOP;
                else if((int)(sy) == (int)(y - 1.f + content_h_draw))
                    clipflags = CELL_FLAGS_CLIP_BOTTOM;
            }

            // --- gutter ---
            if (ta->gutter > 0) {
                char ln[8] = "    ";
                if (!vl->is_wrapped) {
                    int v = lrow + 1, p = 2;
                    if (v <= 0) { ln[p] = '0'; }
                    else { while (v > 0 && p >= 0) { ln[p--] = (char)('0' + v % 10); v /= 10; } }
                }
                for (int c = 0; c < ta->gutter; c++)
                    ui_draw_cell_flags(ui, (float)(x + c), sy,
                                 (unsigned char)ln[c < 4 ? c : 3], 
                                 ta_col_gutter(ui), clipflags);
            }

            // --- text cells ---
            char *txt = ta->lines[lrow].text;
            bool is_last_seg = (vl->char_offset + vl->length == ta->lines[lrow].length);


            for (int c = 0; sx < (float)(x + ta->gutter + text_width); c++) {
                unsigned char ch = ' ';
                unsigned int color = rowcol;
                int lcol         = vl->char_offset + c;
                bool drawing_content = (txt && c < vl->length);

                if (drawing_content) {
                    ch = (unsigned char)txt[lcol];
                    if (is_selected(ta, lrow, lcol))
                        color = ta_col_selection(ui);
                }

                // Also invoke the override for the one position past the end of
// the last visual segment when lineTag is set (end-of-line animation).
                bool call_override = drawing_content ||
                    (!drawing_content && c == vl->length && is_last_seg &&
                     ta->lines[lrow].lineTag != 0);

                if (call_override && ta->callbacks && ta->callbacks->draw_cell_override) {
                    sx += ta->callbacks->draw_cell_override(
                        ta->callbacks->user, ui, ta, lrow, lcol, sx, sy, ch, color, clipflags);
                } else {
                    ui_draw_cell_flags(ui, sx, sy, ch, color, clipflags);
                    sx += 1.0f;
                }
            }

            // Trailing newline selection highlight
            {
                int L = ta->lines[lrow].length;
                if (vl->char_offset + vl->length == L && is_selected(ta, lrow, L)) {
                    float eol_sx = (float)(x + ta->gutter) + (float)vl->length;
                    if (eol_sx < (float)(x + w - scrollbar_w))
                        ui_draw_cell(ui, eol_sx, sy, ' ',
                            ta_col_selection(ui));
                }
            }




        } else {
            // Past all content: draw blank row
            sy += 1.0f;
            for (int c = 0; c < w - scrollbar_w; c++)
                ui_draw_cell(ui, (float)(x + c), sy, ' ',
                    ta_col_gutter(ui));
        }
    }

    // ---- cursor ----
    if (!ta->drag_active && cursor_visible && ui->focus_id == id && cursor_draw_sy >= 0.0f) {
        // Round exactly like ui_draw_cell_flags did for the text cells: with
        // fractional scroll sy is not integral, and truncating here put the
        // caret a row off from its own character whenever frac(sy) >= 0.5.
        int icsx = (int)(cursor_draw_sx + 0.5f);
        int icsy = (int)(cursor_draw_sy + 0.5f);
        if (icsy >= y && icsy < y + h && icsx >= x && icsx < x + w) {
            OutputCell cell;
            ui_get_cell(ui, icsx, icsy, &cell);
            //int blink = ((int)(ui->time * 2.0f)) & 1;
            //if (blink)
                ui_draw_cell_raw(ui, icsx, icsy, cell.ch,
                    ta_col_cursor(ui),
                    ui->global_scale, ui->global_weight,
                    cell.offset_x, cell.offset_y, true, 0);
        }
    }



    // ---- bottom status line ----
    if (ta->show_bottom_status) {
        int  total_chars = 0;
        for (int i = 0; i < ta->num_lines; i++) total_chars += ta->lines[i].length;

        // Build status string: Ln %d, Col %d | %d Lines, %d Chars
        char  status[128];
        int   sp = 0;
        char  num[16];

        // Helper to append a literal string
        #define APPEND_STR(s) do { const char *__s = (s); while (*__s) status[sp++] = *__s++; } while(0)
        // Helper to append an integer (returns number of chars written)
        #define APPEND_INT(v) do { \
            int __v = (v); \
            if (__v == 0) { status[sp++] = '0'; } \
            else { \
                int __tp = 0; \
                if (__v < 0) { status[sp++] = '-'; __v = -__v; } \
                while (__v > 0) { num[__tp++] = '0' + (__v % 10); __v /= 10; } \
                while (__tp > 0) { status[sp++] = num[--__tp]; } \
            } \
        } while(0)

        APPEND_STR("Ln ");
        APPEND_INT(ta->cur_row + 1);
        APPEND_STR(", Col ");
        APPEND_INT(ta->cur_col + 1);
        APPEND_STR(" | ");
        APPEND_INT(ta->num_lines);
        APPEND_STR(" Lines, ");
        APPEND_INT(total_chars);
        APPEND_STR(" Chars");
        status[sp] = '\0';

        #undef APPEND_STR
        #undef APPEND_INT

        // Draw text first, then fill background behind it
        float sy = (float)(y + h - 1);
        unsigned int status_col = ui_theme_color(ui, UI_COL_BUTTON_ACT);
        ui_draw_text(ui, (float)x, sy, status, status_col);
        ui_fill_rect(ui, (float)x, sy, w, 1, ' ', status_col);
    }

    ui_item_disable_end(ui, disabled);
    return changed;
}
