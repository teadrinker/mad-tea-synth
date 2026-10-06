#include "interactive_coding.h"
#include "ui/textmode_ui.h"
#include "ui/textmode.h" // fix clip dependency
#include "ui/ui_scrubber.h"


#ifdef INCLUDE_TEST_VM_EXAMPLE
#include "interactive_coding_internal.h"
#include "ui/textmode_ui_textarea.h"
#include "vm/vm.h"
#include "vm/vm_emit_c.h"
#include "vm/vm_emit_curlywas.h"
#include "vm/vm_emit_script.h"
#include "parser/parser.h"
#include "common/token_iterator.h"
#include "common/string_pure.h"
#include "common/math_pure.h"
#include "platform/platform.h"




#define snprintf s_snprintf

// Raw fixed-point storage as the number it stands for. Always keeps a decimal
// point, so an fx value never reads back as a plain integer.
static void ic_format_fx(char *out, size_t out_size, int raw, int shift) {
    double scale = 1.0;
    for (int i = 0; i < shift; i++) scale *= 2.0;   // 1 << 32 would be UB
    char num[S_FROM_NUMBER_MAX_CHARS];
    s_from_number((double)raw / scale, num);
    bool has_point = false;
    for (const char *c = num; *c; c++) {
        if (*c == '.' || *c == 'e') { has_point = true; break; }
    }
    s_snprintf(out, out_size, "%s%s", num, has_point ? "" : ".0");
}



static float ic_draw_cell_override(void *user, UIContext *ui, UITextArea *ta,
                                    int lrow, int lcol,
                                    float sx, float sy,
                                    unsigned char ch, unsigned int color, unsigned int flags);
static void ic_on_content_updated(void *user, UITextArea *ta);
static void ic_on_insert_text(void *user, UITextArea *ta, int row, int col, int length);
static void ic_complete_forget_editor(InteractiveCoding *ic, ICEditor *ed);
static void ic_complete_release(InteractiveCoding *ic);

static const char *k_default_code =

"// Hover to inspect, right-click to pin\n"
"// Right-click + drag to adjust values\n"
"// Right-click gutter to comment / uncomment\n"
"// Tab for hints / autocomplete\n"
"\n"
"x = 3\n"
"y = 1 + 2f + 3.0 + 3e4\n"
"\n"
"// block styles:\n"
"if(x == 1) { y = 10; } // C style\n"
"if x == 2 then y = 20  // one-liner\n"
"if x == 3              // indented\n"
"    y = 30\n"
"\n"
"sq = x => x * x\n"
"sq(6)\n"
"\n"
"fact = n =>\n"
"    if n < 2\n"
"        return 1.0\n"
"    return n * fact(n - 1)\n"
"fact(6)\n"
"\n";



// ---- helpers to map (lrow,lcol) to an ASTNode* via txt_to_ref ----

static int is_fixed_token(InternID id) {
    return token_is_named(id);
}

static ASTNode *ast_node_get_root(ASTNode *node) {
    if (!node) return NULL;
    while (node->parent) node = node->parent;
    return node;
}

static ASTNode *ast_node_at(ParseResult *pr, int lrow, int lcol) {
    if (!pr || !pr->txt_to_ref)
        return NULL;
    size_t offset;
    if (lrow == 0) {
        offset = (size_t)lcol;
    } else if (pr->line_offsets && lrow - 1 < pr->num_lines_offsets) {
        offset = pr->line_offsets[lrow - 1] + (size_t)lcol;
    } else {
        return NULL;
    }
    if (offset >= pr->num_txt_to_ref)
        return NULL;
    return pr->txt_to_ref[offset];
}

static bool valid_ast_node_at(ParseResult *pr, int lrow, int lcol) {
    if (!pr || !pr->txt_to_ref)
        return false;
    size_t offset;
    if (lrow == 0) {
        offset = (size_t)lcol;
    } else if (pr->line_offsets && lrow - 1 < pr->num_lines_offsets) {
        offset = pr->line_offsets[lrow - 1] + (size_t)lcol;
    } else {
        return false;
    }
    if (offset >= pr->num_txt_to_ref)
        return false;
    return true;
}



// All four are defined with the rest of inspection, further down.
static void ic_inspect_forget(InteractiveCoding *ic);
static void ic_inspect_forget_parse(InteractiveCoding *ic);
static ICEditor *ic_active_editor(InteractiveCoding *ic);
static void ic_inspect_refresh_scrub(InteractiveCoding *ic, ICEditor *ed, const char *src);
static bool ic_is_pin_call(InteractiveCoding *ic, ASTNode *n);

// The editor a textarea belongs to; NULL for the results panel and anything
// else sharing the callback table.
static ICEditor *find_editor_for_ta(InteractiveCoding *ic, UITextArea *ta) {
    if (!ic || !ta) return NULL;
    for (int i = 0; i < ic->num_editors; i++)
        if (ic->editors[i]->ta == ta) return ic->editors[i];
    return NULL;
}

static void format_parse_error_verbose(char *out, size_t out_size,
    Parser *parser, ParseResult *pr, const char *source_text, Tsys *sys)
{
    const char *err = parser_get_error(pr);
    bool is_roundtrip = (s_strcmp(err, "unparse round-trip verification failed") == 0);
    if (is_roundtrip) {
        char *unparsed = unparse_asts(parser, pr);
        if (pr->error_row >= 0 && pr->error_col >= 0) {
            s_snprintf(out, out_size,
                "Parse error at %d:%d: %s\n--- original:\n%s\n+++ unparsed:\n%s",
                pr->error_row + 1, pr->error_col + 1,
                err, source_text, unparsed ? unparsed : "(null)");
        } else {
            s_snprintf(out, out_size,
                "Parse error: %s\n--- original:\n%s\n+++ unparsed:\n%s",
                err, source_text, unparsed ? unparsed : "(null)");
        }
        if (unparsed) sys->free(unparsed);
    } else {
        if (pr->error_row >= 0 && pr->error_col >= 0) {
            s_snprintf(out, out_size,
                "Parse error at %d:%d: %s",
                pr->error_row + 1, pr->error_col + 1, err);
        } else {
            s_snprintf(out, out_size,
                "Parse error: %s", err);
        }
    }
}

