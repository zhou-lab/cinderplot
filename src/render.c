/* render.c — spec + data -> measured gtable -> cairo PDF.
 *
 * Scales are continuous with an optional log10 transform: all layout,
 * binning and breaks happen in TRANSFORMED space; tick labels show data-
 * space values (ggplot semantics). Layers render in spec order.
 *
 * Facet layout follows ggplot2's facet_wrap (dims = rev(n2mfrow(n)),
 * shared scales, strips above panels, staircase axes) and facet_grid
 * (one panel per row x column combination, the empty ones included,
 * column strips on top and rotated row strips on the right). */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Col C_BAR = {0.349, 0.349, 0.349};   /* grey35, ggplot bar fill */

static double text_w(cairo_t *cr, double size, const char *s) {
    cairo_text_extents_t e;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}
static double font_h(cairo_t *cr, double size) {
    cairo_font_extents_t fe;
    cairo_set_font_size(cr, size);
    cairo_font_extents(cr, &fe);
    return fe.ascent + fe.descent;
}

/* estimated multi-column legend width, for the automatic fold */
static double leg_est_w(cairo_t *cr, char **lv, int k, int nc) {
    int rows = (k + nc - 1) / nc;
    double total = 0;
    for (int c = 0; c < nc; c++) {
        double lw = 0;
        for (int i = c * rows; i < (c + 1) * rows && i < k; i++) {
            double w = text_w(cr, SZ_AXIS_TEXT, lv[i]);
            if (w > lw) lw = w;
        }
        total += KEY_SIZE + TXT_GAP + lw + (c < nc - 1 ? HALF_LINE : 0);
    }
    return total;
}

