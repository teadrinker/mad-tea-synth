#ifndef GLYPH_RENDER_H
#define GLYPH_RENDER_H

#include "common/tsys.h"

// ===== Glyph path data types =====

typedef enum {
    SEG_LINE_MOVE = 0,
    SEG_LINE_TO
} SegType;


// Sub-field masks: round(bits 0-3), cap(bits 4-7), width(bits 8-11)
#define GLYPH_FLAG_ROUND_MASK            ((1<< 0)|(1<< 1)|(1<< 2)|(1<< 3))
#define GLYPH_FLAG_CAP_MASK              ((1<< 4)|(1<< 5)|(1<< 6)|(1<< 7))
#define GLYPH_FLAG_WIDTH_MASK            ((1<< 8)|(1<< 9)|(1<<10)|(1<<11))
#define GLYPH_FLAG_MOVE_DESC_MASK        ((1<<12)|(1<<13)|(1<<14))  
#define GLYPH_FLAG_MOVE_GRID_ALIGN_MASK  ((1<<15)|(1<<16)|(1<<17))  

#define GLYPH_FLAG_ROUND(flags)          ((flags) & GLYPH_FLAG_ROUND_MASK)
#define GLYPH_FLAG_CAP(flags)            (((flags) & GLYPH_FLAG_CAP_MASK) >> 4)
#define GLYPH_FLAG_WIDTH(flags)          (((flags) & GLYPH_FLAG_WIDTH_MASK) >> 8)

#define GLYPH_FLAG_SET_ROUND(flags, val) ((flags) = ((flags) & ~GLYPH_FLAG_ROUND_MASK) | ((val) & 0xF))
#define GLYPH_FLAG_SET_CAP(flags, val)   ((flags) = ((flags) & ~GLYPH_FLAG_CAP_MASK) | (((val) & 0xF) << 4))
#define GLYPH_FLAG_SET_WIDTH(flags, val) ((flags) = ((flags) & ~GLYPH_FLAG_WIDTH_MASK) | (((val) & 0xF) << 8))

#define GLYPH_FLAG_MOVE_DESC(flags)            (((flags) & GLYPH_FLAG_MOVE_DESC_MASK) >> 12)
#define GLYPH_FLAG_MOVE_GRID(flags)            (((flags) & GLYPH_FLAG_MOVE_GRID_ALIGN_MASK) >> 15)

#define GLYPH_FLAG_SET_MOVE_DESC(flags, val) ((flags) = ((flags) & ~GLYPH_FLAG_MOVE_DESC_MASK) | (((val) & 0x7) << 12))
#define GLYPH_FLAG_SET_MOVE_GRID(flags, val) ((flags) = ((flags) & ~GLYPH_FLAG_MOVE_GRID_ALIGN_MASK) | (((val) & 0x7) << 15))

typedef enum {
    GLYPH_ROUND_NOT_ROUNDED   = 0,
    GLYPH_ROUND_DEFAULT       = 1,
    GLYPH_ROUND_SMALL_RADIUS  = 2,
    GLYPH_ROUND_MEDIUM_RADIUS = 3,
} GlyphRound;

typedef enum {
    GLYPH_CAP_SQUARE          = 0, 
    GLYPH_CAP_ROUND           = 1,
    GLYPH_CAP_BUTT            = 2, 
    GLYPH_CAP_AXIS_SNAP       = 3,
    GLYPH_CAP_AXIS_SNAP_BUTT  = 4,
//    GLYPH_CAP_HORISONTAL      = 5,
//    GLYPH_CAP_HORISONTAL_BUTT = 6,
//    GLYPH_CAP_VERTICAL        = 7,
//    GLYPH_CAP_VERTICAL_BUTT   = 8,
} GlyphCaps;

typedef enum {
    GLYPH_WIDTH_NORMAL       = 0, 
    GLYPH_WIDTH_THIN         = 1,
    GLYPH_WIDTH_MEDIUM       = 2,
    GLYPH_WIDTH_THICK        = 3,
} GlyphWidth;




#define GLYPH_COUNT     168

#define GLYPH_W  14
#define GLYPH_H  22

typedef struct {
    SegType type;
    float x, y;
    int flags;  // bitwise OR of GLYPH_FLAG_ROUND etc.
} GlyphSegment;

typedef struct {
    GlyphSegment *segs;
    int count;
} GLYPH_DATA;

// Packed glyph segment: 2 uint32 per segment.
// word0: bits 0-14 = x*4, bits 15-29 = y*4, bit 30 = move (1 = SEG_LINE_MOVE)
// word1: glyph flags
#define GLYPH_PACK_WORD0(x4, y4, move) \
    (((unsigned int)(x4) & 0x7FFFu) | (((unsigned int)(y4) & 0x7FFFu) << 15) | \
     (((unsigned int)(move) & 1u) << 30))
