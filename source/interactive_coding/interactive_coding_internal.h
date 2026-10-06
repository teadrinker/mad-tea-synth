#ifndef INTERACTIVE_CODING_INTERNAL_H
#define INTERACTIVE_CODING_INTERNAL_H

// ===========================================================================
// Module-internal interface: interactive_coding.c <-> its embedders
// ===========================================================================
// NOT public API -- interactive_coding.h is. An embedder composing a STACK of
// editors shares the struct definitions below rather than talking through a
// wall of getters; one editor needs interactive_coding.h alone. The dependency
// runs one way: interactive_coding.c knows nothing about any embedder.

#include "interactive_coding.h"
#include "ui/textmode_ui.h"
#include "ui/textmode_ui_textarea.h"
#include "ui/ui_scrubber.h"
#include "parser/parser.h"
#include "common/array.h"
#include "ic_inspect_fmt.h"

// The freeform editor plus every editor an owner registers.
#define IC_MAX_EDITORS 65

// The argument list an append_call wrap's trailing call is made with. One size
// shared by the compile that splices it and the inspection gate that watches it
// for movement: comparing a truncation against a full string would report a
// change that is not there, so both must clip alike.
#define IC_WRAP_ARGS_MAX 256

// ---------------------------------------------------------------------------
// Inspection sites
// ---------------------------------------------------------------------------
// How many expressions report at once in one run. Slot 0 is the hovered one,
// the rest pins in source order. A site past the cap keeps its inspect(...)
// wrapper -- the text must never silently differ from what runs -- but gets no
// id, no label and no results line.
#define IC_INSPECT_MAX_SITES IC_PROBES_MAX
#define IC_INSPECT_ID_HOVER  0

// How long a newly hovered expression has to be held before it is inspected,
// in seconds. A reading costs a parse+compile+execute, so a pointer crossing a
// line must not pay for every token on the way.
#define IC_INSPECT_HOVER_DELAY 0.15f

// An element index over a comma of an array literal is rarely what the pointer
// is asking about, so it waits longer than a reading.
#define IC_INSPECT_INDEX_DELAY 0.5f

// Width of one pinned reading's line in the results panel. Not the panel's own
// width: it scrolls and resizes under the text, so a measured one goes stale.
#define IC_INSPECT_LINE_MAX  96

// ---------------------------------------------------------------------------
// Overlay labels
// ---------------------------------------------------------------------------
// A short-lived block of text that wants a free spot near some code this frame:
// an inspection reading, an argument hint, a completion menu. One layer places,
// claims and paints them all, so they never fight over the same cells.
#define IC_OV_MAX_ROWS   48
#define IC_OV_TEXT_MAX   8192
#define IC_OV_MAX_LABELS (IC_INSPECT_MAX_SITES + 4)
#define IC_OV_MAX_RECTS  (3 * IC_INSPECT_MAX_SITES + 2 * IC_OV_MAX_ROWS)

typedef enum { IC_OV_PIN, IC_OV_HOVER, IC_OV_HINT, IC_OV_WARN, IC_OV_MENU } ICOverlayKind;

// Keep clear of the cells right of the anchor on its own row: the caret is
// about to type into them.
#define IC_OV_NO_SAME_ROW_RIGHT (1u << 0)
// Try the row under the anchor before the one over it.
#define IC_OV_PREFER_BELOW      (1u << 1)
// Never split a long label over several rows.
#define IC_OV_ONE_ROW           (1u << 2)
// On the anchor's own row, from two cells past it, over whatever is there.
#define IC_OV_SAME_ROW          (1u << 3)
// Exactly at (at_x, at_y): the caller has already found the cells free.
#define IC_OV_AT                (1u << 4)

// Where a label was last placed, so a pin does not re-search every frame.
// placed == 0 is "not placed yet"; place_avail == 0 is "placed, nowhere to put
// it", which draws nothing without re-searching. place_ax/ay is the screen
// anchor the search ran against, so scroll, resize and rewrap are all detected
// by the anchor having moved.
typedef struct {
    float place_x, place_y;
    float place_ax, place_ay;
    float place_ax2, place_ay2;
    int   place_avail;
    int   place_rows;       // 2 or 3 when a long label was split over rows
    int   place_alt;        // which of text / alt_text was placed
    int   placed;
} ICOverlayCache;

