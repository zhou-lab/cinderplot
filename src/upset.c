/* upset.c — an UpSet plot (UpSetR / ComplexHeatmap::UpSet): one column per
 * intersection, its size as a bar on top and its member sets as filled dots
 * in a set-by-intersection matrix below, joined by a vertical line.
 *
 *    n |  ▇
 *      |  ▇  ▅  ▃
 *      +-----------
 *  A   |  ●  ●  ·
 *  B   |  ●  ·  ●      (· = not a member, shaded every other row)
 *  C   |  ·  ●  ●
 *
 * Input is long: one row per (intersection, member set), with the size n
 * repeated on each of an intersection's rows and an optional fill= column
 * (one level per intersection) that colours its bar. Columns keep the file's
 * order (order="input", for a file already ranked) or sort by n
 * (order="n"); max= keeps the first N. set_size=TRUE adds UpSetR's set-size
 * bars left of the set names. Layout is worked out in natural points with y
 * down and mapped to the canvas at the end; a --size stretches the columns
 * and the bar panel to fill it. */
#include "cinderplot.h"
#include <math.h>
#include <string.h>

#define UP_MAXSET 64

typedef struct {
    const char *name;
    double n;
    int lev;                       /* fill level, -1 = none */
    unsigned char in[UP_MAXSET];   /* member of set s */
    int order;                     /* first-appearance rank (stable sort) */
} UInter;

