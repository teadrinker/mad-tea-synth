#ifndef RENDER_LAYOUT_H
#define RENDER_LAYOUT_H

// ===================================================================
// RLayout -- how a destination byte splits into colour channels, chosen at
// runtime by color_ramp_setup(add, bit_offset, low, mid, high). The palette
// is the caller's business; this only says which byte means which level.
//
//   P    = the channels packed low first (`low` in the lowest bits)
//   byte = add + (P << bit_offset)
//   P    = (((byte - add) & 0xFF) >> bit_offset) & pmask        (decode)
//
// The decode is modular, not clamped: off-ramp bytes floor or wrap, and for
// (192,0,2,2,2) it reduces to `byte & 0x3F`, exactly how the RS_PEBBLE_TIME2
// path reads a GColor8 pixel.
//
// Only TUs built without RS_PEBBLE_TIME2 read it (RSurface::layout). A NULL
// layout, or (0,0,8), is plain 8-bit gray and takes the gray path unchanged.
//
// Layouts with two or more channels must keep `add` off the channel bits, so
// a channel lock can be a byte mask (see rb_px_ramp).
// ===================================================================

#define RLAYOUT_MAX_CH 3

typedef struct RLayout {
    int key;                                // 0 until rlayout_build
    unsigned char add, off;
    unsigned char bits[RLAYOUT_MAX_CH];     // low, mid, high; 0 = absent
    unsigned char nch, gray8, max_bits;
    unsigned char pmask;
    unsigned char lut_mask;                 // Tier B index mask: pmask if byte & pmask decodes, else 0xFF
    unsigned char pshift[RLAYOUT_MAX_CH];   // channel position within P
    unsigned char chmask[RLAYOUT_MAX_CH];   // channel bits within the byte
    unsigned char dec8[RLAYOUT_MAX_CH][256];// byte -> channel level as 0..255
    unsigned char enc8[256];                // 0..255 gray -> byte, every channel rounded
} RLayout;

static inline int rlayout_valid(int add, int off, int b0, int b1, int b2) {
    int sum, pmax;
    if (add < 0 || add > 255 || off < 0 || off > 7) return 0;
    if (b0 < 1 || b1 < 0 || b2 < 0) return 0;
    if (b2 > 0 && b1 == 0) return 0;
    sum = b0 + b1 + b2;
    if (off + sum > 8) return 0;
    pmax = (1 << sum) - 1;
    if (add + (pmax << off) > 255) return 0;
    if (b1 > 0 && (add & (pmax << off)) != 0) return 0;
    return 1;
}

static inline int rlayout_key_of(int add, int off, int b0, int b1, int b2) {
    return (1 << 24) | add | (off << 8) | (b0 << 11) | (b1 << 15) | (b2 << 19);
}

// Builds the tables from add/off/bits, which must be valid. An all-zero
// RLayout builds as (192,0,2,2,2), the GColor8 cube.
static inline void rlayout_build(RLayout *l) {
    int ch, b, v, sum = 0;
    if (!l->bits[0]) {
        l->add = 192;
        l->off = 0;
        l->bits[0] = l->bits[1] = l->bits[2] = 2;
    }
    l->nch = 0;
    l->max_bits = 0;
    for (ch = 0; ch < RLAYOUT_MAX_CH; ch++) {
        if (!l->bits[ch]) break;
        l->pshift[ch] = (unsigned char)sum;
        sum += l->bits[ch];
        if (l->bits[ch] > l->max_bits) l->max_bits = l->bits[ch];
        l->nch++;
    }
    for (; ch < RLAYOUT_MAX_CH; ch++) { l->bits[ch] = 0; l->pshift[ch] = 0; }
    l->pmask    = (unsigned char)((1 << sum) - 1);
    l->gray8    = (unsigned char)(l->nch == 1 && sum == 8 && l->add == 0 && l->off == 0);
    l->lut_mask = (unsigned char)((l->off == 0 && (l->add & l->pmask) == 0) ? l->pmask : 0xFF);
    for (ch = 0; ch < RLAYOUT_MAX_CH; ch++) {
        int lmax = (1 << l->bits[ch]) - 1;
        l->chmask[ch] = (unsigned char)((lmax << l->pshift[ch]) << l->off);
    }
    for (b = 0; b < 256; b++) {
        int p = (((b - l->add) & 0xFF) >> l->off) & l->pmask;
        for (ch = 0; ch < RLAYOUT_MAX_CH; ch++) {
            int lmax = (1 << l->bits[ch]) - 1;
            int lvl  = lmax ? (p >> l->pshift[ch]) & lmax : 0;
            l->dec8[ch][b] = (unsigned char)(lmax ? (lvl * 255 + lmax / 2) / lmax : 0);
        }
    }
    for (v = 0; v < 256; v++) {
        int p = 0;
        for (ch = 0; ch < l->nch; ch++) {
            int lmax = (1 << l->bits[ch]) - 1;
            p |= ((v * lmax + 127) / 255) << l->pshift[ch];
        }
        l->enc8[v] = (unsigned char)(l->add + (p << l->off));
    }
    l->key = rlayout_key_of(l->add, l->off, l->bits[0], l->bits[1], l->bits[2]);
}

// 1 and the layout rebuilt (or kept, if unchanged), or 0 and untouched.
static inline int rlayout_set(RLayout *l, int add, int off, int b0, int b1, int b2) {
    if (!rlayout_valid(add, off, b0, b1, b2)) return 0;
    if (l->key && l->key == rlayout_key_of(add, off, b0, b1, b2)) return 1;
    l->add = (unsigned char)add;
    l->off = (unsigned char)off;
    l->bits[0] = (unsigned char)b0;
    l->bits[1] = (unsigned char)b1;
    l->bits[2] = (unsigned char)b2;
    rlayout_build(l);
    return 1;
}

// Byte bits a draw with these lock flags may NOT write (0 for one channel).
// Lock bit 2 << ch locks channel ch, low first.
static inline int rlayout_locked_bits(const RLayout *l, int lock_flags) {
    int ch, m = 0;
    if (l->nch < 2) return 0;
    for (ch = 0; ch < l->nch; ch++)
        if (lock_flags & (2 << ch)) m |= l->chmask[ch];
    return m;
}

#endif // RENDER_LAYOUT_H
