#ifndef TEXTMODE_H
#define TEXTMODE_H

#include "common/tsys.h"
#include "font/font_cache.h"
#include "textmode_cell.h"




typedef struct {
    TMCell cell;
    int col;
    int row;
} TMSprite;

// ---- Additive AA line overlay ----
// Draws antialiased lines on top of everything else (cells, sprites, glyphs)
// during tm_render. Enabled by default; define TM_DISABLE_LINES (as a
// precompile flag, before this header is included) to compile it out
// entirely, e.g. for size-constrained builds that don't need it.
#ifndef TM_DISABLE_LINES

typedef struct {
    int x; // 16.16 fixed point
    int y; // 16.16 fixed point
    unsigned char opacity;    // 0..255
    unsigned char is_move_to; // 1: start a new sub-path here without drawing
    unsigned char color; 
    unsigned char _unused; 
} TMLinePoint;

#endif // TM_DISABLE_LINES

// ---- Indexed 8-bit rendering ----

typedef struct {
    unsigned char num_colors;
    unsigned char first_palette_id;
} IndexedColorRamp;

typedef struct {
    int w;
    int h;
    int stride;
    unsigned char *pixels;
    int cap;                     // bytes allocated for `pixels` (>= w*h), 0 if unowned
    unsigned int palette[256];   // ARGB
    IndexedColorRamp ramps[128]; // ramps assume darker-to-brighter order
} Indexed8Bit;

// Look up palette index for (intensity 0..255, gradient).
static inline unsigned char get_indexed_color(Indexed8Bit *i8, unsigned char c, unsigned char gradient) {
    IndexedColorRamp *g = &i8->ramps[gradient];
    return (unsigned char)(g->first_palette_id + (((int)c * (int)g->num_colors) >> 8));
}

static inline unsigned char get_indexed_color_fb(Indexed8Bit *i8, unsigned int color) {
    IndexedColorRamp *g = &i8->ramps[TM_COLOR_GRADIENT(color)];
    return (unsigned char)(g->first_palette_id + (((int)TM_COLOR_FG(color) * (int)g->num_colors) >> 8));
}

static inline unsigned char get_indexed_color_bg(Indexed8Bit *i8, unsigned int color) {
    IndexedColorRamp *g = &i8->ramps[TM_COLOR_GRADIENT(color)];
    return (unsigned char)(g->first_palette_id + (((int)TM_COLOR_BG(color) * (int)g->num_colors) >> 8));
}

typedef struct {
    TMCell       *cells;
    int           cols;
    int           rows;
    TMSprite     *sprites;
    int           num_sprites;
    Tsys         *sys;
    FontCache    *fc;
    int           glyph_grid_w;
    int           glyph_grid_h;
    Indexed8Bit   tmp_buffer;
#ifndef TM_DISABLE_LINES
    TMLinePoint  *line_points;
    int           num_line_points;
    int           line_points_cap;
#endif
} TextMode;

// ---- Lifecycle ----
TextMode *tm_create(Tsys *sys, FontCache *fc, int cols, int rows, int num_sprites);
void      tm_destroy(TextMode *tm);

void tm_resize(TextMode *tm, int new_cols, int new_rows, unsigned int color);
void tm_clear(TextMode *tm, unsigned int color);

int tm_get(TextMode *tm, int col, int row, TMCell *out);
#define tm_put(tm, col, row, ch, color) tm_put_full(tm, col, row, ch, color, 1.0f, 1.0f, 0.0f, 0.0f)

void tm_put_full(TextMode *tm, int col, int row,
    unsigned char ch, unsigned int color,
    float scale, float weight, float offset_x, float offset_y);

// Write a string starting at (col, row), wrapping at the right edge.
// Returns the column immediately after the last written character.
int tm_puts(TextMode *tm, int col, int row,
           const char *str, unsigned int color);

// ---- Additive AA line overlay ----
// Immediate-mode path API: tm_move_to starts a new sub-path at (x, y)
// without drawing; tm_line_to draws an antialiased segment from the current
// pen position to (x, y). Coordinates are in pixel space (floats at the
// API boundary only -- everything is rasterized in 16.16 fixed point
// internally). alpha is 0..1. The path is cleared each tm_render call, so
// callers re-issue it every frame (like the rest of TextMode's immediate
// drawing model).
#ifndef TM_DISABLE_LINES
void tm_move_to(TextMode *tm, float x, float y, float alpha, int color);
void tm_line_to(TextMode *tm, float x, float y, float alpha, int color);
void tm_lines_clear(TextMode *tm);
#endif // TM_DISABLE_LINES

// ---- Indexed8Bit helpers ----
void i8_set_default_colors(Indexed8Bit *i8);
void i8_to_argb(Indexed8Bit *i8,
                int pixel_w, int pixel_h, int pixel_stride,
                unsigned int *dstRgba,
                float opacity_min, float opacity_max,
                int flags);

void tm_render(const TextMode *tm,
               int pixel_w, int pixel_h, int pixel_stride, unsigned int *dstRgba,
               float cell_width_mul, float line_height, unsigned int default_color,
               int flags);

#endif // TEXTMODE_H
