#ifndef UI_GLUE_H
#define UI_GLUE_H

#include "textmode.h"
#include "textmode_ui.h"

// Copy UIContext screen to TextMode cells, comparing each cell.
// Returns 1 if any cell changed (render needed), 0 if identical to last frame.
// Always performs the copy; use the return value to decide whether tm_render
// must be called again.
int ui_blit_to_textmode(UIContext *ui, TextMode *tm);

// Widget style for ui_glue_scale_picker: how the list of named values is
// presented. Dropdown suits a longer list (ui_demo's 5-step Zoom); option
// bar gives every item equal width, so the row can't shift as the selection
// changes -- better for a handful of items (madteasynth's S/M/L UI size).
typedef enum {
    UI_GLUE_PICKER_DROPDOWN,
    UI_GLUE_PICKER_OPTION_BAR,
} UiGluePickerStyle;

// Draws a labeled control that picks one of `count` named values (e.g. font
// scale steps, line-height steps). `*index` is the active item, persisted by
// the caller across frames. On a change, `*index` is updated and, if
// `out_value` is non-NULL, `*out_value` is set to `values[*index]` -- the
// caller decides whether to apply that immediately (as ui_demo does) or
// stash it for a deferred regrid (as madteasynth does). `flags` is passed
// through to the underlying widget (e.g. UI_DROPDOWN_HIDE_SELECTION; ignored
// by the option-bar style). Returns nonzero if the selection changed this
// frame.
int ui_glue_scale_picker(UIContext *ui, UiGluePickerStyle style, const char *label,
                          int *index, const char *const items[], const float *values,
                          int count, float *out_value, int flags);

// Canonical "Zoom" font-scale control: Tiny/Small/Medium/Large/Huge, mapped
// to ui_glue_zoom_values (0.5/0.8/1.0/1.5/2.0). ui_demo's header uses it
// directly; madteasynth uses it in place of its old 3-item S/M/L bar.
#define UI_GLUE_ZOOM_COUNT 8
// UI_GLUE_ZOOM_COUNT names -- a pointer, not an array, because it aliases the
// front of ui_glue_zoom_items_auto below (see ui_glue.c).
extern const char *const *const ui_glue_zoom_items;
extern const float ui_glue_zoom_values[UI_GLUE_ZOOM_COUNT];
// The ui->global_scale each zoom level should be drawn at -- applied by
// ui_glue_draw_zoom whenever the selection changes.
extern const float ui_glue_scale_values[UI_GLUE_ZOOM_COUNT];

// The two levels the display-scale mapping is anchored to (see
// ui_glue_zoom_index_for_dpi).
#define UI_GLUE_ZOOM_TINY_A_INDEX   1   // "Tiny (a)"
#define UI_GLUE_ZOOM_MEDIUM_A_INDEX 4   // "Medium (a)"

// The zoom level that suits a display scale, where `dpi` is the factor
// each_frame is handed (1.0 = 96 DPI = 100%). The mapping is the straight
// line through 100% -> "Tiny (a)" and 200% -> "Medium (a)"; the level nearest
// that line is returned, so scales outside 100..200% still land somewhere
// sensible rather than clamping at the anchors.
int ui_glue_zoom_index_for_dpi(float dpi);

// The same list with an extra trailing "Auto" item, for ui_glue_draw_zoom_auto.
#define UI_GLUE_ZOOM_AUTO_COUNT (UI_GLUE_ZOOM_COUNT + 1)
#define UI_GLUE_ZOOM_AUTO_INDEX UI_GLUE_ZOOM_COUNT
extern const char *const ui_glue_zoom_items_auto[UI_GLUE_ZOOM_AUTO_COUNT];

// Draws the Zoom dropdown. *value is snapped to its nearest listed level
// every frame purely for display (so a value that arrived from outside this
// widget -- e.g. plugin state a host just restored -- still shows a valid
// selection instead of none); it is only overwritten when the user actually
// picks a different item. On a change it also sets ui->global_scale to the
// picked level's ui_glue_scale_values entry, so the in-cell glyph scale
// follows the zoom without the caller doing it. Returns nonzero if the
// selection changed this frame.
int ui_glue_draw_zoom(UIContext *ui, float *value);

// The listed zoom level nearest `value`, without drawing anything -- for
// normalizing a value read from persisted state (e.g. a restored host
// chunk) before the widget gets a chance to draw and snap it itself.
float ui_glue_zoom_snap(float value);

// ui_glue_draw_zoom plus an "Auto" item, for callers that know the display
// scale. `*auto_enabled` is the caller's persisted flag: set while "Auto" is
// the selection, cleared as soon as a fixed level is picked. Whenever it is
// set -- every frame, not just the frame it is picked -- *value and
// ui->global_scale are re-derived from `dpi`, so the zoom follows the display
// it is on. Returns nonzero when the selection changed or when Auto moved
// *value, i.e. whenever the caller needs to regrid.
int ui_glue_draw_zoom_auto(UIContext *ui, float *value, float dpi, int *auto_enabled);

// Continuous "Scale" slider, bound straight to ui->global_scale -- the scale
// each glyph is drawn at inside its cell, the exact sibling of the Grade
// slider's ui->global_weight. Unlike Zoom it leaves the cell size alone, so
// the grid keeps its column and row count and there is nothing to regrid: it
// applies on the frame it is dragged, wherever the caller draws it. Sits left
// of ui_glue_draw_zoom in both headers. Restores item width to what it was on
// entry. Returns nonzero if the value changed this frame.
#define UI_GLUE_SCALE_MIN     0.5f
#define UI_GLUE_SCALE_DEFAULT 1.0f
#define UI_GLUE_SCALE_MAX     2.0f
int ui_glue_draw_scale(UIContext *ui);

// Draws a "Grade" / "LineH" slider pair on the current line (call
// ui_same_line first if it shouldn't start a new row). Grade is bound
// directly to ui->global_weight (glyph weight, drag-curve 0/1/2). LineH
// writes into *line_height using the given min/middle/max drag curve, so
// each caller can point it at whatever actually drives its layout and use
// whatever range makes sense there: ui_demo binds ui->global_line_height
// directly and applies it immediately, while madteasynth binds a pending
// value that's picked up by its regrid path on the next Draw. Restores item
// width to what it was on entry. Returns nonzero if either slider changed
// this frame.
int ui_glue_draw_grade_lineh(UIContext *ui, float *line_height,
                              float lineh_min, float lineh_middle, float lineh_max);

// Draws the "Black"/"Text" pair that grades the whole theme: the black point
// (where the darkest colour sits) and the text intensity (where UI_COL_DEFAULT's
// text lands). Both are bound straight to the UIContext and read by
// ui_theme_color on every lookup, so they apply on the frame they are dragged
// with nothing to regrid -- the sibling of ui_glue_draw_grade_lineh, and drawn
// beside it in both headers. Restores item width to what it was on entry.
// Returns nonzero if either slider changed this frame.
#define UI_GLUE_BLACK_POINT_MAX      (80.0f  / 255.0f)
#define UI_GLUE_TEXT_INTENSITY_MIN   ( 0.0f  / 255.0f)
#define UI_GLUE_TEXT_INTENSITY_MAX   (255.0f / 255.0f)
int ui_glue_draw_theme(UIContext *ui);

#endif // UI_GLUE_H