static GTable *build_legend(cairo_t *cr, const Theme *th, const char *title, const Factor *f,
                            const Col *pal, int haspoint, int hasline, int hasbox, int hastext,
                            const int *shapes, int ncol, int reverse, char *err) {
    if (ncol < 1) ncol = 1;
    if (ncol > f->nlev) ncol = f->nlev;
    int rows = (f->nlev + ncol - 1) / ncol;
    /* The table takes 4 grid columns per legend column and 2 rows per entry
     * row out of GT_MAXDIM each; a fold past that wrote off the end of colw
     * (guide_legend(nrow=1) on a 100-level factor did). Refuse it by name. */
    if (4 * ncol - 1 > GT_MAXDIM || 2 + 2 * rows - 1 > GT_MAXDIM) {
        snprintf(err, CP_ERRLEN, "legend `%s` with %d levels folded to %d "
                 "column%s and %d row%s exceeds the layout limit; use "
                 "guide_legend(nrow=/ncol=) smaller", title ? title : "",
                 f->nlev, ncol, ncol == 1 ? "" : "s", rows, rows == 1 ? "" : "s");
        return NULL;
    }
    GTable *t = cp_xcalloc(1, sizeof(GTable));
    /* column-major fill (ggplot guide_legend byrow=FALSE): entry i sits in
     * column i/rows, row i%rows. Each column is [key][gap][labels-of-that-
     * column's-width], with a gutter between columns. */
    double *lw = cp_xcalloc(ncol, sizeof(double));
    double total = 0;
    for (int c = 0; c < ncol; c++) {
        lw[c] = 0;
        for (int di = c * rows; di < (c + 1) * rows && di < f->nlev; di++) {
            int i = reverse ? f->nlev - 1 - di : di;
            double w = text_w(cr, SZ_AXIS_TEXT, f->levels[i]);
            if (w > lw[c]) lw[c] = w;
        }
        total += KEY_SIZE + TXT_GAP + lw[c] + (c < ncol - 1 ? HALF_LINE : 0);
    }
    double title_w = text_w(cr, SZ_BASE, title);
    if (title_w > total) lw[ncol - 1] += title_w - total;

    t->ncol = 4 * ncol - 1;
    for (int c = 0; c < ncol; c++) {
        t->colw[4 * c]     = upt(KEY_SIZE);
        t->colw[4 * c + 1] = upt(TXT_GAP);
        t->colw[4 * c + 2] = upt(lw[c]);
        if (c < ncol - 1) t->colw[4 * c + 3] = upt(HALF_LINE);
    }
    free(lw);
    int has_title = title && *title;
    t->nrow = 2 + 2 * rows - 1;
    t->rowh[0] = upt(has_title ? font_h(cr, SZ_BASE) : 0);
    t->rowh[1] = upt(has_title ? HALF_LINE : 0);
    for (int i = 0; i < rows; i++) {
        t->rowh[2 + 2 * i] = upt(KEY_SIZE);
        if (i < rows - 1) t->rowh[3 + 2 * i] = upt(HALF_LINE * 0.4);
    }

    Grob *g;
    if (has_title) {
        g = gt_add(t, G_TEXT, 0, 0, 0, t->ncol - 1);
        g->str = title; g->size = SZ_BASE; g->col = th->title;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    static const double half = 0.5;
    for (int di = 0; di < f->nlev; di++) {
        /* guide_legend(reverse=TRUE): flip the DISPLAY order only — draw
         * order and colours stay tied to the factor levels */
        int i = reverse ? f->nlev - 1 - di : di;
        int r = 2 + 2 * (di % rows), kc = 4 * (di / rows);
        if (th->key_bg_on) { g = gt_add(t, G_RECT, r, kc, r, kc); g->col = th->key_bg; }
        if (hasbox) {
            g = gt_add(t, G_RECT, r, kc, r, kc);
            g->col = pal[i]; g->sub = 1;
            g->x0 = 0.15; g->x1 = 0.85; g->y0 = 0.15; g->y1 = 0.85;
        }
        if (hasline) {
            g = gt_add(t, G_LINE, r, kc, r, kc);
            g->col = pal[i]; g->lw = lw_pt(0.5);
            g->x0 = 0.1; g->x1 = 0.9; g->y0 = g->y1 = 0.5;
        }
        if (haspoint) {
            g = gt_add(t, G_POINTS, r, kc, r, kc);
            g->n = 1; g->px = &half; g->py = &half;
            g->pcol = &pal[i]; g->radius = PT_RADIUS;
            if (shapes) g->shape = shapes[i];   /* the key IS the glyph here */
        }
        if (hastext) {                       /* geom_text/geom_label key: a letter */
            g = gt_add(t, G_TEXT, r, kc, r, kc);
            g->str = "a"; g->size = SZ_AXIS_TEXT; g->col = pal[i];
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }
        g = gt_add(t, G_TEXT, r, kc + 2, r, kc + 2);
        g->str = f->levels[i]; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = 0; g->ty = 0.5; g->hj = 0; g->va = V_INKCENTER;
    }
    return t;
}

/* continuous-colour legend: a vertical colorbar with tick labels */
static GTable *build_colorbar_legend(cairo_t *cr, const Theme *th, const char *title,
                                     const FillScale *fs, double lo, double hi) {
    const double BARW = 12, BARH = 80;      /* colorbar pt dimensions */
    double br[16];
    int nb = extended_breaks(lo, hi, 5, br, 16), nf = 0;
    for (int i = 0; i < nb; i++) if (br[i] >= lo && br[i] <= hi) br[nf++] = br[i];
    int dec = axis_decimals(br, nf);
    double labw = 0;
    for (int i = 0; i < nf; i++) {
        char b[32]; fmt_break(br[i], dec, b, sizeof b);
        double w = text_w(cr, SZ_AXIS_TEXT, b);
        if (w > labw) labw = w;
    }
    double barcol = BARW + TICK_LEN + TXT_GAP + labw;
    double titlew = title ? text_w(cr, SZ_BASE, title) : 0;
    double w = fmax(barcol, titlew);

    GTable *t = cp_xcalloc(1, sizeof(GTable));
    t->ncol = 1; t->colw[0] = upt(w);
    t->nrow = 3;
    t->rowh[0] = upt(title ? font_h(cr, SZ_BASE) : 0);
    t->rowh[1] = upt(title ? HALF_LINE : 0);
    t->rowh[2] = upt(BARH);

    Grob *g;
    if (title) {
        g = gt_add(t, G_TEXT, 0, 0, 0, 0);
        g->str = title; g->size = SZ_BASE; g->col = th->title;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    const int NSTEP = 64;
    double barw_npc = BARW / w;
    for (int k = 0; k < NSTEP; k++) {       /* colorbar strips (value-correct) */
        double v = lo + (k + 0.5) / NSTEP * (hi - lo);
        g = gt_add(t, G_RECT, 2, 0, 2, 0);
        g->sub = 1; g->col = fill_map_value(fs, v, lo, hi);
        g->x0 = 0; g->x1 = barw_npc;
        g->y0 = (double)k / NSTEP; g->y1 = (double)(k + 1) / NSTEP;
    }
    for (int i = 0; i < nf; i++) {           /* ticks + labels */
        double frac = hi > lo ? (br[i] - lo) / (hi - lo) : 0.5;
        char *lab = cp_xmalloc(32); fmt_break(br[i], dec, lab, 32);
        g = gt_add(t, G_LINE, 2, 0, 2, 0);
        g->col = th->tick; g->lw = lw_pt(0.5);
        g->x0 = barw_npc; g->x1 = barw_npc + TICK_LEN / w; g->y0 = g->y1 = frac;
        g = gt_add(t, G_TEXT, 2, 0, 2, 0);
        g->str = lab; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = (BARW + TICK_LEN + TXT_GAP) / w; g->ty = frac; g->hj = 0; g->va = V_INKCENTER;
    }
    return t;
}

/* ---- aes(size=): map a numeric value to a point radius. ggplot's default
 * scale_size_continuous uses area_pal(range=c(1,6)): the value is rescaled to
 * [0,1], then the SIZE aesthetic = 1 + 5*sqrt(t) (so radius scales as sqrt of
 * value -> area is linear in value). The size aesthetic is converted to a pt
 * radius with the same factor geom_point(size=) uses (PT_RADIUS at size 1.5). */
static double size_to_radius(double v, double lo, double hi) {
    double t = hi > lo ? (v - lo) / (hi - lo) : 0.5;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double sz = 1.0 + (6.0 - 1.0) * sqrt(t);   /* area_pal(range = c(1, 6)) */
    return PT_RADIUS * sz / 1.5;
}

/* size legend: a few representative breaks, each a black circle at its mapped
 * radius plus the value label (mirrors build_legend's discrete-key layout) */
static GTable *build_size_legend(cairo_t *cr, const Theme *th, const char *title,
                                 const double *br, const double *radii, int nb, int dec) {
    GTable *t = cp_xcalloc(1, sizeof(GTable));
    /* the table needs 2 + 2*nb - 1 rows in rowh[GT_MAXDIM]; cap the break
     * count so it can never overflow (defensive: extended_breaks(m=5) stays
     * well under this today, but the bound must not depend on that). */
    int nbmax = (GT_MAXDIM - 1) / 2;
    if (nb > nbmax) nb = nbmax;
    char **labs = cp_xmalloc(nb * sizeof(char *));
    double label_w = 0;
    for (int i = 0; i < nb; i++) {
        labs[i] = cp_xmalloc(32); fmt_break(br[i], dec, labs[i], 32);
        double w = text_w(cr, SZ_AXIS_TEXT, labs[i]);
        if (w > label_w) label_w = w;
    }
    double title_w = title ? text_w(cr, SZ_BASE, title) : 0;
    if (title_w > KEY_SIZE + TXT_GAP + label_w)
        label_w = title_w - KEY_SIZE - TXT_GAP;

    t->ncol = 3;
    t->colw[0] = upt(KEY_SIZE);
    t->colw[1] = upt(TXT_GAP);
    t->colw[2] = upt(label_w);
    t->nrow = 2 + 2 * nb - 1;
    t->rowh[0] = upt(title ? font_h(cr, SZ_BASE) : 0);
    t->rowh[1] = upt(title ? HALF_LINE : 0);
    for (int i = 0; i < nb; i++) {
        t->rowh[2 + 2 * i] = upt(KEY_SIZE);
        if (i < nb - 1) t->rowh[3 + 2 * i] = upt(HALF_LINE * 0.4);
    }

    Grob *g;
    if (title) {
        g = gt_add(t, G_TEXT, 0, 0, 0, 2);
        g->str = title; g->size = SZ_BASE; g->col = th->title;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    static const double half = 0.5;
    Col *black = cp_xmalloc(sizeof(Col)); *black = C_BLACK;
    for (int i = 0; i < nb; i++) {
        int r = 2 + 2 * i;
        if (th->key_bg_on) { g = gt_add(t, G_RECT, r, 0, r, 0); g->col = th->key_bg; }
        g = gt_add(t, G_POINTS, r, 0, r, 0);
        g->n = 1; g->px = &half; g->py = &half;
        g->pcol = black; g->radius = radii[i];
        g = gt_add(t, G_TEXT, r, 2, r, 2);
        g->str = labs[i]; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = 0; g->ty = 0.5; g->hj = 0; g->va = V_INKCENTER;
    }
    return t;
}

/* Vertically stack several guide sub-tables (colour + size) into one legend
 * block, ggplot-style: each guide centred in a fixed-height row, gaps between,
 * column width = the widest guide. A single guide is returned unchanged. */
static GTable *stack_guides(GTable **gs, int n) {
    if (n == 1) return gs[0];
    GTable *c = cp_xcalloc(1, sizeof(GTable));
    double w = 0;
    for (int i = 0; i < n; i++) w = fmax(w, gt_fixed_w(gs[i]));
    c->ncol = 1; c->colw[0] = upt(w);
    c->nrow = 2 * n - 1;
    for (int i = 0; i < n; i++) {
        c->rowh[2 * i] = upt(gt_fixed_h(gs[i]));
        if (i < n - 1) c->rowh[2 * i + 1] = upt(2 * HALF_LINE);   /* legend.spacing */
    }
    for (int i = 0; i < n; i++) {
        Grob *g = gt_add(c, G_TABLE, 2 * i, 0, 2 * i, 0);
        g->child = gs[i];
    }
    return c;
}

/* coord_flip(): transpose a panel-content grob's npc coordinates (x <-> y).
 * Every panel grob is placed in npc [0,1]^2 within the panel cell, so swapping
 * the two axes is a valid flip; the axes/gridlines are re-pointed separately. */
static void flip_grob(Grob *g) {
    double t;
    t = g->x0; g->x0 = g->y0; g->y0 = t;
    t = g->x1; g->x1 = g->y1; g->y1 = t;
    t = g->tx; g->tx = g->ty; g->ty = t;
    const double *p = g->px; g->px = g->py; g->py = p;
}

/* minor breaks: midpoints between majors in transformed space, extended
 * one gap beyond each end, filtered to the limits */
static int make_minors(const double *maj, int nmaj, double lo, double hi, double *out) {
    if (nmaj < 2) return 0;
    int n = 0;
    for (int i = -1; i < nmaj; i++) {
        double gap = i < 0 ? maj[1] - maj[0]
                   : i + 1 < nmaj ? maj[i + 1] - maj[i] : maj[i] - maj[i - 1];
        double m = i < 0 ? maj[0] - gap / 2 : maj[i] + gap / 2;
        if (m >= lo && m <= hi) out[n++] = m;
    }
    return n;
}

/* The sub-structure drawn between consecutive powers of the base, as offsets in
 * transformed space within one step: for base 10 the familiar 1..9 ladder, for
 * base 2 just the midpoint 1.5 (2..9 would land outside the octave, and the one
 * interior point is what ggplot's log2 minor breaks amount to). Index 0 is the
 * power itself. */
static int log_subdiv(int base, double *off, double *val) {
    if (base == 2) {
        off[0] = 0.0; val[0] = 1.0;
        off[1] = log2(1.5); val[1] = 1.5;
        return 2;
    }
    for (int d = 1; d <= 9; d++) { off[d - 1] = log10((double)d); val[d - 1] = d; }
    return 9;
}

/* log minor breaks: the sub-division above repeated over every power, in
 * transformed space, filtered to [lo, hi] — the characteristic log grid.
 * Majors are drawn on top, so a minor coinciding with a major is simply
 * covered. */
static int log_minors(int base, double lo, double hi, double *out, int max_out) {
    double off[9], val[9];
    int nd = log_subdiv(base, off, val);
    int n = 0;
    for (int k = (int)floor(lo) - 1; k <= (int)ceil(hi) + 1 && n < max_out; k++)
        for (int d = 1; d < nd && n < max_out; d++) {   /* skip the power itself */
            double t = k + off[d];
            if (t >= lo - 1e-9 && t <= hi + 1e-9) out[n++] = t;
        }
    return n;
}

/* log tick marks (ggplot annotation_logticks): the sub-division ladder over
 * every power, in transformed space, with a length per mark — long at the power
 * itself, mid at the half-way value (5 for base 10, 1.5 for base 2), short at
 * the rest. Positions are transformed-space; the caller maps them to npc.
 * Lengths in points (ggplot defaults 0.3/0.2/0.1 cm). */
static int log_tick_marks(int base, double lo, double hi, double *pos,
                          double *len, int max_out) {
    const double CM_PT = 72.0 / 2.54;
    const double LONG = 0.30 * CM_PT, MID = 0.20 * CM_PT, SHORT = 0.10 * CM_PT;
    double off[9], val[9];
    int nd = log_subdiv(base, off, val);
    double half = base == 2 ? 1.5 : 5.0;
    int n = 0;
    for (int k = (int)floor(lo) - 1; k <= (int)ceil(hi) + 1 && n < max_out; k++)
        for (int d = 0; d < nd && n < max_out; d++) {
            double t = k + off[d];
            if (t < lo - 1e-9 || t > hi + 1e-9) continue;
            pos[n] = t;
            len[n] = val[d] == 1.0 ? LONG : val[d] == half ? MID : SHORT;
            n++;
        }
    return n;
}

typedef struct { double x, y; Col c; } Pt;
static int cmp_pt_x(const void *a, const void *b) {
    double d = ((const Pt *)a)->x - ((const Pt *)b)->x;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* (panel, x, row) key for stacking duplicated geom_col x values: rows that
 * share a bar sort together, in input order within the bar */
typedef struct { int p; double x; int r; } StackKey;
static int cmp_stackkey(const void *a, const void *b) {
    const StackKey *ka = a, *kb = b;
    if (ka->p != kb->p) return ka->p < kb->p ? -1 : 1;
    if (ka->x != kb->x) return ka->x < kb->x ? -1 : 1;
    return ka->r < kb->r ? -1 : ka->r > kb->r ? 1 : 0;
}

static int cmp_double(const void *a, const void *b) {
    double av = *(const double *)a, bv = *(const double *)b;
    return av < bv ? -1 : av > bv ? 1 : 0;
}

/* type-7 quantile (R's default) on an ascending-sorted array */
static double quantile7(const double *s, int n, double p) {
    if (n == 1) return s[0];
    double h = (n - 1) * p;
    int lo = (int)floor(h);
    if (lo >= n - 1) return s[n - 1];
    return s[lo] + (h - lo) * (s[lo + 1] - s[lo]);
}

/* boxplot five-number summary + Tukey whiskers on sorted s (ggplot StatBoxplot) */
typedef struct { double q1, med, q3, wlo, whi; } BoxStat;
static void box_stats(const double *s, int n, BoxStat *b) {
    b->q1 = quantile7(s, n, 0.25);
    b->med = quantile7(s, n, 0.5);
    b->q3 = quantile7(s, n, 0.75);
    double iqr = b->q3 - b->q1, hi = b->q3 + 1.5 * iqr, lo = b->q1 - 1.5 * iqr;
    b->whi = s[0]; for (int i = 0; i < n; i++) if (s[i] <= hi) b->whi = s[i];
    b->wlo = s[n - 1]; for (int i = n - 1; i >= 0; i--) if (s[i] >= lo) b->wlo = s[i];
}

/* ggrepel-style label placement. Each label is an axis-aligned box (half-widths
 * hw/hh) at centre cx/cy, anchored to a data point ax/ay. We iterate: separate
 * overlapping label boxes and push boxes off data points (both along the axis of
 * least penetration), then apply a weak spring back to the anchor and clamp to
 * the panel. Coordinates are panel points, y up. Deterministic (no RNG). */
typedef struct { double ax, ay, cx, cy, hw, hh; } RLabel;
static void repel_labels(RLabel *L, int n, const double *px, const double *py, int np,
                         double pw, double ph, double pad) {
    for (int it = 0; it < 2600; it++) {
        double moved = 0;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                double dx = L[i].cx - L[j].cx, dy = L[i].cy - L[j].cy;
                double ox = (L[i].hw + L[j].hw + pad) - fabs(dx);
                double oy = (L[i].hh + L[j].hh + pad) - fabs(dy);
                if (ox <= 0 || oy <= 0) continue;
                if (ox < oy) {                 /* separate horizontally */
                    double s = (dx == 0 ? (i < j ? 1 : -1) : dx > 0 ? 1 : -1) * ox / 2;
                    L[i].cx += s; L[j].cx -= s;
                } else {                       /* separate vertically */
                    double s = (dy == 0 ? (i < j ? 1 : -1) : dy > 0 ? 1 : -1) * oy / 2;
                    L[i].cy += s; L[j].cy -= s;
                }
                moved += ox < oy ? ox : oy;
            }
        for (int i = 0; i < n; i++)            /* push labels off data points */
            for (int k = 0; k < np; k++) {
                double dx = L[i].cx - px[k], dy = L[i].cy - py[k];
                double ox = (L[i].hw + pad) - fabs(dx), oy = (L[i].hh + pad) - fabs(dy);
                if (ox <= 0 || oy <= 0) continue;
                if (ox < oy) L[i].cx += (dx == 0 ? 1 : dx > 0 ? 1 : -1) * ox;
                else         L[i].cy += (dy == 0 ? 1 : dy > 0 ? 1 : -1) * oy;
                moved += ox < oy ? ox : oy;
            }
        for (int i = 0; i < n; i++) {          /* weak spring + clamp to panel */
            L[i].cx += (L[i].ax - L[i].cx) * 0.006;
            L[i].cy += (L[i].ay - L[i].cy) * 0.006;
            double lo = L[i].hw, hi = pw - L[i].hw;
            if (hi > lo) L[i].cx = fmin(fmax(L[i].cx, lo), hi);
            lo = L[i].hh; hi = ph - L[i].hh;
            if (hi > lo) L[i].cy = fmin(fmax(L[i].cy, lo), hi);
        }
        if (moved < 0.05) break;
    }
}

/* genome coordinate scale: chromosomes concatenated in seqinfo order */
typedef struct { char **chr; double *off, *len; int n; double total; } GenomeScale;
static GenomeScale *genome_load(const char *path, char *err) {
    DataFrame *sq = df_read_csv(path, err);
    if (!sq) return NULL;
    const Column *sc = df_col(sq, "chrom"), *lc = df_col(sq, "length");
    if (!sc || sc->type != COL_STR || !lc || lc->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "seqinfo `%s` needs a text `chrom` and numeric `length` column", path);
        return NULL;
    }
    if (sq->nrow < 1) {
        snprintf(err, CP_ERRLEN, "seqinfo `%s` has no rows", path);
        return NULL;
    }
    GenomeScale *g = cp_xmalloc(sizeof *g);
    g->n = sq->nrow;
    g->chr = cp_xmalloc(g->n * sizeof(char *));
    g->off = cp_xmalloc(g->n * sizeof(double));
    g->len = cp_xmalloc(g->n * sizeof(double));
    double cum = 0;
    for (int i = 0; i < g->n; i++) {
        g->chr[i] = sc->str[i]; g->len[i] = lc->num[i]; g->off[i] = cum; cum += g->len[i];
    }
    g->total = cum;
    if (!(cum > 0)) {   /* all-zero/negative lengths: axis would divide by 0 */
        snprintf(err, CP_ERRLEN, "seqinfo `%s` has no positive total length", path);
        free(g->chr); free(g->off); free(g->len); free(g);
        return NULL;
    }
    return g;
}
static double genome_off(const GenomeScale *g, const char *chr) {
    for (int i = 0; i < g->n; i++) if (!strcmp(g->chr[i], chr)) return g->off[i];
    return -1;   /* sentinel: chromosome absent from seqinfo */
}

static int cmp_double_asc(const void *a, const void *b) {
    double d = *(const double *)a - *(const double *)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

/* Cell size for geom_tile along one axis: 1 for a discrete axis (categories are
 * at 1..k), else the smallest positive gap between distinct values in the panel.
 * A regular grid therefore tiles exactly; an irregular one gets the tightest
 * spacing rather than overlapping cells. Falls back to the range when a panel
 * holds a single distinct value. */
static double tile_step(const DataFrame *df, const int *use, const Factor *ff,
                        int panel, int discrete, int is_x,
                        const Column *col, const PlotSpec *spec) {
    if (discrete) return 1.0;
    double *v = cp_xmalloc((size_t)df->nrow * sizeof(double));
    int n = 0;
    for (int r = 0; r < df->nrow; r++) {
        if (!use[r] || (ff && ff->idx[r] != panel)) continue;
        double t = col->num[r];
        if (isnan(t)) continue;
        int lb = is_x ? spec->log_x : spec->log_y;
        if (lb) { if (t <= 0) continue; t = cp_logt(lb, t); }
        v[n++] = t;
    }
    double step = 0, lo = 0, hi = 0;
    if (n > 1) {
        qsort(v, n, sizeof(double), cmp_double_asc);
        lo = v[0]; hi = v[n - 1];
        for (int i = 1; i < n; i++) {
            double dgap = v[i] - v[i - 1];
            if (dgap > 1e-12 && (step == 0 || dgap < step)) step = dgap;
        }
    }
    free(v);
    if (step > 0) return step;
    return (hi > lo) ? (hi - lo) : 1.0;      /* one distinct value: unit cell */
}

/* One panel's axes: the trained range, its breaks, and everything derived from
 * them. With facet_wrap(scales="fixed") every panel points at one shared
 * instance, which is what keeps a fixed figure byte-identical to before free
 * scales existed; with free scales each panel owns one.
 *
 * The break arrays are heap, not the fixed [40] buffers they replace. That is
 * what lifts the 40-category limit on a discrete axis: the cap only ever
 * existed because the axis buffers could not hold more. */
typedef struct {
    double x0, x1, y0, y1;               /* expanded range, transformed space */
    double *xbr, *ybr;   int nxbr, nybr; /* break positions, data space */
    char  **xlabs, **ylabs;              /* their labels */
    double *xnpc, *ynpc;                 /* the same breaks, panel npc */
    double *xmin_br, *ymin_br; int nxmin, nymin;   /* minor breaks */
    double *xlt_pos, *xlt_len; int xlt_n;          /* log tick marks */
    double *ylt_pos, *ylt_len; int ylt_n;
    /* discrete free scales: global factor level -> slot in this panel, or -1.
     * NULL when the panel shows every level, which is always so under fixed. */
    int *xmap, *ymap; int nxlev, nylev;
    int shared;                          /* 1 = the fixed-scale instance */
} PanelScale;

/* Breaks and labels for one continuous axis over its expanded range [lo, hi]
 * (transformed space): the user's breaks=/labels= when given, else the
 * extended-Wilkinson ladder, or the powers on a log axis. One routine for the
 * shared pass and the per-panel free-scale pass, and for linear and log axes
 * alike -- breaks=/labels= used to be silently ignored on a log axis. br and
 * labs hold MAX_BREAKS entries; on a log axis a given break is placed at its
 * log and labelled with its plain value, so breaks=c(1,10,100) read 1 10 100. */
static int axis_breaks(int logb, double lo, double hi, int nuser, const double *ubr,
                       int nulab, char *const *ulab, int pct, char axis,
                       double *br, char **labs) {
    if (!nuser && logb) return log_breaks(logb, lo, hi, br, labs, 16);
    int nb, keep[MAX_BREAKS];                 /* original index, so labels= stays paired */
    double vals[MAX_BREAKS];                  /* data-space value of each kept break */
    if (nuser) {
        nb = nuser;
        for (int i = 0; i < nb; i++) br[i] = cp_logt(logb, ubr[i]);
    } else nb = extended_breaks(lo, hi, 5, br, 16);
    int n = 0;
    for (int i = 0; i < nb; i++)
        if (br[i] >= lo && br[i] <= hi) {
            keep[n] = i; vals[n] = nuser ? ubr[i] : br[i]; br[n++] = br[i];
        }
    /* An explicit break outside the range is dropped, as in ggplot2 -- but
     * dropping every one leaves the axis silently unlabelled, which reads
     * as a bug in the figure rather than in the call. */
    if (nuser && n == 0)
        fprintf(stderr, "cinderplot: warning: every %c break given lies outside "
                "the data range [%g, %g]; the axis has no labels\n", axis,
                logb ? pow(logb, lo) : lo, logb ? pow(logb, hi) : hi);
    int dec = axis_decimals(vals, n), pdec = dec - 2 < 0 ? 0 : dec - 2;
    for (int i = 0; i < n; i++) {
        if (nulab) {                          /* labels=c(...): the given text */
            labs[i] = cp_xstrdup(ulab[keep[i]]);
            continue;
        }
        labs[i] = cp_xmalloc(32);
        if (pct) snprintf(labs[i], 32, "%.*f%%", pdec, vals[i] * 100);
        else fmt_break(vals[i], dec, labs[i], 32);
    }
    return n;
}

/* ---- range training, shared by the fixed pass and the per-panel free pass ----
 * Under facet_wrap(scales="free_*") each panel trains its own range, and the
 * copy of this logic it used had drifted: it lost the errorbar lower bound and
 * every reference line, so geom_hline() simply vanished under free_y. One set
 * of routines, called with p = -1 for the whole data and p >= 0 for one
 * panel's rows, so the two passes cannot disagree again. Transformed space
 * throughout; the endpoint columns may hold NaN (the geoms skip those). */
static void train_rows_x(const PlotSpec *spec, const DataFrame *df, const int *use,
                         const Factor *ff, int p, const Column *xc, const Column *xec,
                         double *lo, double *hi) {
    for (int r = 0; r < df->nrow; r++) {
        if (!use[r] || (p >= 0 && ff->idx[r] != p)) continue;
        double t = cp_logt(spec->log_x, xc->num[r]);
        if (t < *lo) *lo = t;
        if (t > *hi) *hi = t;
        if (xec && !isnan(xec->num[r])) {       /* segment/rect end extends x */
            double te = cp_logt(spec->log_x, xec->num[r]);
            if (te < *lo) *lo = te;
            if (te > *hi) *hi = te;
        }
    }
}
static void train_rows_y(const PlotSpec *spec, const DataFrame *df, const int *use,
                         const Factor *ff, int p, const Column *yc, const Column *yec,
                         const Column *yminc, double *lo, double *hi) {
    for (int r = 0; r < df->nrow; r++) {
        if (!use[r] || (p >= 0 && ff->idx[r] != p)) continue;
        double t = cp_logt(spec->log_y, yc->num[r]);
        if (t < *lo) *lo = t;
        if (t > *hi) *hi = t;
        if (yec && !isnan(yec->num[r])) {       /* segment end / errorbar top */
            double te = cp_logt(spec->log_y, yec->num[r]);
            if (te < *lo) *lo = te;
            if (te > *hi) *hi = te;
        }
        if (yminc && !isnan(yminc->num[r])) {   /* errorbar lower bound */
            double te = cp_logt(spec->log_y, yminc->num[r]);
            if (te < *lo) *lo = te;
            if (te > *hi) *hi = te;
        }
    }
}
/* Reference lines expand the panel to include their intercept (ggplot).
 * train_x / train_y say which axis this call trains; txmin/txmax are NULL
 * when x is discrete or genomic, which is what switches abline off (it reads
 * the x range -- possibly the shared, fixed one -- to place its endpoints,
 * and trains y). Returns the number of intercepts dropped for having no
 * place on a log axis -- a value <= 0 used to poison the scale to -inf. */
static int train_ref_lines(const PlotSpec *spec, int train_x, int train_y,
                           double *txmin, double *txmax, double *tymin, double *tymax) {
    int drop = 0;
    for (int li = 0; li < spec->nlayers; li++) {
        const Layer *L = &spec->layers[li];
        if (L->type == GEOM_HLINE && L->has_intercept && train_y) {
            double t = cp_logt(spec->log_y, L->intercept);
            if (!isfinite(t)) { drop++; continue; }
            if (t < *tymin) *tymin = t;
            if (t > *tymax) *tymax = t;
        } else if (L->type == GEOM_VLINE && L->has_intercept && train_x && txmin) {
            double t = cp_logt(spec->log_x, L->intercept);
            if (!isfinite(t)) { drop++; continue; }
            if (t < *txmin) *txmin = t;
            if (t > *txmax) *txmax = t;
        } else if (L->type == GEOM_ABLINE && train_y && txmin) {
            /* ggplot draws abline in TRANSFORMED space (y' = a + b x' with
             * x', y' the log values on a log axis), so the line is straight
             * on the page and its endpoints are always finite. Un-
             * transforming with pow(10, ...) here used to blow a log2 axis up
             * to 10^10 and bend the line on a log10 one. */
            double ex[2] = { *txmin, *txmax };
            for (int k = 0; k < 2; k++) {
                double yv = L->intercept + L->slope * ex[k];
                if (yv < *tymin) *tymin = yv;
                if (yv > *tymax) *tymax = yv;
            }
        }
    }
    return drop;
}

/* a stroke geom's line width: the layer's linewidth= (size=) when given,
 * else the geom's ggplot default, both in linewidth units */
#define LAYER_LW(L, dflt) lw_pt((L)->line_lw > 0 ? (L)->line_lw : (dflt))

int render_plot(const PlotSpec *spec, const DataFrame *df, const char *out,
                double w_pt, double h_pt, char *err) {
    /* ---- layer summary ---- */
    int haspoint = 0, hasline = 0, hascol = 0, nhist = 0, hasbox = 0, hasbar = 0, hasdens = 0, hastext = 0;
    int hastile = 0;
    for (int i = 0; i < spec->nlayers; i++) {
        if (spec->layers[i].type == GEOM_POINT
            || spec->layers[i].type == GEOM_JITTER) haspoint = 1;
        if (spec->layers[i].type == GEOM_LINE
            || spec->layers[i].type == GEOM_SMOOTH) hasline = 1;
        if (spec->layers[i].type == GEOM_COL) hascol = 1;
        if (spec->layers[i].type == GEOM_TILE) hastile = 1;
        if (spec->layers[i].type == GEOM_HISTOGRAM) nhist++;
        if (spec->layers[i].type == GEOM_BOXPLOT) hasbox = 1;
        if (spec->layers[i].type == GEOM_BAR) hasbar = 1;
        if (spec->layers[i].type == GEOM_DENSITY) hasdens = 1;
        if (spec->layers[i].type == GEOM_TEXT || spec->layers[i].type == GEOM_LABEL) hastext = 1;
    }
    int flip = spec->coord_flip;   /* coord_flip(): x and y axes swapped */

    /* ---- resolve columns ---- */
    const Column *xc = df_col(df, spec->x.col);
    if (!xc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->x.col); return -1; }
    /* discrete x when the column is a string or wrapped in factor() */
    int disc_x = (xc->type == COL_STR) || spec->x.is_factor;
    Factor *xf = disc_x ? factor_make(df, xc) : NULL;
    if (xf && spec->x.nlevels
        && factor_relevel(xf, df->nrow, spec->x.levels, spec->x.nlevels,
                          "x", err)) return -1;
    if (!disc_x && (xc->type != COL_NUM)) {
        snprintf(err, CP_ERRLEN, "x column `%s` is not numeric", spec->x.col); return -1;
    }
    if (disc_x && spec->log_x) {
        snprintf(err, CP_ERRLEN, "scale_x_log%d() needs a continuous x",
                 spec->log_x); return -1;
    }
    if (disc_x && nhist) {
        snprintf(err, CP_ERRLEN, "geom_histogram() needs a continuous x"); return -1;
    }
    if (disc_x && hasdens) {
        snprintf(err, CP_ERRLEN, "geom_density() needs a continuous x"); return -1;
    }
    if (hasbox && !disc_x) {
        snprintf(err, CP_ERRLEN, "geom_boxplot() needs a discrete x; use aes(x=factor(%s), ...)", spec->x.col);
        return -1;
    }
    if (hasbar && !disc_x) {
        snprintf(err, CP_ERRLEN, "geom_bar() needs a discrete x; use aes(x=factor(%s), ...)", spec->x.col);
        return -1;
    }
    const Column *yc = NULL;
    Factor *yf = NULL;
    int disc_y = 0;
    if (spec->y.col) {
        yc = df_col(df, spec->y.col);
        if (!yc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->y.col); return -1; }
        /* A categorical y is meaningful for a tile grid (region x sample), and
         * for nothing else here: every other geom either computes y itself
         * (histogram, density, bar) or draws a magnitude from an origin, where
         * a category has no arithmetic. So discrete y is accepted with
         * geom_tile() and refused elsewhere, naming the alternative. */
        disc_y = (yc->type == COL_STR) || spec->y.is_factor;
        if (disc_y && !hastile) {
            snprintf(err, CP_ERRLEN, "a discrete y is supported only with "
                     "geom_tile(); for a category-vs-value chart put the "
                     "category on x and add coord_flip()");
            return -1;
        }
        if (!disc_y && yc->type != COL_NUM) {
            snprintf(err, CP_ERRLEN, "column `%s` must be numeric for y", spec->y.col);
            return -1;
        }
        if (disc_y) {
            yf = factor_make(df, yc);
            if (spec->y.nlevels
                && factor_relevel(yf, df->nrow, spec->y.levels, spec->y.nlevels,
                                  "y", err)) return -1;
            if (spec->log_y) {
                snprintf(err, CP_ERRLEN, "scale_y_log%d() cannot apply to a discrete y",
                         spec->log_y);
                return -1;
            }
        }
    }
    const Column *labc = NULL;
    if (hastext) {
        if (!spec->label.col) {
            snprintf(err, CP_ERRLEN, "geom_text()/geom_label() needs aes(label=...)");
            return -1;
        }
        labc = df_col(df, spec->label.col);
        if (!labc) {
            snprintf(err, CP_ERRLEN, "column `%s` not found", spec->label.col);
            return -1;
        }
    }
    /* geom_segment/rect endpoints (xend required, yend defaults to y) */
    int hasseg = 0, hasrect = 0, rect_top = 0;
    for (int i = 0; i < spec->nlayers; i++) {
        if (spec->layers[i].type == GEOM_SEGMENT) hasseg = 1;
        if (spec->layers[i].type == GEOM_RECT) {
            hasrect = 1;
            if (!spec->layers[i].data) rect_top = 1;   /* full 4-corner rect */
        }
    }
    const Column *xec = NULL, *yec = NULL;
    if (spec->xend.col) {
        xec = df_col(df, spec->xend.col);
        if (!xec) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->xend.col); return -1; }
    }
    if (spec->yend.col) {
        yec = df_col(df, spec->yend.col);
        if (!yec) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->yend.col); return -1; }
    }
    const Column *yminc = NULL;
    if (spec->ymin.col) {
        yminc = df_col(df, spec->ymin.col);
        if (!yminc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->ymin.col); return -1; }
        if (yminc->type != COL_NUM) {
            snprintf(err, CP_ERRLEN, "ymin column `%s` must be numeric", spec->ymin.col);
            return -1;
        }
    }
    int hasrange = 0;
    for (int i = 0; i < spec->nlayers; i++)
        if (spec->layers[i].type == GEOM_ERRORBAR
            || spec->layers[i].type == GEOM_LINERANGE) hasrange = 1;
    if (hasrange && (!yminc || !spec->yend.col)) {
        snprintf(err, CP_ERRLEN, "geom_errorbar()/geom_linerange() need "
                 "aes(ymin=, ymax=)");
        return -1;
    }
    if (xec && xec->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "xend column `%s` must be numeric", spec->xend.col);
        return -1;
    }
    if (yec && yec->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "yend column `%s` must be numeric", spec->yend.col);
        return -1;
    }
    if (hasseg && !xec && !disc_x) {
        snprintf(err, CP_ERRLEN, "geom_segment() needs aes(xend=...)"); return -1;
    }
    if (hasrect && !xec) {
        snprintf(err, CP_ERRLEN, "geom_rect() needs aes(xmin, xmax)"); return -1;
    }
    if (rect_top && !yec) {
        snprintf(err, CP_ERRLEN, "geom_rect() needs aes(ymin, ymax) (or data= for a full-height band)"); return -1;
    }
    /* genome coordinate x-scale: concatenate chromosomes via seqinfo offsets */
    int genome_x = spec->genome_seqinfo != NULL;
    if (flip && genome_x) {
        snprintf(err, CP_ERRLEN, "coord_flip() is not supported with scale_x_genome()"); return -1;
    }
    GenomeScale *gs = NULL;
    double *roff = NULL;             /* per-row genome offset (-1 = drop) */
    if (genome_x) {
        if (disc_x || spec->log_x) {
            snprintf(err, CP_ERRLEN, "scale_x_genome() needs a continuous, non-log x"); return -1;
        }
        if (!spec->chrom.col) {
            snprintf(err, CP_ERRLEN, "scale_x_genome() needs a chromosome column: aes(chrom=...)"); return -1;
        }
        if (!(gs = genome_load(spec->genome_seqinfo, err))) return -1;
        const Column *cc = df_col(df, spec->chrom.col);
        if (!cc || cc->type != COL_STR) {
            snprintf(err, CP_ERRLEN, "chrom column `%s` must be text", spec->chrom.col); return -1;
        }
        roff = cp_xmalloc(df->nrow * sizeof(double));
        for (int r = 0; r < df->nrow; r++) roff[r] = genome_off(gs, cc->str[r]);
    }
    Factor *cf = NULL;
    const Column *colc = NULL;          /* continuous colour column */
    int cont_col = 0;
    FillScale cscale = spec->colour_scale;
    /* Histogram bars carry a colour/fill only when it is constant within each
     * panel -- the common faceted case. geom_col() instead STACKS a varying
     * discrete fill (ggplot's default position); dodging stays unimplemented.
     * The constant-per-panel rule is checked once the facet factor exists
     * (search bars_const_fill). */
    int bars_const_fill = 0;
    if (spec->colour.col) {
        if (nhist) bars_const_fill = 1;
        const Column *cc = df_col(df, spec->colour.col);
        if (!cc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->colour.col); return -1; }
        if (!spec->colour.is_factor && cc->type == COL_NUM) {
            cont_col = 1; colc = cc;    /* continuous colour aesthetic */
            if (!spec->has_colour_scale) {     /* ggplot default: blue gradient */
                cscale.kind = FILL_GRADIENT;
                parse_color("#132B43", &cscale.low);
                parse_color("#56B1F7", &cscale.high);
            }
        } else {
            cf = factor_make(df, cc);
            if (spec->colour.nlevels
                && factor_relevel(cf, df->nrow, spec->colour.levels,
                                  spec->colour.nlevels, "colour/fill", err)) return -1;
            /* the legend gt reserves 2*nlev+1 rows in a GT_MAXDIM grid, so a
             * discrete colour/fill scale is bounded to what that grid holds. */
            if (2 * cf->nlev + 1 > GT_MAXDIM) {
                snprintf(err, CP_ERRLEN,
                         "colour/fill `%s` has %d levels; the discrete legend "
                         "supports at most %d", spec->colour.col, cf->nlev,
                         (GT_MAXDIM - 1) / 2);
                return -1;
            }
        }
    }
    if (spec->identity_scale && cont_col) {
        snprintf(err, CP_ERRLEN, "scale_*_identity needs a text column of "
                 "colour names/#RRGGBB values mapped to colour/fill");
        return -1;
    }
    if (hasbar && cont_col) {
        /* geom_col() takes a continuous fill (each bar mapped through the
         * gradient, as geom_rect() long has); geom_bar() counts rows itself,
         * so a per-row continuous fill has no single value per bar. */
        snprintf(err, CP_ERRLEN, "a continuous colour/fill on geom_bar() is not "
                 "implemented; map a discrete column instead");
        return -1;
    }
    if (hascol && cf && !disc_x) {
        /* stacking accumulates per x category; on a continuous x there is no
         * category to stack within, and drawing overlapping bars would look
         * plausible and be wrong. */
        snprintf(err, CP_ERRLEN, "stacked geom_col() (a varying colour/fill) "
                 "needs a discrete x; wrap it as aes(x=factor(%s), ...)",
                 spec->x.col ? spec->x.col : "x");
        return -1;
    }
    /* aes(shape=): a discrete column mapped to point glyphs. Six levels, as in
     * ggplot2 -- past that the glyphs stop being tellable apart, and refusing
     * is more use than inventing a seventh nobody can name. */
    Factor *shf = NULL;
    if (spec->shape.col) {
        const Column *shc = df_col(df, spec->shape.col);
        if (!shc) {
            snprintf(err, CP_ERRLEN, "column `%s` not found", spec->shape.col);
            return -1;
        }
        if (!haspoint) {
            snprintf(err, CP_ERRLEN, "aes(shape=) needs a point layer "
                     "(geom_point or geom_jitter)");
            return -1;
        }
        shf = factor_make(df, shc);
        if (spec->shape.nlevels
            && factor_relevel(shf, df->nrow, spec->shape.levels,
                              spec->shape.nlevels, "shape", err)) return -1;
        if (shf->nlev > 6) {
            snprintf(err, CP_ERRLEN, "aes(shape=%s) has %d levels; the shape "
                     "palette holds 6 -- map a lower-cardinality column, or use "
                     "facet_wrap() for this one", spec->shape.col, shf->nlev);
            return -1;
        }
    }

    /* aes(group=): a discrete column that partitions the rows into series
     * for geom_line()/geom_smooth() without touching colour or the legend --
     * five reconstructions in one colour, each its own line. With a discrete
     * colour= as well, a series is one (colour level, group level) pair, as
     * in ggplot2. series[r] is the row's series and ngf the group stride, so
     * series s draws in pal[s / ngf]; with no group= that is the colour index
     * unchanged, and the line geoms below never look at cf directly. */
    Factor *gf = NULL;
    if (spec->group.col) {
        const Column *gc = df_col(df, spec->group.col);
        if (!gc) {
            snprintf(err, CP_ERRLEN, "column `%s` not found", spec->group.col);
            return -1;
        }
        if (!spec->group.is_factor && gc->type == COL_NUM) {
            /* ggplot2 would silently treat it as discrete; say so instead */
            snprintf(err, CP_ERRLEN, "aes(group=%s) is a numeric column; group= "
                     "partitions rows by a discrete key -- write factor(%s) to "
                     "say so", spec->group.col, spec->group.col);
            return -1;
        }
        if (!hasline) {
            snprintf(err, CP_ERRLEN, "aes(group=) needs a geom_line() or "
                     "geom_smooth() layer to partition into series; points, "
                     "segments and text are drawn per row and need no grouping");
            return -1;
        }
        /* the stat geoms group by colour= only; a group= they ignore would
         * be a silent no-op, and on geom_density() a wrong answer */
        const char *ungrouped = hasdens ? "geom_density()" : hasbox ? "geom_boxplot()"
                              : hasbar ? "geom_bar()" : hascol ? "geom_col()"
                              : nhist ? "geom_histogram()" : hastile ? "geom_tile()" : NULL;
        if (ungrouped) {
            snprintf(err, CP_ERRLEN, "aes(group=) with %s is not implemented; "
                     "that layer groups by colour= only", ungrouped);
            return -1;
        }
        gf = factor_make(df, gc);
        if (spec->group.nlevels
            && factor_relevel(gf, df->nrow, spec->group.levels,
                              spec->group.nlevels, "group", err)) return -1;
    }
    int ngf = gf ? gf->nlev : 1;
    int nseries = (cf ? cf->nlev : 1) * ngf;
    int *series = cp_xmalloc(df->nrow * sizeof(int));
    for (int r = 0; r < df->nrow; r++) {
        int ci = cf ? cf->idx[r] : 0, gi = gf ? gf->idx[r] : 0;
        series[r] = (ci < 0 || gi < 0) ? -1 : ci * ngf + gi;
    }

    /* aes(size=): numeric column mapped to point area (geom_point) */
    const Column *szc = NULL;
    if (spec->size.col) {
        szc = df_col(df, spec->size.col);
        if (!szc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->size.col); return -1; }
        if (szc->type != COL_NUM || spec->size.is_factor) {
            snprintf(err, CP_ERRLEN, "aes(size=) needs a numeric column; `%s` is not numeric", spec->size.col);
            return -1;
        }
        if (!haspoint) {
            snprintf(err, CP_ERRLEN, "aes(size=) is only implemented for geom_point()");
            return -1;
        }
    }
    /* One level per PANEL. facet_wrap gets the facet column's factor straight;
     * facet_grid gets a synthetic row-major one whose levels are every (row,
     * col) pair, so the combinations the data never uses still own a panel and
     * every panel-indexed array below (histogram bins, bar counts, per-panel
     * scales) is sized and addressed exactly as it was for a wrap. */
    Factor *ff = NULL;
    Factor *rowf = NULL, *colf = NULL;    /* facet_grid sides; NULL = "." */
    const int grid = spec->facet_grid;
    if (grid) {
        if (spec->facet_rowvar) {
            const Column *rc = df_col(df, spec->facet_rowvar);
            if (!rc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->facet_rowvar); return -1; }
            rowf = factor_make(df, rc);
            if (rowf->nlev < 1) {
                snprintf(err, CP_ERRLEN, "facet_grid() row column `%s` has no values",
                         spec->facet_rowvar);
                return -1;
            }
        }
        if (spec->facet_colvar) {
            const Column *cc2 = df_col(df, spec->facet_colvar);
            if (!cc2) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->facet_colvar); return -1; }
            colf = factor_make(df, cc2);
            if (colf->nlev < 1) {
                snprintf(err, CP_ERRLEN, "facet_grid() column column `%s` has no values",
                         spec->facet_colvar);
                return -1;
            }
        }
        int gnr = rowf ? rowf->nlev : 1, gnc = colf ? colf->nlev : 1;
        ff = cp_xcalloc(1, sizeof *ff);
        ff->nlev = gnr * gnc;
        ff->levels = cp_xmalloc((size_t)ff->nlev * sizeof(char *));
        for (int i = 0; i < gnr; i++)
            for (int j = 0; j < gnc; j++) {
                char lb[256];
                if (rowf && colf)
                    snprintf(lb, sizeof lb, "%s, %s", rowf->levels[i], colf->levels[j]);
                else
                    snprintf(lb, sizeof lb, "%s", rowf ? rowf->levels[i] : colf->levels[j]);
                ff->levels[i * gnc + j] = cp_xstrdup(lb);
            }
        ff->idx = cp_xmalloc((size_t)(df->nrow ? df->nrow : 1) * sizeof(int));
        for (int r = 0; r < df->nrow; r++) {
            int i = rowf ? rowf->idx[r] : 0, j = colf ? colf->idx[r] : 0;
            ff->idx[r] = (i < 0 || j < 0) ? -1 : i * gnc + j;
        }
    } else if (spec->facet_var) {
        const Column *fc = df_col(df, spec->facet_var);
        if (!fc) { snprintf(err, CP_ERRLEN, "column `%s` not found", spec->facet_var); return -1; }
        ff = factor_make(df, fc);
        if (ff->nlev < 1) { snprintf(err, CP_ERRLEN, "facet column `%s` has no values", spec->facet_var); return -1; }
        if (spec->n_facet_levels
            && factor_relevel(ff, df->nrow, spec->facet_levels,
                              spec->n_facet_levels, "facet_wrap()", err)) return -1;
    }
    if ((spec->free_x || spec->free_y) && !ff) {
        snprintf(err, CP_ERRLEN, "facet_wrap(scales=) needs facets; there is only "
                 "one panel to scale");
        return -1;
    }

    if (spec->polar) {
        /* the supported subset is exactly the radar chart; everything else
         * would render plausibly and be wrong */
        if (!disc_x) {
            snprintf(err, CP_ERRLEN, "coord_polar() draws radar charts: it "
                     "needs a discrete x whose categories become the spokes "
                     "(wrap it as aes(x=factor(...)))");
            return -1;
        }
        if (genome_x || ff || spec->free_x || spec->free_y
            || spec->nannos || spec->nhobjs) {
            snprintf(err, CP_ERRLEN, "coord_polar() is not implemented with "
                     "%s", genome_x ? "scale_x_genome()"
                     : ff ? (grid ? "facet_grid()" : "facet_wrap()")
                     : (spec->free_x || spec->free_y) ? "free scales"
                     : spec->nannos ? "annotate()" : "annotation()");
            return -1;
        }
        for (int li = 0; li < spec->nlayers; li++)
            if (spec->layers[li].type != GEOM_LINE
                && spec->layers[li].type != GEOM_POINT) {
                snprintf(err, CP_ERRLEN, "coord_polar() supports geom_line() "
                         "(drawn closed) and geom_point(); general polar "
                         "coordinates (pie/rose bars) are not implemented");
                return -1;
            }
        /* the radar drawing colours by the discrete series only; a
         * continuous colour, shape or size built its legend and was then
         * ignored on the marks */
        if (cont_col || shf || szc) {
            snprintf(err, CP_ERRLEN, "coord_polar(): %s mappings are not "
                     "implemented (only geom_line()/geom_point() with a "
                     "discrete colour)", cont_col ? "continuous colour="
                     : shf ? "shape=" : "size=");
            return -1;
        }
    }
    /* ---- usable rows (NA and log-domain filtering) ----
     * Inf counts as missing (isfinite, not !isnan): R writes Inf, strtod
     * reads it, and one such cell used to blank the whole panel by training
     * the scale to infinity. NaN in the optional endpoint columns (xend,
     * yend, ymin) is left to the geoms, which skip it. */
    int *use = cp_xmalloc(df->nrow * sizeof(int)), nuse = 0, d_na = 0, d_log = 0;
    for (int r = 0; r < df->nrow; r++) {
        int xok = disc_x ? (xf->idx[r] >= 0)
                : genome_x ? (roff[r] >= 0 && isfinite(xc->num[r]))
                : isfinite(xc->num[r]);
        int ok = xok && (!yc || (disc_y ? yf->idx[r] >= 0 : isfinite(yc->num[r])))
              && (!cf || cf->idx[r] >= 0) && (!shf || shf->idx[r] >= 0)
              && (!gf || gf->idx[r] >= 0)
              && (!cont_col || isfinite(colc->num[r]))
              && (!ff || ff->idx[r] >= 0)
              && (!szc || isfinite(szc->num[r]))
              && (!xec || !isinf(xec->num[r]))
              && (!yec || !isinf(yec->num[r]))
              && (!yminc || !isinf(yminc->num[r]));
        if (!ok) d_na++;
        /* every value the row puts on a log axis has to be positive, the
         * endpoints included: an errorbar reaching down to 0 on a log y used
         * to train the axis to -inf and empty the panel. ggplot drops the row
         * (NaN <= 0 is false, so a missing endpoint still passes). */
        else if ((spec->log_x && (xc->num[r] <= 0 || (xec && xec->num[r] <= 0)))
                 || (spec->log_y && ((yc && yc->num[r] <= 0)
                                     || (yec && yec->num[r] <= 0)
                                     || (yminc && yminc->num[r] <= 0)))) {
            ok = 0; d_log++;
        }
        use[r] = ok;
        nuse += ok;
    }
    if (nuse == 0) { snprintf(err, CP_ERRLEN, "no complete rows to plot"); return -1; }

    /* bars_const_fill: colour/fill was mapped on a bar geom. Allowed only if
     * every panel holds a single colour level, so each panel's bars take one
     * hue; anything else is a stack/dodge and stays unimplemented. */
    int *panelfill = NULL;
    if (bars_const_fill) {
        if (cont_col) {
            snprintf(err, CP_ERRLEN, "a continuous colour/fill on bars is not "
                     "implemented; map a discrete column instead");
            return -1;
        }
        int np_ = ff ? ff->nlev : 1;
        panelfill = cp_xmalloc(np_ * sizeof(int));
        for (int i = 0; i < np_; i++) panelfill[i] = -1;
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            int p = ff ? ff->idx[r] : 0;
            if (panelfill[p] < 0) panelfill[p] = cf->idx[r];
            else if (panelfill[p] != cf->idx[r]) {
                snprintf(err, CP_ERRLEN,
                         "colour/fill on bars varies within %s, which would need "
                         "stacking or dodging (not implemented); it is supported "
                         "only when constant per panel, e.g. facet_wrap(~%s)",
                         ff ? "a facet" : "the plot", spec->colour.col);
                free(panelfill);
                return -1;
            }
        }
    }
    if (d_na) fprintf(stderr, "cinderplot: removed %d rows with missing values\n", d_na);
    if (d_log) fprintf(stderr, "cinderplot: removed %d rows with non-positive values on a log axis\n", d_log);

    /* continuous colour domain: limits (squished) or the data range */
    double cdmin = 0, cdmax = 1;
    if (cont_col) {
        if (cscale.has_limits) { cdmin = cscale.lim_lo; cdmax = cscale.lim_hi; }
        else {
            cdmin = 1e300; cdmax = -1e300;
            for (int r = 0; r < df->nrow; r++)
                if (use[r] && !isnan(colc->num[r])) {
                    if (colc->num[r] < cdmin) cdmin = colc->num[r];
                    if (colc->num[r] > cdmax) cdmax = colc->num[r];
                }
            if (cdmax <= cdmin) cdmax = cdmin + 1;
        }
    }
