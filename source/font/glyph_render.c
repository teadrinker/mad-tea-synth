#include "glyph_render.h"
#include "common/math_pure.h"
#include "common/render_polygon.h"
#include "common/render_aaline.h"

#define GLYPH_MAX_SEGS  128

// todo: end vs start, try to avoid double code?
// todo: export lines

typedef struct { float x, y; } FVec2;

// The scanline rasterizer lives in common/render_polygon.h; this adapts it to Tsys by
// supplying the scratch buffers.
static void gr_rasterize_alloc(Tsys *sys, const GrPath* p, int width, int height,
                               unsigned char* output) {
    if (p->edge_count == 0) return;

    GrRasterBuffers buf;
    buf.coverage      = (int*)sys->malloc((size_t)width * sizeof(int));
    buf.edge_order    = (int*)sys->malloc((size_t)p->edge_count * sizeof(int));
    buf.active_x      = (long long*)sys->malloc((size_t)POLY_SUBSAMPLES * POLY_MAX_CROSSINGS * sizeof(long long));
    buf.active_step   = (long long*)sys->malloc((size_t)POLY_SUBSAMPLES * POLY_MAX_CROSSINGS * sizeof(long long));
    buf.active_end    = (int*)sys->malloc((size_t)POLY_SUBSAMPLES * POLY_MAX_CROSSINGS * sizeof(int));
#if POLY_UNION_FILL
    buf.active_dir    = (int*)sys->malloc((size_t)POLY_SUBSAMPLES * POLY_MAX_CROSSINGS * sizeof(int));
#endif
    buf.active_count  = (int*)sys->malloc((size_t)POLY_SUBSAMPLES * sizeof(int));
    buf.next_idx      = (int*)sys->malloc((size_t)POLY_SUBSAMPLES * sizeof(int));
#if POLY_BUCKET_SORT
    buf.row_start     = (int*)sys->malloc((size_t)(height + 2) * sizeof(int));
#endif
    // Optional (gr_rasterize divides inline without it), so a null from malloc
    // costs speed and nothing else -- hence its absence from the check below.
    buf.edge_step     = (long long*)sys->malloc((size_t)p->edge_count * sizeof(long long));

    if (buf.coverage && buf.edge_order && buf.active_x && buf.active_step && buf.active_end
#if POLY_UNION_FILL
        && buf.active_dir
#endif
        && buf.active_count && buf.next_idx
        ) {
        RSurface surf = rs_tile(output, width, height);
        // 65536 == 1.0 in gr_rasterize's Q16.16 alpha: the float glyph path has
        // no per-draw coverage of its own (see the aaline_polygon call below,
        // which passes FIXED_ONE for the same reason).
        gr_rasterize(p, width, height, &surf, 0, 0, 0, 65536, &buf);
    }

    sys->free(buf.coverage);
    sys->free(buf.edge_order);
    sys->free(buf.active_x);
    sys->free(buf.active_step);
    sys->free(buf.active_end);
#if POLY_UNION_FILL
    sys->free(buf.active_dir);
#endif
    sys->free(buf.active_count);
    sys->free(buf.next_idx);
#if POLY_BUCKET_SORT
    sys->free(buf.row_start);
#endif
    sys->free(buf.edge_step);
}

// Stroke geometry core (line_isect, build_smoothed, flatten_smoothed,
// stroke_build_outline), shared with render_lowspec.c; this TU takes the
// default float config.
#include "glyph_render_core.h"

// Scratch for the geometry temporaries (glyph_render_core.h's scratch_alloc).
// Desktop has no static-footprint budget, so this is one generously-sized
// buffer reused across calls rather than a threaded parameter -- ~289 KB at
// the default maxima, 320 KB for headroom.
#define GLYPH_RENDER_SCRATCH_BYTES (320 * 1024)
static char s_glyph_render_scratch[GLYPH_RENDER_SCRATCH_BYTES];

// One assembled outline contour, sized for the worst case (see
// RGC_OUTLINE_MAX in glyph_render_core.h). stroke_build_outline builds into
// this; the fill path then feeds it to a GrPath via gr_add_contour, and the
// capture path copies it into the GlyphOutline. Reused across subpaths/calls.
static FVec2 s_glyph_contour[RGC_OUTLINE_MAX];

// Q16.16 conversion scratch for aa_add_contour below -- interleaved
// (x0,y0,x1,y1,...) pairs, the layout common/render_aaline.h's
// aaline_polygon expects. Same sizing/reuse rationale as s_glyph_contour.
static fixed s_glyph_contour_fx[RGC_OUTLINE_MAX * 2];

