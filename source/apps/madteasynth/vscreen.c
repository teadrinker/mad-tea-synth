#include "vscreen.h"

// Provisioning plus one flat shim per primitive; drawing lives in font/render_ctx.c.
#include "font/render_ctx.h"

// Both fonts, for the font() resolver.
#include "font/font_idealist_hacker_mono_lowspec.h"
#include "font/font_assembly_line_lowspec.h"

// The framebuffer: static, so it outlives every VM, or borrowed at
// VSCREEN_FRAMEBUFFER_ADDR (microw8, where a private copy wouldn't fit). Either
// way a link-time constant, so s_ctx needs no init hook.

// Bump region for render_glyph_lowspec; UI-thread only. microw8 shrinks it in
// song_config.h.
static char s_glyph_scratch[VSCREEN_GLYPH_SCRATCH_BYTES];

// Bump arena for off-screen images.
static char s_image_arena[VSCREEN_IMAGE_ARENA_BYTES];

// Table capacities the context borrows.
static RenderImage s_images[VSCREEN_MAX_IMAGES];
static RSurface    s_target_stack[VSCREEN_TARGET_STACK_MAX];
static int         s_target_flags[VSCREEN_TARGET_STACK_MAX];

static const FONT_LOWSPEC *vscreen_font_resolve(void *user, int id);

// This context's glyph cache, reached through font_resolve_user since drawing fills it.
#if VSCREEN_FONT_CACHE_BYTES > 0
static char s_font_cache_mem[VSCREEN_FONT_CACHE_BYTES];
static VScreenFontCache s_font_cache = { s_font_cache_mem, VSCREEN_FONT_CACHE_BYTES, 0 };
#else
static VScreenFontCache s_font_cache;
#endif

// Everything a link-time constant: there is no init hook.
static RenderCtx s_ctx = {
    .screen = { VSCREEN_PIXELS, VSCREEN_W, 0, 0, VSCREEN_W, VSCREEN_H },
    .generation = 1,

    .scratch     = s_glyph_scratch,
    .scratch_end = s_glyph_scratch + VSCREEN_GLYPH_SCRATCH_BYTES,

    .image_arena     = s_image_arena,
    .image_arena_end = s_image_arena + VSCREEN_IMAGE_ARENA_BYTES,
    .images          = s_images,
    .image_cap       = VSCREEN_MAX_IMAGES,
    .image_max_w     = VSCREEN_IMAGE_MAX_W,
    .image_max_h     = VSCREEN_IMAGE_MAX_H,

    .target_stack = s_target_stack,
    .target_flags = s_target_flags,
    .target_cap   = VSCREEN_TARGET_STACK_MAX,

    .font_resolve      = vscreen_font_resolve,
    .font_resolve_user = &s_font_cache,

    // The global the exported song.c names; other hosts provision their own.
    .palette     = vscreen_palette,
    .palette_len = VSCREEN_PALETTE_LEN,
};

// Exposed so a VM can get it as user_data.
RenderCtx *vscreen_ctx(void) { return &s_ctx; }

unsigned char *vscreen_pixels(void) { return VSCREEN_PIXELS; }

#ifndef VSCREEN_FRAMEBUFFER_ADDR
// Not static: song.c indexes it as `vscreen_screen`.
unsigned char vscreen_screen_storage[VSCREEN_MAX_W * VSCREEN_MAX_H];
#endif

// The default cube, built at compile time; compiled out for a borrowed palette.
#ifndef VSCREEN_PALETTE_ADDR
#define VSP_1(i)  VSCREEN_PALETTE_DEFAULT(i)
#define VSP_4(i)  VSP_1(i), VSP_1((i)+1), VSP_1((i)+2), VSP_1((i)+3)
#define VSP_16(i) VSP_4(i), VSP_4((i)+4), VSP_4((i)+8), VSP_4((i)+12)
#define VSP_64(i) VSP_16(i), VSP_16((i)+16), VSP_16((i)+32), VSP_16((i)+48)
int vscreen_palette[256] = { VSP_64(0), VSP_64(64), VSP_64(128), VSP_64(192) };
#undef VSP_1
#undef VSP_4
#undef VSP_16
#undef VSP_64
#endif

// Housekeeping: each _ctx form does the work, the flat form forwards to s_ctx.

unsigned int vscreen_generation_ctx(RenderCtx *c) { return c ? c->generation : 0; }
unsigned int vscreen_generation(void)             { return vscreen_generation_ctx(&s_ctx); }

void vscreen_touch_ctx(RenderCtx *c) { if (c) c->generation++; }
void vscreen_touch(void)             { vscreen_touch_ctx(&s_ctx); }

unsigned char *vscreen_pixels_ctx(RenderCtx *c) { return c ? c->screen.pixels : 0; }

