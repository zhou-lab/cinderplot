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
                            const Col *pal, int haspoint, int hasline, int hasbox, int hastext) {
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
    g->str = title; g->size = SZ_BASE; g->col = th->title;
    g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    static const double half = 0.5;
    for (int i = 0; i < f->nlev; i++) {
        int r = 2 + 2 * i;
        if (th->key_bg_on) { g = gt_add(t, G_RECT, r, 0, r, 0); g->col = th->key_bg; }
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
        if (hastext) {                       /* geom_text/geom_label key: a letter */
            g = gt_add(t, G_TEXT, r, 0, r, 0);
            g->str = "a"; g->size = SZ_AXIS_TEXT; g->col = pal[i];
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }
        g = gt_add(t, G_TEXT, r, 2, r, 2);
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
        char b[32]; fmt_break(br[i], dec, b);
        double w = text_w(cr, SZ_AXIS_TEXT, b);
        if (w > labw) labw = w;
    }
    double barcol = BARW + TICK_LEN + TXT_GAP + labw;
    double titlew = title ? text_w(cr, SZ_BASE, title) : 0;
    double w = fmax(barcol, titlew);

    GTable *t = calloc(1, sizeof(GTable));
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
        char *lab = malloc(32); fmt_break(br[i], dec, lab);
        g = gt_add(t, G_LINE, 2, 0, 2, 0);
        g->col = th->tick; g->lw = lw_pt(0.5);
        g->x0 = barw_npc; g->x1 = barw_npc + TICK_LEN / w; g->y0 = g->y1 = frac;
        g = gt_add(t, G_TEXT, 2, 0, 2, 0);
        g->str = lab; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = (BARW + TICK_LEN + TXT_GAP) / w; g->ty = frac; g->hj = 0; g->va = V_INKCENTER;
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

/* log10 minor breaks: d x 10^k for d in 2..9, in transformed (log10) space,
 * filtered to [lo, hi] — the characteristic log grid. Majors are drawn on top,
 * so a minor coinciding with a major is simply covered. */
static int log_minors(double lo, double hi, double *out, int max_out) {
    int n = 0;
    for (int k = (int)floor(lo) - 1; k <= (int)ceil(hi) + 1 && n < max_out; k++)
        for (int d = 2; d <= 9 && n < max_out; d++) {
            double t = k + log10((double)d);
            if (t >= lo - 1e-9 && t <= hi + 1e-9) out[n++] = t;
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
        sprintf(err, "seqinfo `%s` needs a text `chrom` and numeric `length` column", path);
        return NULL;
    }
    GenomeScale *g = malloc(sizeof *g);
    g->n = sq->nrow;
    g->chr = malloc(g->n * sizeof(char *));
    g->off = malloc(g->n * sizeof(double));
    g->len = malloc(g->n * sizeof(double));
    double cum = 0;
    for (int i = 0; i < g->n; i++) {
        g->chr[i] = sc->str[i]; g->len[i] = lc->num[i]; g->off[i] = cum; cum += g->len[i];
    }
    g->total = cum;
    return g;
}
static double genome_off(const GenomeScale *g, const char *chr) {
    for (int i = 0; i < g->n; i++) if (!strcmp(g->chr[i], chr)) return g->off[i];
    return -1;   /* sentinel: chromosome absent from seqinfo */
}

/* cytoband gieStain -> colour: grey ramp for gpos*, red centromere */
static Col stain_color(const char *s) {
    if (!strcmp(s, "acen")) { Col c = {0.878, 0, 0}; return c; }      /* #E00000 */
    if (!strcmp(s, "gneg")) return C_WHITE;
    if (!strcmp(s, "gpos25")) { Col c = {0.753, 0.753, 0.753}; return c; }
    if (!strcmp(s, "gpos50")) { Col c = {0.565, 0.565, 0.565}; return c; }
    if (!strcmp(s, "gpos75")) { Col c = {0.376, 0.376, 0.376}; return c; }
    if (!strcmp(s, "gpos100")) { Col c = {0, 0, 0}; return c; }
    Col c = {0.502, 0.502, 0.502}; return c;                          /* gvar/stalk */
}

int render_plot(const PlotSpec *spec, const DataFrame *df, const char *out,
                double w_pt, double h_pt, char *err) {
    /* ---- layer summary ---- */
    int haspoint = 0, hasline = 0, hascol = 0, nhist = 0, hasbox = 0, hasbar = 0, hasdens = 0, hastext = 0;
    for (int i = 0; i < spec->nlayers; i++) {
        if (spec->layers[i].type == GEOM_POINT) haspoint = 1;
        if (spec->layers[i].type == GEOM_LINE) hasline = 1;
        if (spec->layers[i].type == GEOM_COL) hascol = 1;
        if (spec->layers[i].type == GEOM_HISTOGRAM) nhist++;
        if (spec->layers[i].type == GEOM_BOXPLOT) hasbox = 1;
        if (spec->layers[i].type == GEOM_BAR) hasbar = 1;
        if (spec->layers[i].type == GEOM_DENSITY) hasdens = 1;
        if (spec->layers[i].type == GEOM_TEXT || spec->layers[i].type == GEOM_LABEL) hastext = 1;
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
    if (disc_x && hasdens) {
        sprintf(err, "geom_density() needs a continuous x"); return -1;
    }
    if (hasbox && !disc_x) {
        sprintf(err, "geom_boxplot() needs a discrete x; use aes(x=factor(%s), ...)", spec->x.col);
        return -1;
    }
    if (hasbar && !disc_x) {
        sprintf(err, "geom_bar() needs a discrete x; use aes(x=factor(%s), ...)", spec->x.col);
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
        if (!xec) { sprintf(err, "column `%s` not found", spec->xend.col); return -1; }
    }
    if (spec->yend.col) {
        yec = df_col(df, spec->yend.col);
        if (!yec) { sprintf(err, "column `%s` not found", spec->yend.col); return -1; }
    }
    if (hasseg && !xec && !disc_x) {
        sprintf(err, "geom_segment() needs aes(xend=...)"); return -1;
    }
    if (hasrect && !xec) {
        sprintf(err, "geom_rect() needs aes(xmin, xmax)"); return -1;
    }
    if (rect_top && !yec) {
        sprintf(err, "geom_rect() needs aes(ymin, ymax) (or data= for a full-height band)"); return -1;
    }
    /* genome coordinate x-scale: concatenate chromosomes via seqinfo offsets */
    int genome_x = spec->genome_seqinfo != NULL;
    GenomeScale *gs = NULL;
    double *roff = NULL;             /* per-row genome offset (-1 = drop) */
    if (genome_x) {
        if (disc_x || spec->log_x) {
            sprintf(err, "scale_x_genome() needs a continuous, non-log x"); return -1;
        }
        if (!spec->chrom.col) {
            sprintf(err, "scale_x_genome() needs a chromosome column: aes(chrom=...)"); return -1;
        }
        if (!(gs = genome_load(spec->genome_seqinfo, err))) return -1;
        const Column *cc = df_col(df, spec->chrom.col);
        if (!cc || cc->type != COL_STR) {
            sprintf(err, "chrom column `%s` must be text", spec->chrom.col); return -1;
        }
        roff = malloc(df->nrow * sizeof(double));
        for (int r = 0; r < df->nrow; r++) roff[r] = genome_off(gs, cc->str[r]);
    }
    Factor *cf = NULL;
    const Column *colc = NULL;          /* continuous colour column */
    int cont_col = 0;
    FillScale cscale = spec->colour_scale;
    if (spec->colour.col) {
        if (hascol || nhist) {
            sprintf(err, "colour/fill on bars (stacking/dodging) is not implemented yet");
            return -1;
        }
        const Column *cc = df_col(df, spec->colour.col);
        if (!cc) { sprintf(err, "column `%s` not found", spec->colour.col); return -1; }
        if (!spec->colour.is_factor && cc->type == COL_NUM) {
            cont_col = 1; colc = cc;    /* continuous colour aesthetic */
            if (!spec->has_colour_scale) {     /* ggplot default: blue gradient */
                cscale.kind = FILL_GRADIENT;
                parse_color("#132B43", &cscale.low);
                parse_color("#56B1F7", &cscale.high);
            }
        } else {
            cf = factor_make(df, cc);
        }
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
        int xok = disc_x ? (xf->idx[r] >= 0)
                : genome_x ? (roff[r] >= 0 && !isnan(xc->num[r]))
                : !isnan(xc->num[r]);
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

#define TY(v) (spec->log_y ? log10(v) : (v))
/* transformed x for row r: category position (discrete), genome offset+pos
 * (genome), or raw value (continuous) */
#define XVAL(r) (disc_x ? (double)(xf->idx[r] + 1) \
               : genome_x ? (roff[r] + xc->num[r]) : xc->num[r])
#define TXR(r)  (spec->log_x ? log10(XVAL(r)) : XVAL(r))
/* genome offset applied to any within-chromosome position (e.g. xend) */
#define GX(r, v) (genome_x ? (roff[r] + (v)) : (v))

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
                double te = spec->log_x ? log10(xec->num[r]) : xec->num[r];
                if (te < txmin) txmin = te;
                if (te > txmax) txmax = te;
            }
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

    /* ---- stat_count for geom_bar: counts per (panel, x-category, group) ---- */
    int barng = cf ? cf->nlev : 1;
    int *barcount = NULL, barmax = 0;
    if (hasbar) {
        barcount = calloc((size_t)npan * xf->nlev * barng, sizeof(int));
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

    /* ---- stat_density: Gaussian KDE per (panel, colour group), bandwidth
     * nrd0 (Silverman), evaluated at DENS_N points over [min-3bw, max+3bw]
     * (ggplot's cut=3). The x-scale stays on the data range (ggplot-style). --- */
#define DENS_N 512
    int densg = cf ? cf->nlev : 1;
    double *dens_x = NULL, *dens_y = NULL, dens_max = 0;
    if (hasdens) {
        const Layer *densl = NULL;
        for (int li = 0; li < spec->nlayers; li++)
            if (spec->layers[li].type == GEOM_DENSITY) densl = &spec->layers[li];
        dens_x = malloc((size_t)npan * densg * DENS_N * sizeof(double));
        dens_y = malloc((size_t)npan * densg * DENS_N * sizeof(double));
        double *buf = malloc((size_t)df->nrow * sizeof(double));
        for (int p = 0; p < npan; p++)
            for (int gg = 0; gg < densg; gg++) {
                int n = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p) && (!cf || cf->idx[r] == gg))
                        buf[n++] = TXR(r);
                size_t base = ((size_t)(p * densg + gg)) * DENS_N;
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
        free(buf);
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
    } else if (hasbar) {
        tymin = 0; tymax = spec->log_y ? log10((double)barmax) : (double)barmax;
    } else if (hasdens) {
        tymin = 0; tymax = dens_max;
    } else {
        for (int r = 0; r < df->nrow; r++) {
            if (!use[r]) continue;
            double t = TY(yc->num[r]);
            if (t < tymin) tymin = t;
            if (t > tymax) tymax = t;
            if (yec && !isnan(yec->num[r])) {   /* segment end extends y range */
                double te = TY(yec->num[r]);
                if (te < tymin) tymin = te;
                if (te > tymax) tymax = te;
            }
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
            double t = spec->log_x ? log10(L->intercept) : L->intercept;
            if (t < txmin) txmin = t;
            if (t > txmax) txmax = t;
        }
    }

    /* user axis limits (xlim/ylim or scale_*_log10(limits=)): override the
     * data-driven range with the requested domain (log10-transformed when the
     * axis is log). Default expansion is applied below as usual. */
    if (spec->has_xlim && !disc_x && !genome_x) {
        txmin = spec->log_x ? log10(spec->xlim_lo) : spec->xlim_lo;
        txmax = spec->log_x ? log10(spec->xlim_hi) : spec->xlim_hi;
    }
    if (spec->has_ylim) {
        tymin = spec->log_y ? log10(spec->ylim_lo) : spec->ylim_lo;
        tymax = spec->log_y ? log10(spec->ylim_hi) : spec->ylim_hi;
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
    double y0 = tymin - 0.05 * (tymax - tymin), y1 = tymax + 0.05 * (tymax - tymin);
    /* reserve the bottom `ideo_npc` of the panel for the ideogram track */
    double ideo_npc = (spec->ideogram_path && genome_x) ? 0.06 : 0;
    if (ideo_npc > 0) y0 -= (y1 - y0) * ideo_npc / (1 - ideo_npc);
#define NPCX(t) (((t) - x0) / (x1 - x0))
#define NPCY(t) (((t) - y0) / (y1 - y0))

    double xbr[40], ybr[16];
    char **xlabs = malloc(40 * sizeof(char *)), **ylabs = malloc(16 * sizeof(char *));
    int nxbr, nybr;
    /* genome mode uses separate axis arrays: gridlines at chrom boundaries,
     * labels (chrom names) at chrom midpoints */
    double *gax_pos = NULL; char **gax_lab = NULL; int gax_n = 0;
    if (disc_x) {                          /* one break per category, level labels */
        nxbr = xf->nlev;
        for (int i = 0; i < nxbr; i++) { xbr[i] = i + 1; xlabs[i] = strdup(xf->levels[i]); }
    } else if (genome_x) {
        nxbr = gs->n > 1 ? gs->n - 1 : 0;  /* internal boundaries = faint gridlines */
        for (int i = 0; i < nxbr; i++) { xbr[i] = gs->off[i + 1]; xlabs[i] = strdup(""); }
        gax_n = gs->n;
        gax_pos = malloc(gax_n * sizeof(double));
        gax_lab = malloc(gax_n * sizeof(char *));
        for (int i = 0; i < gs->n; i++) {
            gax_pos[i] = NPCX(gs->off[i] + gs->len[i] / 2);
            const char *nm = gs->chr[i];
            if (!strncmp(nm, "chr", 3)) nm += 3;   /* compact: chr1 -> 1 */
            gax_lab[i] = strdup(nm);
        }
    } else if (spec->log_x) {
        nxbr = log10_breaks(x0, x1, xbr, xlabs, 16);
    } else {
        int nb = extended_breaks(x0, x1, 5, xbr, 16), n = 0;
        for (int i = 0; i < nb; i++) if (xbr[i] >= x0 && xbr[i] <= x1) xbr[n++] = xbr[i];
        nxbr = n;
        int dec = axis_decimals(xbr, nxbr), pdec = dec - 2 < 0 ? 0 : dec - 2;
        for (int i = 0; i < nxbr; i++) {
            xlabs[i] = malloc(32);
            if (spec->x_pct) sprintf(xlabs[i], "%.*f%%", pdec, xbr[i] * 100);
            else fmt_break(xbr[i], dec, xlabs[i]);
        }
    }
    if (spec->log_y) {
        nybr = log10_breaks(y0, y1, ybr, ylabs, 16);
    } else {
        int nb = extended_breaks(y0, y1, 5, ybr, 16), n = 0;
        for (int i = 0; i < nb; i++) if (ybr[i] >= y0 && ybr[i] <= y1) ybr[n++] = ybr[i];
        nybr = n;
        int dec = axis_decimals(ybr, nybr), pdec = dec - 2 < 0 ? 0 : dec - 2;
        for (int i = 0; i < nybr; i++) {
            ylabs[i] = malloc(32);
            if (spec->y_pct) sprintf(ylabs[i], "%.*f%%", pdec, ybr[i] * 100);
            else fmt_break(ybr[i], dec, ylabs[i]);
        }
    }
    double *xnpc = malloc(nxbr * sizeof(double)), *ynpc = malloc(nybr * sizeof(double));
    for (int i = 0; i < nxbr; i++) xnpc[i] = NPCX(xbr[i]);
    for (int i = 0; i < nybr; i++) ynpc[i] = NPCY(ybr[i]);
    double xmin_br[32], ymin_br[32];
    int nxmin = (disc_x || genome_x) ? 0
              : spec->log_x ? log_minors(x0, x1, xmin_br, 32)
              : make_minors(xbr, nxbr, x0, x1, xmin_br);
    int nymin = spec->log_y ? log_minors(y0, y1, ymin_br, 32)
              : make_minors(ybr, nybr, y0, y1, ymin_br);

    /* log tick marks (ggplot annotation_logticks): 1..9 x 10^k drawn INSIDE the
     * panel from the axis edge inward — long at decades, mid at 5, short at the
     * rest. Lengths in points (ggplot defaults 0.3/0.2/0.1 cm), npc at draw. */
    const double CM_PT = 72.0 / 2.54;
    const double LT_LONG = 0.30 * CM_PT, LT_MID = 0.20 * CM_PT, LT_SHORT = 0.10 * CM_PT;
    double xlt_pos[80], xlt_len[80]; int xlt_n = 0;
    double ylt_pos[80], ylt_len[80]; int ylt_n = 0;
    if (spec->log_x)
        for (int k = (int)floor(x0) - 1; k <= (int)ceil(x1) + 1 && xlt_n < 80; k++)
            for (int d = 1; d <= 9 && xlt_n < 80; d++) {
                double t = k + log10((double)d);
                if (t >= x0 - 1e-9 && t <= x1 + 1e-9) {
                    xlt_pos[xlt_n] = NPCX(t);
                    xlt_len[xlt_n] = d == 1 ? LT_LONG : d == 5 ? LT_MID : LT_SHORT; xlt_n++;
                }
            }
    if (spec->log_y)
        for (int k = (int)floor(y0) - 1; k <= (int)ceil(y1) + 1 && ylt_n < 80; k++)
            for (int d = 1; d <= 9 && ylt_n < 80; d++) {
                double t = k + log10((double)d);
                if (t >= y0 - 1e-9 && t <= y1 + 1e-9) {
                    ylt_pos[ylt_n] = NPCY(t);
                    ylt_len[ylt_n] = d == 1 ? LT_LONG : d == 5 ? LT_MID : LT_SHORT; ylt_n++;
                }
            }

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
    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    const Theme *th = &THEMES[spec->theme];   /* active theme (THEME_GRAY = default) */

    double ylab_w = 0;
    for (int i = 0; i < nybr; i++) {
        double w = cp_label_w(cr, SZ_AXIS_TEXT, ylabs[i]);   /* superscript-aware */
        if (w > ylab_w) ylab_w = w;
    }
    double labh = font_h(cr, SZ_AXIS_TEXT), baseh = font_h(cr, SZ_BASE);
    double striph = ff ? labh + 2 * STRIP_PAD : 0;

    const char *xtitle = spec->lab_x ? spec->lab_x : genome_x ? "" : spec->x.expr;
    const char *ytitle = spec->lab_y ? spec->lab_y
                       : nhist || hasbar ? "count" : hasdens ? "density" : spec->y.expr;

    Col *pal = NULL;
    GTable *leg = NULL;
    const char *col_title = spec->lab_colour ? spec->lab_colour : spec->colour.expr;
    if (cf) {
        pal = malloc(cf->nlev * sizeof(Col));
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
        } else hue_palette(cf->nlev, pal);
        leg = build_legend(cr, th, col_title, cf, pal, haspoint,
                           hasline || hasseg || hasdens, hasbox || hasbar || hasrect, hastext);
    } else if (cont_col) {
        leg = build_colorbar_legend(cr, th, col_title, &cscale, cdmin, cdmax);
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
    T->rowh[2] = upt(spec->lab_subtitle ? font_h(cr, SZ_BASE)
                   : spec->lab_title ? HALF_LINE : 0);
    for (int r = 0; r < nrowp; r++) {
        T->rowh[SR(r)] = upt(striph);
        T->rowh[PR(r)] = unull(1);
        if (r < nrowp - 1) T->rowh[PR(r) + 1] = upt(PANEL_SPACE);
    }
    int r_axis = 3 * nrowp + 2;
    T->rowh[r_axis]     = upt(TICK_LEN + TXT_GAP + labh);
    T->rowh[r_axis + 1] = upt(HALF_LINE / 2);
    T->rowh[r_axis + 2] = upt(baseh);
    T->rowh[r_axis + 3] = upt(spec->lab_caption ? fmax(MARGIN, font_h(cr, SZ_AXIS_TEXT)) : MARGIN);

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
        g = gt_add(T, G_TEXT, r_axis + 3, PC(0), r_axis + 3, PC(ncolp - 1));
        g->str = spec->lab_caption; g->size = SZ_AXIS_TEXT; g->col = th->title;
        g->tx = 1; g->ty = 1; g->hj = 1; g->va = V_TOP;
    }
    if (th->axis_title_on) {
        g = gt_add(T, G_TEXT, r_axis + 2, PC(0), r_axis + 2, PC(ncolp - 1));
        g->str = xtitle; g->size = SZ_BASE; g->col = th->axis_title;
        g->tx = 0.5; g->ty = 1; g->hj = 0.5; g->va = V_TOP;
        g = gt_add(T, G_TEXT, PR(0), 1, PR(nrowp - 1), 1);
        g->str = ytitle; g->size = SZ_BASE; g->col = th->axis_title;
        g->tx = 0.5; g->ty = 0.5; g->rot90 = 1;
    }
    if (leg) {
        g = gt_add(T, G_TABLE, SR(0), T->ncol - 2, PR(nrowp - 1), T->ncol - 2);
        g->child = leg;
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

        if (ff) {
            if (th->strip_bg_on) { g = gt_add(T, G_RECT, SR(pr), C, SR(pr), C); g->col = th->strip_bg; }
            g = gt_add(T, G_TEXT, SR(pr), C, SR(pr), C);
            g->str = ff->levels[p]; g->size = SZ_AXIS_TEXT; g->col = th->strip_text;
            g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
        }

        if (th->panel_bg_on) { g = gt_add(T, G_RECT, R, C, R, C); g->col = th->panel_bg; }
        if (th->grid_minor_on) {
            for (int i = 0; i < nxmin; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_minor; g->lw = lw_pt(th->grid_minor_lw); g->clip = 1;
                g->x0 = g->x1 = NPCX(xmin_br[i]); g->y0 = 0; g->y1 = 1;
            }
            for (int i = 0; i < nymin; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_minor; g->lw = lw_pt(th->grid_minor_lw); g->clip = 1;
                g->y0 = g->y1 = NPCY(ymin_br[i]); g->x0 = 0; g->x1 = 1;
            }
        }
        if (th->grid_major_on) {
            for (int i = 0; i < nxbr; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_major; g->lw = lw_pt(th->grid_major_lw); g->clip = 1;
                g->x0 = g->x1 = xnpc[i]; g->y0 = 0; g->y1 = 1;
            }
            for (int i = 0; i < nybr; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = th->grid_major; g->lw = lw_pt(th->grid_major_lw); g->clip = 1;
                g->y0 = g->y1 = ynpc[i]; g->x0 = 0; g->x1 = 1;
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
            for (int i = 0; i < xlt_n; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = g->x1 = xlt_pos[i]; g->y0 = 0; g->y1 = xlt_len[i] / panelh_pt;
            }
        if (spec->log_y && panelw_pt > 0)
            for (int i = 0; i < ylt_n; i++) {
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
                g->y0 = g->y1 = ylt_pos[i]; g->x0 = 0; g->x1 = ylt_len[i] / panelw_pt;
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
            } else if (gt == GEOM_DENSITY) {
                for (int gg = 0; gg < densg; gg++) {
                    size_t bse = ((size_t)(p * densg + gg)) * DENS_N;
                    double *px = malloc(DENS_N * sizeof(double));
                    double *py = malloc(DENS_N * sizeof(double));
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
            } else if (gt == GEOM_COL) {
                double base = spec->log_y ? 0.0 : NPCY(0.0);
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    double tx = TXR(r), ty = TY(yc->num[r]);
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                    g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(tx - colw / 2); g->x1 = NPCX(tx + colw / 2);
                    g->y0 = fmin(base, NPCY(ty)); g->y1 = fmax(base, NPCY(ty));
                }
            } else if (gt == GEOM_BAR) {
                /* stat_count bars, width 0.9, stacked by colour group with
                 * the last factor level at the bottom (ggplot position_stack) */
                for (int cat = 0; cat < xf->nlev; cat++) {
                    double xi = cat + 1, cum = 0;
                    for (int grp = barng - 1; grp >= 0; grp--) {
                        int cnt = barcount[((size_t)(p * xf->nlev + cat)) * barng + grp];
                        if (!cnt) continue;
                        g = gt_add(T, G_RECT, R, C, R, C);
                        g->col = cf ? pal[grp] : spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                        g->sub = 1; g->clip = 1;
                        g->x0 = NPCX(xi - 0.45); g->x1 = NPCX(xi + 0.45);
                        g->y0 = NPCY(cum); g->y1 = NPCY(cum + cnt);
                        cum += cnt;
                    }
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
                    pcol[np] = spec->layers[li].has_color ? spec->layers[li].color
                             : cf ? pal[cf->idx[r]] : cont_col ? CCOL(r) : C_BLACK;
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
                    g->col = spec->layers[li].has_color ? spec->layers[li].color
                           : cf ? pal[grp] : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                }
            } else if (gt == GEOM_SEGMENT && spec->layers[li].data) {
                /* per-layer data (e.g. CBS segments): its own file, genome-
                 * offset horizontal lines from start..end at y */
                const Layer *L = &spec->layers[li];
                DataFrame *d2 = df_read_csv(L->data, err);
                if (!d2) return -1;
                const Column *c_chr = genome_x ? df_col(d2, spec->chrom.col) : NULL;
                const Column *c_x = df_col(d2, spec->x.col);
                const Column *c_xe = spec->xend.col ? df_col(d2, spec->xend.col) : NULL;
                const Column *c_y = df_col(d2, L->ycol ? L->ycol : spec->y.col);
                if (!c_x || !c_y || (genome_x && !c_chr)) {
                    sprintf(err, "geom_segment(data=%s): missing chrom/x/y column", L->data);
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
                                      : spec->log_x ? log10(xec->num[r]) : xec->num[r])
                                     : TXR(r));
                    g->y0 = NPCY(TY(yc->num[r]));
                    g->y1 = NPCY(yec ? TY(yec->num[r]) : TY(yc->num[r]));
                }
            } else if (gt == GEOM_RECT && spec->layers[li].data) {
                /* region-highlight bands: own file, full panel height,
                 * genome-offset x. Drawn in layer order (put before the
                 * points to shade behind them) */
                const Layer *L = &spec->layers[li];
                DataFrame *d2 = df_read_csv(L->data, err);
                if (!d2) return -1;
                const Column *c_chr = genome_x ? df_col(d2, spec->chrom.col) : NULL;
                const Column *c_x = df_col(d2, spec->x.col);
                const Column *c_xe = spec->xend.col ? df_col(d2, spec->xend.col) : NULL;
                if (!c_x || !c_xe || (genome_x && !c_chr)) {
                    sprintf(err, "geom_rect(data=%s): needs chrom/xmin/xmax columns", L->data);
                    return -1;
                }
                Col fixed = L->has_color ? L->color : (Col){0.85, 0.85, 0.85};
                for (int r2 = 0; r2 < d2->nrow; r2++) {
                    double off = genome_x ? genome_off(gs, c_chr->str[r2]) : 0;
                    if ((genome_x && off < 0) || isnan(c_x->num[r2]) || isnan(c_xe->num[r2])) continue;
                    g = gt_add(T, G_RECT, R, C, R, C);
                    g->col = fixed; g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(off + c_x->num[r2]); g->x1 = NPCX(off + c_xe->num[r2]);
                    g->y0 = 0; g->y1 = 1;         /* full panel height */
                }
            } else if (gt == GEOM_RECT) {
                /* filled rectangle per row: (xmin,ymin) .. (xmax,ymax) */
                Col fixed = spec->layers[li].has_color ? spec->layers[li].color : C_BAR;
                for (int r = 0; r < df->nrow; r++) {
                    if (!use[r] || (ff && ff->idx[r] != p)) continue;
                    if (isnan(xec->num[r]) || isnan(yec->num[r])) continue;
                    double a = NPCX(TXR(r));
                    double b = NPCX(genome_x ? GX(r, xec->num[r])
                                  : spec->log_x ? log10(xec->num[r]) : xec->num[r]);
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
            } else if (gt == GEOM_HLINE) {
                const Layer *L = &spec->layers[li];
                if (L->has_intercept) {
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = NPCY(TY(L->intercept));
                }
            } else if (gt == GEOM_VLINE) {
                const Layer *L = &spec->layers[li];
                if (L->has_intercept) {
                    double xt = spec->log_x ? log10(L->intercept) : L->intercept;
                    g = gt_add(T, G_LINE, R, C, R, C);
                    g->col = L->has_color ? L->color : C_BLACK;
                    g->lw = lw_pt(0.5); g->clip = 1;
                    g->y0 = 0; g->y1 = 1; g->x0 = g->x1 = NPCX(xt);
                }
            } else if (gt == GEOM_ABLINE) {
                const Layer *L = &spec->layers[li];
                double xl = spec->log_x ? pow(10, x0) : x0;   /* data-space edges */
                double xr = spec->log_x ? pow(10, x1) : x1;
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = L->has_color ? L->color : C_BLACK;
                g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = 0; g->x1 = 1;
                g->y0 = NPCY(TY(L->intercept + L->slope * xl));
                g->y1 = NPCY(TY(L->intercept + L->slope * xr));
            } else if (gt == GEOM_TEXT || gt == GEOM_LABEL) {
                const Layer *L = &spec->layers[li];
                double fs = (L->txt_size > 0 ? L->txt_size : 3.88) * 2.845276; /* mm -> pt */
                const Column *labc = df_col(df, spec->label.col);
                double ndx = x1 > x0 ? L->nudge_x / (x1 - x0) : 0;   /* data -> npc */
                double ndy = y1 > y0 ? L->nudge_y / (y1 - y0) : 0;
                int cap = 0;
                for (int r = 0; r < df->nrow; r++)
                    if (use[r] && (!ff || ff->idx[r] == p)
                        && !isnan(TXR(r)) && !isnan(yc->num[r])) cap++;
                if (cap > 0) {
                    RLabel *rl = malloc(cap * sizeof(RLabel));
                    const char **strs = malloc(cap * sizeof(char *));
                    Col *cols = malloc(cap * sizeof(Col));
                    double *px = malloc(cap * sizeof(double)), *py = malloc(cap * sizeof(double));
                    double bpad = (gt == GEOM_LABEL ? fs * 0.25 : 0) + PT_RADIUS * 0.6;
                    int m = 0;
                    for (int r = 0; r < df->nrow; r++) {
                        if (!use[r] || (ff && ff->idx[r] != p)) continue;
                        if (isnan(TXR(r)) || isnan(yc->num[r])) continue;
                        const char *s;
                        if (labc->type == COL_STR) s = labc->str[r];
                        else { char *tmp = malloc(32); fmt_num(labc->num[r], tmp); s = tmp; }
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
                        g->hj = 0.5; g->va = V_INKCENTER;
                        if (gt == GEOM_LABEL) {
                            g->text_box = 1; g->box_fill = C_WHITE; g->box_line = cols[i];
                        }
                    }
                    free(rl); free(strs); free(cols); free(px); free(py);
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
                sprintf(err, "ideogram cytoband needs chrom,start,end,stain columns"); return -1;
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

        if (pc == 0) {
            g = gt_add(T, G_AXIS_Y, R, 3, R, 3);
            g->n = nybr; g->py = ynpc; g->labels = ylabs;   /* log ticks are drawn inside the panel */
            g->axis_styled = 1; g->tick_col = th->tick; g->hide_ticks = !th->tick_on;
            g->text_col = th->axis_text; g->hide_text = !th->axis_text_on;
        }
    }

    /* x axes: under the bottom-most panel of each column */
    for (int c = 0; c < ncolp && c < npan; c++) {
        int rb = (npan - 1 - c) / ncolp;
        if (rb == nrowp - 1)
            g = gt_add(T, G_AXIS_X, r_axis, PC(c), r_axis, PC(c));
        else
            g = gt_add(T, G_AXIS_X, PR(rb) + 1, PC(c), PR(rb + 1), PC(c));
        if (genome_x) { g->n = gax_n; g->px = gax_pos; g->labels = gax_lab; }
        else {
            g->n = nxbr; g->px = xnpc; g->labels = xlabs;   /* log ticks are drawn inside the panel */
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
        sprintf(err, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
