// png2span: turn an image into scanline spans.
// Each channel is treated as a normalized grayscale image:
//   1. runs of fully lit pixels become spans, and are cleared
//   2. a non-zero pixel touching a span extends it by its own value (50% gray ->
//      half a pixel), and is cleared
//   3. what is left becomes pixel-centered spans; an isolated pair of
//      neighbours merges into one span (0,.5,.5,0 -> x 1.5, length 1)
// GRAY is a fourth run over BT.601 luma; a colourless image only fills GRAY.
// First the image is reduced by the largest factor it is an exact
// nearest-neighbour blow-up of (`res`).

#ifndef PNG2SPAN_H
#define PNG2SPAN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y, len;
} P2SSpan;

enum { P2S_R = 0, P2S_G = 1, P2S_B = 2, P2S_GRAY = 3, P2S_CHANNELS = 4 };

typedef struct {
    P2SSpan *spans[P2S_CHANNELS];
    int      count[P2S_CHANNELS];
    int      width, height;         // resolution the spans are in
    int      src_width, src_height; // the image's own size
    int      res;                   // width * res == src_width
    int      color;                 // 0 = no color in the source, only GRAY filled
} P2SResult;

// 1 on success, 0 on failure (p2s_last_error). With `has_alpha`, the colour
// channels are premultiplied by it.
int  p2s_from_rgba(const unsigned char *rgba, int w, int h, int has_alpha, P2SResult *out);
int  p2s_from_file(const char *path, P2SResult *out);
void p2s_free(P2SResult *r);

const char *p2s_last_error(void);

// x, y and length in 10 bits each, rounded independently so a sub-pixel span
// keeps its width.
int p2s_pack(const P2SSpan *s);

// malloc'd strings, or NULL. `label` goes in the snippet's header comment.
char *p2s_emit_report(const P2SResult *r);            // the RES=/R=/RC=... text
char *p2s_emit_snippet(const P2SResult *r, const char *label); // codesynth source
void  p2s_free_text(char *text);

// "dir/foo.png" -> "dir/foo_spans.txt"
void p2s_report_path(char *dst, size_t size, const char *src);

#ifdef __cplusplus
}
#endif

#endif
