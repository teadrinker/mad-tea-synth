#ifndef TEXTMODE_CELL_H
#define TEXTMODE_CELL_H

// this file exists so that textmode_ui don't depend on textmode,
// in case we want to have another textmode backend...


// clip - always clips at cell center (ignoring offset)
#define CELL_FLAGS_CLIP_R      (1 << 1)
#define CELL_FLAGS_CLIP_L      (1 << 2)
#define CELL_FLAGS_CLIP_TOP    (1 << 3)
#define CELL_FLAGS_CLIP_BOTTOM (1 << 4)

// Set by the UI overlay system to prevent later writes to the same cell.
#define CELL_FLAGS_DRAWN       (1 << 0)

typedef struct {
    unsigned char ch;
    unsigned char _unused;
    unsigned short int flags;
    unsigned int color;    // foreground | (gradient << 8) | (background << 16)
    float scale;
    float weight;
    float offset_x;
    float offset_y;
} TMCell;

// UI layer uses the same struct, aliased for semantic clarity.
typedef TMCell OutputCell;

// ---- helpers ----
static inline void cell_set(TMCell *c, unsigned char ch, unsigned int color,
                            float scale, float weight, float offset_x, float offset_y, int flags) {
    c->ch = ch;
    c->color = color;
    c->scale = scale;
    c->weight = weight;
    c->offset_x = offset_x;
    c->offset_y = offset_y;
    c->flags = flags;
}


// fg, bg are 8-bit intensity values (0-255)
// gradient is an 8-bit gradient ID (0=gray, 1=cold, 2=warm, 3=green)
#define TM_COLOR_PACK(fg, gradient, bg) \
    ((unsigned int)(unsigned char)(fg) | \
     ((unsigned int)(unsigned char)(gradient) << 8) | \
     ((unsigned int)(unsigned char)(bg) << 16))
#define TM_COLOR_FG(c)      ((unsigned char)((c) & 0xFF))
#define TM_COLOR_GRADIENT(c) ((unsigned char)(((c) >> 8) & 0xFF))
#define TM_COLOR_BG(c)      ((unsigned char)(((c) >> 16) & 0xFF))

// Convert 0xAARRGGBB foreground/background to packed format.
// Uses green channel as intensity, ignores alpha.
static inline unsigned int tm_color_from_argb(unsigned int fg_argb, unsigned int bg_argb) {
    unsigned char fg = (unsigned char)((fg_argb >> 8) & 0xFF);
    unsigned char bg = (unsigned char)((bg_argb >> 8) & 0xFF);
    unsigned char r = (unsigned char)((fg_argb >> 16) & 0xFF);
    unsigned char g = (unsigned char)((fg_argb >> 8) & 0xFF);
    unsigned char b = (unsigned char)(fg_argb & 0xFF);
    unsigned char gradient = 0;
    int dom = 30;
    if (     (int)b - (int)(r > g ? r : g) > dom) gradient = 1;  // cold
    else if ((int)r - (int)(g > b ? g : b) > dom) gradient = 2;  // warm
    else if ((int)g - (int)(r > b ? r : b) > dom) gradient = 3;  // green
    return TM_COLOR_PACK(fg, gradient, bg);
}
static inline unsigned int tm_color_from_argb_rawbg(unsigned int fg_argb, unsigned int bg, int threshold) {
    unsigned char fg = (unsigned char)((fg_argb >> 8) & 0xFF);
    unsigned char r = (unsigned char)((fg_argb >> 16) & 0xFF);
    unsigned char g = (unsigned char)((fg_argb >> 8) & 0xFF);
    unsigned char b = (unsigned char)(fg_argb & 0xFF);
    unsigned char gradient = 0;
    if(bg < threshold) { // avoid colors if background is too bright
        int dom = 30;
        if (     (int)b - (int)(r > g ? r : g) > dom) gradient = 1;  // cold
        else if ((int)r - (int)(g > b ? g : b) > dom) gradient = 2;  // warm
        else if ((int)g - (int)(r > b ? r : b) > dom) gradient = 3;  // green
    }
    return TM_COLOR_PACK(fg, gradient, bg);
}

#define TM_COLOR_FADE  (c, fade)      ((c&0xFFFFFF00) | ((int)TM_COLOR_BG(c) + ((        fade           * ((int)TM_COLOR_FG(c) - (int)TM_COLOR_BG(c)))>>8) ))
#define TM_COLOR_FADE_F(c, fade)      ((c&0xFFFFFF00) | ((int)TM_COLOR_BG(c) + (( ((int)(fade * 255.f)) * ((int)TM_COLOR_FG(c) - (int)TM_COLOR_BG(c)))>>8) ))

// Flags for tm_render / i8_to_argb
#define TM_FLAGS_SWAP_RED_BLUE_CHANNELS   (1 << 17)



#endif // TEXTMODE_CELL_H
