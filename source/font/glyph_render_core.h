// glyph_render_core.h -- stroke-outline geometry core, shared by the float
// (glyph_render.c) and fixed-point (render_lowspec.c) renderers.
//
// A textual template with NO include guard: #include once per TU, each with a
// different RGC_FLOAT config (a second include redefines everything). Before
// including, the includer must:
//   1. Optionally #define the RGC_* macros below (defaults = float).
//   2. Provide a type `FVec2` with `RGC_FLOAT x, y;` (each TU defines its own).
//   3. For fixed point, #include "common/math_fixedp.h" first.
//
// Contains: line_isect, build_smoothed, flatten_smoothed, stroke_build_outline.

#include <stdint.h>
#include <stddef.h>
#include "common/math_pure.h"
#include "glyph_render.h"
#include "common/render_polygon.h"

#ifndef GLYPH_MAX_SEGS
#define GLYPH_MAX_SEGS  128
#endif

// Bezier-flattening headroom: flatten_smoothed turns each control-point pair
// into up to 8 points, so flat[]/etc are sized GLYPH_SMOOTH_MAX*this. Safe
// below 8 only when corner rounding is disabled (radius==0, so no control
// points) -- see font_lowspec_pebble.c.
#ifndef RGC_FLATTEN_MUL
#define RGC_FLATTEN_MUL 8
#endif

// Capacity of flat[] and everything indexed by it. Default
// GLYPH_SMOOTH_MAX*RGC_FLATTEN_MUL; override to a measured tighter bound
// (font_lowspec_pebble.c uses an exact 124).
#ifndef RGC_FLAT_MAX
#define RGC_FLAT_MAX (GLYPH_SMOOTH_MAX * RGC_FLATTEN_MUL)
#endif

// MAX_OUTLINE's degraded-corner arc margin, decoupled from ARC_STEPS (arc
// quality, must stay 8). Override below 8 only if half_width < every corner
// radius (then the degraded-arc path is dead) -- see font_lowspec_pebble.c.
#ifndef RGC_ARC_MARGIN
#define RGC_ARC_MARGIN 8   /* == ARC_STEPS */
#endif

// Quadratic-bezier flattening subdivision count in flatten_smoothed. Default is
// a fixed 8 segments per rounded corner (historical). Lower RGC_BEZIER_STEPS to
// trade curve smoothness for fewer output vertices -> fewer rasterizer edges.
// Or define RGC_BEZIER_ADAPTIVE to pick the count per corner from its on-screen
// chord deviation (a big corner gets more segments than a tiny one), capped at
// RGC_BEZIER_MAX_STEPS. The flat[] buffer headroom is sized off *_MAX_STEPS, so
// keep it >= any per-corner count actually produced (and see RGC_FLAT_MAX).
#ifndef RGC_BEZIER_STEPS
#define RGC_BEZIER_STEPS 8
#endif
#ifndef RGC_BEZIER_MAX_STEPS
#define RGC_BEZIER_MAX_STEPS 8
#endif
// Flatness tolerance (in destination pixels) for RGC_BEZIER_ADAPTIVE.
#ifndef RGC_BEZIER_TOL
#define RGC_BEZIER_TOL RGC_CONST(0.2f)
#endif

// Worst-case vertex count of one assembled contour (both sides + both cap
// arcs). stroke_build_outline's out_contour must be at least this large (it
// builds the left side directly into it).
#define RGC_OUTLINE_MAX (2 * (RGC_FLAT_MAX + GLYPH_MAX_SEGS * RGC_ARC_MARGIN) + 2 * 8)

// Scratch (sidestack) bump allocator -- see common/scratch_alloc.h. Used
// by build_smoothed/stroke_build_outline for their KB-to-tens-of-KB of
// temporaries; the backing buffer is owned by the top of the call chain
// (font_lowspec_pebble.c on Pebble, glyph_render.c on desktop).
#include "common/scratch_alloc.h"

// ---------------------------------------------------------------------
// Numeric configuration (float defaults; override before including for
// a fixed-point instantiation -- see render_lowspec.c).
// ---------------------------------------------------------------------

#ifndef RGC_FLOAT
#define RGC_FLOAT float
#endif
#ifndef RGC_MUL
#define RGC_MUL(a,b)    ((a)*(b))
#endif
#ifndef RGC_DIV
#define RGC_DIV(a,b)    ((a)/(b))
#endif
#ifndef RGC_SQRT
#define RGC_SQRT(a)     m_sqrtf(a)
#endif
#ifndef RGC_SIN
#define RGC_SIN(a)      m_sinf(a)
#endif
#ifndef RGC_COS
#define RGC_COS(a)      m_cosf(a)
#endif
#ifndef RGC_ACOS
#define RGC_ACOS(a)     m_acosf(a)
#endif
#ifndef RGC_ATAN2
#define RGC_ATAN2(y,x)  m_atan2f(y,x)
#endif
#ifndef RGC_ABS
#define RGC_ABS(a)      m_fabsf(a)
#endif
#ifndef RGC_CONST
#define RGC_CONST(lit)  (lit)
#endif
#ifndef RGC_FROM_INT
#define RGC_FROM_INT(i) ((RGC_FLOAT)(i))
#endif
// Ceil to int (used by RGC_BEZIER_ADAPTIVE). Fixed point overrides this.
#ifndef RGC_CEIL_INT
#define RGC_CEIL_INT(v) ((int)m_ceilf(v))
#endif
#ifndef RGC_FX
#define RGC_FX(v)       ((int)((v) * 65536.0f))
#endif
// "Effectively zero" divisor guard. Must NOT collapse to literal 0 in fixed
// point: integer div-by-0 is UB and crashes (unlike float's inf/nan), and
// RGC_CONST(1e-6f) truncates to 0 under fx16_from_float.
#ifndef RGC_EPS
#define RGC_EPS         RGC_CONST(1e-6f)
#endif
// Vector length. Split out so fixed point can override just this: the naive
// RGC_SQRT(dx*dx + dy*dy) form truncates each fx16 square before summing and
// overflows once |dx| or |dy| > ~181 (well within a valid length's range).
// The float path has no such risk.
#ifndef RGC_HYPOT
#define RGC_HYPOT(dx,dy) RGC_SQRT(RGC_MUL(dx,dx) + RGC_MUL(dy,dy))
#endif