#define CCOL(r) fill_map_value(&cscale, colc->num[r], cdmin, cdmax)

    /* size domain (data range over the used rows) */
    double szmin = 0, szmax = 1;
    if (szc) {
        szmin = 1e300; szmax = -1e300;
        for (int r = 0; r < df->nrow; r++)
            if (use[r] && !isnan(szc->num[r])) {
                if (szc->num[r] < szmin) szmin = szc->num[r];
                if (szc->num[r] > szmax) szmax = szc->num[r];
            }
        if (szmax <= szmin) szmax = szmin + 1;
    }

#define TY(v) (cp_logt(spec->log_y, (v)))
/* transformed x for row r: category position (discrete), genome offset+pos
 * (genome), or raw value (continuous) */
/* Discrete free scales renumber the categories a panel actually shows, so a
 * level's position depends on which panel is drawn. xmap/ymap carry that
 * renumbering (global level -> slot in this panel); NULL means the panel shows
 * every level, which is always the case under fixed scales. */
    const int *xmap = NULL, *ymap = NULL;
#define YVAL(r) (disc_y ? (double)((ymap ? ymap[yf->idx[r]] : yf->idx[r]) + 1) : yc->num[r])
#define XVAL(r) (disc_x ? (double)((xmap ? xmap[xf->idx[r]] : xf->idx[r]) + 1) \
               : genome_x ? (roff[r] + xc->num[r]) : xc->num[r])
#define TXR(r)  (cp_logt(spec->log_x, XVAL(r)))
/* genome offset applied to any within-chromosome position (e.g. xend) */
#define GX(r, v) (genome_x ? (roff[r] + (v)) : (v))

    /* ---- panel grid: ggplot2 wrap_dims = rev(grDevices::n2mfrow(n)), unless
     * the caller fixed one side. ncol= matters whenever the panels cross two
     * factors: at 8 panels the automatic shape wraps 3 per row, which splits
     * the pairs the figure exists to compare into different rows, and no
     * levels= ordering can fix that. ---- */
    int npan = ff ? ff->nlev : 1, ncolp, nrowp;
    if (grid) {                     /* the two factors ARE the shape */
        ncolp = colf ? colf->nlev : 1;
        nrowp = rowf ? rowf->nlev : 1;
    } else if (spec->facet_ncol > 0 && spec->facet_nrow > 0) {
        ncolp = spec->facet_ncol; nrowp = spec->facet_nrow;
        if (ncolp * nrowp < npan) {
            snprintf(err, CP_ERRLEN, "facet_wrap(ncol=%d, nrow=%d) holds %d panels "
                     "but there are %d", ncolp, nrowp, ncolp * nrowp, npan);
            return -1;
        }
    } else if (spec->facet_ncol > 0) {
        ncolp = spec->facet_ncol;
        nrowp = (npan + ncolp - 1) / ncolp;
    } else if (spec->facet_nrow > 0) {
        nrowp = spec->facet_nrow;
        ncolp = (npan + nrowp - 1) / nrowp;
    } else if (npan <= 3)  { ncolp = npan;            nrowp = 1; }
    else if (npan <= 6)  { ncolp = (npan + 1) / 2;  nrowp = 2; }
    else if (npan <= 12) { ncolp = (npan + 2) / 3;  nrowp = 3; }
    else {
        ncolp = (int)ceil(sqrt((double)npan));
        nrowp = (npan + ncolp - 1) / ncolp;
    }
    /* The outer gtable is 4 rows per panel row plus 7 of chrome, and 2 columns
     * per panel column plus 7 (the extra one is facet_grid's right-hand strip);
     * both have to fit GT_MAXDIM. The old guard counted 3 rows per panel row,
     * which let facet_wrap(~v, ncol=1) with 63+ levels write past rowh[]. */
    int maxrowp = (GT_MAXDIM - 7) / 4;                       /* 4n + 6 rows, +1 for
                                                              * the free_colour row */
    int maxcolp = (GT_MAXDIM - 6 - (grid ? 1 : 0)) / 2;
    if (ncolp > maxcolp || nrowp > maxrowp) {
        snprintf(err, CP_ERRLEN, "too many facet panels (%d): a %d x %d layout "
                 "exceeds the %d rows by %d columns the grid can hold",
                 npan, nrowp, ncolp, maxrowp, maxcolp);
        return -1;
    }

    /* facet_grid free scales are per COLUMN (x) and per ROW (y), as ggplot2's
     * facet_grid does it: every panel down a column shares one x range. Two
     * group factors say so -- they map a data row to its panel column / panel
     * row -- and the per-panel training below filters through them. Under
     * facet_wrap each panel is its own group, XG/YG are the identity, and the
     * code runs exactly as it did. */
    const Factor *xgf = ff, *ygf = ff;
#define XG(p) (grid ? (p) % ncolp : (p))
#define YG(p) (grid ? (p) / ncolp : (p))
    if (grid && (spec->free_x || spec->free_y)) {
        Factor *gx = cp_xcalloc(1, sizeof *gx), *gy = cp_xcalloc(1, sizeof *gy);
        gx->nlev = ncolp; gy->nlev = nrowp;
        gx->levels = gy->levels = NULL;       /* only idx[] is ever read */
        gx->idx = cp_xmalloc((size_t)(df->nrow ? df->nrow : 1) * sizeof(int));
        gy->idx = cp_xmalloc((size_t)(df->nrow ? df->nrow : 1) * sizeof(int));
        for (int r = 0; r < df->nrow; r++) {
            gx->idx[r] = ff->idx[r] < 0 ? -1 : ff->idx[r] % ncolp;
            gy->idx[r] = ff->idx[r] < 0 ? -1 : ff->idx[r] / ncolp;
        }
        xgf = gx; ygf = gy;
    }

    /* A per-layer data= file is the same file in every panel, so read it once
     * and keep it. It used to be opened, parsed and typed once per panel. */
    DataFrame *layer_df[MAX_LAYERS] = {0};

    /* A 4-corner rect layer from its own file (x/xend + y/yend all present
     * there) takes part in scale training, as main-data rects do — otherwise
     * a rect reaching past the main data is silently clipped at the panel
     * edge. Region-highlight bands (no y extent in the file) never trained
     * the scales and still do not. */
    double lxmin = 1e300, lxmax = -1e300, lymin = 1e300, lymax = -1e300;
    /* reference values (rect layer rows, annotate() marks, hline intercepts)
     * that cannot sit on a log axis are dropped and reported once, as ggplot
     * does; one of them used to poison the whole scale to -inf */
    int d_ref = 0;
    for (int li = 0; li < spec->nlayers; li++) {
        const Layer *L = &spec->layers[li];
        if (L->type != GEOM_RECT || !L->data || genome_x) continue;
        if (!layer_df[li] && !(layer_df[li] = df_read_csv(L->data, err)))
            return -1;
        DataFrame *d2 = layer_df[li];
        const Column *c_x = df_col(d2, spec->x.col);
        const Column *c_xe = spec->xend.col ? df_col(d2, spec->xend.col) : NULL;
        const Column *c_y = spec->y.col ? df_col(d2, spec->y.col) : NULL;
        const Column *c_ye = spec->yend.col ? df_col(d2, spec->yend.col) : NULL;
        if (!c_x || !c_xe || !c_y || !c_ye
            || c_x->type != COL_NUM || c_xe->type != COL_NUM
            || c_y->type != COL_NUM || c_ye->type != COL_NUM) continue;
        for (int r2 = 0; r2 < d2->nrow; r2++) {
            if (isnan(c_x->num[r2]) || isnan(c_xe->num[r2])
                || isnan(c_y->num[r2]) || isnan(c_ye->num[r2])) continue;
            double ta = cp_logt(spec->log_x, c_x->num[r2]);
            double tb = cp_logt(spec->log_x, c_xe->num[r2]);
            double ua = TY(c_y->num[r2]), ub = TY(c_ye->num[r2]);
            if (!isfinite(ta) || !isfinite(tb) || !isfinite(ua) || !isfinite(ub)) {
                d_ref++; continue;
            }
            if (fmin(ta, tb) < lxmin) lxmin = fmin(ta, tb);
            if (fmax(ta, tb) > lxmax) lxmax = fmax(ta, tb);
            if (fmin(ua, ub) < lymin) lymin = fmin(ua, ub);
            if (fmax(ua, ub) > lymax) lymax = fmax(ua, ub);
        }
    }

    /* annotate() marks participate in scale training too (as in ggplot2) —
     * a label placed just past the data would otherwise silently clip. Rides
     * the same accumulators as the 4-corner rect layers, which only merge
     * into continuous axes. */
    for (int a2 = 0; a2 < spec->nannos; a2++) {
        const Annotate *an = &spec->annos[a2];
        double xs[2] = { an->x, an->has_xend ? an->xend : an->x };
        double ys2[2] = { an->y, an->has_yend ? an->yend : an->y };
        for (int k = 0; k < 2; k++) {
            double t = cp_logt(spec->log_x, xs[k]), u = TY(ys2[k]);
            if (!isfinite(t) || !isfinite(u)) { d_ref++; continue; }
            if (!genome_x) {
                if (t < lxmin) lxmin = t;
                if (t > lxmax) lxmax = t;
            }
            if (u < lymin) lymin = u;
            if (u > lymax) lymax = u;
        }
    }

    /* ---- x scale training (transformed space) ---- */
    double txmin, txmax;
    if (disc_x) {                          /* categories at 1..k */
        txmin = 1; txmax = xf->nlev;
    } else if (genome_x) {                 /* whole genome, exact */
        txmin = 0; txmax = gs->total;
    } else {
        txmin = 1e300; txmax = -1e300;
        train_rows_x(spec, df, use, ff, -1, xc, xec, &txmin, &txmax);
        if (lxmin < txmin) txmin = lxmin;       /* 4-corner rect layer files */
        if (lxmax > txmax) txmax = lxmax;
        if (txmax == txmin) { txmin -= 0.5; txmax += 0.5; }
    }

    /* ---- stat_bin for histogram layers (bins on the transformed scale,
     * ggplot's default alignment: boundary = width/2) ----
     * ggplot's bin_breaks_bins, verified via ggplot_build: width =
     * range/(bins-1) (or the whole range for bins=1, 0.1 for no range), the
     * edges on the lattice (k + 1/2) * width, the first edge the last such
     * point at or below the minimum, and the last edge the first past the
     * maximum -- so the number of bins is `bins`, or one fewer when the
     * minimum sits exactly on an edge. Bins are right-closed (a, b] with the
     * lowest edge inclusive, which is where integer data on the edges goes
     * in ggplot; left-closed bins counted every such value one bin over.
     * Under free_x each panel bins its own range, as ggplot does; the
     * x-scale is then trained on the edges (below), so the outer bars are
     * never clipped by a panel trained on the data alone. */
    typedef struct { int nbins; double *start, *width; int *nb, *counts; int max; } Hist;
    Hist hist[MAX_LAYERS];
    memset(hist, 0, sizeof hist);
    int hist_free_x = ff && spec->free_x && !disc_x && !genome_x;
    for (int li = 0; li < spec->nlayers; li++) {
        if (spec->layers[li].type != GEOM_HISTOGRAM) continue;
        Hist *hs = &hist[li];
        int bins = spec->layers[li].bins;
        hs->nbins = bins;                       /* per-panel stride */
        hs->start = cp_xmalloc(npan * sizeof(double));
        hs->width = cp_xmalloc(npan * sizeof(double));
        hs->nb = cp_xmalloc(npan * sizeof(int));
        hs->counts = cp_xcalloc((size_t)npan * hs->nbins, sizeof(int));
        for (int p = 0; p < npan; p++) {
            double lo = txmin, hi = txmax;
            if (hist_free_x) {
                lo = 1e300; hi = -1e300;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || xgf->idx[r] != XG(p)) continue;
                    double t = TXR(r);
                    if (t < lo) lo = t;
                    if (t > hi) hi = t;
                }
                if (lo > hi) lo = hi = 0;       /* empty panel */
            }
            double w = hi <= lo ? 0.1 : bins > 1 ? (hi - lo) / (bins - 1) : hi - lo;
            double boundary = bins > 1 || hi <= lo ? w / 2 : lo;
            double origin = boundary + floor((lo - boundary) / w) * w;
            int nb = (int)floor((hi - origin) / w + 1 - 1e-8);
            if (nb < 1) nb = 1;
            if (nb > bins) nb = bins;
            hs->start[p] = origin; hs->width[p] = w; hs->nb[p] = nb;
        }
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            int p = ff ? ff->idx[r] : 0;
            double w = hs->width[p], fuzz = 1e-8 * w;
            int bin = (int)ceil((TXR(r) - hs->start[p] - fuzz) / w) - 1;
            if (bin < 0) bin = 0;
            if (bin >= hs->nb[p]) bin = hs->nb[p] - 1;
            hs->counts[p * hs->nbins + bin]++;
        }
        for (int i = 0; i < npan * hs->nbins; i++)
            if (hs->counts[i] > hs->max) hs->max = hs->counts[i];
        if (!hist_free_x) {                     /* bin edges train the x scale */
            double lo = hs->start[0], hi = hs->start[0] + hs->nb[0] * hs->width[0];
            if (lo < txmin) txmin = lo;
            if (hi > txmax) txmax = hi;
        }
    }

    /* ---- stat_count for geom_bar: counts per (panel, x-category, group) ---- */
    int barng = cf ? cf->nlev : 1;
    int *barcount = NULL, barmax = 0;
    if (hasbar) {
        barcount = cp_xcalloc((size_t)npan * xf->nlev * barng, sizeof(int));
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            int p = ff ? ff->idx[r] : 0, grp = cf ? cf->idx[r] : 0;
            barcount[((size_t)(p * xf->nlev + xf->idx[r])) * barng + grp]++;
        }
        for (int p = 0; p < npan; p++)          /* max stacked total per category */
            for (int cat = 0; cat < xf->nlev; cat++) {
                int total = 0;
                for (int g = 0; g < barng; g++)
                    total += barcount[((size_t)(p * xf->nlev + cat)) * barng + g];
                if (total > barmax) barmax = total;
            }
    }

    /* ---- stacked geom_col: value sums per (panel, x-category, fill group).
     * ggplot's default position for geom_col is stack, so a varying discrete
     * fill stacks; duplicated (category, group) rows add, as they do there.
     * Negative values would need ggplot's two-sided stacking; refuse rather
     * than draw overlapping segments that look plausible and are wrong. */
    double *colsum = NULL, *colstack_max = NULL;   /* per-panel max total */
    if (hascol && cf) {
        int ng = cf->nlev;
        colsum = cp_xcalloc((size_t)npan * xf->nlev * ng, sizeof(double));
        colstack_max = cp_xcalloc(npan, sizeof(double));
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            if (yc->num[r] < 0) {
                snprintf(err, CP_ERRLEN, "stacked geom_col() with negative "
                         "values is not implemented (row with %s = %g)",
                         spec->y.col, yc->num[r]);
                free(colsum); free(colstack_max);
                return -1;
            }
            int p = ff ? ff->idx[r] : 0;
            colsum[((size_t)(p * xf->nlev + xf->idx[r])) * ng + cf->idx[r]] += yc->num[r];
        }
        for (int p = 0; p < npan; p++)
            for (int cat = 0; cat < xf->nlev; cat++) {
                double total = 0;
                for (int g = 0; g < ng; g++)
                    total += colsum[((size_t)(p * xf->nlev + cat)) * ng + g];
                if (total > colstack_max[p]) colstack_max[p] = total;
            }
    }
    /* Without a discrete fill, duplicated x values still stack: ggplot's
     * default position for geom_col is stack, so A=1 and A=2 read as A=3,
     * not as two bars painted over each other with the taller one showing.
     * Each row's offset is fixed here in row order (ggplot's stacking order);
     * negatives stack downward from 0, as position_stack does. A unique x
     * gets offset 0 and draws exactly as before. */
    double *coloff = NULL, *colstack_min = NULL;
    if (hascol && !cf) {
        coloff = cp_xcalloc(df->nrow, sizeof(double));
        colstack_max = cp_xcalloc(npan, sizeof(double));
        colstack_min = cp_xcalloc(npan, sizeof(double));
        StackKey *keys = cp_xmalloc((nuse ? nuse : 1) * sizeof(StackKey));
        int nk = 0;
        for (int r = 0; r < df->nrow; r++)
            if (use[r]) keys[nk++] = (StackKey){ ff ? ff->idx[r] : 0, TXR(r), r };
        qsort(keys, nk, sizeof(StackKey), cmp_stackkey);
        double up = 0, down = 0;
        for (int i = 0; i < nk; i++) {
            if (i == 0 || keys[i].p != keys[i-1].p || keys[i].x != keys[i-1].x)
                up = down = 0;
            double v = yc->num[keys[i].r];
            if (v >= 0) {
                coloff[keys[i].r] = up; up += v;
                if (up > colstack_max[keys[i].p]) colstack_max[keys[i].p] = up;
            } else {
                coloff[keys[i].r] = down; down += v;
                if (down < colstack_min[keys[i].p]) colstack_min[keys[i].p] = down;
            }
        }
        free(keys);
    }

    /* ---- stat_density: Gaussian KDE per (panel, colour group), bandwidth
     * nrd0 (Silverman), evaluated at DENS_N points over [min-3bw, max+3bw]
     * (ggplot's cut=3). The x-scale stays on the data range (ggplot-style). --- */
