#include "font_cache.h"
#include "gen_font.h"

#include "common/math_pure.h"
#include "glyph_render.h"

#define Q16_SCALE 256.0f

static unsigned short float_to_q16(float v) {
    if (v < 0.0f) v = 0.0f;
    int iv = (int)(v * Q16_SCALE + 0.5f);
    if (iv > 65535) iv = 65535;
    return (unsigned short)iv;
}

static unsigned int make_hash(int idx, unsigned short sq, unsigned short swq,
                               unsigned short dq, unsigned short oq) {
    unsigned int h = (unsigned int)idx + ((unsigned int)sq << 16);
    h ^= (unsigned int)swq | ((unsigned int)dq << 16);
    h ^= (unsigned int)oq * 0x9e3779b9u;
    h = ((h >> 16) ^ h) * 0x85ebca6bu;
    h = ((h >> 13) ^ h) * 0xc2b2ae35u;
    h = (h >> 16) ^ h;
    return h;
}

FontCache *font_cache_create(Tsys *sys, FONT_SETTINGS *settings) {
    if (!sys || !settings) return NULL;
    FontCache *fc = (FontCache *)sys->malloc(sizeof(FontCache));
    if (!fc) return NULL;
    fc->sys = sys;
    fc->settings = settings;
    fc->lru_tick = 0;
    fc->bytes_used = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        fc->cache[i].bitmap = NULL;
        fc->cache[i].valid = 0;
    }
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        fc->hash_heads[i] = -1;
    }
    return fc;
}

void font_cache_destroy(FontCache *fc) {
    if (!fc) return;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (fc->cache[i].bitmap)
            fc->sys->free(fc->cache[i].bitmap);
    }
    fc->bytes_used = 0;
    fc->sys->free(fc);
}

void font_cache_clear(FontCache *fc) {
    if (!fc) return;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (fc->cache[i].bitmap) {
            fc->sys->free(fc->cache[i].bitmap);
            fc->cache[i].bitmap = NULL;
        }
        fc->cache[i].valid = 0;
    }
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        fc->hash_heads[i] = -1;
    }
    fc->bytes_used = 0;
}

static CacheEntry *find_in_cache(FontCache *fc, int idx, float scale, float stroke_width) {
    unsigned short sq  = float_to_q16(scale);
    unsigned short swq = float_to_q16(stroke_width);
    unsigned short dq  = float_to_q16(fc->settings->descend);
    unsigned short oq  = float_to_q16(fc->settings->optical_size);
    unsigned int h = make_hash(idx, sq, swq, dq, oq);
    int bucket = h & (CACHE_HASH_SIZE - 1);

    int curr = fc->hash_heads[bucket];
    while (curr >= 0) {
        CacheEntry *e = &fc->cache[curr];
        if (e->valid &&
            e->glyph_idx == idx &&
            e->scale_q == sq &&
            e->stroke_width_q == swq &&
            e->descend_q == dq &&
            e->optical_size_q == oq)
            return e;
        curr = e->next;
    }
    return NULL;
}

static void remove_from_chain(FontCache *fc, int index) {
    CacheEntry *e = &fc->cache[index];
    unsigned int h = make_hash(e->glyph_idx, e->scale_q, e->stroke_width_q,
                                e->descend_q, e->optical_size_q);
    int bucket = h & (CACHE_HASH_SIZE - 1);
    int prev = -1;
    int curr = fc->hash_heads[bucket];
    while (curr >= 0 && curr != index) {
        prev = curr;
        curr = fc->cache[curr].next;
    }
    if (curr == index) {
        if (prev < 0)
            fc->hash_heads[bucket] = e->next;
        else
            fc->cache[prev].next = e->next;
    }
}

// Unhook slot `index` from its hash chain and drop its bitmap. Leaves the slot
// invalid and reusable.
static void evict_slot(FontCache *fc, int index) {
    remove_from_chain(fc, index);
    CacheEntry *e = &fc->cache[index];
    if (e->bitmap) {
        fc->bytes_used -= (size_t)e->bw * (size_t)e->bh;
        fc->sys->free(e->bitmap);
        e->bitmap = NULL;
    }
    e->valid = 0;
}

