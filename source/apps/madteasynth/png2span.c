// png2span core; see png2span.h.

#include "png2span.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// STB_IMAGE_STATIC: nanovg.c links its own copy. STBI_WINDOWS_UTF8: dropped
// paths arrive as UTF-8.
#define STB_IMAGE_STATIC
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static char s_error[256];

const char *p2s_last_error(void) { return s_error; }

static void set_error(const char *what, const char *detail) {
    if (detail) snprintf(s_error, sizeof s_error, "%s: %s", what, detail);
    else        snprintf(s_error, sizeof s_error, "%s", what);
}

// ---------------------------------------------------------------- span list

typedef struct {
    P2SSpan *v;
    int count;
    int cap;
} SpanList;

static int span_add(SpanList *l, float x, float y, float len) {
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        P2SSpan *v = (P2SSpan *)realloc(l->v, (size_t)cap * sizeof(P2SSpan));
        if (!v) return 0;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->count].x = x;
    l->v[l->count].y = y;
    l->v[l->count].len = len;
    l->count++;
    return 1;
}

static void sort_by_x(P2SSpan *v, int count) {
    for (int i = 1; i < count; i++) {
        P2SSpan tmp = v[i];
        int j = i - 1;
        while (j >= 0 && v[j].x > tmp.x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = tmp;
    }
}

static int extract_spans(float *px, int w, int h, SpanList *out) {
    for (int y = 0; y < h; y++) {
        float *row = px + (size_t)y * (size_t)w;
        int row_start = out->count;

        for (int x = 0; x < w;) {
            if (row[x] != 1.0f) {
                x++;
                continue;
            }
            int e = x;
            while (e + 1 < w && row[e + 1] == 1.0f) e++;
            memset(row + x, 0, (size_t)(e - x + 1) * sizeof(float));

            float x0 = (float)x, x1 = (float)(e + 1);
            if (x > 0 && row[x - 1] > 0.0f) {
                x0 -= row[x - 1];
                row[x - 1] = 0.0f;
            }
            if (e + 1 < w && row[e + 1] > 0.0f) {
                x1 += row[e + 1];
                row[e + 1] = 0.0f;
            }
            if (!span_add(out, x0, (float)y, x1 - x0)) return 0;
            x = e + 2;
        }

        for (int x = 0; x < w;) {
            if (row[x] == 0.0f) {
                x++;
                continue;
            }
            int e = x;
            while (e + 1 < w && row[e + 1] != 0.0f) e++;
            if (e == x + 1) {
                float len = row[x] + row[e];
                if (!span_add(out, (float)(x + 1) - len * 0.5f, (float)y, len)) return 0;
            } else {
                for (int i = x; i <= e; i++)
                    if (!span_add(out, (float)i + 0.5f - row[i] * 0.5f, (float)y, row[i])) return 0;
            }
            x = e + 1;
        }

        sort_by_x(out->v + row_start, out->count - row_start);
    }
    return 1;
}

// -------------------------------------------------------------- image passes

static int has_color(const unsigned char *rgba, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = rgba + i * 4;
        if (p[3] && (p[0] != p[1] || p[1] != p[2])) return 1;
    }
    return 0;
}

// BT.601 luma, kept on the 0..255 grid so that white stays exactly 1.0.
static unsigned char luma(const unsigned char *p) {
    float y = 0.299f * (float)p[0] + 0.587f * (float)p[1] + 0.114f * (float)p[2];
    int v = (int)(y + 0.5f);
    return (unsigned char)(v > 255 ? 255 : v);
}

static int block_uniform(const unsigned char *rgba, int w, int f, int bx, int by) {
    const unsigned char *ref = rgba + ((size_t)by * (size_t)w + (size_t)bx) * 4;
    for (int y = by; y < by + f; y++)
        for (int x = bx; x < bx + f; x++)
            if (memcmp(rgba + ((size_t)y * (size_t)w + (size_t)x) * 4, ref, 4)) return 0;
    return 1;
}

// Largest factor the image is a pixel-exact nearest-neighbour blow-up of.
static int detect_res(const unsigned char *rgba, int w, int h) {
    static const int cand[] = { 32, 16, 8, 6, 4, 3, 2 };
    for (int c = 0; c < (int)(sizeof cand / sizeof cand[0]); c++) {
        int f = cand[c], ok = 1;
        if (w % f || h % f) continue;
        for (int by = 0; by < h && ok; by += f)
            for (int bx = 0; bx < w && ok; bx += f)
                ok = block_uniform(rgba, w, f, bx, by);
        if (ok) return f;
    }
    return 1;
}