typedef struct {
    ICOverlayKind   kind;
    // Must outlive the flush at the end of the frame. '\n' makes explicit rows.
    const char     *text;
    // Tried when `text` does not fit whole; may be a shorter or a taller form.
    const char     *alt_text;
    // The anchor span in BUFFER coordinates, (row, col) leftmost and
    // (row2, col2) rightmost inclusive.
    int             row, col, row2, col2;
    float           focus_x;     // preferred centre in cells; < 0 = middle of the span
    int             align_col;   // >= 0: put text[align_at] over this buffer column
    int             align_at;
    unsigned        flags;
    ICOverlayCache *cache;       // NULL = placed afresh every frame
    const char     *alt2_text;   // a third form, tried after alt_text
    int             align_at_alt;   // align_at for alt_text
    float           at_x, at_y;     // IC_OV_AT: the text's top left, screen cells
} ICOverlayLabel;

// The label layer (ic_overlay.c, #included by interactive_coding.c).
void ic_overlay_begin(InteractiveCoding *ic);
void ic_overlay_add(InteractiveCoding *ic, UIContext *ui, UITextArea *ta, const ICOverlayLabel *l);
void ic_overlay_flush(InteractiveCoding *ic, UIContext *ui);

// A run of cells one label has claimed, in screen coordinates.
typedef struct { float x, y; int len; } ICOverlayRect;

typedef struct {
    const char   *label;
    float         x, y, ay1;
    int           avail, rows, r0;
    ICOverlayKind kind;
    bool          explicit_rows;
    bool          fixed;        // never moved to separate it from another label
} ICOverlayPlaced;

typedef struct {
    // Cells already claimed by a label this frame. First-draw-wins, so a second
    // label overlapping a first disappears into it rather than blending.
    ICOverlayRect   drawn[IC_OV_MAX_RECTS];
    int             num_drawn;
    // This frame's labels, placed and claimed but not yet painted, so two that
    // ended up touching can still be pulled apart. r0 .. r0+rows-1 are its rows
    // in `drawn`; ay1 the row its expression ends on.
    ICOverlayPlaced placed[IC_OV_MAX_LABELS];
    int             num_placed;
    // Cached labels re-place themselves this frame (an edit moved free cells
    // without moving any anchor).
    int             force_place;
    // The textarea this frame's labels were placed against, for the flush.
    UITextArea     *ta;
    // Constraints on the placement search in flight; see ic_overlay_add.
    bool            block_right;
    float           block_x, block_y;
    bool            prefer_below;
    // The argument hint was claimed this frame (it sits between pins and hover).
    bool            hint_added;
    // Where the menu label landed this frame. It is not painted by the flush:
    // its owner draws buttons there.
    bool            menu_placed;
    const char     *menu_label;
    float           menu_x, menu_y;
    int             menu_avail, menu_rows;
} ICOverlay;

typedef struct {
    ICInspect stats;            // filled by the sink during the one private run
    char      label[512];       // the in-code reading; "" for nothing to show

    // The expression's two ends in BUFFER coordinates, (row, col) leftmost and
    // (row2, col2) rightmost inclusive. Relative to these rather than to the
    // pointer, which is what lets a pin be drawn with the mouse nowhere near
    // it. Both ends, because placement measures to the NEARER one.
    int       row, col;
    int       row2, col2;

    // The whole inspect(...) call's byte span, and its argument's, for the
    // unpin and strip edits. Both zero for the hover slot, which owns no text.
    size_t    call_lo, call_hi;
    size_t    arg_lo,  arg_hi;

    // Cached placement, pins only -- the hover's is recomputed every frame.
    ICOverlayCache cache;
} ICInspectSite;

// Ceiling on one run's print() output. Not a buffer size -- the buffer grows to
// fit -- just where more output stops being useful and starts being a way for a
// runaway loop to eat the machine. The panel appends a warning when it is hit,
// so a truncated log never reads as a complete one.
#define IC_PRINT_MAX_BYTES (2 * 1024 * 1024)
#define IC_PRINT_CAP_NOTE  "\nwarning: print capped at 2mb"

// Op budget for the live test run. Sized for a body a real embedder already
// runs once per frame, so code that works in the host does not report "op
// budget exhausted" in the editor meant to be checking it. A cap, not a cost:
// an ordinary body finishes orders of magnitude below it.
#define IC_RUN_OP_BUDGET 3200000