// Index of the least recently used valid entry, or -1 if there is none.
// `except` is never returned (the caller's freshly rendered entry).
static int lru_index(FontCache *fc, int except) {
    int oldest = -1;
    int oldest_tick = 0x7fffffff;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (i == except) continue;
        if (fc->cache[i].valid && fc->cache[i].lru_clock < oldest_tick) {
            oldest_tick = fc->cache[i].lru_clock;
            oldest = i;
        }
    }
    return oldest;
}

// Drop least-recently-used entries until the cache is back inside its byte
// budget. Without this the cache is bounded in entries only, and a sweeping
// `descend` / `optical_size` fills all MAX_CACHE_ENTRIES slots with bitmaps
// nothing will ask for again.
static void trim_to_budget(FontCache *fc, int keep) {
    while (fc->bytes_used > (size_t)FONT_CACHE_BYTE_BUDGET) {
        int victim = lru_index(fc, keep);
        if (victim < 0) break;
        evict_slot(fc, victim);
    }
}

static CacheEntry *alloc_slot(FontCache *fc) {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!fc->cache[i].valid) return &fc->cache[i];
    }
    int oldest = lru_index(fc, -1);
    if (oldest < 0) return NULL;
    evict_slot(fc, oldest);
    return &fc->cache[oldest];
}

static float triangle_wave(float x) {
    return 1.0f - 2.0f * m_fabsf(0.5f - 0.5f * x + m_floorf(0.5f * x)); 
}

static void render_to_cache(FontCache *fc, CacheEntry *e) {
    const GLYPH_DATA *g = &fc->settings->glyphs[e->glyph_idx];
    float scale = e->scale_q / Q16_SCALE;
    float stroke_width = e->stroke_width_q / Q16_SCALE;

    if (!g->count) { e->valid = 1; return; }

    int pad = (int)(stroke_width * 0.5f) + 2;
    if (pad < 2) pad = 2;
    e->ox = 0.f;
    e->oy = (float)pad;
    e->bw = (int)(fc->settings->glyph_grid_size[0] * scale);
    e->bh = (int)(fc->settings->glyph_grid_size[1] * scale) + pad * 2 + (int)(fc->settings->descend * scale * 6);
    if (e->bw < 1) e->bw = 1;
    if (e->bh < 1) e->bh = 1;

    if (e->bw > 4096) e->bw = 4096;
    if (e->bh > 4096) e->bh = 4096;

    e->bitmap = (unsigned char *)fc->sys->malloc(e->bw * e->bh);
    if (!e->bitmap) return;
    fc->bytes_used += (size_t)e->bw * (size_t)e->bh;
    fc->sys->memset(e->bitmap, 0, e->bw * e->bh);

    int flags = e->glyph_idx >= 127 - 32 ? RG_FLAGS_IGNORE_GRADE_FIT : 0; 

    if((scale == 1.f || scale == 0.5f) && !(flags & RG_FLAGS_IGNORE_GRADE_FIT)) {

        // pixel perfect alignment

        // ugly hack as special case for scale == 1.f and scale == 0.5f - turn off grade fit!
        flags |= RG_FLAGS_IGNORE_GRADE_FIT;

        // without grade fit, we can do this for pixel alignment
        float tri = 0.5f * triangle_wave(stroke_width + (scale < 0.75f ? 1.f : 0.f));
        e->ox += tri;
        e->oy += tri;
    }

    render_glyph(fc->sys, g, e->bw, e->bh, e->bitmap, fc->settings,
                 stroke_width, scale, scale,
                 e->ox, e->oy, flags);
    e->valid = 1;
}

static CacheEntry *get_or_render(FontCache *fc, int idx, float scale, float stroke_width) {
    if (idx < 0 || idx >= fc->settings->glyph_count) return NULL;
    CacheEntry *e = find_in_cache(fc, idx, scale, stroke_width);
    if (e) {
        e->lru_clock = ++fc->lru_tick;
        return e;
    }
    e = alloc_slot(fc);
    if (!e) return NULL;

    unsigned short sq  = float_to_q16(scale);
    unsigned short swq = float_to_q16(stroke_width);
    unsigned short dq  = float_to_q16(fc->settings->descend);
    unsigned short oq  = float_to_q16(fc->settings->optical_size);

    e->glyph_idx      = idx;
    e->scale_q        = sq;
    e->stroke_width_q = swq;
    e->descend_q      = dq;
    e->optical_size_q = oq;
    e->bitmap         = NULL;
    e->bw             = 0;
    e->bh             = 0;
    e->ox             = 0;
    e->oy             = 0;
    e->valid          = 0;
    e->lru_clock      = ++fc->lru_tick;

    render_to_cache(fc, e);

    if (e->valid) {
        unsigned int h = make_hash(idx, sq, swq, dq, oq);
        int bucket = h & (CACHE_HASH_SIZE - 1);
        e->next = fc->hash_heads[bucket];
        fc->hash_heads[bucket] = (int)(e - fc->cache);
        trim_to_budget(fc, (int)(e - fc->cache));
    }
    return e;
}

