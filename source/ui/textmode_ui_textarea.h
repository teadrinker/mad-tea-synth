#ifndef TEXTMODE_UI_TEXTAREA_H
#define TEXTMODE_UI_TEXTAREA_H

#include "textmode_ui.h"

// Multi-line text input control for the textmode UI.
//
// A UITextArea owns its own text buffer (array of heap lines), cursor,
// selection and scroll state. It renders into a UIContext screen buffer and
// consumes the context's buffered keyboard/mouse events when focused.
//
// Each character has an optional uint "tag" (tag==0 is the default/null state).
// Tags survive edits -- the tag array is shifted/split/merged in lock-step with
// the text. Clients read/write tags via the public API; the textarea never
// interprets them.
//
// Callbacks (UITextAreaCallbacks) let a client:
// - react to content changes (insert/delete text and lines)
// - override per-cell rendering (custom colours, bold, animations ...)
// - customise per-line vertical layout

typedef struct UITextArea    UITextArea;
typedef struct UIContext      UIContext;  // forward-declared in textmode_ui.h

// ===== Client callback table =====
typedef struct {
    // delete text: fired BEFORE the text is removed.
// For normal editing length==1; paste may fire multiple times with larger chunks.
    void (*on_delete_text)(void *user, UITextArea *ta, int row, int col, int length);

    // insert text: fired AFTER the text is inserted.
    void (*on_insert_text)(void *user, UITextArea *ta, int row, int col, int length);

    // delete line: fired BEFORE the line is removed (line join).
    void (*on_delete_line)(void *user, UITextArea *ta, int row);

    // insert line: fired AFTER the new line is inserted (Enter / paste newline).
    void (*on_insert_line)(void *user, UITextArea *ta, int row);

    // fired once after all edits in a frame have been applied.
    void (*on_content_updated)(void *user, UITextArea *ta);

    // fired when scroll position changes (wheel, scrollbar drag, etc.).
// The user may read ta->scroll to get the new integer scroll offset.
    void (*on_scroll)(void *user, UITextArea *ta);

    // Called at the start of each visual line.
// sx = left edge of text area (after gutter), sy = row position BEFORE this line.
// Return how much to advance sy (normally 1.0).  Default if NULL: return 1.0.
    float (*on_draw_line_begin)(void *user, UIContext *ui, UITextArea *ta,
                                int lrow, int lcol, float sx, float sy);

    // Per-cell render override for content characters (drawing_content==true) and,
// for the first position past the end of the last visual segment when lineTag!=0.
// ch / color are what the textarea would draw by default (color is packed format:
// fg|(gradient<<8)|(bg<<16)).
// Return how much to advance sx (normally 1.0).
    float (*draw_cell_override)(void *user, UIContext *ui, UITextArea *ta,
                                int lrow, int lcol,
                                float sx, float sy,
                                unsigned char ch, unsigned int color, unsigned int flags);

    // Fired when the user presses a mouse button inside the textarea.
// Return true (default) to let the textarea handle the click (set cursor,
// start selection, etc.).  Return false to skip -- the caller may use this
// to reserve certain cells for overlay widgets (e.g. constant scrubbers).
// row/col are in logical (source) coordinates at the clicked cell.
    bool (*on_mouse_press)(void *user, UITextArea *ta, int row, int col, int mouse_button);

    void *user;
} UITextAreaCallbacks;


// ===== Lifecycle =====
UITextArea *ui_textarea_create(Tsys *sys);
void        ui_textarea_destroy(UITextArea *ta);

// Register (or clear) the callback table.  The pointer must remain valid
// for the lifetime of the textarea or until replaced.
void ui_textarea_set_callbacks(UITextArea *ta, UITextAreaCallbacks *callbacks);

// Return the current scroll offset (integer position, fractional ignored).
float ui_textarea_get_scroll(UITextArea *ta);

// Set the scroll offset, in visual-line units (the same value get_scroll
// returns). Negative values clamp to 0; an offset past the end of the text is
// clamped on the next draw call, once there is a layout to clamp
// against. For restoring a saved view position -- to *follow* the cursor, move
// the cursor instead and let the widget scroll to it.
void  ui_textarea_set_scroll(UITextArea *ta, float scroll);

// Replace contents (newline-separated).  Preserves lines that are identical;
// only the differing middle portion is replaced.  Tags of identical lines survive.
// Undo history is preserved when the change is partial; a full rebuild resets undo.
// If clear_history is true, the entire text is rebuilt from scratch and undo is
// reset (use for initial content).
void  ui_textarea_set_text(UITextArea *ta, const char *text, bool clear_history);
// Return a freshly-allocated copy (caller frees via sys->free).
char *ui_textarea_get_text(UITextArea *ta);

// ===== Options =====
void ui_textarea_set_show_line_numbers(UITextArea *ta, bool show);
void ui_textarea_set_show_bottom_status(UITextArea *ta, bool show);
void ui_textarea_set_enable_fractional_scroll(UITextArea *ta, bool enable);

// ===== Introspection =====
int  ui_textarea_line_count(UITextArea *ta);
int  ui_textarea_line_length(UITextArea *ta, int lrow);
void ui_textarea_get_cursor(UITextArea *ta, int *row, int *col);

// Move the cursor to (row, col), clamped to the current text, and drop any
// selection. For restoring a saved caret position (see e.g. the plugin-state
// restore in apps/madteasynth) -- for programmatic *edits* use
// ui_textarea_insert_string / ui_textarea_delete_range instead, which move the
// cursor themselves.
void ui_textarea_set_cursor(UITextArea *ta, int row, int col);