// Re-parse one editor's buffer into ed->parse_result. Every editor shares the
// instance's one Parser, so the arena this points into is shared too.
static void ic_editor_parse_for_feedback(InteractiveCoding *ic, ICEditor *ed) {
    if (!ed) return;
    ed->mouse_hover_node = NULL;
    ed->keyboard_cursor_pos_node = NULL;
    // A reused arena address would compare equal to a fresh node and leave the
    // last parse's label sitting beside new text.
    if (ic->ins_ed == ed) ic_inspect_forget_parse(ic);
    free_parse_result(&ic->parser, &ed->parse_result);

    if (!ed->ta) {
        ed->needs_parse = false;
        return;
    }

    char *text = ui_textarea_get_text(ed->ta);
    ed->parse_result = parse_to_asts(&ic->parser, text, PARSE_VERIFY | PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
    ed->needs_parse = false;
    ed->edit_serial++;

    {
        const char *err = parser_get_error(&ed->parse_result);
        if (!err) {
            if (ed->good_src) ic->sys->free(ed->good_src);
            ed->good_src = s_strdup(text, ic->sys->malloc);
        }
        if (err) {
            ed->has_error_bg = true;
            ed->error_bg_row = ed->parse_result.error_row;
            ed->error_bg_col = ed->parse_result.error_col;
            ed->has_visual_parse_error = true;
            format_parse_error_verbose(ed->visual_parse_error, sizeof(ed->visual_parse_error),
                &ic->parser, &ed->parse_result, text, ic->sys);
        } else {
            ed->has_error_bg = false;
            ed->has_visual_parse_error = false;
        }
        ic->result_dirty = true;
    }

    ic->sys->free(text);

    int cr, cc;
    ui_textarea_get_cursor(ed->ta, &cr, &cc);
    ed->keyboard_cursor_pos_node = ast_node_at(&ed->parse_result, cr, cc);
}


// ===== syntax colours =====
// A theme slot plus an offset along its ramp, so highlighting follows
// ui_theme_color's grading instead of being frozen. Hue comes from the slot:
// the three accent slots carry the cold/warm/green gradients.
#define COL_IDENT(ui)         ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT),      60, 0)
#define COL_OPERATOR(ui)      ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT),      52, 0)
#define COL_DEFAULT(ui)       ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT),      27, 0)
#define COL_MUTED_DEFAULT(ui) ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_DEFAULT),      -7, 0)
#define COL_COMMENT(ui)       ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_LABEL),         8, 0)
// Pinned inspection: the `inspect` word and its reading, both green so the two
// read as one thing. A hover reading stays COL_COMMENT -- green means pinned.
#define COL_INSPECT(ui)       ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_GREEN_ACCENT),  8, 0)
#define COL_BRACKET(ui)       ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_COLD_ACCENT),  20, 0)
#define COL_NUMBER(ui)        ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_WARM_ACCENT),  10, 0)
#define COL_STRING(ui)        ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_WARM_ACCENT), -15, 0)
#define COL_UNPARSED(ui)      ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_WARM_ACCENT), -88, 0)
// Equal to COL_NUMBER on purpose.
#define COL_ERROR(ui)         ui_adjust_color_additive(ui, ui_theme_color(ui, UI_COL_WARM_ACCENT),  10, 0)


// The syntax colour dropped onto whatever background the textarea painted
// there. On a light background the hue goes: a ramp reads as a colour cast.
static unsigned int ic_fg_on_bg(unsigned int col, unsigned char bg) {
    return TM_COLOR_PACK(TM_COLOR_FG(col),
                         bg < 50 ? TM_COLOR_GRADIENT(col) : 0,
                         bg);
}

static unsigned int ast_node_color(const UIContext *ui, ASTNode *node) {
    if (!node) return COL_COMMENT(ui);

    // One-line block openers are brackets, but they are words -- colour them
    // like the `if`/`while`/`for` they sit beside.
    if (node->left_bracket == TOK_THEN || node->left_bracket == TOK_DO
        || node->left_bracket == TOK_ELSE)
        return COL_IDENT(ui);
    if (node->left_bracket != 0)
        return COL_BRACKET(ui);
    if (node->number_flags != NUM_NONE)
        return COL_NUMBER(ui);
    if (node->string)
        return COL_STRING(ui);
    if (node->token) {
        if (is_fixed_token(node->token)) {
            if (TOKEN_IS_BRACKET(node->token)) {
                return COL_BRACKET(ui);
            }
            // A fixed token, but a word: keep it looking like `if`, not `+`.
            if (node->token == TOK_ELSE) return COL_IDENT(ui);
            return COL_OPERATOR(ui);
        }
        return COL_IDENT(ui);
    }

    return COL_DEFAULT(ui);
}

static bool is_bracket(char c) { return (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}'); }
static float related(ASTNode *a, ASTNode *b, char c) {
    if (!a || !b) return 1.f;
    if(a == b && a->number_flags != 0)
        return 2.f;
    if(a == b && (a->left_bracket) && is_bracket(c))
        return 2.f;
    return 1.f;
}

static float ic_draw_cell_override(void *user, UIContext *ui, UITextArea *ta,
                                    int lrow, int lcol,
                                    float sx, float sy,
                                    unsigned char ch, unsigned int color, unsigned int flags) {
    InteractiveCoding* ic = (InteractiveCoding*) user;

    ICEditor *ed = find_editor_for_ta(ic, ta);
    if (!ed) {
        ui_draw_cell_flags_weight(ui, sx, sy, ch, color, flags, ui->global_weight);
        return 1.0f;
    }
    if (ed->needs_parse) {
        ic_editor_parse_for_feedback(ic, ed);
    }
    ParseResult *pr = &ed->parse_result;

    unsigned int out_col = COL_UNPARSED(ui);
    unsigned char orig_bg = TM_COLOR_BG(color);
    float weight = ui->global_weight;

    // ---- drag preview: show what lines will look like after the toggle ----
    if (ic->rc_gutter_active == ta) {
        int pr1 = ic->rc_anchor_row;
        int pr2 = ic->rc_current_row;
        if (pr1 > pr2) { int t = pr1; pr1 = pr2; pr2 = t; }
        if (lrow >= pr1 && lrow <= pr2) {
            bool is_commented = (ui_textarea_line_length(ta, lrow) >= 2 &&
                                 ui_textarea_get_char_at(ta, lrow, 0) == '/' &&
                                 ui_textarea_get_char_at(ta, lrow, 1) == '/');
            out_col = is_commented ? COL_MUTED_DEFAULT(ui) : COL_COMMENT(ui);
            ui_draw_cell_flags_weight(ui, sx, sy, ch,
                ic_fg_on_bg(out_col, orig_bg), flags, weight);
            return 1.0f;
        }
    }

    // ---- normal rendering ----
    ASTNode *node = ast_node_at(pr, lrow, lcol);
    if (valid_ast_node_at(pr, lrow, lcol)) {
        out_col = ast_node_color(ui, node);
        // A pin's `inspect` is the editor's mark, not the user's code. Callee
        // position only, so a variable named `inspect` is left alone.
        if (node && node->parent && node->parent->left == node &&
            ic_is_pin_call(ic, node->parent))
            out_col = COL_INSPECT(ui);
    }
    if (node && ui_hovering_enabled(ui)) {
        weight *= (float)m_fmax(related(ed->keyboard_cursor_pos_node, node, ch),
                                 related(ed->mouse_hover_node, node, ch));
    }

    if (ed->has_error_bg && lrow == ed->error_bg_row && lcol == ed->error_bg_col) {
        out_col = COL_ERROR(ui);
    }

    unsigned int final_color = ic_fg_on_bg(out_col, orig_bg);
    ui_draw_cell_flags_weight(ui, sx, sy, ch, final_color, flags, weight);
    return 1.0f;
}

static void ic_on_content_updated(void *user, UITextArea *ta) {
    InteractiveCoding *ic = (InteractiveCoding *)user;
    if (!ic) return;

    ICEditor *ed = find_editor_for_ta(ic, ta);
    if (!ed) return;

    ed->needs_parse = true;
    ed->pending_run = (ic->auto_run != 0);
}

static void toggle_comment_line(UITextArea *ta, int row) {
    int len = ui_textarea_line_length(ta, row);
    bool has_slash_slash = (len >= 2 &&
                            ui_textarea_get_char_at(ta, row, 0) == '/' &&
                            ui_textarea_get_char_at(ta, row, 1) == '/');
    if (has_slash_slash) {
        if (len >= 3 && ui_textarea_get_char_at(ta, row, 2) == ' ')
            ui_textarea_delete_range(ta, row, 0, row, 3);
        else
            ui_textarea_delete_range(ta, row, 0, row, 2);
    } else {
        ui_textarea_insert_string(ta, row, 0, "// ");
    }
}

// interactive_coding_wrap_func_body() is pure text formatting, and embedders
// that only compile code need it without linking the editor. #included rather
// than listed as its own source so builds that already compile this file keep
// getting the symbol. A target wanting the helper WITHOUT the editor compiles
// interactive_coding_wrap.c directly; nothing may do both.
#include "interactive_coding_wrap.c"

// Inverse of interactive_coding_wrap_func_body() for the wrap on `ed`. The
// number scrubber unparses from ast_node_get_root(), which under a freeform
// wrap is the root of the WRAPPED assignment rather than the bare body -- and
// feeding that back in wraps it twice. Returns NULL when there is no wrap or
// the text does not match the shell, so callers fall back to `wrapped` as-is.
//
// Skipped for an append_call wrap: that buffer is a bare body whose unparsed
// root never carries the shell, so the match could only ever misfire.
static char *strip_editor_wrap(InteractiveCoding *ic, ICEditor *ed, const char *wrapped) {
    if (!ed || !ed->wrap_name || !ed->wrap_params || ed->wrap_append_call || !wrapped)
        return NULL;

    size_t prefix_max = s_strlen(ed->wrap_name) + s_strlen(ed->wrap_params) + 16;
    char *prefix = (char*)ic->sys->malloc(prefix_max);
    if (!prefix) return NULL;
    s_snprintf(prefix, prefix_max, "%s = (%s) => %c\n",
        ed->wrap_name, ed->wrap_params, ed->wrap_open);
    size_t prefix_len = s_strlen(prefix);

    char suffix[3] = { '\n', ed->wrap_close, '\0' };
    size_t suffix_len = s_strlen(suffix);
    size_t wrapped_len = s_strlen(wrapped);

    char *result = NULL;
    if (wrapped_len >= prefix_len + suffix_len &&
        s_strncmp(wrapped, prefix, prefix_len) == 0 &&
        s_strncmp(wrapped + wrapped_len - suffix_len, suffix, suffix_len) == 0) {
        size_t body_len = wrapped_len - prefix_len - suffix_len;
        result = (char*)ic->sys->malloc(body_len + 1);
        if (result) {
            ic->sys->memcpy(result, wrapped + prefix_len, body_len);
            result[body_len] = '\0';
        }
    }
    ic->sys->free(prefix);
    return result;
}

// Build default argument values from param signature.
// "n:i32, x:f32" -> "0, 0.0f"
static void build_default_args(const char *params, char *out, int out_max) {
    out[0] = '\0';
    if (!params || !params[0]) return;
    const char *p = params;
    int off = 0;
    int first = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        while (*p && *p != ':' && *p != ',' && *p != ' ') p++;
        if (*p == ':') p++;
        while (*p == ' ') p++;
        const char *type_start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        int type_len = (int)(p - type_start);
        if (!first && off < out_max - 2) { out[off++] = ','; out[off++] = ' '; }
        first = 0;
        if (type_len == 3 && s_strncmp(type_start, "i32", 3) == 0) {
            if (off < out_max - 1) out[off++] = '0';
        } else if (type_len == 3 && s_strncmp(type_start, "i64", 3) == 0) {
            if (off < out_max - 1) out[off++] = '0';
        } else if (type_len == 3 && s_strncmp(type_start, "f32", 3) == 0) {
            if (off < out_max - 4) { out[off++] = '0'; out[off++] = '.'; out[off++] = '0'; out[off++] = 'f'; }
        } else if (type_len == 3 && s_strncmp(type_start, "f64", 3) == 0) {
            if (off < out_max - 3) { out[off++] = '0'; out[off++] = '.'; out[off++] = '0'; }
        } else {
            if (off < out_max - 1) out[off++] = '0';
        }
    }
    out[off] = '\0';
}

// The arguments an append_call wrap's trailing call is made with, as they stand
// RIGHT NOW: the host gets first refusal, zeros are the fallback. Asked rather
// than cached, and shared with the inspection gate, which asks a frame ahead of
// the run to notice the answer changing -- one implementation, so the string
// compared cannot differ from the string spliced. Writes "" for a wrap that
// appends no call, which is what tells the gate it has nothing to watch.
static void ic_wrap_call_args(InteractiveCoding *ic, ICEditor *ed,
                              char *out, int out_max) {
    out[0] = '\0';
    if (!ed || !ed->wrap_name || !ed->wrap_params || !ed->wrap_append_call) return;
    if (!ic->on_wrap_args ||
        !ic->on_wrap_args(ed->wrap_params, out, out_max, ic->on_wrap_args_user) ||
        out[0] == '\0')
        build_default_args(ed->wrap_params, out, out_max);
}

// Assemble the source `body` is actually compiled as:
//
//     [prelude\n]name = (params) => <open>body<close>[; name(defaults)]
//
// NULL when nothing needs wrapping; otherwise a malloc'd string the caller
// frees. *header_lines receives the lines prepended, which every reported error
// row is shifted back by so the user sees positions in their own text.
static char *build_wrapped_source(InteractiveCoding *ic, ICEditor *ed,
                                  const char *body, int *header_lines, size_t *body_off) {
    int   lines   = 0;
    size_t off    = 0;
    char *wrapped = NULL;

    if (ed->wrap_name && ed->wrap_params) {
        char *def = interactive_coding_wrap_func_body(ic->sys, ed->wrap_name, ed->wrap_params,
                                                      ed->wrap_open, ed->wrap_close, body);
        if (def) {
            lines = 1;
            off   = s_strlen(def) - s_strlen(body) - 2;   // "\n<close>" follows the body
            if (ed->wrap_append_call) {
                // On the same line as the definition's closing brace, so the
                // header stays one line and error rows keep lining up.
                char default_args[IC_WRAP_ARGS_MAX];
                ic_wrap_call_args(ic, ed, default_args, (int)sizeof(default_args));

                size_t call_len = s_strlen(def) + s_strlen(ed->wrap_name)
                                + s_strlen(default_args) + 8;
                char *called = (char*)ic->sys->malloc(call_len);
                if (called) {
                    s_snprintf(called, call_len, "%s; %s(%s)", def, ed->wrap_name, default_args);
                    ic->sys->free(def);
                    def = called;
                }
            }
            wrapped = def;
        }
    }

    if (ed->prelude && ed->prelude[0]) {
        const char *base = wrapped ? wrapped : body;
        size_t pn = s_strlen(ed->prelude), bn = s_strlen(base);
        char *prefixed = (char*)ic->sys->malloc(pn + 1 + bn + 1);
        if (prefixed) {
            ic->sys->memcpy(prefixed, ed->prelude, pn);
            prefixed[pn] = '\n';
            ic->sys->memcpy(prefixed + pn + 1, base, bn + 1);
            lines += ed->prelude_lines;
            off   += pn + 1;
            if (wrapped) ic->sys->free(wrapped);
            wrapped = prefixed;
        }
    }

    if (header_lines) *header_lines = lines;
    if (body_off) *body_off = off;
    return wrapped;
}

// The VM bakes "row:col: " into its message. This has the row and column
// separately and shifts the row back past the wrap/prelude lines, so it drops
// the baked copy and re-adds the adjusted one.
static const char *strip_vm_pos_prefix(const char *msg) {
    const char *s = msg;
    int digits = 0;
    while (*s >= '0' && *s <= '9') { s++; digits++; }
    if (!digits || *s != ':') return msg;
    s++;
    digits = 0;
    while (*s >= '0' && *s <= '9') { s++; digits++; }
    if (!digits || s[0] != ':' || s[1] != ' ') return msg;
    return s + 2;
}

static void ic_clear_emitted_outputs(InteractiveCoding *ic) {
    if (ic->c_code_output)   { ic->sys->free(ic->c_code_output);   ic->c_code_output = NULL; }
    if (ic->curlywas_output) { ic->sys->free(ic->curlywas_output); ic->curlywas_output = NULL; }
    if (ic->js_output)       { ic->sys->free(ic->js_output);       ic->js_output = NULL; }
    if (ic->lua_output)      { ic->sys->free(ic->lua_output);      ic->lua_output = NULL; }
}

// The script backends refuse what they cannot represent faithfully (i64 in JS,
// f32 in Lua) and the refusal names the `#rewire` that fixes it. Show that
// message: an empty box would read as "the backend produced nothing".
static char *ic_emit_or_reason(InteractiveCoding *ic, char *emitted, VM *vm) {
    if (emitted) return emitted;
    const char *err = vm_last_error(vm);
    if (!err) err = "emit failed";
    size_t n = s_strlen(err) + 2;
    char *out = (char *)ic->sys->malloc(n);
    if (out) s_snprintf(out, n, "%s", err);
    return out;
}

// ---------------------------------------------------------------------------
// print() capture
// ---------------------------------------------------------------------------
// The sink installed on every VM this module runs. Nowhere else installs one,
// which is the arrangement print is built around: the same script exported to
// C, JS or Lua prints nothing and needs no print of its own. Past
// IC_PRINT_MAX_BYTES the run keeps going and the output stops -- a stalled
// editor is worse than a truncated log.
static void ic_print_sink(void *user, const unsigned char *s, int n) {
    ICEditor *ed = (ICEditor *)user;
    if (!ed || !ed->sys) return;
    if (ed->print_capped) return;

    size_t have = array_len(&ed->print_out);
    if (have + (size_t)n + 1 > (size_t)IC_PRINT_MAX_BYTES) {
        ed->print_capped = true;
        return;
    }
    if (n > 0 && !array_push_n(&ed->print_out, s, (size_t)n, ed->sys)) {
        ed->print_dropped = true;
        return;
    }
    static const unsigned char nl = '\n';
    if (!array_push_n(&ed->print_out, &nl, 1, ed->sys)) ed->print_dropped = true;
}

// Keep the allocation, drop the contents: freeing would make a scrub drag
// allocate and release the whole buffer on every mouse move.
static void ic_print_reset(ICEditor *ed) {
    array_clear(&ed->print_out);
    ed->print_capped  = false;
    ed->print_dropped = false;
}

// The value of a body that returns an array or a slice, as `[3, 4] (i32)` -- the
// same spelling a hover reading uses, minus the range/history machinery, which a
// single value has no use for. Anything it cannot read (a string, a []u8 buffer, a
// struct record: byte views, not lists of numbers) keeps the old plain note.
//
// `rt` is the body's return type; its element kind and shift come from there.
static void ic_format_array_result(char *dst, size_t dst_size, Args *a, VMType rt) {
    int kind;
    switch (rt.kind) {
        case VMT_ARR_I32: case VMT_SLICE_I32: kind = VMT_I32; break;
        case VMT_ARR_F32: case VMT_SLICE_F32: kind = VMT_F32; break;
        case VMT_ARR_F64: case VMT_SLICE_F64: kind = VMT_F64; break;
        default: kind = VMT_VOID; break;
    }
    double vals[IC_INSPECT_ARR_ELEMS];
    int total = 0;
    int n = kind == VMT_VOID ? -1
                             : args_get_result_elems(a, vals, IC_INSPECT_ARR_ELEMS, &total);
    if (n < 0) {
        s_snprintf(dst, dst_size, "(array / unsupported type)");
        return;
    }

    char type[8], text[IC_INSPECT_ARR_ELEMS * (S_FROM_NUMBER_MAX_CHARS + 6) + 8];
    int shift = vmtype_fx_shift(rt);
    ic_ins_arr_type_name(kind, shift, type, (int)sizeof(type));
    // Whole reals print as integers; the note names their type.
    int whole = ic_ins_is_real(kind, shift);
    for (int i = 0; i < n && whole; i++) whole = ic_ins_is_whole(vals[i]);
    // Leave the room the type note needs, so a long array is shortened rather
    // than the note it is meaningless without.
    int room = (int)dst_size - 1 - (int)s_strlen(type) - 3;
    if (ic_ins_arr_text(vals, n, total, whole ? VMT_I32 : kind, whole ? 0 : shift, text, (int)sizeof(text), room) <= 0)
        s_snprintf(text, sizeof(text), "[...]");
    s_snprintf(dst, dst_size, "%s (%s)", text, type);
}

// The body reached the embedder: stamp it and tell both callbacks. Called
// last by every path that has one, so a callback may re-enter.
static void ic_code_changed(InteractiveCoding *ic, ICEditor *ed, const char *body) {
    ed->code_serial++;
    if (ic->on_code_changed)  ic->on_code_changed(body, ic->on_code_changed_user);
    if (ic->on_code_changed2) ic->on_code_changed2(body, ed->code_serial, ic->on_code_changed2_user);
}

// In host mode the caret background belongs to the host's error once nothing
// local claims it.
static void ic_host_error_bg(ICEditor *ed) {
    ed->has_error_bg = ed->has_host_error && ed->host_error_row >= 0;
    ed->error_bg_row = ed->host_error_row;
    ed->error_bg_col = ed->host_error_col;
}

// How far vm_execute_core goes. RUN is local mode; host mode stops at COMPILE
// (local check on) or PARSE, and hands the body over even when the compile
// fails, since the host judges it for itself.
enum { IC_EXEC_RUN = 0, IC_EXEC_COMPILE, IC_EXEC_PARSE };

// parse -> compile -> run. `src` is the assembled source -- the editor's own
// text under its wrap and prelude -- and `body` is that text alone, which is
// all an embedder is told about (see ic_code_changed).
// wrap_header_lines is subtracted from error rows, undoing what
// build_wrapped_source() prepended so a reported row lands on a line the user
// can see. The four error stages and the caret background go on `ed`; emitted
// output is instance-wide and stays on `ic`.
static void vm_execute_core(InteractiveCoding *ic, ICEditor *ed,
                            const char *src, const char *body,
                            int wrap_header_lines, int mode) {
    Parser *p = &ic->parser;
    ic_clear_emitted_outputs(ic);
    // Before the parse: a run that never executes must not leave the previous
    // run's print output beside a parse error. The host owns it in host mode.
    if (mode == IC_EXEC_RUN) ic_print_reset(ed);
    ParseResult res = parse_to_asts(p, src, PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);

    if (parser_get_error(&res)) {
        const char *err = parser_get_error(&res);
        ed->has_error_bg = true;
        ed->error_bg_row = res.error_row - wrap_header_lines;
        ed->error_bg_col = res.error_col;
        ed->has_parse_error = true;
        ed->has_compile_error = false;
        ed->has_execute_error = false;
        int disp_row = res.error_row - wrap_header_lines;
        if (disp_row >= 0 && res.error_col >= 0) {
            s_snprintf(ed->parse_error, sizeof(ed->parse_error),
                "Parse error at %d:%d: %s",
                disp_row + 1, res.error_col + 1, err);
        } else {
            s_snprintf(ed->parse_error, sizeof(ed->parse_error),
                "Parse error: %s", err);
        }
        ic->result_dirty = true;
        free_parse_result(p, &res);
        return;
    }

    if (mode == IC_EXEC_PARSE) {
        ed->has_parse_error   = false;
        ed->has_compile_error = false;
        ic_host_error_bg(ed);
        ic->result_dirty = true;
        free_parse_result(p, &res);
        ic_code_changed(ic, ed, body);
        return;
    }

    VM *vm = vm_create(ic->sys, p);
    if (vm && mode == IC_EXEC_RUN) vm_set_print_sink(vm, ic_print_sink, ed);
    // Before func_create: the embedder's C functions have to exist on this VM
    // for a body that calls them to compile.
    if (vm && ic->on_vm_setup) ic->on_vm_setup(vm, ic->on_vm_setup_user);
    if (!vm) {
        ed->has_parse_error = false;
        ed->has_compile_error = true;
        ed->has_execute_error = false;
        s_snprintf(ed->compile_error, sizeof(ed->compile_error), "Error: failed to create VM");
        ed->has_error_bg = false;
        ic->result_dirty = true;
        free_parse_result(p, &res);
        return;
    }

    Func *f = func_create(vm, res.code_tree, &res);
    if (!f) {
        const char *err = vm_last_error(vm);
        int erow = vm_last_error_row(vm) - wrap_header_lines;
        int ecol = vm_last_error_col(vm);
        ed->has_error_bg = true;
        ed->error_bg_row = erow;
        ed->error_bg_col = ecol;
        ed->has_parse_error = false;
        ed->has_compile_error = true;
        ed->has_execute_error = false;
        if (err) {
            const char *msg = strip_vm_pos_prefix(err);
            int raw_row = vm_last_error_row(vm);
            if (erow >= 0 && ecol >= 0) {
                s_snprintf(ed->compile_error, sizeof(ed->compile_error),
                    "Compile error at %d:%d: %s",
                    erow + 1, ecol + 1, msg);
            } else if (raw_row >= 0 && ecol >= 0) {
                // In the wrap header or prelude, at a line the user cannot
                // see: report the position in the assembled source instead.
                s_snprintf(ed->compile_error, sizeof(ed->compile_error),
                    "Compile error in prelude at %d:%d: %s",
                    raw_row + 1, ecol + 1, msg);
            } else {
                s_snprintf(ed->compile_error, sizeof(ed->compile_error),
                    "Compile error: %s", msg);
            }
        } else {
            s_snprintf(ed->compile_error, sizeof(ed->compile_error),
                "Compile error (unknown)");
        }
        ic->result_dirty = true;
        vm_destroy(vm);
        free_parse_result(p, &res);
        if (mode != IC_EXEC_RUN) ic_code_changed(ic, ed, body);
        return;
    }

    // The frame limit is this editor's own run buffer, not the language's: a
    // host runs the body in a frame of its own, so a compile-only check skips it.
    size_t need = func_frame_size(f);
    if (mode == IC_EXEC_RUN && need > 4096) {
        ed->has_parse_error = false;
        ed->has_compile_error = true;
        ed->has_execute_error = false;
        s_snprintf(ed->compile_error, sizeof(ed->compile_error),
            "Compile error: function frame too large (%d bytes, max 4096)", (int)need);
        ed->has_error_bg = false;
        ic->result_dirty = true;
        vm_destroy(vm);
        free_parse_result(p, &res);
        return;
    }
    if (mode == IC_EXEC_COMPILE) {
        // Compiled, not run: the result line, the run error and the print log
        // are the host's.
        ed->has_parse_error   = false;
        ed->has_compile_error = false;
        ic_host_error_bg(ed);
        ic->result_dirty = true;
        if (ic->show_c_code)   ic->c_code_output   = ic_emit_or_reason(ic, vm_emit_c(vm), vm);
        if (ic->show_curlywas) ic->curlywas_output = ic_emit_or_reason(ic, vm_emit_curlywas(vm), vm);
        if (ic->show_js)       ic->js_output       = ic_emit_or_reason(ic, vm_emit_js(vm), vm);
        if (ic->show_lua)      ic->lua_output      = ic_emit_or_reason(ic, vm_emit_lua(vm), vm);
        vm_destroy(vm);
        free_parse_result(p, &res);
        ic_code_changed(ic, ed, body);
        return;
    }
    unsigned char frame[4096];
    Args a;
    args_bind(&a, f, frame, sizeof(frame));
    VMStatus st = func_run(f, &a, IC_RUN_OP_BUDGET);

    ed->has_parse_error = false;
    ed->has_compile_error = false;

    if (st == VM_BUDGET) {
        ed->has_execute_error = true;
        s_snprintf(ed->execute_error, sizeof(ed->execute_error), "Error: op budget exhausted");
        ed->has_error_bg = false;
    } else if (st != VM_OK) {
        ed->has_execute_error = true;
        const char *rerr = vm_last_error(vm);
        s_snprintf(ed->execute_error, sizeof(ed->execute_error),
            "Error: %s", rerr ? rerr : "runtime failure");
        ed->has_error_bg = false;
    } else {
        ed->has_execute_error = false;
        VMType rt = func_return_type(f);
        ed->has_error_bg = false;
        if (rt.kind == VMT_VOID) {
            s_snprintf(ed->execute_error, sizeof(ed->execute_error), "(void)");
        } else if (rt.kind == VMT_I32 && vmtype_fx_shift(rt) > 0) {
            // The number it stands for, not the raw storage: `1 as fx16` is
            // 1.0 (fx16), not 65536 (i32).
            char num[S_FROM_NUMBER_MAX_CHARS];
            ic_format_fx(num, sizeof(num), args_get_i32_result(&a), vmtype_fx_shift(rt));
            s_snprintf(ed->execute_error, sizeof(ed->execute_error),
                "%s (fx%d)", num, vmtype_fx_shift(rt));
        } else if (rt.kind == VMT_I32) {
            int val = args_get_i32_result(&a);
            s_snprintf(ed->execute_error, sizeof(ed->execute_error), "%d (i32)", val);
        } else if (rt.kind == VMT_F32) {
            float val = args_get_f32_result(&a);
            s_snprintf(ed->execute_error, sizeof(ed->execute_error), "%g (f32)", (double)val);
        } else if (rt.kind == VMT_F64) {
            double val = args_get_f64_result(&a);
            s_snprintf(ed->execute_error, sizeof(ed->execute_error), "%g (f64)", val);
        } else if (rt.kind == VMT_I64) {
            long long val = args_get_i64_result(&a);
            s_snprintf(ed->execute_error, sizeof(ed->execute_error), "%lld (i64)", val);
        } else {
            ic_format_array_result(ed->execute_error, sizeof(ed->execute_error), &a, rt);
        }
    }
    ic->result_dirty = true;

    if (ic->show_c_code)   ic->c_code_output   = ic_emit_or_reason(ic, vm_emit_c(vm), vm);
    if (ic->show_curlywas) ic->curlywas_output = ic_emit_or_reason(ic, vm_emit_curlywas(vm), vm);
    if (ic->show_js)       ic->js_output       = ic_emit_or_reason(ic, vm_emit_js(vm), vm);
    if (ic->show_lua)      ic->lua_output      = ic_emit_or_reason(ic, vm_emit_lua(vm), vm);

    vm_destroy(vm);
    free_parse_result(p, &res);

    // Only once parse+compile succeeded. Embedders re-sync off this rather
    // than polling get_text(), which lags a live scrub -- the buffer is not
    // rewritten until the drag releases. The BODY, not the assembled source:
    // the wrap and the prelude are this editor's own scaffolding, and an
    // embedder that had to strip them back off was coupled to their exact
    // spelling. Fires last, so the callback may re-enter.
    ic_code_changed(ic, ed, body);
}

// Compile and run `src` as the body of `ed`, applying its wrap and prelude
// first. Every entry point goes through here rather than calling
// vm_execute_core() on a raw buffer, which would skip the wrap.
static void ic_editor_execute(InteractiveCoding *ic, ICEditor *ed, const char *src) {
    int   wrap_header_lines = 0;
    char *wrapped = build_wrapped_source(ic, ed, src, &wrap_header_lines, NULL);
    int   mode = ic->run_mode != IC_RUN_HOST ? IC_EXEC_RUN
               : ic->local_check            ? IC_EXEC_COMPILE : IC_EXEC_PARSE;
    vm_execute_core(ic, ed, wrapped ? wrapped : src, src, wrap_header_lines, mode);
    if (wrapped) ic->sys->free(wrapped);
    // Inspection watches this, so a reading follows a re-run even while the
    // pointer sits still.
    ed->run_serial++;
}

// The four error stages reduced to the one line an editor shows for itself.
// The results panel renders the freeform editor's stages in more detail.
static void ic_editor_store_result(InteractiveCoding *ic, ICEditor *ed) {
    (void)ic;
    if (ed->has_parse_error) {
        s_snprintf(ed->result, sizeof(ed->result), "[Parse] %s", ed->parse_error);
        ed->result_is_error = true;
    } else if (ed->has_compile_error) {
        s_snprintf(ed->result, sizeof(ed->result), "[Compile] %s", ed->compile_error);
        ed->result_is_error = true;
    } else if (ed->has_host_error) {
        s_snprintf(ed->result, sizeof(ed->result), "[Host] %s", ed->host_error);
        ed->result_is_error = true;
    } else {
        s_snprintf(ed->result, sizeof(ed->result), "%s", ed->execute_error);
        ed->result_is_error = ed->has_execute_error;
    }
    ed->result_dirty = false;
}

bool ic_editor_last_run_ok(const ICEditor *ed) {
    return !ed->has_parse_error && !ed->has_compile_error && !ed->has_host_error &&
           !ed->has_execute_error;
}

bool ic_editor_run(InteractiveCoding *ic, ICEditor *ed) {
    if (!ed || !ed->ta || !ic->parser_inited) return false;

    char *src = ui_textarea_get_text(ed->ta);
    if (!src || src[0] == '\0') {
        if (src) ic->sys->free(src);
        ic_clear_emitted_outputs(ic);
        ic_print_reset(ed);
        ed->has_error_bg = false;
        ed->has_parse_error = false;
        ed->has_compile_error = false;
        ed->has_execute_error = false;
        s_snprintf(ed->execute_error, sizeof(ed->execute_error), "(empty)");
        s_snprintf(ed->result, sizeof(ed->result), "(empty)");
        ed->result_is_error = false;
        ed->result_dirty = false;
        ic->result_dirty = true;
        return false;
    }

    ic_editor_execute(ic, ed, src);
    ic_editor_store_result(ic, ed);
    if (ed->on_ran && ic->run_mode == IC_RUN_LOCAL && ic_editor_last_run_ok(ed))
        ed->on_ran(ic, ed, src, ed->on_ran_user);
    ic->sys->free(src);
    return true;
}



// ===== Public API =====

InteractiveCoding *interactive_coding_create(Tsys *sys) {
    InteractiveCoding *ic = (InteractiveCoding *)sys->malloc(sizeof(InteractiveCoding));
    sys->memset(ic, 0, sizeof(InteractiveCoding));
    if (!ic) return NULL;
    ic->sys  = sys;

    ic->result_ta = ui_textarea_create(sys);
    ui_textarea_set_show_line_numbers(ic->result_ta, false);
    ui_textarea_set_show_bottom_status(ic->result_ta, false);
    s_strcpy(ic->result, "(not run)");
    ic->result_is_error = false;
    ic->result_dirty = true;

    ic->auto_run         = true;
    ic->show_c_code      = false;
    ic->show_curlywas    = false;
    ic->show_js          = false;
    ic->show_lua         = false;
    ic->c_code_output   = NULL;
    ic->curlywas_output = NULL;
    ic->js_output       = NULL;
    ic->lua_output      = NULL;
    ic->results_split_t = 0.7f;
    // On by default. It costs a private compile+run per expression the pointer
    // lands on, and touches host state for a body whose natives do -- an
    // embedder that cannot afford either calls set_hover_inspect(false).
    ic->hover_inspect   = true;
    ic->inspect_available = true;
    ic->ins_stale       = false;
    ic_inspect_forget(ic);
    ic->ins_ed          = NULL;
    ic->ins_run_serial  = 0;
    ic->ov.num_drawn   = 0;
    ic->ov.num_placed  = 0;
    ic->dragging_results_header = false;
    ic->drag_anchor_y_results   = 0.0f;
    ic->drag_anchor_split_results = 0.7f;

    ic->rc_gutter_active = NULL;
    ic->rc_drag_target = NULL;
    ic->rc_drag_ed     = NULL;
    ic->rc_anchor_row   = 0;
    ic->rc_current_row  = 0;
    ic->sys->memset(&ic->rc_scrubber, 0, sizeof(ic->rc_scrubber));

    // Parser first: an editor's parse result is allocated out of it.
    parser_init(&ic->parser, sys);
    ic->parser_inited = true;

    // Wire callbacks -- embedded in struct so pointer stays valid
    sys->memset(&ic->callbacks, 0, sizeof(ic->callbacks));
    ic->callbacks.user                = ic;
    ic->callbacks.draw_cell_override  = ic_draw_cell_override;
    ic->callbacks.on_content_updated  = ic_on_content_updated;
    ic->callbacks.on_insert_text      = ic_on_insert_text;
    ic->cp.on = true;
    ic->cp.hints = true;

    ic_editor_init(ic, &ic->freeform);
    ui_textarea_set_text(ic->freeform.ta, k_default_code, true);
    ic->freeform.pending_run = true; // trigger initial auto-run

    return ic;
}

void ic_editor_init(InteractiveCoding *ic, ICEditor *ed) {
    if (!ic || !ed) return;
    ic->sys->memset(ed, 0, sizeof(*ed));
    ed->sys = ic->sys;
    ed->ta = ui_textarea_create(ic->sys);
    ui_textarea_set_callbacks(ed->ta, &ic->callbacks);
    ed->needs_parse  = true;
    ed->result_dirty = true;
    array_init(&ed->print_out, 1);

    if (ic->num_editors < IC_MAX_EDITORS)
        ic->editors[ic->num_editors++] = ed;
}

// Release everything an editor owns and drop every pointer aimed at it. The
// parse result is freed through the instance's parser, the arena it came from.
void ic_editor_free(InteractiveCoding *ic, ICEditor *ed) {
    if (!ic || !ed) return;

    for (int i = 0; i < ic->num_editors; i++) {
        if (ic->editors[i] != ed) continue;
        ic->editors[i] = ic->editors[--ic->num_editors];
        break;
    }
    ic_complete_forget_editor(ic, ed);
    if (ic->rc_drag_ed == ed) { ic->rc_drag_ed = NULL; ic->rc_drag_target = NULL; }
    // ins_node points into the parse arena freed below.
    if (ic->ins_ed == ed) { ic_inspect_forget(ic); ic->ins_ed = NULL; }
    if (ic->active_override == ed) ic->active_override = NULL;
    if (ed->ta && ic->rc_gutter_active == ed->ta) ic->rc_gutter_active = NULL;

    if (ed->ta) {
        free_parse_result(&ic->parser, &ed->parse_result);
        ui_textarea_destroy(ed->ta);
        ed->ta = NULL;
    }
    if (ed->wrap_name)   { ic->sys->free(ed->wrap_name);   ed->wrap_name = NULL; }
    if (ed->wrap_params) { ic->sys->free(ed->wrap_params); ed->wrap_params = NULL; }
    if (ed->prelude)     { ic->sys->free(ed->prelude);     ed->prelude = NULL; }
    if (ed->good_src)    { ic->sys->free(ed->good_src);    ed->good_src = NULL; }
    array_free(&ed->print_out, ic->sys);
}

// See interactive_coding_internal.h for the contract of these two.
void ic_editor_set_wrap(InteractiveCoding *ic, ICEditor *ed,
                        const char *name, const char *params,
                        char open, char close, bool append_call) {
    if (s_strcmp(ed->wrap_name ? ed->wrap_name : "", name ? name : "") == 0 &&
        s_strcmp(ed->wrap_params ? ed->wrap_params : "", params ? params : "") == 0 &&
        (!name || (ed->wrap_open == open && ed->wrap_close == close &&
                   ed->wrap_append_call == append_call)))
        return;

    if (ed->wrap_name)   { ic->sys->free(ed->wrap_name);   ed->wrap_name = NULL; }
    if (ed->wrap_params) { ic->sys->free(ed->wrap_params); ed->wrap_params = NULL; }
    if (!name) {
        ed->wrap_append_call = false;
        ed->needs_parse = true;
        ed->pending_run = true;
        return;
    }

    ed->wrap_name        = s_strdup(name, ic->sys->malloc);
    ed->wrap_params      = s_strdup(params ? params : "", ic->sys->malloc);
    ed->wrap_open        = open;
    ed->wrap_close       = close;
    ed->wrap_append_call = append_call;
    ed->needs_parse = true;
    ed->pending_run = true;
}

void ic_editor_set_prelude(InteractiveCoding *ic, ICEditor *ed, const char *prelude) {
    const char *cur = ed->prelude ? ed->prelude : "";
    const char *nxt = prelude ? prelude : "";
    if (s_strcmp(cur, nxt) == 0) return;

    if (ed->prelude) { ic->sys->free(ed->prelude); ed->prelude = NULL; }
    ed->prelude_lines = 0;
    if (nxt[0]) {
        ed->prelude = s_strdup(nxt, ic->sys->malloc);
        // Its own newlines plus the one build_wrapped_source inserts.
        int n = 1;
        for (const char *p = nxt; *p; p++) if (*p == '\n') n++;
        ed->prelude_lines = n;
    }
    ed->pending_run = true;
}

void interactive_coding_destroy(InteractiveCoding *ic) {
    if (!ic) return;

    interactive_coding_end_undo_group(ic);
    ic_complete_release(ic);
    ic_editor_free(ic, &ic->freeform);
    ui_textarea_destroy(ic->result_ta);
    if (ic->c_code_output) ic->sys->free(ic->c_code_output);
    if (ic->curlywas_output) ic->sys->free(ic->curlywas_output);
    if (ic->js_output) ic->sys->free(ic->js_output);
    if (ic->lua_output) ic->sys->free(ic->lua_output);
    if (ic->parser_inited) {
        parser_deinit(&ic->parser);
    }
    ic->sys->free(ic);
}

// Update cursor + hover tracking and arm the number scrubber for one editor.
void ic_editor_update_cursor_and_hover(InteractiveCoding *ic, UIContext *ui, ICEditor *ed)
{
    UITextArea  *ta = ed->ta;
    ParseResult *pr = &ed->parse_result;

    int cr, cc;
    ui_textarea_get_cursor(ta, &cr, &cc);
    ed->keyboard_cursor_pos_node = ast_node_at(pr, cr, cc);

    int hr, hc;
    if (ic->rc_drag_target == NULL && ui_textarea_screen_to_pos(ta, ui->mouse_x, ui->mouse_y, &hr, &hc)) {
        ed->mouse_hover_node = ast_node_at(pr, hr, hc);
        ASTNode* hover = ed->mouse_hover_node;

        // Not past 2^53: the scrubber works in doubles and would round the value.
        if(hover && hover->number_flags && !ast_has_exact_i64(hover) && ui->mouse_pressed[UI_MOUSE_BUTTON_RIGHT]) {
            int lSide = hc;
            int rSide = hc;
            while (ast_node_at(pr, hr, lSide - 1) == hover) { lSide--; }
            while (ast_node_at(pr, hr, rSide + 1) == hover) { rSide++; }
            float sx = ui->mouse_x + (float)(lSide - hc);
            float sy = ui->mouse_y;
            int is_float = (hover->number_flags & NUM_TYPE) == NUM_FLOAT;
            int num_chars = (rSide - lSide + 1) - (is_float ? 1 : 0);
            int flags = ((hover->number_flags & NUM_TYPE) == NUM_DOUBLE  ? UI_SCRUBBER_FLAGS_FORCE_DOT : 0) |
                        ((hover->number_flags & NUM_TYPE) == NUM_INTEGER ? UI_SCRUBBER_FLAGS_INTEGER   : 0) |
                        UI_SCRUBBER_FLAGS_WARM_RAMP;
            ui_scrubber_init(ui, &ic->rc_scrubber, hover->number, is_float ? "f" : "", num_chars, sx, sy, flags);
            ic->rc_drag_target = hover;
            ic->rc_drag_ed     = ed;
            // Capture for the whole drag, or every other control keeps
            // hit-testing under the pointer. Registered against the RIGHT
            // button that drives it, or ui_end() drops it next frame.
            ui_set_active_control_btn(ui, ui_id_from_ptr(&ic->rc_scrubber), NULL,
                                      UI_MOUSE_BUTTON_RIGHT);
        }
    } else {
        ed->mouse_hover_node = NULL;
    }
}

// Shift during a scrub of an integer literal promotes it to a double, so the
// decimals dialled in survive the unparse ("4" -> "4.3", or "4.0"). The
// scrubber latches it for the rest of the drag; mirror it onto the node.
static void ic_apply_scrubber_promotion(InteractiveCoding *ic) {
    ASTNode *t = ic->rc_drag_target;
    if (!t || !ic->rc_scrubber.promoted_to_double) return;
    if ((t->number_flags & NUM_TYPE) != NUM_INTEGER) return;
    t->number_flags = (NumberFlags)((t->number_flags & ~NUM_TYPE) | NUM_DOUBLE);
}

// The buffer is deliberately NOT rewritten during a scrub drag: that would re-parse
// and free the arena rc_drag_target points into. The live value lives in the
// mutated AST, which is what feeds the panel and get_text() until mouse-up.
void ic_update_scrubber(InteractiveCoding *ic, UIContext *ui) {
    if (!ic->rc_drag_target) return;

    ICEditor *ed = ic->rc_drag_ed;
    if (!ed) { ic->rc_drag_target = NULL; ui_set_active_control(ui, 0, NULL); return; }

    ic_apply_scrubber_promotion(ic);

    if (ui->mouse_released[UI_MOUSE_BUTTON_RIGHT]) {
        // Commit. number_as_token = 0 makes the unparser format from the
        // number field rather than the old token text.
        ic->rc_drag_target->number_as_token = 0;
        ASTNode *root = ast_node_get_root(ic->rc_drag_target);
        char *new_code = unparse_expression(&ic->parser, root);
        if (new_code) {
            char *body = strip_editor_wrap(ic, ed, new_code);
            ui_textarea_set_text(ed->ta, body ? body : new_code, false);
            ed->needs_parse = true;
            if (body) ic->sys->free(body);
            ic->sys->free(new_code);
        }
        ic->rc_drag_target = NULL;
        ic->rc_drag_ed     = NULL;
        ui_set_active_control(ui, 0, NULL);
        return;
    }

    if (!ui_scrubber(ui, &ic->rc_scrubber)) return;

    ic->rc_drag_target->number = ui_scrubber_get_value(&ic->rc_scrubber);
    ic->rc_drag_target->number_flags = (NumberFlags)(ic->rc_drag_target->number_flags & ~NUM_EXACT_I64);
    ic->rc_drag_target->number_as_token = 0;

    // Auto-run gates the drag as it gates typing -- a scrub is an edit. The
    // value is still mirrored onto the AST, so the overlay and get_text()
    // follow the drag; what stops is the per-frame compile and everything
    // hanging off it.
    if (!ic->auto_run) return;

    ASTNode *root = ast_node_get_root(ic->rc_drag_target);
    char *new_code = unparse_expression(&ic->parser, root);
    if (!new_code) return;

    char *body = strip_editor_wrap(ic, ed, new_code);
    const char *src = body ? body : new_code;

    ic_editor_execute(ic, ed, src);
    ic_editor_store_result(ic, ed);

    // Pins follow the drag, read from `src` -- the unparsed, scrubbed source --
    // rather than from the buffer, which holds the pre-drag number until
    // mouse-up. In host mode that only re-aims the probes.
    ic_inspect_refresh_scrub(ic, ed, src);

    // The same follow-up a normal run gets, so an owner forwarding working code
    // keeps up with the drag.
    if (ed->on_ran && ic->run_mode == IC_RUN_LOCAL && ic_editor_last_run_ok(ed))
        ed->on_ran(ic, ed, src, ed->on_ran_user);

    if (body) ic->sys->free(body);
    ic->sys->free(new_code);
}

// ===========================================================================
// Hover inspection
// ===========================================================================
// Hover an expression and see the values it took. The buffer is never
// rewritten: an instrumented COPY with __ins(...) spliced around the hovered
// subexpression is compiled and run privately, and the values it reports are
// drawn as a label in nearby whitespace.

// Rows above/below the hovered one to search for a gap.
#define IC_INSPECT_SEARCH_ROWS 5
// Below four cells there is not even room for the "..." that says it was cut.
#define IC_INSPECT_MIN_CHARS   4
// A label longer than this tries two rows before it is truncated into one.
#define IC_INSPECT_SPLIT_LEN   40
// A label longer than this tries three rows before two.
#define IC_INSPECT_SPLIT3_LEN  60
// Blank cells kept each side. A gap that only just fits the text cannot be
// read: dropped into the one space of `a = 1` it comes out as `a =5 1`.
#define IC_INSPECT_PAD         1
// The anchor offset when the span is deliberately NOT under the pointer.
#define IC_INSPECT_NO_ANCHOR   ((size_t)-1)

// `id` is the slot: 0 is the hovered expression, 1..n-1 the pins.
// VM_INSPECT_ID_NONE arrives from the inspect(...) inside a pin's wrapper; the
// values come from the __ins around it, so the inner report is dropped.
static void ic_inspect_sink(void *user, int id, int kind, int fx_shift, double value) {
    InteractiveCoding *ic = (InteractiveCoding *)user;
    if (!ic || id < 0 || id >= ic->ins_num_sites) return;
    ic_inspect_record(&ic->ins_sites[id].stats, kind, fx_shift, value);
}

// The same for an array or slice: `kind`/`fx_shift` are the element's, `vals`
// the first `n` of `total` elements. Same slot rules as the scalar sink.
static void ic_inspect_arr_sink(void *user, int id, int kind, int fx_shift,
                                int total, const double *vals, int n) {
    InteractiveCoding *ic = (InteractiveCoding *)user;
    if (!ic || id < 0 || id >= ic->ins_num_sites) return;
    ic_inspect_record_array(&ic->ins_sites[id].stats, kind, fx_shift, total, vals, n);
}

static void ic_inspect_forget(InteractiveCoding *ic) {
    if (!ic) return;
    ic->ins_node        = NULL;
    // Cleared, so the next frame with something to show re-runs rather than
    // believing a comparison against sites that are gone.
    ic->ins_wrap_args[0] = '\0';
    ic->ins_num_sites   = 1;
    ic->ins_hover_valid = false;
    ic->ins_place_todo  = 2;
    for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) {
        ic->ins_sites[i].label[0] = '\0';
        ic->ins_sites[i].cache.placed   = 0;
        ic->ins_sites[i].row      = -1;
    }
}