// RG_FLAGS_RENDER_LINE_OUTLINE's counterpart to gr_add_contour: stroke the
// same (open) contour as closed AA lines at full alpha via the shared
// aaline_polygon (common/render_aaline.h) -- render_lowspec.c's fixed-
// point twin reinterprets its contour array directly instead of converting,
// since fx16 there already *is* Q16.16.
static void aa_add_contour(RSurface* surf, const FVec2* contour, int n) {
    if (n < 2) return;
    for (int i = 0; i < n; i++) {
        s_glyph_contour_fx[2*i]     = RGC_FX(contour[i].x);
        s_glyph_contour_fx[2*i + 1] = RGC_FX(contour[i].y);
    }
    // Full alpha, default blend: render_glyph takes neither (the desktop TU's
    // fill twin, gr_rasterize_alloc, has no alpha either), so there is nothing
    // to forward -- unlike the fixed-point TU, where render_line/render_ellipse
    // hand their own alpha/blend straight through.
    aaline_polygon(surf, s_glyph_contour_fx, n, 0, 0, FIXED_ONE, 0);
}

// Build outline polygon for a stroke subpath and add edges to the path.
// When a rounded corner has half_width >= corner radius ("degraded"),
// the centerline is kept sharp and a round arc is emitted on the outer
// side of the stroke at that corner, while the inner side gets a miter join.
//

// Outline point with quadratic control flag (TrueType convention):
// on-curve(is_ctrl=0) -> on-curve(is_ctrl=0)  = line segment
// on-curve(is_ctrl=0) -> ctrl(is_ctrl=1) -> on-curve = quadratic Bezier
typedef struct { FVec2 pt; int is_ctrl; } NqOutPt;

#define NQ_FX(v) ((int)((v) * 65536.0f))

// Compute offset quadratic control point (used by DONT_FLATTEN_QUADRATICS path).
// Given centre-quad P0->C->P2 with unit start/end tangents T0/T2, offset
// by distance hw on the side indicated by sign (+1=left, -1=right).
static FVec2 nq_offset_ctrl_file(FVec2 P0, FVec2 C, FVec2 P2,
                                  FVec2 T0, FVec2 T2, float hw, float sign) {
    FVec2 N0 = { -T0.y * sign,  T0.x * sign };
    FVec2 N2 = { -T2.y * sign,  T2.x * sign };
    FVec2 O0 = { P0.x + hw * N0.x, P0.y + hw * N0.y };
    FVec2 O2 = { P2.x + hw * N2.x, P2.y + hw * N2.y };
    FVec2 D0 = { C.x - P0.x, C.y - P0.y };
    FVec2 D1 = { P2.x - C.x,  P2.y - C.y };
    float det = D0.x * D1.y - D0.y * D1.x;
    if (m_fabsf(det) < 1e-6f)
        return (FVec2){ C.x + hw * (N0.x + N2.x) * 0.5f,
                        C.y + hw * (N0.y + N2.y) * 0.5f };
    float t = ((O2.x - O0.x) * D1.y - (O2.y - O0.y) * D1.x) / det;
    return (FVec2){ O0.x + t * D0.x, O0.y + t * D0.y };
}

// Emit an outline (NqOutPt array) to GrPath. forward=0 reverses; do_moveto=0
// continues the current subpath (right side). Quadratics subdivided here.
static void nq_emit_file(GrPath *path, NqOutPt *arr, int cnt, int forward, int do_moveto) {
    if (cnt < 2) return;
    if (forward) {
        if (do_moveto)
            gr_move_to(path, NQ_FX(arr[0].pt.x), NQ_FX(arr[0].pt.y));
        else
            gr_line_to(path, NQ_FX(arr[0].pt.x), NQ_FX(arr[0].pt.y));
        int i = 1;
        while (i < cnt) {
            if (arr[i].is_ctrl && i + 1 < cnt) {
                FVec2 p0 = arr[i-1].pt, cp = arr[i].pt, p1 = arr[i+1].pt;
                for (int s = 1; s <= 4; s++) {
                    float t = (float)s / 4.0f, mt = 1.0f - t;
                    gr_line_to(path,
                        NQ_FX(mt*mt*p0.x + 2*mt*t*cp.x + t*t*p1.x),
                        NQ_FX(mt*mt*p0.y + 2*mt*t*cp.y + t*t*p1.y));
                }
                i += 2;
            } else {
                gr_line_to(path, NQ_FX(arr[i].pt.x), NQ_FX(arr[i].pt.y));
                i++;
            }
        }
    } else {
        if (do_moveto)
            gr_move_to(path, NQ_FX(arr[cnt-1].pt.x), NQ_FX(arr[cnt-1].pt.y));
        else
            gr_line_to(path, NQ_FX(arr[cnt-1].pt.x), NQ_FX(arr[cnt-1].pt.y));
        int i = cnt - 2;
        while (i >= 0) {
            if (i >= 1 && arr[i].is_ctrl) {
                FVec2 p0 = arr[i+1].pt, cp = arr[i].pt, p1 = arr[i-1].pt;
                for (int s = 1; s <= 4; s++) {
                    float t = (float)s / 4.0f, mt = 1.0f - t;
                    gr_line_to(path,
                        NQ_FX(mt*mt*p0.x + 2*mt*t*cp.x + t*t*p1.x),
                        NQ_FX(mt*mt*p0.y + 2*mt*t*cp.y + t*t*p1.y));
                }
                i -= 2;
            } else {
                gr_line_to(path, NQ_FX(arr[i].pt.x), NQ_FX(arr[i].pt.y));
                i--;
            }
        }
    }
}