typedef struct ICEditor ICEditor;

// ---------------------------------------------------------------------------
// Completion and argument help (ic_complete.c)
// ---------------------------------------------------------------------------
#define IC_NAME_MAX   48
#define IC_PARAMS_MAX 192
#define IC_TYPES_MAX  128

// 0..5 are vm.h's VMNameKind, in order; the rest are names the buffer declares.
typedef enum {
    ICN_NATIVE, ICN_LIB, ICN_CONST, ICN_HOST_BUF, ICN_GLOBAL, ICN_INTRINSIC,
    ICN_FUNC, ICN_VAR, ICN_PARAM, ICN_LOOPVAR, ICN_CONSTDECL, ICN_DEFINE, ICN_WRAP, ICN_OPTION, ICN_OPTION_ALT, ICN_DIRECTIVE
} ICNameKind;

typedef struct {
    char name[IC_NAME_MAX];
    int  kind;                  // ICNameKind
    int  n_params;              // -1 = not callable
    int  n_optional;            // trailing params with defaults
    char params[IC_PARAMS_MAX]; // "x0, y0, col" -- or "" when unknown
    char types[IC_TYPES_MAX];   // "f64, f64, ..." -- or "" when unknown
    char ret[16];
    int  scope_id;              // while collecting: the binding lambda, -1 = none
    int  scope_lo, scope_hi;    // rows of that lambda; -1 when the name is global
} ICName;

typedef struct { ICName *v; int n, cap; } ICCatalog;

typedef enum { IC_CALL_NONE, IC_CALL_CALL, IC_CALL_LITERAL, IC_CALL_INDEX } ICCallKind;

// Where an offset sits in the call being written, read off the TEXT -- the
// parse of `f(a, ` has failed exactly when the hint is wanted.
typedef struct {
    ICCallKind kind;
    size_t     callee_lo, callee_hi;   // the name being called
    int        arg_index;              // depth-0 commas before the offset
    size_t     open_off;               // the `(` / `[`, or the end of a paren-less callee
    bool       parens;                 // CALL written with `(`
    bool       in_decl;                // a lambda's parameter list: not a call
} ICCallCtx;

bool ic_call_context(const char *text, size_t len, size_t off, ICCallCtx *out);

#define IC_MENU_MAX_HITS 200
#define IC_MENU_DIGITS   9
#define IC_MENU_MAX_ROWS 40
#define IC_HINT_TEXT     512
#define IC_MENU_SEG_MAX  (IC_MENU_MAX_HITS + 1)

typedef struct { int off, len, row, col, idx; } ICMenuSeg;
typedef struct { float x, y; int w; } ICMenuBtn;

// Fired after a successful run of `ed`, with the exact source that ran, so the
// two run paths -- a normal run and a live scrub frame -- share one notion of
// "this editor just produced working code".
typedef void (*ICEditorRanFn)(InteractiveCoding *ic, ICEditor *ed,
                              const char *src, void *user);

// ===========================================================================
// ICEditor -- one editable code buffer
// ===========================================================================
// A plain InteractiveCoding has exactly one; a multi-editor embedder one per
// pane. One set of fields serves both, so no consumer has to branch on which
// copy it is addressing.
struct ICEditor {
    UITextArea *ta;          // NULL until ic_editor_init()
    Tsys       *sys;         // copied by ic_editor_init, so a callback handed
                             // only an ICEditor can still allocate

    // ---- syntax highlighting / parse feedback ----
    // A parse of the RAW buffer, never of the wrapped source: it drives
    // colouring, hover and the scrubber, all addressed in coordinates the user
    // can actually see.
    bool        needs_parse;
    ParseResult parse_result;
    ASTNode    *mouse_hover_node;
    ASTNode    *keyboard_cursor_pos_node;
    int         error_bg_row;
    int         error_bg_col;
    bool        has_error_bg;

    // ---- 4-stage error tracking ----
    char        visual_parse_error[512];
    bool        has_visual_parse_error;
    char        parse_error[512];
    bool        has_parse_error;
    char        compile_error[512];
    bool        has_compile_error;
    char        execute_error[512];
    bool        has_execute_error;

    // ---- last run ----
    char        result[4096];
    bool        result_is_error;
    bool        result_dirty;
    bool        pending_run;