static unsigned char *downscale(const unsigned char *rgba, int w, int h, int f) {
    int dw = w / f, dh = h / f;
    unsigned char *out = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            size_t src = ((size_t)(y * f + f / 2) * (size_t)w + (size_t)(x * f + f / 2)) * 4;
            memcpy(out + ((size_t)y * (size_t)dw + (size_t)x) * 4, rgba + src, 4);
        }
    return out;
}

void p2s_free(P2SResult *r) {
    if (!r) return;
    for (int ch = 0; ch < P2S_CHANNELS; ch++) {
        free(r->spans[ch]);
        r->spans[ch] = NULL;
        r->count[ch] = 0;
    }
}

int p2s_from_rgba(const unsigned char *rgba, int w, int h, int has_alpha, P2SResult *out) {
    memset(out, 0, sizeof *out);
    if (!rgba || w <= 0 || h <= 0) {
        set_error("empty image", NULL);
        return 0;
    }

    out->src_width = w;
    out->src_height = h;
    out->res = detect_res(rgba, w, h);

    unsigned char *small = NULL;
    if (out->res > 1) {
        small = downscale(rgba, w, h, out->res);
        if (small) {
            w /= out->res;
            h /= out->res;
        } else {
            out->res = 1;
        }
    }
    const unsigned char *img = small ? small : rgba;
    out->width = w;
    out->height = h;

    size_t n = (size_t)w * (size_t)h;
    float *px = (float *)malloc(n * sizeof(float));
    if (!px) {
        free(small);
        set_error("out of memory", NULL);
        return 0;
    }

    out->color = has_color(img, n);
    int ok = 1;
    for (int ch = out->color ? 0 : P2S_GRAY; ch < P2S_CHANNELS && ok; ch++) {
        for (size_t i = 0; i < n; i++) {
            const unsigned char *p = img + i * 4;
            float v = (float)(ch == P2S_GRAY ? luma(p) : p[ch]) * (1.0f / 255.0f);
            if (has_alpha) v *= (float)p[3] * (1.0f / 255.0f);
            px[i] = v;
        }
        SpanList list = { NULL, 0, 0 };
        ok = extract_spans(px, w, h, &list);
        out->spans[ch] = list.v;
        out->count[ch] = list.count;
    }

    free(px);
    free(small);
    if (!ok) {
        p2s_free(out);
        set_error("out of memory", NULL);
    }
    return ok;
}

int p2s_from_file(const char *path, P2SResult *out) {
    memset(out, 0, sizeof *out);
    int w = 0, h = 0, comp = 0;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba) {
        set_error(path, stbi_failure_reason());
        return 0;
    }
    int ok = p2s_from_rgba(rgba, w, h, comp == 2 || comp == 4, out);
    stbi_image_free(rgba);
    return ok;
}

int p2s_pack(const P2SSpan *s) {
    int x0 = (int)(s->x + 0.5f);
    int y = (int)(s->y + 0.5f);
    int len = (int)(s->len + 0.5f);
    if (len < 1) len = 1;
    if (x0 > 1023) x0 = 1023;
    if (y > 1023) y = 1023;
    if (len > 1023) len = 1023;
    return (x0 << 20) | (y << 10) | len;
}

// ------------------------------------------------------------ text emitters

typedef struct {
    char *text;
    size_t len, cap;
    int failed;
} Buf;

static void buf_add(Buf *b, const char *s) {
    if (b->failed) return;
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *t = (char *)realloc(b->text, cap);
        if (!t) { b->failed = 1; return; }
        b->text = t;
        b->cap = cap;
    }
    memcpy(b->text + b->len, s, n + 1);
    b->len += n;
}

static void buf_num(Buf *b, float v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%.5f", v);
    char *dot = strchr(tmp, '.');
    if (dot) {
        char *end = tmp + strlen(tmp) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (end == dot) *end = '\0';
    }
    buf_add(b, tmp);
}

static char *buf_finish(Buf *b) {
    if (b->failed) {
        free(b->text);
        set_error("out of memory", NULL);
        return NULL;
    }
    if (!b->text) buf_add(b, "");
    return b->text;
}

static const char *kChannelNames[P2S_CHANNELS] = { "R", "G", "B", "GRAY" };

