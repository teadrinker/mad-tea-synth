// ---- placement -------------------------------------------------------------

// What is at this screen position, as far as a label is concerned.
//
// OUTSIDE is kept apart from TAKEN because the end-of-line rules look for a
// row's last occupied cell, and the wall past the right edge is not code:
// counting it would put every row's "end of line" off the textarea. Below the
// last line of the buffer is not outside: that rest of the rect is free.
#define IC_CELL_OUTSIDE 0
#define IC_CELL_FREE    1
#define IC_CELL_TAKEN   2
static int ic_ov_cell_kind(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                                float sx, float sy) {
    int r, c;
    bool in_text = ui_textarea_screen_to_pos(ta, sx, sy, &r, &c);
    if (!in_text && !ui_textarea_screen_past_end(ta, sx, sy)) return IC_CELL_OUTSIDE;

    // Anything already painted this frame owns its cell -- first-draw-wins, so
    // drawing over it would vanish rather than blend. Mostly the scrubber's
    // digit wheels, which sit exactly where a pin on that line wants to be.
    OutputCell cell;
    if (ui_get_cell(ui, (int)sx, (int)sy, &cell) && (cell.flags & CELL_FLAGS_DRAWN))
        return IC_CELL_TAKEN;

    // Labels drawn this frame claim their padding too: two readings butted
    // together are as unreadable as one butted against code.
    for (int i = 0; i < ic->ov.num_drawn; i++) {
        ICOverlayRect *d = &ic->ov.drawn[i];
        if (sy == d->y && sx >= d->x && sx < d->x + (float)d->len) return IC_CELL_TAKEN;
    }
    if (ic->ov.block_right && sy == ic->ov.block_y && sx >= ic->ov.block_x) return IC_CELL_TAKEN;
    if (!in_text) return IC_CELL_FREE;
    char ch = ui_textarea_get_char_at(ta, r, c);   // '\0' past end-of-line
    return (ch == '\0' || ch == ' ' || ch == '\t') ? IC_CELL_FREE : IC_CELL_TAKEN;
}

// Off the rect counts as not free -- there is nothing there to draw on. A
// column of a `rows`-high block, from row sy down.
static bool ic_ov_cell_free(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                                 float sx, float sy, int rows) {
    for (int k = 0; k < rows; k++)
        if (ic_ov_cell_kind(ic, ui, ta, sx, sy + (float)k) != IC_CELL_FREE) return false;
    return true;
}

// ---- what makes one spot better than another -------------------------------
//
// Rule 1 puts the label past the end of the expression's own line when it is
// close enough. It exists because the general search is easy to talk into a
// spot several rows away when there is obvious free space at the end of the
// line, and it is the one spot that costs no row of eye movement.
//
// Rule 2 is for the rows either side: a label there reads as a CAPTION, so it
// is CENTRED on the hovered token rather than aligned on the start of the
// subtree, which on a wide expression is nowhere near the character the
// pointer is on. Taken only when it fits exactly centred.
//
// Rule 3 is rule 1 for those two rows, with a shorter reach -- they already
// cost a row of eye movement -- measured from that same centre, and needing a
// line with something ON it, past the end of a BLANK line being the middle of
// nowhere.
//
// Rule 4 is the fallback: the cheapest spot by distance, a ROW step counting a
// QUARTER of a column step, so far-to-the-right loses to directly above or
// below. Costs are quarter-columns (x * 4, y * 1) to stay in integers; on the
// expression's own row(s) distance is to its NEARER END, and off them to the
// centred position rule 2 asked for.
#define IC_INSPECT_EOL_REACH     15   // rule 1: the expression's own line
#define IC_INSPECT_EOL_REACH_ADJ 13   // rule 3: the lines either side
#define IC_INSPECT_COL_COST 4    // one column step, in quarter-columns
#define IC_INSPECT_ROW_COST 1    // one row step: a quarter of a column
// How far either side of the expression to look for a gap.
#define IC_INSPECT_SEARCH_COLS 100
// What a three-row block pays, in columns, per row it is shifted off centre.
#define IC_INSPECT_SHIFT_COLS  8