    // Bumped on every compile+run, so inspection can follow a re-run while the
    // pointer sits perfectly still.
    unsigned int run_serial;

    // ---- host-run mode ----
    // code_serial: bumped each time the body is handed to the code-changed
    // callbacks. host_serial: the newest serial whose host results are shown.
    // A host parse/compile error lives apart from the local stages, which win.
    unsigned int code_serial;
    unsigned int host_serial;
    char        host_error[512];
    bool        has_host_error;
    int         host_error_row, host_error_col;

    // ---- print() output from the last run ----
    // Bytes exactly as print() produced them, one '\n' per call. array_clear'd
    // rather than freed at the start of each run: a scrub re-runs on every
    // mouse move, so freeing would churn the heap once per frame.
    Array       print_out;      // element_size 1
    bool        print_capped;   // hit IC_PRINT_MAX_BYTES; output is incomplete
    bool        print_dropped;  // an append failed outright (out of memory)

    // ---- wrap config ----
    // With name/params set the buffer compiles as
    //   [prelude\n]name = (params) => <open>body<close>[; name(defaults)]
    // rather than as a standalone script, because it holds a bare function
    // body: live compile errors are then the real ones instead of "undefined"
    // for every param, and append_call makes the test run execute something.
    // Every error row is shifted back past the lines these add, so none of the
    // header is visible.
    char       *wrap_name;
    char       *wrap_params;
    char        wrap_open;
    char        wrap_close;
    bool        wrap_append_call;
    char       *prelude;
    int         prelude_lines;

    // ---- post-run hook ----
    ICEditorRanFn on_ran;
    void         *on_ran_user;

    // ---- completion ----
    // The buffer as it stood at the last parse that succeeded: what the name
    // catalog is read from while the live text does not parse.
    char         *good_src;
    // Bumped by every re-parse, i.e. by every change to the text.
    unsigned int  edit_serial;
};

// Everything the three editing aids remember between frames.
typedef struct {
    bool       on;                  // interactive_coding_set_completion

    // ---- argument hint: a typed comma or `(` ----
    ICEditor  *hint_ed;
    bool       hint_armed;          // set by on_insert_text, read next frame
    bool       hint_on;
    size_t     hint_open_off;
    ICName     hint_sig;            // the callee as the catalog described it
    unsigned   hint_serial;         // what hint_text was built for
    int        hint_row, hint_col, hint_arg;
    bool       hint_parens;
    bool       hint_have;
    bool       hint_warn;
    char       hint_text[IC_HINT_TEXT];
    char       hint_alt[IC_HINT_TEXT];

    // ---- keyword clause hint: `for `, `while `, ... ----
    ICEditor  *kw_ed;
    int        kw_idx;              // 1 + index into the keyword table; 0 = none
    int        kw_row, kw_col;      // the caret, as of the last frame

    // ---- completion hint: the one completion Tab would write ----
    bool       hints;               // the Hints box; per-keypress work, so it can go off
    ICEditor  *ch_ed;
    int        ch_row, ch_col;
    unsigned   ch_serial;
    char       ch_key[IC_NAME_MAX]; // the word the hint was built for
    bool       ch_have;
    bool       ch_tab;              // shown by Tab, kept while the caret stays put
    char       ch_text[256], ch_alt[IC_NAME_MAX + 8];

    // The names a VM enumerates for this host, built once per setup callback.
    ICCatalog                   base;
    bool                        base_ok;
    InteractiveCodingVmSetupFn  base_setup;
    void                       *base_user;

    // ---- a hovered comma: the parameter names either side, or `id=N` ----
    ICEditor  *cmh_ed;
    int        cmh_row, cmh_col;
    unsigned   cmh_serial;
    float      cmh_since;
    int        cmh_kind;            // 0 none, 1 call, 2 array literal
    char       cmh_callee[IC_NAME_MAX];
    int        cmh_arg;
    bool       cmh_built;
    char       cmh_text[160], cmh_alt[160];
    int        cmh_align, cmh_align_alt;
    bool       cmh_suppress;        // this frame: the value hover stands down

    // ---- the Tab menu ----
    bool       menu_open;
    ICEditor  *menu_ed;
    int        menu_row, menu_c0;
    char       menu_q[IC_NAME_MAX];
    ICCatalog  menu_cat;            // built at Tab, reused while it re-filters
    int        menu_hits[IC_MENU_MAX_HITS];
    int        menu_n;
    // The page: entries menu_first .. +menu_per, as many as there is room for;
    // the digits 1-9 label the nine from menu_num on (Tab moves them). The
    // grid's columns, and where it goes (at = false: wherever the overlay finds
    // room). Re-laid out when the page or its anchor changed (icc_menu_layout).
    int        menu_first, menu_per, menu_num, menu_cols;
    int        menu_len[IC_MENU_MAX_HITS + 1];
    bool       menu_at, menu_dirty;
    float      menu_at_x, menu_at_y;
    float      menu_lay_ax, menu_lay_ay;
    char       menu_text[IC_OV_TEXT_MAX];     // one row
    char       menu_col[IC_OV_TEXT_MAX];      // the grid, '\n' between rows
    // The buttons inside each of the two texts: where it sits in the string
    // and on the label, and which entry of the page it picks (-1 = "..more",
    // which turns the page).
    ICMenuSeg  menu_seg[2][IC_MENU_SEG_MAX];
    int        menu_nseg[2];
    // What was drawn last frame, in screen cells: a press landing there keeps
    // the menu open for the button to take it.
    ICMenuBtn  menu_btn[IC_MENU_SEG_MAX];
    int        menu_nbtn;
} ICComplete;

