#include "ui_glue.h"

const float ui_glue_zoom_values [UI_GLUE_ZOOM_COUNT] = { 0.36f,0.5f,0.666666f, 0.8f,      0.95f, 1.0f, 1.25f, 2.f };
const float ui_glue_scale_values[UI_GLUE_ZOOM_COUNT] = { 0.8f, 1.0f,1.0,       1.0f, 1.0f/0.95f, 1.0f, 1.0f, 1.0f };

const char *const ui_glue_zoom_items_auto[UI_GLUE_ZOOM_AUTO_COUNT] = {
    "Very Tiny", "Tiny (a)", "Smaller", "Small", "Medium (a)", "Medium Spacy (a)", "Large", "Huge",
    "Auto"
};
// The plain list is that same table read one item short, not a second copy
// of it: two lists of the same names would sooner or later drift apart.
const char *const *const ui_glue_zoom_items = ui_glue_zoom_items_auto;

int ui_blit_to_textmode(UIContext *ui, TextMode *tm) {
    int cols = (ui->cols < tm->cols) ? ui->cols : tm->cols;
    int rows = (ui->rows < tm->rows) ? ui->rows : tm->rows;
    int changed = 0;

    for (int r = 0; r < rows; r++) {
        OutputCell *src = &ui->screen[r * ui->cols];
        TMCell     *dst = &tm->cells[r * tm->cols];
        for (int c = 0; c < cols; c++) {
            if (!changed &&
                (src[c].ch       != dst[c].ch       ||
                 src[c].flags    != dst[c].flags    ||
                 src[c].color    != dst[c].color    ||
                 src[c].scale    != dst[c].scale    ||
                 src[c].weight   != dst[c].weight   ||
                 src[c].offset_x != dst[c].offset_x ||
                 src[c].offset_y != dst[c].offset_y)) {
                changed = 1;
            }
            dst[c].ch       = src[c].ch;
            dst[c].flags    = src[c].flags;
            dst[c].color    = src[c].color;
            dst[c].scale    = src[c].scale;
            dst[c].weight   = src[c].weight;
            dst[c].offset_x = src[c].offset_x;
            dst[c].offset_y = src[c].offset_y;
        }
    }

    return changed;
}

int ui_glue_scale_picker(UIContext *ui, UiGluePickerStyle style, const char *label,
                          int *index, const char *const items[], const float *values,
                          int count, float *out_value, int flags) {
    // ui_option_bar takes `const char *const items[]` directly; ui_dropdown
    // takes `const char *items[]` (it never writes through it either, the
    // signature just predates the const-items convention), so the cast below
    // is just bridging that mismatch, not discarding real constness.
    int changed = (style == UI_GLUE_PICKER_OPTION_BAR)
        ? ui_option_bar(ui, label, index, count, items, flags)
        : ui_dropdown(ui, label, index, count, (const char **)items, flags);
    if (changed && out_value) {
        *out_value = values[*index];
    }
    return changed;
}

static int ui_glue_zoom_nearest_index(float value) {
    // No fabsf: this file also compiles into the freestanding wasm build
    // (--no-standard-libraries), which has no libm/math.h.
    int index = 0;
    float diff = value - ui_glue_zoom_values[0];
    float best = diff < 0.f ? -diff : diff;
    for (int i = 1; i < UI_GLUE_ZOOM_COUNT; i++) {
        diff = value - ui_glue_zoom_values[i];
        float d = diff < 0.f ? -diff : diff;
        if (d < best) { best = d; index = i; }
    }
    return index;
}

float ui_glue_zoom_snap(float value) {
    return ui_glue_zoom_values[ui_glue_zoom_nearest_index(value)];
}

int ui_glue_zoom_index_for_dpi(float dpi) {
    // Anchors: 100% display scale wants "Tiny (a)", 200% wants "Medium (a)".
    // The listed levels aren't evenly spaced, so interpolate in zoom value and
    // then snap, rather than interpolating over the indices.
    float lo = ui_glue_zoom_values[UI_GLUE_ZOOM_TINY_A_INDEX];
    float hi = ui_glue_zoom_values[UI_GLUE_ZOOM_MEDIUM_A_INDEX];
    return ui_glue_zoom_nearest_index(lo + (dpi - 1.0f) * (hi - lo));
}