// Compute the scaled centerline for one subpath and build its outline
// contour into out_contour[] (capacity RGC_OUTLINE_MAX). Returns the vertex
// count (0 if the subpath is too short). Pure geometry -- no path, no fill.
static int stroke_subpath_outline(FVec2* out_contour, int max_contour,
                                 const GlyphSegment* segs, int start, int end,
                                 float half_width, const FONT_SETTINGS *settings,
                                 float scale_x, float scale_y,
                                 float ox, float oy, int flags) {
    float center_scale = scale_y;

    float box_center_x = settings->glyph_box_center[0];
    float box_center_y = settings->glyph_box_center[1];
    float box_default_hw  = settings->glyph_box_default_hw;
    float glyph_hw = half_width / scale_x;

    if(!(flags & RG_FLAGS_IGNORE_GRADE_FIT)) {

        //float box_scale_x = (settings->glyph_box_hsize[0] - glyph_hw) / (settings->glyph_box_hsize[0] - box_default_hw);
        float box_scale_y = (settings->glyph_box_hsize[1] - glyph_hw) / (settings->glyph_box_hsize[1] - box_default_hw);
        scale_x *= box_scale_y;
        scale_y *= box_scale_y;        
    }
    
    float uniform_scale = scale_y;
    //float uniform_scale = (scaleCoordsX + scaleCoordsY) * 0.5f;

    float global_radius        = settings->radius;
    float global_small_radius  = settings->small_radius_mul  * global_radius;
    float global_medium_radius = settings->medium_radius_mul * global_radius;

    // Interpolate width multipliers: lerp(1, mul, t) where t = smoothstep(range_start, range_end, half_width/scale_x)
    float hw_range = settings->half_width_settings_range_end - settings->half_width_settings_range_start;
    float hw_t = (hw_range > 0.001f) ? (glyph_hw - settings->half_width_settings_range_start) / hw_range : 1.0f;
    if      (hw_t < 0.0f) hw_t = 0.0f;
    else if (hw_t > 1.0f) hw_t = 1.0f;

    float wmul[16];
    wmul[0] = half_width;
    for(int i = 0; i < 15; i++) {
        wmul[i + 1] = (1.0f - hw_t + hw_t * settings->width_mul[i + 1]) * half_width;
    }

    int n_orig = end - start;
    if (n_orig < 2) return 0;

    int start_cap = GLYPH_FLAG_CAP(segs[start].flags);
    int end_cap   = GLYPH_FLAG_CAP(segs[end - 1].flags);

    float scaled_radius = global_radius * uniform_scale;
    float scaled_small_radius = global_small_radius * uniform_scale;
    float scaled_medium_radius = global_medium_radius * uniform_scale;

    FVec2 pts[GLYPH_MAX_SEGS];
    int move_id = 0;
    int pt_flags[GLYPH_MAX_SEGS];
    for (int i = 0; i < n_orig; i++) {
        float x = segs[start + i].x;
        float y = segs[start + i].y;
        int flags = segs[start + i].flags;
        if((move_id = GLYPH_FLAG_MOVE_DESC(flags)) != 0) {
            x += settings->move_desc_x[move_id] * settings->descend;
            y += settings->move_desc_y[move_id] * settings->descend;
        }
        if((move_id = GLYPH_FLAG_MOVE_GRID(flags)) != 0) {
            x += settings->move_grid_x[move_id] * settings->optical_size;
            y += settings->move_grid_y[move_id] * settings->optical_size;
        }

        pts[i].x = ox + scale_x * (x - box_center_x) + center_scale * box_center_x;
        pts[i].y = oy + scale_y * (y - box_center_y) + center_scale * box_center_y;
        pt_flags[i] = flags;
    }

    // Per-point half_width for outline offsets
    float pt_hw[GLYPH_MAX_SEGS];
    for (int i = 0; i < n_orig; i++)
        pt_hw[i] = wmul[GLYPH_FLAG_WIDTH(pt_flags[i])];

    return stroke_build_outline(out_contour, max_contour,
                         pts, pt_flags, pt_hw, n_orig, start_cap, end_cap,
                         scaled_radius, scaled_small_radius, scaled_medium_radius, half_width,
                         s_glyph_render_scratch, s_glyph_render_scratch + GLYPH_RENDER_SCRATCH_BYTES);
}