// ===========================================================================
// InteractiveCoding -- the editor host
// ===========================================================================
// Owns the one parser every editor parses into, the results panel they report
// through, and the chrome around the code area. A multi-editor embedder borrows
// all of that and supplies its own code area.
struct InteractiveCoding {
    Tsys       *sys;
    UIContext  *ui;            // set each frame, used by callbacks

    // ---- The editor this instance owns outright ----
    ICEditor    freeform;

    // The editor the public accessors act on; NULL means the freeform one. A
    // multi-editor embedder aims this at whichever pane has focus, so the
    // accessors follow the user without being reimplemented.
    ICEditor   *active_override;

    // ---- Every editor whose textarea routes through our callbacks ----
    // The cell highlighter and the content-changed hook are handed a bare
    // UITextArea and have to find its editor. Filled by ic_editor_init().
    ICEditor   *editors[IC_MAX_EDITORS];
    int         num_editors;

    // ---- Results textarea (shared) ----
    // The assembled panel text: the active editor's own result plus whatever
    // backend output is switched on. One editor's result is ICEditor::result.
    UITextArea *result_ta;
    char        result[4096];
    bool        result_is_error;
    bool        result_dirty;

    // ---- Global settings ----
    // Sticky companion to set_freeform_wrap: an owner re-asserts the wrap every
    // frame, so "also invoke it" cannot ride in that call's arguments.
    bool        freeform_run_body;
    bool        auto_run;
    bool        show_c_code;
    bool        show_curlywas;
    bool        show_js;
    bool        show_lua;
    char       *c_code_output;
    char       *curlywas_output;
    char       *js_output;
    char       *lua_output;

    // ---- Parser (shared across all editors) ----
    Parser      parser;
    bool        parser_inited;

    // ---- Where the results panel was last drawn (cell space) ----
    // Recorded by ic_chrome_results_panel, whichever composition drew it, so an
    // embedder can overlay the rect. w == 0 is "not drawn yet", which is also
    // the zeroed state.
    int         result_rect_x, result_rect_y, result_rect_w, result_rect_h;

    // ---- Results split (draggable header between code area and results) ----
    float       results_split_t;
    bool        dragging_results_header;
    float       drag_anchor_y_results;
    float       drag_anchor_split_results;

    // ---- Persistent callback table ----
    UITextAreaCallbacks callbacks;

    // ---- Embedder-held undo group (begin/end_undo_group) ----
    UITextArea* undo_group_ta;
    int         undo_group_mark;

    // ---- Right-click gutter comment toggle ----
    UITextArea* rc_gutter_active;
    int         rc_anchor_row;
    int         rc_current_row;
    ASTNode    *rc_drag_target;
    ICEditor   *rc_drag_ed;    // editor the scrub target's parse belongs to

    // ---- Number scrubber state ----
    UIScrubber  rc_scrubber;