// Where a label of `need` padded cells goes at the END of screen rows sy ..
// sy+rows-1: one cell past the last occupied cell of any of them, the whole
// block free from there. Columns are offsets from base_x, and the answer is the
// PADDED block's offset. False when the rows are blank -- there is no tail to
// sit behind.
static bool ic_ov_row_tail(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                                float base_x, float sy, int rows, int need, int *out_c) {
    int last_taken = 0;
    int have       = 0;
    for (int k = 0; k < rows; k++) {
        for (int c = -IC_INSPECT_SEARCH_COLS; c <= IC_INSPECT_SEARCH_COLS; c++) {
            if (ic_ov_cell_kind(ic, ui, ta, base_x + (float)c, sy + (float)k) == IC_CELL_TAKEN &&
                (!have || c > last_taken)) {
                last_taken = c;
                have = 1;
            }
        }
    }
    if (!have) return false;

    int st = last_taken + 1;
    for (int c = st; c < st + need; c++)
        if (!ic_ov_cell_free(ic, ui, ta, base_x + (float)c, sy, rows))
            return false;

    *out_c = st;
    return true;
}

// The cost of a label whose text starts at column offset `tx` on row `sy`, in
// quarter-columns: distance to whichever END of the expression is nearer.
// `tail_dx` is the rightmost character's offset from the leftmost, ay0/ay1 the
// rows the two ends are on (equal unless wrap has split the expression).
//
// Off both of those rows the label is a caption centred on the hovered token,
// so the distance that matters is to `center_tx` -- where its text starts when
// centred -- and not to an end of the subtree columns away from the pointer.
static int ic_ov_min_cost(int tx, float sy, int tail_dx, float ay0, float ay1,
                               int center_tx) {
    int dy0 = (int)(sy - ay0); if (dy0 < 0) dy0 = -dy0;
    int dy1 = (int)(sy - ay1); if (dy1 < 0) dy1 = -dy1;

    if (dy0 && dy1) {
        int dx = tx - center_tx; if (dx < 0) dx = -dx;
        int dy = dy0 < dy1 ? dy0 : dy1;
        return dx * IC_INSPECT_COL_COST + dy * IC_INSPECT_ROW_COST;
    }

    int dx0 = tx;            if (dx0 < 0) dx0 = -dx0;
    int dx1 = tx - tail_dx;  if (dx1 < 0) dx1 = -dx1;

    int c0 = dx0 * IC_INSPECT_COL_COST + dy0 * IC_INSPECT_ROW_COST;
    int c1 = dx1 * IC_INSPECT_COL_COST + dy1 * IC_INSPECT_ROW_COST;
    return c0 < c1 ? c0 : c1;
}

