#include "font/render_ctx.h"

// scratch_bump -- the image arena carves images the same way the renderer
// carves its temporaries. Already reachable through render_lowspec.h
// (render_polygon.h pulls it in), but included explicitly since this file uses
// it directly.
#include "common/scratch_alloc.h"

// The stateful layer above render_lowspec.h -- see render_ctx.h for the model
// (layering, borrowed provisioning, the script-facing API spec). Every body
// here is a target resolve, an argument shift and a forward to render_*; all
// rendering behaviour lives in render_lowspec.c, not here.

// ---- draw target ---------------------------------------------------------
//
// Every primitive draws into render_ctx_target() rather than straight at the
// screen, so push_target()/pop_target() can redirect the whole set without
// each primitive knowing about it. Depth 0 is the screen and is the only depth
// that bumps the generation counter -- drawing into an image changes nothing a
// host would re-upload.

// The blend flags every draw into the current target must carry (see
// common/render_surface.h). The screen is whatever encoding render_lowspec's
// TU was compiled for -- 0 -- while images are full 8-bit grayscale, so a
// pushed image contributes BLEND_8BIT_GRAYSCALE and the rasterizer keeps all
// 256 levels instead of quantizing to the 4 a GColor8 channel holds. That is
// also what makes image_getpixel's return a 0-255 gray.
#define RENDER_CTX_SCREEN_BLEND_FLAGS 0
#define RENDER_CTX_IMAGE_BLEND_FLAGS  BLEND_8BIT_GRAYSCALE

static int render_ctx_target_flags(RenderCtx *c)
{
    return c->target_depth > 0 ? c->target_flags[c->target_depth - 1]
                               : RENDER_CTX_SCREEN_BLEND_FLAGS;
}

// Folds the script's blend mode together with the current target's encoding
// flags into the single int the rasterizers take.
//
// The mask is load-bearing: a script's NEGATIVE blend (the triangle-fill
// backend) is sign-extended, so every high bit is already 1 and `blend | flags`
// would silently be a no-op. Masking to the mode field first, then adding the
// flags, is what lets the two coexist -- rs_blend_mode sign-extends the mode
// back out on the other side.
#define RENDER_CTX_BLEND(c, script_blend) \
    (((script_blend) & BLEND_MODE_MASK) | render_ctx_target_flags(c))

// The surface to draw into, or 0 when the current target swallows draws --
// nothing pushed and no screen bound (a stray call outside a render pass), or
// a failed push_target.
static RSurface *render_ctx_target(RenderCtx *c)
{
    if (c->target_depth > 0) {
        RSurface *top = &c->target_stack[c->target_depth - 1];
        return top->pixels ? top : 0;
    }
    return c->screen.pixels ? &c->screen : 0;
}

// Post-draw: only a draw that reached the screen is worth a repaint.
static void render_ctx_touched(RenderCtx *c)
{
    if (c->target_depth <= 0) c->generation++;
}

void render_ctx_bind_screen(RenderCtx *c, const RSurface *fb)
{
    if (fb) c->screen = *fb;
    else    render_fill_bytes((unsigned char *)&c->screen, 0, (int)sizeof(c->screen));
}

// ---- drawing primitives --------------------------------------------------
// Thin wrappers over render_lowspec.h's render_* (all behaviour lives THERE,
// see that header's ARCHITECTURE note). The fx16(int) args (col / ascii /
// blend) arrive fx16-coerced from the VM and are shifted back down here, while
// geometry and coverage (amount / alpha) stay raw fx16.

int render_ctx_putpixel_i32(RenderCtx *c, int x, int y, int col)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    if ((unsigned)x < (unsigned)s->clip_w && (unsigned)y < (unsigned)s->clip_h) {
        s->pixels[y * s->stride + x] = (unsigned char)(col & 0xFF);
        render_ctx_touched(c);
    }
    return 0;
}

int render_ctx_point_fx16(RenderCtx *c, int x, int y, int amount, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    render_point(s, x, y, amount, RENDER_CTX_BLEND(c, blend >> FX16_SHIFT));
    render_ctx_touched(c);
    return 0;
}

int render_ctx_line_fx16(RenderCtx *c, int x1, int y1, int x2, int y2,
                         int stroke_width, int alpha, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    render_line(s, x1, y1, x2, y2, stroke_width, alpha,
                RENDER_CTX_BLEND(c, blend >> FX16_SHIFT), c->scratch, c->scratch_end,
                c->blend_cache);
    render_ctx_touched(c);
    return 0;
}