// ===================================================================
// line_isect -- intersection of two lines given as point + direction.
// ===================================================================
static int line_isect(RGC_FLOAT ax, RGC_FLOAT ay, RGC_FLOAT adx, RGC_FLOAT ady,
                      RGC_FLOAT bx, RGC_FLOAT by, RGC_FLOAT bdx, RGC_FLOAT bdy,
                      RGC_FLOAT *ox, RGC_FLOAT *oy) {
    RGC_FLOAT det = RGC_MUL(adx, bdy) - RGC_MUL(ady, bdx);
    if (RGC_ABS(det) < RGC_EPS) return 0;
    RGC_FLOAT t = RGC_DIV(RGC_MUL(bx - ax, bdy) - RGC_MUL(by - ay, bdx), det);
    *ox = ax + RGC_MUL(t, adx);
    *oy = ay + RGC_MUL(t, ady);
    return 1;
}

// Build smoothed centerline with overshoot capping. Rounded-corner tangent
// distances on a shared segment must not overshoot each other or a segment
// end. Two passes (end segments first, then interior) cap them: overlapping
// corners each pull back half the overlap, and a corner's effective tangent
// distance is the min of its two segments' constraints.
static int build_smoothed(const FVec2* pts, const int* flags, int n,
                          RGC_FLOAT radius, RGC_FLOAT small_radius, RGC_FLOAT medium_radius, RGC_FLOAT half_width,
                          FVec2* out_pts, int* out_ctrl, int* out_degraded, int* out_orig_idx,
                          int max_out, char *scratch, char *scratch_end) {
    if (n < 2) {
        for (int i = 0; i < n && i < max_out; i++) {
            out_pts[i].x = pts[i].x; out_pts[i].y = pts[i].y;
            out_ctrl[i] = 0;
            if (out_degraded) out_degraded[i] = 0;
        }
        return n;
    }

    // ---- Phase 1: segment lengths ----
    RGC_FLOAT *seg_len = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    if (!seg_len) return 0;
    for (int i = 0; i < n - 1; i++) {
        RGC_FLOAT dx = pts[i+1].x - pts[i].x;
        RGC_FLOAT dy = pts[i+1].y - pts[i].y;
        seg_len[i] = RGC_HYPOT(dx, dy);
    }

    // ---- Phase 2: per-corner geometry & desired tangent distances ----
    int    *want_sm      = scratch_alloc(int, GLYPH_MAX_SEGS);
    RGC_FLOAT *desired_td = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    RGC_FLOAT *sinHA      = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    RGC_FLOAT *cosHA      = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    FVec2  *v1_arr        = scratch_alloc(FVec2, GLYPH_MAX_SEGS);
    FVec2  *v2_arr        = scratch_alloc(FVec2, GLYPH_MAX_SEGS);
    FVec2  *dir_arr       = scratch_alloc(FVec2, GLYPH_MAX_SEGS);
    RGC_FLOAT *dir_len_arr = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    if (!want_sm || !desired_td || !sinHA || !cosHA || !v1_arr || !v2_arr || !dir_arr || !dir_len_arr)
        return 0;

    for (int i = 0; i < n; i++) {
        want_sm[i] = 0;
        desired_td[i] = RGC_CONST(0.0f);
        sinHA[i] = RGC_CONST(0.0f);
        cosHA[i] = RGC_CONST(1.0f);
        v1_arr[i].x = RGC_CONST(0.0f); v1_arr[i].y = RGC_CONST(0.0f);
        v2_arr[i].x = RGC_CONST(0.0f); v2_arr[i].y = RGC_CONST(0.0f);
        dir_arr[i].x = RGC_CONST(0.0f); dir_arr[i].y = RGC_CONST(0.0f);
        dir_len_arr[i] = RGC_CONST(0.0f);

        int has_prev = (i > 0);
        int has_next = (i < n - 1);
        int rv = flags ? GLYPH_FLAG_ROUND(flags[i]) : 0;
        int flag_round  = (rv == GLYPH_ROUND_DEFAULT);
        int flag_small  = (rv == GLYPH_ROUND_SMALL_RADIUS);
        int flag_medium = (rv == GLYPH_ROUND_MEDIUM_RADIUS);
        (void)flag_round;
        RGC_FLOAT pt_radius;
        if (flag_small) pt_radius = small_radius;
        else if (flag_medium) pt_radius = medium_radius;
        else pt_radius = radius;
        if (!has_prev || !has_next || rv == GLYPH_ROUND_NOT_ROUNDED || pt_radius <= RGC_CONST(0.01f))
            continue;

        FVec2 v1; v1.x = pts[i-1].x - pts[i].x; v1.y = pts[i-1].y - pts[i].y;
        FVec2 v2; v2.x = pts[i+1].x - pts[i].x; v2.y = pts[i+1].y - pts[i].y;
        RGC_FLOAT len1 = RGC_HYPOT(v1.x, v1.y);
        RGC_FLOAT len2 = RGC_HYPOT(v2.x, v2.y);
        if (len1 < RGC_CONST(0.001f) || len2 < RGC_CONST(0.001f)) continue;
        v1.x = RGC_DIV(v1.x, len1); v1.y = RGC_DIV(v1.y, len1);
        v2.x = RGC_DIV(v2.x, len2); v2.y = RGC_DIV(v2.y, len2);

        FVec2 dir; dir.x = v1.x + v2.x; dir.y = v1.y + v2.y;
        RGC_FLOAT dlen = RGC_HYPOT(dir.x, dir.y);

        RGC_FLOAT dot = RGC_MUL(v1.x,v2.x) + RGC_MUL(v1.y,v2.y);
        if (dot < RGC_CONST(-1.0f)) dot = RGC_CONST(-1.0f);
        if (dot >  RGC_CONST(1.0f)) dot =  RGC_CONST(1.0f);
        RGC_FLOAT ha = RGC_DIV(RGC_ACOS(dot), RGC_FROM_INT(2));
        if (ha < RGC_CONST(0.001f)) continue;

        want_sm[i] = 1;
        v1_arr[i] = v1;
        v2_arr[i] = v2;
        dir_arr[i] = dir;
        dir_len_arr[i] = dlen;
        sinHA[i] = RGC_SIN(ha);
        cosHA[i] = RGC_COS(ha);
        desired_td[i] = RGC_DIV(RGC_MUL(pt_radius, cosHA[i]), sinHA[i]);
    }

    // ---- Phase 3: cap tangent distances for overshoot ----
    RGC_FLOAT *eff_td = scratch_alloc(RGC_FLOAT, GLYPH_MAX_SEGS);
    if (!eff_td) return 0;
    for (int i = 0; i < n; i++) eff_td[i] = desired_td[i];

    // Two passes: end segments first (they have a non-rounded endpoint, td=0,
    // so capping them first gives interior corners more room), then interior.
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < n - 1; j++) {
            int is_end_segment = (j == 0 || j == n - 2);
            if (pass == 0 && !is_end_segment) continue;   // pass 0: ends only
            if (pass == 1 &&  is_end_segment) continue;   // pass 1: interior only

            RGC_FLOAT left  = (j     > 0    && want_sm[j])   ? eff_td[j]   : RGC_CONST(0.0f);
            RGC_FLOAT right = (j + 1 < n - 1 && want_sm[j+1]) ? eff_td[j+1] : RGC_CONST(0.0f);
            RGC_FLOAT len = seg_len[j];
            if (len < RGC_CONST(0.001f)) continue;

            // Cap individually to segment length
            if (left  > len) left  = len;
            if (right > len) right = len;

            // Pull back each by half the overlap
            RGC_FLOAT overshoot = left + right - len;
            if (overshoot > RGC_CONST(0.0f)) {
                left  -= RGC_MUL(overshoot, RGC_CONST(0.5f));
                right -= RGC_MUL(overshoot, RGC_CONST(0.5f));
                if (left  < RGC_CONST(0.0f)) left  = RGC_CONST(0.0f);
                if (right < RGC_CONST(0.0f)) right = RGC_CONST(0.0f);
            }

            // Write back, taking min with current value
            if (j     > 0      && want_sm[j]   && left  < eff_td[j])   eff_td[j]   = left;
            if (j + 1 < n - 1  && want_sm[j+1] && right < eff_td[j+1]) eff_td[j+1] = right;
        }
    }

    // ---- Phase 4: generate output using effective tangent distances ----
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int has_prev = (i > 0);
        int has_next = (i < n - 1);
        int rv2 = flags ? GLYPH_FLAG_ROUND(flags[i]) : 0;
        int flag_small  = (rv2 == GLYPH_ROUND_SMALL_RADIUS);
        int flag_medium = (rv2 == GLYPH_ROUND_MEDIUM_RADIUS);
        RGC_FLOAT pt_radius;
        if (flag_small) pt_radius = small_radius;
        else if (flag_medium) pt_radius = medium_radius;
        else pt_radius = radius;
        int want_smooth = want_sm[i];
        int is_degraded = want_smooth && half_width > RGC_CONST(0.0f) && half_width >= pt_radius;
        // pt_radius already accounts for flag_small/flag_medium/flag_round
        int do_smooth = want_smooth && !is_degraded;

        if (!has_prev || !has_next || !do_smooth) {
            if (cnt >= max_out) break;
            out_pts[cnt].x = pts[i].x;
            out_pts[cnt].y = pts[i].y;
            out_ctrl[cnt] = 0;
            if (out_degraded) out_degraded[cnt] = is_degraded ? 1 : 0;
            if (out_orig_idx) out_orig_idx[cnt] = i;
            cnt++;
            continue;
        }

        RGC_FLOAT tangentDist = eff_td[i];

        // If radius was capped to effectively zero, output a sharp corner
        if (tangentDist < RGC_CONST(0.001f)) {
            if (cnt >= max_out) break;
            out_pts[cnt].x = pts[i].x;
            out_pts[cnt].y = pts[i].y;
            out_ctrl[cnt] = 0;
            if (out_degraded) out_degraded[cnt] = 0;
            if (out_orig_idx) out_orig_idx[cnt] = i;
            cnt++;
            continue;
        }

        // Derive effective radius from the capped tangent distance.