// Applies a level to both halves of the zoom state. Returns nonzero if *value
// actually moved (the caller regrids off that, ui->global_scale needs no
// regrid so it is just written).
static int ui_glue_zoom_apply(UIContext *ui, float *value, int index) {
    int moved = (*value != ui_glue_zoom_values[index]);
    *value = ui_glue_zoom_values[index];
    ui->global_scale = ui_glue_scale_values[index];
    return moved;
}

int ui_glue_draw_zoom(UIContext *ui, float *value) {
    int index = ui_glue_zoom_nearest_index(*value);
    int changed = ui_glue_scale_picker(ui, UI_GLUE_PICKER_DROPDOWN, "Zoom", &index,
                                        ui_glue_zoom_items, ui_glue_zoom_values, UI_GLUE_ZOOM_COUNT,
                                        value, UI_DROPDOWN_HIDE_SELECTION);
    if (changed) {
        // Each zoom level carries the in-cell glyph scale that makes it look
        // right (the levels whose cell size doesn't divide evenly need a nudge);
        // picking a level applies it, same as if the Scale slider was dragged.
        ui->global_scale = ui_glue_scale_values[index];
    }
    return changed;
}

int ui_glue_draw_zoom_auto(UIContext *ui, float *value, float dpi, int *auto_enabled) {
    int on = (auto_enabled && *auto_enabled);
    int index = on ? UI_GLUE_ZOOM_AUTO_INDEX : ui_glue_zoom_nearest_index(*value);
    // out_value stays NULL: "Auto" has no zoom value of its own to copy out,
    // so both branches below assign *value themselves.
    int changed = ui_glue_scale_picker(ui, UI_GLUE_PICKER_DROPDOWN, "Zoom", &index,
                                        ui_glue_zoom_items_auto, ui_glue_zoom_values,
                                        UI_GLUE_ZOOM_AUTO_COUNT, NULL,
                                        UI_DROPDOWN_HIDE_SELECTION);
    if (changed) {
        on = (index == UI_GLUE_ZOOM_AUTO_INDEX);
        if (auto_enabled) *auto_enabled = on;
        if (!on) changed = ui_glue_zoom_apply(ui, value, index) || changed;
    }
    if (on) {
        // Every frame, not just the frame Auto was picked: this is what makes
        // the zoom follow a dpi change (moving the window to another display).
        changed = ui_glue_zoom_apply(ui, value, ui_glue_zoom_index_for_dpi(dpi)) || changed;
    }
    return changed;
}

int ui_glue_draw_scale(UIContext *ui) {
    // Same width the Grade/LineH pair uses, so the three sliders in the header
    // line up rather than each sizing itself off its own label.
    ui_push_item_width(ui, 18);
    int changed = ui_slider(ui, "Scale", &ui->global_scale,
                            UI_GLUE_SCALE_MIN, UI_GLUE_SCALE_DEFAULT, UI_GLUE_SCALE_MAX);
    ui_pop_item_width(ui);
    return changed;
}

int ui_glue_draw_theme(UIContext *ui) {
    // Same 18 cells as Scale/Grade/LineH so the header's sliders line up.
    ui_push_item_width(ui, 18);
    // 0 as the low end, not the default: the black point is a floor, and being
    // able to take it all the way to true black is the point of the control.
    int changed = ui_slider(ui, "Black", &ui->theme_black_point,
                            0.0f, UI_THEME_BLACK_POINT_DEFAULT, UI_GLUE_BLACK_POINT_MAX);
    ui_same_line(ui);
    changed |= ui_slider(ui, "Text", &ui->theme_text_intensity,
                         UI_GLUE_TEXT_INTENSITY_MIN, UI_THEME_TEXT_INTENSITY_DEFAULT,
                         UI_GLUE_TEXT_INTENSITY_MAX);
    ui_pop_item_width(ui);
    return changed;
}

int ui_glue_draw_grade_lineh(UIContext *ui, float *line_height,
                              float lineh_min, float lineh_middle, float lineh_max) {
    ui_push_item_width(ui, 18);
    int changed = ui_slider(ui, "Grade", &ui->global_weight, 0.3f, 1.0f, 2.0f);
    ui_same_line(ui);
    changed |= ui_slider(ui, "LineH", line_height, lineh_min, lineh_middle, lineh_max);
    ui_pop_item_width(ui);
    return changed;
}
