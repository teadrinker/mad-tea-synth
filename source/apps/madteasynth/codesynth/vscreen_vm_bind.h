#ifndef VSCREEN_VM_BIND_H
#define VSCREEN_VM_BIND_H

// Each takes the VM user_data (a CodeSynthVmCtx) and draws through its context.

#ifdef __cplusplus
extern "C" {
#endif

int  vscreen_putpixel_i32_ctx(void *ctx, int x, int y, int col);
int  vscreen_point_fx16_ctx(void *ctx, int x, int y, int amount, int blend);
int  vscreen_line_fx16_ctx(void *ctx, int x1, int y1, int x2, int y2,
                           int stroke_width, int alpha, int blend);
int  vscreen_ellipse_fx16_ctx(void *ctx, int x, int y, int radius_w, int radius_h,
                              int alpha, int blend);
int  vscreen_circle_fx16_ctx(void *ctx, int x, int y, int radius, int alpha, int blend);
int  vscreen_rect_fx16_ctx(void *ctx, int x, int y, int w, int h, int col,
                           int alpha, int blend);
int  vscreen_background_fx16_ctx(void *ctx, int col, int transparency);
int  vscreen_glyph_fx16_ctx(void *ctx, int x, int y, int size, int stroke_width,
                            int ascii, int alpha, int blend);
void vscreen_text_ctx(void *ctx, int x, int y, int size, int stroke_width,
                      const unsigned char *str, int len, int alpha, int blend,
                      int letter_spacing, int line_height);
int  vscreen_font_i32_ctx(void *ctx, int id);
int  vscreen_text_align_i32_ctx(void *ctx, int id);
int  vscreen_color_ramp_setup_i32_ctx(void *ctx, int add, int bit_offset,
                                      int low, int mid, int high);
int  vscreen_image_alloc_i32_ctx(void *ctx, int w, int h);
int  vscreen_image_getpixel_i32_ctx(void *ctx, int image_id, int x, int y);
int  vscreen_image_sample_fx16_ctx(void *ctx, int image_id, int x, int y);
int  vscreen_push_target_i32_ctx(void *ctx, int image_id);
int  vscreen_pop_target_i32_ctx(void *ctx, int unused);

#ifdef __cplusplus
}
#endif

#endif // VSCREEN_VM_BIND_H
