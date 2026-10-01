#ifndef FONT_CACHE_H
#define FONT_CACHE_H

#include "glyph_render.h"

#define MAX_CACHE_ENTRIES 4096
#define CACHE_HASH_SIZE   4096

// Entries are keyed on `descend` and `optical_size` as well as glyph/scale/
// stroke, so a control that sweeps one of those (ui_demo's LineH slider drives
// `descend`) mints a fresh set of ~100 glyph bitmaps per quantisation step.
// MAX_CACHE_ENTRIES alone bounds the *count*, not the bytes: at a large font
// size 4096 retained bitmaps run to tens of megabytes, which on a fixed arena
// is the difference between fitting and not. Cap the bytes as well and let the
// LRU trim back to it.
#ifndef FONT_CACHE_BYTE_BUDGET
#define FONT_CACHE_BYTE_BUDGET (4 * 1024 * 1024)
#endif

typedef struct {
    const unsigned char *bitmap;
    int bw, bh;
    float ox, oy;
} FontCacheGlyphInfo;

typedef struct {
    int glyph_idx;
    unsigned short scale_q;
    unsigned short stroke_width_q;
    unsigned short descend_q;
    unsigned short optical_size_q;
    unsigned char *bitmap;
    int bw, bh;
    float ox, oy;
    int valid;
    int lru_clock;
    int next;
} CacheEntry;

typedef struct FontCache {
    Tsys *sys;
    FONT_SETTINGS *settings;
    CacheEntry cache[MAX_CACHE_ENTRIES];
    int hash_heads[CACHE_HASH_SIZE];
    int lru_tick;
    size_t bytes_used;   // sum of bw*bh over entries holding a bitmap
    FontCacheGlyphInfo info;
} FontCache;

enum Align {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT
};

FontCache *font_cache_create(Tsys *sys, FONT_SETTINGS *settings);
void font_cache_destroy(FontCache *fc);
void font_cache_clear(FontCache *fc);

void font_cache_draw_text(FontCache *fc, const char *string, float x, float y, enum Align align,
               float font_size, float font_width, unsigned int color,
               int dstW, int dstH, unsigned int *dstRgba);

float font_cache_measure(FontCache *fc, const char *string, float font_size, float font_width);

const FontCacheGlyphInfo *font_cache_get(FontCache *fc, int idx, float scale, float stroke_width);

#endif