#define GLYPH_UNPACK_X4(w)   ((int)((w) & 0x7FFFu))
#define GLYPH_UNPACK_Y4(w)   ((int)(((w) >> 15) & 0x7FFFu))
#define GLYPH_UNPACK_MOVE(w) ((int)(((w) >> 30) & 1u))

// Smoothed centerline (for editor visualization)
typedef struct {
    float x, y;
    int is_quad_control;
} GlyphSmoothPt;

#ifndef GLYPH_SMOOTH_MAX
#define GLYPH_SMOOTH_MAX  640
#endif

// ===== Font settings struct =====

typedef struct {
    float radius;
    float small_radius_mul;
    float medium_radius_mul;

    float width_mul[16];  // id 0 : unused
    unsigned int width_flags[16]; // id 0 : unused

    float move_desc_x[8]; // id 0 : unused
    float move_desc_y[8]; // id 0 : unused
    float move_grid_x[8]; // id 0 : unused
    float move_grid_y[8]; // id 0 : unused

    float half_width_settings_range_start;
    float half_width_settings_range_end;

    float descend;
    float optical_size;

    int glyph_count;
    GLYPH_DATA *glyphs;
    GlyphSegment* glyph_segment_data;

    int glyph_grid_size[2];
    float glyph_box_hsize[2];
    float glyph_box_center[2];
    float glyph_box_default_hw;
} FONT_SETTINGS;

// ===== Public API =====

#define RG_FLAGS_IGNORE_GRADE_FIT 1

// Draw the stroke outline as anti-aliased lines (common/render_aaline.h) at full
// alpha instead of filling it with the scanline rasterizer -- a wireframe of
// the very same contour the fill path builds. Overlapping contours blend
// rather than replace, so crossings read slightly hotter than a single line.
#define RG_FLAGS_RENDER_LINE_OUTLINE 2

// Render a glyph into a width*height grayscale buffer (row-major, 0-255).
// scaleCoordsX/Y map glyph coords to pixels; offsetX/Y shift after scaling so
// glyph (0,0) lands at pixel (offsetX, offsetY). Typical: pad the buffer by
// >= strokewidth/2, pass offset=pad, blit at (dest - pad).
void render_glyph(Tsys *sys, const GLYPH_DATA *data, int width, int height,
                  unsigned char* grayscale_output,
                  const FONT_SETTINGS *settings,
                  float strokewidth,
                  float scaleCoordsX, float scaleCoordsY,
                  float offsetX, float offsetY, int flags);

void font_settings_free_glyphs(Tsys *sys, FONT_SETTINGS *fs);

// ===== Vector outline capture (for font export / toolchain) =====

typedef struct {
    float x, y;
} GlyphOutlinePt;

#define GLYPH_OUTLINE_MAX_CONTOURS  32
#define GLYPH_OUTLINE_MAX_POINTS    8192

typedef struct {
    GlyphOutlinePt *points;          // flat array of all contour points
    int contour_start[GLYPH_OUTLINE_MAX_CONTOURS];  // index into points[]
    int contour_len[GLYPH_OUTLINE_MAX_CONTOURS];    // point count in this contour
    int num_contours;
    int total_points;                // used entries in points[]
    int max_points;                  // capacity of points[]
    int own_allocation;              // 1 if points was malloc'd by init
} GlyphOutline;

// Initialize an outline with a dynamically-allocated points array.
// Returns 1 on success, 0 on allocation failure.
int  glyph_outline_init(Tsys *sys, GlyphOutline *ol, int max_points);

// Free the internal points array (only if own_allocation).
void glyph_outline_free(Tsys *sys, GlyphOutline *ol);

// Start a new contour.  Each render_glyph_outline call clears the outline first.
void glyph_outline_begin_contour(GlyphOutline *ol);

// Add a point to the current contour.
void glyph_outline_add_point(GlyphOutline *ol, float x, float y);

// Close the current contour (no-op; contours are finalized by begin_contour).
void glyph_outline_end_contour(GlyphOutline *ol);

// Render a glyph into a vector outline (filled polygon contours).
// The outline must be initialized with glyph_outline_init() first.
// After calling, ol->num_contours closed-polygon contours are in ol->points.
void render_glyph_outline(Tsys *sys, const GLYPH_DATA *data,
                          GlyphOutline *outline,
                          const FONT_SETTINGS *settings,
                          float strokewidth,
                          float scaleCoordsX, float scaleCoordsY,
                          float offsetX, float offsetY, int flags);

// Build the smoothed centerline of a subpath into out[] (room for
// GLYPH_SMOOTH_MAX), returning the point count. scale applies to coords and
// radius; corners with half_width >= radius are kept sharp (degraded).
int glyph_build_smoothed(const GlyphSegment* segs, int start, int count,
                          const FONT_SETTINGS *settings,
                          float scale,
                          float half_width,
                          float offset_x, float offset_y,
                          GlyphSmoothPt* out, int max_out);

#endif