int *vscreen_palette_ctx(RenderCtx *c)     { return c ? c->palette : 0; }
int  vscreen_palette_len_ctx(RenderCtx *c) { return c ? c->palette_len : 0; }

// The context's clip rect is the live size.
int vscreen_width_ctx(RenderCtx *c)  { return c ? c->screen.clip_w : 0; }
int vscreen_height_ctx(RenderCtx *c) { return c ? c->screen.clip_h : 0; }
int vscreen_stride_ctx(RenderCtx *c) { return c ? c->screen.stride : 0; }

int vscreen_width(void)  { return vscreen_width_ctx(&s_ctx); }
int vscreen_height(void) { return vscreen_height_ctx(&s_ctx); }
int vscreen_stride(void) { return vscreen_stride_ctx(&s_ctx); }

// Every vscreen-provisioned framebuffer is sized to this ceiling.
int vscreen_set_size_ctx(RenderCtx *c, int w, int h)
{
    if (!c) return 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > VSCREEN_MAX_W) w = VSCREEN_MAX_W;
    if (h > VSCREEN_MAX_H) h = VSCREEN_MAX_H;
    if (w == c->screen.clip_w && h == c->screen.clip_h) return 0;

    c->screen.stride = w;
    c->screen.clip_x = 0;
    c->screen.clip_y = 0;
    c->screen.clip_w = w;
    c->screen.clip_h = h;
    // The old picture would be sheared at the new stride.
    vscreen_clear_ctx(c, 0);
    return 1;
}

int vscreen_set_size(int w, int h)
{
#ifdef VSCREEN_FRAMEBUFFER_ADDR
    // Borrowed framebuffer: the stride is the host's.
    (void)w; (void)h;
    return 0;
#else
    return vscreen_set_size_ctx(&s_ctx, w, h);
#endif
}

void vscreen_clear_ctx(RenderCtx *c, int argb8)
{
    if (!c || !c->screen.pixels) return;
    unsigned char col = (unsigned char)(argb8 & 0xFF);
    // The live size, not the macro.
    int n = c->screen.stride * c->screen.clip_h;
    for (int i = 0; i < n; i++)
        c->screen.pixels[i] = col;
    c->generation++;
}

void vscreen_clear(int argb8) { vscreen_clear_ctx(&s_ctx, argb8); }

void vscreen_expand_rgba(unsigned int *dst, int dst_stride)
{ vscreen_expand_rgba_ctx(&s_ctx, dst, dst_stride); }

void vscreen_expand_rgba_ctx(RenderCtx *c, unsigned int *dst, int dst_stride)
{
    // A table lookup, thanks to the 0xAABBGGRR layout. The framebuffer's alpha bits
    // are ignored, as on a Pebble.
    if (!c || !c->screen.pixels || !dst) return;
    // The context's palette, not the global.
    const int *pal = c->palette;
    const int  pal_len = pal ? c->palette_len : 0;

    const int sw = c->screen.clip_w, sh = c->screen.clip_h;
    const int stride = c->screen.stride;
    for (int y = 0; y < sh; y++)
    {
        const unsigned char *src = c->screen.pixels + y * stride;
        unsigned int *row = dst + y * dst_stride;
        for (int x = 0; x < sw; x++)
        {
            int idx = src[x];
            row[x] = idx < pal_len ? (unsigned int)pal[idx] : 0u;
        }
    }
}

// font() id -> FONT_LOWSPEC. Idealist Hacker Mono (0, 1) draws through the
// context's cache; Assembly Line (2) is uncached. A null cache draws uncached.
static const FONT_LOWSPEC *vscreen_cached_font(VScreenFontCache *fc,
                                               const FONT_LOWSPEC *src)
{
    if (!fc || !fc->mem) return src;
    // A block too small falls back to the plain font.
    if (!fc->font) fc->font = render_ctx_build_font_cache(src, fc->mem, fc->bytes);
    return fc->font;
}

static const FONT_LOWSPEC *vscreen_font_resolve(void *user, int id)
{
    VScreenFontCache *fc = (VScreenFontCache *)user;
    switch (id) {
        case 2:  return &font_assembly_line_lowspec_font;
        case 0:
        case 1:  return vscreen_cached_font(fc, &font_idealist_hacker_mono_lowspec_font);
        default: return 0;   // unrecognized id: leave the current font selected
    }
}