    // ---- Inspection (the `Inspect` box on the button row) ----
    // One private run fills every slot -- 0 is the hover, 1..n-1 the pins, each
    // given its id by WRAPPING it in the copy rather than by rewriting the
    // user's text. That run is a full parse+compile+execute, so it happens only
    // when an input changes and the formatted labels are cached. A pin is text,
    // which is what lets it survive a re-parse when the node pointer below
    // cannot.
    bool          hover_inspect;    // the checkbox; on by default
    // The host's veto (interactive_coding_set_inspect_available): while false
    // the box is drawn unchecked and disabled and nothing inspects, whatever
    // hover_inspect holds -- which stays as the user left it.
    bool          inspect_available;
    // The host says the values moved with no re-run (interactive_coding_
    // invalidate_inspection); consumed by the next ic_update_inspect.
    bool          ins_stale;
    ICInspectSite ins_sites[IC_INSPECT_MAX_SITES];
    int           ins_num_sites;    // 1 (hover only) .. IC_INSPECT_MAX_SITES
    ASTNode      *ins_node;         // the hovered node slot 0 was computed for
    ICEditor     *ins_ed;           // the editor these sites belong to
    unsigned int  ins_run_serial;   // ed->run_serial when they were computed

    // The wrap call's arguments as they stood when those sites were computed.
    // A host answering on_wrap_args from something LIVE moves them with no
    // other trigger noticing. Compared as TEXT: the exact string the next run
    // would splice, so the check and the run cannot disagree.
    char          ins_wrap_args[IC_WRAP_ARGS_MAX];

    // The hovered expression's byte span, kept even when its label is
    // suppressed: the pin/unpin gesture works on it, and an already-pinned
    // expression has no label but must still be un-pinnable.
    size_t        ins_hover_lo, ins_hover_hi;
    bool          ins_hover_valid;

    // The bump timer: the node the pointer is on now, and when it landed there.
    // BUMPED (restarted) every time that node changes, so only a pointer that
    // settles for IC_INSPECT_HOVER_DELAY spends a run. Separate from ins_node,
    // which is what the sites were actually computed for.
    ASTNode      *ins_hover_pending;
    float         ins_hover_since;

    // Frames of forced re-placement, for what the per-site anchor test cannot
    // see: an edit elsewhere on the line changes the free cells without moving
    // the anchor. 2 rather than 1, because the search reads the layout the
    // PREVIOUS frame's draw left behind, so the first frame after a change
    // measures the old wrap.
    int           ins_place_todo;

    // The label layer: placement, claims and painting for every overlay.
    ICOverlay     ov;

    // ---- Completion and argument help ----
    ICComplete    cp;

    // What the last refresh asked about, kept for formatting a pass that
    // arrives later (host mode): whether there is a hover reading, whether it
    // is a parameter list's signatures, and its source text.
    bool          ins_have_hover;
    bool          ins_hover_sig;
    char          ins_hover_span[512];

    // ---- Host-run mode (interactive_coding_set_run_mode) ----
    ICRunMode     run_mode;
    bool          local_check;
    // The probe set last published, and its serial.
    ICProbe       ins_probes[IC_INSPECT_MAX_SITES];
    int           ins_num_probes;
    unsigned int  ins_probe_serial;
    // The inspect_begin..end pass in flight was accepted.
    bool          ins_pass_ok;
    // The host_begin..end block in flight: accepted, and for which editor.
    ICEditor     *host_ed;
    bool          host_ok;

    // ---- Code-changed callback (see interactive_coding.h) ----
    InteractiveCodingCodeChangedFn on_code_changed;
    void                          *on_code_changed_user;
    InteractiveCodingCodeChanged2Fn on_code_changed2;
    void                           *on_code_changed2_user;
    InteractiveCodingProbesChangedFn on_probes_changed;
    void                            *on_probes_changed_user;

    // ---- VM-setup callback (see interactive_coding.h) ----
    InteractiveCodingVmSetupFn     on_vm_setup;
    void                          *on_vm_setup_user;

    // ---- Wrap-call arguments (see interactive_coding.h) ----
    // NULL = the zeros built from the parameter types.
    InteractiveCodingWrapArgsFn    on_wrap_args;
    void                          *on_wrap_args_user;
};

// ===========================================================================
// Editor operations
// ===========================================================================

// Create the textarea, wire it to the callbacks and register it for
// textarea->editor lookup. An editor that skips this is never highlighted.
void ic_editor_init(InteractiveCoding *ic, ICEditor *ed);
void ic_editor_free(InteractiveCoding *ic, ICEditor *ed);