int render_ctx_ellipse_fx16(RenderCtx *c, int x, int y, int radius_w, int radius_h,
                            int alpha, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    render_ellipse(s, x, y, radius_w, radius_h, alpha,
                   RENDER_CTX_BLEND(c, blend >> FX16_SHIFT), c->scratch, c->scratch_end,
                   c->blend_cache);
    render_ctx_touched(c);
    return 0;
}

int render_ctx_circle_fx16(RenderCtx *c, int x, int y, int radius, int alpha, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    render_circle(s, x, y, radius, alpha,
                  RENDER_CTX_BLEND(c, blend >> FX16_SHIFT), c->scratch, c->scratch_end,
                  c->blend_cache);
    render_ctx_touched(c);
    return 0;
}

int render_ctx_rect_fx16(RenderCtx *c, int x, int y, int w, int h, int col,
                         int alpha, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    render_rect(s, x, y, w, h, col >> FX16_SHIFT, alpha,
                RENDER_CTX_BLEND(c, blend >> FX16_SHIFT), c->blend_cache);
    render_ctx_touched(c);
    return 0;
}

int render_ctx_background_fx16(RenderCtx *c, int col, int transparency)
{
    // Fully transparent: nothing to paint. render_rect would skip an alpha of 0
    // by itself now, but bailing here also spares the target lookup and the
    // generation bump that would repaint the screen for no visible change.
    if (transparency >= FX16_ONE) return 0;
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    // Whole *target*, not whole screen: inside a push_target this is the "clear
    // the image" call, so it has to follow the target's extent.
    // transparency (0 = opaque, FX16_ONE = fully transparent) -> render_rect's
    // coverage alpha (its inverse). The blend itself lives in render_rect.
    // Blend mode 0 (screen) plus the target's encoding flags -- inside a
    // push_target `col` is a 0-255 gray, on the screen a GColor8.
    render_rect(s, 0, 0, fx16_from_int(s->clip_w), fx16_from_int(s->clip_h),
                col >> FX16_SHIFT, FX16_ONE - transparency, RENDER_CTX_BLEND(c, 0),
                c->blend_cache);
    render_ctx_touched(c);
    return 0;
}

int render_ctx_font_i32(RenderCtx *c, int id)
{
    if (!c->font_resolve) return 0;
    const FONT_LOWSPEC *f = c->font_resolve(c->font_resolve_user, id);
    if (f) c->font = f;   // an id the caller doesn't know leaves the font alone
    return 0;
}

// The selected font, resolving the caller's id 0 on first use (see the
// font_resolve note in render_ctx.h).
static const FONT_LOWSPEC *render_ctx_font(RenderCtx *c)
{
    if (!c->font && c->font_resolve)
        c->font = c->font_resolve(c->font_resolve_user, 0);
    return c->font;
}

int render_ctx_text_align_i32(RenderCtx *c, int id)
{
    c->text_align = id;
    return 0;
}

int render_ctx_glyph_fx16(RenderCtx *c, int x, int y, int size, int stroke_width,
                          int ascii, int alpha, int blend)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return 0;
    const FONT_LOWSPEC *f = render_ctx_font(c);
    if (!f) return 0;
    // flags 0 = fill (rasterized tile); a negative stroke_width from the song
    // selects the outline path inside render_glyph_lowspec_ascii.
    render_glyph_lowspec_ascii(f, s, stroke_width, size, x, y, ascii >> FX16_SHIFT, 0,
                               c->scratch, c->scratch_end,
                               alpha, RENDER_CTX_BLEND(c, blend >> FX16_SHIFT));
    render_ctx_touched(c);
    return 0;
}

void render_ctx_text(RenderCtx *c, int x, int y, int size, int stroke_width,
                     const unsigned char *str, int len, int alpha, int blend,
                     int letter_spacing, int line_height)
{
    RSurface *s = render_ctx_target(c);
    if (!s) return;
    const FONT_LOWSPEC *f = render_ctx_font(c);
    if (!f) return;
    render_text_lowspec_ascii(f, s, stroke_width, size, x, y, str, len, 0,
                              c->scratch, c->scratch_end,
                              alpha, RENDER_CTX_BLEND(c, blend >> FX16_SHIFT),
                              letter_spacing, c->text_align, line_height);
    render_ctx_touched(c);
}