const FontCacheGlyphInfo *font_cache_get(FontCache *fc, int idx, float scale, float stroke_width) {
    if (!fc) return NULL;
    CacheEntry *e = get_or_render(fc, idx, scale, stroke_width);
    if (!e || !e->valid) return NULL;

    if (!e->bitmap) {
        fc->info.bitmap = NULL;
        fc->info.bw = 0;
        fc->info.bh = 0;
        fc->info.ox = 0;
        fc->info.oy = 0;
        return &fc->info;
    }

    fc->info.bitmap = e->bitmap;
    fc->info.bw     = e->bw;
    fc->info.bh     = e->bh;
    fc->info.ox     = e->ox;
    fc->info.oy     = e->oy;
    return &fc->info;
}

float font_cache_measure(FontCache *fc, const char *string, float font_size, float font_width) {
    (void)font_width;
    if (!fc || !string) return 0.0f;
    int len = 0;
    while (string[len]) len++;
    return (float)len * (float)fc->settings->glyph_grid_size[0] * font_size;
}

void font_cache_draw_text(FontCache *fc, const char *string, float x, float y, enum Align align,
               float font_size, float font_width, unsigned int color,
               int dstW, int dstH, unsigned int *dstRgba) {
    if (!fc || !string || !*string || !dstRgba || font_size <= 0.0f || dstW <= 0 || dstH <= 0)
        return;

    unsigned int ca = (color >> 24) & 0xFF;
    unsigned int cr = (color >> 16) & 0xFF;
    unsigned int cg = (color >>  8) & 0xFF;
    unsigned int cb =  color        & 0xFF;
    if (ca == 0) ca = 255;

    int len = 0;
    while (string[len]) len++;
    float advance = (float)fc->settings->glyph_grid_size[0] * font_size;
    float total_w = (float)len * advance;

    float start_x = x;
    if (align == ALIGN_CENTER) start_x = x - total_w * 0.5f;
    else if (align == ALIGN_RIGHT) start_x = x - total_w;

    float stroke_width = font_width * font_size;
    int glyph_count = fc->settings->glyph_count;

    float cx = start_x;
    for (int i = 0; string[i]; i++) {
        int idx = (unsigned char)string[i] - 32;
        if (idx >= 0 && idx < glyph_count) {
            CacheEntry *e = get_or_render(fc, idx, font_size, stroke_width);
            if (e && e->valid && e->bitmap) {
                int bx0 = (int)(cx)       - (int)(e->ox);
                int by0 = (int)(y + 0.5f)  - (int)(e->oy);

                for (int by = 0; by < e->bh; by++) {
                    int dst_y = by0 + by;
                    if (dst_y < 0 || dst_y >= dstH) continue;
                    for (int bx = 0; bx < e->bw; bx++) {
                        unsigned char a = e->bitmap[by * e->bw + bx];
                        if (a == 0) continue;
                        int dst_x = bx0 + bx;
                        if (dst_x < 0 || dst_x >= dstW) continue;

                        unsigned int *dst = &dstRgba[dst_y * dstW + dst_x];
                        unsigned int dr = (*dst >> 16) & 0xFF;
                        unsigned int dg = (*dst >>  8) & 0xFF;
                        unsigned int db =  *dst        & 0xFF;

                        unsigned int alpha = (a * ca + 127) / 255;
                        unsigned int inv  = 255 - alpha;
                        unsigned int rr = (cr * alpha + dr * inv + 127) / 255;
                        unsigned int rg = (cg * alpha + dg * inv + 127) / 255;
                        unsigned int rb = (cb * alpha + db * inv + 127) / 255;

                        *dst = 0xFF000000 | (rr << 16) | (rg << 8) | rb;
                    }
                }
            }
        }
        cx += advance;
    }
}