// The parse the sites were computed against is going. Locally that is the end of
// them; in host mode the readings are the host's and stay up, and the next
// refresh decides which still stand.
static void ic_inspect_forget_parse(InteractiveCoding *ic) {
    if (ic->run_mode != IC_RUN_HOST) { ic_inspect_forget(ic); return; }
    ic->ins_node        = NULL;
    ic->ins_hover_valid = false;
    ic->ins_place_todo  = 2;
    for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) ic->ins_sites[i].cache.placed = 0;
}

// Drop the hover reading alone, leaving the pins as they stand.
//
// No run: the pins do not depend on the hovered expression, so taking slot 0
// away cannot change them. This is the whole point of the bump timer -- a
// pointer sweeping a line drops its reading on every token it crosses, and
// each of those must be free.
static void ic_inspect_drop_hover(InteractiveCoding *ic) {
    ICInspectSite *s = &ic->ins_sites[IC_INSPECT_ID_HOVER];
    s->label[0] = '\0';
    s->row = s->col = s->row2 = s->col2 = -1;
    s->cache.placed = 0;
    ic_inspect_reset(&s->stats);
    ic->ins_node        = NULL;
    ic->ins_hover_valid = false;
}

// Hovering a function's NAME inspects what the call returns: `sq` in `sq(6)`
// asks "what does this give me", and `sq` alone is not a reportable type.
//
// A call node carries TOK_EMPTYSTRING (the parser's marker for an application)
// with `left` the callee and `right` the argument group -- covering `sq(6)` and
// juxtaposed `sq 6` alike, and excluding a binop parent, whose token is the
// operator. The call owns no characters, so it is only ever reachable as the
// name's parent. Loops, so `f(1)(2)` promotes all the way out from `f`.
static ASTNode *ic_inspect_promote_call(ASTNode *hover) {
    while (hover && hover->parent) {
        ASTNode *p = hover->parent;
        if (p->token != TOK_EMPTYSTRING || !p->right) break;
        // The callee: `sq` in sq(6).
        int from_name = (p->left == hover);
        // The ARGUMENT GROUP: the pointer is on the call's own parens. Asking
        // of the group instead is broken, not merely less useful -- its span
        // starts at the `(`, so `sq(6)` splices to `sq__ins((6), 0)`, gluing
        // the wrapper onto the name. `left_bracket` keeps this to a
        // parenthesised group, so `f 1` still inspects the `1`.
        int from_parens = (p->right == hover && hover->left_bracket);
        if (!from_name && !from_parens) break;
        hover = p;
    }
    return hover;
}