#define DENS_N 512
    int densg = cf ? cf->nlev : 1;
    /* map each layer to its density-layer index so multiple geom_density()
     * layers each keep their own bw=/adjust= and curve (not just the last) */
    const Layer *dlayer[MAX_LAYERS]; int li2di[MAX_LAYERS], ndens = 0;
    for (int li = 0; li < spec->nlayers; li++) {
        li2di[li] = -1;
        if (spec->layers[li].type == GEOM_DENSITY) {
            li2di[li] = ndens; dlayer[ndens++] = &spec->layers[li];
        }
    }
    double *dens_x = NULL, *dens_y = NULL, dens_max = 0;
    if (hasdens) {
        dens_x = cp_xmalloc((size_t)ndens * npan * densg * DENS_N * sizeof(double));
        dens_y = cp_xmalloc((size_t)ndens * npan * densg * DENS_N * sizeof(double));
        double *buf = cp_xmalloc((size_t)df->nrow * sizeof(double));
        for (int di = 0; di < ndens; di++) {
          const Layer *densl = dlayer[di];
          for (int p = 0; p < npan; p++)
            for (int gg = 0; gg < densg; gg++) {
                int n = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p) && (!cf || cf->idx[r] == gg))
                        buf[n++] = TXR(r);
                size_t base = ((size_t)((di * npan + p) * densg + gg)) * DENS_N;
                if (n < 2) {
                    for (int j = 0; j < DENS_N; j++) { dens_x[base+j] = txmin; dens_y[base+j] = 0; }
                    continue;
                }
                double mean = 0;
                for (int i = 0; i < n; i++) mean += buf[i];
                mean /= n;
                double var = 0;
                for (int i = 0; i < n; i++) { double d = buf[i] - mean; var += d * d; }
                var /= (n - 1);
                double sd = sqrt(var);
                qsort(buf, n, sizeof(double), cmp_double);
                double iqr = quantile7(buf, n, 0.75) - quantile7(buf, n, 0.25);
                double lo = fmin(sd, iqr / 1.34);           /* R's bw.nrd0 (1.34, not 1.349) */
                if (lo <= 0) lo = sd > 0 ? sd : (fabs(buf[0]) > 0 ? fabs(buf[0]) : 1);
                double bw = (densl->bw > 0 ? densl->bw : 0.9 * lo * pow((double)n, -0.2))
                          * densl->adjust;               /* bw= override, x adjust= */
                if (bw <= 0) bw = 1e-6;
                /* eval over [min-3bw, max+3bw] (cut=3); the curve is clipped to
                 * the data-range panel at draw time, matching ggplot */
                double xlo = buf[0] - 3 * bw, xhi = buf[n-1] + 3 * bw;
                double inv = 1.0 / ((double)n * bw * sqrt(2 * M_PI));
                for (int j = 0; j < DENS_N; j++) {
                    double xj = xlo + (xhi - xlo) * j / (DENS_N - 1), s = 0;
                    for (int i = 0; i < n; i++) {
                        double u = (xj - buf[i]) / bw;
                        s += exp(-0.5 * u * u);
                    }
                    double d = s * inv;
                    dens_x[base+j] = xj; dens_y[base+j] = d;
                    if (d > dens_max) dens_max = d;
                }
            }
        }
        free(buf);
    }

    /* ---- y scale training ---- */
    double tymin = 1e300, tymax = -1e300;
    if (nhist) {
        tymin = 0; /* log10(1), the smallest rendered non-zero count */
        tymax = 0;
        for (int li = 0; li < spec->nlayers; li++)
            if (spec->layers[li].type == GEOM_HISTOGRAM) {
                double ymax = cp_logt(spec->log_y, (double)hist[li].max);
                if (ymax > tymax) tymax = ymax;
            }
    } else if (hasbar) {
        tymin = 0; tymax = cp_logt(spec->log_y, (double)barmax);
    } else if (hasdens) {
        tymin = 0; tymax = dens_max;
    } else {
        /* a discrete y sits at 1..k; its range below is never read (the
         * expansion works from the level count), it just has to be sane */
        if (disc_y) { tymin = 1; tymax = yf->nlev; }
        else train_rows_y(spec, df, use, ff, -1, yc, yec, yminc, &tymin, &tymax);
        if (lymin < tymin) tymin = lymin;       /* 4-corner rect layer files */
        if (lymax > tymax) tymax = lymax;
        if (colstack_max) {                     /* stacked totals set the top */
            double mx = 0;
            for (int p = 0; p < npan; p++)
                if (colstack_max[p] > mx) mx = colstack_max[p];
            double t = TY(mx);
            if (t > tymax) tymax = t;
        }
        if (colstack_min && !spec->log_y)       /* ... and the bottom */
            for (int p = 0; p < npan; p++)
                if (colstack_min[p] < tymin) tymin = colstack_min[p];
        if (hascol && !spec->log_y) {           /* bars are anchored at 0 */
            if (tymin > 0) tymin = 0;
            if (tymax < 0) tymax = 0;
        }
    }
    if (tymax == tymin) { tymin -= 0.5; tymax += 0.5; }

    /* reference lines (hline/vline/abline) widen the trained ranges */
    d_ref += train_ref_lines(spec, 1, !disc_y,
                             (disc_x || genome_x) ? NULL : &txmin,
                             (disc_x || genome_x) ? NULL : &txmax, &tymin, &tymax);
    if (d_ref)
        fprintf(stderr, "cinderplot: warning: dropped %d reference value%s with "
                "no place on a log axis (not positive)\n", d_ref, d_ref == 1 ? "" : "s");

    /* user axis limits (xlim/ylim or scale_*_log10(limits=)): override the
     * data-driven range with the requested domain (log10-transformed when the
     * axis is log). Default expansion is applied below as usual. */
    /* A limit or break list on a discrete axis has no data-space to apply
     * to; it used to parse, run and do nothing. Reversed limits would need a
     * reversed axis, which is not implemented -- say so rather than report
     * the collapsed range as an expand= problem. */
    if ((spec->has_xlim || spec->n_x_breaks || spec->n_x_break_labs) && disc_x) {
        snprintf(err, CP_ERRLEN, "%s on a discrete x axis is not implemented",
                 spec->has_xlim ? "xlim()/limits=" : "breaks=/labels=");
        return -1;
    }
    if ((spec->has_xlim || spec->n_x_breaks || spec->n_x_break_labs) && genome_x) {
        snprintf(err, CP_ERRLEN, "%s is not implemented with scale_x_genome(); "
                 "use regions() for a window",
                 spec->has_xlim ? "xlim()/limits=" : "breaks=/labels=");
        return -1;
    }
    if ((spec->has_ylim || spec->n_y_breaks || spec->n_y_break_labs) && disc_y) {
        snprintf(err, CP_ERRLEN, "%s on a discrete y axis is not implemented",
                 spec->has_ylim ? "ylim()/limits=" : "breaks=/labels=");
        return -1;
    }
    if (spec->has_xlim && !(spec->xlim_lo < spec->xlim_hi)) {
        snprintf(err, CP_ERRLEN, "xlim(): lo must be < hi (got %g, %g); reversed "
                 "axes are not implemented", spec->xlim_lo, spec->xlim_hi);
        return -1;
    }
    if (spec->has_ylim && !(spec->ylim_lo < spec->ylim_hi)) {
        snprintf(err, CP_ERRLEN, "ylim(): lo must be < hi (got %g, %g); reversed "
                 "axes are not implemented", spec->ylim_lo, spec->ylim_hi);
        return -1;
    }
    if (spec->log_x && spec->has_xlim && (spec->xlim_lo <= 0 || spec->xlim_hi <= 0)) {
        snprintf(err, CP_ERRLEN, "x limits must be positive on a log axis, got "
                 "[%g, %g]", spec->xlim_lo, spec->xlim_hi);
        return -1;
    }
    if (spec->log_y && spec->has_ylim && (spec->ylim_lo <= 0 || spec->ylim_hi <= 0)) {
        snprintf(err, CP_ERRLEN, "y limits must be positive on a log axis, got "
                 "[%g, %g]", spec->ylim_lo, spec->ylim_hi);
        return -1;
    }
    if (spec->has_xlim && !disc_x && !genome_x) {
        txmin = cp_logt(spec->log_x, spec->xlim_lo);
        txmax = cp_logt(spec->log_x, spec->xlim_hi);
    }
    if (spec->has_ylim) {
        tymin = cp_logt(spec->log_y, spec->ylim_lo);
        tymax = cp_logt(spec->log_y, spec->ylim_hi);
    }
    /* warn about data outside the limits: cinderplot clips such points to the
     * panel (ggplot drops them). Report the count like ggplot's "Removed N". */
    if ((spec->has_xlim || spec->has_ylim) && !nhist && !hasbar && !hasdens) {
        int nout = 0;
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            int out = 0;
            if (spec->has_xlim && !disc_x && !genome_x) {
                double t = TXR(r);
                if (t < txmin - 1e-9 || t > txmax + 1e-9) out = 1;
            }
            if (spec->has_ylim && yc) {
                double t = TY(yc->num[r]);
                if (t < tymin - 1e-9 || t > tymax + 1e-9) out = 1;
            }
            nout += out;
        }
        if (nout)
            fprintf(stderr, "cinderplot: warning: %d point%s outside the axis limits "
                    "(clipped to the panel)\n", nout, nout == 1 ? "" : "s");
    }

    /* ---- expansion + breaks. Discrete x uses ggplot's additive 0.6 on
     * each side; continuous uses 5% of the range. ---- */
    double xm2 = spec->has_x_expand ? spec->x_exp_mult : -1;   /* -1 = default */
    double xa2 = spec->has_x_expand ? spec->x_exp_add : -1;
    double ym2 = spec->has_y_expand ? spec->y_exp_mult : -1;
    double ya2 = spec->has_y_expand ? spec->y_exp_add : -1;
    double x0, x1;
    if (disc_x) {
        /* the expansion is measured from the TILE EDGE (category ± 0.5),
         * as ggplot trains it — so expand=c(0,0) leaves the outer cells
         * flush with the frame instead of cut in half. The default 0.5 +
         * 0.1 reproduces the previous [1-0.6, k+0.6] exactly. */
        double e = xm2 < 0 ? 0.1 : xm2 * (xf->nlev - 1) + xa2;
        x0 = 0.5 - e; x1 = xf->nlev + 0.5 + e;
    }
    else if (genome_x) { x0 = 0; x1 = gs->total; }     /* no expansion */
    else {
        double e = xm2 < 0 ? 0.05 * (txmax - txmin)
                 : xm2 * (txmax - txmin) + xa2;
        x0 = txmin - e; x1 = txmax + e;
    }
    double y0, y1;
    if (disc_y) {
        double e = ym2 < 0 ? 0.1 : ym2 * (yf->nlev - 1) + ya2;
        y0 = 0.5 - e; y1 = yf->nlev + 0.5 + e;   /* from the tile edge, as x */
    }
    else {
        double e = ym2 < 0 ? 0.05 * (tymax - tymin)
                 : ym2 * (tymax - tymin) + ya2;
        y0 = tymin - e; y1 = tymax + e;
    }
    if (x1 <= x0 || y1 <= y0) {
        int ex = x1 <= x0 ? spec->has_x_expand : spec->has_y_expand;
        snprintf(err, CP_ERRLEN, ex ? "expand= collapsed the %c axis to nothing"
                                    : "the %c axis collapsed to nothing", x1 <= x0 ? 'x' : 'y');
        return -1;
    }
    /* reserve the bottom `ideo_npc` of the panel for the ideogram track */
    double ideo_npc = (spec->ideogram_path && genome_x) ? 0.06 : 0;
    if (flip && ideo_npc > 0) {
        snprintf(err, CP_ERRLEN, "coord_flip() is not supported with ideogram()"); return -1;
    }
    if (ideo_npc > 0) y0 -= (y1 - y0) * ideo_npc / (1 - ideo_npc);
#define NPCX(t) (((t) - x0) / (x1 - x0))
#define NPCY(t) (((t) - y0) / (y1 - y0))

    /* Sized to the axis: a discrete axis draws one break per category, and
     * capping these at a fixed 40 was the only reason a discrete axis could not
     * carry more. Continuous axes never ask for more than 16. */
    int xbrcap = disc_x ? xf->nlev + 1 : MAX_BREAKS;
    int ybrcap = disc_y ? yf->nlev + 1 : MAX_BREAKS;
    double *xbr = cp_xmalloc(xbrcap * sizeof(double));
    double *ybr = cp_xmalloc(ybrcap * sizeof(double));
    char **xlabs = cp_xmalloc(xbrcap * sizeof(char *));
    char **ylabs = cp_xmalloc(ybrcap * sizeof(char *));
    int nxbr, nybr;
    /* genome mode uses separate axis arrays: gridlines at chrom boundaries,
     * labels (chrom names) at chrom midpoints */
    double *gax_pos = NULL; char **gax_lab = NULL; int gax_n = 0;
    if (disc_x) {                          /* one break per category, level labels */
        nxbr = xf->nlev;
        for (int i = 0; i < nxbr; i++) { xbr[i] = i + 1; xlabs[i] = cp_xstrdup(xf->levels[i]); }
    } else if (genome_x) {
        nxbr = gs->n > 1 ? gs->n - 1 : 0;  /* internal boundaries = faint gridlines */
        for (int i = 0; i < nxbr; i++) { xbr[i] = gs->off[i + 1]; xlabs[i] = cp_xstrdup(""); }
        gax_n = gs->n;
        gax_pos = cp_xmalloc(gax_n * sizeof(double));
        gax_lab = cp_xmalloc(gax_n * sizeof(char *));
        for (int i = 0; i < gs->n; i++) {
            gax_pos[i] = NPCX(gs->off[i] + gs->len[i] / 2);
            const char *nm = gs->chr[i];
            if (!strncmp(nm, "chr", 3)) nm += 3;   /* compact: chr1 -> 1 */
            gax_lab[i] = cp_xstrdup(nm);
        }
    } else {
        nxbr = axis_breaks(spec->log_x, x0, x1, spec->n_x_breaks, spec->x_breaks,
                           spec->n_x_break_labs, spec->x_break_labs, spec->x_pct,
                           'x', xbr, xlabs);
    }
    if (disc_y) {                          /* one break per category, level labels */
        nybr = yf->nlev;
        for (int i = 0; i < nybr; i++) { ybr[i] = i + 1; ylabs[i] = cp_xstrdup(yf->levels[i]); }
    } else {
        nybr = axis_breaks(spec->log_y, y0, y1, spec->n_y_breaks, spec->y_breaks,
                           spec->n_y_break_labs, spec->y_break_labs, spec->y_pct,
                           'y', ybr, ylabs);
    }
    double *xnpc = cp_xmalloc(nxbr * sizeof(double)), *ynpc = cp_xmalloc(nybr * sizeof(double));
    for (int i = 0; i < nxbr; i++) xnpc[i] = NPCX(xbr[i]);
    for (int i = 0; i < nybr; i++) ynpc[i] = NPCY(ybr[i]);
    /* make_minors() writes up to nmaj + 1 entries, and breaks=c(...) may give
     * MAX_BREAKS majors; the old 32-double stack buffers overran past 31 */
    double *xmin_br = cp_xmalloc((MAX_BREAKS + 1) * sizeof(double));
    double *ymin_br = cp_xmalloc((MAX_BREAKS + 1) * sizeof(double));
    int nxmin = (disc_x || genome_x) ? 0
              : spec->log_x ? log_minors(spec->log_x, x0, x1, xmin_br, 32)
              : make_minors(xbr, nxbr, x0, x1, xmin_br);
    int nymin = disc_y ? 0
              : spec->log_y ? log_minors(spec->log_y, y0, y1, ymin_br, 32)
              : make_minors(ybr, nybr, y0, y1, ymin_br);

    /* log tick marks drawn INSIDE the panel from the axis edge inward. */
    double xlt_pos[80], xlt_len[80]; int xlt_n = 0;
    double ylt_pos[80], ylt_len[80]; int ylt_n = 0;
    if (spec->log_x) {
        xlt_n = log_tick_marks(spec->log_x, x0, x1, xlt_pos, xlt_len, 80);
        for (int i = 0; i < xlt_n; i++) xlt_pos[i] = NPCX(xlt_pos[i]);
    }
    if (spec->log_y) {
        ylt_n = log_tick_marks(spec->log_y, y0, y1, ylt_pos, ylt_len, 80);
        for (int i = 0; i < ylt_n; i++) ylt_pos[i] = NPCY(ylt_pos[i]);
    }

    /* ---- gather the trained range and its breaks into the shared panel scale.
     * Under facet_wrap(scales="fixed") -- every figure today -- all panels point
     * at this one instance, so the panel loop reads it exactly as it read the
     * loose variables before, and the output is unchanged. Free scales replace
     * individual entries with panel-specific ones. ---- */
    PanelScale shared = {0};
    shared.x0 = x0; shared.x1 = x1; shared.y0 = y0; shared.y1 = y1;
    shared.xbr = xbr; shared.nxbr = nxbr; shared.xlabs = xlabs; shared.xnpc = xnpc;
    shared.ybr = ybr; shared.nybr = nybr; shared.ylabs = ylabs; shared.ynpc = ynpc;
    shared.xmin_br = xmin_br; shared.nxmin = nxmin;
    shared.ymin_br = ymin_br; shared.nymin = nymin;
    shared.xlt_pos = xlt_pos; shared.xlt_len = xlt_len; shared.xlt_n = xlt_n;
    shared.ylt_pos = ylt_pos; shared.ylt_len = ylt_len; shared.ylt_n = ylt_n;
    shared.nxlev = disc_x ? xf->nlev : 0;
    shared.nylev = disc_y ? yf->nlev : 0;
    shared.shared = 1;
    PanelScale *ps = cp_xmalloc(npan * sizeof(PanelScale));
    for (int p = 0; p < npan; p++) ps[p] = shared;

    /* ---- facet_wrap(scales=): give each panel its own range and breaks ----
     *
     * Only the axes the caller freed are replaced; the rest keep pointing at the
     * shared instance, so scales="free_x" leaves the y axis provably identical
     * to a fixed figure. Panels are trained from their own rows, then the same
     * expansion and break rules run again per panel. */
    if (ff && (spec->free_x || spec->free_y)) {
        for (int p = 0; p < npan; p++) {
            PanelScale *S = &ps[p];
            S->shared = 0;
            /* the un-expanded ranges the reference lines train against: the
             * shared ones unless this pass frees the axis (set below) */
            double plo_x = txmin, phi_x = txmax, plo_y = tymin, phi_y = tymax;
            if (spec->free_x) {
                if (disc_x) {
                    /* Keep the global level ORDER, drop the levels this panel
                     * has no rows for, and renumber what is left to 1..k. That
                     * is ggplot2's drop = TRUE, and it means an explicit
                     * factor(x, levels=) still decides the ordering. */
                    int *map = cp_xmalloc(xf->nlev * sizeof(int));
                    for (int l = 0; l < xf->nlev; l++) map[l] = -1;
                    int k = 0;
                    for (int l = 0; l < xf->nlev; l++)
                        for (int r = 0; r < df->nrow; r++)
                            if (use[r] && xgf->idx[r] == XG(p) && xf->idx[r] == l) {
                                map[l] = k++; break;
                            }
                    S->xmap = map; S->nxlev = k;
                    double e = spec->has_x_expand
                             ? spec->x_exp_mult * ((k ? k : 1) - 1) + spec->x_exp_add
                             : 0.1;
                    S->x0 = 0.5 - e; S->x1 = (k ? k : 1) + 0.5 + e;
                } else if (genome_x) {
                    snprintf(err, CP_ERRLEN, "facet_wrap(scales=) cannot free a "
                             "scale_x_genome() axis; the genome axis is shared by "
                             "construction (use regions() for several windows)");
                    return -1;
                } else {
                    /* the same rows, layer files and reference lines the
                     * shared pass trains on, restricted to this panel */
                    double lo = 1e300, hi = -1e300;
                    train_rows_x(spec, df, use, xgf, XG(p), xc, xec, &lo, &hi);
                    if (lxmin < lo) lo = lxmin;      /* rect layer files, annotate() */
                    if (lxmax > hi) hi = lxmax;
                    for (int li = 0; li < spec->nlayers; li++)   /* histogram bin edges */
                        if (spec->layers[li].type == GEOM_HISTOGRAM) {
                            const Hist *hs = &hist[li];
                            if (hs->start[p] < lo) lo = hs->start[p];
                            if (hs->start[p] + hs->nb[p] * hs->width[p] > hi)
                                hi = hs->start[p] + hs->nb[p] * hs->width[p];
                        }
                    train_ref_lines(spec, 1, 0, &lo, &hi, &plo_y, &phi_y);   /* vline */
                    plo_x = lo; phi_x = hi;           /* abline reads the x range */
                    /* A panel can be empty: levels= may name a level the data
                     * never uses. Draw it blank rather than refusing -- naming
                     * it was deliberate. */
                    /* An explicit xlim() is a domain the caller chose; freeing
                     * the axis frees the BREAKS, not the limits. Without this
                     * the limit parsed, ran, and did nothing. */
                    if (spec->has_xlim) {
                        lo = cp_logt(spec->log_x, spec->xlim_lo);
                        hi = cp_logt(spec->log_x, spec->xlim_hi);
                    }
                    if (lo > hi) { lo = 0; hi = 0; }
                    if (hi == lo) { lo -= 0.5; hi += 0.5; }
                    double e = spec->has_x_expand
                             ? spec->x_exp_mult * (hi - lo) + spec->x_exp_add
                             : 0.05 * (hi - lo);
                    S->x0 = lo - e; S->x1 = hi + e;
                }
            }
            if (spec->free_y) {
                if (disc_y) {
                    int *map = cp_xmalloc(yf->nlev * sizeof(int));
                    for (int l = 0; l < yf->nlev; l++) map[l] = -1;
                    int k = 0;
                    for (int l = 0; l < yf->nlev; l++)
                        for (int r = 0; r < df->nrow; r++)
                            if (use[r] && ygf->idx[r] == YG(p) && yf->idx[r] == l) {
                                map[l] = k++; break;
                            }
                    S->ymap = map; S->nylev = k;
                    double e = spec->has_y_expand
                             ? spec->y_exp_mult * ((k ? k : 1) - 1) + spec->y_exp_add
                             : 0.1;
                    S->y0 = 0.5 - e; S->y1 = (k ? k : 1) + 0.5 + e;
                } else {
                    double lo = 1e300, hi = -1e300;
                    /* The stat geoms take their height from a computed maximum
                     * rather than from the rows, so each reads that panel's own
                     * counts instead of the figure-wide one. */
                    if (nhist) {
                        int mx = 0;
                        for (int li = 0; li < spec->nlayers; li++) {
                            if (spec->layers[li].type != GEOM_HISTOGRAM) continue;
                            const Hist *hs = &hist[li];
                            for (int q = 0; q < npan; q++) {
                                if (YG(q) != YG(p)) continue;
                                for (int b = 0; b < hs->nbins; b++) {
                                    int cnt = hs->counts[q * hs->nbins + b];
                                    if (cnt > mx) mx = cnt;
                                }
                            }
                        }
                        lo = 0; hi = cp_logt(spec->log_y, (double)(mx ? mx : 1));
                    } else if (hasbar) {
                        int mx = 0;
                        for (int q = 0; q < npan; q++) {
                            if (YG(q) != YG(p)) continue;
                            for (int cat = 0; cat < xf->nlev; cat++) {
                                int total = 0;
                                for (int gq = 0; gq < barng; gq++)
                                    total += barcount[((size_t)(q * xf->nlev + cat)) * barng + gq];
                                if (total > mx) mx = total;
                            }
                        }
                        lo = 0; hi = cp_logt(spec->log_y, (double)(mx ? mx : 1));
                    } else if (hasdens) {
                        double mx = 0;
                        for (int di = 0; di < ndens; di++)
                            for (int q = 0; q < npan; q++) {
                                if (YG(q) != YG(p)) continue;
                                for (int gg = 0; gg < densg; gg++) {
                                    size_t base = ((size_t)((di * npan + q) * densg + gg)) * DENS_N;
                                    for (int j = 0; j < DENS_N; j++)
                                        if (dens_y[base + j] > mx) mx = dens_y[base + j];
                                }
                            }
                        lo = 0; hi = mx;
                    }
                    if (!nhist && !hasbar && !hasdens) {
                        train_rows_y(spec, df, use, ygf, YG(p), yc, yec, yminc, &lo, &hi);
                        if (lymin < lo) lo = lymin;  /* rect layer files, annotate() */
                        if (lymax > hi) hi = lymax;
                        for (int q = 0; q < npan; q++) {
                            if (YG(q) != YG(p)) continue;
                            if (colstack_max && TY(colstack_max[q]) > hi)
                                hi = TY(colstack_max[q]);   /* stacked totals */
                            if (colstack_min && !spec->log_y && colstack_min[q] < lo)
                                lo = colstack_min[q];
                        }
                        if (hascol && !spec->log_y) {   /* bars anchor at 0 */
                            if (lo > 0) lo = 0;
                            if (hi < 0) hi = 0;
                        }
                    }
                    /* hline/abline intercepts, against this panel's x range
                     * when that is free too, else the shared one */
                    train_ref_lines(spec, 0, 1, (disc_x || genome_x) ? NULL : &plo_x,
                                    (disc_x || genome_x) ? NULL : &phi_x, &lo, &hi);
                    if (spec->has_ylim) {      /* as for x above */
                        lo = cp_logt(spec->log_y, spec->ylim_lo);
                        hi = cp_logt(spec->log_y, spec->ylim_hi);
                    }
                    if (lo > hi) { lo = 0; hi = 0; }
                    if (hi == lo) { lo -= 0.5; hi += 0.5; }
                    double e = spec->has_y_expand
                             ? spec->y_exp_mult * (hi - lo) + spec->y_exp_add
                             : 0.05 * (hi - lo);
                    S->y0 = lo - e; S->y1 = hi + e;
                }
            }
        }
        /* Breaks for the freed axes. Same rules as the shared pass above, run
         * once per panel over that panel's range; the arrays are allocated to
         * the size actually needed, so a discrete axis is no longer capped at
         * the 40 the old fixed buffers held. */
        for (int p = 0; p < npan; p++) {
            PanelScale *S = &ps[p];
            if (spec->free_x) {
                if (disc_x) {
                    S->nxbr = S->nxlev;
                    S->xbr = cp_xmalloc((S->nxbr + 1) * sizeof(double));
                    S->xlabs = cp_xmalloc((S->nxbr + 1) * sizeof(char *));
                    for (int l = 0; l < xf->nlev; l++)
                        if (S->xmap[l] >= 0) {
                            S->xbr[S->xmap[l]] = S->xmap[l] + 1;
                            S->xlabs[S->xmap[l]] = cp_xstrdup(xf->levels[l]);
                        }
                } else {
                    /* breaks=/labels= apply per panel: each freed panel keeps
                     * the given breaks that land inside ITS range */
                    S->xbr = cp_xmalloc(MAX_BREAKS * sizeof(double));
                    S->xlabs = cp_xmalloc(MAX_BREAKS * sizeof(char *));
                    S->nxbr = axis_breaks(spec->log_x, S->x0, S->x1, spec->n_x_breaks,
                                          spec->x_breaks, spec->n_x_break_labs,
                                          spec->x_break_labs, spec->x_pct, 'x',
                                          S->xbr, S->xlabs);
                }
                S->xnpc = cp_xmalloc((S->nxbr + 1) * sizeof(double));
                for (int i = 0; i < S->nxbr; i++)
                    S->xnpc[i] = (S->xbr[i] - S->x0) / (S->x1 - S->x0);
                S->xmin_br = cp_xmalloc((MAX_BREAKS + 1) * sizeof(double));
                S->nxmin = disc_x ? 0
                         : spec->log_x ? log_minors(spec->log_x, S->x0, S->x1, S->xmin_br, 32)
                         : make_minors(S->xbr, S->nxbr, S->x0, S->x1, S->xmin_br);
                S->xlt_n = 0;
                if (spec->log_x) {
                    S->xlt_pos = cp_xmalloc(80 * sizeof(double));
                    S->xlt_len = cp_xmalloc(80 * sizeof(double));
                    S->xlt_n = log_tick_marks(spec->log_x, S->x0, S->x1,
                                              S->xlt_pos, S->xlt_len, 80);
                    for (int i = 0; i < S->xlt_n; i++)
                        S->xlt_pos[i] = (S->xlt_pos[i] - S->x0) / (S->x1 - S->x0);
                }
            }
            if (spec->free_y) {
                if (disc_y) {
                    S->nybr = S->nylev;
                    S->ybr = cp_xmalloc((S->nybr + 1) * sizeof(double));
                    S->ylabs = cp_xmalloc((S->nybr + 1) * sizeof(char *));
                    for (int l = 0; l < yf->nlev; l++)
                        if (S->ymap[l] >= 0) {
                            S->ybr[S->ymap[l]] = S->ymap[l] + 1;
                            S->ylabs[S->ymap[l]] = cp_xstrdup(yf->levels[l]);
                        }
                } else {
                    S->ybr = cp_xmalloc(MAX_BREAKS * sizeof(double));
                    S->ylabs = cp_xmalloc(MAX_BREAKS * sizeof(char *));
                    S->nybr = axis_breaks(spec->log_y, S->y0, S->y1, spec->n_y_breaks,
                                          spec->y_breaks, spec->n_y_break_labs,
                                          spec->y_break_labs, spec->y_pct, 'y',
                                          S->ybr, S->ylabs);
                }
                S->ynpc = cp_xmalloc((S->nybr + 1) * sizeof(double));
                for (int i = 0; i < S->nybr; i++)
                    S->ynpc[i] = (S->ybr[i] - S->y0) / (S->y1 - S->y0);
                S->ymin_br = cp_xmalloc((MAX_BREAKS + 1) * sizeof(double));
                S->nymin = disc_y ? 0
                         : spec->log_y ? log_minors(spec->log_y, S->y0, S->y1, S->ymin_br, 32)
                         : make_minors(S->ybr, S->nybr, S->y0, S->y1, S->ymin_br);
                S->ylt_n = 0;
                if (spec->log_y) {
                    S->ylt_pos = cp_xmalloc(80 * sizeof(double));
                    S->ylt_len = cp_xmalloc(80 * sizeof(double));
                    S->ylt_n = log_tick_marks(spec->log_y, S->y0, S->y1,
                                              S->ylt_pos, S->ylt_len, 80);
                    for (int i = 0; i < S->ylt_n; i++)
                        S->ylt_pos[i] = (S->ylt_pos[i] - S->y0) / (S->y1 - S->y0);
                }
            }
        }
    }

    /* ---- geom_col bar width: 0.9 x min gap between distinct x. The same
     * 0.9 x resolution is ggplot's default errorbar cap (width=), so a cap on
     * x = 0.1, 0.2, 0.3 no longer runs from edge to edge; the resolution of a
     * discrete axis is 1. ---- */
    double colw = 0.9;
    int haseb = 0;
    for (int li = 0; li < spec->nlayers; li++)
        if (spec->layers[li].type == GEOM_ERRORBAR) haseb = 1;
    if (hascol || haseb) {
        double *xs = cp_xmalloc(nuse * sizeof(double));
        int nx = 0;
        for (int r = 0; r < df->nrow; r++)
            if (use[r]) xs[nx++] = TXR(r);
        qsort(xs, nx, sizeof(double), cmp_double);
        double res = 1e300;
        for (int i = 1; i < nx; i++)
            if (xs[i] - xs[i - 1] > 1e-9 && xs[i] - xs[i - 1] < res) res = xs[i] - xs[i - 1];
        colw = res < 1e300 ? 0.9 * res : 0.9;
        free(xs);
    }

    /* ---- boxplot dodge: side-by-side boxes when colour is a different
     * grouping than x (i.e. some x-category holds >1 colour group) ---- */
    int box_dodge = 0;
    if (hasbox && cf && disc_x) {
        for (int cat = 0; cat < xf->nlev && !box_dodge; cat++) {
            int *seen = cp_xcalloc(cf->nlev, sizeof(int)), cnt = 0;
            for (int r = 0; r < df->nrow; r++)
                if (use[r] && xf->idx[r] == cat && cf->idx[r] >= 0 && !seen[cf->idx[r]]) {
                    seen[cf->idx[r]] = 1; cnt++;
                }
            if (cnt > 1) box_dodge = 1;
            free(seen);
        }
    }
    int box_slots = box_dodge ? cf->nlev : 1;

