#include "textmode.h"
#include "common/math_pure.h" // clamp

// Local overlap-safe memmove to avoid standard includes
static void *local_memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

// ===== Create / Destroy =====

TextMode *tm_create(Tsys *sys, FontCache *fc, int cols, int rows, int num_sprites) {
    TextMode *tm = (TextMode *)sys->malloc(sizeof(TextMode));
    if (!tm) return NULL;
    tm->sys = sys;
    tm->fc = fc;
    if (fc && fc->settings) {
        tm->glyph_grid_w = fc->settings->glyph_grid_size[0];
        tm->glyph_grid_h = fc->settings->glyph_grid_size[1];
    } else {
        tm->glyph_grid_w = GLYPH_W;
        tm->glyph_grid_h = GLYPH_H;
    }
    tm->cols = cols;
    tm->rows = rows;
    tm->sprites = num_sprites ? (TMSprite *)sys->malloc(sizeof(TMSprite) * num_sprites) : NULL;
    if (num_sprites && !tm->sprites) { sys->free(tm); return NULL; }
    tm->num_sprites = num_sprites;
    tm->cells = (TMCell *)sys->malloc(sizeof(TMCell) * cols * rows);
    if (!tm->cells) { sys->free(tm); return NULL; }
    // Default color: fg=0xCC (204=light gray), gradient=0 (gray), bg=0
    tm_clear(tm, TM_COLOR_PACK(0xCC, 0, 0));
    // Initialize tmp_buffer
    tm->tmp_buffer.pixels = NULL;
    tm->tmp_buffer.w = 0;
    tm->tmp_buffer.h = 0;
    tm->tmp_buffer.stride = 0;
    tm->tmp_buffer.cap = 0;
#ifndef TM_DISABLE_LINES
    tm->line_points = NULL;
    tm->num_line_points = 0;
    tm->line_points_cap = 0;
#endif
    return tm;
}

void tm_destroy(TextMode *tm) {
    if (tm) {
        Tsys *sys = tm->sys;
        if (tm->tmp_buffer.pixels) sys->free(tm->tmp_buffer.pixels);
        sys->free(tm->cells);
        sys->free(tm->sprites);
#ifndef TM_DISABLE_LINES
        if (tm->line_points) sys->free(tm->line_points);
#endif
        sys->free(tm); // tm_create allocated it; no caller frees it separately
    }
}


// ===== Resize =====

void tm_resize(TextMode *tm, int new_cols, int new_rows,
               unsigned int color) {
    if (new_cols <= 0 || new_rows <= 0) return;
    Tsys *sys = tm->sys;
    TMCell *new_cells = (TMCell *)sys->malloc(sizeof(TMCell) * new_cols * new_rows);
    if (!new_cells) return;
    // Fill new buffer with defaults
    for (int i = 0; i < new_cols * new_rows; i++) {
        cell_set(&new_cells[i], '0', color, 1.f, 1.f, 0.f, 0.f, 0);
    }
    // Copy over what fits from the old buffer
    int copy_rows = tm->rows < new_rows ? tm->rows : new_rows;
    int copy_cols = tm->cols < new_cols ? tm->cols : new_cols;
    for (int r = 0; r < copy_rows; r++) {
        sys->memcpy(new_cells + r * new_cols,
                    tm->cells + r * tm->cols,
                    sizeof(TMCell) * copy_cols);
    }
    sys->free(tm->cells);
    tm->cells = new_cells;
    tm->cols = new_cols;
    tm->rows = new_rows;
}

// ===== Clear =====

void tm_clear(TextMode *tm, unsigned int color) {
    for (int i = 0; i < tm->cols * tm->rows; i++) {
        tm->cells[i].ch = ' ';
        tm->cells[i].color = color;
        tm->cells[i].flags = 0;
    }
}

void tm_put_full(TextMode *tm, int col, int row,
            unsigned char ch, unsigned int color,
            float scale, float weight, float offset_x, float offset_y) {
    if (col < 0 || col >= tm->cols || row < 0 || row >= tm->rows) return;
    TMCell *c = &tm->cells[row * tm->cols + col];
    cell_set(c, ch, color, scale, weight, offset_x, offset_y, 0);
}

int tm_get(TextMode *tm, int col, int row, TMCell *out) {
    if (col < 0 || col >= tm->cols || row < 0 || row >= tm->rows) return 0;
    *out = tm->cells[row * tm->cols + col];
    return 1;
}