// Is `n` the bare identifier `name`? The parser has no keywords, so `return`
// is an ordinary interned identifier.
static bool ic_ident_is(InteractiveCoding *ic, ASTNode *n, const char *name) {
    if (!n || !n->token || is_fixed_token(n->token) || n->left_bracket) return false;
    const char *s = intern_get_cstr(&ic->parser.intern, n->token);
    return s && s_strcmp(s, name) == 0;
}

// An assignment's RIGHT-HAND SIDE, reached from either the `=` or the name
// being assigned: both mean "what is going INTO a", and neither is inspectable
// as it stands -- the `=` node's span is the whole statement and the name's is
// a binding, so wrapping either fails to compile.
//
// `n` has to be the WHOLE left side. `a: f64 = expr` needs help getting there,
// since the name hangs off a TOK_COLON; `xs[i] = v` arrives whole, because
// ic_inspect_retarget promotes the index first.
//
// Compound assignment takes the OTHER answer: `a += b` keeps a value, so the
// node itself reports the new `a`; its rhs would be wrong by exactly the old
// value.
static ASTNode *ic_inspect_assign_rhs(ASTNode *n) {
    if (!n) return NULL;

    // On the `=`. A group carrying the token is `(a = 1)`, which is itself an
    // expression and is inspected as it stands.
    if (n->token == TOK_EQ && n->left && n->right && !n->left_bracket)
        return n->right;

    // On the name. Climb the annotation first, so `a: f64 = expr` counts.
    ASTNode *t = n;
    if (t->parent && t->parent->token == TOK_COLON && t->parent->left == t)
        t = t->parent;
    ASTNode *p = t->parent;
    if (!p || p->left != t || !p->right || p->left_bracket) return NULL;
    if (p->token == TOK_EQ) return p->right;
    if (TOKEN_IS_COMPOUND_ASSIGN(p->token)) return p;

    return NULL;
}

// A lambda's BODY, when `n` is the `=>`. NULL when there is no single
// expression: a braced body is a statement list with no value of its own.
//
// Never the `=>` itself -- an arrow function is not an expression the compiler
// can take the value of, so it has to be replaced or refused, never passed
// through.
static ASTNode *ic_inspect_arrow_body(ASTNode *n) {
    if (!n || n->token != TOK_ARROW_F) return NULL;
    ASTNode *body = n->right;
    if (!body || body->left_bracket == TOK_LBRACE) return NULL;
    return body;
}

// Is the pointer on a lambda's PARAMETER -- anywhere left of a `=>`? A
// parameter is a binding, not an expression, so wrapping one does not compile.
// Everything left of the arrow is parameters, so reaching the arrow from its
// `left` is the whole test.
static bool ic_inspect_is_lambda_param(ASTNode *n) {
    for (ASTNode *c = n; c && c->parent; c = c->parent)
        if (c->parent->token == TOK_ARROW_F) return c->parent->left == c;
    return false;
}

// Is `n` a lambda's whole parameter list? Hovering one reports the function's
// signatures rather than a value.
static bool ic_inspect_is_param_list(ASTNode *n) {
    return n && n->parent && n->parent->token == TOK_ARROW_F && n->parent->left == n;
}

// What the pointer is really asking about, where the node under it is not the
// interesting expression.
//
//   `return expr`  -- an APPLICATION of the identifier `return`, so the callee
//                     promotion lands on it; retarget to the argument.
//   `=>`           -- the function BODY.
//   `a = expr`     -- the rhs, from the `=` and from the name alike. When that
//                     rhs is a lambda the arrow rule applies on top, so a
//                     function's name asks what its body gives.
//   a parameter    -- the whole parameter list, from a name, annotation,
//                     paren or comma alike: the label is the signatures, and
//                     it is never wrapped.
//
// NULL when there is nothing worth inspecting, which the caller treats like an
// unsupported type: no label and no pin. The pin gesture works on whatever this
// returns, so a shape that cannot be inspected must be refused HERE rather than
// written into the buffer as an `inspect(...)` that will not compile.
static ASTNode *ic_inspect_retarget(InteractiveCoding *ic, ASTNode *hover) {
    if (!hover) return NULL;

    if (hover->token == TOK_ARROW_F) return ic_inspect_arrow_body(hover);
    if (ic_inspect_is_lambda_param(hover)) {
        while (!ic_inspect_is_param_list(hover)) hover = hover->parent;
        return hover;
    }

    // A field or swizzle name is not an expression on its own: `y` in p.y
    // asks what the dot asks. Before the call promotion, so p.xs[i] still
    // climbs to the whole index.
    if (hover->parent && hover->parent->token == TOK_DOT && hover->parent->right == hover)
        hover = hover->parent;

    hover = ic_inspect_promote_call(hover);

    if (hover->token == TOK_EMPTYSTRING && ic_ident_is(ic, hover->left, "return"))
        return hover->right;

    // AFTER the promotion, so `xs[i] = v` is covered: `xs` promotes to the
    // whole index, which is then the left of the assignment like any name.
    ASTNode *rhs = ic_inspect_assign_rhs(hover);
    if (rhs) return rhs->token == TOK_ARROW_F ? ic_inspect_arrow_body(rhs) : rhs;

    return hover;
}

// The text span of `hover`'s whole SUBTREE, as byte offsets into the buffer.
// ed->parse_result is a parse of the raw buffer, so a txt_to_ref offset IS a
// buffer offset -- that equivalence is the mechanism. A cell belongs to the
// span when `hover` is on its node's parent chain, which is why hovering a
// bracket yields the group and hovering an operator yields both operands.
static bool ic_inspect_subtree_span(ParseResult *pr, ASTNode *node, size_t text_len,
                                    size_t *out_lo, size_t *out_hi) {
    return vm_subtree_span(pr, node, text_len, out_lo, out_hi) != 0;
}

static bool ic_inspect_span(ICEditor *ed, ASTNode *hover, const char *text,
                            size_t text_len, size_t hover_off,
                            size_t *out_lo, size_t *out_hi) {
    ParseResult *pr = &ed->parse_result;
    size_t lo, hi;
    if (!ic_inspect_subtree_span(pr, hover, text_len, &lo, &hi)) return false;

    // Refused across a line (a block is not an expression), cut before a
    // comment, trailing blanks trimmed -- in that order (vm_inspect_span_clamp).
    if (!vm_inspect_span_clamp(text, &lo, &hi)) return false;

    // Whatever the clamps did, the span must still cover the pointed-at cell:
    // one trimmed away from the pointer describes something the user is not
    // looking at. IC_INSPECT_NO_ANCHOR switches that off for the retargets that
    // move away from the pointer on purpose (`return`, `=>`).
    if (hover_off != IC_INSPECT_NO_ANCHOR && (hover_off < lo || hover_off >= hi))
        return false;

    *out_lo = lo;
    *out_hi = hi;
    return true;
}

// ---- text insertions (the pin gesture) --------------------------------------
//
// Inspection itself never edits text: every site is a probe (vm_set_inspect_
// probes) on the node its span names. Only pinning writes into the buffer.
typedef struct {
    size_t offset;
    int    depth;     // ties at one offset break outermost-first (see below)
    char   text[24];
} ICInsEdit;

// By (offset, depth). Openers carry ascending depth and closers descending, so
// two insertions on the SAME byte nest rather than interleave.
static void ic_ins_edit_sort(ICInsEdit *e, int n) {
    for (int i = 1; i < n; i++) {
        ICInsEdit key = e[i];
        int j = i - 1;
        while (j >= 0 && (e[j].offset > key.offset ||
                          (e[j].offset == key.offset && e[j].depth > key.depth))) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = key;
    }
}

// text with every edit spliced in. Caller frees.
static char *ic_inspect_splice_many(InteractiveCoding *ic, const char *text, size_t text_len,
                                    ICInsEdit *edits, int n) {
    ic_ins_edit_sort(edits, n);

    size_t extra = 0;
    for (int i = 0; i < n; i++) extra += s_strlen(edits[i].text);

    char *out = (char *)ic->sys->malloc(text_len + extra + 1);
    if (!out) return NULL;

    size_t w = 0, read = 0;
    for (int i = 0; i < n; i++) {
        size_t at = edits[i].offset;
        if (at > text_len) at = text_len;
        if (at > read) { ic->sys->memcpy(out + w, text + read, at - read); w += at - read; read = at; }
        size_t el = s_strlen(edits[i].text);
        ic->sys->memcpy(out + w, edits[i].text, el);
        w += el;
    }
    ic->sys->memcpy(out + w, text + read, text_len - read);
    w += text_len - read;
    out[w] = '\0';
    return out;
}

// ---- pins -------------------------------------------------------------------

// Is `n` an inspect(...) application -- a pin site? The application shape is
// the one ic_inspect_promote_call walks, so this takes inspect(x), inspect x
// and inspect (x) alike.
static bool ic_is_pin_call(InteractiveCoding *ic, ASTNode *n) {
    return n && n->token == TOK_EMPTYSTRING && n->right &&
           ic_ident_is(ic, n->left, "inspect");
}

// Every pin site in the buffer, in source order. Only the span fields are
// written, so a caller can pass ic->ins_sites + 1 (slot 0 is the hover) or a
// scratch array. Over the AST rather than the text: a pin has to be a real call
// node for its argument's span to be known, and `inspect` in a string is not.
static void ic_inspect_collect_pins(InteractiveCoding *ic, ParseResult *pr, ASTNode *n,
                                    size_t text_len, ICInspectSite *out, int max,
                                    int *count) {
    if (!n || *count >= max) return;

    if (ic_is_pin_call(ic, n)) {
        size_t clo, chi, alo, ahi;
        if (ic_inspect_subtree_span(pr, n, text_len, &clo, &chi) &&
            ic_inspect_subtree_span(pr, n->right, text_len, &alo, &ahi)) {
            // The argument's span INCLUDES its brackets, so unwrapping to it
            // leaves `(sq(6))` where `sq(6)` was pinned -- equivalent, but not
            // the inverse of pinning, so the parens accumulate. Trim them.
            if (n->right->left_bracket && ahi - alo >= 2) { alo++; ahi--; }
            ICInspectSite *st = &out[(*count)++];
            st->call_lo = clo; st->call_hi = chi;
            st->arg_lo  = alo; st->arg_hi  = ahi;
        }
        // Keep descending: pinning twice nests, and the inner one is a site.
    }

    ic_inspect_collect_pins(ic, pr, n->left,  text_len, out, max, count);
    ic_inspect_collect_pins(ic, pr, n->right, text_len, out, max, count);
    for (size_t i = 0; i < array_len(&n->items); i++)
        ic_inspect_collect_pins(ic, pr, *(ASTNode **)array_get(&n->items, i),
                                text_len, out, max, count);
}