// Somewhere near the expression for a label `want` cells wide.
//
// Rules 1-3 are taken outright. Failing all three, every row within
// IC_INSPECT_SEARCH_ROWS -- including the hovered one -- is scanned and the
// CHEAPEST fit wins, not the first that fits. Each row is scanned once into
// maximal free runs rather than probed per offset: screen_to_pos is not free
// and this runs every frame a label is up.
//
// The search is for a free run of want + 2*PAD and the text starts PAD into it.
//
// (ax0, ay0) is where the expression STARTS on screen and (ax1, ay1) the cell
// its LAST character is on, both already FLOORED to the grid; they differ in y
// only under word wrap. Floored because a sub-cell fraction would test one row
// while the draw landed on the boundary between two.
//
// `focus_x` is what a label on another ROW is centred on: the middle of the
// hovered TOKEN, which on a wide expression is not its start -- half a subtree
// can sit between the two. Callers with no pointer to go on (a pin) pass the
// middle of the expression instead. Cell coordinates, and a half-cell fraction
// is meaningful: an even-width token has its middle on a cell boundary.
//
// `rows` is the block's height and (*out_x, *out_y) its top row. Each rule
// reasons about the block's row nearest the expression, so a caption above
// grows upwards and one below downwards.
//
// Returns the width available at (*out_x, *out_y): `want` when something
// fitted, otherwise the widest gap found, which the caller truncates into --
// one row only; a taller block that does not fit returns 0. 0 also when there
// is nowhere usable.
static int ic_ov_place(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                            float ax0, float ay0, float ax1, float ay1,
                            float focus_x, int want, int rows, float *out_x, float *out_y) {
    // Ideally aligned with the expression itself.
    float pref_x = ax0;

    // Its two ends as column offsets from pref_x, which every distance below is
    // measured against. 0 for a single-character expression.
    int tail_dx = (int)(ax1 - ax0);
    if (tail_dx < 0) tail_dx = 0;

    int need = want + 2 * IC_INSPECT_PAD;

    // Where the TEXT starts when the label is centred on the focus: its own
    // middle over that cell, rounded so the two halves differ by at most one.
    int center_tx = (int)m_floor((double)(focus_x - pref_x)
                                 - (double)(want - 1) * 0.5 + 0.5);

    // What a reader travels to reach a label whose TEXT starts at tx, sy.
    #define IC_INS_COST(tx, sy)                                                     \
        ic_ov_min_cost((tx), (sy), tail_dx, ay0, ay1, center_tx)

    float ry = (float)(rows - 1);

    // ---- rule 1: the end of the expression's own line ----
    {
        int st;
        if (ic_ov_row_tail(ic, ui, ta, pref_x, ay1, rows, need, &st)) {
            int tx = st + IC_INSPECT_PAD;
            // Plain columns: the reach counts characters, not rule 4's cost.
            int dl = tx < 0 ? -tx : tx;
            int dr = tx - tail_dx; if (dr < 0) dr = -dr;
            if ((dr < dl ? dr : dl) <= IC_INSPECT_EOL_REACH) {
                *out_x = pref_x + (float)tx;
                *out_y = ay1;
                return want;
            }
        }
    }

    // The two rows a caption can go on, above first: a reading over the token
    // is read before the code it belongs to, one under it after.
    float adj[2] = { ay0 - 1.0f - ry, ay1 + 1.0f };
    if (ic->ov.prefer_below) { float t = adj[0]; adj[0] = adj[1]; adj[1] = t; }

    // ---- rule 2: centred on the token, on the row above or below ----
    // A blank row is fine here, unlike rule 3 -- centred on the token it is not
    // the middle of nowhere, it is directly over or under what it reads.
    for (int t = 0; t < 2; t++) {
        int st = center_tx - IC_INSPECT_PAD, fits = 1;
        for (int c = st; c < st + need; c++) {
            if (!ic_ov_cell_free(ic, ui, ta, pref_x + (float)c, adj[t], rows)) {
                fits = 0;
                break;
            }
        }
        if (!fits) continue;
        *out_x = pref_x + (float)center_tx;
        *out_y = adj[t];
        return want;
    }

    // ---- rule 3: the end of the row above, then of the row below ----
    for (int t = 0; t < 2; t++) {
        int st;
        if (!ic_ov_row_tail(ic, ui, ta, pref_x, adj[t], rows, need, &st)) continue;
        int tx = st + IC_INSPECT_PAD;
        // Plain columns, and from the centre rule 2 wanted: what matters on
        // another row is how far the label sits from the TOKEN.
        int d = tx - center_tx; if (d < 0) d = -d;
        if (d > IC_INSPECT_EOL_REACH_ADJ) continue;
        *out_x = pref_x + (float)tx;
        *out_y = adj[t];
        return want;
    }

    // ---- rule 4: the cheapest gap anywhere nearby ----
    int   best_cost = -1;                 // < 0 = nothing fits yet
    float best_x = 0, best_y = 0;
    int   wide_len = 0, wide_cost = 0;    // widest partial, for truncation
    float wide_x = 0, wide_y = 0;

    for (int dy = -IC_INSPECT_SEARCH_ROWS - (rows - 1); dy <= IC_INSPECT_SEARCH_ROWS; dy++) {
        float sy = ay0 + (float)dy;

        // The block's row nearest the expression, which is what it costs.
        float near_y = sy, near_d = -1.0f;
        for (int k = 0; k < rows; k++) {
            float ky = sy + (float)k;
            float d  = ky < ay0 ? ay0 - ky : (ky > ay1 ? ky - ay1 : 0.0f);
            if (near_d < 0.0f || d < near_d) { near_y = ky; near_d = d; }
        }

        int c = -IC_INSPECT_SEARCH_COLS;
        while (c <= IC_INSPECT_SEARCH_COLS) {
            if (!ic_ov_cell_free(ic, ui, ta, pref_x + (float)c, sy, rows)) { c++; continue; }
            int run_start = c;
            while (c <= IC_INSPECT_SEARCH_COLS &&
                   ic_ov_cell_free(ic, ui, ta, pref_x + (float)c, sy, rows)) c++;
            int run_len = c - run_start;

            if (run_len >= need) {
                // `st` starts the PADDED block and the text lands PAD cells
                // into it, so the ideal `st` is the ideal TEXT offset less PAD.
                // On the expression's own row(s) that offset is 0, aligning the
                // label on its START -- the nearer-end rule picks WHICH row,
                // not where on it. Off them it is rule 2's centre.
                int own = (near_y == ay0 || near_y == ay1);
                int lo = run_start, hi = run_start + run_len - need;
                int st = (own ? 0 : center_tx) - IC_INSPECT_PAD;
                if (st < lo) st = lo;
                if (st > hi) st = hi;

                // The TEXT's offset -- the distance a reader actually sees.
                int tx = st + IC_INSPECT_PAD;

                int cost = IC_INS_COST(tx, near_y);
                if (best_cost < 0 || cost < best_cost) {
                    best_cost = cost;
                    best_x = pref_x + (float)st;
                    best_y = sy;
                }
            } else if (rows == 1) {
                int cost = IC_INS_COST(run_start + IC_INSPECT_PAD, sy);
                if (run_len > wide_len || (run_len == wide_len && cost < wide_cost)) {
                    wide_len = run_len; wide_cost = cost;
                    wide_x = pref_x + (float)run_start;
                    wide_y = sy;
                }
            }
        }
    }
    #undef IC_INS_COST

    if (best_cost >= 0) {
        *out_x = best_x + (float)IC_INSPECT_PAD;
        *out_y = best_y;
        return want;
    }

    // The widest gap found, less the padding it also has to hold.
    int avail = wide_len - 2 * IC_INSPECT_PAD;
    if (avail >= IC_INSPECT_MIN_CHARS) {
        *out_x = wide_x + (float)IC_INSPECT_PAD;
        *out_y = wide_y;
        return avail;
    }
    return 0;
}