int tm_puts(TextMode *tm, int col, int row,
            const char *str, unsigned int color) {
    if (!str) return col;
    int c = col;
    while (*str) {
        if (c >= tm->cols) {
            break;
        }
        if (*str == '\n') {
            c = 0;
            row++;
            if (row >= tm->rows) {
                break;
            }
            str++;
            continue;
        }
        if (*str == '\r') {
            c = 0;
            str++;
            continue;
        }
        unsigned char ch = (unsigned char)*str;
        tm_put(tm, c, row, ch, color);
        c++;
        str++;
    }
    return c;
}

// ===== Additive AA line overlay =====
#ifndef TM_DISABLE_LINES

#define AALINE_PLOT_4_X_64 1
#include "common/render_aaline.h"

void tm_lines_clear(TextMode *tm) {
    tm->num_line_points = 0;
}

static TMLinePoint *tm_line_push(TextMode *tm, float x, float y, float alpha, int is_move_to, int color) {
    if (tm->num_line_points >= tm->line_points_cap) {
        int new_cap = tm->line_points_cap ? tm->line_points_cap * 2 : 64;
        TMLinePoint *grown = (TMLinePoint *)tm->sys->realloc(tm->line_points, sizeof(TMLinePoint) * new_cap);
        if (!grown) return NULL;
        tm->line_points = grown;
        tm->line_points_cap = new_cap;
    }
    TMLinePoint *p = &tm->line_points[tm->num_line_points++];
    p->x = fixed_from_float(x);
    p->y = fixed_from_float(y);
    alpha = clamp(alpha, 0.0f, 1.0f);
    p->opacity = (unsigned char)(alpha * 255.0f + 0.5f);
    p->is_move_to = (unsigned char)is_move_to;
    p->color = (unsigned char)color;
    return p;
}

void tm_move_to(TextMode *tm, float x, float y, float alpha, int color) {
    tm_line_push(tm, x, y, alpha, 1, color);
}

void tm_line_to(TextMode *tm, float x, float y, float alpha, int color) {
    tm_line_push(tm, x, y, alpha, 0, color);
}

static void tm_render_lines(const TextMode *tm, Indexed8Bit *dst) {
    if (tm->num_line_points < 2) return;
    RSurface surf;
    surf.stride = dst->stride;
    surf.pixels = dst->pixels;
    surf.clip_x = 0;
    surf.clip_y = 0;
    surf.clip_w = dst->w;
    surf.clip_h = dst->h;
    surf.layout = 0;

    for (int i = 1; i < tm->num_line_points; i++) {
        TMLinePoint *p1 = &tm->line_points[i];
        if (p1->is_move_to) continue;
        TMLinePoint *p0 = &tm->line_points[i - 1];
        fixed a0 = fixed_div(fixed_from_int(p0->opacity), fixed_from_int(255));
        fixed a1 = fixed_div(fixed_from_int(p1->opacity), fixed_from_int(255));
        aaline(&surf, p0->x, p0->y, a0, p1->x, p1->y, a1, p0->color);
    }
}

#endif // TM_DISABLE_LINES

// ===== Indexed8Bit helpers =====

void i8_set_default_colors(Indexed8Bit *i8) {
    unsigned int *pal = i8->palette;
    //unsigned char gray [8*3] = {0,0,0,27,28,43,58,65,88,95,108,130,141,153,166,197,200,203,247,247,247,255,255,255};
    //unsigned char cold [8*3] = {0,0,0,27,28,43,46,55,108,74,95,190,104,160,240,193,221,255,230,241,255,255,255,255};
    //unsigned char warm [8*3] = {0,0,0,27,28,43,85,42,90,160,67,84,233,129,96,255,193,98,255,250,136,255,255,255};
    //unsigned char green[8*3] = {0,0,0,27,28,43,58,65,92,95,108,130,94,168,100,166,214,115,255,250,136,255,255,255};
    unsigned char gray [8*3] = {0,0,0,38,39,56,73,80,103,110,123,144,154,165,177,205,207,211,248,248,248,255,255,255};
    unsigned char cold [8*3] = {0,0,0,38,39,56,59,69,123,89,110,198,119,171,242,201,225,255,233,243,255,255,255,255};
    unsigned char warm [8*3] = {0,0,0,38,39,56,100,55,105,171,82,99,236,143,111,255,201,113,255,250,149,255,255,255};
    unsigned char green[8*3] = {0,0,0,38,39,56,73,80,108,110,123,144,110,179,115,177,220,130,255,250,149,255,255,255};

    const unsigned char *samples[4] = {
        gray, cold, warm, green
    };

    for (int ramp = 0; ramp < 4; ramp++) {
        i8->ramps[ramp].num_colors = 64;
        i8->ramps[ramp].first_palette_id = (unsigned char)(ramp * 64);
        const unsigned char *s = samples[ramp];
        for (int i = 0; i < 64; i++) {
            float fi = (float)i * 7.0f / 63.0f;
            int idx = (int)fi;
            float t = fi - (float)idx;
            if (idx > 6) idx = 6;
            float r = (float)s[idx*3]   * (1.0f - t) + (float)s[(idx+1)*3]   * t;
            float g = (float)s[idx*3+1] * (1.0f - t) + (float)s[(idx+1)*3+1] * t;
            float b = (float)s[idx*3+2] * (1.0f - t) + (float)s[(idx+1)*3+2] * t;
            int a = i * 255 / 63;
            int pi = ramp * 64 + i;
            pal[pi] = ((unsigned int)a << 24) |
                      (((unsigned int)(r + 0.5f) & 0xFF) << 16) |
                      (((unsigned int)(g + 0.5f) & 0xFF) <<  8) |
                       ((unsigned int)(b + 0.5f) & 0xFF);
        }
    }
    for (int ramp = 4; ramp < 128; ramp++) {
        i8->ramps[ramp].num_colors = 0;
        i8->ramps[ramp].first_palette_id = 0;
    }
}