// ===================================================================
// Public API
// ===================================================================

void render_glyph(Tsys *sys, const GLYPH_DATA *data, int width, int height,
                  unsigned char* grayscale_output,
                  const FONT_SETTINGS *settings,
                  float strokewidth,
                  float scaleCoordsX, float scaleCoordsY,
                  float offsetX, float offsetY, int flags) {
    sys->memset(grayscale_output, 0, width * height);

    float half_width = strokewidth * 0.5f;

    // Wireframe mode strokes each contour with AA lines as it is assembled;
    // the fill mode instead accumulates every contour into one path and
    // rasterizes once at the end (winding rules need the whole path).
    int line_outline = (flags & RG_FLAGS_RENDER_LINE_OUTLINE) != 0;
    RSurface surf = rs_tile(grayscale_output, width, height);

    GrPath path;
    gr_path_init(&path);

    // Iterate subpaths
    int i = 0;
    while (i < data->count) {
        if (data->segs[i].type != SEG_LINE_MOVE) { i++; continue; }
        int start = i;
        i++;
        while (i < data->count && data->segs[i].type == SEG_LINE_TO) i++;
        int end = i;

        if (half_width > 1.f / 256.f) {
            int n = stroke_subpath_outline(s_glyph_contour, RGC_OUTLINE_MAX,
                                           data->segs, start, end,
                                           half_width, settings,
                                           scaleCoordsX, scaleCoordsY,
                                           offsetX, offsetY, flags);
            if (line_outline) aa_add_contour(&surf, s_glyph_contour, n);
            else              gr_add_contour(&path, s_glyph_contour, n);
        }
    }

    if (!line_outline)
        gr_rasterize_alloc(sys, &path, width, height, grayscale_output);
}

void font_settings_free_glyphs(Tsys *sys, FONT_SETTINGS *fs) {
    if (!sys || !fs) return;
    if (fs->glyph_segment_data) {
        sys->free(fs->glyph_segment_data);
        fs->glyph_segment_data = NULL;
    }
    if (fs->glyphs) {
        sys->free(fs->glyphs);
        fs->glyphs = NULL;
    }
    fs->glyph_count = 0;
}

// ===================================================================
// Vector outline capture helpers
// ===================================================================

int glyph_outline_init(Tsys *sys, GlyphOutline *ol, int max_points) {
    if (!ol || max_points <= 0) return 0;
    ol->points = (GlyphOutlinePt*)sys->malloc(max_points * sizeof(GlyphOutlinePt));
    if (!ol->points) return 0;
    ol->num_contours = 0;
    ol->total_points = 0;
    ol->max_points = max_points;
    ol->own_allocation = 1;
    return 1;
}

void glyph_outline_free(Tsys *sys, GlyphOutline *ol) {
    if (!ol || !ol->points) return;
    if (ol->own_allocation) sys->free(ol->points);
    ol->points = NULL;
    ol->num_contours = 0;
    ol->total_points = 0;
    ol->max_points = 0;
    ol->own_allocation = 0;
}

void glyph_outline_begin_contour(GlyphOutline *ol) {
    if (!ol || ol->num_contours >= GLYPH_OUTLINE_MAX_CONTOURS) return;
    ol->contour_start[ol->num_contours] = ol->total_points;
    ol->contour_len[ol->num_contours] = 0;
    ol->num_contours++;  // now active — add_point will write to contour[num_contours-1]
}

void glyph_outline_add_point(GlyphOutline *ol, float x, float y) {
    if (!ol || !ol->points || ol->total_points >= ol->max_points) return;
    if (ol->num_contours == 0) return;  // no active contour
    int ci = ol->num_contours - 1;
    ol->points[ol->total_points].x = x;
    ol->points[ol->total_points].y = y;
    ol->total_points++;
    ol->contour_len[ci]++;
}