// A tall block is not a caption: it sits BESIDE the expression, its middle row
// on the row the expression ends on. It may shift up or down, but only so far
// that one of its rows stays on that row, and only when that brings it at least
// IC_INSPECT_SHIFT_COLS columns nearer. The cheapest fit wins, by distance from
// the text's start to the nearer end of the expression. Returns `want`, or 0
// when nothing fits -- a tall block is never truncated.
static int ic_ov_place_beside(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                                   float ax0, float ax1, float ay1, int want, int rows,
                                   float *out_x, float *out_y) {
    float pref_x = ax0;
    int tail_dx = (int)(ax1 - ax0);
    if (tail_dx < 0) tail_dx = 0;
    int need = want + 2 * IC_INSPECT_PAD;
    int mid  = (rows - 1) / 2;

    int best_cost = -1;
    float best_x = 0, best_y = 0;
    // k is the block row on ay1: centred first, then above, then below, so a
    // tie keeps the earlier.
    for (int i = 0; i < rows; i++) {
        int k = i == 0 ? mid : (i <= mid ? mid + i : mid - (i - mid));
        if (k < 0 || k >= rows) continue;
        int shift = k > mid ? k - mid : mid - k;
        float sy = ay1 - (float)k;

        int c = -IC_INSPECT_SEARCH_COLS;
        while (c <= IC_INSPECT_SEARCH_COLS) {
            if (!ic_ov_cell_free(ic, ui, ta, pref_x + (float)c, sy, rows)) { c++; continue; }
            int run_start = c;
            while (c <= IC_INSPECT_SEARCH_COLS &&
                   ic_ov_cell_free(ic, ui, ta, pref_x + (float)c, sy, rows)) c++;
            int lo = run_start, hi = c - need;
            if (hi < lo) continue;

            int ends[2] = { 0, tail_dx };
            for (int e = 0; e < 2; e++) {
                int st = ends[e] - IC_INSPECT_PAD;
                if (st < lo) st = lo;
                if (st > hi) st = hi;
                int tx = st + IC_INSPECT_PAD;
                int d0 = tx < 0 ? -tx : tx;
                int d1 = tx - tail_dx; if (d1 < 0) d1 = -d1;
                int cost = (d0 < d1 ? d0 : d1) + shift * IC_INSPECT_SHIFT_COLS;
                if (best_cost < 0 || cost < best_cost) {
                    best_cost = cost;
                    best_x = pref_x + (float)tx;
                    best_y = sy;
                }
            }
        }
    }
    if (best_cost < 0) return 0;
    *out_x = best_x;
    *out_y = best_y;
    return want;
}

static bool ic_ov_is_explicit(const char *t) {
    for (; *t; t++)
        if (*t == '\n') return true;
    return false;
}

// Split a copy of a label into its rows: the '\n'-separated ones when the text
// has them, else where ic_inspect_split says -- the same cuts ic_ov_place_label
// measured. Returns the row count.
static int ic_ov_label_rows(char *draw, int rows, bool explicit_rows, char **out) {
    out[0] = draw;
    if (explicit_rows) {
        int n = 1;
        for (char *c = draw; *c && n < IC_OV_MAX_ROWS; c++) {
            if (*c != '\n') continue;
            *c = '\0';
            out[n++] = c + 1;
        }
        return n;
    }
    int cuts[2];
    if (rows < 2 || ic_inspect_split(draw, rows, cuts) < 0) return 1;
    for (int k = 0; k < rows - 1; k++) {
        draw[cuts[k]] = '\0';
        out[k + 1] = draw + cuts[k] + 1;
    }
    return rows;
}