// Both no-op when the value is already what was asked for: owners re-assert
// these every frame, and re-arming a parse+compile each time would leave the
// editor permanently dirty. name == NULL clears the wrap.
void ic_editor_set_wrap(InteractiveCoding *ic, ICEditor *ed,
                        const char *name, const char *params,
                        char open, char close, bool append_call);
void ic_editor_set_prelude(InteractiveCoding *ic, ICEditor *ed, const char *prelude);

// Compile + run the current buffer. False when there was nothing to run.
bool ic_editor_run(InteractiveCoding *ic, ICEditor *ed);
bool ic_editor_last_run_ok(const ICEditor *ed);

// Per-frame input handling for one editor.
void ic_editor_update_cursor_and_hover(InteractiveCoding *ic, UIContext *ui, ICEditor *ed);
void ic_editor_update_gutter_toggle(InteractiveCoding *ic, UIContext *ui, UITextArea *ta);

// Drive an in-flight number scrub, whichever editor owns it. Once per frame,
// BEFORE the textareas are drawn: first-draw-wins, so the overlay has to claim
// its cells first.
void ic_update_scrubber(InteractiveCoding *ic, UIContext *ui);

// Draw the inspection labels, re-running the private instrumented compile first
// when an input changed. Same place and same reason as ic_update_scrubber: a
// label drawn after the textarea loses every one of its cells to it.
void ic_update_overlay(InteractiveCoding *ic, UIContext *ui);

// The names a script can use at a point, from the VM (built-ins and everything
// the host's on_vm_setup registers) and from the buffer's last good parse.
void ic_catalog_build(InteractiveCoding *ic, ICEditor *ed, ICCatalog *out);
void ic_catalog_free(InteractiveCoding *ic, ICCatalog *c);
// Candidates for the word `q`, best first, written to hits[] as indices into
// the catalog. The word itself is not a candidate.
int  ic_catalog_query(const ICCatalog *c, const char *q, int qlen, int caret_row,
                      int *hits, int max_hits);
const ICName *ic_catalog_find(const ICCatalog *c, const char *name, int caret_row);
void ic_update_inspect(InteractiveCoding *ic, UIContext *ui);
// Whether inspection is running: the user's box AND the host's say-so.
static inline bool ic_inspect_on(const InteractiveCoding *ic) {
    return ic->hover_inspect && ic->inspect_available;
}

// ===========================================================================
// Chrome -- the frame around the code area
// ===========================================================================
// Shared by both compositions, so the results panel, the draggable split and
// the run buttons exist once rather than once per mode.

typedef struct {
    int   avail_h;    // rows between the reserved top rows and the button row
    int   content_y;  // first row of the code area
    int   content_h;  // rows the code area gets
    int   header_y;   // the draggable "Results" bar
    int   result_y;
    int   result_h;
    int   btn_row;
    float min_frac, max_frac;
} ICChrome;

// top_rows: rows reserved above the code area for the caller's own header;
// 0 when there is none.
void ic_chrome_layout(InteractiveCoding *ic, int y, int h, int top_rows, ICChrome *out);

// The draggable "Results" bar. Updates ic->results_split_t from the drag.
void ic_chrome_results_header(InteractiveCoding *ic, UIContext *ui, int x, int w,
                              const ICChrome *c);

// Render ic->result (plus any C / CurlyWas output) into the results textarea.
// The caller fills ic->result and clears ic->result_dirty first.
void ic_chrome_results_panel(InteractiveCoding *ic, UIContext *ui, int x, int w,
                             const ICChrome *c);

// Concatenate ic->result with the C / CurlyWas outputs and push it to the
// results textarea. Called by whoever assembled ic->result.
void ic_format_result_with_outputs(InteractiveCoding *ic);

#define IC_BTN_RUN (1 << 0)   // the Run button fired this frame

// Bottom row: Run / Auto-run / Show / Inspect. Returns IC_BTN_*. Run is
// reported rather than performed because what it means differs -- the freeform
// editor runs itself, an embedder's pane may also push the result on.
int ic_chrome_button_row(InteractiveCoding *ic, UIContext *ui, int x, int w,
                         const ICChrome *c);

#endif // INTERACTIVE_CODING_INTERNAL_H