// All other geometric quantities are computed from this effective
// radius so the bezier stays geometrically consistent.
        RGC_FLOAT eff_radius = RGC_DIV(RGC_MUL(tangentDist, sinHA[i]), cosHA[i]);
        RGC_FLOAT moveDist    = RGC_DIV(eff_radius, sinHA[i]);
        RGC_FLOAT n2Dist      = moveDist - eff_radius;
        RGC_FLOAT controlDist = RGC_DIV(n2Dist, cosHA[i]);

        FVec2 b = pts[i];
        FVec2 v1 = v1_arr[i];
        FVec2 v2 = v2_arr[i];
        FVec2 dir = dir_arr[i];
        RGC_FLOAT dir_len = dir_len_arr[i];

        if (cnt + 5 > max_out) break;

        FVec2 n1; n1.x = b.x + RGC_MUL(v1.x, tangentDist); n1.y = b.y + RGC_MUL(v1.y, tangentDist);
        FVec2 c1; c1.x = b.x + RGC_MUL(v1.x, controlDist); c1.y = b.y + RGC_MUL(v1.y, controlDist);
        FVec2 n2_pt;
        if (dir_len < RGC_CONST(0.001f)) {
            n2_pt = b;
        } else {
            n2_pt.x = b.x + RGC_MUL(RGC_DIV(dir.x, dir_len), n2Dist);
            n2_pt.y = b.y + RGC_MUL(RGC_DIV(dir.y, dir_len), n2Dist);
        }
        FVec2 c2; c2.x = b.x + RGC_MUL(v2.x, controlDist); c2.y = b.y + RGC_MUL(v2.y, controlDist);
        FVec2 n3; n3.x = b.x + RGC_MUL(v2.x, tangentDist); n3.y = b.y + RGC_MUL(v2.y, tangentDist);

        out_pts[cnt].x = n1.x;   out_pts[cnt].y = n1.y;   out_ctrl[cnt] = 0; if (out_degraded) out_degraded[cnt] = 0; if (out_orig_idx) out_orig_idx[cnt] = i; cnt++;
        out_pts[cnt].x = c1.x;   out_pts[cnt].y = c1.y;   out_ctrl[cnt] = 1; if (out_degraded) out_degraded[cnt] = 0; if (out_orig_idx) out_orig_idx[cnt] = i; cnt++;
        out_pts[cnt].x = n2_pt.x; out_pts[cnt].y = n2_pt.y; out_ctrl[cnt] = 0; if (out_degraded) out_degraded[cnt] = 0; if (out_orig_idx) out_orig_idx[cnt] = i; cnt++;
        out_pts[cnt].x = c2.x;   out_pts[cnt].y = c2.y;   out_ctrl[cnt] = 1; if (out_degraded) out_degraded[cnt] = 0; if (out_orig_idx) out_orig_idx[cnt] = i; cnt++;
        out_pts[cnt].x = n3.x;   out_pts[cnt].y = n3.y;   out_ctrl[cnt] = 0; if (out_degraded) out_degraded[cnt] = 0; if (out_orig_idx) out_orig_idx[cnt] = i; cnt++;
    }
    return cnt;
}