// The row count and widest row of a label of explicit rows.
static int ic_ov_explicit_size(const char *label, int *out_w) {
    char draw[IC_OV_TEXT_MAX];
    s_strncpy(draw, label, sizeof(draw) - 1);
    draw[sizeof(draw) - 1] = '\0';
    char *row[IC_OV_MAX_ROWS];
    int n = ic_ov_label_rows(draw, 0, true, row);
    int w = 0;
    for (int k = 0; k < n; k++) {
        int rl = (int)s_strlen(row[k]);
        if (rl > w) w = rl;
    }
    *out_w = w;
    return n;
}

// Claim the cells a label and its padding cover, a rect per row, so the next
// label this frame treats them as occupied. Painted by ic_overlay_flush once
// every label is placed, so two that touch can still be pulled apart. `ay1` is
// the row the expression ends on.
static void ic_ov_claim(InteractiveCoding *ic, ICOverlayKind kind, const char *label, float x, float y,
                        int avail, int rows, bool explicit_rows, float ay1, bool fixed) {
    ICOverlay *ov = &ic->ov;
    if (avail <= 0 || ov->num_placed >= IC_OV_MAX_LABELS) return;
    char draw[IC_OV_TEXT_MAX];
    s_strncpy(draw, label, sizeof(draw) - 1);
    draw[sizeof(draw) - 1] = '\0';
    char *row[IC_OV_MAX_ROWS];
    int n = ic_ov_label_rows(draw, rows, explicit_rows, row);
    if (ov->num_drawn + n > IC_OV_MAX_RECTS) return;

    ICOverlayPlaced *p = &ov->placed[ov->num_placed++];
    p->label         = label;
    p->x             = x;
    p->y             = y;
    p->ay1           = ay1;
    p->avail         = avail;
    p->rows          = n;
    p->kind          = kind;
    p->explicit_rows = explicit_rows;
    p->fixed         = fixed;
    p->r0            = ov->num_drawn;
    for (int k = 0; k < n; k++) {
        int len = (int)s_strlen(row[k]);
        if (avail < len) len = ic_inspect_truncate(row[k], avail);
        // Padding claimed with the text -- see ic_ov_cell_kind. A row cut
        // to nothing claims no cell.
        ICOverlayRect *r = &ov->drawn[ov->num_drawn++];
        r->x   = x - (float)IC_INSPECT_PAD;
        r->y   = y + (float)k;
        r->len = len > 0 ? len + 2 * IC_INSPECT_PAD : 0;
    }
}

// A placed label's text as an inclusive box, and its cell count. False when it
// claims no cell.
static bool ic_ov_placed_box(InteractiveCoding *ic, const ICOverlayPlaced *p,
                             int *x0, int *x1, int *y0, int *y1, int *cells) {
    int w = 0, sum = 0;
    for (int k = 0; k < p->rows; k++) {
        int len = ic->ov.drawn[p->r0 + k].len - 2 * IC_INSPECT_PAD;
        if (len <= 0) continue;
        if (len > w) w = len;
        sum += len;
    }
    if (w <= 0) return false;
    *x0 = (int)p->x; *x1 = (int)p->x + w - 1;
    *y0 = (int)p->y; *y1 = (int)p->y + p->rows - 1;
    if (cells) *cells = sum;
    return true;
}

// No blank row or column between two labels, corners included.
static bool ic_ov_placed_touch(InteractiveCoding *ic, const ICOverlayPlaced *a,
                               const ICOverlayPlaced *b) {
    int ax0, ax1, ay0, ay1, bx0, bx1, by0, by1;
    if (!ic_ov_placed_box(ic, a, &ax0, &ax1, &ay0, &ay1, NULL)) return false;
    if (!ic_ov_placed_box(ic, b, &bx0, &bx1, &by0, &by1, NULL)) return false;
    int gx = (ax0 > bx0 ? ax0 : bx0) - (ax1 < bx1 ? ax1 : bx1) - 1;
    int gy = (ay0 > by0 ? ay0 : by0) - (ay1 < by1 ? ay1 : by1) - 1;
    return gx <= 0 && gy <= 0;
}

