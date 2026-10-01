#ifndef VSCREEN_H_INCLUDED
#define VSCREEN_H_INCLUDED

// The Pebble binding of the vscreen_* API documented in font/render_ctx.h;
// main.c defines the shims. The framebuffer is borrowed for one
// song_visual_render pass, and draws outside it drop. Off-screen images get
// 512 B (one 25x20 image), at most 2 images and 2 target levels, so image_alloc
// can return 0 where the desktop succeeds.

// `screen` and `palette`, named directly by song.c. Here vscreen_screen is a
// pointer set only during the render pass; raw writes through it are unchecked.
// Scripts index with the folded `stride`; main.c logs once if the captured
// stride differs.
extern unsigned char *vscreen_screen;
// width * height, found at capture time.
extern int vscreen_screen_len;
#define VSCREEN_SCREEN_LEN vscreen_screen_len

// 0xAABBGGRR entries. Writes have no effect (the panel's colours are fixed); it
// exists so bodies compile unchanged.
extern int vscreen_palette[256];
#define VSCREEN_PALETTE_LEN 256

// rgb(r,g,b); mirrors the desktop vscreen.h. Ternaries for
// -Werror=misleading-indentation.
static inline int vscreen_rgb_i32(int r, int g, int b) {
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (int)((unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16) | 0xFF000000u);
}

int  vscreen_putpixel_i32(int x, int y, int c);
int  vscreen_point_fx16(int x, int y, int amount, int blend);
int  vscreen_line_fx16(int x1, int y1, int x2, int y2, int stroke_width, int alpha, int blend);
int  vscreen_ellipse_fx16(int x, int y, int radius_w, int radius_h, int alpha, int blend);
int  vscreen_circle_fx16(int x, int y, int radius, int alpha, int blend);
int  vscreen_rect_fx16(int x, int y, int w, int h, int col, int alpha, int blend);
int  vscreen_background_fx16(int col, int transparency);
int  vscreen_glyph_fx16(int x, int y, int size, int stroke_width, int ascii, int alpha, int blend);
// `str` is a byte string (one ASCII code per byte, `len` bytes, no NUL) --
// the VM's []u8 slice ABI. See font/render_ctx.h.
void vscreen_text(int x, int y, int size, int stroke_width,
                  const unsigned char *str, int len, int alpha, int blend, int letter_spacing, int line_height);
int  vscreen_font_i32(int id);
int  vscreen_text_align_i32(int id);

// ---- off-screen image targets ----
int  vscreen_image_alloc_i32(int w, int h);
int  vscreen_image_getpixel_i32(int image_id, int x, int y);
int  vscreen_image_sample_fx16(int image_id, int x, int y);
int  vscreen_push_target_i32(int image_id);
// `unused`: the VM has no zero-arg registration.
int  vscreen_pop_target_i32(int unused);
// Called before each visual body, so image ids are frame-temporary.
void vscreen_images_reset(void);

#endif // VSCREEN_H_INCLUDED