void i8_to_argb(Indexed8Bit *i8,
                int pixel_w, int pixel_h, int pixel_stride,
                unsigned int *dstRgba,
                float opacity_min, float opacity_max,
                int flags) {
    if (!i8 || !i8->pixels || !dstRgba) return;

    unsigned int tmp_pal[256];
    int use_opacity = (opacity_min != 1.0f || opacity_max != 1.0f);
    int swap_rb = (flags & TM_FLAGS_SWAP_RED_BLUE_CHANNELS) ? 1 : 0;

    for (int i = 0; i < 256; i++) {
        unsigned int argb = i8->palette[i];
        unsigned int a = (argb >> 24) & 0xFF;
        unsigned int r = (argb >> 16) & 0xFF;
        unsigned int g = (argb >>  8) & 0xFF;
        unsigned int b =  argb        & 0xFF;
        if (swap_rb) { unsigned int t = r; r = b; b = t; }
        if (use_opacity) {
            float opacity = opacity_min + (opacity_max - opacity_min) * (float)a / 255.0f;
            unsigned int na = (unsigned int)(opacity * 255.0f);
            if (na > 255) na = 255;
            tmp_pal[i] = (na << 24) | (r << 16) | (g << 8) | b;
        } 
        else 
        {
            tmp_pal[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }

    if (use_opacity) {
        for (int py = 0; py < pixel_h; py++) {
            for (int px = 0; px < pixel_w; px++) {
                unsigned int color = tmp_pal[i8->pixels[py * i8->stride + px]];
                unsigned int sa = (color >> 24) & 0xFF;
                if (sa == 255) {
                    dstRgba[py * pixel_stride + px] = color;
                } else if (sa == 0) {
                    // transparent: keep destination
                } else {
                    unsigned int *dst = &dstRgba[py * pixel_stride + px];
                    unsigned int da = (*dst >> 24) & 0xFF;
                    unsigned int dr = (*dst >> 16) & 0xFF;
                    unsigned int dg = (*dst >>  8) & 0xFF;
                    unsigned int db =  *dst        & 0xFF;
                    unsigned int inv = 255 - sa;
                    unsigned int or_ = ((color >> 16) & 0xFF) * sa + dr * inv + 127;
                    unsigned int og_ = ((color >>  8) & 0xFF) * sa + dg * inv + 127;
                    unsigned int ob_ =  (color       & 0xFF) * sa + db * inv + 127;
                    unsigned int oa_ = sa * 255 + da * inv + 127;
                    *dst = ((oa_ / 255) << 24) | ((or_ / 255) << 16) |
                        ((og_ / 255) << 8) | (ob_ / 255);
                }
            }
        }
    } else {
        for (int py = 0; py < pixel_h; py++) {
            for (int px = 0; px < pixel_w; px++) {
                unsigned int color = tmp_pal[i8->pixels[py * i8->stride + px]];
                dstRgba[py * pixel_stride + px] = color;
            }
        }
    }
}

// ===== 8-bit indexed per-cell rendering helpers =====

// TEXTMODE_GAMMA_CORRECT: blend glyph coverage in a gamma space instead of
// straight in palette-index space.
//
// The default path treats the ramp index as if it were linear light and does
// `*p += alpha * (fg - bg)`, which makes antialiased stems look thinner and
// dirtier than they should -- half coverage is not half brightness on a
// display.  With the flag on we map fg and bg through tm_gamma_to_linear,
// interpolate there, and map the result back through tm_gamma_to_display.
//
// Because the result is a function of (fg, bg, alpha) alone it no longer needs
// to know what was already in the framebuffer: the blend is written, not
// accumulated, and the background fill that ran first is simply assumed to have
// left `bg` behind.  That also means the glyph pass is idempotent.
//
// The curve is a true pow(x, 2.2) / pow(x, 1/2.2) pair, tabulated.  Sizing the
// linear side is the only real decision here: linear light near black is so
// compressed that the table's second entry is the darkest non-black value the
// blend can express at all, and everything below it collapses to 0.  Against a
// double-precision reference, over every fg x bg x alpha on a 64-entry ramp:
//
//     linear   bytes   max index err   ramp indices reachable
//        256     768         6              59 / 64
//        512    1024         4              61 / 64
//       1024    1536         3              63 / 64
//       4096    4608         1              64 / 64
//
// 1024 is the knee.  Going to 4096 buys back the single darkest ramp step and
// costs 3 KB more .rodata; if that matters more than the space (or less), the
// tables below are generated -- regenerate them for a different size.
#ifndef TEXTMODE_GAMMA_CORRECT
#define TEXTMODE_GAMMA_CORRECT 1
#endif

#if TEXTMODE_GAMMA_CORRECT
#define TM_GAMMA_LINEAR_SIZE 1024

/* display (0..255) -> linear (0..1023), gamma 2.2.
   pow(d/255, 2.2) * 1023.  Needs 10 bits: linear light near black is so
   compressed that an 8-bit linear intermediate collapses every display
   value below ~130 onto the same few entries. */
static const unsigned short tm_gamma_to_linear[256] = {
       0,   0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,
       1,   1,   2,   2,   2,   3,   3,   3,   4,   4,   5,   5,
       6,   6,   7,   7,   8,   9,   9,  10,  11,  11,  12,  13,
      14,  15,  16,  16,  17,  18,  19,  20,  21,  23,  24,  25,
      26,  27,  28,  30,  31,  32,  34,  35,  36,  38,  39,  41,
      42,  44,  46,  47,  49,  51,  52,  54,  56,  58,  60,  61,
      63,  65,  67,  69,  71,  73,  76,  78,  80,  82,  84,  87,
      89,  91,  94,  96,  98, 101, 103, 106, 109, 111, 114, 117,
     119, 122, 125, 128, 130, 133, 136, 139, 142, 145, 148, 151,
     155, 158, 161, 164, 167, 171, 174, 177, 181, 184, 188, 191,
     195, 198, 202, 206, 209, 213, 217, 221, 225, 228, 232, 236,
     240, 244, 248, 252, 257, 261, 265, 269, 274, 278, 282, 287,
     291, 295, 300, 304, 309, 314, 318, 323, 328, 333, 337, 342,
     347, 352, 357, 362, 367, 372, 377, 382, 387, 393, 398, 403,
     408, 414, 419, 425, 430, 436, 441, 447, 452, 458, 464, 470,
     475, 481, 487, 493, 499, 505, 511, 517, 523, 529, 535, 542,
     548, 554, 561, 567, 573, 580, 586, 593, 599, 606, 613, 619,
     626, 633, 640, 647, 653, 660, 667, 674, 681, 689, 696, 703,
     710, 717, 725, 732, 739, 747, 754, 762, 769, 777, 784, 792,
     800, 807, 815, 823, 831, 839, 847, 855, 863, 871, 879, 887,
     895, 903, 912, 920, 928, 937, 945, 954, 962, 971, 979, 988,
     997,1005,1014,1023,
};

/* linear (0..1023) -> display (0..255), gamma 2.2.
   pow(l/1023, 1/2.2) * 255.  Generated, so there is no pow() at runtime
   and no init order to get wrong; costs 1024 bytes of .rodata. */
static const unsigned char tm_gamma_to_display[TM_GAMMA_LINEAR_SIZE] = {
       0,  11,  15,  18,  21,  23,  25,  26,  28,  30,  31,  32,  34,  35,  36,  37,
      39,  40,  41,  42,  43,  44,  45,  45,  46,  47,  48,  49,  50,  50,  51,  52,
      53,  54,  54,  55,  56,  56,  57,  58,  58,  59,  60,  60,  61,  62,  62,  63,
      63,  64,  65,  65,  66,  66,  67,  68,  68,  69,  69,  70,  70,  71,  71,  72,
      72,  73,  73,  74,  74,  75,  75,  76,  76,  77,  77,  78,  78,  79,  79,  80,
      80,  81,  81,  81,  82,  82,  83,  83,  84,  84,  84,  85,  85,  86,  86,  87,
      87,  87,  88,  88,  89,  89,  89,  90,  90,  91,  91,  91,  92,  92,  93,  93,
      93,  94,  94,  94,  95,  95,  96,  96,  96,  97,  97,  97,  98,  98,  98,  99,
      99,  99, 100, 100, 101, 101, 101, 102, 102, 102, 103, 103, 103, 104, 104, 104,
     105, 105, 105, 106, 106, 106, 107, 107, 107, 108, 108, 108, 108, 109, 109, 109,
     110, 110, 110, 111, 111, 111, 112, 112, 112, 112, 113, 113, 113, 114, 114, 114,
     115, 115, 115, 115, 116, 116, 116, 117, 117, 117, 117, 118, 118, 118, 119, 119,
     119, 119, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 123, 123, 123, 123,
     124, 124, 124, 124, 125, 125, 125, 125, 126, 126, 126, 127, 127, 127, 127, 128,
     128, 128, 128, 129, 129, 129, 129, 130, 130, 130, 130, 131, 131, 131, 131, 132,
     132, 132, 132, 133, 133, 133, 133, 134, 134, 134, 134, 135, 135, 135, 135, 136,
     136, 136, 136, 137, 137, 137, 137, 138, 138, 138, 138, 138, 139, 139, 139, 139,
     140, 140, 140, 140, 141, 141, 141, 141, 142, 142, 142, 142, 142, 143, 143, 143,
     143, 144, 144, 144, 144, 144, 145, 145, 145, 145, 146, 146, 146, 146, 146, 147,
     147, 147, 147, 148, 148, 148, 148, 148, 149, 149, 149, 149, 149, 150, 150, 150,
     150, 151, 151, 151, 151, 151, 152, 152, 152, 152, 152, 153, 153, 153, 153, 154,
     154, 154, 154, 154, 155, 155, 155, 155, 155, 156, 156, 156, 156, 156, 157, 157,
     157, 157, 157, 158, 158, 158, 158, 158, 159, 159, 159, 159, 159, 160, 160, 160,
     160, 160, 161, 161, 161, 161, 161, 162, 162, 162, 162, 162, 163, 163, 163, 163,
     163, 164, 164, 164, 164, 164, 165, 165, 165, 165, 165, 165, 166, 166, 166, 166,
     166, 167, 167, 167, 167, 167, 168, 168, 168, 168, 168, 168, 169, 169, 169, 169,
     169, 170, 170, 170, 170, 170, 171, 171, 171, 171, 171, 171, 172, 172, 172, 172,
     172, 173, 173, 173, 173, 173, 173, 174, 174, 174, 174, 174, 174, 175, 175, 175,
     175, 175, 176, 176, 176, 176, 176, 176, 177, 177, 177, 177, 177, 177, 178, 178,
     178, 178, 178, 179, 179, 179, 179, 179, 179, 180, 180, 180, 180, 180, 180, 181,
     181, 181, 181, 181, 181, 182, 182, 182, 182, 182, 182, 183, 183, 183, 183, 183,
     183, 184, 184, 184, 184, 184, 185, 185, 185, 185, 185, 185, 186, 186, 186, 186,
     186, 186, 186, 187, 187, 187, 187, 187, 187, 188, 188, 188, 188, 188, 188, 189,
     189, 189, 189, 189, 189, 190, 190, 190, 190, 190, 190, 191, 191, 191, 191, 191,
     191, 192, 192, 192, 192, 192, 192, 192, 193, 193, 193, 193, 193, 193, 194, 194,
     194, 194, 194, 194, 195, 195, 195, 195, 195, 195, 195, 196, 196, 196, 196, 196,
     196, 197, 197, 197, 197, 197, 197, 197, 198, 198, 198, 198, 198, 198, 199, 199,
     199, 199, 199, 199, 199, 200, 200, 200, 200, 200, 200, 201, 201, 201, 201, 201,
     201, 201, 202, 202, 202, 202, 202, 202, 202, 203, 203, 203, 203, 203, 203, 204,
     204, 204, 204, 204, 204, 204, 205, 205, 205, 205, 205, 205, 205, 206, 206, 206,
     206, 206, 206, 206, 207, 207, 207, 207, 207, 207, 207, 208, 208, 208, 208, 208,
     208, 209, 209, 209, 209, 209, 209, 209, 210, 210, 210, 210, 210, 210, 210, 211,
     211, 211, 211, 211, 211, 211, 212, 212, 212, 212, 212, 212, 212, 213, 213, 213,
     213, 213, 213, 213, 213, 214, 214, 214, 214, 214, 214, 214, 215, 215, 215, 215,
     215, 215, 215, 216, 216, 216, 216, 216, 216, 216, 217, 217, 217, 217, 217, 217,
     217, 218, 218, 218, 218, 218, 218, 218, 218, 219, 219, 219, 219, 219, 219, 219,
     220, 220, 220, 220, 220, 220, 220, 221, 221, 221, 221, 221, 221, 221, 221, 222,
     222, 222, 222, 222, 222, 222, 223, 223, 223, 223, 223, 223, 223, 223, 224, 224,
     224, 224, 224, 224, 224, 225, 225, 225, 225, 225, 225, 225, 225, 226, 226, 226,
     226, 226, 226, 226, 226, 227, 227, 227, 227, 227, 227, 227, 228, 228, 228, 228,
     228, 228, 228, 228, 229, 229, 229, 229, 229, 229, 229, 229, 230, 230, 230, 230,
     230, 230, 230, 230, 231, 231, 231, 231, 231, 231, 231, 232, 232, 232, 232, 232,
     232, 232, 232, 233, 233, 233, 233, 233, 233, 233, 233, 234, 234, 234, 234, 234,
     234, 234, 234, 235, 235, 235, 235, 235, 235, 235, 235, 236, 236, 236, 236, 236,
     236, 236, 236, 237, 237, 237, 237, 237, 237, 237, 237, 238, 238, 238, 238, 238,
     238, 238, 238, 238, 239, 239, 239, 239, 239, 239, 239, 239, 240, 240, 240, 240,
     240, 240, 240, 240, 241, 241, 241, 241, 241, 241, 241, 241, 242, 242, 242, 242,
     242, 242, 242, 242, 243, 243, 243, 243, 243, 243, 243, 243, 243, 244, 244, 244,
     244, 244, 244, 244, 244, 245, 245, 245, 245, 245, 245, 245, 245, 245, 246, 246,
     246, 246, 246, 246, 246, 246, 247, 247, 247, 247, 247, 247, 247, 247, 248, 248,
     248, 248, 248, 248, 248, 248, 248, 249, 249, 249, 249, 249, 249, 249, 249, 249,
     250, 250, 250, 250, 250, 250, 250, 250, 251, 251, 251, 251, 251, 251, 251, 251,
     251, 252, 252, 252, 252, 252, 252, 252, 252, 252, 253, 253, 253, 253, 253, 253,
     253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255,
};
#endif // TEXTMODE_GAMMA_CORRECT


static void render_cell_bg_8bit(const TextMode *tm, TMCell *cell, Indexed8Bit *dst,
                                int cell_w, int cell_h, int col, int row, unsigned char bg_def) {

    unsigned char bg = get_indexed_color(dst, TM_COLOR_BG(cell->color), TM_COLOR_GRADIENT(cell->color)); // background always uses gray ramp

    if (bg == bg_def) return; 

    int x0 = col * cell_w;
    int y0 = row * cell_h;

    int clip_mid_x = x0 + cell_w / 2;
    int clip_mid_y = y0 + cell_h / 2;
    unsigned short clip_flags = cell->flags;

    x0 += (int)(cell->offset_x * cell_w);
    y0 += (int)(cell->offset_y * cell_h);

    // Same half-plane argument as the glyph blit: resolve the surviving span in
    // each axis once, which leaves a flat constant fill the compiler can widen.
    int x_lo = 0, x_hi = dst->w;
    int y_lo = 0, y_hi = dst->h;
    if ((clip_flags & CELL_FLAGS_CLIP_L)      && clip_mid_x > x_lo) x_lo = clip_mid_x;
    if ((clip_flags & CELL_FLAGS_CLIP_R)      && clip_mid_x < x_hi) x_hi = clip_mid_x;
    if ((clip_flags & CELL_FLAGS_CLIP_TOP)    && clip_mid_y > y_lo) y_lo = clip_mid_y;
    if ((clip_flags & CELL_FLAGS_CLIP_BOTTOM) && clip_mid_y < y_hi) y_hi = clip_mid_y;

    int dx_start = x_lo - x0; if (dx_start < 0)      dx_start = 0;
    int dx_end   = x_hi - x0; if (dx_end > cell_w)   dx_end   = cell_w;
    int dy_start = y_lo - y0; if (dy_start < 0)      dy_start = 0;
    int dy_end   = y_hi - y0; if (dy_end > cell_h)   dy_end   = cell_h;
    if (dx_start >= dx_end || dy_start >= dy_end) return;

    int span = dx_end - dx_start;
    unsigned char *prow = dst->pixels + (y0 + dy_start) * dst->stride + (x0 + dx_start);

    for (int dy = dy_start; dy < dy_end; dy++, prow += dst->stride) {
        for (int i = 0; i < span; i++) prow[i] = bg;
    }
}

static void render_cell_8bit(const TextMode *tm, TMCell *cell, Indexed8Bit *dst,
                             int cell_w, int cell_h, int glyph_h,
                             int col, int row, float cell_width_mul) {
    int idx = cell->ch;
    if (idx < 32) return;
    idx -= 32;

    unsigned char fg = TM_COLOR_FG(cell->color);
    unsigned char bg = TM_COLOR_BG(cell->color);

    int cell_scale_allowed = idx < 127 - 32 ? 1 : 0;

    float weight = cell->weight;
    if(weight > 2.f) {
        // dont allow fatter than 2, convert wanted thickness to brighter color
        fg = (unsigned char)(fg + m_fminf(1.0f, (weight - 2.f) * 0.25f) * (255 - fg));
        weight = 2.f; 
    }    
    float stroke_width = cell_width_mul * 2.f * weight;
    if(stroke_width < 1.f) {
        // don't allow thinner than 1 pixel: convert to opacity:
        fg = (unsigned char)(bg + stroke_width * (fg - bg));
        stroke_width = 1.f;
    }
    float cache_font_size = cell_width_mul * (cell_scale_allowed ? cell->scale : 1.f );

    const FontCacheGlyphInfo *g = font_cache_get(tm->fc, idx, cache_font_size, stroke_width);
    if (!g || !g->bitmap) return;

    int x0 = col * cell_w;
    int y0 = row * cell_h;

    int clip_mid_x = x0 + cell_w / 2;
    int clip_mid_y = y0 + cell_h / 2;
    unsigned short clip_flags = cell->flags;

    y0 += (cell_h - glyph_h) >> 1; // center glyphs vertically

    int bx0 = x0 - (int)(g->ox);
    int by0 = y0 - (int)(g->oy);
    bx0 -= (int)(((cache_font_size - cell_width_mul)/cell_width_mul) * 0.5f * cell_w  - cell->offset_x * cell_w);
    by0 -= (int)(((cache_font_size - cell_width_mul)/cell_width_mul) * 0.5f * glyph_h - cell->offset_y * cell_h);


    if (fg == bg) return;

    const IndexedColorRamp *ramp = &dst->ramps[TM_COLOR_GRADIENT(cell->color)];

#if TEXTMODE_GAMMA_CORRECT
    int pal_base  = (int)ramp->first_palette_id;
    int pal_num   = (int)ramp->num_colors;
    int lin_bg    = (int)tm_gamma_to_linear[bg];
    int lin_delta = (int)tm_gamma_to_linear[fg] - lin_bg;
#else
    int mul_precalc = (((int)fg - (int)bg) * (int)ramp->num_colors);
#endif

    // Every surviving pixel has to clear four half-plane tests per axis (the
    // framebuffer edge and the half-cell clip), so what survives is one
    // contiguous span in x and one in y.  Resolve both spans up front and the
    // inner loop keeps only the alpha==0 skip.
    int x_lo = 0, x_hi = dst->w;
    int y_lo = 0, y_hi = dst->h;
    if ((clip_flags & CELL_FLAGS_CLIP_L)      && clip_mid_x > x_lo) x_lo = clip_mid_x;
    if ((clip_flags & CELL_FLAGS_CLIP_R)      && clip_mid_x < x_hi) x_hi = clip_mid_x;
    if ((clip_flags & CELL_FLAGS_CLIP_TOP)    && clip_mid_y > y_lo) y_lo = clip_mid_y;
    if ((clip_flags & CELL_FLAGS_CLIP_BOTTOM) && clip_mid_y < y_hi) y_hi = clip_mid_y;

    int bx_start = x_lo - bx0; if (bx_start < 0)     bx_start = 0;
    int bx_end   = x_hi - bx0; if (bx_end > g->bw)   bx_end   = g->bw;
    int by_start = y_lo - by0; if (by_start < 0)     by_start = 0;
    int by_end   = y_hi - by0; if (by_end > g->bh)   by_end   = g->bh;
    if (bx_start >= bx_end || by_start >= by_end) return;

    int span = bx_end - bx_start;
    const unsigned char *srow = g->bitmap + by_start * g->bw + bx_start;
    unsigned char *prow = dst->pixels + (by0 + by_start) * dst->stride + (bx0 + bx_start);

    for (int by = by_start; by < by_end; by++, srow += g->bw, prow += dst->stride) {
        for (int i = 0; i < span; i++) {
            unsigned int alpha = srow[i];
            if (alpha == 0) continue;
#if TEXTMODE_GAMMA_CORRECT
            // alpha + (alpha >> 7) maps 0..255 onto 0..256 so full coverage
            // lands exactly on lin_fg instead of 255/256 of the way there.
            int lin = lin_bg + (((int)(alpha + (alpha >> 7)) * lin_delta) >> 8);
            prow[i] = (unsigned char)(pal_base + ((tm_gamma_to_display[lin] * pal_num) >> 8));
#else
            prow[i] += (unsigned char)((alpha * mul_precalc) >> 16);
#endif
        }
    }
}

// ===== Top-level wrapper =====

void tm_render(const TextMode *tm,
               int pixel_w, int pixel_h, int pixel_stride, unsigned int *dstRgba,
               float cell_width_mul, float line_height, unsigned int default_color,
               int flags) {

    int gw = tm->fc->settings->glyph_grid_size[0];
    int gh = tm->fc->settings->glyph_grid_size[1];
    int cell_w  = (int)((float)gw * cell_width_mul);
    int cell_h  = (int)((float)gh * cell_width_mul * line_height);
    int glyph_h = (int)((float)gh * cell_width_mul);
    if (cell_w <= 0 || cell_h <= 0) return;

    tm->fc->settings->descend = clamp((line_height - 1.0f) * 2.5f, 0.f, 1.f);  // increase descenders as line_height goes from 1 to 1.4

    // Scratch indexed-colour buffer for this render pass. It lives on the
    // TextMode and only ever grows: a full-screen buffer is megabytes, and
    // malloc/free-ing one every frame walks it to a new address whenever a
    // smaller, longer-lived allocation (a font-cache glyph bitmap, say) lands
    // in the hole it just left. On a fixed arena that marches the big buffer
    // through the heap and shreds every large free run, so a later frame finds
    // no contiguous block big enough and the app silently stops rendering.
    Tsys *sys = tm->sys;
    Indexed8Bit *tmp = &((TextMode *)tm)->tmp_buffer;
    int need = pixel_w * pixel_h;
    if (tmp->cap < need) {
        unsigned char *pix = (unsigned char *)sys->malloc((size_t)need);
        if (!pix) return;
        if (tmp->pixels) sys->free(tmp->pixels);
        tmp->pixels = pix;
        tmp->cap = need;
    }
    tmp->w = pixel_w;
    tmp->h = pixel_h;
    tmp->stride = pixel_w;

    i8_set_default_colors(tmp);

    // --- Fill with default background ---
    unsigned char def_bg_idx = get_indexed_color_bg(tmp, default_color);
    for (int i = 0; i < pixel_w * pixel_h; i++)
        tmp->pixels[i] = def_bg_idx;

    // --- Render cell backgrounds ---
    for (int row = 0; row < tm->rows; row++) {
        for (int col = 0; col < tm->cols; col++) {
            TMCell *cell = (TMCell *)&tm->cells[row * tm->cols + col];
            render_cell_bg_8bit(tm, cell, tmp, cell_w, cell_h, col, row, def_bg_idx);
        }
    }
    for (int i = 0; i < tm->num_sprites; i++) {
        TMSprite *s = (TMSprite *)&tm->sprites[i];
        render_cell_bg_8bit(tm, &s->cell, tmp, cell_w, cell_h, s->col, s->row, def_bg_idx);
    }

    // --- Render glyphs ---
    for (int row = 0; row < tm->rows; row++) {
        for (int col = 0; col < tm->cols; col++) {
            TMCell *cell = (TMCell *)&tm->cells[row * tm->cols + col];
            render_cell_8bit(tm, cell, tmp, cell_w, cell_h, glyph_h, col, row, cell_width_mul);
        }
    }
    for (int i = 0; i < tm->num_sprites; i++) {
        TMSprite *s = (TMSprite *)&tm->sprites[i];
        render_cell_8bit(tm, &s->cell, tmp, cell_w, cell_h, glyph_h, s->col, s->row, cell_width_mul);
    }

#ifndef TM_DISABLE_LINES
    // --- Additive AA line overlay, drawn on top of everything else ---
    tm_render_lines(tm, tmp);
    ((TextMode *)tm)->num_line_points = 0;
#endif

    // --- Convert indexed to ARGB ---
    i8_to_argb(tmp, pixel_w, pixel_h, pixel_stride, dstRgba, 1.0f, 1.0f, flags);
}