char *p2s_emit_report(const P2SResult *r) {
    Buf b = { NULL, 0, 0, 0 };
    char tmp[64];
    snprintf(tmp, sizeof tmp, "RES=%d\n", r->res);
    buf_add(&b, tmp);

    int first = r->color ? P2S_R : P2S_GRAY;
    for (int ch = first; ch < P2S_CHANNELS; ch++) {
        buf_add(&b, kChannelNames[ch]);
        buf_add(&b, "=[");
        for (int i = 0; i < r->count[ch]; i++) {
            if (i) buf_add(&b, ", ");
            buf_num(&b, r->spans[ch][i].x);
            buf_add(&b, ",");
            buf_num(&b, r->spans[ch][i].y);
            buf_add(&b, ",");
            buf_num(&b, r->spans[ch][i].len);
        }
        buf_add(&b, "]\n");
    }
    for (int ch = first; ch < P2S_CHANNELS; ch++) {
        buf_add(&b, kChannelNames[ch]);
        buf_add(&b, "C=[");
        for (int i = 0; i < r->count[ch]; i++) {
            snprintf(tmp, sizeof tmp, "%s%d", i ? ", " : "", p2s_pack(&r->spans[ch][i]));
            buf_add(&b, tmp);
        }
        buf_add(&b, "]\n");
    }

    char mul[16];
    mul[0] = '\0';
    if (r->res > 1) snprintf(mul, sizeof mul, " * %d", r->res);
    buf_add(&b, "\n");
    buf_add(&b, r->color ? "// int rc[] = { RC };   // or GC / BC / GRAYC\n"
                         : "// int rc[] = { GRAYC };\n");
    buf_add(&b, "// int n = sizeof(rc) / sizeof(rc[0]), i = 0;\n");
    buf_add(&b, "// while (i < n) {\n");
    buf_add(&b, "//     int v = rc[i++];\n");
    snprintf(tmp, sizeof tmp, "//     int x = ((v >> 20) & 1023)%s,", mul);
    buf_add(&b, tmp);
    snprintf(tmp, sizeof tmp, " y = ((v >> 10) & 1023)%s,", mul);
    buf_add(&b, tmp);
    snprintf(tmp, sizeof tmp, " len = (v & 1023)%s;\n", mul);
    buf_add(&b, tmp);
    buf_add(&b, "//     line(x, y, x + len, y);\n");
    buf_add(&b, "// }\n");
    return buf_finish(&b);
}

// The GRAY list as codesynth source: packed spans plus a for-in loop drawing them.
char *p2s_emit_snippet(const P2SResult *r, const char *label) {
    Buf b = { NULL, 0, 0, 0 };
    char tmp[256];
    int count = r->count[P2S_GRAY];

    snprintf(tmp, sizeof tmp, "// %s %dx%d, res %d, %d spans\n",
             label ? label : "image", r->src_width, r->src_height, r->res, count);
    buf_add(&b, tmp);

    buf_add(&b, "spans = [");
    for (int i = 0; i < count; i++) {
        snprintf(tmp, sizeof tmp, "%s%d", i ? "," : "", p2s_pack(&r->spans[P2S_GRAY][i]));
        buf_add(&b, tmp);
    }
    if (!count) buf_add(&b, "0"); // an empty array literal has no type to infer
    buf_add(&b, "]\n");

    char mul[16];
    mul[0] = '\0';
    if (r->res > 1) snprintf(mul, sizeof mul, " * %d", r->res);

    buf_add(&b, "for span in spans\n");
    snprintf(tmp, sizeof tmp, "    spanx = ((span >> 20) & 1023)%s\n", mul);
    buf_add(&b, tmp);
    snprintf(tmp, sizeof tmp, "    spany = ((span >> 10) & 1023)%s\n", mul);
    buf_add(&b, tmp);
    snprintf(tmp, sizeof tmp, "    spanw = ( span        & 1023)%s\n", mul);
    buf_add(&b, tmp);
    buf_add(&b, "    line(spanx, spany, spanx + spanw, spany, 1)\n");
    return buf_finish(&b);
}

void p2s_free_text(char *text) { free(text); }

void p2s_report_path(char *dst, size_t size, const char *src) {
    const char *dot = strrchr(src, '.');
    const char *slash = strrchr(src, '/');
    const char *back = strrchr(src, '\\');
    if (!slash || (back && back > slash)) slash = back;
    if (dot && (!slash || dot > slash)) {
        size_t stem = (size_t)(dot - src);
        if (stem >= size) stem = size - 1;
        memcpy(dst, src, stem);
        dst[stem] = '\0';
    } else {
        snprintf(dst, size, "%s", src);
    }
    size_t used = strlen(dst);
    snprintf(dst + used, size - used, "_spans.txt");
}