/* Four grid rows per panel row: facet strip, panel, annotation band (zero-
 * height when no annotation() is given), then the inter-row gap / freed axis. */
#define SR(r) (3 + 4 * (r))
#define PR(r) (4 + 4 * (r))
#define PC(c) (4 + 2 * (c))

    /* ---- measurement, then the real surface ----
     * The canvas may still be sized from its own labels below, and a surface is
     * fixed at creation, so measure on a scratch one first and open the output
     * only once the size is settled. Nothing is drawn before then: gt_add only
     * records grobs, and gt_render runs at the end. */
    /* Font metrics differ between surface types, so measuring on a scratch
     * surface and drawing on another moves text by a hair. Only the auto-fit
     * path can afford that -- it just needs a size that fits -- so a caller who
     * gave a size gets the output surface from the start, exactly as before. */
    int autosize = (w_pt <= 0 || h_pt <= 0);
    cairo_surface_t *msurf = NULL, *surf = NULL;
    cairo_t *cr;
    if (autosize) {
        msurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
        cr = cairo_create(msurf);
    } else {
        surf = cp_surface_create(out, w_pt, h_pt);
        cr = cairo_create(surf);
    }
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    /* the active theme, with its chrome line widths scaled by the effective
     * base_line_size (spec beats the env var beats the 0.5 default) */
    if (spec->base_line_size > 0) cp_line_scale = spec->base_line_size / 0.5;
    Theme thv = THEMES[spec->theme];
    thv.grid_major_lw *= cp_line_scale;
    thv.grid_minor_lw *= cp_line_scale;
    thv.border_lw     *= cp_line_scale;
    thv.axis_line_lw  *= cp_line_scale;
    const Theme *th = &thv;

    /* Under coord_flip the x aesthetic is drawn on the LEFT (vertical) axis and
     * y on the BOTTOM; otherwise the usual y-left / x-bottom. lax = left axis
     * (ticks at npc-y positions), bax = bottom axis (ticks at npc-x). */
    int lax_n, bax_n; double *bax_pos; char **lax_lab, **bax_lab;
    if (flip) {
        lax_n = nxbr; lax_lab = xlabs;
        bax_n = nybr; bax_pos = ynpc; bax_lab = ylabs;
    } else {
        lax_n = nybr; lax_lab = ylabs;
        bax_n = nxbr; bax_pos = xnpc; bax_lab = xlabs;
    }

    /* Width reserved for the left axis labels. A freed left axis differs per
     * panel, and only the left-most column sits against this margin, so it is
     * the widest label among the panels in column 0 -- the other columns are
     * carried by the gaps sized above. */
    double ylab_w = 0;
    if (flip ? spec->free_x : spec->free_y) {
        for (int p = 0; p < npan; p++) {
            if (p % ncolp != 0) continue;
            const PanelScale *S = &ps[p];
            int n = flip ? S->nxbr : S->nybr;
            char **lb = flip ? S->xlabs : S->ylabs;
            for (int i = 0; i < n; i++) {
                double w = cp_label_w(cr, SZ_AXIS_TEXT, lb[i]);
                if (w > ylab_w) ylab_w = w;
            }
        }
    } else {
        for (int i = 0; i < lax_n; i++) {
            double w = cp_label_w(cr, SZ_AXIS_TEXT, lax_lab[i]);   /* superscript-aware */
            if (w > ylab_w) ylab_w = w;
        }
    }
    double labh = font_h(cr, SZ_AXIS_TEXT), baseh = font_h(cr, SZ_BASE);
    double striph = ff ? labh + 2 * STRIP_PAD : 0;
    /* facet_wrap strips every panel row. facet_grid instead puts ONE row of
     * column strips along the top and a column of rotated row strips down the
     * right, ggplot2-style, so an 8-panel grid spends one strip's height
     * rather than four. */
    double striph_top  = (!grid || colf) ? striph : 0;
    double striph_side = (grid && rowf) ? striph : 0;
    int nstriprows = grid ? (colf ? 1 : 0) : nrowp;

    const char *xtitle = spec->lab_x ? spec->lab_x : genome_x ? "" : spec->x.expr;
    const char *ytitle = spec->lab_y ? spec->lab_y
                       : nhist || hasbar ? "count" : hasdens ? "density" : spec->y.expr;
    /* axis titles follow the flip: the left (rotated) title names the vertical
     * axis, the bottom title the horizontal axis */
    const char *left_title   = flip ? xtitle : ytitle;
    const char *bottom_title = flip ? ytitle : xtitle;

    /* ---- grammar-mode annotation(): a categorical metadata band under each
     * panel, keyed by x category name — the wheatmap idea brought over, so a
     * cohort strip (IGHV status, batch) stops riding the fill scale and
     * polluting its legend. Each band carries its own palette and legend.
     * File shape: first column = x category names (text), column= picks the
     * value column (default: the last). ---- */
    struct GrAnn { Factor *f; Col *apal; int *catlev; const char *title; };
    struct GrAnn anns[MAX_HMOBJS]; int nann = 0;
    const double ANN_STRIP = 9.0, ANN_GAP = 2.0, ANN_PAD = 3.0;   /* pt */
    if (spec->nhobjs) {
        if (!disc_x || flip || genome_x) {
            snprintf(err, CP_ERRLEN, "annotation() under a grammar panel keys "
                     "on x categories, so it needs a discrete x%s",
                     flip ? " and no coord_flip()" : "");
            return -1;
        }
        for (int i = 0; i < spec->nhobjs; i++) {
            const HMObj *o = &spec->hobjs[i];
            DataFrame *ad = df_read_csv(o->data, err);
            if (!ad) return -1;
            if (ad->ncol < 2) {
                snprintf(err, CP_ERRLEN, "annotation(%s): needs a key column "
                         "(x category names) and a value column", o->data);
                return -1;
            }
            const Column *key = &ad->cols[0];
            if (key->type != COL_STR) {
                snprintf(err, CP_ERRLEN, "annotation(%s): the first column must "
                         "be text naming the x categories", o->data);
                return -1;
            }
            const Column *val;
            if (o->column) {
                val = df_col(ad, o->column);
                if (!val) {
                    snprintf(err, CP_ERRLEN, "annotation(%s): column `%s` not "
                             "found", o->data, o->column);
                    return -1;
                }
            } else val = &ad->cols[ad->ncol - 1];
            Factor *af = factor_make(ad, val);
            int *catlev = cp_xmalloc(xf->nlev * sizeof(int));
            int miss = 0;
            for (int l = 0; l < xf->nlev; l++) {
                catlev[l] = -1;
                for (int r2 = 0; r2 < ad->nrow; r2++)
                    if (!strcmp(key->str[r2], xf->levels[l])) {
                        catlev[l] = af->idx[r2];
                        break;
                    }
                if (catlev[l] < 0) miss++;
            }
            if (miss)
                fprintf(stderr, "cinderplot: warning: annotation(%s): %d of %d "
                        "x categories have no row; drawn in the missing-value "
                        "grey\n", o->data, miss, xf->nlev);
            Col *apal = cp_xmalloc(af->nlev * sizeof(Col));
            hue_palette(af->nlev, apal);
            anns[nann].f = af; anns[nann].apal = apal; anns[nann].catlev = catlev;
            anns[nann].title = o->column ? o->column : val->name;
            nann++;
        }
    }
    double band_h = nann ? ANN_PAD + nann * ANN_STRIP + (nann - 1) * ANN_GAP : 0;

    /* ---- facet_wrap(scales="free_colour"): each facet owns its colour
     * scale. One figure, several colour meanings — the same cells coloured
     * by origin in one panel and by patient in the next — with one legend
     * block per panel, titled by the panel's name. Per-panel palettes live
     * in fc_pal[p * cf->nlev + level]; levels absent from a panel stay NA
     * (never drawn). ---- */
    Col *fc_pal = NULL;
    GTable *fc_leg[12] = {0};        /* free_colour, single facet row: the
                                      * per-panel legend blocks */
    if (spec->free_colour) {
        if (!ff) {
            snprintf(err, CP_ERRLEN, "scales=\"free_colour\" frees the colour "
                     "scale per facet and needs facet_wrap()");
            return -1;
        }
        if (!cf) {
            snprintf(err, CP_ERRLEN, "scales=\"free_colour\" needs a discrete "
                     "colour/fill aesthetic (a continuous one has a single "
                     "shared ramp)");
            return -1;
        }
        if (spec->identity_scale) {
            snprintf(err, CP_ERRLEN, "scales=\"free_colour\" does nothing "
                     "under scale_*_identity(); drop one of them");
            return -1;
        }
        if (npan > 12) {
            snprintf(err, CP_ERRLEN, "scales=\"free_colour\" draws one legend "
                     "per facet and supports at most 12 facets (%d given)", npan);
            return -1;
        }
        int named = spec->has_manual && spec->n_manual > 0
                  && spec->manual_names[0] != NULL;
        fc_pal = cp_xmalloc((size_t)npan * cf->nlev * sizeof(Col));
        for (int i = 0; i < npan * cf->nlev; i++) fc_pal[i] = C_NA;
        int *present = cp_xmalloc(cf->nlev * sizeof(int));
        Col *tmp = cp_xmalloc(cf->nlev * sizeof(Col));
        for (int p = 0; p < npan; p++) {
            int k = 0;
            for (int l = 0; l < cf->nlev; l++) {
                int seen = 0;
                for (int rr = 0; rr < df->nrow; rr++)
                    if (use[rr] && ff->idx[rr] == p && cf->idx[rr] == l)
                        { seen = 1; break; }
                if (seen) present[k++] = l;
            }
            if (!named && (spec->has_manual || spec->brewer_disc)
                && k > spec->n_manual) {
                snprintf(err, CP_ERRLEN, "facet `%s` has %d colour levels but "
                         "the %s gives %d", ff->levels[p], k,
                         spec->brewer_disc ? "palette" : "values= list",
                         spec->n_manual);
                return -1;
            }
            if (!spec->has_manual && !spec->brewer_disc) hue_palette(k, tmp);
            for (int j = 0; j < k; j++) {
                int l = present[j];
                Col c2 = C_NA;
                if (named) {
                    for (int m2 = 0; m2 < spec->n_manual; m2++)
                        if (spec->manual_names[m2]
                            && !strcmp(spec->manual_names[m2], cf->levels[l]))
                            { c2 = spec->manual_cols[m2]; break; }
                } else if (spec->has_manual || spec->brewer_disc) {
                    c2 = spec->manual_cols[j];   /* positional, per panel */
                } else c2 = tmp[j];
                fc_pal[(size_t)p * cf->nlev + l] = c2;
            }
        }
        free(present); free(tmp);
    }

    /* ---- the panel size the auto-fit below will choose, worked out here
     * because a legend that would not fit those panels has to fold BEFORE it
     * is built. The auto-fit block ("auto-fit the canvas") reads these same
     * values, so the two cannot disagree. ---- */
    double catpitch = labh * 1.15;          /* readable pitch for one category */
    int maxx = 0, maxy = 0;
    for (int p = 0; p < npan; p++) {
        if (ps[p].nxlev > maxx) maxx = ps[p].nxlev;
        if (ps[p].nylev > maxy) maxy = ps[p].nylev;
    }
    /* coord_flip() turns the category axis vertical, so the x categories
     * then drive the panel HEIGHT; sizing the width from them left 40 bars
     * overprinting in a 4in-tall panel */
    int hcats = flip ? maxy : maxx, vcats = flip ? maxx : maxy;
    double auto_panelw = hcats ? hcats * catpitch : 4.0 * 72;
    double auto_panelh = vcats ? vcats * catpitch : 2.6 * 72;
    double auto_h_raw = MARGIN * 2 + labh + TICK_LEN + TXT_GAP + baseh
                      + (spec->lab_title ? font_h(cr, SZ_TITLE) : 0)
                      + nrowp * (auto_panelh + band_h) + nstriprows * striph
                      + (nrowp - 1) * PANEL_SPACE;
    double auto_h = fmin(30.0 * 72, fmax(4.0 * 72, auto_h_raw));   /* the clamped canvas */
    /* the rows the legend column does not span (it sits from the first strip
     * to the last panel): title block, last band, axis rows and margins */
    double leg_chrome_h = MARGIN + (spec->lab_title ? font_h(cr, SZ_TITLE) : 0)
                        + (spec->lab_subtitle ? baseh : spec->lab_title ? HALF_LINE : 0)
                        + band_h + TICK_LEN + TXT_GAP + labh + HALF_LINE / 2 + baseh
                        + (spec->lab_caption ? fmax(MARGIN, labh) : MARGIN);

    Col *pal = NULL;
    GTable *leg = NULL;
    const char *col_title = spec->lab_colour ? spec->lab_colour : spec->colour.expr;
    /* Guides stack top-to-bottom: colour (or fill) first, then size — the
     * order ggplot uses for a point layer mapping both; annotation() bands
     * append one legend block each, titled by their value column. */
    GTable *guides[3 + MAX_HMOBJS]; int nguide = 0;
    if (cf) {
        pal = cp_xmalloc(cf->nlev * sizeof(Col));
        if (spec->identity_scale) {
            /* the level string is the colour (ggplot's scale_*_identity):
             * a hex/name column paints itself, and there is no legend --
             * the colours state nothing beyond themselves. */
            for (int i = 0; i < cf->nlev; i++)
                if (parse_color(cf->levels[i], &pal[i])) {
                    snprintf(err, CP_ERRLEN, "scale_*_identity: level `%s` of "
                             "`%s` is not a colour (use names or #RRGGBB)",
                             cf->levels[i], spec->colour.col);
                    return -1;
                }
        } else if (spec->has_manual && !spec->brewer_disc
            && spec->n_manual > 0 && spec->manual_names[0] == NULL
            && cf->nlev > spec->n_manual) {
            /* a positional list shorter than the factor painted the tail
             * levels NA-grey with no warning; like the brewer sets, say it. */
            snprintf(err, CP_ERRLEN, "scale_*_manual gives %d colours; `%s` "
                     "has %d levels", spec->n_manual, spec->colour.col,
                     cf->nlev);
            return -1;
        }
        if (spec->brewer_disc && !spec->free_colour
            && cf->nlev > spec->n_manual) {
            /* scale_*_manual tolerates a short positional list (grey fill),
             * but a named Brewer set running out would silently grey the tail
             * levels -- say it instead. */
            snprintf(err, CP_ERRLEN, "palette `%s` has %d colours; `%s` has %d "
                     "levels", spec->brewer_disc, spec->n_manual,
                     spec->colour.col, cf->nlev);
            return -1;
        }
        if (spec->has_manual) {                 /* scale_*_manual(values=) */
            int named = spec->n_manual > 0 && spec->manual_names[0] != NULL;
            for (int i = 0; i < cf->nlev; i++) {
                Col c = C_NA;                   /* grey for an unmapped level */
                if (named) {
                    for (int k = 0; k < spec->n_manual; k++)
                        if (spec->manual_names[k] && !strcmp(spec->manual_names[k], cf->levels[i]))
                            { c = spec->manual_cols[k]; break; }
                } else if (i < spec->n_manual) c = spec->manual_cols[i];
                pal[i] = c;
            }
        } else if (!spec->identity_scale) hue_palette(cf->nlev, pal);
        /* The palette is still built when the legend is suppressed -- the marks
         * and bars are coloured from it; only the guide is dropped. An
         * identity scale never draws one: the colours state nothing beyond
         * themselves (ggplot's guide = "none" default for identity). */
        if (spec->free_colour && !spec->no_legend) {
            /* one legend block per facet, titled by the facet's name, keyed
             * to that facet's own palette and level subset. With the facets
             * in ONE ROW, each block goes in a legend row directly under its
             * own panel (the patchwork look); multi-row facet grids fall
             * back to the shared right-margin stack. */
            for (int p = 0; p < npan; p++) {
                int k = 0;
                char **lv = cp_xmalloc(cf->nlev * sizeof(char *));
                Col *pc2 = cp_xmalloc(cf->nlev * sizeof(Col));
                for (int l = 0; l < cf->nlev; l++) {
                    Col c2 = fc_pal[(size_t)p * cf->nlev + l];
                    if (c2.r == C_NA.r && c2.g == C_NA.g && c2.b == C_NA.b)
                        continue;                /* level absent from panel */
                    lv[k] = cf->levels[l]; pc2[k] = c2; k++;
                }
                if (!k) { free(lv); free(pc2); continue; }
                Factor pf = { k, lv, NULL };
                int nc2;
                if (spec->legend_ncol) nc2 = spec->legend_ncol;
                else if (spec->legend_nrow)
                    nc2 = (k + spec->legend_nrow - 1) / spec->legend_nrow;
                else {
                    /* automatic fold: as many columns as fit this panel's
                     * width (a rough chrome estimate suffices for a folding
                     * heuristic); when the canvas is auto-sized, cap the
                     * block at 6 rows instead */
                    if (w_pt > 0) {
                        double colw3 = (w_pt - 86) / ncolp - PANEL_SPACE;
                        nc2 = 1;
                        while (nc2 < k && leg_est_w(cr, lv, k, nc2 + 1) <= colw3
                               && (k + nc2) / (nc2 + 1) < (k + nc2 - 1) / nc2)
                            nc2++;
                    } else nc2 = (k + 5) / 6;
                }
                /* labs(colour=) overrides the facet-name title; an empty
                 * string drops it, since the strip right above already
                 * names the facet */
                const char *bt = spec->lab_colour ? spec->lab_colour
                               : ff->levels[p];
                GTable *lg = build_legend(cr, th, bt, &pf, pc2,
                                   haspoint, hasline || hasseg || hasdens || hasrange,
                                   hasbox || hasbar || hascol || hasrect || hastile,
                                   hastext, NULL, nc2, spec->legend_reverse, err);
                if (!lg) return -1;
                /* inside the panels every block has its own panel to sit
                 * in whatever the grid shape; only the margin placement
                 * needs the single row */
                if (nrowp == 1 || spec->legend_inside) fc_leg[p] = lg;
                else guides[nguide++] = lg;
            }
        } else if (!spec->no_legend && !spec->identity_scale) {
            int nc2 = spec->legend_ncol ? spec->legend_ncol
                    : spec->legend_nrow
                    ? (cf->nlev + spec->legend_nrow - 1) / spec->legend_nrow : 1;
            if (!spec->legend_ncol && !spec->legend_nrow && h_pt <= 0) {
                /* auto-fit: a legend up to a quarter taller than the figure
                 * stretches it (a few extra rows read better than a second
                 * column); past that it folds into as many columns as the
                 * natural panel height holds, as free_colour blocks do. A
                 * 32-level stack used to grow the canvas to 9in and still
                 * clip its title. */
                double pitch = KEY_SIZE + 0.4 * HALF_LINE;
                double thead = col_title && *col_title ? baseh + HALF_LINE : 0;
                double stack_h = thead + cf->nlev * pitch - 0.4 * HALF_LINE;
                if (stack_h + leg_chrome_h > 1.25 * auto_h) {
                    int rows_fit = (int)((auto_h - leg_chrome_h - thead + 0.4 * HALF_LINE) / pitch);
                    if (rows_fit < 1) rows_fit = 1;
                    nc2 = (cf->nlev + rows_fit - 1) / rows_fit;
                }
            }
            GTable *lg = build_legend(cr, th, col_title, cf, pal, haspoint,
                               hasline || hasseg || hasdens || hasrange,
                               hasbox || hasbar || hascol || hasrect || hastile,
                               hastext, NULL, nc2, spec->legend_reverse, err);
            if (!lg) return -1;
            guides[nguide++] = lg;
        }
    } else if (cont_col && !spec->no_legend) {
        guides[nguide++] = build_colorbar_legend(cr, th, col_title, &cscale, cdmin, cdmax);
    }
    if (szc && !spec->no_legend_size) {          /* size legend: representative breaks */
        double sbr[16]; int nsb = extended_breaks(szmin, szmax, 5, sbr, 16), nf = 0;
        for (int i = 0; i < nsb; i++) if (sbr[i] >= szmin && sbr[i] <= szmax) sbr[nf++] = sbr[i];
        if (nf == 0) { sbr[0] = szmin; sbr[1] = szmax; nf = szmax > szmin ? 2 : 1; }
        double srad[16];
        for (int i = 0; i < nf; i++) srad[i] = size_to_radius(sbr[i], szmin, szmax);
        int sdec = axis_decimals(sbr, nf);
        const char *sz_title = spec->size.expr;
        guides[nguide++] = build_size_legend(cr, th, sz_title, sbr, srad, nf, sdec);
    }
    if (shf && !spec->no_legend_shape) {
        /* A shape legend keys the GLYPH, so its swatches are all one colour --
         * otherwise the reader reads a colour that means nothing. */
        Col *spal = cp_xmalloc(shf->nlev * sizeof(Col));
        int *sidx = cp_xmalloc(shf->nlev * sizeof(int));
        for (int i = 0; i < shf->nlev; i++) { spal[i] = C_BLACK; sidx[i] = i; }
        const char *sh_title = spec->shape.expr;
        GTable *lg = build_legend(cr, th, sh_title, shf, spal, 1, 0, 0, 0, sidx, 1, 0, err);
        if (!lg) return -1;
        guides[nguide++] = lg;
    }
    for (int a = 0; a < nann; a++)               /* annotation band keys */
        if (!spec->no_legend) {
            GTable *lg = build_legend(cr, th, anns[a].title, anns[a].f,
                                      anns[a].apal, 0, 0, 1, 0, NULL, 1, 0, err);
            if (!lg) return -1;
            guides[nguide++] = lg;
        }
    if (nguide) leg = stack_guides(guides, nguide);
    GTable *inside_leg = NULL;
    if (spec->legend_inside && leg) {
        if (npan > 1) {
            snprintf(err, CP_ERRLEN, "theme(legend.position=\"inside\") with "
                     "facets needs scales=\"free_colour\" (each block goes "
                     "inside its own panel); a single shared %s legend has no "
                     "one panel to sit in", spec->free_colour ? "size/shape"
                     : "");
            return -1;
        }
        inside_leg = leg;
        leg = NULL;                  /* nothing reserved in the margins */
    }
    double fc_leg_h = 0, fc_leg_w = 0;   /* per-panel legend row: max height/width */
    int fc_leg_wp = -1;                  /* which facet owns the widest block */
    for (int p = 0; p < npan && p < 12; p++)
        if (fc_leg[p]) {
            if (gt_fixed_h(fc_leg[p]) > fc_leg_h) fc_leg_h = gt_fixed_h(fc_leg[p]);
            if (gt_fixed_w(fc_leg[p]) > fc_leg_w) { fc_leg_w = gt_fixed_w(fc_leg[p]); fc_leg_wp = p; }
        }
    if (spec->legend_inside) { fc_leg_h = 0; fc_leg_w = 0; }   /* inside the panels */
    if (fc_leg_h > 0) fc_leg_h += HALF_LINE;

    /* ---- outer table ---- */
    GTable *T = cp_xcalloc(1, sizeof(GTable));
    /* the extra column is facet_grid's right-hand strip; the legend columns
     * stay addressed from the right end, so nothing else moves */
    int rstrip_c = 2 * ncolp + 3;
    T->ncol = 2 * ncolp + 6 + (grid ? 1 : 0);
    T->colw[0] = upt(MARGIN);
    T->colw[1] = upt(baseh);
    T->colw[2] = upt(HALF_LINE / 2);
    T->colw[3] = upt(ylab_w + TXT_GAP + TICK_LEN);
    /* Under free scales the gap between panels has to hold an axis, not just
     * whitespace: every panel carries its own ticks and labels. Each gap is
     * sized by the widest label of the panels immediately to its right. */
    int lfree_l = (flip ? spec->free_x : spec->free_y) && !grid;
    for (int c = 0; c < ncolp; c++) {
        T->colw[PC(c)] = unull(1);
        if (c < ncolp - 1) {
            double gap = PANEL_SPACE;
            if (lfree_l) {
                double wmax = 0;
                for (int p = 0; p < npan; p++) {
                    if (p % ncolp != c + 1) continue;
                    const PanelScale *S = &ps[p];
                    int n = flip ? S->nxbr : S->nybr;
                    char **lb = flip ? S->xlabs : S->ylabs;
                    for (int i = 0; i < n; i++) {
                        double w = cp_label_w(cr, SZ_AXIS_TEXT, lb[i]);
                        if (w > wmax) wmax = w;
                    }
                }
                gap = TICK_LEN + TXT_GAP + wmax + PANEL_SPACE;
            }
            T->colw[PC(c) + 1] = upt(gap);
        }
    }
    if (grid) T->colw[rstrip_c] = upt(striph_side);
    T->colw[T->ncol - 3] = upt(leg ? 2 * HALF_LINE : 0);
    T->colw[T->ncol - 2] = upt(leg ? gt_fixed_w(leg) : 0);
    T->colw[T->ncol - 1] = upt(MARGIN);

    T->nrow = 4 * nrowp + 6 + (fc_leg_h > 0 ? 1 : 0);
    T->rowh[0] = upt(MARGIN);
    T->rowh[1] = upt(spec->lab_title ? font_h(cr, SZ_TITLE) : 0);
    T->rowh[2] = upt(spec->lab_subtitle ? font_h(cr, SZ_BASE)
                   : spec->lab_title ? HALF_LINE : 0);
    /* ---- auto-fit the canvas when no --size was given ----
     * The 6x4 default ignores how much the figure has to show, so a 20-category
     * axis or an 8-panel facet was squeezed into the same box as a 3-point
     * scatter. Give every panel room for its own categories and let the figure
     * grow, but never shrink below the old default, and never touch a size the
     * caller asked for. */
    {
        if (w_pt <= 0) {
            double panelw = auto_panelw;
            /* a per-panel legend block wider than its panel widens the
             * column — the block is pinned under the panel and cannot
             * borrow a neighbour's space */
            if (fc_leg_w > panelw) panelw = fc_leg_w;
            double chrome = MARGIN + ylab_w + TICK_LEN + TXT_GAP + baseh + MARGIN
                          + striph_side       /* facet_grid row strips */
                          + (leg ? gt_fixed_w(leg) + 2 * HALF_LINE : 0);
            w_pt = chrome + ncolp * panelw + (ncolp - 1) * PANEL_SPACE;
            w_pt = fmin(30.0 * 72, fmax(6.0 * 72, w_pt));
        }
        if (h_pt <= 0) {
            h_pt = auto_h_raw;
            /* a tall legend stack (many discrete levels) sizes the canvas
             * too: its column spans strips and panels only, so it needs its
             * own height plus every row outside that span -- allowing just
             * the margins left the title clipped off the top */
            if (leg) h_pt = fmax(h_pt, gt_fixed_h(leg) + leg_chrome_h);
            h_pt += fc_leg_h;        /* per-panel legend row (free_colour) */
            h_pt = fmin(30.0 * 72, fmax(4.0 * 72, h_pt));
        }
    }
    if (fc_leg_w > 0) {
        double chrome = MARGIN + ylab_w + TICK_LEN + TXT_GAP + baseh + MARGIN;
        double colw2 = (w_pt - chrome - (ncolp - 1) * PANEL_SPACE) / ncolp;
        if (fc_leg_w > colw2)
            fprintf(stderr, "cinderplot: warning: the `%s` legend block is "
                    "%.1fin wide but its panel column is %.1fin; it will "
                    "overrun or clip — give a wider --size, fold it with "
                    "guide_legend(nrow=), or shorten the labels\n",
                    ff && fc_leg_wp >= 0 ? ff->levels[fc_leg_wp] : "widest",
                    fc_leg_w / 72, colw2 / 72);
    }
    /* the legend's cell is the figure less the rows above and below it;
     * a couple of half-lines of overhang into the margins is tolerated */
    if (leg && gt_fixed_h(leg) > h_pt - leg_chrome_h + 2 * HALF_LINE)
        fprintf(stderr, "cinderplot: warning: the legend stack needs %.1fin of "
                "a %.1fin figure and will clip; give a taller --size, fold it "
                "with guide_legend(ncol=), drop it with guides(colour=\"none\"), "
                "or reduce the levels\n",
                (gt_fixed_h(leg) + leg_chrome_h) / 72, h_pt / 72);

    if (autosize) {          /* size settled: open the real surface */
        cairo_destroy(cr); cairo_surface_destroy(msurf);
        surf = cp_surface_create(out, w_pt, h_pt);
        cr = cairo_create(surf);
        cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
    }

    /* ---- bottom-axis label angle ----
     * Discrete labels are drawn one per category, so a crowded axis runs them
     * into each other. Measure what a panel has to hold and lean them over when
     * they do not fit, which loses nothing (unlike thinning or truncating).
     * A caller who named an angle gets it, including angle=0 for "leave them
     * alone"; <0 is the default and means decide here. */
    double bang = flip ? spec->y_angle : spec->x_angle;
    int bdisc = flip ? disc_y : disc_x;
    if (bang < 0) {
        bang = 0;
        if (bdisc) {
            /* Rough panel width: the canvas less the left chrome and any
             * legend, split between the columns. Exact enough to decide
             * whether the labels fit, which is all this has to answer. */
            double chrome = MARGIN + ylab_w + TICK_LEN + TXT_GAP + baseh + MARGIN
                          + striph_side       /* facet_grid row strips */
                          + (leg ? gt_fixed_w(leg) + 2 * HALF_LINE : 0);
            double panel_w = fmax(1, (w_pt - chrome) / ncolp);
            double need = 0;
            for (int p = 0; p < npan; p++) {
                const PanelScale *S = &ps[p];
                int n = flip ? S->nybr : S->nxbr;
                char **lb = flip ? S->ylabs : S->xlabs;
                double sum = 0;
                for (int i = 0; i < n; i++)
                    sum += cp_label_w(cr, SZ_AXIS_TEXT, lb[i]) + TXT_GAP;
                if (sum > need) need = sum;
            }
            /* 45 degrees buys a factor of ~1/cos(45); past that go vertical. */
            if (need > panel_w) bang = need > panel_w * 1.6 ? 90 : 45;
        }
    }
    /* Height the rotated labels need: the longest one leaned over, plus the
     * glyph height carried by the lean. */
    double blab_h = labh;
    if (bang > 0) {
        double wmax = 0;
        for (int p = 0; p < npan; p++) {
            const PanelScale *S = &ps[p];
            int n = flip ? S->nybr : S->nxbr;
            char **lb = flip ? S->ylabs : S->xlabs;
            for (int i = 0; i < n; i++) {
                double w = cp_label_w(cr, SZ_AXIS_TEXT, lb[i]);
                if (w > wmax) wmax = w;
            }
        }
        double rad = bang * M_PI / 180.0;
        blab_h = wmax * sin(rad) + labh * cos(rad);
    }
    /* Cheap clipping warnings (no wrapping is attempted): a long title runs
     * off the canvas edge, and leaned-over category labels can eat the panel
     * they label. Both render silently otherwise. */
    if (blab_h > 0.25 * h_pt)
        fprintf(stderr, "cinderplot: warning: the rotated %s tick labels take "
                "%.0f%% of the figure height; consider coord_flip(), shorter "
                "labels, or a taller --size\n", flip ? "y" : "x",
                100 * blab_h / h_pt);
    {
        const char *wide[3] = { spec->lab_title, spec->lab_subtitle, bottom_title };
        const double wsz[3] = { SZ_TITLE, SZ_BASE, SZ_BASE };
        for (int i = 0; i < 3; i++)
            if (wide[i] && text_w(cr, wsz[i], wide[i]) > w_pt - 2 * MARGIN)
                fprintf(stderr, "cinderplot: warning: the %s \"%s\" is wider than "
                        "the %.1fin canvas and will be clipped; shorten it or give "
                        "a wider --size\n", i == 0 ? "title" : i == 1 ? "subtitle"
                        : "x axis title", wide[i], w_pt / 72);
        if (left_title && text_w(cr, SZ_BASE, left_title) > h_pt - 2 * MARGIN)
            fprintf(stderr, "cinderplot: warning: the y axis title \"%s\" is taller "
                    "than the %.1fin canvas and will be clipped; shorten it or give "
                    "a taller --size\n", left_title, h_pt / 72);
    }

    int bfree_l = (flip ? spec->free_y : spec->free_x) && !grid;
    for (int r = 0; r < nrowp; r++) {
        T->rowh[SR(r)] = upt(grid ? (r == 0 ? striph_top : 0) : striph);
        T->rowh[PR(r)] = unull(1);
        T->rowh[PR(r) + 1] = upt(band_h);        /* annotation bands (0 = none) */
        if (r < nrowp - 1)
            T->rowh[PR(r) + 2] = upt(bfree_l ? TICK_LEN + TXT_GAP + blab_h + PANEL_SPACE
                                             : PANEL_SPACE);
    }
    int r_axis = 4 * nrowp + 2;
    int r_last = r_axis + 3 + (fc_leg_h > 0 ? 1 : 0);   /* caption/margin row */
    T->rowh[r_axis]     = upt(TICK_LEN + TXT_GAP + blab_h);
    T->rowh[r_axis + 1] = upt(HALF_LINE / 2);
    T->rowh[r_axis + 2] = upt(baseh);
    if (fc_leg_h > 0) T->rowh[r_axis + 3] = upt(fc_leg_h);
    T->rowh[r_last] = upt(spec->lab_caption ? fmax(MARGIN, font_h(cr, SZ_AXIS_TEXT)) : MARGIN);

    /* ---- static text & legend ---- */
    Grob *g;
    if (spec->lab_title) {
        g = gt_add(T, G_TEXT, 1, PC(0), 1, PC(ncolp - 1));
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = th->title;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    if (spec->lab_subtitle) {
        g = gt_add(T, G_TEXT, 2, PC(0), 2, PC(ncolp - 1));
        g->str = spec->lab_subtitle; g->size = SZ_BASE; g->col = th->title;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    if (spec->lab_caption) {
        g = gt_add(T, G_TEXT, r_last, PC(0), r_last, PC(ncolp - 1));
        g->str = spec->lab_caption; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = 1; g->ty = 1; g->hj = 1; g->va = V_TOP;
    }
    if (th->axis_title_on && !spec->polar) {
        g = gt_add(T, G_TEXT, r_axis + 2, PC(0), r_axis + 2, PC(ncolp - 1));
        g->str = bottom_title; g->size = SZ_BASE; g->col = th->axis_title;
        g->tx = 0.5; g->ty = 1; g->hj = 0.5; g->va = V_TOP;
        g = gt_add(T, G_TEXT, PR(0), 1, PR(nrowp - 1), 1);
        g->str = left_title; g->size = SZ_BASE; g->col = th->axis_title;
        g->tx = 0.5; g->ty = 0.5; g->rot90 = 1;
    }
    if (leg) {
        g = gt_add(T, G_TABLE, SR(0), T->ncol - 2, PR(nrowp - 1), T->ncol - 2);
        g->child = leg;
    }
    for (int p = 0; p < npan && p < 12; p++)
        if (fc_leg[p] && !spec->legend_inside) {
            g = gt_add(T, G_TABLE, r_axis + 3, PC(p % ncolp), r_axis + 3, PC(p % ncolp));
            g->child = fc_leg[p]; g->sub = 1;   /* top-align the row */
        }

    /* panel cell size in points (all panels share it — every panel row/col has
     * null weight 1), for label repel which works in physical units */
    double panelw_pt, panelh_pt;
    {
        double fw = 0, nw = 0, fh = 0, nh = 0;
        for (int c = 0; c < T->ncol; c++)
            if (T->colw[c].k == U_PT) fw += T->colw[c].v; else nw += T->colw[c].v;
        for (int r = 0; r < T->nrow; r++)
            if (T->rowh[r].k == U_PT) fh += T->rowh[r].v; else nh += T->rowh[r].v;
        panelw_pt = nw > 0 ? fmax(0, w_pt - fw) / nw : 0;
        panelh_pt = nh > 0 ? fmax(0, h_pt - fh) / nh : 0;
    }

    /* ---- facet_grid strips ----
     * Column strips run along the top, one per panel column; row strips down
     * the right, rotated -90 so they read top-to-bottom, one per panel row.
     * Each spans its whole column/row rather than sitting over one panel,
     * which is what makes the grid readable: a label is stated once. */
    if (grid) {
        for (int c = 0; colf && c < ncolp; c++) {
            if (th->strip_bg_on) {
                g = gt_add(T, G_RECT, SR(0), PC(c), SR(0), PC(c));
                g->col = th->strip_bg;
            }
            g = gt_add(T, G_TEXT, SR(0), PC(c), SR(0), PC(c));
            g->str = colf->levels[c]; g->size = SZ_AXIS_TEXT; g->col = th->strip_text;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }
        for (int r = 0; rowf && r < nrowp; r++) {
            if (th->strip_bg_on) {
                g = gt_add(T, G_RECT, PR(r), rstrip_c, PR(r), rstrip_c);
                g->col = th->strip_bg;
            }
            g = gt_add(T, G_TEXT, PR(r), rstrip_c, PR(r), rstrip_c);
            g->str = rowf->levels[r]; g->size = SZ_AXIS_TEXT; g->col = th->strip_text;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->rot = -90;
        }
    }

    /* ---- panels ---- */
    for (int p = 0; p < npan; p++) {
        int pr = p / ncolp, pc = p % ncolp;
        int R = PR(pr), C = PC(pc);
        /* Point the range at this panel. NPCX/NPCY read x0..y1, so every
         * coordinate mapping below follows the panel without being told. Under
         * fixed scales these are the values they already had. */
        const PanelScale *S = &ps[p];
        x0 = S->x0; x1 = S->x1; y0 = S->y0; y1 = S->y1;
        xmap = S->xmap; ymap = S->ymap;
        /* free_colour: shadow the palette with this panel's own, so every
         * colour lookup below (points, lines, bars, boxes) follows the
         * panel without being told — the same trick the ranges use above. */
        if (fc_pal) pal = fc_pal + (size_t)p * cf->nlev;

        if (ff && !grid) {
            if (th->strip_bg_on) { g = gt_add(T, G_RECT, SR(pr), C, SR(pr), C); g->col = th->strip_bg; }
            g = gt_add(T, G_TEXT, SR(pr), C, SR(pr), C);
            g->str = ff->levels[p]; g->size = SZ_AXIS_TEXT; g->col = th->strip_text;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }

        int gstart = T->ngrobs;   /* first panel-content grob (all in cell R,C); */
                                  /* under coord_flip these get transposed below */
        if (th->panel_bg_on) { g = gt_add(T, G_RECT, R, C, R, C); g->col = th->panel_bg; }
        if (th->grid_minor_on && !spec->polar) {
            for (int i = 0; i < S->nxmin; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_minor; g->lw = lw_pt(th->grid_minor_lw); g->clip = 1;
                g->x0 = g->x1 = NPCX(S->xmin_br[i]); g->y0 = 0; g->y1 = 1;
            }
            for (int i = 0; i < S->nymin; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_minor; g->lw = lw_pt(th->grid_minor_lw); g->clip = 1;
                g->y0 = g->y1 = NPCY(S->ymin_br[i]); g->x0 = 0; g->x1 = 1;
            }
        }
        if (th->grid_major_on && !spec->polar) {
            for (int i = 0; i < S->nxbr; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_major; g->lw = lw_pt(th->grid_major_lw); g->clip = 1;
                g->x0 = g->x1 = S->xnpc[i]; g->y0 = 0; g->y1 = 1;
            }
            for (int i = 0; i < S->nybr; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_major; g->lw = lw_pt(th->grid_major_lw); g->clip = 1;
                g->y0 = g->y1 = S->ynpc[i]; g->x0 = 0; g->x1 = 1;
            }
        }
        if (th->border_on && !spec->polar) {          /* bw / linedraw / light / few */
            g = gt_add(T, G_RECT, R, C, R, C);
            g->col = th->border; g->stroke = 1; g->lw = lw_pt(th->border_lw);
        }
        if (th->axis_line_on && !spec->polar) {       /* classic / pubr */
            g = gt_add(T, G_LINE, R, C, R, C);        /* bottom */
            g->col = th->axis_line; g->lw = lw_pt(th->axis_line_lw);
            g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = 0;
            g = gt_add(T, G_LINE, R, C, R, C);        /* left */
            g->col = th->axis_line; g->lw = lw_pt(th->axis_line_lw);
            g->y0 = 0; g->y1 = 1; g->x0 = g->x1 = 0;
        }
        /* annotation_logticks: log ticks inside the panel, growing from the
         * bottom (x) / left (y) edge; lengths converted from points to npc */
        if (spec->log_x && panelh_pt > 0)
            for (int i = 0; i < S->xlt_n; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = g->x1 = S->xlt_pos[i]; g->y0 = 0; g->y1 = S->xlt_len[i] / panelh_pt;
            }
        if (spec->log_y && panelw_pt > 0)
            for (int i = 0; i < S->ylt_n; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
                g->y0 = g->y1 = S->ylt_pos[i]; g->x0 = 0; g->x1 = S->ylt_len[i] / panelw_pt;
            }

        /* ---- coord_polar(): the radar chart, drawn whole ----
         * Categories at angles clockwise from 12 o'clock; y maps to radius
         * over the trained (padded) range; dashed rings at the y breaks with
         * their values labelled up the vertical spoke; each colour series a
         * CLOSED polyline. The radius lives in points and divides per axis,
         * so the circle stays a circle whatever the panel's aspect (the
         * circular tree's trick). */
        if (spec->polar) {
            int k = xf->nlev;
            double rmax = fmin(panelw_pt, panelh_pt) / 2 - 2.6 * labh;
            if (rmax < 30) rmax = fmin(panelw_pt, panelh_pt) / 2 * 0.72;
            double cx = 0.5, cy = 0.5;
            const double TAU = 2 * M_PI;
            /* rings */
            for (int i = 0; i < nybr; i++) {
                double rr = (ybr[i] - y0) / (y1 - y0);
                if (rr < 0.03) continue;
                const int NC2 = 73;
                double *px = cp_xmalloc(NC2 * sizeof(double));
                double *py = cp_xmalloc(NC2 * sizeof(double));
                for (int j = 0; j < NC2; j++) {
                    double ang = spec->polar_start + TAU * j / (NC2 - 1);
                    px[j] = cx + rmax * rr * sin(ang) / panelw_pt;
                    py[j] = cy + rmax * rr * cos(ang) / panelh_pt;
                }
                g = gt_add(T, G_POLYLINE, R, C, R, C);
                g->n = NC2; g->px = px; g->py = py;
                g->col = (Col){0.6, 0.6, 0.6};
                g->lw = lw_pt(th->grid_major_lw > 0 ? th->grid_major_lw : 0.5);
                g->dash = 1; g->clip = 1;
                g = gt_add(T, G_TEXT, R, C, R, C);   /* ring value */
                g->str = ylabs[i]; g->size = SZ_AXIS_TEXT * 0.85;
                g->col = (Col){0.45, 0.45, 0.45};
                g->tx = cx - 3.0 / panelw_pt;
                g->ty = cy + (rmax * rr) / panelh_pt;
                g->hj = 1; g->va = V_INKCENTER;
            }
            /* spokes + category labels */
            for (int l = 0; l < k; l++) {
                double ang = spec->polar_start + TAU * l / k;
                double sx2 = sin(ang), cy2 = cos(ang);
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = (Col){0.8, 0.8, 0.8};
                g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
                g->x0 = cx; g->y0 = cy;
                g->x1 = cx + rmax * sx2 / panelw_pt;
                g->y1 = cy + rmax * cy2 / panelh_pt;
                g = gt_add(T, G_TEXT, R, C, R, C);
                g->str = xf->levels[l]; g->size = SZ_AXIS_TEXT;
                g->col = th->axis_text;
                g->tx = cx + rmax * 1.06 * sx2 / panelw_pt;
                g->ty = cy + (rmax * 1.06 * cy2 + labh * 0.55 * cy2) / panelh_pt;
                g->hj = (1 - sx2) / 2;             /* right side left-anchors */
                g->va = V_INKCENTER;
            }
            /* series, in layer order */
            for (int li = 0; li < spec->nlayers; li++) {
                const Layer *L = &spec->layers[li];
                for (int grp = 0; grp < nseries; grp++) {
                    int np = 0;
                    for (int cat = 0; cat < k; cat++)
                        for (int r2 = 0; r2 < df->nrow; r2++)
                            if (use[r2] && xf->idx[r2] == cat
                                && series[r2] == grp) np++;
                    if (!np) continue;
                    double *px = cp_xmalloc((np + 1) * sizeof(double));
                    double *py = cp_xmalloc((np + 1) * sizeof(double));
                    int m2 = 0;
                    for (int cat = 0; cat < k; cat++)
                        for (int r2 = 0; r2 < df->nrow; r2++) {
                            if (!use[r2] || xf->idx[r2] != cat
                                || series[r2] != grp) continue;
                            double ang = spec->polar_start + TAU * cat / k;
                            double rr = (TY(yc->num[r2]) - y0) / (y1 - y0);
                            if (rr < 0) rr = 0;
                            px[m2] = cx + rmax * rr * sin(ang) / panelw_pt;
                            py[m2] = cy + rmax * rr * cos(ang) / panelh_pt;
                            m2++;
                        }
                    Col sc2 = L->has_color ? L->color
                            : cf ? pal[grp / ngf] : C_BLACK;
                    if (L->type == GEOM_LINE && m2 >= 2) {
                        px[m2] = px[0]; py[m2] = py[0];   /* CLOSE the series */
                        g = gt_add(T, G_POLYLINE, R, C, R, C);
                        g->n = m2 + 1; g->px = px; g->py = py;
                        g->col = sc2; g->lw = LAYER_LW(L, 0.5); g->clip = 1;
                        g->alpha = L->alpha; g->dash = L->dash;
                    } else if (L->type == GEOM_POINT) {
                        Col *pc2 = cp_xmalloc(m2 * sizeof(Col));
                        for (int j = 0; j < m2; j++) pc2[j] = sc2;
                        g = gt_add(T, G_POINTS, R, C, R, C);
                        g->n = m2; g->px = px; g->py = py; g->pcol = pc2;
                        g->radius = L->point_size > 0
                                  ? L->point_size * 2.845276 / 2 : PT_RADIUS;
                        g->clip = 1; g->alpha = L->alpha;
                    } else { free(px); free(py); }
                }
            }
        }

        /* layers, in spec order */
        for (int li = 0; !spec->polar && li < spec->nlayers; li++) {
            GeomType gt = spec->layers[li].type;
            /* alpha= and linetype= belong to the whole layer, and every geom
             * builds its grobs differently, so rather than threading them
             * through each branch, note where this layer's grobs start and
             * stamp them all once the branch has run (see end of the loop). */
            int grob0 = T->ngrobs;
            if (gt == GEOM_HISTOGRAM) {
                Hist *hs = &hist[li];
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int b = 0; b < hs->nb[p]; b++) {
                    int cnt = hs->counts[p * hs->nbins + b];
                    if (!cnt) continue;
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : panelfill && panelfill[p] >= 0 ? pal[panelfill[p]]
                           : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(hs->start[p] + b * hs->width[p]);
                    g->x1 = NPCX(hs->start[p] + (b + 1) * hs->width[p]);
                    g->y0 = base;
                    g->y1 = NPCY(cp_logt(spec->log_y, (double)cnt));
                }
            } else if (gt == GEOM_DENSITY) {
                if (cont_col && !spec->layers[li].has_color) {   /* as geom_line */
                    snprintf(err, CP_ERRLEN, "continuous colour= on geom_density() "
                             "is not implemented; use factor()");
                    return -1;
                }
                int di = li2di[li];
                for (int gg = 0; gg < densg; gg++) {
                    size_t bse = ((size_t)((di * npan + p) * densg + gg)) * DENS_N;
                    double *px = cp_xmalloc(DENS_N * sizeof(double));
                    double *py = cp_xmalloc(DENS_N * sizeof(double));
                    for (int j = 0; j < DENS_N; j++) {
                        px[j] = NPCX(dens_x[bse+j]);
                        py[j] = NPCY(dens_y[bse+j]);
                    }
                    g = gt_add(T, G_POLYLINE, R, C, R, C);
                    g->n = DENS_N; g->px = px; g->py = py;
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cf ? pal[gg] : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                }
            } else if (gt == GEOM_COL && colsum) {
                /* stacked by fill group, as geom_bar: values summed per
                 * (category, group), last factor level at the bottom
                 * (ggplot position_stack) */
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                int ng = cf->nlev;
                for (int cat = 0; cat < xf->nlev; cat++) {
                    int slot = xmap ? xmap[cat] : cat;   /* freed x renumbers */
                    if (slot < 0) continue;
                    double xi = slot + 1, cum = 0;
                    for (int grp = ng - 1; grp >= 0; grp--) {
                        double v = colsum[((size_t)(p * xf->nlev + cat)) * ng + grp];
                        if (v <= 0) continue;
                        double top = cum + v;
                        g = gt_add(T, G_RECT, R, C, R, C);
                        g->col = pal[grp];
                        g->sub = 1; g->clip = 1;
                        g->x0 = NPCX(xi - colw / 2); g->x1 = NPCX(xi + colw / 2);
                        g->y0 = cum <= 0 ? base : NPCY(cp_logt(spec->log_y, cum));
                        g->y1 = NPCY(cp_logt(spec->log_y, top));
                        cum = top;
                    }
                }
            } else if (gt == GEOM_COL) {
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    /* rows sharing an x stack: this one starts where the
                     * earlier rows of its bar ended (coloff, 0 for a unique x) */
                    double tx = TXR(r), off = coloff ? coloff[r] : 0;
                    double ya = off == 0 ? base : NPCY(TY(off));
                    double yb = NPCY(TY(off + yc->num[r]));
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cont_col ? CCOL(r)
                           : panelfill && panelfill[p] >= 0 ? pal[panelfill[p]]
                           : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(tx - colw / 2); g->x1 = NPCX(tx + colw / 2);
                    g->y0 = fmin(ya, yb); g->y1 = fmax(ya, yb);
                }
            } else if (gt == GEOM_BAR) {
                /* stat_count bars, width 0.9, stacked by colour group with
                 * the last factor level at the bottom (ggplot position_stack) */
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int cat = 0; cat < xf->nlev; cat++) {
                    int slot = xmap ? xmap[cat] : cat;   /* freed x renumbers */
                    if (slot < 0) continue;
                    double xi = slot + 1, cum = 0;
                    for (int grp = barng - 1; grp >= 0; grp--) {
                        int cnt = barcount[((size_t)(p * xf->nlev + cat)) * barng + grp];
                        if (!cnt) continue;
                        double top = cum + cnt;
                        g = gt_add(T, G_RECT, R, C, R, C);
                        g->col = cf ? pal[grp] : spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                        g->sub = 1; g->clip = 1;
                        g->x0 = NPCX(xi - 0.45); g->x1 = NPCX(xi + 0.45);
                        /* honour scale_y_log10 like geom_histogram/geom_col; the
                         * bottom segment starts at the axis base (log10(0) = -inf) */
                        g->y0 = cum <= 0 ? base : NPCY(cp_logt(spec->log_y, cum));
                        g->y1 = NPCY(cp_logt(spec->log_y, top));
                        cum = top;
                    }
                }
            } else if (gt == GEOM_POINT || gt == GEOM_JITTER) {
                const Layer *JL = &spec->layers[li];
                /* geom_jitter: geom_point with a random offset, so overlapping
                 * observations spread out and density becomes visible. On a
                 * discrete axis every point otherwise stacks on the category
                 * centre and 400 of them look like 40.
                 *
                 * The offset is deterministic. These figures are rebuilt from a
                 * lab-notebook src block, so a plot that moved every render
                 * would be a reproducibility bug, not a nicety -- hence a fixed
                 * default seed, mixed with the layer index so two jitter layers
                 * do not land identically, and seed= to choose another. */
                unsigned jseed = (JL->has_jitter_seed ? JL->jitter_seed : 20260809u)
                               + 1013u * (unsigned)li;
                double jw = 0, jh = 0;
                if (gt == GEOM_JITTER) {
                    /* ggplot2 defaults both to 40% of the data resolution. We
                     * default the VERTICAL to zero instead: y is a measured
                     * value here, and moving it invents data. Ask for height=
                     * to get ggplot's behaviour. */
                    jw = JL->jitter_w > 0 ? JL->jitter_w : 0.4;
                    jh = JL->jitter_h;
                    if (disc_x && jw > 0.5) jw = 0.5;   /* never cross into the next slot */
                }
                int np = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p)) np++;
                double *px = cp_xmalloc(np * sizeof(double)), *py = cp_xmalloc(np * sizeof(double));
                Col *pcol = cp_xmalloc(np * sizeof(Col));
                double *prad = szc ? cp_xmalloc(np * sizeof(double)) : NULL;   /* size aes */
                int *pshp = shf ? cp_xmalloc(np * sizeof(int)) : NULL;         /* shape aes */
                np = 0;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    double jx = TXR(r), jy = TY(yc->num[r]);
                    if (gt == GEOM_JITTER) {
                        /* one small deterministic PRNG, seeded per point so the
                         * offset follows the ROW rather than the draw order --
                         * re-sorting the input then moves nothing */
                        unsigned h = jseed ^ (unsigned)(r * 2654435761u);
                        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
                        h *= 3266489917u; h ^= h >> 16;
                        double u1 = (double)(h & 0xFFFF) / 65535.0;
                        double u2 = (double)((h >> 16) & 0xFFFF) / 65535.0;
                        jx += (u1 * 2 - 1) * jw;
                        jy += (u2 * 2 - 1) * jh;
                    }
                    px[np] = NPCX(jx); py[np] = NPCY(jy);
                    pcol[np] = spec->layers[li].has_color ? spec->layers[li].color
                             : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
                    if (prad) prad[np] = size_to_radius(szc->num[r], szmin, szmax);
                    if (pshp) pshp[np] = shf->idx[r];
                    np++;
                }
                g = gt_add(T, G_POINTS, R, C, R, C);
                g->n = np; g->px = px; g->py = py; g->pcol = pcol; g->pradius = prad;
                g->pshape = pshp;
                g->raster = spec->layers[li].raster;
                g->radius = spec->layers[li].point_size > 0
                          ? PT_RADIUS * spec->layers[li].point_size / 1.5 : PT_RADIUS;
                g->clip = 1;
            } else if (gt == GEOM_SMOOTH) {
                /* LOESS (cp_loess, smooth.c), as ggplot2's default for
                 * n < 1000, fitted in transformed data space and mapped to
                 * the panel afterwards.
                 *
                 * One curve per series (colour x group) per panel, like
                 * geom_line -- a single smooth across groups would average
                 * away the very difference the layer is there to show. */
                const Layer *SL = &spec->layers[li];
                double span = SL->span > 0 ? SL->span : 0.75;
                for (int grp = 0; grp < nseries; grp++) {
                    int nn = 0;
                    for (int r = 0; r < df->nrow; r++)
                        if (use[r] && (!ff || ff->idx[r] == p)
                                   && series[r] == grp) nn++;
                    if (nn < 4) continue;          /* nothing to fit through */
                    double *sx = cp_xmalloc(nn * sizeof(double));
                    double *sy = cp_xmalloc(nn * sizeof(double));
                    nn = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)) continue;
                        if (series[r] != grp) continue;
                        sx[nn] = TXR(r); sy[nn] = TY(yc->num[r]); nn++;
                    }
                    const int NS = 200;            /* output resolution */
                    double *ox = cp_xmalloc(NS * sizeof(double));
                    double *oy = cp_xmalloc(NS * sizeof(double));
                    int npt = cp_loess(sx, sy, nn, span, NS, ox, oy, err);
                    if (npt < 0) return -1;
                    if (npt >= 2) {
                        g = gt_add(T, G_POLYLINE, R, C, R, C);
                        double *lx = cp_xmalloc(npt * sizeof(double));
                        double *ly = cp_xmalloc(npt * sizeof(double));
                        for (int k = 0; k < npt; k++) { lx[k] = NPCX(ox[k]); ly[k] = NPCY(oy[k]); }
                        g->n = npt; g->px = lx; g->py = ly;
                        g->col = SL->has_color ? SL->color : cf ? pal[grp / ngf] : C_BLACK;
                        g->lw = lw_pt(SL->line_lw > 0 ? SL->line_lw : 1.0);
                        g->dash = SL->dash; g->alpha = SL->alpha; g->clip = 1;
                    }
                    free(sx); free(sy); free(ox); free(oy);
                }
            } else if (gt == GEOM_LINE) {
                /* A continuous colour has no group to draw one line per, and
                 * a black line beside a colourbar claims a mapping it does
                 * not have; a layer colour= overrides the mapping and is fine. */
                if (cont_col && !spec->layers[li].has_color) {
                    snprintf(err, CP_ERRLEN, "continuous colour= on geom_line() "
                             "is not implemented; use factor()");
                    return -1;
                }
                for (int grp = 0; grp < nseries; grp++) {
                    int np = 0;
                    for (int r = 0; r < df->nrow; r++)
                        if (use[r] && (!ff || ff->idx[r] == p)
                                   && series[r] == grp) np++;
                    if (np < 2) continue;
                    Pt *pts = cp_xmalloc(np * sizeof(Pt));
                    np = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)
                                    || series[r] != grp) continue;
                        pts[np].x = NPCX(TXR(r));
                        pts[np].y = NPCY(TY(yc->num[r]));
                        np++;
                    }
                    qsort(pts, np, sizeof(Pt), cmp_pt_x);
                    double *px = cp_xmalloc(np * sizeof(double)), *py = cp_xmalloc(np * sizeof(double));
                    for (int i = 0; i < np; i++) { px[i] = pts[i].x; py[i] = pts[i].y; }
                    free(pts);
                    g = gt_add(T, G_POLYLINE, R, C, R, C);
                    g->n = np; g->px = px; g->py = py;
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cf ? pal[grp / ngf] : C_BLACK;
                    g->lw = LAYER_LW(&spec->layers[li], 0.5); g->clip = 1;
                }
            } else if (gt == GEOM_ERRORBAR || gt == GEOM_LINERANGE) {
                /* a vertical range per row, ymin..ymax at x; errorbar adds
                 * flat caps of width= (x-axis units in transformed space,
                 * as ggplot applies it), default 0.9 x the x resolution.
                 * The bars follow a colour= mapping but not a fill= one
                 * (ggplot draws those black): painted in the fill palette
                 * the half of each whisker inside its bar disappears. */
                double wda = spec->layers[li].eb_width > 0
                           ? spec->layers[li].eb_width : colw;
                int mapped = spec->colour.col && !spec->colour.is_fill;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (isnan(yminc->num[r]) || isnan(yec->num[r])) continue;
                    double tx = TXR(r);
                    double ylo = NPCY(TY(yminc->num[r]));
                    double yhi = NPCY(TY(yec->num[r]));
                    Col ec = spec->layers[li].has_color ? spec->layers[li].color
                           : !mapped ? C_BLACK
                           : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
                    double elw = LAYER_LW(&spec->layers[li], 0.5);
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = ec; g->lw = elw; g->clip = 1;
                    g->x0 = g->x1 = NPCX(tx);
                    g->y0 = ylo; g->y1 = yhi;
                    if (gt == GEOM_ERRORBAR) {
                        for (int e2 = 0; e2 < 2; e2++) {
                            g = gt_add(T, G_LINE, R, C, R, C);
                            g->col = ec; g->lw = elw; g->clip = 1;
                            g->x0 = NPCX(tx - wda / 2);
                            g->x1 = NPCX(tx + wda / 2);
                            g->y0 = g->y1 = e2 ? yhi : ylo;
                        }
                    }
                }
            } else if (gt == GEOM_SEGMENT && spec->layers[li].data) {
                /* per-layer data (e.g. CBS segments): its own file, genome-
                 * offset horizontal lines from start..end at y */
                const Layer *L = &spec->layers[li];
                if (!layer_df[li] && !(layer_df[li] = df_read_csv(L->data, err)))
                    return -1;
                DataFrame *d2 = layer_df[li];
                const Column *c_chr = genome_x ? df_col(d2, spec->chrom.col) : NULL;
                const Column *c_x = df_col(d2, spec->x.col);
                const Column *c_xe = spec->xend.col ? df_col(d2, spec->xend.col) : NULL;
                const Column *c_y = df_col(d2, L->ycol ? L->ycol : spec->y.col);
                const Column *c_ye2 = spec->yend.col ? df_col(d2, spec->yend.col) : NULL;
                if (c_ye2 && c_ye2->type != COL_NUM) c_ye2 = NULL;
                if (!c_x || !c_y || (genome_x && !c_chr)) {
                    snprintf(err, CP_ERRLEN, "geom_segment(data=%s): missing chrom/x/y column", L->data);
                    return -1;
                }
                Col lcol = L->has_color ? L->color : C_BLACK;
                for (int r2 = 0; r2 < d2->nrow; r2++) {
                    double off = genome_x ? genome_off(gs, c_chr->str[r2]) : 0;
                    if (genome_x && off < 0) continue;
                    if (isnan(c_x->num[r2]) || isnan(c_y->num[r2])) continue;
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = lcol; g->lw = LAYER_LW(L, 0.6); g->clip = 1;
                    /* through the log transform like geom_rect(data=); a
                     * 10..100 segment used to land off a log10 panel */
                    g->x0 = NPCX(genome_x ? off + c_x->num[r2]
                                 : cp_logt(spec->log_x, c_x->num[r2]));
                    g->x1 = NPCX(genome_x ? off + (c_xe ? c_xe->num[r2] : c_x->num[r2])
                                 : cp_logt(spec->log_x, c_xe ? c_xe->num[r2] : c_x->num[r2]));
                    g->y0 = NPCY(TY(c_y->num[r2]));
                    /* with yend resolvable in the file the segment is
                     * vertical/diagonal; the horizontal CBS default stays */
                    g->y1 = c_ye2 && !isnan(c_ye2->num[r2])
                          ? NPCY(TY(c_ye2->num[r2])) : g->y0;
                }
            } else if (gt == GEOM_SEGMENT) {
                /* one line per row: (x,y) -> (xend, yend); yend defaults to
                 * y. A layer y= names a different START column — it used to
                 * parse and silently do nothing without data= */
                const Column *syc = yc;
                if (spec->layers[li].ycol) {
                    syc = df_col(df, spec->layers[li].ycol);
                    if (!syc || syc->type != COL_NUM) {
                        snprintf(err, CP_ERRLEN, "geom_segment(y=%s): %s",
                                 spec->layers[li].ycol,
                                 syc ? "must be numeric" : "column not found");
                        return -1;
                    }
                }
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (xec && isnan(xec->num[r])) continue;
                    if (isnan(syc->num[r])) continue;
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
                    g->lw = LAYER_LW(&spec->layers[li], 0.5); g->clip = 1;
                    g->x0 = NPCX(TXR(r));
                    g->x1 = NPCX(xec ? (genome_x ? GX(r, xec->num[r])
                                      : cp_logt(spec->log_x, xec->num[r]))
                                     : TXR(r));
                    g->y0 = NPCY(TY(syc->num[r]));
                    g->y1 = NPCY(yec ? TY(yec->num[r]) : TY(yc->num[r]));
                }
            } else if (gt == GEOM_TILE) {
                /* One filled cell per row, centred on (x, y). Cell size is 1 on
                 * a discrete axis, where categories sit at 1..k; on a continuous
                 * axis it is the smallest gap between distinct values, which is
                 * what a pre-binned grid (geom_raster) wants and degrades
                 * sensibly for an irregular one. */
                double wx = tile_step(df, use, ff, p, disc_x, 1, xc, spec);
                double wy = tile_step(df, use, ff, p, disc_y, 0, yc, spec);
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    double cx = TXR(r), cy = disc_y ? YVAL(r) : TY(yc->num[r]);
                    if (isnan(cx) || isnan(cy)) continue;
                    /* the body takes the MAPPED fill; a layer colour= is the
                     * BORDER, as in ggplot2 -- it used to take over the fill,
                     * so geom_tile(colour="white") blanked every cell */
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(cx - wx / 2); g->x1 = NPCX(cx + wx / 2);
                    g->y0 = NPCY(cy - wy / 2); g->y1 = NPCY(cy + wy / 2);
                    if (spec->layers[li].has_color) {
                        double bw2 = spec->layers[li].tile_lw > 0
                                   ? spec->layers[li].tile_lw : 0.1;
                        g = gt_add(T, G_RECT, R, C, R, C);
                        g->col = spec->layers[li].color;
                        g->sub = 1; g->stroke = 1; g->lw = lw_pt(bw2); g->clip = 1;
                        g->x0 = NPCX(cx - wx / 2); g->x1 = NPCX(cx + wx / 2);
                        g->y0 = NPCY(cy - wy / 2); g->y1 = NPCY(cy + wy / 2);
                    }
                }
            } else if (gt == GEOM_RECT && spec->layers[li].data) {
                /* A rect layer with its own file is one of two figures. With y
                 * and yend resolvable in that file it is a 4-corner rect per
                 * row, exactly as for the main data; without them it is a
                 * region-highlight band spanning the panel height (the genome
                 * shading this branch was built for). The band reading used
                 * to be the ONLY one, so a 4-corner layer file silently drew
                 * grey full-height bands and any fill mapping vanished. */
                const Layer *L = &spec->layers[li];
                if (!layer_df[li] && !(layer_df[li] = df_read_csv(L->data, err)))
                    return -1;
                DataFrame *d2 = layer_df[li];
                const Column *c_chr = genome_x ? df_col(d2, spec->chrom.col) : NULL;
                const Column *c_x = df_col(d2, spec->x.col);
                const Column *c_xe = spec->xend.col ? df_col(d2, spec->xend.col) : NULL;
                if (!c_x || !c_xe || (genome_x && !c_chr)) {
                    snprintf(err, CP_ERRLEN, "geom_rect(data=%s): needs chrom/xmin/xmax columns", L->data);
                    return -1;
                }
                const Column *c_y = spec->y.col ? df_col(d2, spec->y.col) : NULL;
                const Column *c_ye = spec->yend.col ? df_col(d2, spec->yend.col) : NULL;
                if (c_y && c_y->type != COL_NUM) c_y = NULL;
                if (c_ye && c_ye->type != COL_NUM) c_ye = NULL;
                int corner4 = c_y && c_ye;
                /* colour: the layer's own colour= wins; else a mapped
                 * colour/fill aes reads THIS file's column through the same
                 * scale and domain as the main data, so the one legend stays
                 * truthful for both layers. A mapped aes whose column is
                 * missing here is an error, not a silent grey. */
                const Column *c_col = NULL;
                if (!L->has_color && spec->colour.col && (cf || cont_col)) {
                    c_col = df_col(d2, spec->colour.col);
                    if (!c_col) {
                        snprintf(err, CP_ERRLEN, "geom_rect(data=%s): colour/fill "
                                 "column `%s` not in this file", L->data, spec->colour.col);
                        return -1;
                    }
                    if (cont_col && c_col->type != COL_NUM) {
                        snprintf(err, CP_ERRLEN, "geom_rect(data=%s): colour/fill "
                                 "column `%s` must be numeric to share the "
                                 "continuous scale", L->data, spec->colour.col);
                        return -1;
                    }
                    if (cf && c_col->type != COL_STR) {
                        snprintf(err, CP_ERRLEN, "geom_rect(data=%s): colour/fill "
                                 "column `%s` must be text to match the discrete "
                                 "levels of the main data", L->data, spec->colour.col);
                        return -1;
                    }
                }
                Col fixed = L->has_color ? L->color
                          : corner4 ? C_BAR : (Col){0.85, 0.85, 0.85};
                for (int r2 = 0; r2 < d2->nrow; r2++) {
                    double off = genome_x ? genome_off(gs, c_chr->str[r2]) : 0;
                    if ((genome_x && off < 0) || isnan(c_x->num[r2]) || isnan(c_xe->num[r2])) continue;
                    Col col = fixed;
                    if (c_col) {
                        if (cont_col) {
                            col = isnan(c_col->num[r2]) ? C_NA
                                : fill_map_value(&cscale, c_col->num[r2], cdmin, cdmax);
                        } else {
                            int lev = -1;
                            for (int k = 0; k < cf->nlev; k++)
                                if (!strcmp(c_col->str[r2], cf->levels[k])) { lev = k; break; }
                            if (lev < 0) {
                                snprintf(err, CP_ERRLEN, "geom_rect(data=%s): value "
                                         "\"%s\" in `%s` is not a level of the main "
                                         "data's `%s`", L->data, c_col->str[r2],
                                         spec->colour.col, spec->colour.col);
                                return -1;
                            }
                            col = pal[lev];
                        }
                    }
                    if (corner4 && (isnan(c_y->num[r2]) || isnan(c_ye->num[r2])))
                        continue;
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = col; g->sub = 1; g->clip = 1;
                    double ax = genome_x ? off + c_x->num[r2]
                              : cp_logt(spec->log_x, c_x->num[r2]);
                    double bx = genome_x ? off + c_xe->num[r2]
                              : cp_logt(spec->log_x, c_xe->num[r2]);
                    g->x0 = NPCX(fmin(ax, bx)); g->x1 = NPCX(fmax(ax, bx));
                    if (corner4) {
                        double ay = NPCY(TY(c_y->num[r2])), by = NPCY(TY(c_ye->num[r2]));
                        g->y0 = fmin(ay, by); g->y1 = fmax(ay, by);
                    } else {
                        g->y0 = 0; g->y1 = 1;    /* full panel height */
                    }
                }
            } else if (gt == GEOM_RECT) {
                /* filled rectangle per row: (xmin,ymin) .. (xmax,ymax); a
                 * layer fill= overrides a mapped one, as geom_point's does */
                Col fixed = spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (isnan(xec->num[r]) || isnan(yec->num[r])) continue;
                    double a = NPCX(TXR(r));
                    double b = NPCX(genome_x ? GX(r, xec->num[r])
                                  : cp_logt(spec->log_x, xec->num[r]));
                    double c0 = NPCY(TY(yc->num[r])), d = NPCY(TY(yec->num[r]));
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? fixed
                           : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : fixed;
                    g->sub = 1; g->clip = 1;
                    g->x0 = fmin(a, b); g->x1 = fmax(a, b);
                    g->y0 = fmin(c0, d); g->y1 = fmax(c0, d);
                }
            } else if (gt == GEOM_BOXPLOT) {
                /* five-number summary + Tukey whiskers + outliers, in
                 * transformed-y space; position_dodge2 when box_dodge */
                if (cont_col && !spec->layers[li].has_color) {   /* as geom_line */
                    snprintf(err, CP_ERRLEN, "continuous colour= on geom_boxplot() "
                             "is not implemented; use factor()");
                    return -1;
                }
                const double WFULL = 0.75;               /* undodged box width */
                int *present = cp_xmalloc(box_slots * sizeof(int));
                for (int cat = 0; cat < xf->nlev; cat++) {
                    int slot = xmap ? xmap[cat] : cat;   /* freed x renumbers */
                    if (slot < 0) continue;
                    /* dodge2 with preserve="total": the groups this category
                     * actually holds share its full width, so a category
                     * with fewer groups is not squeezed off-centre into the
                     * slots of the absent ones */
                    int ns = 0;
                    for (int s = 0; s < box_slots; s++) {
                        int any = 0;
                        for (int r = 0; r < df->nrow && !any; r++)
                            if (use[r] && (!ff || ff->idx[r] == p) && xf->idx[r] == cat
                                && (box_slots == 1 || cf->idx[r] == s)) any = 1;
                        if (any) present[ns++] = s;
                    }
                    double slotw = WFULL / (ns ? ns : 1);
                    double boxw = box_slots > 1 ? slotw * 0.9 : WFULL;  /* padding 0.1 */
                    for (int si = 0; si < ns; si++) {
                        int s = present[si];
                        int ny = 0, anyg = -1;
                        for (int r = 0; r < df->nrow; r++)
                            if (use[r] && (!ff || ff->idx[r] == p) && xf->idx[r] == cat
                                && (box_slots == 1 || cf->idx[r] == s)) ny++;
                        if (ny == 0) continue;
                        double *ys = cp_xmalloc(ny * sizeof(double));
                        ny = 0;
                        for (int r = 0; r < df->nrow; r++)
                            if (use[r] && (!ff || ff->idx[r] == p) && xf->idx[r] == cat
                                && (box_slots == 1 || cf->idx[r] == s)) {
                                ys[ny++] = TY(yc->num[r]);
                                anyg = cf ? cf->idx[r] : -1;
                            }
                        qsort(ys, ny, sizeof(double), cmp_double);
                        BoxStat b; box_stats(ys, ny, &b);
                        double center = (slot + 1) - WFULL / 2 + slotw * (si + 0.5);
                        double xl = NPCX(center - boxw / 2), xr = NPCX(center + boxw / 2);
                        double xm = NPCX(center);
                        /* aes(fill=) colours the box BODY and keeps ggplot's
                         * dark chrome (outline, whiskers, median, outliers);
                         * aes(colour=) colours the chrome over a white body.
                         * One shared aes carries both spellings, so the
                         * recorded spelling decides — writing fill= used to
                         * silently render the colour= look. A layer constant
                         * follows the same rule by its own spelling, and
                         * overrides the mapping on its side only. */
                        const Layer *BL = &spec->layers[li];
                        Col grpc = cf ? pal[box_slots > 1 ? s : anyg] : C_TICK;
                        int fillbox = cf && spec->colour.is_fill;
                        Col lc = fillbox ? C_TICK : grpc;
                        Col body = fillbox ? grpc : C_WHITE;
                        if (BL->has_color) {
                            if (BL->color_is_fill) body = BL->color;
                            else lc = BL->color;
                        }

                        for (int w = 0; w < 2; w++) {    /* whiskers */
                            g = gt_add(T, G_LINE, R, C, R, C);
                            g->col = lc; g->lw = lw_pt(0.5); g->clip = 1;
                            g->x0 = g->x1 = xm;
                            g->y0 = NPCY(w ? b.q1 : b.q3); g->y1 = NPCY(w ? b.wlo : b.whi);
                        }
                        g = gt_add(T, G_RECT, R, C, R, C);   /* box body */
                        g->col = body; g->sub = 1; g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = NPCY(b.q1); g->y1 = NPCY(b.q3);
                        g = gt_add(T, G_RECT, R, C, R, C);   /* box outline */
                        g->col = lc; g->sub = 1; g->stroke = 1; g->lw = lw_pt(0.5); g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = NPCY(b.q1); g->y1 = NPCY(b.q3);
                        g = gt_add(T, G_LINE, R, C, R, C);   /* median (fatten 2) */
                        g->col = lc; g->lw = lw_pt(1.0); g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = g->y1 = NPCY(b.med);

                        int nout = 0;
                        if (!spec->layers[li].no_outliers)
                            for (int i = 0; i < ny; i++)
                                if (ys[i] > b.whi || ys[i] < b.wlo) nout++;
                        if (nout) {
                            double *ox = cp_xmalloc(nout * sizeof(double)), *oy = cp_xmalloc(nout * sizeof(double));
                            Col *oc = cp_xmalloc(nout * sizeof(Col));
                            nout = 0;
                            for (int i = 0; i < ny; i++)
                                if (ys[i] > b.whi || ys[i] < b.wlo) {
                                    ox[nout] = xm; oy[nout] = NPCY(ys[i]); oc[nout] = lc; nout++;
                                }
                            g = gt_add(T, G_POINTS, R, C, R, C);
                            g->n = nout; g->px = ox; g->py = oy; g->pcol = oc;
                            g->radius = PT_RADIUS; g->clip = 1;
                        }
                        free(ys);
                    }
                }
                free(present);
            } else if (gt == GEOM_HLINE) {
                const Layer *L = &spec->layers[li];
                /* a reference value <= 0 has no place on a log axis; ggplot
                 * drops such a line rather than drawing at log10(<=0) = NaN */
                double yt = NPCY(TY(L->intercept));
                if (L->has_intercept && isfinite(yt)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = LAYER_LW(L, 0.5); g->clip = 1;
                    g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = yt;
                }
            } else if (gt == GEOM_VLINE) {
                const Layer *L = &spec->layers[li];
                double xt = cp_logt(spec->log_x, L->intercept);
                double xn = NPCX(xt);
                if (L->has_intercept && isfinite(xn)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = LAYER_LW(L, 0.5); g->clip = 1;
                    g->y0 = 0; g->y1 = 1; g->x0 = g->x1 = xn;
                }
            } else if (gt == GEOM_ABLINE) {
                const Layer *L = &spec->layers[li];
                /* in TRANSFORMED space, as ggplot draws it: straight on the
                 * page, base-agnostic, and finite at both panel edges (see
                 * train_ref_lines) */
                double yl = NPCY(L->intercept + L->slope * x0);
                double yr = NPCY(L->intercept + L->slope * x1);
                if (isfinite(yl) && isfinite(yr)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = LAYER_LW(L, 0.5); g->clip = 1;
                    g->x0 = 0; g->x1 = 1;
                    g->y0 = yl; g->y1 = yr;
                }
            } else if (gt == GEOM_TEXT || gt == GEOM_LABEL) {
                const Layer *L = &spec->layers[li];
                double fs = (L->txt_size > 0 ? L->txt_size : 3.88) * 2.845276; /* mm -> pt */
                double ndx = x1 > x0 ? L->nudge_x / (x1 - x0) : 0;   /* data -> npc */
                double ndy = y1 > y0 ? L->nudge_y / (y1 - y0) : 0;
                /* y goes through the same discrete mapping the tile and
                 * point layers use: with a string y there is no num[] to
                 * read at all, and with factor(y) the label belongs at the
                 * factor slot, not the raw value. */
#define TYR(r) (disc_y ? YVAL(r) : TY(yc->num[r]))
                int cap = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p)
                        && !isnan(TXR(r)) && !isnan(TYR(r))) cap++;
                if (cap > 0) {
                    RLabel *rl = cp_xmalloc(cap * sizeof(RLabel));
                    const char **strs = cp_xmalloc(cap * sizeof(char *));
                    Col *cols = cp_xmalloc(cap * sizeof(Col));
                    double *px = cp_xmalloc(cap * sizeof(double)), *py = cp_xmalloc(cap * sizeof(double));
                    double bpad = (gt == GEOM_LABEL ? fs * 0.25 : 0) + PT_RADIUS * 0.6;
                    int m = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)) continue;
                        if (isnan(TXR(r)) || isnan(TYR(r))) continue;
                        const char *s;
                        if (labc->type == COL_STR) s = labc->str[r];
                        else if (isnan(labc->num[r])) continue;   /* NA: no label */
                        else { char *tmp = cp_xmalloc(32); fmt_num(labc->num[r], tmp, 32); s = tmp; }
                        /* Blank and NA labels draw nothing at all -- no glyph and,
                         * for the repel geoms, no leader line. ggplot2/ggrepel drop
                         * them, and it is the only way to label a subset of points:
                         * otherwise a volcano labelling its top 8 of 230 hits gets
                         * ~220 leader lines radiating to empty strings. */
                        if (!s || !*s || !strcmp(s, "NA")) continue;
                        double axp = NPCX(TXR(r)) * panelw_pt, ayp = NPCY(TYR(r)) * panelh_pt;
                        px[m] = axp; py[m] = ayp;
                        rl[m].hw = text_w(cr, fs, s) / 2 + bpad;
                        rl[m].hh = font_h(cr, fs) / 2 + bpad;
                        rl[m].ax = axp + ndx * panelw_pt; rl[m].ay = ayp + ndy * panelh_pt;
                        if (L->repel) {         /* scatter starts (golden angle) to break jams */
                            double th = m * 2.3999632, rad = rl[m].hh + 3;
                            rl[m].cx = rl[m].ax + rad * cos(th);
                            rl[m].cy = rl[m].ay + rad * sin(th);
                        } else { rl[m].cx = rl[m].ax; rl[m].cy = rl[m].ay; }
                        strs[m] = s;
                        /* text takes the colour= mapping, never fill=: a
                         * fill-mapped label on its own tile is invisible
                         * (same colour as the tile). ggplot2 draws it black. */
                        cols[m] = L->has_color ? L->color
                                : spec->colour.is_fill ? C_BLACK
                                : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
                        m++;
                    }
                    if (L->repel && panelw_pt > 0 && panelh_pt > 0)
                        repel_labels(rl, m, px, py, m, panelw_pt, panelh_pt, 2.0);
                    if (L->repel)                         /* connectors, drawn under the text */
                        for (int i = 0; i < m; i++) {
                            double ex = rl[i].cx - px[i], ey = rl[i].cy - py[i];
                            if (fabs(ex) <= rl[i].hw && fabs(ey) <= rl[i].hh) continue;
                            double fx = fabs(ex) > 1e-6 ? rl[i].hw / fabs(ex) : 1e9;
                            double fy = fabs(ey) > 1e-6 ? rl[i].hh / fabs(ey) : 1e9;
                            double f = fmin(fmin(fx, fy), 1.0);
                            g = gt_add(T, G_LINE, R, C, R, C);
                            g->col = (Col){0.6, 0.6, 0.6}; g->lw = lw_pt(0.3); g->clip = 1;
                            g->x0 = px[i] / panelw_pt; g->y0 = py[i] / panelh_pt;
                            g->x1 = (rl[i].cx - ex * f) / panelw_pt;
                            g->y1 = (rl[i].cy - ey * f) / panelh_pt;
                        }
                    for (int i = 0; i < m; i++) {
                        g = gt_add(T, G_TEXT, R, C, R, C);
                        g->str = strs[i]; g->size = fs; g->clip = 1; g->col = cols[i];
                        g->tx = panelw_pt > 0 ? rl[i].cx / panelw_pt : NPCX(0);
                        g->ty = panelh_pt > 0 ? rl[i].cy / panelh_pt : NPCY(0);
                        g->hj = L->has_txt_hjust ? L->txt_hjust : 0.5;
                        g->va = V_INKCENTER;
                        g->rot = L->txt_angle;        /* 0 = the usual path */
                        if (gt == GEOM_LABEL) {
                            g->text_box = 1; g->box_fill = C_WHITE; g->box_line = cols[i];
                        }
                    }
                    free(rl); free(strs); free(cols); free(px); free(py);
                }
