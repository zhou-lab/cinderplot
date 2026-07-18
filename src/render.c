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

static GTable *build_legend(cairo_t *cr, const char *title, const Factor *f,
                            const Col *pal, int haspoint, int hasline, int hasbox) {
    GTable *t = calloc(1, sizeof(GTable));
    double label_w = 0;
    for (int i = 0; i < f->nlev; i++) {
        double w = text_w(cr, SZ_AXIS_TEXT, f->levels[i]);
        if (w > label_w) label_w = w;
    }
    double title_w = text_w(cr, SZ_BASE, title);
    if (title_w > KEY_SIZE + TXT_GAP + label_w)
        label_w = title_w - KEY_SIZE - TXT_GAP;

    t->ncol = 3;
    t->colw[0] = upt(KEY_SIZE);
    t->colw[1] = upt(TXT_GAP);
    t->colw[2] = upt(label_w);
    t->nrow = 2 + 2 * f->nlev - 1;
    t->rowh[0] = upt(font_h(cr, SZ_BASE));
    t->rowh[1] = upt(HALF_LINE);
    for (int i = 0; i < f->nlev; i++) {
        t->rowh[2 + 2 * i] = upt(KEY_SIZE);
        if (i < f->nlev - 1) t->rowh[3 + 2 * i] = upt(HALF_LINE * 0.4);
    }

    Grob *g = gt_add(t, G_TEXT, 0, 0, 0, 2);
    g->str = title; g->size = SZ_BASE; g->col = C_BLACK;
    g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    static const double half = 0.5;
    for (int i = 0; i < f->nlev; i++) {
        int r = 2 + 2 * i;
        g = gt_add(t, G_RECT, r, 0, r, 0); g->col = C_KEYBG;
        if (hasbox) {
            g = gt_add(t, G_RECT, r, 0, r, 0);
            g->col = pal[i]; g->sub = 1;
            g->x0 = 0.15; g->x1 = 0.85; g->y0 = 0.15; g->y1 = 0.85;
        }
        if (hasline) {
            g = gt_add(t, G_LINE, r, 0, r, 0);
            g->col = pal[i]; g->lw = lw_pt(0.5);
            g->x0 = 0.1; g->x1 = 0.9; g->y0 = g->y1 = 0.5;
        }
        if (haspoint) {
            g = gt_add(t, G_POINTS, r, 0, r, 0);
            g->n = 1; g->px = &half; g->py = &half;
            g->pcol = &pal[i]; g->radius = PT_RADIUS;
        }
        g = gt_add(t, G_TEXT, r, 2, r, 2);
        g->str = f->levels[i]; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
        g->tx = 0; g->ty = 0.5; g->hj = 0; g->va = V_INKCENTER;
    }
    return t;
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

int render_plot(const PlotSpec *spec, const DataFrame *df, const char *out,
                double w_pt, double h_pt, char *err) {
    /* ---- layer summary ---- */
    int haspoint = 0, hasline = 0, hascol = 0, nhist = 0, hasbox = 0;
    for (int i = 0; i < spec->nlayers; i++) {
        if (spec->layers[i].type == GEOM_POINT) haspoint = 1;
        if (spec->layers[i].type == GEOM_LINE) hasline = 1;
        if (spec->layers[i].type == GEOM_COL) hascol = 1;
        if (spec->layers[i].type == GEOM_HISTOGRAM) nhist++;
        if (spec->layers[i].type == GEOM_BOXPLOT) hasbox = 1;
    }

    /* ---- resolve columns ---- */
    const Column *xc = df_col(df, spec->x.col);
    if (!xc) { sprintf(err, "column `%s` not found", spec->x.col); return -1; }
    /* discrete x when the column is a string or wrapped in factor() */
    int disc_x = (xc->type == COL_STR) || spec->x.is_factor;
    Factor *xf = disc_x ? factor_make(df, xc) : NULL;
    if (!disc_x && (xc->type != COL_NUM)) {
        sprintf(err, "x column `%s` is not numeric", spec->x.col); return -1;
    }
    if (disc_x && spec->log_x) {
        sprintf(err, "scale_x_log10() needs a continuous x"); return -1;
    }
    if (disc_x && nhist) {
        sprintf(err, "geom_histogram() needs a continuous x"); return -1;
    }
    if (hasbox && !disc_x) {
        sprintf(err, "geom_boxplot() needs a discrete x; use aes(x=factor(%s), ...)", spec->x.col);
        return -1;
    }
    const Column *yc = NULL;
    if (spec->y.col) {
        yc = df_col(df, spec->y.col);
        if (!yc) { sprintf(err, "column `%s` not found", spec->y.col); return -1; }
        if (yc->type != COL_NUM || spec->y.is_factor) {
            sprintf(err, "discrete positional scales are not implemented; y must be numeric");
            return -1;
        }
    }
    Factor *cf = NULL;
    if (spec->colour.col) {
        if (hascol || nhist) {
            sprintf(err, "colour/fill on bars (stacking/dodging) is not implemented yet");
            return -1;
        }
        const Column *cc = df_col(df, spec->colour.col);
        if (!cc) { sprintf(err, "column `%s` not found", spec->colour.col); return -1; }
        if (!spec->colour.is_factor && cc->type == COL_NUM) {
            sprintf(err, "continuous colour scales are not implemented; "
                         "use colour=factor(%s)", spec->colour.col);
            return -1;
        }
        cf = factor_make(df, cc);
    }
    Factor *ff = NULL;
    if (spec->facet_var) {
        const Column *fc = df_col(df, spec->facet_var);
        if (!fc) { sprintf(err, "column `%s` not found", spec->facet_var); return -1; }
        ff = factor_make(df, fc);
        if (ff->nlev < 1) { sprintf(err, "facet column `%s` has no values", spec->facet_var); return -1; }
    }

    /* ---- usable rows (NA and log-domain filtering) ---- */
    int *use = malloc(df->nrow * sizeof(int)), nuse = 0, d_na = 0, d_log = 0;
    for (int r = 0; r < df->nrow; r++) {
        int xok = disc_x ? (xf->idx[r] >= 0) : !isnan(xc->num[r]);
        int ok = xok && (!yc || !isnan(yc->num[r]))
              && (!cf || cf->idx[r] >= 0) && (!ff || ff->idx[r] >= 0);
        if (!ok) d_na++;
        else if ((spec->log_x && xc->num[r] <= 0) || (spec->log_y && yc && yc->num[r] <= 0)) {
            ok = 0; d_log++;
        }
        use[r] = ok;
        nuse += ok;
    }
    if (nuse == 0) { sprintf(err, "no complete rows to plot"); return -1; }
    if (d_na) fprintf(stderr, "cinderplot: removed %d rows with missing values\n", d_na);
    if (d_log) fprintf(stderr, "cinderplot: removed %d rows with non-positive values on a log axis\n", d_log);

#define TY(v) (spec->log_y ? log10(v) : (v))
/* transformed x for row r: category position (discrete) or value (continuous) */
#define XVAL(r) (disc_x ? (double)(xf->idx[r] + 1) : xc->num[r])
#define TXR(r)  (spec->log_x ? log10(XVAL(r)) : XVAL(r))

    /* ---- panel grid: ggplot2 wrap_dims = rev(grDevices::n2mfrow(n)) ---- */
    int npan = ff ? ff->nlev : 1, ncolp, nrowp;
    if (npan <= 3)       { ncolp = npan;            nrowp = 1; }
    else if (npan <= 6)  { ncolp = (npan + 1) / 2;  nrowp = 2; }
    else if (npan <= 12) { ncolp = (npan + 2) / 3;  nrowp = 3; }
    else {
        ncolp = (int)ceil(sqrt((double)npan));
        nrowp = (npan + ncolp - 1) / ncolp;
    }
    if (2 * ncolp + 6 > GT_MAXDIM || 3 * nrowp + 6 > GT_MAXDIM) {
        sprintf(err, "too many facet panels (%d)", npan);
        return -1;
    }

    /* ---- x scale training (transformed space) ---- */
    double txmin, txmax;
    if (disc_x) {                          /* categories at 1..k */
        txmin = 1; txmax = xf->nlev;
    } else {
        txmin = 1e300; txmax = -1e300;
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            double t = TXR(r);
            if (t < txmin) txmin = t;
            if (t > txmax) txmax = t;
        }
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
        hs->counts = calloc((size_t)npan * hs->nbins, sizeof(int));
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

    /* ---- y scale training ---- */
    double tymin = 1e300, tymax = -1e300;
    if (nhist) {
        tymin = 0; /* log10(1), the smallest rendered non-zero count */
        tymax = 0;
        for (int li = 0; li < spec->nlayers; li++)
            if (spec->layers[li].type == GEOM_HISTOGRAM) {
                double ymax = spec->log_y ? log10((double)hist[li].max)
                                          : (double)hist[li].max;
                if (ymax > tymax) tymax = ymax;
            }
    } else {
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            double t = TY(yc->num[r]);
            if (t < tymin) tymin = t;
            if (t > tymax) tymax = t;
        }
        if (hascol && !spec->log_y) {           /* bars are anchored at 0 */
            if (tymin > 0) tymin = 0;
            if (tymax < 0) tymax = 0;
        }
    }
    if (tymax == tymin) { tymin -= 0.5; tymax += 0.5; }

    /* ---- expansion + breaks. Discrete x uses ggplot's additive 0.6 on
     * each side; continuous uses 5% of the range. ---- */
    double x0, x1;
    if (disc_x) { x0 = 1 - 0.6; x1 = xf->nlev + 0.6; }
    else { x0 = txmin - 0.05 * (txmax - txmin); x1 = txmax + 0.05 * (txmax - txmin); }
    double y0 = tymin - 0.05 * (tymax - tymin), y1 = tymax + 0.05 * (tymax - tymin);
#define NPCX(t) (((t) - x0) / (x1 - x0))
#define NPCY(t) (((t) - y0) / (y1 - y0))

    double xbr[16], ybr[16];
    char **xlabs = malloc(16 * sizeof(char *)), **ylabs = malloc(16 * sizeof(char *));
    int nxbr, nybr;
    if (disc_x) {                          /* one break per category, level labels */
        nxbr = xf->nlev;
        for (int i = 0; i < nxbr; i++) { xbr[i] = i + 1; xlabs[i] = strdup(xf->levels[i]); }
    } else if (spec->log_x) {
        nxbr = log10_breaks(x0, x1, xbr, xlabs, 16);
    } else {
        int nb = extended_breaks(x0, x1, 5, xbr, 16), n = 0;
        for (int i = 0; i < nb; i++) if (xbr[i] >= x0 && xbr[i] <= x1) xbr[n++] = xbr[i];
        nxbr = n;
        int dec = axis_decimals(xbr, nxbr);
        for (int i = 0; i < nxbr; i++) { xlabs[i] = malloc(32); fmt_break(xbr[i], dec, xlabs[i]); }
    }
    if (spec->log_y) {
        nybr = log10_breaks(y0, y1, ybr, ylabs, 16);
    } else {
        int nb = extended_breaks(y0, y1, 5, ybr, 16), n = 0;
        for (int i = 0; i < nb; i++) if (ybr[i] >= y0 && ybr[i] <= y1) ybr[n++] = ybr[i];
        nybr = n;
        int dec = axis_decimals(ybr, nybr);
        for (int i = 0; i < nybr; i++) { ylabs[i] = malloc(32); fmt_break(ybr[i], dec, ylabs[i]); }
    }
    double *xnpc = malloc(nxbr * sizeof(double)), *ynpc = malloc(nybr * sizeof(double));
    for (int i = 0; i < nxbr; i++) xnpc[i] = NPCX(xbr[i]);
    for (int i = 0; i < nybr; i++) ynpc[i] = NPCY(ybr[i]);
    double xmin_br[32], ymin_br[32];
    int nxmin = disc_x ? 0 : make_minors(xbr, nxbr, x0, x1, xmin_br);
    int nymin = make_minors(ybr, nybr, y0, y1, ymin_br);

    /* ---- geom_col bar width: 0.9 x min gap between distinct x ---- */
    double colw = 0.9;
    if (hascol) {
        double *xs = malloc(nuse * sizeof(double));
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
            int *seen = calloc(cf->nlev, sizeof(int)), cnt = 0;
            for (int r = 0; r < df->nrow; r++)
                if (use[r] && xf->idx[r] == cat && cf->idx[r] >= 0 && !seen[cf->idx[r]]) {
                    seen[cf->idx[r]] = 1; cnt++;
                }
            if (cnt > 1) box_dodge = 1;
            free(seen);
        }
    }
    int box_slots = box_dodge ? cf->nlev : 1;