// The same recipe as s_ctx, for caller-owned storage.
void vscreen_ctx_provision(RenderCtx *c,
                           unsigned char *pixels, int w, int h,
                           int *palette, int palette_len,
                           char *scratch, int scratch_bytes,
                           char *image_arena, int image_arena_bytes,
                           RenderImage *images, int image_cap,
                           RSurface *target_stack, int *target_flags, int target_cap,
                           VScreenFontCache *font_cache,
                           char *font_cache_mem, int font_cache_bytes)
{
    if (!c) return;

    // Zero first, so unset fields are null.
    render_fill_bytes((unsigned char *)c, 0, (int)sizeof(*c));

    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > VSCREEN_MAX_W) w = VSCREEN_MAX_W;
    if (h > VSCREEN_MAX_H) h = VSCREEN_MAX_H;

    c->screen.pixels = pixels;
    c->screen.stride = w;
    c->screen.clip_x = 0;
    c->screen.clip_y = 0;
    c->screen.clip_w = w;
    c->screen.clip_h = h;
    c->generation    = 1;

    c->scratch     = scratch;
    c->scratch_end = scratch ? scratch + scratch_bytes : 0;

    c->image_arena     = image_arena;
    c->image_arena_end = image_arena ? image_arena + image_arena_bytes : 0;
    c->images          = images;
    c->image_cap       = images ? image_cap : 0;
    c->image_max_w     = VSCREEN_IMAGE_MAX_W;
    c->image_max_h     = VSCREEN_IMAGE_MAX_H;

    c->target_stack = target_stack;
    c->target_flags = target_flags;
    c->target_cap   = (target_stack && target_flags) ? target_cap : 0;

    c->font_resolve      = vscreen_font_resolve;
    c->font_resolve_user = (void *)font_cache;
    if (font_cache) {
        font_cache->mem   = font_cache_bytes > 0 ? font_cache_mem : 0;
        font_cache->bytes = font_cache_bytes;
        font_cache->font  = 0;
    }

    c->palette     = palette;
    c->palette_len = palette ? palette_len : 0;
    if (palette)
        for (int i = 0; i < palette_len; i++)
            palette[i] = VSCREEN_PALETTE_DEFAULT(i);

    // blend_cache 0 selects render_lowspec's shared one: pure, so safe across
    // contexts but not threads; visual bodies all run on the UI thread.
}

// The flat vscreen_* API song.c calls, bound to s_ctx. Fix behaviour in
// font/render_ctx.c, not here.

int vscreen_putpixel_i32(int x, int y, int c)
{ return render_ctx_putpixel_i32(&s_ctx, x, y, c); }

int vscreen_point_fx16(int x, int y, int amount, int blend)
{ return render_ctx_point_fx16(&s_ctx, x, y, amount, blend); }

int vscreen_line_fx16(int x1, int y1, int x2, int y2, int stroke_width, int alpha, int blend)
{ return render_ctx_line_fx16(&s_ctx, x1, y1, x2, y2, stroke_width, alpha, blend); }

int vscreen_ellipse_fx16(int x, int y, int radius_w, int radius_h, int alpha, int blend)
{ return render_ctx_ellipse_fx16(&s_ctx, x, y, radius_w, radius_h, alpha, blend); }

int vscreen_circle_fx16(int x, int y, int radius, int alpha, int blend)
{ return render_ctx_circle_fx16(&s_ctx, x, y, radius, alpha, blend); }

int vscreen_rect_fx16(int x, int y, int w, int h, int col, int alpha, int blend)
{ return render_ctx_rect_fx16(&s_ctx, x, y, w, h, col, alpha, blend); }

int vscreen_background_fx16(int col, int transparency)
{ return render_ctx_background_fx16(&s_ctx, col, transparency); }

int vscreen_glyph_fx16(int x, int y, int size, int stroke_width, int ascii, int alpha, int blend)
{ return render_ctx_glyph_fx16(&s_ctx, x, y, size, stroke_width, ascii, alpha, blend); }

void vscreen_text(int x, int y, int size, int stroke_width,
                  const unsigned char *str, int len, int alpha, int blend,
                  int letter_spacing, int line_height)
{ render_ctx_text(&s_ctx, x, y, size, stroke_width, str, len, alpha, blend,
                  letter_spacing, line_height); }

int vscreen_font_i32(int id)       { return render_ctx_font_i32(&s_ctx, id); }
int vscreen_text_align_i32(int id) { return render_ctx_text_align_i32(&s_ctx, id); }

int vscreen_image_alloc_i32(int w, int h)
{ return render_ctx_image_alloc_i32(&s_ctx, w, h); }

int vscreen_image_getpixel_i32(int image_id, int x, int y)
{ return render_ctx_image_getpixel_i32(&s_ctx, image_id, x, y); }

int vscreen_image_sample_fx16(int image_id, int x, int y)
{ return render_ctx_image_sample_fx16(&s_ctx, image_id, x, y); }

int vscreen_push_target_i32(int image_id)
{ return render_ctx_push_target_i32(&s_ctx, image_id); }

int vscreen_pop_target_i32(int unused)
{ return render_ctx_pop_target_i32(&s_ctx, unused); }

void vscreen_images_reset(void) { render_ctx_images_reset(&s_ctx); }