#undef TYR
            }

            /* Stamp this layer's alpha= / linetype= onto every grob it just
             * produced. Done once here rather than in each geom branch, so a
             * new geom picks both up for free. Theme and axis grobs are built
             * outside this loop and are untouched. */
            if (spec->layers[li].alpha > 0 || spec->layers[li].dash) {
                for (int gi = grob0; gi < T->ngrobs; gi++) {
                    T->grobs[gi].alpha = spec->layers[li].alpha;
                    T->grobs[gi].dash = spec->layers[li].dash;
                }
            }
        }

        /* ideogram track: cytoband rects in the reserved bottom band */
        if (ideo_npc > 0) {
            DataFrame *cb = df_read_csv(spec->ideogram_path, err);
            if (!cb) return -1;
            const Column *bc = df_col(cb, "chrom"), *bs = df_col(cb, "start"),
                         *be = df_col(cb, "end"), *bt = df_col(cb, "stain");
            if (!bc || !bs || !be || !bt) {
                snprintf(err, CP_ERRLEN, "ideogram cytoband needs chrom,start,end,stain columns"); return -1;
            }
            double yb0 = 0.010, yb1 = ideo_npc - 0.010;   /* npc band at panel bottom */
            for (int r2 = 0; r2 < cb->nrow; r2++) {
                double off = genome_off(gs, bc->str[r2]);
                if (off < 0) continue;
                g = gt_add(T, G_RECT, R, C, R, C);
                g->col = stain_color(bt->str[r2]); g->sub = 1; g->clip = 1;
                g->x0 = NPCX(off + bs->num[r2]); g->x1 = NPCX(off + be->num[r2]);
                g->y0 = yb0; g->y1 = yb1;
            }
        }

        /* annotate(): one-off marks at literal data coords, drawn over the
         * geoms in every panel. Coordinates go through the panel's own
         * scales, and the coord_flip transpose below catches these grobs
         * like any other panel content -- they must therefore be emitted
         * BEFORE it; they used to follow it and stayed unflipped. */
        for (int a2 = 0; a2 < spec->nannos; a2++) {
            const Annotate *an = &spec->annos[a2];
            double ax = NPCX(genome_x ? an->x : cp_logt(spec->log_x, an->x));
            double ay = NPCY(TY(an->y));
            if (an->kind == ANNO_TEXT) {
                g = gt_add(T, G_TEXT, R, C, R, C);
                g->str = an->label;
                g->size = an->size > 0 ? an->size : SZ_AXIS_TEXT;
                g->col = an->has_color ? an->color : C_BLACK;
                g->tx = ax; g->ty = ay;
                g->hj = an->has_hjust ? an->hjust : 0.5;
                g->rot = an->angle;                   /* 0 = the usual path */
                g->va = !an->has_vjust ? V_INKCENTER
                      : an->vjust == 0 ? V_BOTTOM     /* text sits above y */
                      : an->vjust == 1 ? V_TOP        /* text hangs below y */
                      : V_INKCENTER;
            } else if (an->kind == ANNO_SEGMENT) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = an->has_color ? an->color : C_BLACK;
                g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = ax; g->y0 = ay;
                g->x1 = NPCX(genome_x ? an->xend : cp_logt(spec->log_x, an->xend));
                g->y1 = NPCY(TY(an->yend));
            } else {                       /* ANNO_RECT */
                Col grey = {0.85, 0.85, 0.85};
                g = gt_add(T, G_RECT, R, C, R, C);
                g->col = an->has_color ? an->color : grey;
                g->sub = 1; g->clip = 1;
                double bx = NPCX(genome_x ? an->xend : cp_logt(spec->log_x, an->xend));
                double by = NPCY(TY(an->yend));
                g->x0 = fmin(ax, bx); g->x1 = fmax(ax, bx);
                g->y0 = fmin(ay, by); g->y1 = fmax(ay, by);
            }
        }

        /* coord_flip: transpose every panel-content grob (x <-> y npc). The
         * gridlines therefore align with the re-pointed left/bottom axes. */
        if (flip)
            for (int gi = gstart; gi < T->ngrobs; gi++) flip_grob(&T->grobs[gi]);

        /* Left axis. Shared scales label the left column only, because every
         * panel in a row carries the same one; a freed axis differs per panel,
         * so each gets its own, drawn in the spacer to its left. */
        int lfree = (flip ? spec->free_x : spec->free_y) && !grid;
        if (!spec->polar && (pc == 0 || lfree)) {
            g = gt_add(T, G_AXIS_Y, R, pc == 0 ? 3 : PC(pc) - 1, R, pc == 0 ? 3 : PC(pc) - 1);
            g->n = flip ? S->nxbr : S->nybr;
            g->py = flip ? S->xnpc : S->ynpc;
            g->labels = flip ? S->xlabs : S->ylabs;
            g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
            g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
        }
        /* annotation() bands under this panel: one strip per call, stacked
         * top-to-bottom, each cell a category-wide chip (bar width 0.9, so
         * chips align under stacked geom_col bars). x positions go through
         * the panel's own scale, so bands stay aligned under free_x too. */
        for (int a = 0; a < nann; a++) {
            double ytop = 1 - (ANN_PAD + a * (ANN_STRIP + ANN_GAP)) / band_h;
            double ybot = ytop - ANN_STRIP / band_h;
            for (int l = 0; l < xf->nlev; l++) {
                int slot = spec->free_x ? S->xmap[l] : l;
                if (slot < 0) continue;          /* level absent from this panel */
                double xi = slot + 1;
                int lev = anns[a].catlev[l];
                g = gt_add(T, G_RECT, R + 1, C, R + 1, C);
                g->sub = 1; g->clip = 1;
                g->col = lev >= 0 ? anns[a].apal[lev] : C_NA;
                g->x0 = NPCX(xi - 0.45); g->x1 = NPCX(xi + 0.45);
                g->y0 = ybot; g->y1 = ytop;
            }
        }

        /* Bottom axis. Shared scales draw one per column, under the lowest panel
         * of that column (below); a freed axis is per panel. */
        int bfree = (flip ? spec->free_y : spec->free_x) && !grid;
        if (bfree && !spec->polar) {
            int rb = (npan - 1 - pc) / ncolp;
            int arow = (pr == rb && rb == nrowp - 1) ? r_axis : PR(pr) + 2;
            g = gt_add(T, G_AXIS_X, arow, C, arow, C);
            g->n = flip ? S->nybr : S->nxbr;
            g->px = flip ? S->ynpc : S->xnpc;
            g->labels = flip ? S->ylabs : S->xlabs;
            g->label_angle = bang;
            g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
            g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
        }
    }

    /* x axes: under the bottom-most panel of each column (bottom axis: x, or y
     * under flip). Genome mode (never flipped) keeps its chrom-name axis. */
    int bfree_col = (flip ? spec->free_y : spec->free_x) && grid;
    for (int c = 0; c < ncolp && c < npan && !spec->polar
             && (grid || !(flip ? spec->free_y : spec->free_x)); c++) {
        int rb = (npan - 1 - c) / ncolp;
        if (rb == nrowp - 1)
            g = gt_add(T, G_AXIS_X, r_axis, PC(c), r_axis, PC(c));
        else
            g = gt_add(T, G_AXIS_X, PR(rb) + 2, PC(c), PR(rb + 1), PC(c));
        if (genome_x) { g->n = gax_n; g->px = gax_pos; g->labels = gax_lab; }
        else if (bfree_col) {
            /* facet_grid(scales="free_x"): the column's own breaks, taken from
             * its bottom panel -- every panel above shares them */
            const PanelScale *S = &ps[rb * ncolp + c];
            g->n = flip ? S->nybr : S->nxbr;
            g->px = flip ? S->ynpc : S->xnpc;
            g->labels = flip ? S->ylabs : S->xlabs;
            g->label_angle = bang;
        } else {
            g->n = bax_n; g->px = bax_pos; g->labels = bax_lab;   /* log ticks drawn inside the panel */
            g->label_angle = bang;
        }
        g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
        g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
    }

    /* inside legends go in LAST, over the panel content they sit on */
    if (spec->legend_inside) {
        for (int p = 0; p < npan && p < 12; p++)
            if (fc_leg[p]) {
                g = gt_add(T, G_TABLE, PR(p / ncolp), PC(p % ncolp),
                           PR(p / ncolp), PC(p % ncolp));
                g->child = fc_leg[p]; g->n = 1;   /* anchored in the panel */
                g->tx = spec->leg_ix; g->ty = spec->leg_iy;
            }
        if (inside_leg) {
            g = gt_add(T, G_TABLE, PR(0), PC(0), PR(0), PC(0));
            g->child = inside_leg; g->n = 1;
            g->tx = spec->leg_ix; g->ty = spec->leg_iy;
        }
    }

    /* ---- go ---- */
    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_status_t st = cairo_status(cr);   /* a label cairo rejected (not UTF-8) poisons cr;
                                             * every later call was a no-op and the figure blank */
    cairo_destroy(cr);
    if (st == CAIRO_STATUS_SUCCESS) st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        snprintf(err, CP_ERRLEN, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