// ===================================================================
// Off-screen images -- image_alloc / image_getpixel / push_target /
// pop_target. The model (frame-temporary arena, no free, push/pop always
// balanced) is documented in render_ctx.h; this is just the bookkeeping.
// ===================================================================

void render_ctx_images_reset(RenderCtx *c)
{
    c->image_cursor    = c->image_arena;
    c->image_count     = 0;
    // Also drops a target the previous body pushed and never popped -- without
    // this, one unbalanced body would send every later body's drawing into an
    // image whose memory has just been handed back.
    c->target_depth    = 0;
    c->target_overflow = 0;
}

int render_ctx_image_alloc_i32(RenderCtx *c, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    if (w > c->image_max_w || h > c->image_max_h) return 0;
    if (c->image_count >= c->image_cap) return 0;
    if (!c->image_arena) return 0;   // no arena provisioned
    // First call before any host reset (a live-edit preview can draw before
    // the first tick), so start the arena here rather than assuming reset went
    // first.
    if (!c->image_cursor) c->image_cursor = c->image_arena;

    unsigned char *px = (unsigned char*)scratch_bump(
        &c->image_cursor, c->image_arena_end, (size_t)w * (size_t)h);
    if (!px) return 0;   // arena full
    render_fill_bytes(px, 0, w * h);

    RenderImage *img = &c->images[c->image_count++];
    img->pixels = px;
    img->w = w;
    img->h = h;
    return c->image_count;   // ids are 1-based so 0 can mean "didn't fit"
}

// Resolves an id from image_alloc, or 0 if it names no live image.
static RenderImage *render_ctx_image(RenderCtx *c, int image_id)
{
    if (image_id < 1 || image_id > c->image_count) return 0;
    return &c->images[image_id - 1];
}

// One tap out of an already-resolved image, 0 outside it. Split out so the
// bounds rule is written once and image_sample's four taps can share a single
// id lookup instead of re-resolving per corner.
static int render_ctx_image_tap(RenderImage *img, int x, int y)
{
    if ((unsigned)x >= (unsigned)img->w || (unsigned)y >= (unsigned)img->h) return 0;
    return img->pixels[y * img->w + x];
}

int render_ctx_image_getpixel_i32(RenderCtx *c, int image_id, int x, int y)
{
    RenderImage *img = render_ctx_image(c, image_id);
    if (!img) return 0;
    return render_ctx_image_tap(img, x, y);
}

int render_ctx_image_sample_fx16(RenderCtx *c, int image_id, int x, int y)
{
    RenderImage *img = render_ctx_image(c, image_id >> FX16_SHIFT);
    if (!img) return 0;

    // Arithmetic shift floors and the mask keeps the matching fraction, for
    // negative coordinates too: x = -0.5 gives x0 = -1, fx = 0.5.
    int x0 = x >> FX16_SHIFT, fx = x & (FX16_ONE - 1);
    int y0 = y >> FX16_SHIFT, fy = y & (FX16_ONE - 1);

    // Landed exactly on a pixel centre: no neighbours needed, and this is the
    // common case for a body that samples on integer coordinates anyway.
    if ((fx | fy) == 0)
        return render_ctx_image_tap(img, x0, y0) << FX16_SHIFT;

    // Off-image taps read 0 exactly like image_getpixel, so a sample straddling
    // the border fades to black rather than smearing the edge pixel outward.
    int p00 = render_ctx_image_tap(img, x0,     y0);
    int p10 = render_ctx_image_tap(img, x0 + 1, y0);
    int p01 = render_ctx_image_tap(img, x0,     y0 + 1);
    int p11 = render_ctx_image_tap(img, x0 + 1, y0 + 1);

    // Horizontal lerps, in 32 bits and exact: a 0-255 level shifted up is 24
    // bits and (p1 - p0) * fx adds no more, so neither can overflow an int.
    int top = (p00 << FX16_SHIFT) + (p10 - p00) * fx;
    int bot = (p01 << FX16_SHIFT) + (p11 - p01) * fx;
    // The vertical one can't: (bot - top) is already Q16.16, so the product
    // with fy needs the 64-bit intermediate fx16_mul carries. Halving both
    // factors to 8 fractional bits would keep it in 32 bits but drop up to a
    // whole gray level on a steep pair -- the precision this call exists for.
    return top + (int)fx16_mul(bot - top, fy);
}