// Returns true if there is a non-empty selection; normalises so r1/c1 <= r2/c2.
bool ui_textarea_get_selection(UITextArea *ta, int *r1, int *c1, int *r2, int *c2);

// Returns the character at (row,col) or '\0' if out of range.
char ui_textarea_get_char_at(UITextArea *ta, int row, int col);

// Convert absolute screen coordinates (cell coords) to logical row/col.
// Returns true if the coordinates are within the textarea's text region.
bool ui_textarea_screen_to_pos(UITextArea *ta, float screen_x, float screen_y,
                                int *out_row, int *out_col);

// True for a cell of the text region below the last visual line: inside the
// rect, but past the end of the buffer.
bool ui_textarea_screen_past_end(UITextArea *ta, float screen_x, float screen_y);

// The inverse: where a logical row/col currently sits on screen. Resolves word
// wrap (a wrapped row spans several visual lines, and the column picks which)
// and scroll, and needs the same layout screen_to_pos does -- the one the last
// draw call left behind.
//
// Returns false when the position has no screen cell: outside the buffer, or
// scrolled out of view, or behind the scrollbar column. That is deliberately
// not the same as returning an off-rect coordinate, so a caller drawing at the
// result cannot paint outside the widget.
bool ui_textarea_pos_to_screen(UITextArea *ta, int row, int col,
                               float *out_x, float *out_y);

// Convert absolute screen coordinates to a logical row if the position is
// within the gutter area (line-number margin). Returns true when in the gutter,
// false otherwise (including if the position is in the text region or outside
// the textarea entirely).
bool ui_textarea_screen_to_gutter_row(UITextArea *ta, float screen_x, float screen_y,
                                       int *out_row);

// ===== Per-character tags =====
// tag==0 is the default (no-tag) state and requires no heap allocation.
unsigned int ui_textarea_get_tag(UITextArea *ta, int row, int col);
void         ui_textarea_set_tag(UITextArea *ta, int row, int col, unsigned int tag);

// Per-line tag (one per logical line, always available with no allocation).
unsigned int ui_textarea_get_line_tag(UITextArea *ta, int row);
void         ui_textarea_set_line_tag(UITextArea *ta, int row, unsigned int tag);

// ===== Undo / Redo =====
// Manually trigger undo or redo (also available via Ctrl+Z / Ctrl+Y in the widget).
void ui_textarea_undo(UITextArea *ta);
void ui_textarea_redo(UITextArea *ta);

// ===== Undo batching =====
// Wrap a sequence of programmatic edits (delete_range, insert_string, etc.)
// so they become a single undo/redo step.
void ui_textarea_undo_begin_batch(UITextArea *ta);
void ui_textarea_undo_end_batch(UITextArea *ta);

// Collapse everything pushed since a mark into one step, for edits spread over
// frames (a batch cannot stay open across frames: the widget's own keyboard
// handling aborts it). No-op if an undo went below the mark meanwhile.
int  ui_textarea_undo_mark(UITextArea *ta);
void ui_textarea_undo_merge_since(UITextArea *ta, int mark);

// ===== Programmatic editing primitives =====
// These fire callbacks (on_insert_text, on_delete_text, on_delete_line,
// on_insert_line, on_content_updated) and save undo snapshots, just as if
// the user had typed the edits.

// Delete the range [(r1,c1) .. (r2,c2)).  r1 must be <= r2; if equal,
// c1 must be <= c2.  Out-of-bounds coordinates are clamped.
void ui_textarea_delete_range(UITextArea *ta, int r1, int c1, int r2, int c2);

// Insert a (possibly multi-line) string at (row,col).
void ui_textarea_insert_string(UITextArea *ta, int row, int col, const char *s);

// ===== Immediate-mode widget =====
// Draws a `w` x `h` textarea at the layout pen, processes input, and advances
// the pen like every other widget in this library -- so a text field drops into
// a flowing page beside ui_button and ui_label without the page having to know
// what row it is on (see the layout note at the top of textmode_ui.h).
// Returns 1 if content changed this frame.
int ui_textarea(UIContext *ui, UITextArea *ta, int w, int h);

// One-row numeric field built on a UITextArea (see textmode_ui_textarea.c for
// the sync/sanitize/clamp behaviour). `ta` should be a plain UITextArea
// created with line numbers and the bottom status line both off, same as any
// other one-row field. Returns true the frame *value changes.
bool ui_textarea_int(UIContext *ui, UITextArea *ta, int *value,
                     int min_value, int max_value, int w);

// One-row text field bound to a caller-owned string: whenever `model` differs
// from what the field last showed or produced, it replaces the buffer. After a
// user edit, returns the new text with every character in `strip` (may be NULL)
// removed; the pointer is owned by `ta` and valid until its next call. NULL when
// nothing was edited.
const char *ui_textarea_str(UIContext *ui, UITextArea *ta, const char *model,
                            const char *strip, int w);

// The same two widgets pinned to an explicit cell rectangle instead. These read
// neither the pen nor the item-width stack and do not move the pen, so they sit
// outside the layout entirely -- which is what you want for the one big editor
// pane that owns a region of the screen, and what you do NOT want for a field
// on a form. Reaching for these to lay a form out is what forced pages to
// number their rows by hand.
int ui_textarea_absolute_pos(UIContext *ui, UITextArea *ta,
                             int x, int y, int w, int h);
bool ui_textarea_int_absolute_pos(UIContext *ui, UITextArea *ta, int *value,
                                  int min_value, int max_value,
                                  int x, int y, int w);
const char *ui_textarea_str_absolute_pos(UIContext *ui, UITextArea *ta, const char *model,
                                         const char *strip, int x, int y, int w);

#endif // TEXTMODE_UI_TEXTAREA_H