/// Compile and run the buffer privately, every probe compiled in by node
// (vm_set_inspect_probes) -- the local executor. The text is never touched.
//
// vm_execute_core with everything that REPORTS stripped out, which is the point
// rather than an optimisation: a hover must not change what the user sees or
// what the embedder is told. Nothing here reaches on_code_changed, ed->on_ran,
// ed->result, the four error stages, the print log or any emitter.
//
// on_vm_setup IS called: the embedder's natives have to exist on this VM or a
// body naming one would not compile, and binding them is also what makes the
// values real -- this runs in the same environment a keystroke does.
//
// Every failure is swallowed. Returns whether the program actually RAN, which
// the caller needs because "reported nothing" and "never ran" differ once pins
// exist: a hover that cannot be inspected must not blank every pin to `-`.
//
// A probe marked is_sig wraps nothing: the function's signatures are written to
// `sigs` instead. Read after the compile and whether or not it succeeded --
// every call site compiled before an error has already specialised the function.
static int ic_run_probes(InteractiveCoding *ic, ICEditor *ed, const char *src,
                         const ICProbe *probes, int n, char *sigs, int sigs_max) {
    Parser *p = &ic->parser;
    if (sigs && sigs_max > 0) sigs[0] = '\0';

    int    header_lines = 0;
    size_t body_off = 0;
    char *wrapped = build_wrapped_source(ic, ed, src, &header_lines, &body_off);
    const char *full = wrapped ? wrapped : src;
    size_t full_len = s_strlen(full);

    ParseResult res = parse_to_asts(p, full, PARSE_OUTPUT_REFS);
    if (parser_get_error(&res)) {
        free_parse_result(p, &res);
        if (wrapped) ic->sys->free(wrapped);
        return 0;
    }

    VMProbe  vp[IC_INSPECT_MAX_SITES];
    int      nvp = 0;
    ASTNode *sig_node = NULL;
    for (int i = 0; i < n; i++) {
        size_t lo = body_off + probes[i].lo, hi = body_off + probes[i].hi;
        if (probes[i].is_sig) {
            if (res.txt_to_ref && lo < res.num_txt_to_ref) sig_node = res.txt_to_ref[lo];
            continue;
        }
        ASTNode *node = vm_probe_node_for_span(&res, full, full_len, lo, hi);
        if (!node || nvp >= IC_INSPECT_MAX_SITES) continue;
        vp[nvp].node = node;
        vp[nvp].id   = probes[i].id;
        nvp++;
    }

    VM *vm = vm_create(ic->sys, p);
    if (!vm) {
        free_parse_result(p, &res);
        if (wrapped) ic->sys->free(wrapped);
        return 0;
    }
    // No print sink installed: a hover must not append to the print log.
    vm_set_inspect_sink(vm, ic_inspect_sink, ic);
    vm_set_inspect_array_sink(vm, ic_inspect_arr_sink, ic);
    vm_set_inspect_probes(vm, vp, nvp);
    if (ic->on_vm_setup) ic->on_vm_setup(vm, ic->on_vm_setup_user);

    int ran = 0;
    Func *f = func_create(vm, res.code_tree, &res);
    if (f) {
        size_t need = func_frame_size(f);
        if (need <= 4096) {
            unsigned char frame[4096];
            Args a;
            args_bind(&a, f, frame, sizeof(frame));
            // Status ignored on purpose: values collected before a fault (or
            // before the op budget ran out) are still values worth showing.
            func_run(f, &a, IC_RUN_OP_BUDGET);
            ran = 1;
        }
    }
    if (sig_node && sigs && sigs_max > 0) vm_signature_labels(vm, sig_node, sigs, sigs_max);
    vm_destroy(vm);
    free_parse_result(p, &res);
    if (wrapped) ic->sys->free(wrapped);
    return ran;
}

// A byte offset as a buffer (row, col). line_offsets[k] is the start of row
// k+1 -- the same mapping ast_node_at reads.
static void ic_inspect_rowcol(ParseResult *pr, size_t off, int *out_row, int *out_col) {
    int    row = 0;
    size_t row_start = 0;
    for (int k = 0; k < pr->num_lines_offsets; k++) {
        if (pr->line_offsets[k] > off) break;
        row_start = pr->line_offsets[k];
        row = k + 1;
    }
    *out_row = row;
    *out_col = (int)(off - row_start);
}

static bool ic_probe_eq(const ICProbe *a, const ICProbe *b) {
    return a->id == b->id && a->lo == b->lo && a->hi == b->hi && a->is_sig == b->is_sig;
}

// Hand the host a new probe set. A set equal to the last one is not news: the
// serial stays, so passes in flight for it still land.
static void ic_inspect_publish(InteractiveCoding *ic, ICEditor *ed, const ICProbe *p, int n) {
    if (n > IC_INSPECT_MAX_SITES) n = IC_INSPECT_MAX_SITES;
    if (n == ic->ins_num_probes) {
        int same = 1;
        for (int i = 0; i < n && same; i++) same = ic_probe_eq(&p[i], &ic->ins_probes[i]);
        if (same) return;
    }
    for (int i = 0; i < n; i++) ic->ins_probes[i] = p[i];
    ic->ins_num_probes = n;
    ic->ins_probe_serial++;
    if (ic->on_probes_changed)
        ic->on_probes_changed(ic->ins_probes, n, ed ? ed->code_serial : 0,
                              ic->ins_probe_serial, ic->on_probes_changed_user);
}

// Who is being inspected, and the probe set that asks about them. Slot 0 is the
// hovered expression, 1.. the pins in source order; each site gets its anchors
// and its stats are reset. Returns the probe count.
static int ic_inspect_build_sites(InteractiveCoding *ic, ICEditor *ed, ASTNode *hover,
                                  size_t hover_off, const char *text, size_t text_len,
                                  ICProbe *probes) {
    for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) {
        ICInspectSite *s = &ic->ins_sites[i];
        s->label[0] = '\0';
        s->row = s->col = s->row2 = s->col2 = -1;
        s->call_lo = s->call_hi = s->arg_lo = s->arg_hi = 0;
        s->cache.placed = 0;
        ic_inspect_reset(&s->stats);
    }
    ParseResult *pr = &ed->parse_result;

    int pins = 0;
    if (pr->code_tree)
        ic_inspect_collect_pins(ic, pr, pr->code_tree, text_len,
                                ic->ins_sites + 1, IC_INSPECT_MAX_SITES - 1, &pins);

    size_t hlo = 0, hhi = 0;
    int    have_hover = hover && ic_inspect_span(ed, hover, text, text_len, hover_off, &hlo, &hhi);

    // An expression that is ALREADY pinned would report twice. Keep the pin's
    // label: it is anchored, and it is what the user asked for.
    for (int i = 1; have_hover && i <= pins; i++)
        if (hlo == ic->ins_sites[i].call_lo && hhi == ic->ins_sites[i].call_hi) have_hover = 0;

    // A parameter list is bindings, not a value: nothing is wrapped, the compile
    // is asked for the function's signatures instead.
    int sig = have_hover && ic_inspect_is_param_list(hover);

    ic->ins_num_sites  = pins + 1;
    ic->ins_have_hover = have_hover != 0;
    ic->ins_hover_sig  = sig != 0;
    // Kept: a reading that merely repeats what is on screen is worth nothing.
    ic->ins_hover_span[0] = '\0';
    if (have_hover && hhi - hlo < sizeof(ic->ins_hover_span)) {
        ic->sys->memcpy(ic->ins_hover_span, text + hlo, hhi - hlo);
        ic->ins_hover_span[hhi - hlo] = '\0';
    }

    // Where each label wants to sit: both ends, since placement measures to
    // whichever is nearer. hi is one PAST the span, so the last character is at
    // hi - 1.
    if (have_hover) {
        ic_inspect_rowcol(pr, hlo, &ic->ins_sites[0].row, &ic->ins_sites[0].col);
        ic_inspect_rowcol(pr, hhi > hlo ? hhi - 1 : hlo,
                          &ic->ins_sites[0].row2, &ic->ins_sites[0].col2);
    }
    int n = 0;
    for (int i = 1; i <= pins; i++) {
        ICInspectSite *s = &ic->ins_sites[i];
        ic_inspect_rowcol(pr, s->call_lo, &s->row, &s->col);
        ic_inspect_rowcol(pr, s->call_hi > s->call_lo ? s->call_hi - 1 : s->call_lo,
                          &s->row2, &s->col2);
        probes[n].id = i; probes[n].lo = s->call_lo; probes[n].hi = s->call_hi; probes[n].is_sig = 0;
        n++;
    }
    if (have_hover) {
        probes[n].id = IC_INSPECT_ID_HOVER; probes[n].lo = hlo; probes[n].hi = hhi; probes[n].is_sig = sig;
        n++;
    }
    return n;
}

// Every site's collected stats into its label.
static void ic_inspect_format_sites(InteractiveCoding *ic) {
    for (int i = 0; i < ic->ins_num_sites; i++) {
        ICInspectSite *s = &ic->ins_sites[i];
        if (i == IC_INSPECT_ID_HOVER && (!ic->ins_have_hover || ic->ins_hover_sig)) continue;
        ic_inspect_format(&s->stats, s->label, (int)sizeof(s->label));

        // A pin that never executed (an untaken branch, an uncalled function)
        // must show SOMETHING or it looks broken. A hover has no such problem:
        // no label means the pointer is not on anything reportable.
        if (i != IC_INSPECT_ID_HOVER && s->label[0] == '\0')
            s_snprintf(s->label, sizeof(s->label), "-");
    }

    // Being told `6` over the literal `6` says nothing. `2.500` over `2.5`
    // is the same (padding only), but `7 (10)` over a literal 7 survives -- the
    // count is new information. Hover only: a pin was placed deliberately, and
    // one rendering as nothing looks broken rather than tactful.
    if (ic->ins_have_hover && !ic->ins_hover_sig &&
        ic_inspect_label_is_source(ic->ins_hover_span, ic->ins_sites[0].label))
        ic->ins_sites[0].label[0] = '\0';
}

// Recompute the hovered expression AND every pin. Locally: one private run fills
// every slot and the labels are formatted. In host mode: the probe set goes to
// the host, and the readings on screen stay until its pass arrives.
static void ic_inspect_refresh(InteractiveCoding *ic, ICEditor *ed, ASTNode *hover,
                               size_t hover_off) {
    // Pins occupy a line each in the results panel, so a refresh that changes
    // them makes the panel recompose. Remembered across the reset, so the run
    // that REMOVES the last pin still repaints the panel showing it.
    int had_pins = ic->ins_num_sites - 1;

    // The readings as they stand, put back where this refresh cannot replace
    // them -- otherwise a hover whose run fails drops every pin to the `-` that
    // means "never executed". Stats as well as labels: the in-code label renders
    // from `label`, the results-panel line from `stats`.
    char      prev[IC_INSPECT_MAX_SITES][sizeof(ic->ins_sites[0].label)];
    ICInspect prev_stats[IC_INSPECT_MAX_SITES];
    for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) {
        s_snprintf(prev[i], sizeof(prev[i]), "%s", ic->ins_sites[i].label);
        prev_stats[i] = ic->ins_sites[i].stats;
    }
    ICProbe prev_hover = { 0, 0, 0, 0 };
    int     had_hover = 0;
    for (int i = 0; i < ic->ins_num_probes; i++)
        if (ic->ins_probes[i].id == IC_INSPECT_ID_HOVER) { prev_hover = ic->ins_probes[i]; had_hover = 1; }

    ic->ins_place_todo = 2;
    if (had_pins) ic->result_dirty = true;

    char *text = (ed && ed->ta) ? ui_textarea_get_text(ed->ta) : NULL;
    if (!text) {
        for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) {
            ic->ins_sites[i].label[0] = '\0';
            ic->ins_sites[i].row = -1;
        }
        ic->ins_num_sites  = 1;
        ic->ins_have_hover = false;
        if (ic->run_mode == IC_RUN_HOST) ic_inspect_publish(ic, ed, NULL, 0);
        return;
    }
    size_t text_len = s_strlen(text);

    ICProbe probes[IC_INSPECT_MAX_SITES];
    int     np   = ic_inspect_build_sites(ic, ed, hover, hover_off, text, text_len, probes);
    int     pins = ic->ins_num_sites - 1;
    if (pins) ic->result_dirty = true;

    #define IC_INSPECT_KEEP_PREV()                                          \
        do { for (int i = 1; i <= pins; i++) {                              \
                 s_snprintf(ic->ins_sites[i].label,                         \
                            sizeof(ic->ins_sites[i].label), "%s", prev[i]); \
                 ic->ins_sites[i].stats = prev_stats[i];                    \
             } } while (0)

    if (ic->run_mode == IC_RUN_HOST) {
        ic->sys->free(text);
        // What is on screen is the best there is until the host answers: the
        // pins by position while their count holds, the hover while it is the
        // same span.
        if (pins == had_pins) IC_INSPECT_KEEP_PREV();
        for (int i = 0; i < np; i++) {
            if (probes[i].id != IC_INSPECT_ID_HOVER || !had_hover || !ic_probe_eq(&probes[i], &prev_hover)) continue;
            s_snprintf(ic->ins_sites[0].label, sizeof(ic->ins_sites[0].label), "%s", prev[0]);
            ic->ins_sites[0].stats = prev_stats[0];
        }
        ic_inspect_publish(ic, ed, probes, np);
        return;
    }

    if (np == 0) { ic->sys->free(text); IC_INSPECT_KEEP_PREV(); return; }

    char sigs[sizeof(ic->ins_sites[0].label)];
    int ran = ic_run_probes(ic, ed, text, probes, np, sigs, (int)sizeof(sigs));
    ic->sys->free(text);
    if (ic->ins_hover_sig) s_snprintf(ic->ins_sites[0].label, sizeof(ic->ins_sites[0].label), "%s", sigs);
    if (!ran) { IC_INSPECT_KEEP_PREV(); return; }
    #undef IC_INSPECT_KEEP_PREV

    ic_inspect_format_sites(ic);
}

