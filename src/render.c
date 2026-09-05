/* render.c — spec + data -> measured gtable -> cairo PDF.
 *
 * Scales are continuous with an optional log10 transform: all layout,
 * binning and breaks happen in TRANSFORMED space; tick labels show data-
 * space values (ggplot semantics). Layers render in spec order.
 *
 * Facet layout follows ggplot2's facet_wrap (dims = rev(n2mfrow(n)),
 * shared scales, strips above panels, staircase axes). */
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

static GTable *build_legend(cairo_t *cr, const Theme *th, const char *title, const Factor *f,
                            const Col *pal, int haspoint, int hasline, int hasbox, int hastext,
                            const int *shapes, int ncol) {
    GTable *t = cp_xcalloc(1, sizeof(GTable));
    if (ncol < 1) ncol = 1;
    if (ncol > f->nlev) ncol = f->nlev;
    int rows = (f->nlev + ncol - 1) / ncol;
    /* column-major fill (ggplot guide_legend byrow=FALSE): entry i sits in
     * column i/rows, row i%rows. Each column is [key][gap][labels-of-that-
     * column's-width], with a gutter between columns. */
    double *lw = cp_xmalloc(ncol * sizeof(double));
    double total = 0;
    for (int c = 0; c < ncol; c++) {
        lw[c] = 0;
        for (int i = c * rows; i < (c + 1) * rows && i < f->nlev; i++) {
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
    t->nrow = 2 + 2 * rows - 1;
    t->rowh[0] = upt(font_h(cr, SZ_BASE));
    t->rowh[1] = upt(HALF_LINE);
    for (int i = 0; i < rows; i++) {
        t->rowh[2 + 2 * i] = upt(KEY_SIZE);
        if (i < rows - 1) t->rowh[3 + 2 * i] = upt(HALF_LINE * 0.4);
    }

    Grob *g = gt_add(t, G_TEXT, 0, 0, 0, t->ncol - 1);
    g->str = title; g->size = SZ_BASE; g->col = th->title;
    g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    static const double half = 0.5;
    for (int i = 0; i < f->nlev; i++) {
        int r = 2 + 2 * (i % rows), kc = 4 * (i / rows);
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
    Factor *ff = NULL;
    if (spec->facet_var) {
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

    /* ---- usable rows (NA and log-domain filtering) ---- */
    int *use = cp_xmalloc(df->nrow * sizeof(int)), nuse = 0, d_na = 0, d_log = 0;
    for (int r = 0; r < df->nrow; r++) {
        int xok = disc_x ? (xf->idx[r] >= 0)
                : genome_x ? (roff[r] >= 0 && !isnan(xc->num[r]))
                : !isnan(xc->num[r]);
        int ok = xok && (!yc || (disc_y ? yf->idx[r] >= 0 : !isnan(yc->num[r])))
              && (!cf || cf->idx[r] >= 0) && (!shf || shf->idx[r] >= 0)
              && (!cont_col || isfinite(colc->num[r]))
              && (!ff || ff->idx[r] >= 0)
              && (!szc || !isnan(szc->num[r]));
        if (!ok) d_na++;
        else if ((spec->log_x && xc->num[r] <= 0) || (spec->log_y && yc && yc->num[r] <= 0)) {
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
    if (spec->facet_ncol > 0 && spec->facet_nrow > 0) {
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
    if (2 * ncolp + 6 > GT_MAXDIM || 3 * nrowp + 6 > GT_MAXDIM) {
        snprintf(err, CP_ERRLEN, "too many facet panels (%d)", npan);
        return -1;
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
            if (fmin(ta, tb) < lxmin) lxmin = fmin(ta, tb);
            if (fmax(ta, tb) > lxmax) lxmax = fmax(ta, tb);
            double ua = TY(c_y->num[r2]), ub = TY(c_ye->num[r2]);
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
            if (!genome_x) {
                double t = cp_logt(spec->log_x, xs[k]);
                if (t < lxmin) lxmin = t;
                if (t > lxmax) lxmax = t;
            }
            double u = TY(ys2[k]);
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
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            double t = TXR(r);
            if (t < txmin) txmin = t;
            if (t > txmax) txmax = t;
            if (xec && !isnan(xec->num[r])) {   /* segment end extends x range */
                double te = cp_logt(spec->log_x, xec->num[r]);
                if (te < txmin) txmin = te;
                if (te > txmax) txmax = te;
            }
        }
        if (lxmin < txmin) txmin = lxmin;       /* 4-corner rect layer files */
        if (lxmax > txmax) txmax = lxmax;
        if (txmax == txmin) { txmin -= 0.5; txmax += 0.5; }
    }

    /* ---- stat_bin for histogram layers (bins on the transformed scale,
     * ggplot's default alignment: boundary = width/2) ---- */
    typedef struct { int nbins; double start, width; int *counts; int max; } Hist;
    Hist hist[MAX_LAYERS];
    memset(hist, 0, sizeof hist);
    for (int li = 0; li < spec->nlayers; li++) {
        if (spec->layers[li].type != GEOM_HISTOGRAM) continue;
        /* ggplot default binning (verified via ggplot_build): width =
         * range/(bins-1), first bin centered on the data minimum, exactly
         * `bins` bins spanning [min - w/2, max + w/2] */
        Hist *hs = &hist[li];
        int bins = spec->layers[li].bins;
        hs->width = bins > 1 ? (txmax - txmin) / (bins - 1) : (txmax - txmin);
        if (hs->width <= 0) hs->width = 1;
        hs->start = txmin - hs->width / 2;
        hs->nbins = bins;
        hs->counts = cp_xcalloc((size_t)npan * hs->nbins, sizeof(int));
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            int p = ff ? ff->idx[r] : 0;
            int bin = (int)((TXR(r) - hs->start) / hs->width);
            if (bin < 0) bin = 0;
            if (bin >= hs->nbins) bin = hs->nbins - 1;
            hs->counts[p * hs->nbins + bin]++;
        }
        for (int i = 0; i < npan * hs->nbins; i++)
            if (hs->counts[i] > hs->max) hs->max = hs->counts[i];
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
                double lo = fmin(sd, iqr / 1.349);          /* R's bw.nrd0 */
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
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            double t = disc_y ? YVAL(r) : TY(yc->num[r]);
            if (t < tymin) tymin = t;
            if (t > tymax) tymax = t;
            if (yec && !isnan(yec->num[r])) {   /* segment end extends y range */
                double te = TY(yec->num[r]);
                if (te < tymin) tymin = te;
                if (te > tymax) tymax = te;
            }
        }
        if (lymin < tymin) tymin = lymin;       /* 4-corner rect layer files */
        if (lymax > tymax) tymax = lymax;
        if (colsum) {                           /* stacked totals set the top */
            double mx = 0;
            for (int p = 0; p < npan; p++)
                if (colstack_max[p] > mx) mx = colstack_max[p];
            double t = TY(mx);
            if (t > tymax) tymax = t;
        }
        if (hascol && !spec->log_y) {           /* bars are anchored at 0 */
            if (tymin > 0) tymin = 0;
            if (tymax < 0) tymax = 0;
        }
    }
    if (tymax == tymin) { tymin -= 0.5; tymax += 0.5; }

    /* reference lines expand the panel to include their intercept (ggplot) */
    for (int li = 0; li < spec->nlayers; li++) {
        const Layer *L = &spec->layers[li];
        if (L->type == GEOM_HLINE && L->has_intercept) {
            double t = TY(L->intercept);
            if (t < tymin) tymin = t;
            if (t > tymax) tymax = t;
        } else if (L->type == GEOM_VLINE && L->has_intercept && !disc_x && !genome_x) {
            /* Same guard hline has: a value <= 0 has no place on a log axis, and
             * feeding its NaN into the comparisons below only works by accident
             * of IEEE semantics. */
            double t = spec->log_x ? (L->intercept > 0
                                      ? cp_logt(spec->log_x, L->intercept) : NAN)
                                   : L->intercept;
            if (isfinite(t)) {
                if (t < txmin) txmin = t;
                if (t > txmax) txmax = t;
            }
        } else if (L->type == GEOM_ABLINE && !disc_x && !genome_x) {
            /* ggplot trains on the line's endpoints, so a slope that leaves the
             * data range still shows where it crosses. Its siblings above have
             * always done this; abline was simply missed. */
            double ex[2] = { txmin, txmax };
            for (int k = 0; k < 2; k++) {
                double xd = spec->log_x ? pow(10, ex[k]) : ex[k];
                double yv = TY(L->intercept + L->slope * xd);
                if (!isfinite(yv)) continue;
                if (yv < tymin) tymin = yv;
                if (yv > tymax) tymax = yv;
            }
        }
    }

    /* user axis limits (xlim/ylim or scale_*_log10(limits=)): override the
     * data-driven range with the requested domain (log10-transformed when the
     * axis is log). Default expansion is applied below as usual. */
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
    double x0, x1;
    if (disc_x) { x0 = 1 - 0.6; x1 = xf->nlev + 0.6; }
    else if (genome_x) { x0 = 0; x1 = gs->total; }     /* no expansion */
    else { x0 = txmin - 0.05 * (txmax - txmin); x1 = txmax + 0.05 * (txmax - txmin); }
    double y0, y1;
    if (disc_y) { y0 = 1 - 0.6; y1 = yf->nlev + 0.6; }   /* additive, like disc_x */
    else { y0 = tymin - 0.05 * (tymax - tymin); y1 = tymax + 0.05 * (tymax - tymin); }
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
    } else if (spec->log_x) {
        nxbr = log_breaks(spec->log_x, x0, x1, xbr, xlabs, 16);
    } else {
        int nb; 
        if (spec->n_x_breaks) {          /* scale_x_continuous(breaks=c(...)) */
            nb = spec->n_x_breaks;
            for (int i = 0; i < nb; i++)
                xbr[i] = cp_logt(spec->log_x, spec->x_breaks[i]);
        } else nb = extended_breaks(x0, x1, 5, xbr, 16);
        int n = 0, keep[MAX_BREAKS];             /* original index, so labels= stays paired */
        for (int i = 0; i < nb; i++)
            if (xbr[i] >= x0 && xbr[i] <= x1) { keep[n] = i; xbr[n++] = xbr[i]; }
        /* An explicit break outside the range is dropped, as in ggplot2 -- but
         * dropping every one leaves the axis silently unlabelled, which reads
         * as a bug in the figure rather than in the call. */
        if (spec->n_x_breaks && n == 0)
            fprintf(stderr, "cinderplot: warning: every x break given lies outside "
                    "the data range [%g, %g]; the axis has no labels\n",
                    x0, x1);
        nxbr = n;
        int dec = axis_decimals(xbr, nxbr), pdec = dec - 2 < 0 ? 0 : dec - 2;
        for (int i = 0; i < nxbr; i++) {
            if (spec->n_x_break_labs) {  /* labels=c(...): the given text */
                xlabs[i] = cp_xstrdup(spec->x_break_labs[keep[i]]);
                continue;
            }
            xlabs[i] = cp_xmalloc(32);
            if (spec->x_pct) snprintf(xlabs[i], 32, "%.*f%%", pdec, xbr[i] * 100);
            else fmt_break(xbr[i], dec, xlabs[i], 32);
        }
    }
    if (disc_y) {                          /* one break per category, level labels */
        nybr = yf->nlev;
        for (int i = 0; i < nybr; i++) { ybr[i] = i + 1; ylabs[i] = cp_xstrdup(yf->levels[i]); }
    } else if (spec->log_y) {
        nybr = log_breaks(spec->log_y, y0, y1, ybr, ylabs, 16);
    } else {
        int nb;
        if (spec->n_y_breaks) {          /* scale_y_continuous(breaks=c(...)) */
            nb = spec->n_y_breaks;
            for (int i = 0; i < nb; i++)
                ybr[i] = cp_logt(spec->log_y, spec->y_breaks[i]);
        } else nb = extended_breaks(y0, y1, 5, ybr, 16);
        int n = 0, keep[MAX_BREAKS];             /* original index, so labels= stays paired */
        for (int i = 0; i < nb; i++)
            if (ybr[i] >= y0 && ybr[i] <= y1) { keep[n] = i; ybr[n++] = ybr[i]; }
        /* An explicit break outside the range is dropped, as in ggplot2 -- but
         * dropping every one leaves the axis silently unlabelled, which reads
         * as a bug in the figure rather than in the call. */
        if (spec->n_y_breaks && n == 0)
            fprintf(stderr, "cinderplot: warning: every y break given lies outside "
                    "the data range [%g, %g]; the axis has no labels\n",
                    y0, y1);
        nybr = n;
        int dec = axis_decimals(ybr, nybr), pdec = dec - 2 < 0 ? 0 : dec - 2;
        for (int i = 0; i < nybr; i++) {
            if (spec->n_y_break_labs) {  /* labels=c(...): the given text */
                ylabs[i] = cp_xstrdup(spec->y_break_labs[keep[i]]);
                continue;
            }
            ylabs[i] = cp_xmalloc(32);
            if (spec->y_pct) snprintf(ylabs[i], 32, "%.*f%%", pdec, ybr[i] * 100);
            else fmt_break(ybr[i], dec, ylabs[i], 32);
        }
    }
    double *xnpc = cp_xmalloc(nxbr * sizeof(double)), *ynpc = cp_xmalloc(nybr * sizeof(double));
    for (int i = 0; i < nxbr; i++) xnpc[i] = NPCX(xbr[i]);
    for (int i = 0; i < nybr; i++) ynpc[i] = NPCY(ybr[i]);
    double xmin_br[32], ymin_br[32];
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
                            if (use[r] && ff->idx[r] == p && xf->idx[r] == l) {
                                map[l] = k++; break;
                            }
                    S->xmap = map; S->nxlev = k;
                    S->x0 = 1 - 0.6; S->x1 = (k ? k : 1) + 0.6;
                } else if (genome_x) {
                    snprintf(err, CP_ERRLEN, "facet_wrap(scales=) cannot free a "
                             "scale_x_genome() axis; the genome axis is shared by "
                             "construction (use regions() for several windows)");
                    return -1;
                } else {
                    double lo = 1e300, hi = -1e300;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || ff->idx[r] != p) continue;
                        double t = TXR(r);
                        if (t < lo) lo = t;
                        if (t > hi) hi = t;
                        if (xec && !isnan(xec->num[r])) {
                            double te = cp_logt(spec->log_x, xec->num[r]);
                            if (te < lo) lo = te;
                            if (te > hi) hi = te;
                        }
                    }
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
                    S->x0 = lo - 0.05 * (hi - lo);
                    S->x1 = hi + 0.05 * (hi - lo);
                }
            }
            if (spec->free_y) {
                if (disc_y) {
                    int *map = cp_xmalloc(yf->nlev * sizeof(int));
                    for (int l = 0; l < yf->nlev; l++) map[l] = -1;
                    int k = 0;
                    for (int l = 0; l < yf->nlev; l++)
                        for (int r = 0; r < df->nrow; r++)
                            if (use[r] && ff->idx[r] == p && yf->idx[r] == l) {
                                map[l] = k++; break;
                            }
                    S->ymap = map; S->nylev = k;
                    S->y0 = 1 - 0.6; S->y1 = (k ? k : 1) + 0.6;
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
                            for (int b = 0; b < hs->nbins; b++) {
                                int cnt = hs->counts[p * hs->nbins + b];
                                if (cnt > mx) mx = cnt;
                            }
                        }
                        lo = 0; hi = cp_logt(spec->log_y, (double)(mx ? mx : 1));
                    } else if (hasbar) {
                        int mx = 0;
                        for (int cat = 0; cat < xf->nlev; cat++) {
                            int total = 0;
                            for (int gq = 0; gq < barng; gq++)
                                total += barcount[((size_t)(p * xf->nlev + cat)) * barng + gq];
                            if (total > mx) mx = total;
                        }
                        lo = 0; hi = cp_logt(spec->log_y, (double)(mx ? mx : 1));
                    } else if (hasdens) {
                        double mx = 0;
                        for (int di = 0; di < ndens; di++)
                            for (int gg = 0; gg < densg; gg++) {
                                size_t base = ((size_t)((di * npan + p) * densg + gg)) * DENS_N;
                                for (int j = 0; j < DENS_N; j++)
                                    if (dens_y[base + j] > mx) mx = dens_y[base + j];
                            }
                        lo = 0; hi = mx;
                    }
                    if (!nhist && !hasbar && !hasdens) {
                        for (int r = 0; r < df->nrow; r++) {
                            if (!use[r] || ff->idx[r] != p) continue;
                            double t = TY(yc->num[r]);
                            if (t < lo) lo = t;
                            if (t > hi) hi = t;
                            if (yec && !isnan(yec->num[r])) {
                                double te = TY(yec->num[r]);
                                if (te < lo) lo = te;
                                if (te > hi) hi = te;
                            }
                        }
                        if (colsum && TY(colstack_max[p]) > hi)
                            hi = TY(colstack_max[p]);   /* stacked totals */
                        if (hascol && !spec->log_y) {   /* bars anchor at 0 */
                            if (lo > 0) lo = 0;
                            if (hi < 0) hi = 0;
                        }
                    }
                    if (spec->has_ylim) {      /* as for x above */
                        lo = cp_logt(spec->log_y, spec->ylim_lo);
                        hi = cp_logt(spec->log_y, spec->ylim_hi);
                    }
                    if (lo > hi) { lo = 0; hi = 0; }
                    if (hi == lo) { lo -= 0.5; hi += 0.5; }
                    S->y0 = lo - 0.05 * (hi - lo);
                    S->y1 = hi + 0.05 * (hi - lo);
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
                } else if (spec->log_x) {
                    S->xbr = cp_xmalloc(16 * sizeof(double));
                    S->xlabs = cp_xmalloc(16 * sizeof(char *));
                    S->nxbr = log_breaks(spec->log_x, S->x0, S->x1, S->xbr, S->xlabs, 16);
                } else {
                    /* breaks=/labels= apply per panel: each freed panel keeps
                     * the given breaks that land inside ITS range (they used
                     * to be silently ignored on a freed axis). */
                    S->xbr = cp_xmalloc(MAX_BREAKS * sizeof(double));
                    S->xlabs = cp_xmalloc(MAX_BREAKS * sizeof(char *));
                    int nb, keep[MAX_BREAKS];
                    if (spec->n_x_breaks) {
                        nb = spec->n_x_breaks;
                        for (int i = 0; i < nb; i++)
                            S->xbr[i] = cp_logt(spec->log_x, spec->x_breaks[i]);
                    } else nb = extended_breaks(S->x0, S->x1, 5, S->xbr, 16);
                    int k = 0;
                    for (int i = 0; i < nb; i++)
                        if (S->xbr[i] >= S->x0 && S->xbr[i] <= S->x1) {
                            keep[k] = i; S->xbr[k++] = S->xbr[i];
                        }
                    S->nxbr = k;
                    int dec = axis_decimals(S->xbr, S->nxbr), pdec = dec - 2 < 0 ? 0 : dec - 2;
                    for (int i = 0; i < S->nxbr; i++) {
                        if (spec->n_x_break_labs) {
                            S->xlabs[i] = cp_xstrdup(spec->x_break_labs[keep[i]]);
                            continue;
                        }
                        S->xlabs[i] = cp_xmalloc(32);
                        if (spec->x_pct) snprintf(S->xlabs[i], 32, "%.*f%%", pdec, S->xbr[i] * 100);
                        else fmt_break(S->xbr[i], dec, S->xlabs[i], 32);
                    }
                }
                S->xnpc = cp_xmalloc((S->nxbr + 1) * sizeof(double));
                for (int i = 0; i < S->nxbr; i++)
                    S->xnpc[i] = (S->xbr[i] - S->x0) / (S->x1 - S->x0);
                S->xmin_br = cp_xmalloc(32 * sizeof(double));
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
                } else if (spec->log_y) {
                    S->ybr = cp_xmalloc(16 * sizeof(double));
                    S->ylabs = cp_xmalloc(16 * sizeof(char *));
                    S->nybr = log_breaks(spec->log_y, S->y0, S->y1, S->ybr, S->ylabs, 16);
                } else {
                    /* as for x: breaks=/labels= apply per freed panel */
                    S->ybr = cp_xmalloc(MAX_BREAKS * sizeof(double));
                    S->ylabs = cp_xmalloc(MAX_BREAKS * sizeof(char *));
                    int nb, keep[MAX_BREAKS];
                    if (spec->n_y_breaks) {
                        nb = spec->n_y_breaks;
                        for (int i = 0; i < nb; i++)
                            S->ybr[i] = cp_logt(spec->log_y, spec->y_breaks[i]);
                    } else nb = extended_breaks(S->y0, S->y1, 5, S->ybr, 16);
                    int k = 0;
                    for (int i = 0; i < nb; i++)
                        if (S->ybr[i] >= S->y0 && S->ybr[i] <= S->y1) {
                            keep[k] = i; S->ybr[k++] = S->ybr[i];
                        }
                    S->nybr = k;
                    int dec = axis_decimals(S->ybr, S->nybr), pdec = dec - 2 < 0 ? 0 : dec - 2;
                    for (int i = 0; i < S->nybr; i++) {
                        if (spec->n_y_break_labs) {
                            S->ylabs[i] = cp_xstrdup(spec->y_break_labs[keep[i]]);
                            continue;
                        }
                        S->ylabs[i] = cp_xmalloc(32);
                        if (spec->y_pct) snprintf(S->ylabs[i], 32, "%.*f%%", pdec, S->ybr[i] * 100);
                        else fmt_break(S->ybr[i], dec, S->ylabs[i], 32);
                    }
                }
                S->ynpc = cp_xmalloc((S->nybr + 1) * sizeof(double));
                for (int i = 0; i < S->nybr; i++)
                    S->ynpc[i] = (S->ybr[i] - S->y0) / (S->y1 - S->y0);
                S->ymin_br = cp_xmalloc(32 * sizeof(double));
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

    /* ---- geom_col bar width: 0.9 x min gap between distinct x ---- */
    double colw = 0.9;
    if (hascol) {
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
    const Theme *th = &THEMES[spec->theme];   /* active theme (THEME_GRAY = default) */

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
                int nc2 = spec->legend_ncol ? spec->legend_ncol
                        : spec->legend_nrow
                        ? (k + spec->legend_nrow - 1) / spec->legend_nrow : 1;
                GTable *lg = build_legend(cr, th, ff->levels[p], &pf, pc2,
                                   haspoint, hasline || hasseg || hasdens,
                                   hasbox || hasbar || hascol || hasrect || hastile,
                                   hastext, NULL, nc2);
                if (nrowp == 1) fc_leg[p] = lg;
                else guides[nguide++] = lg;
            }
        } else if (!spec->no_legend && !spec->identity_scale) {
            int nc2 = spec->legend_ncol ? spec->legend_ncol
                    : spec->legend_nrow
                    ? (cf->nlev + spec->legend_nrow - 1) / spec->legend_nrow : 1;
            guides[nguide++] = build_legend(cr, th, col_title, cf, pal, haspoint,
                               hasline || hasseg || hasdens,
                               hasbox || hasbar || hascol || hasrect || hastile,
                               hastext, NULL, nc2);   /* bars/tiles key as filled boxes */
        }
    } else if (cont_col && !spec->no_legend) {
        guides[nguide++] = build_colorbar_legend(cr, th, col_title, &cscale, cdmin, cdmax);
    }
    if (szc && !spec->no_legend) {               /* size legend: representative breaks */
        double sbr[16]; int nsb = extended_breaks(szmin, szmax, 5, sbr, 16), nf = 0;
        for (int i = 0; i < nsb; i++) if (sbr[i] >= szmin && sbr[i] <= szmax) sbr[nf++] = sbr[i];
        if (nf == 0) { sbr[0] = szmin; sbr[1] = szmax; nf = szmax > szmin ? 2 : 1; }
        double srad[16];
        for (int i = 0; i < nf; i++) srad[i] = size_to_radius(sbr[i], szmin, szmax);
        int sdec = axis_decimals(sbr, nf);
        const char *sz_title = spec->size.expr;
        guides[nguide++] = build_size_legend(cr, th, sz_title, sbr, srad, nf, sdec);
    }
    if (shf && !spec->no_legend) {
        /* A shape legend keys the GLYPH, so its swatches are all one colour --
         * otherwise the reader reads a colour that means nothing. */
        Col *spal = cp_xmalloc(shf->nlev * sizeof(Col));
        int *sidx = cp_xmalloc(shf->nlev * sizeof(int));
        for (int i = 0; i < shf->nlev; i++) { spal[i] = C_BLACK; sidx[i] = i; }
        const char *sh_title = spec->shape.expr;
        guides[nguide++] = build_legend(cr, th, sh_title, shf, spal, 1, 0, 0, 0, sidx, 1);
    }
    for (int a = 0; a < nann; a++)               /* annotation band keys */
        if (!spec->no_legend)
            guides[nguide++] = build_legend(cr, th, anns[a].title, anns[a].f,
                                            anns[a].apal, 0, 0, 1, 0, NULL, 1);
    if (nguide) leg = stack_guides(guides, nguide);
    double fc_leg_h = 0;             /* per-panel legend row height (free_colour) */
    for (int p = 0; p < npan && p < 12; p++)
        if (fc_leg[p] && gt_fixed_h(fc_leg[p]) > fc_leg_h)
            fc_leg_h = gt_fixed_h(fc_leg[p]);
    if (fc_leg_h > 0) fc_leg_h += HALF_LINE;

    /* ---- outer table ---- */
    GTable *T = cp_xcalloc(1, sizeof(GTable));
    T->ncol = 2 * ncolp + 6;
    T->colw[0] = upt(MARGIN);
    T->colw[1] = upt(baseh);
    T->colw[2] = upt(HALF_LINE / 2);
    T->colw[3] = upt(ylab_w + TXT_GAP + TICK_LEN);
    /* Under free scales the gap between panels has to hold an axis, not just
     * whitespace: every panel carries its own ticks and labels. Each gap is
     * sized by the widest label of the panels immediately to its right. */
    int lfree_l = flip ? spec->free_x : spec->free_y;
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
        double catpitch = labh * 1.15;          /* readable pitch for one category */
        int maxx = 0, maxy = 0;
        for (int p = 0; p < npan; p++) {
            if (ps[p].nxlev > maxx) maxx = ps[p].nxlev;
            if (ps[p].nylev > maxy) maxy = ps[p].nylev;
        }
        if (w_pt <= 0) {
            double panelw = disc_x && maxx ? maxx * catpitch : 4.0 * 72;
            double chrome = MARGIN + ylab_w + TICK_LEN + TXT_GAP + baseh + MARGIN
                          + (leg ? gt_fixed_w(leg) + 2 * HALF_LINE : 0);
            w_pt = chrome + ncolp * panelw + (ncolp - 1) * PANEL_SPACE;
            w_pt = fmin(30.0 * 72, fmax(6.0 * 72, w_pt));
        }
        if (h_pt <= 0) {
            double panelh = disc_y && maxy ? maxy * catpitch : 2.6 * 72;
            double chrome = MARGIN * 2 + labh + TICK_LEN + TXT_GAP + baseh
                          + (spec->lab_title ? font_h(cr, SZ_TITLE) : 0);
            h_pt = chrome + nrowp * (panelh + striph + band_h)
                 + (nrowp - 1) * PANEL_SPACE;
            /* a tall legend stack (many discrete levels) sizes the canvas
             * too — it used to overflow the top and clip its first entries */
            if (leg) h_pt = fmax(h_pt, gt_fixed_h(leg) + 4 * MARGIN);
            h_pt += fc_leg_h;        /* per-panel legend row (free_colour) */
            h_pt = fmin(30.0 * 72, fmax(4.0 * 72, h_pt));
        }
    }
    if (leg && gt_fixed_h(leg) > h_pt)
        fprintf(stderr, "cinderplot: warning: the legend stack needs %.1fin of "
                "a %.1fin figure and will clip; give a taller --size, drop it "
                "with guides(colour=\"none\"), or reduce the levels\n",
                gt_fixed_h(leg) / 72, h_pt / 72);

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

    int bfree_l = flip ? spec->free_y : spec->free_x;
    for (int r = 0; r < nrowp; r++) {
        T->rowh[SR(r)] = upt(striph);
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
    if (th->axis_title_on) {
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
        if (fc_leg[p]) {
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

        if (ff) {
            if (th->strip_bg_on) { g = gt_add(T, G_RECT, SR(pr), C, SR(pr), C); g->col = th->strip_bg; }
            g = gt_add(T, G_TEXT, SR(pr), C, SR(pr), C);
            g->str = ff->levels[p]; g->size = SZ_AXIS_TEXT; g->col = th->strip_text;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }

        int gstart = T->ngrobs;   /* first panel-content grob (all in cell R,C); */
                                  /* under coord_flip these get transposed below */
        if (th->panel_bg_on) { g = gt_add(T, G_RECT, R, C, R, C); g->col = th->panel_bg; }
        if (th->grid_minor_on) {
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
        if (th->grid_major_on) {
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
        if (th->border_on) {                          /* bw / linedraw / light / few */
            g = gt_add(T, G_RECT, R, C, R, C);
            g->col = th->border; g->stroke = 1; g->lw = lw_pt(th->border_lw);
        }
        if (th->axis_line_on) {                       /* classic / pubr */
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

        /* layers, in spec order */
        for (int li = 0; li < spec->nlayers; li++) {
            GeomType gt = spec->layers[li].type;
            /* alpha= and linetype= belong to the whole layer, and every geom
             * builds its grobs differently, so rather than threading them
             * through each branch, note where this layer's grobs start and
             * stamp them all once the branch has run (see end of the loop). */
            int grob0 = T->ngrobs;
            if (gt == GEOM_HISTOGRAM) {
                Hist *hs = &hist[li];
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int b = 0; b < hs->nbins; b++) {
                    int cnt = hs->counts[p * hs->nbins + b];
                    if (!cnt) continue;
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = panelfill && panelfill[p] >= 0 ? pal[panelfill[p]]
                           : spec->layers[li].has_color ? spec->layers[li].color
                           : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(hs->start + b * hs->width);
                    g->x1 = NPCX(hs->start + (b + 1) * hs->width);
                    g->y0 = base;
                    g->y1 = NPCY(cp_logt(spec->log_y, (double)cnt));
                }
            } else if (gt == GEOM_DENSITY) {
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
                    double tx = TXR(r), ty = TY(yc->num[r]);
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cont_col ? CCOL(r)
                           : panelfill && panelfill[p] >= 0 ? pal[panelfill[p]]
                           : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(tx - colw / 2); g->x1 = NPCX(tx + colw / 2);
                    g->y0 = fmin(base, NPCY(ty)); g->y1 = fmax(base, NPCY(ty));
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
                /* LOESS, as ggplot2's default for n < 1000: at each output x,
                 * take the nearest `span` fraction of the data, weight them by
                 * the tricube of their scaled distance, and fit a local
                 * quadratic. Degree 2 rather than 1 because a local line
                 * flattens peaks, and a methylation trace is mostly peaks.
                 *
                 * One curve per colour group per panel, like geom_line -- a
                 * single smooth across groups would average away the very
                 * difference the layer is there to show. */
                const Layer *SL = &spec->layers[li];
                double span = SL->span > 0 ? SL->span : 0.75;
                int ngrp = cf ? cf->nlev : 1;
                for (int grp = 0; grp < ngrp; grp++) {
                    int nn = 0;
                    for (int r = 0; r < df->nrow; r++)
                        if (use[r] && (!ff || ff->idx[r] == p)
                                   && (!cf || cf->idx[r] == grp)) nn++;
                    if (nn < 4) continue;          /* nothing to fit through */
                    double *sx = cp_xmalloc(nn * sizeof(double));
                    double *sy = cp_xmalloc(nn * sizeof(double));
                    nn = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)) continue;
                        if (cf && cf->idx[r] != grp) continue;
                        sx[nn] = TXR(r); sy[nn] = TY(yc->num[r]); nn++;
                    }
                    /* sort by x: the neighbourhood is a window over sorted x */
                    for (int a = 1; a < nn; a++) {
                        double kx = sx[a], ky = sy[a]; int b2 = a - 1;
                        while (b2 >= 0 && sx[b2] > kx) {
                            sx[b2+1] = sx[b2]; sy[b2+1] = sy[b2]; b2--;
                        }
                        sx[b2+1] = kx; sy[b2+1] = ky;
                    }
                    int q = (int)ceil(span * nn);
                    if (q < 3) q = 3;
                    if (q > nn) q = nn;
                    const int NS = 200;            /* output resolution */
                    Pt *pts = cp_xmalloc(NS * sizeof(Pt));
                    int npt = 0;
                    for (int k = 0; k < NS; k++) {
                        double x0v = sx[0] + (sx[nn-1] - sx[0]) * k / (NS - 1.0);
                        /* the q nearest by x, as a window [lo, lo+q) */
                        int lo = 0, hi = nn - q;
                        while (lo < hi) {
                            int mid = (lo + hi) / 2;
                            if (x0v - sx[mid] > sx[mid + q] - x0v) lo = mid + 1;
                            else hi = mid;
                        }
                        double dmax = fmax(fabs(x0v - sx[lo]), fabs(sx[lo+q-1] - x0v));
                        if (dmax <= 0) dmax = 1e-12;
                        /* weighted quadratic by normal equations on (x - x0) */
                        double m[3][4] = {{0}};
                        for (int i2 = lo; i2 < lo + q; i2++) {
                            double d = fabs(sx[i2] - x0v) / dmax;
                            if (d >= 1) continue;
                            double t = 1 - d * d * d, w = t * t * t;
                            double u = sx[i2] - x0v, u2 = u * u;
                            double b0 = 1, b1 = u, b2v = u2;
                            double bb[3] = {b0, b1, b2v};
                            for (int a2 = 0; a2 < 3; a2++) {
                                for (int c2 = 0; c2 < 3; c2++) m[a2][c2] += w * bb[a2] * bb[c2];
                                m[a2][3] += w * bb[a2] * sy[i2];
                            }
                        }
                        /* Gaussian elimination with partial pivoting; a
                         * degenerate neighbourhood (every x identical) falls
                         * back to the weighted mean, which is m[0][3]/m[0][0]. */
                        double sol[3] = {0,0,0};
                        int ok = 1;
                        for (int c2 = 0; c2 < 3 && ok; c2++) {
                            int piv = c2;
                            for (int r2 = c2 + 1; r2 < 3; r2++)
                                if (fabs(m[r2][c2]) > fabs(m[piv][c2])) piv = r2;
                            if (fabs(m[piv][c2]) < 1e-12) { ok = 0; break; }
                            if (piv != c2) for (int j2 = 0; j2 < 4; j2++) {
                                double t2 = m[c2][j2]; m[c2][j2] = m[piv][j2]; m[piv][j2] = t2;
                            }
                            for (int r2 = c2 + 1; r2 < 3; r2++) {
                                double f = m[r2][c2] / m[c2][c2];
                                for (int j2 = c2; j2 < 4; j2++) m[r2][j2] -= f * m[c2][j2];
                            }
                        }
                        double yv;
                        if (ok) {
                            for (int r2 = 2; r2 >= 0; r2--) {
                                double acc = m[r2][3];
                                for (int c2 = r2 + 1; c2 < 3; c2++) acc -= m[r2][c2] * sol[c2];
                                sol[r2] = acc / m[r2][r2];
                            }
                            yv = sol[0];           /* the fit AT x0v, where u = 0 */
                        } else if (fabs(m[0][0]) > 1e-12) yv = m[0][3] / m[0][0];
                        else continue;
                        if (!isfinite(yv)) continue;
                        pts[npt].x = NPCX(x0v); pts[npt].y = NPCY(yv); npt++;
                    }
                    if (npt >= 2) {
                        g = gt_add(T, G_POLYLINE, R, C, R, C);
                        double *lx = cp_xmalloc(npt * sizeof(double));
                        double *ly = cp_xmalloc(npt * sizeof(double));
                        for (int k = 0; k < npt; k++) { lx[k] = pts[k].x; ly[k] = pts[k].y; }
                        g->n = npt; g->px = lx; g->py = ly;
                        g->col = SL->has_color ? SL->color : cf ? pal[grp] : C_BLACK;
                        g->lw = lw_pt(SL->point_size > 0 ? SL->point_size : 1.0);
                        g->dash = SL->dash; g->alpha = SL->alpha; g->clip = 1;
                    }
                    free(sx); free(sy); free(pts);
                }
            } else if (gt == GEOM_LINE) {
                int ngrp = cf ? cf->nlev : 1;
                for (int grp = 0; grp < ngrp; grp++) {
                    int np = 0;
                    for (int r = 0; r < df->nrow; r++)
                        if (use[r] && (!ff || ff->idx[r] == p)
                                   && (!cf || cf->idx[r] == grp)) np++;
                    if (np < 2) continue;
                    Pt *pts = cp_xmalloc(np * sizeof(Pt));
                    np = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)
                                    || (cf && cf->idx[r] != grp)) continue;
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
                           : cf ? pal[grp] : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
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
                    g->col = lcol; g->lw = lw_pt(0.6); g->clip = 1;
                    g->x0 = NPCX(off + c_x->num[r2]);
                    g->x1 = NPCX(off + (c_xe ? c_xe->num[r2] : c_x->num[r2]));
                    g->y0 = g->y1 = NPCY(TY(c_y->num[r2]));
                }
            } else if (gt == GEOM_SEGMENT) {
                /* one line per row: (x,y) -> (xend, yend); yend defaults to y */
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (xec && isnan(xec->num[r])) continue;
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->x0 = NPCX(TXR(r));
                    g->x1 = NPCX(xec ? (genome_x ? GX(r, xec->num[r])
                                      : cp_logt(spec->log_x, xec->num[r]))
                                     : TXR(r));
                    g->y0 = NPCY(TY(yc->num[r]));
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
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(cx - wx / 2); g->x1 = NPCX(cx + wx / 2);
                    g->y0 = NPCY(cy - wy / 2); g->y1 = NPCY(cy + wy / 2);
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
                /* filled rectangle per row: (xmin,ymin) .. (xmax,ymax) */
                Col fixed = spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (isnan(xec->num[r]) || isnan(yec->num[r])) continue;
                    double a = NPCX(TXR(r));
                    double b = NPCX(genome_x ? GX(r, xec->num[r])
                                  : cp_logt(spec->log_x, xec->num[r]));
                    double c0 = NPCY(TY(yc->num[r])), d = NPCY(TY(yec->num[r]));
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : fixed;
                    g->sub = 1; g->clip = 1;
                    g->x0 = fmin(a, b); g->x1 = fmax(a, b);
                    g->y0 = fmin(c0, d); g->y1 = fmax(c0, d);
                }
            } else if (gt == GEOM_BOXPLOT) {
                /* five-number summary + Tukey whiskers + outliers, in
                 * transformed-y space; position_dodge2 when box_dodge */
                const double WFULL = 0.75;               /* undodged box width */
                double slotw = WFULL / box_slots;
                double boxw = box_slots > 1 ? slotw * 0.9 : WFULL;  /* padding 0.1 */
                for (int cat = 0; cat < xf->nlev; cat++) {
                    for (int s = 0; s < box_slots; s++) {
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
                        double center = (cat + 1) - WFULL / 2 + slotw * (s + 0.5);
                        double xl = NPCX(center - boxw / 2), xr = NPCX(center + boxw / 2);
                        double xm = NPCX(center);
                        /* aes(fill=) colours the box BODY and keeps ggplot's
                         * dark chrome (outline, whiskers, median, outliers);
                         * aes(colour=) colours the chrome over a white body.
                         * One shared aes carries both spellings, so the
                         * recorded spelling decides — writing fill= used to
                         * silently render the colour= look. */
                        Col grpc = cf ? pal[box_slots > 1 ? s : anyg] : C_TICK;
                        int fillbox = cf && spec->colour.is_fill;
                        Col lc = fillbox ? C_TICK : grpc;
                        Col body = fillbox ? grpc : C_WHITE;

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
            } else if (gt == GEOM_HLINE) {
                const Layer *L = &spec->layers[li];
                /* a reference value <= 0 has no place on a log axis; ggplot
                 * drops such a line rather than drawing at log10(<=0) = NaN */
                double yt = NPCY(TY(L->intercept));
                if (L->has_intercept && isfinite(yt)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = yt;
                }
            } else if (gt == GEOM_VLINE) {
                const Layer *L = &spec->layers[li];
                double xt = cp_logt(spec->log_x, L->intercept);
                double xn = NPCX(xt);
                if (L->has_intercept && isfinite(xn)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->y0 = 0; g->y1 = 1; g->x0 = g->x1 = xn;
                }
            } else if (gt == GEOM_ABLINE) {
                const Layer *L = &spec->layers[li];
                double xl = spec->log_x ? pow(10, x0) : x0;   /* data-space edges */
                double xr = spec->log_x ? pow(10, x1) : x1;
                double yl = NPCY(TY(L->intercept + L->slope * xl));
                double yr = NPCY(TY(L->intercept + L->slope * xr));
                if (isfinite(yl) && isfinite(yr)) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->x0 = 0; g->x1 = 1;
                    g->y0 = yl; g->y1 = yr;
                }
            } else if (gt == GEOM_TEXT || gt == GEOM_LABEL) {
                const Layer *L = &spec->layers[li];
                double fs = (L->txt_size > 0 ? L->txt_size : 3.88) * 2.845276; /* mm -> pt */
                double ndx = x1 > x0 ? L->nudge_x / (x1 - x0) : 0;   /* data -> npc */
                double ndy = y1 > y0 ? L->nudge_y / (y1 - y0) : 0;
                int cap = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p)
                        && !isnan(TXR(r)) && !isnan(yc->num[r])) cap++;
                if (cap > 0) {
                    RLabel *rl = cp_xmalloc(cap * sizeof(RLabel));
                    const char **strs = cp_xmalloc(cap * sizeof(char *));
                    Col *cols = cp_xmalloc(cap * sizeof(Col));
                    double *px = cp_xmalloc(cap * sizeof(double)), *py = cp_xmalloc(cap * sizeof(double));
                    double bpad = (gt == GEOM_LABEL ? fs * 0.25 : 0) + PT_RADIUS * 0.6;
                    int m = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)) continue;
                        if (isnan(TXR(r)) || isnan(yc->num[r])) continue;
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
                        double axp = NPCX(TXR(r)) * panelw_pt, ayp = NPCY(TY(yc->num[r])) * panelh_pt;
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
                        cols[m] = L->has_color ? L->color
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

        /* coord_flip: transpose every panel-content grob (x <-> y npc). The
         * gridlines therefore align with the re-pointed left/bottom axes. */
        if (flip)
            for (int gi = gstart; gi < T->ngrobs; gi++) flip_grob(&T->grobs[gi]);

        /* Left axis. Shared scales label the left column only, because every
         * panel in a row carries the same one; a freed axis differs per panel,
         * so each gets its own, drawn in the spacer to its left. */
        int lfree = flip ? spec->free_x : spec->free_y;
        if (pc == 0 || lfree) {
            g = gt_add(T, G_AXIS_Y, R, pc == 0 ? 3 : PC(pc) - 1, R, pc == 0 ? 3 : PC(pc) - 1);
            g->n = flip ? S->nxbr : S->nybr;
            g->py = flip ? S->xnpc : S->ynpc;
            g->labels = flip ? S->xlabs : S->ylabs;
            g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
            g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
        }
        /* annotate(): one-off marks at literal data coords, drawn over the
         * geoms in every panel. Coordinates go through the panel's own
         * scales, and the coord_flip transpose below catches these grobs
         * like any other panel content. */
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
        int bfree = flip ? spec->free_y : spec->free_x;
        if (bfree) {
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
    for (int c = 0; c < ncolp && c < npan && !(flip ? spec->free_y : spec->free_x); c++) {
        int rb = (npan - 1 - c) / ncolp;
        if (rb == nrowp - 1)
            g = gt_add(T, G_AXIS_X, r_axis, PC(c), r_axis, PC(c));
        else
            g = gt_add(T, G_AXIS_X, PR(rb) + 2, PC(c), PR(rb + 1), PC(c));
        if (genome_x) { g->n = gax_n; g->px = gax_pos; g->labels = gax_lab; }
        else {
            g->n = bax_n; g->px = bax_pos; g->labels = bax_lab;   /* log ticks drawn inside the panel */
            g->label_angle = bang;
        }
        g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
        g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
    }

    /* ---- go ---- */
    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_status_t st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        snprintf(err, CP_ERRLEN, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