// Shift placed label `i` by `dy` rows, when every cell it and its padding would
// cover is free and it would touch no other label there. A three-row block
// keeps a row on its expression's (see ic_ov_place_beside).
static bool ic_ov_placed_move(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                              int i, int dy) {
    ICOverlayPlaced *p = &ic->ov.placed[i];
    ICOverlayRect   *r = &ic->ov.drawn[p->r0];
    float oy = p->y, ny = p->y + (float)dy;
    if (p->rows == 3 && !p->explicit_rows && (p->ay1 < ny || p->ay1 > ny + 2.0f)) return false;

    // Out of its own way while its new cells are tested.
    int lens[IC_OV_MAX_ROWS];
    for (int k = 0; k < p->rows; k++) { lens[k] = r[k].len; r[k].len = 0; }
    bool ok = true;
    for (int k = 0; k < p->rows && ok; k++)
        for (int c = 0; c < lens[k] && ok; c++)
            ok = ic_ov_cell_kind(ic, ui, ta, r[k].x + (float)c, ny + (float)k) == IC_CELL_FREE;
    for (int k = 0; k < p->rows; k++) r[k].len = lens[k];
    if (!ok) return false;

    p->y = ny;
    for (int k = 0; k < p->rows; k++) r[k].y = ny + (float)k;
    for (int j = 0; j < ic->ov.num_placed && ok; j++)
        if (j != i && ic_ov_placed_touch(ic, p, &ic->ov.placed[j])) ok = false;
    if (ok) return true;
    p->y = oy;
    for (int k = 0; k < p->rows; k++) r[k].y = oy + (float)k;
    return false;
}

// Two labels stacked with no row between them read as one. The smaller moves a
// row away from the other -- the later of a tie, which is the hover -- and
// failing that the larger moves the other way. A menu never moves: it is what
// the user is acting on, so whatever touches it gives way -- as does a label
// held to its anchor's row (see IC_OV_SAME_ROW).
static void ic_ov_separate_labels(InteractiveCoding *ic, UIContext *ui, UITextArea *ta) {
    for (int i = 0; i < ic->ov.num_placed; i++) {
        for (int j = i + 1; j < ic->ov.num_placed; j++) {
            ICOverlayPlaced *a = &ic->ov.placed[i], *b = &ic->ov.placed[j];
            int ax0, ax1, ay0, ay1, bx0, bx1, by0, by1, na, nb;
            if (!ic_ov_placed_box(ic, a, &ax0, &ax1, &ay0, &ay1, &na)) continue;
            if (!ic_ov_placed_box(ic, b, &bx0, &bx1, &by0, &by1, &nb)) continue;
            if (ay0 <= by1 && by0 <= ay1) continue;          // side by side, not stacked
            if (!ic_ov_placed_touch(ic, a, b)) continue;
            bool am = a->kind == IC_OV_MENU || a->fixed, bm = b->kind == IC_OV_MENU || b->fixed;
            if (am && bm) continue;
            if (am || bm) {
                int o  = am ? j : i;
                int dy = ic->ov.placed[o].y < ic->ov.placed[am ? i : j].y ? -1 : 1;
                if (!ic_ov_placed_move(ic, ui, ta, o, dy)) ic_ov_placed_move(ic, ui, ta, o, -dy);
                continue;
            }
            int s = na < nb ? i : j, l = s == i ? j : i;
            int dy = ic->ov.placed[s].y < ic->ov.placed[l].y ? -1 : 1;
            if (!ic_ov_placed_move(ic, ui, ta, s, dy))
                ic_ov_placed_move(ic, ui, ta, l, -dy);
        }
    }
}

// Green for a pin, comment-colour for the transient readings and hints.
static unsigned int ic_ov_color(UIContext *ui, ICOverlayKind kind) {
    switch (kind) {
    case IC_OV_PIN:  return COL_INSPECT(ui);
    case IC_OV_WARN: return COL_ERROR(ui);
    case IC_OV_MENU: return COL_IDENT(ui);
    default:         return COL_COMMENT(ui);
    }
}

// ic_ov_place for a whole label. Explicit rows are placed as the block they
// are, or not at all. Otherwise one past IC_INSPECT_SPLIT3_LEN first tries a
// three-row block a third as wide beside the expression (see
// ic_ov_place_beside), then one past IC_INSPECT_SPLIT_LEN a two-row block half
// as wide, split between values; failing those it is one row as before,
// truncated into the widest gap if it must be.
static int ic_ov_place_label(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                             float ax0, float ay0, float ax1, float ay1, float focus_x,
                             const char *label, unsigned flags,
                             float *out_x, float *out_y, int *out_rows) {
    int len = (int)s_strlen(label);
    *out_rows = 1;
    if (ic_ov_is_explicit(label)) {
        int w;
        int n = ic_ov_explicit_size(label, &w);
        if (w <= 0) return 0;
        int got = ic_ov_place(ic, ui, ta, ax0, ay0, ax1, ay1, focus_x, w, n, out_x, out_y);
        if (got != w) return 0;
        *out_rows = n;
        return w;
    }
    for (int rows = 3; rows >= 2 && !(flags & IC_OV_ONE_ROW); rows--) {
        if (len <= (rows == 3 ? IC_INSPECT_SPLIT3_LEN : IC_INSPECT_SPLIT_LEN)) continue;
        int cuts[2];
        int w = ic_inspect_split(label, rows, cuts);
        if (w <= 0) continue;
        int got = rows == 3 ? ic_ov_place_beside(ic, ui, ta, ax0, ax1, ay1, w, rows, out_x, out_y)
                            : ic_ov_place(ic, ui, ta, ax0, ay0, ax1, ay1, focus_x, w, rows, out_x, out_y);
        if (got == w) {
            *out_rows = rows;
            return w;
        }
    }
    return ic_ov_place(ic, ui, ta, ax0, ay0, ax1, ay1, focus_x, len, 1, out_x, out_y);
}