void glyph_outline_end_contour(GlyphOutline *ol) {
    if (!ol) return;
    // Contour is already finalized by begin_contour — nothing to do.
    // The closing point (back to first) is already emitted by the caller as a line_to.
}

// render_glyph_outline -- vector capture variant. Same geometry as
// render_glyph, but each subpath's outline contour is copied into the
// GlyphOutline (closed) instead of rasterized. No GrPath, no rasterizer.
void render_glyph_outline(Tsys *sys, const GLYPH_DATA *data,
                          GlyphOutline *outline,
                          const FONT_SETTINGS *settings,
                          float strokewidth,
                          float scaleCoordsX, float scaleCoordsY,
                          float offsetX, float offsetY, int flags) {
    (void)sys;
    if (!outline || !outline->points) return;

    // Reset outline counters (keep allocation)
    outline->num_contours = 0;
    outline->total_points = 0;

    float half_width = strokewidth * 0.5f;

    // Iterate subpaths (same loop as render_glyph)
    int i = 0;
    while (i < data->count) {
        if (data->segs[i].type != SEG_LINE_MOVE) { i++; continue; }
        int start = i;
        i++;
        while (i < data->count && data->segs[i].type == SEG_LINE_TO) i++;
        int end = i;

        if (half_width <= 1.f / 256.f) continue;
        if (outline->num_contours >= GLYPH_OUTLINE_MAX_CONTOURS) break;

        int n = stroke_subpath_outline(s_glyph_contour, RGC_OUTLINE_MAX,
                                       data->segs, start, end,
                                       half_width, settings,
                                       scaleCoordsX, scaleCoordsY,
                                       offsetX, offsetY, flags);
        if (n < 3) continue;  // drop degenerate/empty contour (e.g. too-short subpath)

        // stroke_build_outline produced an *open* contour (no closing
        // duplicate). The export toolchain consumes each contour through a
        // point pen whose first point is segmentType="move" -- an open UFO
        // contour -- so repeat the first point at the end to close the loop.
        // (The core component stays open; this consumer closes it.)
        int cstart = outline->total_points;
        if (cstart + n + 1 > outline->max_points) break;  // no room for contour + closing point
        for (int p = 0; p < n; p++) {
            outline->points[cstart + p].x = s_glyph_contour[p].x;
            outline->points[cstart + p].y = s_glyph_contour[p].y;
        }
        outline->points[cstart + n].x = s_glyph_contour[0].x;  // closing point
        outline->points[cstart + n].y = s_glyph_contour[0].y;
        n++;

        outline->contour_start[outline->num_contours] = cstart;
        outline->contour_len[outline->num_contours]   = n;
        outline->num_contours++;
        outline->total_points += n;
    }
}

int glyph_build_smoothed(const GlyphSegment* segs, int start, int count,
                          const FONT_SETTINGS *settings,
                          float scale,
                          float half_width,
                          float offset_x, float offset_y,
                          GlyphSmoothPt* out, int max_out) {
    FVec2 pts[GLYPH_MAX_SEGS];
    int pt_flags[GLYPH_MAX_SEGS];
    int n = count;
    if (n < 1) return 0;

    for (int i = 0; i < n && i < GLYPH_MAX_SEGS; i++) {
        pts[i].x = offset_x + segs[start + i].x * scale;
        pts[i].y = offset_y + segs[start + i].y * scale;
        pt_flags[i] = segs[start + i].flags;
    }

    float scaled_radius = settings->radius * scale;
    float scaled_small_radius = settings->small_radius_mul * settings->radius * scale;
    float scaled_medium_radius = settings->medium_radius_mul * settings->radius * scale;
    float scaled_half_width = half_width * scale;

    FVec2 smooth_pts[GLYPH_SMOOTH_MAX];
    int smooth_ctrl[GLYPH_SMOOTH_MAX];
    int sn = build_smoothed(pts, pt_flags, n, scaled_radius, scaled_small_radius, scaled_medium_radius,
                            scaled_half_width, smooth_pts, smooth_ctrl, NULL, NULL,
                            GLYPH_SMOOTH_MAX,
                            s_glyph_render_scratch, s_glyph_render_scratch + GLYPH_RENDER_SCRATCH_BYTES);

    int out_n = sn < max_out ? sn : max_out;
    for (int i = 0; i < out_n; i++) {
        out[i].x = smooth_pts[i].x;
        out[i].y = smooth_pts[i].y;
        out[i].is_quad_control = smooth_ctrl[i];
    }
    return out_n;
}