static double tw(cairo_t *cr, double size, const char *s) {
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

static int by_n_desc(const void *a, const void *b) {
    const UInter *x = a, *y = b;
    if (x->n != y->n) return x->n < y->n ? 1 : -1;
    return x->order - y->order;
}

int render_upset(const PlotSpec *spec, const char *data, const char *out,
                 double w_pt, double h_pt, char *err) {
    DataFrame *df = df_read_csv(data ? data : "-", err);
    if (!df) return -1;
    if (df->nrow == 0) {
        snprintf(err, CP_ERRLEN, "upset(): %s has no rows", data ? data : "stdin");
        return -1;
    }
    const char *nm_i = spec->up_inter ? spec->up_inter : "intersection";
    const char *nm_s = spec->up_set ? spec->up_set : "set";
    const char *nm_n = spec->up_n ? spec->up_n : "n";
    const Column *ci = df_col(df, nm_i), *cs = df_col(df, nm_s), *cn = df_col(df, nm_n);
    const Column *cf = spec->up_fill ? df_col(df, spec->up_fill) : NULL;
    const char *miss = !ci ? nm_i : !cs ? nm_s : !cn ? nm_n
                     : spec->up_fill && !cf ? spec->up_fill : NULL;
    if (miss) {
        snprintf(err, CP_ERRLEN, "upset(): column `%s` not found (give intersection=, "
                 "set=, n=%s)", miss, spec->up_fill ? ", fill=" : "");
        return -1;
    }
    if (cs->type != COL_STR) {
        snprintf(err, CP_ERRLEN, "upset(): the set column `%s` must be text", cs->name);
        return -1;
    }
    if (cn->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "upset(): the n column `%s` must be numeric", cn->name);
        return -1;
    }
    if (cf && cf->type != COL_STR) {
        snprintf(err, CP_ERRLEN, "upset(): the fill column `%s` must be text (one level "
                 "per intersection)", cf->name);
        return -1;
    }

    /* ---- sets: sets=c(...) order, else first appearance ---- */
    const char *setn[UP_MAXSET]; int nset = 0;
    for (int i = 0; i < spec->n_up_sets; i++) {
        if (nset == UP_MAXSET) break;
        setn[nset++] = spec->up_sets[i];
    }
    for (int r = 0; r < df->nrow; r++) {
        const char *v = cs->str[r];
        int k = -1;
        for (int i = 0; i < nset; i++) if (!strcmp(setn[i], v)) { k = i; break; }
        if (k >= 0) continue;
        if (spec->n_up_sets) {
            snprintf(err, CP_ERRLEN, "upset(): set `%s` occurs in the data but not in "
                     "sets=c(...)", v);
            return -1;
        }
        if (nset == UP_MAXSET) {
            snprintf(err, CP_ERRLEN, "upset(): more than %d sets", UP_MAXSET);
            return -1;
        }
        setn[nset++] = v;
    }

    /* ---- intersections, first appearance; n must agree within one ---- */
    UInter *it = cp_xcalloc(df->nrow, sizeof(UInter));
    char **flev = cp_xcalloc(df->nrow, sizeof(char *)); int nflev = 0;
    int nit = 0;
    for (int r = 0; r < df->nrow; r++) {
        char nbuf[64];
        const char *key = ci->str ? ci->str[r] : NULL;
        if (ci->type == COL_NUM) { fmt_break(ci->num[r], -1, nbuf, sizeof nbuf); key = nbuf; }
        if (!key) continue;
        int k = -1;
        for (int i = 0; i < nit; i++) if (!strcmp(it[i].name, key)) { k = i; break; }
        double n = cn->num[r];
        if (!(n >= 0) || !isfinite(n)) {
            snprintf(err, CP_ERRLEN, "upset(): n must be a number >= 0 (intersection `%s`)", key);
            return -1;
        }
        if (k < 0) {
            k = nit++;
            it[k].name = cp_xstrdup(key); it[k].n = n; it[k].lev = -1; it[k].order = k;
        } else if (it[k].n != n) {
            snprintf(err, CP_ERRLEN, "upset(): intersection `%s` has two sizes (%g and %g); "
                     "n repeats one value on each of its rows", key, it[k].n, n);
            return -1;
        }
        int s = 0;
        while (strcmp(setn[s], cs->str[r])) s++;
        it[k].in[s] = 1;
        if (cf && cf->str[r] && *cf->str[r]) {
            int l = -1;
            for (int j = 0; j < nflev; j++) if (!strcmp(flev[j], cf->str[r])) { l = j; break; }
            if (l < 0) { l = nflev; flev[nflev++] = cf->str[r]; }
            if (it[k].lev >= 0 && it[k].lev != l) {
                snprintf(err, CP_ERRLEN, "upset(): intersection `%s` has two fill levels "
                         "(`%s` and `%s`)", key, flev[it[k].lev], flev[l]);
                return -1;
            }
            it[k].lev = l;
        }
    }
    if (nit == 0) { snprintf(err, CP_ERRLEN, "upset(): no intersections"); return -1; }

    /* set sizes over every intersection in the file, before max= */
    double ssize[UP_MAXSET] = {0};
    for (int i = 0; i < nit; i++)
        for (int s = 0; s < nset; s++) if (it[i].in[s]) ssize[s] += it[i].n;
    if (spec->up_order_n) qsort(it, nit, sizeof(UInter), by_n_desc);
    if (spec->up_max > 0 && nit > spec->up_max) nit = spec->up_max;

    /* bar colours: the fill= levels through the discrete palette
     * (scale_fill_manual(values=) names or positions), else a dark grey */
    Col *pal = NULL; char **klab = NULL;
    if (cf) {
        if (nflev == 0) {
            snprintf(err, CP_ERRLEN, "upset(): the fill column `%s` is empty", cf->name);
            return -1;
        }
        pal = cp_xmalloc(nflev * sizeof(Col));
        if (cp_discrete_palette(spec, flev, nflev, pal, err)) return -1;
        if (!(klab = cp_key_labels(spec, flev, nflev, err))) return -1;
    }
    Col bar_default = {0.2, 0.2, 0.2};
    Col dot_on = {0.2, 0.2, 0.2}, dot_off = {0.85, 0.85, 0.85};
    Col shade = {0.95, 0.95, 0.95}, muted = {0.3, 0.3, 0.3};

    /* ---- measure ---- */
    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(ms);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    double FS = SZ_BASE, AS = SZ_AXIS_TEXT, KS = SZ_LEGEND_TEXT;
    double DR = 0.3 * FS;                         /* dot radius */
    double rowh = 1.45 * FS;                      /* one set row */
    double colw0 = 1.6 * FS;                      /* natural column width */
    double asc = 0.72 * AS;                       /* label ink height */
    double setlw = 0;
    for (int s = 0; s < nset; s++) setlw = fmax(setlw, tw(cr, AS, setn[s]));

    /* intersection-size axis */
    double nmax = 0;
    for (int i = 0; i < nit; i++) nmax = fmax(nmax, it[i].n);
    if (nmax <= 0) nmax = 1;
    double ybr[16]; int nyb = extended_breaks(0, nmax, 4, ybr, 16), nyf = 0;
    for (int i = 0; i < nyb; i++) if (ybr[i] >= 0) ybr[nyf++] = ybr[i];
    double ytop = fmax(nmax, nyf ? ybr[nyf - 1] : nmax);
    int ydec = axis_decimals(ybr, nyf);
    double ylabw = 0;
    char ylab[16][32];
    for (int i = 0; i < nyf; i++) {
        fmt_break(ybr[i], ydec, ylab[i], sizeof ylab[i]);
        ylabw = fmax(ylabw, tw(cr, AS, ylab[i]));
    }
    const char *ytitle = spec->lab_y ? spec->lab_y : "Intersection size";
    double ytitleh = *ytitle ? FS * 1.2 : 0;

    /* the column left of the matrix holds the set names below and the
     * y-axis (title + tick labels) above */
    double axisw = ytitleh + ylabw + TICK_LEN + TXT_GAP;
    double lw = fmax(setlw + TXT_GAP * 2, axisw);

    /* set-size bars, left of the names */
    double setw = spec->up_set_size ? fmax(60, 4.5 * FS) : 0;
    double sbr[16]; int nsb = 0, sdec = 0; double smax = 0;
    char slab[16][32];
    double sxh = 0;                               /* their axis below the matrix */
    if (spec->up_set_size) {
        for (int s = 0; s < nset; s++) smax = fmax(smax, ssize[s]);
        if (smax <= 0) smax = 1;
        /* two or three ticks: the axis is short and its numbers long */
        int nb = extended_breaks(0, smax, 2, sbr, 16);
        for (int i = 0; i < nb; i++) if (sbr[i] >= 0) sbr[nsb++] = sbr[i];
        if (nsb) smax = fmax(smax, sbr[nsb - 1]);   /* the axis runs to its top tick */
        sdec = axis_decimals(sbr, nsb);
        double slw = 0;
        for (int i = 0; i < nsb; i++) {
            fmt_break(sbr[i], sdec, slab[i], sizeof slab[i]);
            slw = fmax(slw, tw(cr, AS, slab[i]));
        }
        setw = fmax(setw, (nsb > 1 ? nsb - 1 : 1) * slw * 1.6 + slw);
        sxh = TICK_LEN + TXT_GAP + asc + TXT_GAP + FS * 1.2;
    }

    /* fill legend */
    const char *ktitle = spec->lab_fill ? spec->lab_fill : spec->up_fill;
    double ksw = 0.9 * FS, kgap = 0.45 * FS, krow = 1.35 * FS, kw = 0;
    if (cf) {
        for (int l = 0; l < nflev; l++) kw = fmax(kw, ksw + kgap + tw(cr, KS, klab[l]));
        if (ktitle && *ktitle) kw = fmax(kw, tw(cr, FS, ktitle));
    }
    double legw = cf ? HALF_LINE * 2 + kw : 0;

    double titleh = spec->lab_title ? SZ_TITLE * 1.5 : 0;
    double caph = spec->lab_caption ? AS * 1.8 : 0;
    double gap = HALF_LINE;                       /* bar panel to matrix */
    /* natural: the bar panel about half the matrix height, within reason.
     * A --size first gives the bars ~40% of the height left after the
     * chrome and shrinks the rows (and dots) to fit the rest, the columns
     * to the width; only when rows or columns would fall below a legible
     * floor does the whole figure scale down. */
    double barh = fmin(220, fmax(100, nset * rowh * 0.5));
    double fixw = MARGIN + setw + lw + legw + MARGIN;
    double chromeh = MARGIN + titleh + gap + sxh + caph + MARGIN;
    double colw = colw0;
    double rowmin = AS * 1.15;
    if (h_pt > 0) {
        double rest = h_pt - chromeh;
        barh = fmax(60, 0.4 * rest);
        rowh = fmax(rowmin, fmin(rowh, (rest - barh) / nset));
    }
    if (w_pt > 0) colw = fmax(2.4 * 0.3 * rowmin, (w_pt - fixw) / nit);
    DR = fmin(DR, fmin(0.36 * rowh, 0.36 * colw));
    double Wn = fixw + nit * colw, Hn = chromeh + nset * rowh + barh;
    cairo_destroy(cr); cairo_surface_destroy(ms);

    if (w_pt <= 0 && h_pt <= 0) { w_pt = Wn; h_pt = Hn; }
    else if (w_pt <= 0) w_pt = Wn * h_pt / Hn;
    else if (h_pt <= 0) h_pt = Hn * w_pt / Wn;
    double sc = fmin(1, fmin(w_pt / Wn, h_pt / Hn));
    double ox = (w_pt - sc * Wn) / 2, oy = (h_pt - sc * Hn) / 2;
    #define NX(x) ((ox + sc * (x)) / w_pt)
    #define NY(y) (1 - (oy + sc * (y)) / h_pt)

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    GTable *T = cp_xcalloc(1, sizeof(GTable));
    T->ncol = 1; T->colw[0] = unull(1);
    T->nrow = 1; T->rowh[0] = unull(1);
    Grob *g;
    #define TEXT(s_, x_, y_, sz_, col_, hj_) do {                              \
        g = gt_add(T, G_TEXT, 0, 0, 0, 0);                                    \
        g->str = (s_); g->size = (sz_) * sc; g->col = (col_);                 \
        g->tx = NX(x_); g->ty = NY(y_); g->hj = (hj_); g->va = V_INKCENTER;   \
        } while (0)
    #define RECT(x0_, y0_, x1_, y1_, col_) do {                                \
        g = gt_add(T, G_RECT, 0, 0, 0, 0);                                    \
        g->sub = 1; g->col = (col_);                                          \
        g->x0 = NX(x0_); g->x1 = NX(x1_); g->y0 = NY(y1_); g->y1 = NY(y0_);   \
        } while (0)
    #define LINE(x0_, y0_, x1_, y1_, col_, lw_) do {                           \
        g = gt_add(T, G_LINE, 0, 0, 0, 0);                                    \
        g->col = (col_); g->lw = (lw_) * sc;                                  \
        g->x0 = NX(x0_); g->x1 = NX(x1_); g->y0 = NY(y0_); g->y1 = NY(y1_);   \
        } while (0)

    double mx0 = MARGIN + setw + lw;              /* matrix / bar panel left */
    double mx1 = mx0 + nit * colw;
    double bar_top = MARGIN + titleh, bar_bot = bar_top + barh;
    double my0 = bar_bot + gap;                   /* matrix top */
    double my1 = my0 + nset * rowh;
    #define CX(i) (mx0 + ((i) + 0.5) * colw)
    #define RY(s) (my0 + ((s) + 0.5) * rowh)

    if (spec->lab_title) TEXT(spec->lab_title, MARGIN, MARGIN + SZ_TITLE * 0.6, SZ_TITLE, C_BLACK, 0);

    /* ---- intersection-size bars + their y axis ---- */
    for (int i = 0; i < nit; i++) {
        double h = barh * it[i].n / ytop;
        double hw = 0.35 * colw;
        RECT(CX(i) - hw, bar_bot - h, CX(i) + hw, bar_bot,
             it[i].lev >= 0 && pal ? pal[it[i].lev] : bar_default);
    }
    double axlw = lw_pt(0.5);
    LINE(mx0, bar_top, mx0, bar_bot, muted, axlw);
    for (int i = 0; i < nyf; i++) {
        double y = bar_bot - barh * ybr[i] / ytop;
        LINE(mx0 - TICK_LEN, y, mx0, y, muted, axlw);
        TEXT(cp_xstrdup(ylab[i]), mx0 - TICK_LEN - TXT_GAP, y, AS, muted, 1);
    }
    if (ytitleh > 0) {
        g = gt_add(T, G_TEXT, 0, 0, 0, 0);
        g->str = ytitle; g->size = FS * sc; g->col = C_BLACK; g->rot90 = 1;
        g->tx = NX(mx0 - TICK_LEN - TXT_GAP - ylabw - ytitleh * 0.5);
        g->ty = NY((bar_top + bar_bot) / 2); g->hj = 0.5; g->va = V_INKCENTER;
    }

    /* ---- the membership matrix ---- */
    for (int s = 1; s < nset; s += 2)             /* alternate row shading */
        RECT(mx0, my0 + s * rowh, mx1, my0 + (s + 1) * rowh, shade);
    for (int s = 0; s < nset; s++)
        TEXT(setn[s], mx0 - TXT_GAP, RY(s), AS, C_BLACK, 1);
    for (int i = 0; i < nit; i++) {
        int lo = -1, hi = -1;
        for (int s = 0; s < nset; s++) if (it[i].in[s]) { if (lo < 0) lo = s; hi = s; }
        if (lo >= 0 && hi > lo) LINE(CX(i), RY(lo), CX(i), RY(hi), dot_on, 0.45 * DR * 2);
    }
    for (int i = 0; i < nit; i++) {
        double *px = cp_xmalloc(nset * sizeof(double)), *py = cp_xmalloc(nset * sizeof(double));
        Col *pc = cp_xmalloc(nset * sizeof(Col));
        for (int s = 0; s < nset; s++) {
            px[s] = NX(CX(i)); py[s] = NY(RY(s));
            pc[s] = it[i].in[s] ? dot_on : dot_off;
        }
        g = gt_add(T, G_POINTS, 0, 0, 0, 0);
        g->n = nset; g->px = px; g->py = py; g->pcol = pc; g->radius = DR * sc;
    }

    /* ---- set-size bars (left of the names), axis below ---- */
    if (spec->up_set_size) {
        double sx1 = MARGIN + setw - TXT_GAP, sx0 = MARGIN;
        double span = sx1 - sx0;
        for (int s = 0; s < nset; s++) {
            double w = span * ssize[s] / smax;
            RECT(sx1 - w, RY(s) - 0.3 * rowh, sx1, RY(s) + 0.3 * rowh, bar_default);
        }
        LINE(sx0, my1, sx1, my1, muted, axlw);
        for (int i = 0; i < nsb; i++) {
            double x = sx1 - span * sbr[i] / smax;
            LINE(x, my1, x, my1 + TICK_LEN, muted, axlw);
            TEXT(cp_xstrdup(slab[i]), x, my1 + TICK_LEN + TXT_GAP + asc / 2, AS, muted, 0.5);
        }
        TEXT("Set size", (sx0 + sx1) / 2, my1 + TICK_LEN + TXT_GAP + asc + TXT_GAP + FS * 0.6,
             FS, C_BLACK, 0.5);
    }

    /* ---- fill legend, right of the bars ---- */
    if (cf) {
        double kx = mx1 + HALF_LINE * 2, ky = bar_top;
        if (ktitle && *ktitle) {
            TEXT(ktitle, kx, ky + FS * 0.6, FS, C_BLACK, 0);
            ky += FS * 1.5;
        }
        for (int l = 0; l < nflev; l++) {
            RECT(kx, ky, kx + ksw, ky + ksw, pal[l]);
            TEXT(klab[l], kx + ksw + kgap, ky + ksw / 2, KS, C_BLACK, 0);
            ky += krow;
        }
    }
    if (spec->lab_caption)
        TEXT(spec->lab_caption, Wn - MARGIN, Hn - MARGIN - AS * 0.6, AS, muted, 1);

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);
    cairo_status_t st = cairo_status(cr);
    cairo_destroy(cr);
    if (st == CAIRO_STATUS_SUCCESS) st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        snprintf(err, CP_ERRLEN, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