// Whether a placement showed the whole label rather than a truncation of it.
static bool ic_ov_fits_whole(const char *text, int avail, int rows) {
    if (avail <= 0) return false;
    return rows > 1 || avail >= (int)s_strlen(text);
}

// The row over or under the anchor with the text's `align_at` character
// exactly over the buffer column `align_col`. False when neither row has the
// cells free.
static bool ic_ov_place_aligned(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                                const ICOverlayLabel *l, const char *text, int align_at,
                                float ay0, float ay1, float *out_x, float *out_y) {
    float cx, cy;
    if (!ui_textarea_pos_to_screen(ta, l->row, l->align_col, &cx, &cy)) return false;
    int len = (int)s_strlen(text);
    if (len <= 0 || ic_ov_is_explicit(text)) return false;
    float x = (float)m_floor(cx) - (float)align_at;
    float rows_y[2] = { ay0 - 1.0f, ay1 + 1.0f };
    if (ic->ov.prefer_below) { float t = rows_y[0]; rows_y[0] = rows_y[1]; rows_y[1] = t; }
    for (int t = 0; t < 2; t++) {
        bool free_run = true;
        for (int c = -IC_INSPECT_PAD; c < len + IC_INSPECT_PAD && free_run; c++)
            free_run = ic_ov_cell_free(ic, ui, ta, x + (float)c, rows_y[t], 1);
        if (!free_run) continue;
        *out_x = x;
        *out_y = rows_y[t];
        return true;
    }
    return false;
}

// The cells from two past the anchor to the textarea's right edge, however
// they are occupied: the label goes there regardless. Clipped to the edge.
static int ic_ov_same_row_room(InteractiveCoding *ic, UIContext *ui, UITextArea *ta,
                               float x, float y, int want) {
    int n = 0;
    while (n < want && ic_ov_cell_kind(ic, ui, ta, x + (float)n, y) != IC_CELL_OUTSIDE) n++;
    return n;
}

void ic_overlay_begin(InteractiveCoding *ic) {
    ic->ov.menu_placed  = false;
    ic->ov.num_drawn    = 0;
    ic->ov.num_placed   = 0;
    ic->ov.ta           = NULL;
    ic->ov.block_right  = false;
    ic->ov.prefer_below = false;
    ic->ov.hint_added   = false;
}