#define SR(r) (3 + 3 * (r))
#define PR(r) (4 + 3 * (r))
#define PC(c) (4 + 2 * (c))

    /* ---- surface & measurement ---- */
    cairo_surface_t *surf = cairo_pdf_surface_create(out, w_pt, h_pt);
    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    double ylab_w = 0;
    for (int i = 0; i < nybr; i++) {
        double w = text_w(cr, SZ_AXIS_TEXT, ylabs[i]);
        if (w > ylab_w) ylab_w = w;
    }
    double labh = font_h(cr, SZ_AXIS_TEXT), baseh = font_h(cr, SZ_BASE);
    double striph = ff ? labh + 2 * STRIP_PAD : 0;

    const char *xtitle = spec->lab_x ? spec->lab_x : spec->x.expr;
    const char *ytitle = spec->lab_y ? spec->lab_y : (nhist ? "count" : spec->y.expr);

    Col *pal = NULL;
    GTable *leg = NULL;
    if (cf) {
        pal = malloc(cf->nlev * sizeof(Col));
        hue_palette(cf->nlev, pal);
        leg = build_legend(cr, spec->lab_colour ? spec->lab_colour : spec->colour.expr,
                           cf, pal, haspoint, hasline, hasbox);
    }

    /* ---- outer table ---- */
    GTable *T = calloc(1, sizeof(GTable));
    T->ncol = 2 * ncolp + 6;
    T->colw[0] = upt(MARGIN);
    T->colw[1] = upt(baseh);
    T->colw[2] = upt(HALF_LINE / 2);
    T->colw[3] = upt(ylab_w + TXT_GAP + TICK_LEN);
    for (int c = 0; c < ncolp; c++) {
        T->colw[PC(c)] = unull(1);
        if (c < ncolp - 1) T->colw[PC(c) + 1] = upt(PANEL_SPACE);
    }
    T->colw[T->ncol - 3] = upt(leg ? 2 * HALF_LINE : 0);
    T->colw[T->ncol - 2] = upt(leg ? gt_fixed_w(leg) : 0);
    T->colw[T->ncol - 1] = upt(MARGIN);

    T->nrow = 3 * nrowp + 6;
    T->rowh[0] = upt(MARGIN);
    T->rowh[1] = upt(spec->lab_title ? font_h(cr, SZ_TITLE) : 0);
    T->rowh[2] = upt(spec->lab_title ? HALF_LINE : 0);
    for (int r = 0; r < nrowp; r++) {
        T->rowh[SR(r)] = upt(striph);
        T->rowh[PR(r)] = unull(1);
        if (r < nrowp - 1) T->rowh[PR(r) + 1] = upt(PANEL_SPACE);
    }
    int r_axis = 3 * nrowp + 2;
    T->rowh[r_axis]     = upt(TICK_LEN + TXT_GAP + labh);
    T->rowh[r_axis + 1] = upt(HALF_LINE / 2);
    T->rowh[r_axis + 2] = upt(baseh);
    T->rowh[r_axis + 3] = upt(MARGIN);

    /* ---- static text & legend ---- */
    Grob *g;
    if (spec->lab_title) {
        g = gt_add(T, G_TEXT, 1, PC(0), 1, PC(ncolp - 1));
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = C_BLACK;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    g = gt_add(T, G_TEXT, r_axis + 2, PC(0), r_axis + 2, PC(ncolp - 1));
    g->str = xtitle; g->size = SZ_BASE; g->col = C_BLACK;
    g->tx = 0.5; g->ty = 1; g->hj = 0.5; g->va = V_TOP;
    g = gt_add(T, G_TEXT, PR(0), 1, PR(nrowp - 1), 1);
    g->str = ytitle; g->size = SZ_BASE; g->col = C_BLACK;
    g->tx = 0.5; g->ty = 0.5; g->rot90 = 1;
    if (leg) {
        g = gt_add(T, G_TABLE, SR(0), T->ncol - 2, PR(nrowp - 1), T->ncol - 2);
        g->child = leg;
    }

    /* ---- panels ---- */
    for (int p = 0; p < npan; p++) {
        int pr = p / ncolp, pc = p % ncolp;
        int R = PR(pr), C = PC(pc);

        if (ff) {
            g = gt_add(T, G_RECT, SR(pr), C, SR(pr), C); g->col = C_STRIP;
            g = gt_add(T, G_TEXT, SR(pr), C, SR(pr), C);
            g->str = ff->levels[p]; g->size = SZ_AXIS_TEXT; g->col = C_STRIPTXT;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }

        g = gt_add(T, G_RECT, R, C, R, C); g->col = C_PANEL;
        for (int i = 0; i < nxmin; i++) {
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_WHITE; g->lw = lw_pt(0.25); g->clip = 1;
            g->x0 = g->x1 = NPCX(xmin_br[i]); g->y0 = 0; g->y1 = 1;
        }
        for (int i = 0; i < nymin; i++) {
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_WHITE; g->lw = lw_pt(0.25); g->clip = 1;
            g->y0 = g->y1 = NPCY(ymin_br[i]); g->x0 = 0; g->x1 = 1;
        }
        for (int i = 0; i < nxbr; i++) {
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_WHITE; g->lw = lw_pt(0.5); g->clip = 1;
            g->x0 = g->x1 = xnpc[i]; g->y0 = 0; g->y1 = 1;
        }
        for (int i = 0; i < nybr; i++) {
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_WHITE; g->lw = lw_pt(0.5); g->clip = 1;
            g->y0 = g->y1 = ynpc[i]; g->x0 = 0; g->x1 = 1;
        }

        /* layers, in spec order */
        for (int li = 0; li < spec->nlayers; li++) {
            GeomType gt = spec->layers[li].type;
            if (gt == GEOM_HISTOGRAM) {
                Hist *hs = &hist[li];
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int b = 0; b < hs->nbins; b++) {
                    int cnt = hs->counts[p * hs->nbins + b];
                    if (!cnt) continue;
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = C_BAR; g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(hs->start + b * hs->width);
                    g->x1 = NPCX(hs->start + (b + 1) * hs->width);
                    g->y0 = base;
                    g->y1 = NPCY(spec->log_y ? log10((double)cnt) : (double)cnt);
                }
            } else if (gt == GEOM_COL) {
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    double tx = TXR(r), ty = TY(yc->num[r]);
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = C_BAR; g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(tx - colw / 2); g->x1 = NPCX(tx + colw / 2);
                    g->y0 = fmin(base, NPCY(ty)); g->y1 = fmax(base, NPCY(ty));
                }
            } else if (gt == GEOM_POINT) {
                int np = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p)) np++;
                double *px = malloc(np * sizeof(double)), *py = malloc(np * sizeof(double));
                Col *pcol = malloc(np * sizeof(Col));
                np = 0;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    px[np] = NPCX(TXR(r)); py[np] = NPCY(TY(yc->num[r]));
                    pcol[np] = cf ? pal[cf->idx[r]] : C_BLACK;
                    np++;
                }
                g = gt_add(T, G_POINTS, R, C, R, C);
                g->n = np; g->px = px; g->py = py; g->pcol = pcol;
                g->radius = PT_RADIUS; g->clip = 1;
            } else if (gt == GEOM_LINE) {
                int ngrp = cf ? cf->nlev : 1;
                for (int grp = 0; grp < ngrp; grp++) {
                    int np = 0;
                    for (int r = 0; r < df->nrow; r++)
                        if (use[r] && (!ff || ff->idx[r] == p)
                                   && (!cf || cf->idx[r] == grp)) np++;
                    if (np < 2) continue;
                    Pt *pts = malloc(np * sizeof(Pt));
                    np = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)
                                    || (cf && cf->idx[r] != grp)) continue;
                        pts[np].x = NPCX(TXR(r));
                        pts[np].y = NPCY(TY(yc->num[r]));
                        np++;
                    }
                    qsort(pts, np, sizeof(Pt), cmp_pt_x);
                    double *px = malloc(np * sizeof(double)), *py = malloc(np * sizeof(double));
                    for (int i = 0; i < np; i++) { px[i] = pts[i].x; py[i] = pts[i].y; }
                    free(pts);
                    g = gt_add(T, G_POLYLINE, R, C, R, C);
                    g->n = np; g->px = px; g->py = py;
                    g->col = cf ? pal[grp] : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
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
                        double *ys = malloc(ny * sizeof(double));
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
                        Col lc = cf ? pal[box_slots > 1 ? s : anyg] : C_TICK;

                        for (int w = 0; w < 2; w++) {    /* whiskers */
                            g = gt_add(T, G_LINE, R, C, R, C);
                            g->col = lc; g->lw = lw_pt(0.5); g->clip = 1;
                            g->x0 = g->x1 = xm;
                            g->y0 = NPCY(w ? b.q1 : b.q3); g->y1 = NPCY(w ? b.wlo : b.whi);
                        }
                        g = gt_add(T, G_RECT, R, C, R, C);   /* white fill */
                        g->col = C_WHITE; g->sub = 1; g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = NPCY(b.q1); g->y1 = NPCY(b.q3);
                        g = gt_add(T, G_RECT, R, C, R, C);   /* box outline */
                        g->col = lc; g->sub = 1; g->stroke = 1; g->lw = lw_pt(0.5); g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = NPCY(b.q1); g->y1 = NPCY(b.q3);
                        g = gt_add(T, G_LINE, R, C, R, C);   /* median (fatten 2) */
                        g->col = lc; g->lw = lw_pt(1.0); g->clip = 1;
                        g->x0 = xl; g->x1 = xr; g->y0 = g->y1 = NPCY(b.med);

                        int nout = 0;
                        for (int i = 0; i < ny; i++) if (ys[i] > b.whi || ys[i] < b.wlo) nout++;
                        if (nout) {
                            double *ox = malloc(nout * sizeof(double)), *oy = malloc(nout * sizeof(double));
                            Col *oc = malloc(nout * sizeof(Col));
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
            }
        }

        if (pc == 0) {
            g = gt_add(T, G_AXIS_Y, R, 3, R, 3);
            g->n = nybr; g->py = ynpc; g->labels = ylabs;
        }
    }

    /* x axes: under the bottom-most panel of each column */
    for (int c = 0; c < ncolp && c < npan; c++) {
        int rb = (npan - 1 - c) / ncolp;
        if (rb == nrowp - 1)
            g = gt_add(T, G_AXIS_X, r_axis, PC(c), r_axis, PC(c));
        else
            g = gt_add(T, G_AXIS_X, PR(rb) + 1, PC(c), PR(rb + 1), PC(c));
        g->n = nxbr; g->px = xnpc; g->labels = xlabs;
    }

    /* ---- go ---- */
    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_status_t st = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        sprintf(err, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