int render_ctx_push_target_i32(RenderCtx *c, int image_id)
{
    // Too deep to nest even a sentinel: remember the push so the matching pop
    // cancels against it rather than unwinding the enclosing target.
    if (c->target_depth >= c->target_cap) { c->target_overflow++; return 0; }
    RenderImage *img = render_ctx_image(c, image_id);
    // An unknown id still nests -- as the failed-push sentinel (NULL pixels),
    // so the body's matching pop_target() lands where it expects and the draws
    // in between are dropped rather than landing on the screen.
    if (!img) {
        c->target_flags[c->target_depth] = RENDER_CTX_SCREEN_BLEND_FLAGS;   // swallows draws anyway
        render_fill_bytes((unsigned char *)&c->target_stack[c->target_depth++], 0, (int)sizeof(RSurface));
        return 0;
    }
    // Images are 8-bit grayscale, so every draw into this target carries
    // BLEND_8BIT_GRAYSCALE from here until the matching pop.
    c->target_flags[c->target_depth] = RENDER_CTX_IMAGE_BLEND_FLAGS;
    c->target_stack[c->target_depth++] = rs_tile(img->pixels, img->w, img->h);
    return 1;
}

int render_ctx_pop_target_i32(RenderCtx *c, int unused)
{
    (void)unused;   // see render_ctx.h: pop_target() is a 1-arg native with the arg defaulted
    if (c->target_overflow > 0) c->target_overflow--;
    else if (c->target_depth > 0) c->target_depth--;
    return 0;
}

// ===================================================================
// Glyph raster cache -- the shared half of a caller's font_resolve.
// See render_ctx.h for the sizing and the ownership model.
// ===================================================================

#define RENDER_CTX_FONT_CACHE_SLOTS 96   // >= the fonts' glyph_count (95)

// The two cached (cap_height, stroke_width) pairs. Written once here so both
// platforms cache the same sizes -- a song that draws text at a level only one
// of them holds would otherwise be fast on one and slow on the other.
//
// BLOB SIZING. Undersizing here fails quietly, so the numbers are derived. An
// entry
// costs 2 bytes of header plus, per inked row, 2 bytes plus ceil(width/4) --
// so about 80 bytes for a cap-10 glyph (~13 rows, ~14 wide) and about 210 for
// a cap-20 one (~26 rows, ~24 wide). A blob too small to hold all 95 is not an
// error: each glyph past the end takes a store failure, is marked uncacheable
// for good, and goes back to full geometry on every frame. The sizes below hold
// all 95 with headroom
// (95 * 80 = 7600 and 95 * 210 = 19950).
//
// The cost is heap, not the 64 KB virtual-size cap -- the host mallocs this
// block -- so the trade is ~19 KB of RAM against re-stroking half the alphabet
// every frame. Worth it. Shrink these if the heap gets tight; the cache
// degrades glyph-by-glyph rather than failing.
static void render_ctx_font_cache_levels(FONT_CACHE_LOWSPEC *levels)
{
    render_fill_bytes((unsigned char *)levels, 0, 2 * (int)sizeof(levels[0]));
    levels[0].cap_height = fx16_from_int(10);
    levels[0].stroke_width = fx16_from_int(2);
    levels[0].mem_end_offset = 8192;
    levels[1].cap_height = fx16_from_int(20);
    levels[1].stroke_width = fx16_from_int(4);
    levels[1].mem_end_offset = 20480;
}

int render_ctx_font_cache_bytes(void)
{
    FONT_CACHE_LOWSPEC levels[2];
    render_ctx_font_cache_levels(levels);
    return render_glyph_lowspec_cache_bytes(levels, 2, RENDER_CTX_FONT_CACHE_SLOTS);
}

const FONT_LOWSPEC *render_ctx_build_font_cache(const FONT_LOWSPEC *src,
                                                void *mem, int bytes)
{
    if (!mem) return src;
    FONT_CACHE_LOWSPEC levels[2];
    render_ctx_font_cache_levels(levels);
    const FONT_LOWSPEC *f = render_glyph_lowspec_font_with_cache(
        src, levels, 2, RENDER_CTX_FONT_CACHE_SLOTS, mem, bytes);
    // Falls back to the plain font if the block didn't fit -- same pixels,
    // just uncached.
    return f ? f : src;
}