// Place a label near its anchor and claim its cells. Labels claim first-come:
// the caller adds in priority order. Painted by ic_overlay_flush.
void ic_overlay_add(InteractiveCoding *ic, UIContext *ui, UITextArea *ta, const ICOverlayLabel *l) {
    ICOverlay *ov = &ic->ov;
    if (!l->text || !l->text[0] || l->row < 0) return;
    ICOverlayCache *cache = l->cache;
    ov->ta = ta;

    float ax, ay, ax2, ay2;
    if (!ui_textarea_pos_to_screen(ta, l->row, l->col, &ax, &ay)) {
        if (cache) cache->placed = 0;       // scrolled out of view
        return;
    }
    ax = (float)m_floor(ax); ay = (float)m_floor(ay);
    // The tail can be off-screen while the head is not. Falling back on the
    // head keeps the label drawn, measured start-only for that frame.
    if (!ui_textarea_pos_to_screen(ta, l->row2, l->col2, &ax2, &ay2)) {
        ax2 = ax; ay2 = ay;
    } else {
        ax2 = (float)m_floor(ax2); ay2 = (float)m_floor(ay2);
    }
    float focus = l->focus_x >= 0.0f ? l->focus_x : ((ay2 == ay) ? (ax + ax2) * 0.5f : ax);

    const char *texts[3] = { l->text, l->alt_text, l->alt2_text };
    float x = 0, y = 0;
    int   avail = 0, rows = 1, chosen = 0;

    if (cache && cache->placed && !ov->force_place &&
        ax == cache->place_ax && ay == cache->place_ay &&
        ax2 == cache->place_ax2 && ay2 == cache->place_ay2) {
        x = cache->place_x; y = cache->place_y;
        avail = cache->place_avail; rows = cache->place_rows; chosen = cache->place_alt;
        if (!texts[chosen]) chosen = 0;
    } else {
        bool done = false;
        if (l->flags & IC_OV_AT) {
            int w = 0, n = ic_ov_explicit_size(texts[0], &w);
            x = l->at_x; y = l->at_y; avail = w; rows = n; chosen = 0; done = true;
        }
        ov->block_right  = (l->flags & IC_OV_NO_SAME_ROW_RIGHT) != 0;
        ov->block_x      = ax2 + 1.0f;
        ov->block_y      = ay2;
        ov->prefer_below = (l->flags & IC_OV_PREFER_BELOW) != 0;

        if (l->flags & IC_OV_SAME_ROW) {
            int best = 0;
            for (int t = 0; t < 3 && texts[t]; t++) {
                int len = (int)s_strlen(texts[t]);
                int room = ic_ov_same_row_room(ic, ui, ta, ax2 + 2.0f, ay2, len);
                if (room == len) { x = ax2 + 2.0f; y = ay2; avail = room; chosen = t; best = room; break; }
                if (room > best) { x = ax2 + 2.0f; y = ay2; avail = room; chosen = t; best = room; }
            }
            if (best < IC_INSPECT_MIN_CHARS && best < (int)s_strlen(texts[chosen])) avail = 0;
            done = true;
        }
        if (l->align_col >= 0 && !done) {
            for (int t = 0; t < 3 && texts[t] && !done; t++) {
                if (!ic_ov_place_aligned(ic, ui, ta, l, texts[t], t == 0 ? l->align_at : l->align_at_alt, ay, ay2, &x, &y)) continue;
                avail = (int)s_strlen(texts[t]); rows = 1; chosen = t; done = true;
            }
        }
        // The first form that fits whole wins; failing that, the widest partial.
        int best_avail = 0;
        for (int t = 0; t < 3 && texts[t] && !done; t++) {
            float tx, ty; int trows;
            int got = ic_ov_place_label(ic, ui, ta, ax, ay, ax2, ay2, focus, texts[t], l->flags,
                                        &tx, &ty, &trows);
            if (ic_ov_fits_whole(texts[t], got, trows)) {
                x = tx; y = ty; avail = got; rows = trows; chosen = t; done = true;
            } else if (got > best_avail) {
                x = tx; y = ty; avail = got; rows = trows; chosen = t; best_avail = got;
            }
        }
        ov->block_right  = false;
        ov->prefer_below = false;

        if (cache) {
            cache->place_ax  = ax;  cache->place_ay  = ay;
            cache->place_ax2 = ax2; cache->place_ay2 = ay2;
            cache->place_x = x; cache->place_y = y;
            cache->place_avail = avail; cache->place_rows = rows; cache->place_alt = chosen;
            cache->placed = 1;
        }
    }
    ic_ov_claim(ic, l->kind, texts[chosen], x, y, avail, rows, ic_ov_is_explicit(texts[chosen]), ay2,
                (l->flags & IC_OV_SAME_ROW) != 0);
}

// Separate, then paint, every label claimed this frame.
void ic_overlay_flush(InteractiveCoding *ic, UIContext *ui) {
    if (ic->ov.ta) ic_ov_separate_labels(ic, ui, ic->ov.ta);
    for (int i = 0; i < ic->ov.num_placed; i++) {
        ICOverlayPlaced *p = &ic->ov.placed[i];
        if (p->kind == IC_OV_MENU) {
            ic->ov.menu_placed = true;
            ic->ov.menu_label  = p->label;
            ic->ov.menu_x      = p->x;
            ic->ov.menu_y      = p->y;
            ic->ov.menu_avail  = p->avail;
            ic->ov.menu_rows   = p->rows;
            continue;
        }
        char draw[IC_OV_TEXT_MAX];
        s_strncpy(draw, p->label, sizeof(draw) - 1);
        draw[sizeof(draw) - 1] = '\0';
        char *row[IC_OV_MAX_ROWS];
        int n = ic_ov_label_rows(draw, p->rows, p->explicit_rows, row);
        unsigned int col = ic_ov_color(ui, p->kind);
        for (int k = 0; k < n; k++) {
            int len = (int)s_strlen(row[k]);
            if (p->avail < len && ic_inspect_truncate(row[k], p->avail) <= 0) continue;
            for (int c = 0; row[k][c]; c++)
                ui_draw_cell_flags_weight(ui, p->x + (float)c, p->y + (float)k,
                                          (unsigned char)row[k][c], col, 0, ui->global_weight);
        }
    }
    ic->ov.num_placed = 0;
}
