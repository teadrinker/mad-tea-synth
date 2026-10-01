// The ctx-taking drawing primitives for the visual VMs. song.c calls the flat
// global-bound vscreen_* functions; the interpreter reaches the context through
// the VM. CFuncEntry keeps the emitter's C name and the interpreter's pointer in
// separate slots, so one registration carries both. The names differ from the
// vscreen_* ones since both link into the synth. Behaviour lives in
// font/render_ctx.c.

#include "CodeSynthVmCtx.h"
#include "font/render_ctx.h"

// Null for an audio VM or unset user_data; every thunk then drops the draw.
#define VS_CTX(ctx) RenderCtx *c = ((CodeSynthVmCtx*)(ctx)) \
                                 ? ((CodeSynthVmCtx*)(ctx))->screen : 0

int vscreen_putpixel_i32_ctx(void *ctx, int x, int y, int col) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_putpixel_i32(c, x, y, col);
}

int vscreen_point_fx16_ctx(void *ctx, int x, int y, int amount, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_point_fx16(c, x, y, amount, blend);
}

int vscreen_line_fx16_ctx(void *ctx, int x1, int y1, int x2, int y2,
                          int stroke_width, int alpha, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_line_fx16(c, x1, y1, x2, y2, stroke_width, alpha, blend);
}

int vscreen_ellipse_fx16_ctx(void *ctx, int x, int y, int radius_w, int radius_h,
                             int alpha, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_ellipse_fx16(c, x, y, radius_w, radius_h, alpha, blend);
}

int vscreen_circle_fx16_ctx(void *ctx, int x, int y, int radius, int alpha, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_circle_fx16(c, x, y, radius, alpha, blend);
}

int vscreen_rect_fx16_ctx(void *ctx, int x, int y, int w, int h, int col,
                          int alpha, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_rect_fx16(c, x, y, w, h, col, alpha, blend);
}

int vscreen_background_fx16_ctx(void *ctx, int col, int transparency) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_background_fx16(c, col, transparency);
}

int vscreen_glyph_fx16_ctx(void *ctx, int x, int y, int size, int stroke_width,
                           int ascii, int alpha, int blend) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_glyph_fx16(c, x, y, size, stroke_width, ascii, alpha, blend);
}

void vscreen_text_ctx(void *ctx, int x, int y, int size, int stroke_width,
                      const unsigned char *str, int len, int alpha, int blend,
                      int letter_spacing, int line_height) {
    VS_CTX(ctx); if (!c) return;
    render_ctx_text(c, x, y, size, stroke_width, str, len, alpha, blend,
                    letter_spacing, line_height);
}

int vscreen_font_i32_ctx(void *ctx, int id) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_font_i32(c, id);
}

int vscreen_text_align_i32_ctx(void *ctx, int id) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_text_align_i32(c, id);
}

int vscreen_image_alloc_i32_ctx(void *ctx, int w, int h) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_image_alloc_i32(c, w, h);
}

int vscreen_image_getpixel_i32_ctx(void *ctx, int image_id, int x, int y) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_image_getpixel_i32(c, image_id, x, y);
}

int vscreen_image_sample_fx16_ctx(void *ctx, int image_id, int x, int y) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_image_sample_fx16(c, image_id, x, y);
}

int vscreen_push_target_i32_ctx(void *ctx, int image_id) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_push_target_i32(c, image_id);
}

int vscreen_pop_target_i32_ctx(void *ctx, int unused) {
    VS_CTX(ctx); if (!c) return 0;
    return render_ctx_pop_target_i32(c, unused);
}

#undef VS_CTX