// Flatten smoothed centerline (quadratic control points -> line segments).
// smooth_degraded: optional input; marks degraded corners in smoothed output.
// flat_degraded: optional output; marks degraded corners in flat output.
//
static int flatten_smoothed(const FVec2* smooth_pts, const int* smooth_ctrl,
                             const int* smooth_degraded, const int* smooth_orig_idx,
                             int sn, FVec2* flat, int* flat_degraded,
                             int* flat_orig_idx,
                             int max_flat) {
    int flat_n = 0;
    if (sn < 1) return 0;

    if (flat_n >= max_flat) return 0;
    flat[flat_n] = smooth_pts[0];
    if (flat_degraded) flat_degraded[flat_n] = smooth_degraded ? smooth_degraded[0] : 0;
    if (flat_orig_idx) flat_orig_idx[flat_n] = smooth_orig_idx ? smooth_orig_idx[0] : 0;
    flat_n++;

    for (int i = 1; i < sn; i++) {
        if (smooth_ctrl[i]) {
            if (i + 1 < sn && flat_n + (RGC_BEZIER_MAX_STEPS + 1) <= max_flat) {
                FVec2 p0 = flat[flat_n - 1];
                FVec2 cp = smooth_pts[i];
                FVec2 p1 = smooth_pts[i+1];
#ifdef RGC_BEZIER_ADAPTIVE
                // Segments needed for a flatness tolerance scale as
                // sqrt(deviation/tol); a quadratic's max deviation from its
                // chord is |cp - midpoint(p0,p1)| / 2.
                FVec2 mid; mid.x = RGC_MUL(p0.x + p1.x, RGC_CONST(0.5f));
                            mid.y = RGC_MUL(p0.y + p1.y, RGC_CONST(0.5f));
                RGC_FLOAT dev = RGC_HYPOT(cp.x - mid.x, cp.y - mid.y);
                int steps = RGC_CEIL_INT(RGC_SQRT(RGC_DIV(dev, RGC_BEZIER_TOL)));
                if (steps < 1) steps = 1;
                if (steps > RGC_BEZIER_MAX_STEPS) steps = RGC_BEZIER_MAX_STEPS;
#else
                int steps = RGC_BEZIER_STEPS;
#endif
                for (int s = 1; s <= steps; s++) {
                    RGC_FLOAT t = RGC_DIV(RGC_FROM_INT(s), RGC_FROM_INT(steps));
                    RGC_FLOAT mt = RGC_CONST(1.0f) - t;
                    flat[flat_n].x = RGC_MUL(RGC_MUL(mt,mt), p0.x) + RGC_MUL(RGC_MUL(RGC_FROM_INT(2),RGC_MUL(mt,t)), cp.x) + RGC_MUL(RGC_MUL(t,t), p1.x);
                    flat[flat_n].y = RGC_MUL(RGC_MUL(mt,mt), p0.y) + RGC_MUL(RGC_MUL(RGC_FROM_INT(2),RGC_MUL(mt,t)), cp.y) + RGC_MUL(RGC_MUL(t,t), p1.y);
                    // bezier-interpolated points are never degraded corners
                    if (flat_degraded) flat_degraded[flat_n] = 0;
                    if (flat_orig_idx) flat_orig_idx[flat_n] = smooth_orig_idx ? smooth_orig_idx[i] : 0;
                    flat_n++;
                }
                i++;
            }
        } else {
            FVec2 last = flat[flat_n - 1];
            FVec2 cur = smooth_pts[i];
            if (RGC_ABS(cur.x - last.x) > RGC_CONST(0.01f) || RGC_ABS(cur.y - last.y) > RGC_CONST(0.01f)) {
                if (flat_n < max_flat) {
                    flat[flat_n] = cur;
                    if (flat_degraded) flat_degraded[flat_n] = smooth_degraded ? smooth_degraded[i] : 0;
                    if (flat_orig_idx) flat_orig_idx[flat_n] = smooth_orig_idx ? smooth_orig_idx[i] : 0;
                    flat_n++;
                }
            }
        }
    }
    return flat_n;
}

