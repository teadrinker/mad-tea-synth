#ifndef INTERACTIVE_CODING_H
#define INTERACTIVE_CODING_H

#ifndef INCLUDE_TEST_VM_EXAMPLE
#define INCLUDE_TEST_VM_EXAMPLE
#endif


#ifdef INCLUDE_TEST_VM_EXAMPLE

    #include "ui/textmode_ui.h"  // UIContext, Tsys

    typedef struct InteractiveCoding InteractiveCoding;

    InteractiveCoding *interactive_coding_create(Tsys *sys);
    void               interactive_coding_destroy(InteractiveCoding *ic);

    // Draws the whole editor frame -- code area, results panel, button row --
    // as one `w` x `h` block at the layout pen, and advances the pen past it,
    // so an editor drops into a flowing page like any other widget (see the
    // layout note at the top of ui/textmode_ui.h).
    void interactive_coding_frame(InteractiveCoding *ic, UIContext *ui, int w, int h);

    // The same frame pinned to an explicit cell rectangle, for an embedder
    // dividing the screen into regions rather than flowing down a page. Leaves
    // the pen where it found it.
    void interactive_coding_frame_absolute_pos(InteractiveCoding *ic, UIContext *ui,
                                               int x, int y, int w, int h);
    void interactive_coding_on_select(InteractiveCoding *ic);

    // Whether the active textarea holds keyboard focus in the UIContext of the
    // most recent frame call. An embedder routes real OS focus to a native
    // text-input proxy with this, so a host that steals Ctrl+C/X/V through an
    // accelerator table leaves the editor alone. The setter is a no-op before
    // the first frame, when there is no UIContext yet.
    bool interactive_coding_has_editor_focus(InteractiveCoding *ic);
    void interactive_coding_set_editor_focus(InteractiveCoding *ic);

    // Cell rect the results panel occupied in the most recent frame call, for
    // an embedder that wants to overlay it. False, and nothing written, before
    // the first frame. It moves with the draggable "Results" split, so re-read
    // it every frame rather than caching it.
    bool interactive_coding_get_results_rect(InteractiveCoding *ic,
                                             int *x, int *y, int *w, int *h);

    // Did the active editor's most recent parse/compile/run succeed? An
    // embedder showing anything derived from the code blanks it on false,
    // rather than leaving the last good render up pretending to be current.
    bool interactive_coding_last_run_ok(InteractiveCoding *ic);

    // Caret position, in logical (source line, column) coordinates. get writes
    // -1/-1 with no active textarea; set clamps to the current text and drops
    // any selection.
    void interactive_coding_get_cursor(InteractiveCoding *ic, int *row, int *col);
    void interactive_coding_set_cursor(InteractiveCoding *ic, int row, int col);

    // Vertical scroll offset in visual-line units -- where the VIEW is, not the
    // caret. Separate from the cursor accessors because restoring only the
    // caret snaps the view to it. get returns 0 with no active textarea; set
    // clamps negatives to 0, and an offset past the end of the text is clamped
    // on the next frame, once the text has a layout.
    float interactive_coding_get_scroll(InteractiveCoding *ic);
    void  interactive_coding_set_scroll(InteractiveCoding *ic, float scroll);

    // set_text replaces the textarea content; get_text returns a
    // newly-allocated copy (free via sys->free).
    //
    // get_text is scrub-aware: during a live number-scrub drag the textarea
    // buffer is frozen at the pre-drag text, so it returns the live
    // reverse-parsed source instead -- the value driving the result panel, and
    // what the buffer will hold on mouse-up.
    void        interactive_coding_set_text(InteractiveCoding *ic, const char *text);
    char       *interactive_coding_get_text(InteractiveCoding *ic);
    void        interactive_coding_set_text_and_clear_history(InteractiveCoding *ic, const char *text);

    // Group the active editor's undo: every set_text (and edit) from begin to
    // end_undo_group collapses into one undo step at end, e.g. a whole drag's
    // write-backs. begin while a group is held is a no-op; end without one is
    // a no-op; an undo below the begin point meanwhile leaves the steps as-is.
    void        interactive_coding_begin_undo_group(InteractiveCoding *ic);
    void        interactive_coding_end_undo_group(InteractiveCoding *ic);
    bool        interactive_coding_in_undo_group(InteractiveCoding *ic);

    // True during a live number-scrub drag (right-click-drag on a numeric
    // literal). NOT needed to avoid a stale read -- get_text() is already
    // scrub-aware. Its one use is to suppress set_text() mid-drag, which would
    // rebuild the parse arena the in-flight scrub target points into.
    bool        interactive_coding_is_scrubbing(InteractiveCoding *ic);

    // Fires on every successful parse+compile, scrub frames included. `body` is
    // the editor's own source -- exactly what get_text() would return, with no
    // wrap and no prelude around it -- valid only for the call. The preferred
    // way to keep downstream state in sync: get_text() returns the same live
    // source, but only this says WHEN it changed.
    typedef void (*InteractiveCodingCodeChangedFn)(const char *body, void *user);
    void interactive_coding_set_code_changed_callback(
        InteractiveCoding *ic, InteractiveCodingCodeChangedFn cb, void *user);

    // Called on every VM the live parse/compile creates, after vm_create() and
    // before anything is compiled on it -- where an embedder registers its own
    // C functions (register_c_func_*arg, vm/vm.h). Without it an
    // embedder-provided function reads as an undefined symbol while typing, and
    // the failed compile stops the edit reaching the code-changed callback.
    // NULL clears.
    //
    // `struct VM` is forward-declared at FILE scope: naming it first inside the
    // prototype would declare a prototype-scoped type distinct from vm.h's.
    // Spelled out rather than using vm.h's typedef, so this header stays free
    // of vm.h and of parser.h behind it.
    struct VM;
    typedef void (*InteractiveCodingVmSetupFn)(struct VM *vm, void *user);
    void interactive_coding_set_vm_setup_callback(
        InteractiveCoding *ic, InteractiveCodingVmSetupFn cb, void *user);

    // Wraps `body` as  name = (params) => <open>body<close>. `open`/`close`
    // pick expression-body ('(', ')') vs statement-block ('{', '}') wrapping.
    // Returns a sys->malloc'd string (free via sys->free), or NULL if
    // name/params are missing. Needs no InteractiveCoding instance.
    char *interactive_coding_wrap_func_body(Tsys *sys, const char *name, const char *params,
                                             char open, char close, const char *body);

    // Treat the buffer as a snippet BODY, wrapped via
    // interactive_coding_wrap_func_body before every live parse/compile, rather
    // than as a standalone script. An embedder whose buffer is really a
    // function body then gets accurate compile errors instead of "undefined"
    // for its own params. name == NULL restores the default.
    void interactive_coding_set_freeform_wrap(InteractiveCoding *ic, const char *name,
                                               const char *params, char open, char close);

    // Whether the wrapped body is also INVOKED after being defined. Off by
    // default, which is compile-only: the run proves the body compiles and
    // reports "(void)". On, the result box shows what the body returns and
    // print() inside it reaches the results panel.
    //
    // The cost is that the body RUNS on every keystroke and every scrub frame.
    // Harmless for a pure expression; not harmless for a body whose natives
    // touch host state. Decide per editor.
    //
    // Sticky: re-asserting the wrap, which an owner does every frame, keeps
    // whatever was set here.
    void interactive_coding_set_freeform_run_body(InteractiveCoding *ic, bool run);

    // What that invocation is handed. Without this the arguments are all zero,
    // derived from the params' TYPES alone -- fine for proving a body compiles,
    // useless for seeing what it computes.
    //
    // `params` is the signature the call is being built for, so one callback
    // can serve several and pick the literal SPELLING each numeric domain
    // needs. Write a comma-separated list -- "0.25, 440.0, 1.0" -- into `out`
    // and return non-zero; return 0, or write nothing, for the zeros. Wrong
    // arity simply fails to compile, which the live run reports.
    //
    // Pull, not push: called on each live compile AND each hover inspection

    typedef int (*InteractiveCodingWrapArgsFn)(const char *params, char *out, int out_max,
                                               void *user);
    void interactive_coding_set_wrap_args_callback(
        InteractiveCoding *ic, InteractiveCodingWrapArgsFn cb, void *user);

    void interactive_coding_set_hover_inspect(InteractiveCoding *ic, bool on);

    // The editing aids: the remaining parameters of a call after a typed comma
    // or `(`, the parameter names around a hovered comma, and Tab completion of
    // a name from locals, globals and built-ins. On by default.
    void interactive_coding_set_completion(InteractiveCoding *ic, bool on);

    // The Hint box: show the single completion Tab would write, as you type.
    // Per-keypress work, so a host that cannot afford it turns it off. On by default.
    void interactive_coding_set_hints(InteractiveCoding *ic, bool on);

    void interactive_coding_set_inspect_available(InteractiveCoding *ic, bool available);
    void interactive_coding_invalidate_inspection(InteractiveCoding *ic);
    int interactive_coding_strip_inspections(InteractiveCoding *ic);

    void interactive_coding_set_freeform_prelude(InteractiveCoding *ic, const char *prelude);

    // ---- Host-run mode ------------------------------------------------------
    // IC_RUN_LOCAL (the default): the editor compiles and runs the buffer itself
    // and inspects it with private instrumented runs.
    //
    // IC_RUN_HOST: the editor runs nothing. Every edit that parses is handed to
    // the code-changed callbacks (not only edits that compile, since the editor
    // can no longer tell), stamped with a code serial. What to inspect is handed
    // out as a probe set (on_probes_changed / get_probes), and the host feeds
    // back results, errors, print() output and inspection passes through the
    // host_* and inspect_* calls below, all on the UI thread.
    typedef enum { IC_RUN_LOCAL = 0, IC_RUN_HOST } ICRunMode;
    void interactive_coding_set_run_mode(InteractiveCoding *ic, ICRunMode mode);
    ICRunMode interactive_coding_get_run_mode(InteractiveCoding *ic);

    // Host mode only: still parse AND compile each edit locally, without
    // running it, for instant compile errors and the Show C/JS/Lua output while
    // the host catches up. Off: only parse errors are local. A local parse or
    // compile error is shown ahead of anything the host reports.
    void interactive_coding_set_local_check(InteractiveCoding *ic, bool on);

    // The code-changed callback with the serial the host echoes back in
    // host_begin. Fires alongside the plain one.
    typedef void (*InteractiveCodingCodeChanged2Fn)(const char *body, unsigned code_serial, void *user);
    void interactive_coding_set_code_changed_callback2(
        InteractiveCoding *ic, InteractiveCodingCodeChanged2Fn cb, void *user);

    // One thing to inspect. `lo`/`hi` are a BYTE span of the editor's own text
    // (no wrap, no prelude): a host offsets them into the source it compiles
    // and finds the node with vm_probe_node_for_span. `is_sig` marks a lambda
    // parameter list, which wants the function's signatures (inspect_sigs)
    // rather than values.
    typedef struct ICProbe { int id; size_t lo, hi; int is_sig; } ICProbe;
    #define IC_PROBES_MAX 16

    // Fired whenever the probe set changes, with the serial a pass must quote.
    // The serial only moves when a span or an id actually changes, so a host a
    // few edits behind keeps landing passes while the spans still hold. An empty
    // set means stop inspecting. `probes` is valid only for the call.
    typedef void (*InteractiveCodingProbesChangedFn)(const ICProbe *probes, int n, unsigned code_serial,
                                                     unsigned probe_serial, void *user);
    void interactive_coding_set_probes_changed_callback(
        InteractiveCoding *ic, InteractiveCodingProbesChangedFn cb, void *user);
    // The current set; returns the count (at most `max` written).
    int  interactive_coding_get_probes(InteractiveCoding *ic, ICProbe *out, int max,
                                       unsigned *probe_serial);

    // Results for the body the host was handed under `code_serial`, applied to
    // the active editor. A block for a serial older than the newest one already
    // shown is dropped. Between begin and end: any number of errors (row/col in
    // the editor's own text, -1 when unknown), at most one result line, and
    // print() output in call order.
    enum { IC_HOST_PARSE = 0, IC_HOST_COMPILE, IC_HOST_RUN };
    void interactive_coding_host_begin (InteractiveCoding *ic, unsigned code_serial);
    void interactive_coding_host_error (InteractiveCoding *ic, int stage, int row, int col, const char *msg);
    void interactive_coding_host_result(InteractiveCoding *ic, const char *text);
    void interactive_coding_host_print (InteractiveCoding *ic, const unsigned char *s, int n);
    void interactive_coding_host_end   (InteractiveCoding *ic);

    // One inspection pass: every reading for the probe set `probe_serial`
    // named, replacing the previous pass. A pass quoting any other probe serial
    // is dropped whole and the labels on screen stay. Send one only for a run
    // that happened: a pass with no values reads as "never executed". `kind` /
    // `fx_shift` / `vals` are exactly what VMInspectFn / VMInspectArrFn receive,
    // so a host on the UI thread can forward its VM's sinks straight here.
    void interactive_coding_inspect_begin(InteractiveCoding *ic, unsigned probe_serial);
    void interactive_coding_inspect_value(InteractiveCoding *ic, int id, int kind, int fx_shift, double v);
    void interactive_coding_inspect_array(InteractiveCoding *ic, int id, int kind, int fx_shift,
                                          int total, const double *vals, int n);
    void interactive_coding_inspect_sigs (InteractiveCoding *ic, int id, const char *labels);
    void interactive_coding_inspect_end  (InteractiveCoding *ic);


#endif // INCLUDE_TEST_VM_EXAMPLE



#endif // INTERACTIVE_CODING_H