// Re-read every PIN's values from a source string that is not the buffer, for
// the number scrubber: during a drag the live value exists only in the string
// ic_update_scrubber unparses each frame, so the spans are found in THAT
// string, which needs its own parse, and the pins match the existing slots by
// position in source order. Anchors and placements are untouched -- the buffer
// is unchanged, so the last ordinary refresh's placements still hold. In host
// mode the pins' spans in that string become the probe set instead.
static void ic_inspect_refresh_scrub(InteractiveCoding *ic, ICEditor *ed, const char *src) {
    if (!ic_inspect_on(ic) || ic->ins_ed != ed || ic->ins_num_sites <= 1 || !src) return;

    size_t len = s_strlen(src);
    ParseResult pr = parse_to_asts(&ic->parser, src,
                                   PARSE_VERIFY | PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
    if (parser_get_error(&pr)) { free_parse_result(&ic->parser, &pr); return; }

    ICInspectSite pins[IC_INSPECT_MAX_SITES];
    int           n = 0;
    ic->sys->memset(pins, 0, sizeof(pins));
    if (pr.code_tree)
        ic_inspect_collect_pins(ic, &pr, pr.code_tree, len, pins, IC_INSPECT_MAX_SITES, &n);

    // A different pin count cannot be matched by index, so the old readings
    // stay put. Unparsing a number does not add or remove an inspect(), and if
    // it somehow does, stale beats wrong.
    if (n == ic->ins_num_sites - 1 && n > 0) {
        ICProbe probes[IC_INSPECT_MAX_SITES];
        for (int i = 0; i < n; i++) {
            probes[i].id = i + 1; probes[i].lo = pins[i].call_lo; probes[i].hi = pins[i].call_hi;
            probes[i].is_sig = 0;
        }
        if (ic->run_mode == IC_RUN_HOST) {
            ic_inspect_publish(ic, ed, probes, n);
        } else {
            // Saved before the reset: the run may not happen, and the panel
            // reads these rather than the labels.
            ICInspect prev_stats[IC_INSPECT_MAX_SITES];
            for (int i = 1; i < ic->ins_num_sites; i++) {
                prev_stats[i] = ic->ins_sites[i].stats;
                ic_inspect_reset(&ic->ins_sites[i].stats);
            }

            int ran = ic_run_probes(ic, ed, src, probes, n, NULL, 0);

            // A drag passes through values the code cannot compile with -- an
            // array index out of range, a divisor of zero. Those frames leave
            // the last good reading up rather than flickering every pin to `-`.
            if (ran) {
                for (int i = 1; i < ic->ins_num_sites; i++) {
                    ICInspectSite *s = &ic->ins_sites[i];
                    ic_inspect_format(&s->stats, s->label, (int)sizeof(s->label));
                    if (s->label[0] == '\0') s_snprintf(s->label, sizeof(s->label), "-");
                }
                ic->result_dirty = true;
            } else {
                for (int i = 1; i < ic->ins_num_sites; i++)
                    ic->ins_sites[i].stats = prev_stats[i];
            }
        }
    }
    free_parse_result(&ic->parser, &pr);
}

// The label placement/claim/paint layer. Defined in a file of its own, #included
// like interactive_coding_wrap.c so every build that lists this file gets it.
#include "ic_overlay.c"

// Name catalog, call context, argument hint, comma readings and Tab completion.
#include "ic_complete.c"

// The middle of the TOKEN under (row, col), in screen cells, for placement to
// centre a caption on. The token is the run of characters around that cell
// that all map to the same node -- txt_to_ref's own idea of one, the same run
// the scrubber widens a number over -- so it is the identifier or the operator
// the pointer is on, not the expression it belongs to.
//
// False when the token has no single middle to aim at: off screen, or split
// across two visual lines by wrap.
static bool ic_inspect_token_focus(ICEditor *ed, int row, int col, ASTNode *node,
                                   float *out_x) {
    if (!ed || !ed->ta || !node) return false;
    ParseResult *pr = &ed->parse_result;

    // Whitespace stops the walk as well as a change of node: a node can own
    // characters on the line before this one, and columns are per-line.
    int lo = col, hi = col;
    for (;;) {
        char ch = ui_textarea_get_char_at(ed->ta, row, lo - 1);
        if (ch == '\0' || ch == ' ' || ch == '\t') break;
        if (ast_node_at(pr, row, lo - 1) != node) break;
        lo--;
    }
    for (;;) {
        char ch = ui_textarea_get_char_at(ed->ta, row, hi + 1);
        if (ch == '\0' || ch == ' ' || ch == '\t') break;
        if (ast_node_at(pr, row, hi + 1) != node) break;
        hi++;
    }

    float x0, y0, x1, y1;
    if (!ui_textarea_pos_to_screen(ed->ta, row, lo, &x0, &y0)) return false;
    if (!ui_textarea_pos_to_screen(ed->ta, row, hi, &x1, &y1)) return false;
    if (y0 != y1) return false;
    *out_x = ((float)m_floor(x0) + (float)m_floor(x1)) * 0.5f;
    return true;
}

// ---- pin / unpin -----------------------------------------------------------

// Rewrite the editor's buffer, keeping the view where the user left it --
// ui_textarea_set_text preserves neither caret nor scroll, and a pin is a
// frequent gesture in the middle of code being read.
//
// `at` and `delta` shift the caret when the edit was before it on the same
// line, so it stays on its character rather than sliding by the wrapper.
static void ic_inspect_commit_text(InteractiveCoding *ic, ICEditor *ed,
                                   const char *text, size_t at, int delta) {
    int   row, col;
    float scroll = ui_textarea_get_scroll(ed->ta);
    ui_textarea_get_cursor(ed->ta, &row, &col);

    ParseResult *pr = &ed->parse_result;
    int erow, ecol;
    ic_inspect_rowcol(pr, at, &erow, &ecol);
    if (row == erow && col >= ecol) {
        col += delta;
        if (col < 0) col = 0;
    }

    ui_textarea_set_text(ed->ta, text, false);   // false: ctrl-Z undoes a pin
    ui_textarea_set_cursor(ed->ta, row, col);
    ui_textarea_set_scroll(ed->ta, scroll);

    ed->needs_parse = true;
    ed->pending_run = (ic->auto_run != 0);

    // Everything cached keys off a parse that is about to be thrown away.
    ic->ins_node = NULL;
    ic->ins_ed   = NULL;
    ic->ins_hover_valid = false;
    for (int i = 0; i < IC_INSPECT_MAX_SITES; i++) {
        ic->ins_sites[i].label[0] = '\0';
        ic->ins_sites[i].cache.placed   = 0;
    }
    ic->ins_num_sites  = 1;
    ic->ins_place_todo = 2;
}

// Does `text` hold a pin whose whole call is exactly [lo, hi)?
//
// The paren-less spelling has to be CHECKED rather than reasoned about: a
// paren-less argument scope runs to the end of the enclosing expression, so
// `inspect sq(6) * 2` pins the product and not the call the pointer was on.
// Parsing the candidate and asking what the `inspect` it built covers is the
// answer that cannot drift out of step with the parser.
static bool ic_pin_covers(InteractiveCoding *ic, const char *text, size_t lo, size_t hi) {
    size_t      len = s_strlen(text);
    ParseResult pr  = parse_to_asts(&ic->parser, text,
                                    PARSE_VERIFY | PARSE_OUTPUT_REFS | PARSE_OUTPUT_LINE_OFFSETS);
    bool ok = false;
    if (!parser_get_error(&pr) && pr.code_tree) {
        ICInspectSite pins[IC_INSPECT_MAX_SITES];
        int           n = 0;
        ic->sys->memset(pins, 0, sizeof(pins));
        ic_inspect_collect_pins(ic, &pr, pr.code_tree, len, pins, IC_INSPECT_MAX_SITES, &n);
        for (int i = 0; i < n && !ok; i++)
            ok = (pins[i].call_lo == lo && pins[i].call_hi == hi);
    }
    free_parse_result(&ic->parser, &pr);
    return ok;
}

// Wrap [lo, hi) in an inspect, making the reading permanent.
//
// Spelled `inspect expr` wherever that says the same thing -- the mark reads
// like the code it sits in rather than bracketing it. `inspect(expr)` is the
// fallback for the spans a paren-less argument would over-reach, which is every
// span with more expression after it: `sq(6)` of `sq(6) * 2`, `a` of `a.b`, an
// element of a list.
static void ic_inspect_pin(InteractiveCoding *ic, ICEditor *ed, size_t lo, size_t hi) {
    char *text = ui_textarea_get_text(ed->ta);
    if (!text) return;
    size_t len = s_strlen(text);
    if (hi > len) { ic->sys->free(text); return; }

    ICInsEdit edits[2];
    int n = 0;
    edits[n].offset = lo; edits[n].depth = 0;
    s_snprintf(edits[n].text, sizeof(edits[n].text), "inspect "); n++;

    char *out = ic_inspect_splice_many(ic, text, len, edits, n);
    if (out && !ic_pin_covers(ic, out, lo, hi + 8)) {
        ic->sys->free(out);
        n = 0;
        edits[n].offset = lo; edits[n].depth = 0;
        s_snprintf(edits[n].text, sizeof(edits[n].text), "inspect("); n++;
        edits[n].offset = hi; edits[n].depth = 1;
        s_snprintf(edits[n].text, sizeof(edits[n].text), ")"); n++;
        out = ic_inspect_splice_many(ic, text, len, edits, n);
    }
    ic->sys->free(text);
    if (!out) return;
    // Both spellings open with the same 8 bytes, so the cursor shift is the
    // same either way.
    ic_inspect_commit_text(ic, ed, out, lo, 8);
    ic->sys->free(out);
}

// Delete the `inspect(` and `)` of one wrapper, keeping its argument verbatim
// -- so unpinning is the inverse of pinning even after edits inside it.
static char *ic_inspect_unwrap(InteractiveCoding *ic, const char *text, size_t len,
                               const ICInspectSite *pins, int n) {
    char *out = (char *)ic->sys->malloc(len + 1);
    if (!out) return NULL;
    size_t w = 0, read = 0;
    for (int i = 0; i < n; i++) {
        const ICInspectSite *p = &pins[i];
        if (p->call_lo < read || p->arg_lo < p->call_lo ||
            p->arg_hi < p->arg_lo || p->call_hi < p->arg_hi || p->call_hi > len) continue;
        ic->sys->memcpy(out + w, text + read, p->call_lo - read);  w += p->call_lo - read;
        ic->sys->memcpy(out + w, text + p->arg_lo, p->arg_hi - p->arg_lo);
        w += p->arg_hi - p->arg_lo;
        read = p->call_hi;
    }
    ic->sys->memcpy(out + w, text + read, len - read); w += len - read;
    out[w] = '\0';
    return out;
}

static void ic_inspect_unpin(InteractiveCoding *ic, ICEditor *ed, const ICInspectSite *pin) {
    char *text = ui_textarea_get_text(ed->ta);
    if (!text) return;
    size_t len = s_strlen(text);
    char *out = ic_inspect_unwrap(ic, text, len, pin, 1);
    ic->sys->free(text);
    if (!out) return;
    ic_inspect_commit_text(ic, ed, out, pin->call_lo,
                           -(int)((pin->call_hi - pin->call_lo) - (pin->arg_hi - pin->arg_lo)));
    ic->sys->free(out);
}

// The right-click gesture: pin what is under the pointer, or un-pin it. Returns
// true when the buffer was rewritten.
//
// Taken at the TOP of the frame rather than in
// ic_editor_update_cursor_and_hover at the bottom: the reading the user clicked
// was computed against the node resolved here, and re-deriving one later can
// land on a different node.
static bool ic_inspect_toggle_pin(InteractiveCoding *ic, ICEditor *ed) {
    if (!ic->ins_hover_valid) return false;

    // Already pinned. Two spans match, so a pin comes off from either end: the
    // whole call (what the word `inspect` resolves to) or the expression.
    for (int i = 1; i < ic->ins_num_sites; i++) {
        ICInspectSite *p = &ic->ins_sites[i];
        if ((ic->ins_hover_lo == p->call_lo && ic->ins_hover_hi == p->call_hi) ||
            (ic->ins_hover_lo == p->arg_lo  && ic->ins_hover_hi == p->arg_hi)) {
            ICInspectSite copy = *p;
            ic_inspect_unpin(ic, ed, &copy);
            return true;
        }
    }

    if (ic->ins_num_sites >= IC_INSPECT_MAX_SITES) return false;
    ic_inspect_pin(ic, ed, ic->ins_hover_lo, ic->ins_hover_hi);
    return true;
}

// Draw every pinned reading for one editor, re-running the placement search
// when the anchor moved on screen (scroll, resize, rewrap) or while
// ins_place_todo counts down an edit the anchor test cannot see.
static void ic_inspect_draw_pins(InteractiveCoding *ic, UIContext *ui, ICEditor *sed) {
    if (!sed || !sed->ta) return;

    // A scrub's digit wheels move every frame and are painted just before
    // this, so a placement from before the drag can end up under one.
    ic->ov.force_place = ic->ins_place_todo > 0 || ic->rc_drag_target != NULL;

    for (int i = 1; i < ic->ins_num_sites; i++) {
        ICInspectSite *s = &ic->ins_sites[i];
        ICOverlayLabel l = { IC_OV_PIN, s->label, NULL, s->row, s->col, s->row2, s->col2,
                             -1.0f, -1, 0, 0, &s->cache };
        ic_overlay_add(ic, ui, sed->ta, &l);
    }
    ic->ov.force_place = 0;
    if (ic->ins_place_todo > 0) ic->ins_place_todo--;
}

static void ic_inspect_draw_hover(InteractiveCoding *ic, UIContext *ui, ICEditor *sed,
                                  bool have_focus, float focus_x) {
    ICInspectSite *h = &ic->ins_sites[IC_INSPECT_ID_HOVER];
    // The pointer may be off the reading's own span (a retarget), or on a
    // token that scrolled since; the expression's middle is the fallback.
    ICOverlayLabel l = { IC_OV_HOVER, h->label, NULL, h->row, h->col, h->row2, h->col2,
                         have_focus ? focus_x : -1.0f, -1, 0, 0, NULL };
    ic_overlay_add(ic, ui, sed->ta, &l);
}

void ic_update_inspect(InteractiveCoding *ic, UIContext *ui) {
    if (!ic || !ui) return;
    bool host_stale = ic->ins_stale;
    ic->ins_stale = false;
    if (!ic_inspect_on(ic)) {
        // The host stops inspecting too.
        if (ic->run_mode == IC_RUN_HOST) ic_inspect_publish(ic, ic->ins_ed, NULL, 0);
        return;
    }

    // A scrub owns the pointer, so there is no HOVER reading while one runs.
    // The PINS stay up and stay live -- ic_update_scrubber refreshes them,
    // being the only place the scrubbed source exists -- so all that is left
    // here is to draw them.
    if (ic->rc_drag_target) {
        ic_inspect_draw_pins(ic, ui, ic->ins_ed);
        return;
    }

    float mx = m_floorf(ui->mouse_x), my = m_floorf(ui->mouse_y);

    // Resolved from THIS frame's mouse against the layout last frame's draw
    // left behind -- deliberately not from ed->mouse_hover_node, which is a
    // frame behind. The label is cached but its PLACEMENT is recomputed from
    // the live mouse, so a stale node drew the old token's value beside the new
    // token for a frame. The layout being one frame old is harmless: moving the
    // mouse does not reflow the textarea.
    ICEditor *ed = NULL;
    int mr = 0, mc = 0;
    for (int i = 0; i < ic->num_editors; i++) {
        ICEditor *e = ic->editors[i];
        if (e->ta && ui_textarea_screen_to_pos(e->ta, mx, my, &mr, &mc)) { ed = e; break; }
    }

    // Whose sites are live: the editor under the pointer, else the one that
    // already has them (so pins survive the mouse leaving the code), else the
    // active one (without which a buffer full of pins shows nothing until the
    // pointer has visited it once). One at a time -- one run serves one
    // buffer.
    ICEditor *sed = ed ? ed : (ic->ins_ed ? ic->ins_ed : ic_active_editor(ic));

    // A pending re-parse means the spans would be measured against text that
    // has already changed.
    if (!sed || !sed->ta || sed->needs_parse) {
        ic->ins_node = NULL;
        ic->ins_ed   = NULL;
        ic->ins_hover_valid  = false;
        ic->ins_hover_pending = NULL;   // the wait restarts on the new parse
        ic->ins_sites[0].label[0] = '\0';
        ic->ins_wrap_args[0] = '\0';
        return;
    }

    // ---- what is under the pointer ----
    ASTNode *hover      = NULL;
    ASTNode *raw        = NULL;     // the node under the pointer, before any retarget
    size_t   hover_off   = 0;
    float    focus_x     = 0;       // middle of the token under the pointer
    bool     have_focus  = false;
    if (ed == sed) {
        // The pointer has to be on an actual character. screen_to_pos CLAMPS
        // the column to the end of the line, so the empty space right of one
        // resolves to its last position -- which carries a node, and flashed a
        // label whenever the pointer left a token sideways.
        char ch = ui_textarea_get_char_at(ed->ta, mr, mc);
        hover = (ch == '\0' || ch == ' ' || ch == '\t')
              ? NULL : ast_node_at(&ed->parse_result, mr, mc);

        // The pointed-at cell as a buffer offset, by ast_node_at's arithmetic.
        // ic_inspect_span checks its final span still covers this.
        ParseResult *pr = &ed->parse_result;
        if (mr == 0) hover_off = (size_t)mc;
        else if (pr->line_offsets && mr - 1 < pr->num_lines_offsets)
            hover_off = pr->line_offsets[mr - 1] + (size_t)mc;
        else hover = NULL;

        raw = hover;
        // From the token the pointer is ON, not the expression a retarget
        // moves the reading to: it is where the eye already is.
        have_focus = ic_inspect_token_focus(ed, mr, mc, raw, &focus_x);
        hover = ic_inspect_retarget(ic, hover);
        // A retarget moves the span off the pointer on purpose, so the anchor
        // check stands down for it.
        if (hover != raw) hover_off = IC_INSPECT_NO_ANCHOR;
        // A comma with a reading of its own, and the open Tab menu, take the
        // pointer's attention: no value reading beside them.
        if (ic->cp.cmh_suppress || ic->cp.menu_open) hover = NULL;
    }

    // ---- the bump timer ----
    // A newly hovered expression waits IC_INSPECT_HOVER_DELAY before it is
    // inspected, and the clock is bumped every time the pointer lands on a
    // different one -- so crossing a line costs nothing and only settling
    // spends a run. Until it fires the reading is simply ABSENT: holding the
    // previous token's label would place it against the new token, which is
    // the one thing the layout above goes out of its way not to do.
    //
    // The pin gesture overrides it. Right-clicking a token is settling on it
    // by any reasonable reading, and the pin needs the span this refresh
    // computes.
    if (hover != ic->ins_hover_pending) {
        ic->ins_hover_pending = hover;
        ic->ins_hover_since   = ui->time;
    }
    bool hover_ready = hover == ic->ins_node ||
                       ui->mouse_pressed[UI_MOUSE_BUTTON_RIGHT] ||
                       (ui->time - ic->ins_hover_since) >= IC_INSPECT_HOVER_DELAY;
    if (!hover_ready) hover = NULL;

    // Losing the hover needs no run, and taking that shortcut is what makes a
    // sweep across a line free even with pins up.
    if (!hover && ic->ins_node && sed == ic->ins_ed) ic_inspect_drop_hover(ic);

    // ---- refresh ----
    // Recompute when the pointed-at node changes, when the editor changes, when
    // the code RE-RAN, or when the wrap's ARGUMENTS moved. The last two are
    // what keep a reading current while the pointer sits perfectly still.
    //
    // Arguments are compared as the TEXT that would be spliced, so the check
    // is exactly as coarse as the run. Guarded on there being a reading on
    // screen at all: the run is a full parse+compile+execute, so merely having
    // the editor open must not cost a compile per frame.
    //
    // In host mode the arguments, and every value, are the host's: nothing to
    // watch here.
    char args_now[IC_WRAP_ARGS_MAX];
    args_now[0] = '\0';
    bool host_run = ic->run_mode == IC_RUN_HOST;
    if (!host_run) ic_wrap_call_args(ic, sed, args_now, (int)sizeof(args_now));
    bool have_reading = hover || ic->ins_num_sites > 1;
    bool args_moved = !host_run && have_reading &&
                      (host_stale || s_strcmp(args_now, ic->ins_wrap_args) != 0);

    if (hover != ic->ins_node || sed != ic->ins_ed ||
        sed->run_serial != ic->ins_run_serial || args_moved) {
        ic->ins_node       = hover;
        ic->ins_ed         = sed;
        ic->ins_run_serial = sed->run_serial;
        s_snprintf(ic->ins_wrap_args, sizeof(ic->ins_wrap_args), "%s", args_now);
        ic_inspect_refresh(ic, sed, hover, hover_off);

        // Kept whether or not a label came of it: the pin gesture works on this
        // span, and an already-pinned expression suppresses its label but must
        // still be un-pinnable. Not for a parameter list, which cannot be wrapped.
        ic->ins_hover_valid = false;
        if (hover && !ic_inspect_is_param_list(hover)) {
            char *t = ui_textarea_get_text(sed->ta);
            if (t) {
                ic->ins_hover_valid = ic_inspect_span(sed, hover, t, s_strlen(t), hover_off,
                                                      &ic->ins_hover_lo, &ic->ins_hover_hi);
                ic->sys->free(t);
            }
        }
    }

    // ---- the gesture ----
    // number_flags CLEAR on the cell under the pointer: a numeric literal
    // belongs to the scrubber, which arms on the same field. Disjoint by
    // construction, so neither consumes the button. Tested on `raw`, not the
    // retarget, so the name or `=` of `y = 5` still pins its literal rhs.
    // The pin rewrites the buffer, so the scrubber (armed later this frame)
    // could find a number under the pointer that was not there at the click.
    if (ed == sed && ui->mouse_pressed[UI_MOUSE_BUTTON_RIGHT] &&
        hover && raw && !raw->number_flags && ic_inspect_toggle_pin(ic, sed)) {
        ui_consume_mouse_press(ui, UI_MOUSE_BUTTON_RIGHT);
        return;
    }

    // Pins first: they are fixed, so the transient hover label is the one that
    // gives way when the two compete for a gap.
    ic_inspect_draw_pins(ic, ui, sed);
    ic_complete_add_hint(ic, ui);
    if (ed == sed) ic_inspect_draw_hover(ic, ui, sed, have_focus, focus_x);
}

void ic_update_overlay(InteractiveCoding *ic, UIContext *ui) {
    if (!ic || !ui) return;
    ic_overlay_begin(ic);
    ic_complete_input(ic, ui);
    ic_complete_add_menu(ic, ui);
    ic_update_inspect(ic, ui);
    if (!ic->ov.hint_added) ic_complete_add_hint(ic, ui);
    ic_complete_add_comma(ic, ui);
    ic_overlay_flush(ic, ui);
    ic_complete_draw_menu(ic, ui);
}

void ic_editor_update_gutter_toggle(InteractiveCoding *ic, UIContext *ui, UITextArea *ta) {
    int gutter_row;
    bool in_gutter = ui_textarea_screen_to_gutter_row(ta, ui->mouse_x, ui->mouse_y, &gutter_row);
    if (in_gutter && ui->mouse_pressed[UI_MOUSE_BUTTON_RIGHT]) {
        ic->rc_gutter_active = ta;
        ic->rc_anchor_row   = gutter_row;
        ic->rc_current_row  = gutter_row;
    }
    if (ic->rc_gutter_active == ta) {
        if (in_gutter)
            ic->rc_current_row = gutter_row;
        if (ui->mouse_released[UI_MOUSE_BUTTON_RIGHT]) {
            int r1 = ic->rc_anchor_row;
            int r2 = ic->rc_current_row;
            if (r1 > r2) { int t = r1; r1 = r2; r2 = t; }
            ui_textarea_undo_begin_batch(ta);
            for (int r = r1; r <= r2; r++)
                toggle_comment_line(ta, r);
            ui_textarea_undo_end_batch(ta);
            ic->rc_gutter_active = NULL;
        }
        if (!ui->mouse_down[UI_MOUSE_BUTTON_RIGHT])
            ic->rc_gutter_active = NULL;
    }
}

// Replace the results panel's text, keeping the view where the user left it.
//
// clear_history is TRUE on purpose: the panel is a display surface with one
// writer, so its undo history is dead weight -- and unbounded, since
// undo_end_edit() never retires a group. Clearing it also resets caret and
// scroll, hence the save/restore.
static void ic_result_set_text(InteractiveCoding *ic, const char *text) {
    int row, col;
    ui_textarea_get_cursor(ic->result_ta, &row, &col);
    float scroll = ui_textarea_get_scroll(ic->result_ta);
    ui_textarea_set_text(ic->result_ta, text, true);
    ui_textarea_set_cursor(ic->result_ta, row, col);
    ui_textarea_set_scroll(ic->result_ta, scroll);
}

// The pinned readings, one line each, as they appear in the results panel.
//
// The in-code label has to fit a gap in the whitespace, so it summarises; the
// panel has a whole line, so it shows the values themselves. Each line is
// identified by the source line its site is on -- sixteen lines all reading
// "Inspect:" would not be a readout. Returns the length written into `dst`.
static size_t ic_inspect_result_lines(InteractiveCoding *ic, ICEditor *ped,
                                      char *dst, size_t dst_size) {
    if (!dst || dst_size == 0) return 0;
    dst[0] = '\0';
    // Only the editor whose sites are live, and only when it is the pane the
    // panel is showing.
    if (!ic_inspect_on(ic) || ic->ins_ed != ped || ic->ins_num_sites <= 1) return 0;

    size_t w = 0;
    for (int i = 1; i < ic->ins_num_sites; i++) {
        ICInspectSite *s = &ic->ins_sites[i];
        if (s->row < 0) continue;

        char prefix[32];
        s_snprintf(prefix, sizeof(prefix), "Inspect L%d: ", s->row + 1);

        char line[IC_INSPECT_LINE_MAX];
        int  n = ic_inspect_format_line(&s->stats, prefix, line, (int)sizeof(line),
                                        IC_INSPECT_LINE_MAX - 1);
        if (n <= 0) continue;
        if (w + (size_t)n + 2 >= dst_size) break;
        dst[w++] = '\n';
        ic->sys->memcpy(dst + w, line, (size_t)n);
        w += (size_t)n;
        dst[w] = '\0';
    }
    return w;
}

void ic_format_result_with_outputs(InteractiveCoding *ic) {
    struct { const char *label; bool show; const char *text; } out[] = {
        { "C Code",   ic->show_c_code,   ic->c_code_output   },
        { "JS",       ic->show_js,       ic->js_output       },
        { "Lua",      ic->show_lua,      ic->lua_output      },
        { "MicroW8",  ic->show_curlywas, ic->curlywas_output },
    };
    const int n_out = (int)(sizeof(out) / sizeof(out[0]));

    // Whichever editor's result the panel is showing.
    ICEditor    *ped     = ic->active_override ? ic->active_override : &ic->freeform;
    const char  *pbytes  = (const char *)ped->print_out.data;
    size_t       plen    = array_len(&ped->print_out);
    const char  *pnote   = ped->print_capped  ? IC_PRINT_CAP_NOTE
                         : ped->print_dropped ? "\nwarning: print output dropped (out of memory)"
                                              : "";

    // Ahead of the print log, which grows to megabytes -- anything after it is
    // effectively invisible.
    char   ins_lines[IC_INSPECT_MAX_SITES * IC_INSPECT_LINE_MAX];
    size_t ins_len = ic_inspect_result_lines(ic, ped, ins_lines, sizeof(ins_lines));

    size_t total = s_strlen(ic->result) + ins_len + 1;
    bool any = ins_len > 0;
    if (plen > 0 || pnote[0]) {
        total += 1 + plen + s_strlen(pnote);   // a '\n' between result and log
        any = true;
    }
    for (int i = 0; i < n_out; i++) {
        if (!out[i].show || !out[i].text) continue;
        total += s_strlen(out[i].label) + s_strlen(out[i].text) + 16;   // "\n--- " + " ---\n"
        any = true;
    }

    char *combined = any ? (char *)ic->sys->malloc(total) : NULL;
    if (!combined) { ic_result_set_text(ic, ic->result); return; }

    size_t p = (size_t)s_snprintf(combined, total, "%s", ic->result);
    if (ins_len) {
        ic->sys->memcpy(combined + p, ins_lines, ins_len);
        p += ins_len;
        combined[p] = '\0';
    }
    if (plen > 0 || pnote[0]) {
        // memcpy, not snprintf: the log is arbitrary bytes and may be large.
        combined[p++] = '\n';
        if (plen > 0) { ic->sys->memcpy(combined + p, pbytes, plen); p += plen; }
        size_t nl = s_strlen(pnote);
        if (nl)  { ic->sys->memcpy(combined + p, pnote, nl); p += nl; }
        combined[p] = '\0';
    }
    for (int i = 0; i < n_out; i++) {
        if (!out[i].show || !out[i].text) continue;
        p += (size_t)s_snprintf(combined + p, total - p, "\n--- %s ---\n%s",
                                out[i].label, out[i].text);
    }
    ic_result_set_text(ic, combined);
    ic->sys->free(combined);
}

// The editor the user is typing in: whatever an owner pointed active_override
// at, else the one this instance owns.
static ICEditor *ic_active_editor(InteractiveCoding *ic) {
    if (!ic) return NULL;
    return ic->active_override ? ic->active_override : &ic->freeform;
}

static UITextArea *ic_active_ta(InteractiveCoding *ic) {
    ICEditor *ed = ic_active_editor(ic);
    return ed ? ed->ta : NULL;
}

bool interactive_coding_get_results_rect(InteractiveCoding *ic,
                                         int *x, int *y, int *w, int *h) {
    if (!ic || ic->result_rect_w <= 0 || ic->result_rect_h <= 0) return false;
    if (x) *x = ic->result_rect_x;
    if (y) *y = ic->result_rect_y;
    if (w) *w = ic->result_rect_w;
    if (h) *h = ic->result_rect_h;
    return true;
}

bool interactive_coding_last_run_ok(InteractiveCoding *ic) {
    ICEditor *ed = ic_active_editor(ic);
    return ed ? ic_editor_last_run_ok(ed) : false;
}

bool interactive_coding_has_editor_focus(InteractiveCoding *ic) {
    if (!ic || !ic->ui) return false;
    // Also true for the results panel: the user can select and copy there, and
    // without this a host accelerator table steals Ctrl+C before it reaches
    // result_ta's handle_key().
    if (ui_textarea_has_focus(ic->ui, ic->result_ta)) return true;
    UITextArea *ta = ic_active_ta(ic);
    if (!ta) return false;
    return ui_textarea_has_focus(ic->ui, ta);
}

void interactive_coding_set_editor_focus(InteractiveCoding *ic) {
    if (!ic || !ic->ui) return;
    UITextArea *ta = ic_active_ta(ic);
    if (ta) ic->ui->focus_id = ui_id_from_ptr(ta);
}

void interactive_coding_get_cursor(InteractiveCoding *ic, int *row, int *col) {
    UITextArea *ta = ic_active_ta(ic);
    if (!ta) {
        if (row) *row = -1;
        if (col) *col = -1;
        return;
    }
    ui_textarea_get_cursor(ta, row, col);
}

void interactive_coding_set_cursor(InteractiveCoding *ic, int row, int col) {
    UITextArea *ta = ic_active_ta(ic);
    if (ta) ui_textarea_set_cursor(ta, row, col);
}

float interactive_coding_get_scroll(InteractiveCoding *ic) {
    UITextArea *ta = ic_active_ta(ic);
    return ta ? ui_textarea_get_scroll(ta) : 0.0f;
}

void interactive_coding_set_scroll(InteractiveCoding *ic, float scroll) {
    UITextArea *ta = ic_active_ta(ic);
    if (ta) ui_textarea_set_scroll(ta, scroll);
}

// ===========================================================================
// Chrome
// ===========================================================================
// The frame around the code area: the split, the draggable bar, the results
// panel and the run buttons. Split out of interactive_coding_frame() so a
// multi-editor host can compose its own code area inside the same furniture.

void ic_chrome_layout(InteractiveCoding *ic, int y, int h, int top_rows, ICChrome *o) {
    o->btn_row = y + h - 1;

    // Everything but the caller's top rows, the results bar and the button row
    // is split between the code area and the results panel.
    o->avail_h = h - 2 - top_rows;
    if (o->avail_h < 2) o->avail_h = 2;
    o->content_y = y + top_rows;

    o->min_frac = 2.0f / (float)o->avail_h;
    o->max_frac = 1.0f - o->min_frac;
    // A frame too small to hold two rows on each side (a first frame before
    // the window has a size) has min above max: clamping would drive the
    // fraction to the bottom and keep it there once the frame grows.
    if (o->min_frac <= o->max_frac) {
        if (ic->results_split_t < o->min_frac) ic->results_split_t = o->min_frac;
        if (ic->results_split_t > o->max_frac) ic->results_split_t = o->max_frac;
    }

    o->content_h = (int)(ic->results_split_t * (float)o->avail_h);
    if (o->content_h < 2) o->content_h = 2;
    o->header_y = o->content_y + o->content_h;
    o->result_y = o->header_y + 1;
    o->result_h = o->avail_h - o->content_h;
    if (o->result_h < 1) {
        o->result_h  = 1;
        o->content_h = o->avail_h - 1;
        o->header_y  = o->content_y + o->content_h;
    }
}

void ic_chrome_results_header(InteractiveCoding *ic, UIContext *ui, int x, int w,
                              const ICChrome *c) {
    unsigned int header_col = ui_theme_color(ui, UI_COL_COLD_ACCENT);
    // Draw text FIRST (first-draw wins) so it appears on top
    ui_draw_text(ui, (float)(x + 2), (float)c->header_y, "Results", header_col);
    
    // Draw background LAST (fills only undrawn gaps)
    for (int cx = x; cx < x + w; cx++) {
        ui_draw_cell(ui, (float)cx, (float)c->header_y, ' ', header_col);
    }

    float mx = ui->mouse_x;
    float my = ui->mouse_y;
    bool in_header = (my >= (float)c->header_y && my < (float)(c->header_y + 1) &&
                      mx >= (float)x && mx < (float)(x + w));

    if (ic->dragging_results_header) {
        if (!ui->mouse_down[UI_MOUSE_BUTTON_LEFT]) {
            ic->dragging_results_header = false;
        } else {
            float dy = my - ic->drag_anchor_y_results;
            float new_split = ic->drag_anchor_split_results + dy / (float)c->avail_h;
            if (new_split < c->min_frac) new_split = c->min_frac;
            if (new_split > c->max_frac) new_split = c->max_frac;
            ic->results_split_t = new_split;
        }
    }

    if (in_header && ui->mouse_pressed[UI_MOUSE_BUTTON_LEFT]) {
        ic->dragging_results_header = true;
        ic->drag_anchor_y_results     = my;
        ic->drag_anchor_split_results = ic->results_split_t;
    }
}

void ic_chrome_results_panel(InteractiveCoding *ic, UIContext *ui, int x, int w,
                             const ICChrome *c) {
    // Recorded here, not by the caller: both compositions come through this
    // function, and the split above it is draggable.
    ic->result_rect_x = x;
    ic->result_rect_y = c->result_y;
    ic->result_rect_w = w;
    ic->result_rect_h = c->result_h;
    ui_textarea_absolute_pos(ui, ic->result_ta, x, c->result_y, w, c->result_h);
}

int ic_chrome_button_row(InteractiveCoding *ic, UIContext *ui, int x, int w,
                         const ICChrome *c) {
    int flags = 0;

    // Align with the editor's left edge, not column 1 -- which detaches this
    // row from the code box whenever the frame is placed at x != 1.
    ui_set_cursor(ui, (float)x, (float)c->btn_row);

    // No Run button while Auto-run is on: it could only repeat the run that
    // just happened. Read before the checkbox, so the frame the box is ticked
    // still shows the button the row was laid out with.
    if (!ic->auto_run) {
        if (ui_button(ui, "Run")) flags |= IC_BTN_RUN;
        ui_same_line(ui);
    }
    bool auto_changed = ui_check_box(ui, "Auto-run", &ic->auto_run) != 0;
    ui_same_line(ui);

    bool show_c_changed, show_js_changed, show_lua_changed, show_mw8_changed;
    {
        static const char *const items[4] = { "C", "JS", "Lua", "MicroW8" };
        bool shown[4] = { ic->show_c_code, ic->show_js, ic->show_lua, ic->show_curlywas };
        // Own id per editor: meld and the accordion draw several of these rows.
        ui_push_id_ptr(ui, ic);
        int changed = ui_multi_dropdown(ui, "Preview", 4, items, shown, "None");
        ui_pop_id(ui);
        ic->show_c_code = shown[0]; ic->show_js = shown[1]; ic->show_lua = shown[2]; ic->show_curlywas = shown[3];
        show_c_changed   = (changed & 1) != 0;
        show_js_changed  = (changed & 2) != 0;
        show_lua_changed = (changed & 4) != 0;
        show_mw8_changed = (changed & 8) != 0;
    }
    const int flowed_w = (int)(ui->last_x + ui->last_w) - x;

    // Editor aids, at the right end of the row -- not part of the "Preview"
    // group. Right-aligned only when the row is wide enough to clear the
    // Preview header, since a narrow pane would land on top of it.
    {
        // "Unpin" appears only once there is something to unpin.
        const bool has_pins  = (ic->ins_num_sites > 1);
        const int  kUnpinW   = has_pins ? 8 : 0;  // " [Unpin]"
        const int  kEditorW  = 24 + kUnpinW;      // "Editor: Hint, Inspect v"
        if (w > flowed_w + kEditorW + 2) ui_same_line_col(ui, w - kEditorW + 6);
        else                             ui_same_line_pad(ui, 2);
        static const char *const aids[2] = { "Hint", "Inspect" };
        // Unchecked and greyed out while the host has ruled inspection out; the
        // user's own setting is left alone behind it.
        bool on[2] = { ic->cp.hints, ic_inspect_on(ic) };
        const bool off[2] = { false, !ic->inspect_available };
        ui_push_id_ptr(ui, ic);
        int aid_changed = ui_multi_dropdown_ex(ui, "Editor", 2, aids, on, NULL, UI_MULTI_CHECKBOXES, off);
        ui_pop_id(ui);
        ic->cp.hints = on[0];
        if ((aid_changed & 2) && ic->inspect_available) ic->hover_inspect = on[1];
        // The gesture removes one pin; this removes them all.
        if (has_pins) {
            ui_same_line(ui);
            if (ui_button(ui, "Unpin")) interactive_coding_strip_inspections(ic);
        }
    }

    if (auto_changed || show_c_changed || show_js_changed || show_lua_changed || show_mw8_changed) {
        ic->result_dirty = true;
    }

    // Emitted text only exists after a run, so switching a box on before one
    // would look like the click did nothing. Ask for a run.
    if ((show_c_changed   && ic->show_c_code   && !ic->c_code_output)   ||
        (show_js_changed  && ic->show_js       && !ic->js_output)       ||
        (show_lua_changed && ic->show_lua      && !ic->lua_output)      ||
        (show_mw8_changed && ic->show_curlywas && !ic->curlywas_output)) {
        flags |= IC_BTN_RUN;
    }
    return flags;
}

// The results text for a single editor, every error stage spelled out -- this
// panel is the whole of what the user gets told.
static void ic_render_own_result(InteractiveCoding *ic) {
    if (!ic->result_dirty) return;

    ICEditor *ed = &ic->freeform;
    ic->result[0] = '\0';
    char *p  = ic->result;
    size_t rem = sizeof(ic->result);

    if (ed->has_visual_parse_error) {
        int n = s_snprintf(p, rem, "[Visual Parse] %s\n", ed->visual_parse_error);
        if (n > 0 && (size_t)n < rem) { p += n; rem -= (size_t)n; }
    }

    if (ed->has_parse_error) {
        ic->result_is_error = true;
        s_snprintf(p, rem, "[Parse] %s\n", ed->parse_error);
    } else if (ed->has_compile_error) {
        ic->result_is_error = true;
        s_snprintf(p, rem, "[Compile] %s\n", ed->compile_error);
    } else if (ed->has_host_error) {
        ic->result_is_error = true;
        s_snprintf(p, rem, "[Host] %s\n", ed->host_error);
    } else if (ed->has_execute_error) {
        ic->result_is_error = true;
        s_snprintf(p, rem, "[Execute] %s\n", ed->execute_error);
    } else if (ed->execute_error[0] != '\0') {
        ic->result_is_error = false;
        s_snprintf(p, rem, "%s", ed->execute_error);
    } else {
        ic->result_is_error = false;
        if (rem > 1) s_snprintf(p, rem, "(not run)");
    }

    ic_format_result_with_outputs(ic);
    ic->result_dirty = false;
}

// Pen-relative entry point. Unlike the textarea wrappers this one restores
// line_start_x: the chrome inside places its rows with ui_set_cursor, which
// rewrites it, and ui_advance would then send the pen home to somewhere inside
// the frame rather than the caller's column.
void interactive_coding_frame(InteractiveCoding *ic, UIContext *ui, int w, int h) {
    if (!ic || !ui) return;
    float x = ui->pen_x, y = ui->pen_y;
    float saved_line_start = ui->line_start_x;
    interactive_coding_frame_absolute_pos(ic, ui, (int)x, (int)y, w, h);
    ui->line_start_x = saved_line_start;
    ui_advance(ui, x, y, (float)w, (float)h);
}

void interactive_coding_frame_absolute_pos(InteractiveCoding *ic, UIContext *ui,
                                           int x, int y, int w, int h) {
    if (!ic) return;
    ic->ui = ui;

    // The widgets have no width negotiation -- the button row is simply as wide
    // as its labels -- so in a narrow frame it would run past x+w and paint
    // over whatever is to the right. Clipping truncates the row at the edge and
    // stops the hidden part taking clicks. Popups are unaffected: overlay items
    // are drawn from ui_begin, outside this push/pop.
    ui_push_clip_rect(ui, (float)x, (float)y, (float)w, (float)h);

    // Every label in here is a constant, so two instances on screen at once
    // hash to the same widget ids -- which breaks BOTH: the first drawn claims
    // active_id on press, then fails its own hit test on release and clears it,
    // so the button under the cursor never fires. Scoping on the instance
    // pointer separates them for every widget below at once.
    ui_push_id_ptr(ui, ic);

    ICChrome c;
    ic_chrome_layout(ic, y, h, 0, &c);

    // ---- Code area ----
    // Both overlays first, so they claim their cells before the textarea does.
    ic_update_scrubber(ic, ui);
    ic_update_overlay(ic, ui);

    int ta_changed = ui_textarea_absolute_pos(ui, ic->freeform.ta, x, c.content_y, w, c.content_h);
    if (ic->freeform.pending_run || (ic->auto_run && ta_changed)) {
        ic->freeform.pending_run = false;
        ic_editor_run(ic, &ic->freeform);
    }
    ic_editor_update_cursor_and_hover(ic, ui, &ic->freeform);
    ic_editor_update_gutter_toggle(ic, ui, ic->freeform.ta);

    // ---- Chrome ----
    ic_chrome_results_header(ic, ui, x, w, &c);
    ic_render_own_result(ic);
    ic_chrome_results_panel(ic, ui, x, w, &c);

    if (ic_chrome_button_row(ic, ui, x, w, &c) & IC_BTN_RUN) {
        ic_editor_run(ic, &ic->freeform);
    }

    ui_pop_id(ui);
    ui_pop_clip_rect(ui);
}

void interactive_coding_on_select(InteractiveCoding *ic) {
    ICEditor *ed = ic_active_editor(ic);
    if (!ed) return;
    ed->needs_parse = true;
    // An overridden (pane) editor re-runs unconditionally on select; the one
    // this instance owns only when auto-run is on.
    ed->pending_run = ic->active_override ? true : (ic->auto_run != 0);
}

// Back to the state before anything ran: no result, no error stage, nothing
// emitted. For when the buffer's contents are replaced and will NOT be re-run,
// so that what the panel shows stops describing text that is no longer there.
static void ic_editor_clear_run_state(InteractiveCoding *ic, ICEditor *ed) {
    ic_clear_emitted_outputs(ic);
    ic_print_reset(ed);
    ed->has_error_bg      = false;
    ed->has_parse_error   = false;
    ed->has_compile_error = false;
    ed->has_execute_error = false;
    ed->has_host_error    = false;
    ed->execute_error[0]  = '\0';
    s_snprintf(ed->result, sizeof(ed->result), "(not run)");
    ed->result_is_error = false;
    ed->result_dirty    = true;
    ic->result_dirty    = true;
}

static void ic_set_text(InteractiveCoding *ic, const char *text, bool clear_history) {
    ICEditor *ed = ic_active_editor(ic);
    if (!ed || !ed->ta) return;
    // a clear drops the steps a held undo group would merge
    if (clear_history && ic->undo_group_ta == ed->ta) ic->undo_group_ta = NULL;
    ui_textarea_set_text(ed->ta, text ? text : "", clear_history);
    ed->needs_parse = true;
    // Replacing the buffer is a content change like any keystroke, but it comes
    // from the embedder rather than the textarea, so it raises no ta_changed for
    // the frame loop to react to. Without this an owner swapping the buffer (a
    // different entry selected, an external edit pulled in) leaves the results,
    // the inspection readouts and the error line describing the PREVIOUS text --
    // and with auto-run off, where there is no re-run to replace them, they have
    // to be cleared instead.
    ed->pending_run = (ic->auto_run != 0);
    if (!ed->pending_run) ic_editor_clear_run_state(ic, ed);
}

void interactive_coding_set_text(InteractiveCoding *ic, const char *text) {
    ic_set_text(ic, text, false);
}

void interactive_coding_set_text_and_clear_history(InteractiveCoding *ic, const char *text) {
    ic_set_text(ic, text, true);
}

// The group stays on the textarea it opened on, so an editor switch mid-group
// still closes the right one.
void interactive_coding_begin_undo_group(InteractiveCoding *ic) {
    if (!ic || ic->undo_group_ta) return;
    ICEditor *ed = ic_active_editor(ic);
    if (!ed || !ed->ta) return;
    ic->undo_group_ta = ed->ta;
    ic->undo_group_mark = ui_textarea_undo_mark(ed->ta);
}

void interactive_coding_end_undo_group(InteractiveCoding *ic) {
    if (!ic || !ic->undo_group_ta) return;
    ui_textarea_undo_merge_since(ic->undo_group_ta, ic->undo_group_mark);
    ic->undo_group_ta = NULL;
}

bool interactive_coding_in_undo_group(InteractiveCoding *ic) {
    return ic && ic->undo_group_ta != NULL;
}

void interactive_coding_set_code_changed_callback(
    InteractiveCoding *ic, InteractiveCodingCodeChangedFn cb, void *user) {
    if (!ic) return;
    ic->on_code_changed = cb;
    ic->on_code_changed_user = user;
}

void interactive_coding_set_vm_setup_callback(
    InteractiveCoding *ic, InteractiveCodingVmSetupFn cb, void *user) {
    if (!ic) return;
    ic->on_vm_setup = cb;
    ic->on_vm_setup_user = user;
}

void interactive_coding_set_wrap_args_callback(
    InteractiveCoding *ic, InteractiveCodingWrapArgsFn cb, void *user) {
    if (!ic) return;
    ic->on_wrap_args = cb;
    ic->on_wrap_args_user = user;
}

void interactive_coding_set_freeform_wrap(InteractiveCoding *ic, const char *name,
                                           const char *params, char open, char close) {
    if (!ic) return;
    ic_editor_set_wrap(ic, &ic->freeform, name, params, open, close,
                       ic->freeform_run_body);
}

void interactive_coding_set_freeform_run_body(InteractiveCoding *ic, bool run) {
    if (!ic || ic->freeform_run_body == run) return;
    ic->freeform_run_body = run;
    // Written through as well as remembered: ic_editor_set_wrap early-outs when
    // nothing about the wrap changed, so an owner re-asserting the same name
    // and params would never carry it across.
    ic->freeform.wrap_append_call = run;
    ic->freeform.needs_parse = true;
    ic->freeform.pending_run = true;
}

void interactive_coding_set_hover_inspect(InteractiveCoding *ic, bool on) {
    if (!ic || ic->hover_inspect == on) return;
    ic->hover_inspect = on;
    // Drop the cached labels so switching off clears the screen next frame. The
    // inspect(...) wrappers stay in the buffer: this gates the private run, it
    // does not edit the user's text.
    ic_inspect_forget(ic);
    ic->ins_ed = NULL;
}

void interactive_coding_set_inspect_available(InteractiveCoding *ic, bool available) {
    if (!ic || ic->inspect_available == available) return;
    ic->inspect_available = available;
    // Same as the box being switched: drop the cached labels and the editor
    // they belonged to, so nothing stale draws when it comes back.
    ic_inspect_forget(ic);
    ic->ins_ed = NULL;
}

void interactive_coding_invalidate_inspection(InteractiveCoding *ic) {
    if (ic) ic->ins_stale = true;
}

int interactive_coding_strip_inspections(InteractiveCoding *ic) {
    if (!ic) return 0;
    ICEditor *ed = ic_active_editor(ic);
    if (!ed || !ed->ta) return 0;

    // The sites come from the AST, so the parse has to describe the buffer.
    if (ed->needs_parse) ic_editor_parse_for_feedback(ic, ed);

    char *text = ui_textarea_get_text(ed->ta);
    if (!text) return 0;
    size_t len = s_strlen(text);

    // A scratch list, not ic->ins_sites: this runs whether or not inspection is
    // switched on, and must not disturb what is on screen.
    ICInspectSite pins[IC_INSPECT_MAX_SITES];
    int           n = 0;
    ic->sys->memset(pins, 0, sizeof(pins));
    if (ed->parse_result.code_tree)
        ic_inspect_collect_pins(ic, &ed->parse_result, ed->parse_result.code_tree, len,
                                pins, IC_INSPECT_MAX_SITES, &n);

    if (n == 0) { ic->sys->free(text); return 0; }

    char *out = ic_inspect_unwrap(ic, text, len, pins, n);
    ic->sys->free(text);
    if (!out) return 0;

    ic_inspect_commit_text(ic, ed, out, pins[0].call_lo, 0);
    ic->sys->free(out);
    return n;
}

void interactive_coding_set_freeform_prelude(InteractiveCoding *ic, const char *prelude) {
    if (!ic) return;
    ic_editor_set_prelude(ic, &ic->freeform, prelude);
}

char *interactive_coding_get_text(InteractiveCoding *ic) {
    if (!ic) return NULL;

    // During a scrub the buffer is frozen at the pre-drag text and the live
    // value lives in the mutated AST. Return that unparsed, so callers see the
    // code the editor is evaluating this frame -- and the text the buffer will
    // hold on release, since the commit writes exactly this. The raw buffer
    // would hand back the stale value and fight the code-changed callback.
    if (ic->rc_drag_target) {
        ASTNode *root = ast_node_get_root(ic->rc_drag_target);
        char *code = unparse_expression(&ic->parser, root);
        if (code) {
            char *body = strip_editor_wrap(ic, ic->rc_drag_ed, code);
            if (body) { ic->sys->free(code); return body; }
            return code;
        }
        // Unparse failed: fall through to the buffer rather than returning NULL.
    }

    ICEditor *ed = ic_active_editor(ic);
    return (ed && ed->ta) ? ui_textarea_get_text(ed->ta) : NULL;
}

bool interactive_coding_is_scrubbing(InteractiveCoding *ic) {
    return ic && ic->rc_drag_target != NULL;
}

// ===========================================================================
// Host-run mode
// ===========================================================================

void interactive_coding_set_run_mode(InteractiveCoding *ic, ICRunMode mode) {
    if (!ic || ic->run_mode == mode) return;
    // Leaving host mode, the host stops inspecting.
    if (ic->run_mode == IC_RUN_HOST) ic_inspect_publish(ic, ic->ins_ed, NULL, 0);
    ic->run_mode = mode;
    ic_inspect_forget(ic);
    ic->ins_ed = NULL;
    for (int i = 0; i < ic->num_editors; i++) {
        ICEditor *ed = ic->editors[i];
        ic_editor_clear_run_state(ic, ed);
        ed->host_serial = 0;
        ed->needs_parse = true;
        ed->pending_run = true;
    }
}

ICRunMode interactive_coding_get_run_mode(InteractiveCoding *ic) {
    return ic ? ic->run_mode : IC_RUN_LOCAL;
}

void interactive_coding_set_local_check(InteractiveCoding *ic, bool on) {
    if (!ic || ic->local_check == on) return;
    ic->local_check = on;
    if (!on) {
        ICEditor *ed = ic_active_editor(ic);
        if (ed) ed->has_compile_error = false;
        ic_clear_emitted_outputs(ic);
        ic->result_dirty = true;
    }
}

void interactive_coding_set_code_changed_callback2(
    InteractiveCoding *ic, InteractiveCodingCodeChanged2Fn cb, void *user) {
    if (!ic) return;
    ic->on_code_changed2 = cb;
    ic->on_code_changed2_user = user;
}

void interactive_coding_set_probes_changed_callback(
    InteractiveCoding *ic, InteractiveCodingProbesChangedFn cb, void *user) {
    if (!ic) return;
    ic->on_probes_changed = cb;
    ic->on_probes_changed_user = user;
}

int interactive_coding_get_probes(InteractiveCoding *ic, ICProbe *out, int max,
                                  unsigned *probe_serial) {
    if (!ic) return 0;
    if (probe_serial) *probe_serial = ic->ins_probe_serial;
    int n = ic->ins_num_probes;
    for (int i = 0; i < n && i < max && out; i++) out[i] = ic->ins_probes[i];
    return n;
}

void interactive_coding_host_begin(InteractiveCoding *ic, unsigned code_serial) {
    if (!ic) return;
    ICEditor *ed = ic_active_editor(ic);
    // Newest wins, never "current only": a host is always behind a scrub drag.
    ic->host_ed = ed;
    ic->host_ok = ed && ic->run_mode == IC_RUN_HOST &&
                  (ed->host_serial == 0 || (int)(code_serial - ed->host_serial) >= 0);
    if (!ic->host_ok) return;
    ed->host_serial       = code_serial;
    ed->has_host_error    = false;
    ed->host_error[0]     = '\0';
    ed->host_error_row    = ed->host_error_col = -1;
    ed->has_execute_error = false;
    ed->execute_error[0]  = '\0';
    ic_print_reset(ed);
}

void interactive_coding_host_error(InteractiveCoding *ic, int stage, int row, int col,
                                   const char *msg) {
    if (!ic || !ic->host_ok || !ic->host_ed) return;
    ICEditor *ed = ic->host_ed;
    msg = msg ? strip_vm_pos_prefix(msg) : "error";
    if (stage == IC_HOST_RUN) {
        if (ed->has_execute_error) return;   // the first one is the one that stopped it
        ed->has_execute_error = true;
        s_snprintf(ed->execute_error, sizeof(ed->execute_error), "Error: %s", msg);
        return;
    }
    if (ed->has_host_error) return;
    ed->has_host_error = true;
    ed->host_error_row = row;
    ed->host_error_col = col;
    const char *what = stage == IC_HOST_PARSE ? "Parse" : "Compile";
    if (row >= 0 && col >= 0)
        s_snprintf(ed->host_error, sizeof(ed->host_error), "%s error at %d:%d: %s", what, row + 1, col + 1, msg);
    else
        s_snprintf(ed->host_error, sizeof(ed->host_error), "%s error: %s", what, msg);
}

void interactive_coding_host_result(InteractiveCoding *ic, const char *text) {
    if (!ic || !ic->host_ok || !ic->host_ed || ic->host_ed->has_execute_error) return;
    s_snprintf(ic->host_ed->execute_error, sizeof(ic->host_ed->execute_error), "%s", text ? text : "");
}

void interactive_coding_host_print(InteractiveCoding *ic, const unsigned char *s, int n) {
    if (!ic || !ic->host_ok || !ic->host_ed || !s || n < 0) return;
    ic_print_sink(ic->host_ed, s, n);
}

void interactive_coding_host_end(InteractiveCoding *ic) {
    if (!ic) return;
    ICEditor *ed = ic->host_ed;
    bool ok = ic->host_ok;
    ic->host_ok = false;
    ic->host_ed = NULL;
    if (!ok || !ed) return;
    if (!ed->has_parse_error && !ed->has_compile_error) ic_host_error_bg(ed);
    ic_editor_store_result(ic, ed);
    ic->result_dirty = true;
}

void interactive_coding_inspect_begin(InteractiveCoding *ic, unsigned probe_serial) {
    if (!ic) return;
    ic->ins_pass_ok = ic->run_mode == IC_RUN_HOST && ic_inspect_on(ic) && ic->ins_ed &&
                      probe_serial == ic->ins_probe_serial;
    if (!ic->ins_pass_ok) return;
    for (int i = 0; i < ic->ins_num_sites; i++) ic_inspect_reset(&ic->ins_sites[i].stats);
}

void interactive_coding_inspect_value(InteractiveCoding *ic, int id, int kind, int fx_shift, double v) {
    if (ic && ic->ins_pass_ok) ic_inspect_sink(ic, id, kind, fx_shift, v);
}

void interactive_coding_inspect_array(InteractiveCoding *ic, int id, int kind, int fx_shift,
                                      int total, const double *vals, int n) {
    if (ic && ic->ins_pass_ok) ic_inspect_arr_sink(ic, id, kind, fx_shift, total, vals, n);
}

void interactive_coding_inspect_sigs(InteractiveCoding *ic, int id, const char *labels) {
    if (!ic || !ic->ins_pass_ok || id != IC_INSPECT_ID_HOVER || !ic->ins_hover_sig) return;
    s_snprintf(ic->ins_sites[0].label, sizeof(ic->ins_sites[0].label), "%s", labels ? labels : "");
}

void interactive_coding_inspect_end(InteractiveCoding *ic) {
    if (!ic || !ic->ins_pass_ok) return;
    ic->ins_pass_ok = false;
    ic_inspect_format_sites(ic);
    for (int i = 0; i < ic->ins_num_sites; i++) ic->ins_sites[i].cache.placed = 0;
    ic->ins_place_todo = 2;
    if (ic->ins_num_sites > 1) ic->result_dirty = true;
}

#endif