// Build the outline for a stroke subpath into out_contour[] (capacity >=
// RGC_OUTLINE_MAX), returning the vertex count -- pure lines-to-lines, no
// GrPath. Fill callers pass it to gr_add_contour(); vector consumers (export,
// AA lines) read it directly. The contour is *open* (no closing duplicate),
// in left -> end-cap -> right(backward) -> start-cap order; the left side is
// built directly into out_contour, only the right stays internal. Square/
// axis-snap caps are baked into the sides, round caps add arc vertices. A
// "degraded" corner (half_width >= radius) stays sharp, rounding only the
// outer side and mitering the inner. Returns 0 on a too-short subpath.
//
// pts is mutated in place: cap extension shifts pts[0]/pts[n_orig-1].
static int stroke_build_outline(FVec2* out_contour, int max_contour,
                                  FVec2* pts, const int* pt_flags,
                                  const RGC_FLOAT* pt_hw, int n_orig,
                                  int start_cap, int end_cap,
                                  RGC_FLOAT scaled_radius, RGC_FLOAT scaled_small_radius,
                                  RGC_FLOAT scaled_medium_radius, RGC_FLOAT half_width,
                                  char *scratch, char *scratch_end) {
    if (!out_contour || max_contour < 1) return 0;
    // Axis-snap caps: extend the centerline past the clip line now, clip the
    // cap face to the axis after building. Snap axis = horizontal (clip Y) if
    // the stroke is more vertical (|dy|>=|dx|), else vertical (clip X). Clip
    // line is half_width past the original endpoint, outward (= stroke dir for
    // end caps, opposite for start).
    int start_snap = 0, end_snap = 0;  // 0=none, 1=horizontal(clip Y), 2=vertical(clip X)
    RGC_FLOAT start_snap_sign = RGC_CONST(0.0f), end_snap_sign = RGC_CONST(0.0f);  // sign of stroke component along snap axis
    FVec2 orig_start = pts[0], orig_end = pts[n_orig-1];
    if ((start_cap == GLYPH_CAP_AXIS_SNAP || start_cap == GLYPH_CAP_AXIS_SNAP_BUTT) && n_orig >= 2) {
        RGC_FLOAT dx = pts[1].x - pts[0].x;
        RGC_FLOAT dy = pts[1].y - pts[0].y;
        if (RGC_ABS(dy) >= RGC_ABS(dx)) {
            start_snap = 1;
            start_snap_sign = (dy >= 0) ? RGC_CONST(1.0f) : RGC_CONST(-1.0f);
        } else {
            start_snap = 2;
            start_snap_sign = (dx >= 0) ? RGC_CONST(1.0f) : RGC_CONST(-1.0f);
        }
    }
    if ((end_cap == GLYPH_CAP_AXIS_SNAP || end_cap == GLYPH_CAP_AXIS_SNAP_BUTT) && n_orig >= 2) {
        RGC_FLOAT dx = pts[n_orig-1].x - pts[n_orig-2].x;
        RGC_FLOAT dy = pts[n_orig-1].y - pts[n_orig-2].y;
        if (RGC_ABS(dy) >= RGC_ABS(dx)) {
            end_snap = 1;
            end_snap_sign = (dy >= 0) ? RGC_CONST(1.0f) : RGC_CONST(-1.0f);
        } else {
            end_snap = 2;
            end_snap_sign = (dx >= 0) ? RGC_CONST(1.0f) : RGC_CONST(-1.0f);
        }
    }

    // Extend endpoints for caps before smoothing: square by half_width (cap
    // face perpendicular), axis-snap by 32*half_width (clipped to axis later).
    if (start_cap == GLYPH_CAP_SQUARE && n_orig >= 2) {
        RGC_FLOAT dx = pts[1].x - pts[0].x;
        RGC_FLOAT dy = pts[1].y - pts[0].y;
        RGC_FLOAT len = RGC_HYPOT(dx, dy);
        if (len > RGC_CONST(0.001f)) { pts[0].x -= RGC_MUL(RGC_DIV(dx,len), pt_hw[0]); pts[0].y -= RGC_MUL(RGC_DIV(dy,len), pt_hw[0]); }
    } else if ((start_cap == GLYPH_CAP_AXIS_SNAP || start_cap == GLYPH_CAP_AXIS_SNAP_BUTT)  && n_orig >= 2) {
        RGC_FLOAT dx = pts[1].x - pts[0].x;
        RGC_FLOAT dy = pts[1].y - pts[0].y;
        RGC_FLOAT len = RGC_HYPOT(dx, dy);
        if (len > RGC_CONST(0.001f)) { pts[0].x -= RGC_MUL(RGC_MUL(RGC_DIV(dx,len), pt_hw[0]), RGC_CONST(32.0f)); pts[0].y -= RGC_MUL(RGC_MUL(RGC_DIV(dy,len), pt_hw[0]), RGC_CONST(32.0f)); }
    }
    if (end_cap == GLYPH_CAP_SQUARE && n_orig >= 2) {
        RGC_FLOAT dx = pts[n_orig-1].x - pts[n_orig-2].x;
        RGC_FLOAT dy = pts[n_orig-1].y - pts[n_orig-2].y;
        RGC_FLOAT len = RGC_HYPOT(dx, dy);
        if (len > RGC_CONST(0.001f)) { pts[n_orig-1].x += RGC_MUL(RGC_DIV(dx,len), pt_hw[n_orig-1]); pts[n_orig-1].y += RGC_MUL(RGC_DIV(dy,len), pt_hw[n_orig-1]); }
    } else if ((end_cap == GLYPH_CAP_AXIS_SNAP || end_cap == GLYPH_CAP_AXIS_SNAP_BUTT) && n_orig >= 2) {
        RGC_FLOAT dx = pts[n_orig-1].x - pts[n_orig-2].x;
        RGC_FLOAT dy = pts[n_orig-1].y - pts[n_orig-2].y;
        RGC_FLOAT len = RGC_HYPOT(dx, dy);
        if (len > RGC_CONST(0.001f)) { pts[n_orig-1].x += RGC_MUL(RGC_MUL(RGC_DIV(dx,len), pt_hw[n_orig-1]), RGC_CONST(32.0f)); pts[n_orig-1].y += RGC_MUL(RGC_MUL(RGC_DIV(dy,len), pt_hw[n_orig-1]), RGC_CONST(32.0f)); }
    }

    // Build smoothed centerline (degraded corners are kept sharp)
    FVec2 *smooth_pts      = scratch_alloc(FVec2, GLYPH_SMOOTH_MAX);
    int   *smooth_ctrl     = scratch_alloc(int, GLYPH_SMOOTH_MAX);
    int   *smooth_degraded = scratch_alloc(int, GLYPH_SMOOTH_MAX);
    int   *smooth_orig_idx = scratch_alloc(int, GLYPH_SMOOTH_MAX);
    if (!smooth_pts || !smooth_ctrl || !smooth_degraded || !smooth_orig_idx) return 0;
    // scratch passed by value: build_smoothed's temporaries are freed on
    // return, so flat[] etc below reuse that space (see scratch allocator note).
    int sn = build_smoothed(pts, pt_flags, n_orig, scaled_radius, scaled_small_radius, scaled_medium_radius,
                            half_width, smooth_pts, smooth_ctrl, smooth_degraded, smooth_orig_idx,
                            GLYPH_SMOOTH_MAX, scratch, scratch_end);

    FVec2 *flat          = scratch_alloc(FVec2, RGC_FLAT_MAX);
    int   *flat_degraded = scratch_alloc(int, RGC_FLAT_MAX);
    int   *flat_orig_idx = scratch_alloc(int, RGC_FLAT_MAX);
    if (!flat || !flat_degraded || !flat_orig_idx) return 0;
    int flat_n = flatten_smoothed(smooth_pts, smooth_ctrl, smooth_degraded, smooth_orig_idx,
                                  sn, flat, flat_degraded, flat_orig_idx, RGC_FLAT_MAX);

    // Per-point half_width for outline offsets
    RGC_FLOAT *flat_hw = scratch_alloc(RGC_FLOAT, RGC_FLAT_MAX);
    if (!flat_hw) return 0;
    for (int j = 0; j < flat_n; j++)
        flat_hw[j] = (flat_orig_idx[j] >= 0 && flat_orig_idx[j] < n_orig) ? pt_hw[flat_orig_idx[j]] : half_width;

    int n = flat_n;
    if (n < 2) return 0;

    // Unit direction + left normal per segment (indexed to n-1 = flat_n,
    // bounded by RGC_FLAT_MAX).
    RGC_FLOAT *sdx = scratch_alloc(RGC_FLOAT, RGC_FLAT_MAX);
    RGC_FLOAT *sdy = scratch_alloc(RGC_FLOAT, RGC_FLAT_MAX);
    RGC_FLOAT *snx = scratch_alloc(RGC_FLOAT, RGC_FLAT_MAX);
    RGC_FLOAT *sny = scratch_alloc(RGC_FLOAT, RGC_FLAT_MAX);
    if (!sdx || !sdy || !snx || !sny) return 0;
    for (int i = 0; i < n - 1; i++) {
        RGC_FLOAT dx = flat[i+1].x - flat[i].x;
        RGC_FLOAT dy = flat[i+1].y - flat[i].y;
        RGC_FLOAT len = RGC_HYPOT(dx, dy);
        if (len < RGC_CONST(0.001f)) { dx = RGC_CONST(1.0f); dy = RGC_CONST(0.0f); len = RGC_CONST(1.0f); }
        sdx[i] = RGC_DIV(dx, len); sdy[i] = RGC_DIV(dy, len);
        snx[i] = -sdy[i];   sny[i] = sdx[i];
    }

    // Build outline: left side forward, right side backward; degraded corners
    // get a round arc on the outer side instead of a miter.
    #define ARC_STEPS 8
    #define MAX_OUTLINE (RGC_FLAT_MAX + GLYPH_MAX_SEGS * RGC_ARC_MARGIN)
    // Reject miters by turn angle, not length: cross ~= sin(turn) =
    // line_isect's determinant. Sharp corners have large cross_abs and
    // long-but-valid miters (90deg needs sqrt(2)); only a tiny cross_abs
    // (near-collinear pieces of a gentle curve) makes the determinant noise
    // and spikes the miter to infinity. A distance cap can't tell those apart.
    #define MITER_MIN_SIN RGC_CONST(0.02f)
    // Left builds directly into out_contour; only right needs its own scratch.
    FVec2 *left_out  = out_contour;
    FVec2 *right_out = scratch_alloc(FVec2, MAX_OUTLINE);
    if (!right_out) return 0;
    int left_count = 0, right_count = 0;

    // Butt cap at start
    left_out[left_count].x = flat[0].x + RGC_MUL(snx[0], flat_hw[0]); left_out[left_count].y = flat[0].y + RGC_MUL(sny[0], flat_hw[0]); left_count++;
    right_out[right_count].x = flat[0].x - RGC_MUL(snx[0], flat_hw[0]); right_out[right_count].y = flat[0].y - RGC_MUL(sny[0], flat_hw[0]); right_count++;

    // Interior joins
    for (int i = 1; i < n - 1; i++) {
        int is_deg = flat_degraded[i];
        RGC_FLOAT cross = RGC_MUL(sdx[i-1], sdy[i]) - RGC_MUL(sdy[i-1], sdx[i]);
        RGC_FLOAT cross_abs = RGC_ABS(cross);

        if (is_deg && cross_abs > RGC_CONST(0.001f)) {
            // Degraded corner: round join on outer side, miter on inner side
            RGC_FLOAT dot = RGC_MUL(sdx[i-1], sdx[i]) + RGC_MUL(sdy[i-1], sdy[i]);
            RGC_FLOAT turning = RGC_ATAN2(cross, dot);
            RGC_FLOAT cx_f = flat[i].x, cy_f = flat[i].y;

            // cross < 0  ->  code-left is outer (left-side round arc)
// cross > 0  ->  code-right is outer (right-side round arc)
            if (cross < 0) {
                // Left side: round arc
                RGC_FLOAT hw_i = flat_hw[i];
                RGC_FLOAT sa = RGC_ATAN2(sny[i-1], snx[i-1]);
                for (int k = 0; k <= ARC_STEPS; k++) {
                    RGC_FLOAT a = sa + RGC_MUL(turning, RGC_DIV(RGC_FROM_INT(k), RGC_FROM_INT(ARC_STEPS)));
                    left_out[left_count].x = cx_f + RGC_MUL(hw_i, RGC_COS(a));
                    left_out[left_count].y = cy_f + RGC_MUL(hw_i, RGC_SIN(a));
                    left_count++;
                }
                // Right side: miter (inner)
                RGC_FLOAT ra_x = flat[i].x - RGC_MUL(snx[i-1], hw_i);
                RGC_FLOAT ra_y = flat[i].y - RGC_MUL(sny[i-1], hw_i);
                RGC_FLOAT rb_x = flat[i].x - RGC_MUL(snx[i], hw_i);
                RGC_FLOAT rb_y = flat[i].y - RGC_MUL(sny[i], hw_i);
                RGC_FLOAT rx_, ry_;
                int right_ok = (cross_abs > MITER_MIN_SIN) &&
                    line_isect(ra_x, ra_y, sdx[i-1], sdy[i-1],
                               rb_x, rb_y, sdx[i], sdy[i], &rx_, &ry_);
                if (right_ok) {
                    right_out[right_count].x = rx_; right_out[right_count].y = ry_; right_count++;
                } else {
                    right_out[right_count].x = RGC_MUL(ra_x + rb_x, RGC_CONST(0.5f));
                    right_out[right_count].y = RGC_MUL(ra_y + rb_y, RGC_CONST(0.5f));
                    right_count++;
                }
            } else {
                // Right side: round arc
                RGC_FLOAT hw_i = flat_hw[i];
                RGC_FLOAT sa = RGC_ATAN2(-sny[i-1], -snx[i-1]);
                for (int k = 0; k <= ARC_STEPS; k++) {
                    RGC_FLOAT a = sa + RGC_MUL(turning, RGC_DIV(RGC_FROM_INT(k), RGC_FROM_INT(ARC_STEPS)));
                    right_out[right_count].x = cx_f + RGC_MUL(hw_i, RGC_COS(a));
                    right_out[right_count].y = cy_f + RGC_MUL(hw_i, RGC_SIN(a));
                    right_count++;
                }
                // Left side: miter (inner)
                RGC_FLOAT la_x = flat[i].x + RGC_MUL(snx[i-1], hw_i);
                RGC_FLOAT la_y = flat[i].y + RGC_MUL(sny[i-1], hw_i);
                RGC_FLOAT lb_x = flat[i].x + RGC_MUL(snx[i], hw_i);
                RGC_FLOAT lb_y = flat[i].y + RGC_MUL(sny[i], hw_i);
                RGC_FLOAT lx, ly;
                int left_ok = (cross_abs > MITER_MIN_SIN) &&
                    line_isect(la_x, la_y, sdx[i-1], sdy[i-1],
                               lb_x, lb_y, sdx[i], sdy[i], &lx, &ly);
                if (left_ok) {
                    left_out[left_count].x = lx; left_out[left_count].y = ly; left_count++;
                } else {
                    left_out[left_count].x = RGC_MUL(la_x + lb_x, RGC_CONST(0.5f));
                    left_out[left_count].y = RGC_MUL(la_y + lb_y, RGC_CONST(0.5f));
                    left_count++;
                }
            }
        } else {
            // Regular (non-degraded) corner: miter on both sides
            RGC_FLOAT hw_i = flat_hw[i];
            RGC_FLOAT la_x = flat[i].x + RGC_MUL(snx[i-1], hw_i);
            RGC_FLOAT la_y = flat[i].y + RGC_MUL(sny[i-1], hw_i);
            RGC_FLOAT lb_x = flat[i].x + RGC_MUL(snx[i], hw_i);
            RGC_FLOAT lb_y = flat[i].y + RGC_MUL(sny[i], hw_i);
            RGC_FLOAT lx, ly;
            int well_conditioned = cross_abs > MITER_MIN_SIN;
            int left_ok = well_conditioned &&
                line_isect(la_x, la_y, sdx[i-1], sdy[i-1],
                           lb_x, lb_y, sdx[i], sdy[i], &lx, &ly);

            RGC_FLOAT ra_x = flat[i].x - RGC_MUL(snx[i-1], hw_i);
            RGC_FLOAT ra_y = flat[i].y - RGC_MUL(sny[i-1], hw_i);
            RGC_FLOAT rb_x = flat[i].x - RGC_MUL(snx[i], hw_i);
            RGC_FLOAT rb_y = flat[i].y - RGC_MUL(sny[i], hw_i);
            RGC_FLOAT rx_, ry_;
            int right_ok = well_conditioned &&
                line_isect(ra_x, ra_y, sdx[i-1], sdy[i-1],
                           rb_x, rb_y, sdx[i], sdy[i], &rx_, &ry_);

            if (left_ok) {
                left_out[left_count].x = lx; left_out[left_count].y = ly; left_count++;
            } else {
                left_out[left_count].x = RGC_MUL(la_x + lb_x, RGC_CONST(0.5f));
                left_out[left_count].y = RGC_MUL(la_y + lb_y, RGC_CONST(0.5f));
                left_count++;
            }
            if (right_ok) {
                right_out[right_count].x = rx_; right_out[right_count].y = ry_; right_count++;
            } else {
                right_out[right_count].x = RGC_MUL(ra_x + rb_x, RGC_CONST(0.5f));
                right_out[right_count].y = RGC_MUL(ra_y + rb_y, RGC_CONST(0.5f));
                right_count++;
            }
        }
    }

    // Butt cap at end (also serves as square cap when endpoints were shifted)
    int last = n - 2;
    left_out[left_count].x = flat[n-1].x + RGC_MUL(snx[last], flat_hw[n-1]); left_out[left_count].y = flat[n-1].y + RGC_MUL(sny[last], flat_hw[n-1]); left_count++;
    right_out[right_count].x = flat[n-1].x - RGC_MUL(snx[last], flat_hw[n-1]); right_out[right_count].y = flat[n-1].y - RGC_MUL(sny[last], flat_hw[n-1]); right_count++;

    // Clip the extended axis-snap outline against the snap line: trim vertices
    // past it, using the full (x,y) edge intersection. (Moving only the clipped
    // coordinate would taper diagonal strokes to the wrong width.)
    if (start_snap) {
        RGC_FLOAT square = start_cap == GLYPH_CAP_AXIS_SNAP ? RGC_CONST(1.0f) : RGC_CONST(0.0f);
        int axis = (start_snap == 1) ? 0 : 1; // 0=Y, 1=X
        RGC_FLOAT clip_val = (axis == 0) ? (orig_start.y - RGC_MUL(RGC_MUL(square, pt_hw[0]), start_snap_sign))
                                      : (orig_start.x - RGC_MUL(RGC_MUL(square, pt_hw[0]), start_snap_sign));
        int keep_geq = (start_snap_sign > 0) ? 1 : 0;

        // Clip left_out from start
        {
            int i;
            for (i = 0; i < left_count; i++) {
                RGC_FLOAT v = (axis == 0) ? left_out[i].y : left_out[i].x;
                if (keep_geq ? (v >= clip_val) : (v <= clip_val)) break;
            }
            if (i > 0 && i < left_count) {
                RGC_FLOAT v0 = (axis == 0) ? left_out[i-1].y : left_out[i-1].x;
                RGC_FLOAT v1 = (axis == 0) ? left_out[i].y : left_out[i].x;
                // dv==0: edge parallel to the clip line (fx div-by-0 would
                // crash) -- fall back to t=0 (clamp to the known endpoint).
                RGC_FLOAT dv = v1 - v0;
                RGC_FLOAT t = (dv != 0) ? RGC_DIV(clip_val - v0, dv) : RGC_CONST(0.0f);
                FVec2 isect;
                isect.x = left_out[i-1].x + RGC_MUL(t, left_out[i].x - left_out[i-1].x);
                isect.y = left_out[i-1].y + RGC_MUL(t, left_out[i].y - left_out[i-1].y);
                if (axis == 0) isect.y = clip_val; else isect.x = clip_val;
                left_out[0] = isect;
                for (int j = i; j < left_count; j++)
                    left_out[j - i + 1] = left_out[j];
                left_count -= i - 1;
            }
        }
        // Clip right_out from start
        {
            int i;
            for (i = 0; i < right_count; i++) {
                RGC_FLOAT v = (axis == 0) ? right_out[i].y : right_out[i].x;
                if (keep_geq ? (v >= clip_val) : (v <= clip_val)) break;
            }
            if (i > 0 && i < right_count) {
                RGC_FLOAT v0 = (axis == 0) ? right_out[i-1].y : right_out[i-1].x;
                RGC_FLOAT v1 = (axis == 0) ? right_out[i].y : right_out[i].x;
                // dv==0: edge parallel to the clip line (fx div-by-0 would
                // crash) -- fall back to t=0 (clamp to the known endpoint).
                RGC_FLOAT dv = v1 - v0;
                RGC_FLOAT t = (dv != 0) ? RGC_DIV(clip_val - v0, dv) : RGC_CONST(0.0f);
                FVec2 isect;
                isect.x = right_out[i-1].x + RGC_MUL(t, right_out[i].x - right_out[i-1].x);
                isect.y = right_out[i-1].y + RGC_MUL(t, right_out[i].y - right_out[i-1].y);
                if (axis == 0) isect.y = clip_val; else isect.x = clip_val;
                right_out[0] = isect;
                for (int j = i; j < right_count; j++)
                    right_out[j - i + 1] = right_out[j];
                right_count -= i - 1;
            }
        }
    }
    if (end_snap) {
        RGC_FLOAT square = end_cap == GLYPH_CAP_AXIS_SNAP ? RGC_CONST(1.0f) : RGC_CONST(0.0f);
        int axis = (end_snap == 1) ? 0 : 1;
        RGC_FLOAT clip_val = (axis == 0) ? (orig_end.y + RGC_MUL(RGC_MUL(square, pt_hw[n_orig-1]), end_snap_sign))
                                      : (orig_end.x + RGC_MUL(RGC_MUL(square, pt_hw[n_orig-1]), end_snap_sign));
        int keep_geq = (end_snap_sign > 0) ? 0 : 1; // opposite of start

        // Clip left_out from end
        {
            int i;
            for (i = left_count - 1; i >= 0; i--) {
                RGC_FLOAT v = (axis == 0) ? left_out[i].y : left_out[i].x;
                if (keep_geq ? (v >= clip_val) : (v <= clip_val)) break;
            }
            if (i < left_count - 1 && i >= 0) {
                RGC_FLOAT v0 = (axis == 0) ? left_out[i].y : left_out[i].x;
                RGC_FLOAT v1 = (axis == 0) ? left_out[i+1].y : left_out[i+1].x;
                // dv==0: edge parallel to the clip line (fx div-by-0 would
                // crash) -- fall back to t=0 (clamp to the known endpoint).
                RGC_FLOAT dv = v1 - v0;
                RGC_FLOAT t = (dv != 0) ? RGC_DIV(clip_val - v0, dv) : RGC_CONST(0.0f);
                FVec2 isect;
                isect.x = left_out[i].x + RGC_MUL(t, left_out[i+1].x - left_out[i].x);
                isect.y = left_out[i].y + RGC_MUL(t, left_out[i+1].y - left_out[i].y);
                if (axis == 0) isect.y = clip_val; else isect.x = clip_val;
                left_out[i + 1] = isect;
                left_count = i + 2;
            }
        }
        // Clip right_out from end
        {
            int i;
            for (i = right_count - 1; i >= 0; i--) {
                RGC_FLOAT v = (axis == 0) ? right_out[i].y : right_out[i].x;
                if (keep_geq ? (v >= clip_val) : (v <= clip_val)) break;
            }
            if (i < right_count - 1 && i >= 0) {
                RGC_FLOAT v0 = (axis == 0) ? right_out[i].y : right_out[i].x;
                RGC_FLOAT v1 = (axis == 0) ? right_out[i+1].y : right_out[i+1].x;
                // dv==0: edge parallel to the clip line (fx div-by-0 would
                // crash) -- fall back to t=0 (clamp to the known endpoint).
                RGC_FLOAT dv = v1 - v0;
                RGC_FLOAT t = (dv != 0) ? RGC_DIV(clip_val - v0, dv) : RGC_CONST(0.0f);
                FVec2 isect;
                isect.x = right_out[i].x + RGC_MUL(t, right_out[i+1].x - right_out[i].x);
                isect.y = right_out[i].y + RGC_MUL(t, right_out[i+1].y - right_out[i].y);
                if (axis == 0) isect.y = clip_val; else isect.x = clip_val;
                right_out[i + 1] = isect;
                right_count = i + 2;
            }
        }
    }

    // Assemble the open contour: left is already at out_contour[0..left_count)
    // (the left_out alias), so just append the end cap, reversed right, and
    // start cap. Only round caps add vertices (arc interiors; endpoints are the
    // side vertices already present).
    #define CAP_PI RGC_CONST(3.14159265f)
    int cc = left_count;  // left side already occupies out_contour[0..left_count)
    #define APPEND_PT(px, py) do {                                        \
        RGC_FLOAT ex_ = (px), ey_ = (py);                                 \
        if (cc < max_contour) { out_contour[cc].x = ex_; out_contour[cc].y = ey_; } \
        cc++;                                                             \
    } while (0)

    // End cap: round arc interior (square/axis-snap handled by extension + clipping above)
    if (end_cap == GLYPH_CAP_ROUND) {
        RGC_FLOAT hw_end = flat_hw[n-1];
        RGC_FLOAT sa = RGC_ATAN2(sny[last], snx[last]);
        for (int k = 1; k < ARC_STEPS; k++) {
            RGC_FLOAT a = sa - RGC_MUL(RGC_DIV(RGC_FROM_INT(k), RGC_FROM_INT(ARC_STEPS)), CAP_PI);
            APPEND_PT(flat[n-1].x + RGC_MUL(hw_end, RGC_COS(a)),
                      flat[n-1].y + RGC_MUL(hw_end, RGC_SIN(a)));
        }
    }

    // Right side backward
    for (int i = right_count - 1; i >= 0; i--) APPEND_PT(right_out[i].x, right_out[i].y);

    // Start cap: round arc interior
    if (start_cap == GLYPH_CAP_ROUND) {
        RGC_FLOAT hw_start = flat_hw[0];
        RGC_FLOAT sa = RGC_ATAN2(sny[0], snx[0]);
        for (int k = 1; k < ARC_STEPS; k++) {
            RGC_FLOAT a = sa + CAP_PI - RGC_MUL(RGC_DIV(RGC_FROM_INT(k), RGC_FROM_INT(ARC_STEPS)), CAP_PI);
            APPEND_PT(flat[0].x + RGC_MUL(hw_start, RGC_COS(a)),
                      flat[0].y + RGC_MUL(hw_start, RGC_SIN(a)));
        }
    }

    #undef APPEND_PT
    #undef CAP_PI
    #undef ARC_STEPS
    #undef MAX_OUTLINE
    #undef MITER_MIN_SIN
    // RGC_ARC_MARGIN / RGC_OUTLINE_MAX stay defined (file-scope, callers use them).
    return (cc < max_contour) ? cc : max_contour;
}

// Feed an assembled (open) outline contour into a GrPath as a closed fill
// polygon, repeating the first point to close it -- the glue between the pure
// core and the rasterizer. Vector consumers use the contour directly instead.
static void gr_add_contour(GrPath* path, const FVec2* contour, int n) {
    if (n < 2) return;
    gr_move_to(path, RGC_FX(contour[0].x), RGC_FX(contour[0].y));
    for (int i = 1; i < n; i++)
        gr_line_to(path, RGC_FX(contour[i].x), RGC_FX(contour[i].y));
    gr_line_to(path, RGC_FX(contour[0].x), RGC_FX(contour[0].y));
}
