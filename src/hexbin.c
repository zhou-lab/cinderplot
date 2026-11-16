/* hexbin.c — geom_hex()'s default stat (ggplot2's stat_binhex): count the
 * points per hexagon. The binned layer then draws exactly as
 * geom_hex(stat="identity") does, one hexagon per cell, so this file only
 * replaces the data: cell centres in the x/y columns, the count in `count`,
 * the facet columns carried through, and fill= mapped to the count. */
#include "cinderplot.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* hexbin's cell index for one point (hexbin::hexbin's Fortran hbin): the
 * nearer of the two candidate lattice centres, even rows at whole x steps,
 * odd rows half a step over. Cells are numbered from 0 row-major with
 * jmax per row; returns the 0-based cell. */
static long hb_cell(double sx, double sy, int jmax) {
    const double con1 = 0.25, con2 = 1.0 / 3;
    long iinc = 2L * jmax, jinc = jmax;
    long j1 = (long)(sx + 0.5), i1 = (long)(sy + 0.5);
    double d1 = (sx - j1) * (sx - j1) + 3 * (sy - i1) * (sy - i1);
    if (d1 < con1) return i1 * iinc + j1;
    if (d1 > con2) return (long)sy * iinc + (long)sx + jinc;
    long j2 = (long)sx, i2 = (long)sy;
    double d2 = (sx - j2 - 0.5) * (sx - j2 - 0.5) + 3 * (sy - i2 - 0.5) * (sy - i2 - 0.5);
    return d1 <= d2 ? i1 * iinc + j1 : i2 * iinc + j2 + jinc;
}

/* hex_bounds(): the data range snapped out to whole bin widths, as ggplot2 */
static void hb_bounds(double lo, double hi, double bw, double *b0, double *b1) {
    *b0 = floor(lo / bw) * bw - 1e-6;
    *b1 = ceil(hi / bw) * bw + 1e-6;
}

int hexbin_apply(PlotSpec *spec, DataFrame **dfp, char *err) {
    Layer *L = NULL;
    for (int i = 0; i < spec->nlayers; i++)
        if (spec->layers[i].hex && !spec->layers[i].hex_identity) L = &spec->layers[i];
    if (!L) return 0;
    for (int i = 0; i < spec->nlayers; i++) {
        const Layer *o = &spec->layers[i];
        int refline = (o->type == GEOM_HLINE || o->type == GEOM_VLINE
                       || o->type == GEOM_ABLINE) && !o->data && !o->icept_col;
        if (o != L && !refline) {
            snprintf(err, CP_ERRLEN, "binned geom_hex() replaces the data with its "
                     "hexagon counts, so it combines only with constant reference "
                     "lines; draw the points as geom_hex(stat=\"identity\") or plot "
                     "them separately");
            return -1;
        }
    }
    if (spec->colour.col) {
        snprintf(err, CP_ERRLEN, "binned geom_hex() fills by count; drop aes(fill=/"
                 "colour=) (a mapped summary, ggplot2's stat_summary_hex, is not "
                 "implemented), or map it with geom_hex(stat=\"identity\")");
        return -1;
    }
    if (spec->log_x || spec->log_y || spec->genome_seqinfo) {
        snprintf(err, CP_ERRLEN, "binned geom_hex() needs linear x and y axes");
        return -1;
    }
    const DataFrame *df = *dfp;
    const Column *xc = spec->x.col ? df_col(df, spec->x.col) : NULL;
    const Column *yc = spec->y.col ? df_col(df, spec->y.col) : NULL;
    if (!xc || !yc || xc->type != COL_NUM || yc->type != COL_NUM
        || spec->x.is_factor || spec->y.is_factor) {
        snprintf(err, CP_ERRLEN, "geom_hex() needs numeric x and y columns in aes()");
        return -1;
    }
    /* the facet columns travel with the cells, so each panel bins its own */
    const char *fv[3] = {spec->facet_var, spec->facet_rowvar, spec->facet_colvar};
    const Column *fc[3] = {0};
    int nf = 0;
    for (int k = 0; k < 3; k++) {
        if (!fv[k]) continue;
        const Column *c = df_col(df, fv[k]);
        if (!c) { snprintf(err, CP_ERRLEN, "column `%s` not found", fv[k]); return -1; }
        fc[nf++] = c;
    }

    /* panel id per row: the first row with the same facet values */
    int n = df->nrow;
    int *pan = cp_xmalloc((size_t)(n ? n : 1) * sizeof(int));
    double xlo = INFINITY, xhi = -INFINITY, ylo = INFINITY, yhi = -INFINITY;
    int first = -1;                                /* first finite row */
    for (int r = 0; r < n; r++) {
        pan[r] = -1;
        double x = xc->num[r], y = yc->num[r];
        if (!isfinite(x) || !isfinite(y)) continue;
        if (x < xlo) xlo = x;
        if (x > xhi) xhi = x;
        if (y < ylo) ylo = y;
        if (y > yhi) yhi = y;
        if (first < 0) first = r;
        pan[r] = nf ? r : first;
        for (int q = 0; q < r && nf; q++) {
            if (pan[q] != q) continue;
            int same = 1;
            for (int k = 0; k < nf && same; k++)
                same = fc[k]->type == COL_NUM
                     ? (fc[k]->num[q] == fc[k]->num[r]
                        || (isnan(fc[k]->num[q]) && isnan(fc[k]->num[r])))
                     : !strcmp(fc[k]->str[q] ? fc[k]->str[q] : "",
                               fc[k]->str[r] ? fc[k]->str[r] : "");
            if (same) { pan[r] = q; break; }
        }
    }
    if (!(xhi >= xlo)) {
        free(pan);
        snprintf(err, CP_ERRLEN, "geom_hex(): no rows with finite x and y");
        return -1;
    }
    /* ggplot2's hex_binwidth(): the scale's range over bins= */
    double bwx = L->hex_bw_x > 0 ? L->hex_bw_x : (xhi > xlo ? (xhi - xlo) / L->hex_bins : 1);
    double bwy = L->hex_bw_y > 0 ? L->hex_bw_y : (yhi > ylo ? (yhi - ylo) / L->hex_bins : 1);

    /* output: at most one cell per input row */
    int ncol = 3 + nf;
    DataFrame *out = cp_xcalloc(1, sizeof(DataFrame));
    out->ncol = ncol;
    out->cols = cp_xcalloc((size_t)ncol, sizeof(Column));
    out->header = cp_xcalloc((size_t)ncol, sizeof(char *));
    out->nheader = ncol;
    const char *names[3] = {spec->x.col, spec->y.col, "count"};
    for (int k = 0; k < ncol; k++) {
        Column *c = &out->cols[k];
        c->name = cp_xstrdup(k < 3 ? names[k] : fc[k - 3]->name);
        c->type = k < 3 ? COL_NUM : fc[k - 3]->type;
        if (c->type == COL_NUM) c->num = cp_xmalloc((size_t)(n ? n : 1) * sizeof(double));
        else c->str = cp_xmalloc((size_t)(n ? n : 1) * sizeof(char *));
        out->header[k] = c->name;
    }

    int nout = 0;
    for (int p0 = 0; p0 < n; p0++) {
        if (pan[p0] != p0) continue;               /* one pass per panel */
        double pxlo = INFINITY, pxhi = -INFINITY, pylo = INFINITY, pyhi = -INFINITY;
        for (int r = p0; r < n; r++) {
            if (pan[r] != p0) continue;
            double x = xc->num[r], y = yc->num[r];
            if (x < pxlo) pxlo = x;
            if (x > pxhi) pxhi = x;
            if (y < pylo) pylo = y;
            if (y > pyhi) pyhi = y;
        }
        double xb0, xb1, yb0, yb1;
        hb_bounds(pxlo, pxhi, bwx, &xb0, &xb1);
        hb_bounds(pylo, pyhi, bwy, &yb0, &yb1);
        double xbins = (xb1 - xb0) / bwx, ybins = (yb1 - yb0) / bwy;
        double shape = ybins / xbins;
        int jmax = (int)floor(xbins + 1.5001);
        int imax = 2 * (int)floor(xbins * shape / sqrt(3.0) + 1.5001);
        long lmax = (long)jmax * imax;
        if (lmax <= 0 || lmax > 50000000L) {
            free(pan);
            snprintf(err, CP_ERRLEN, "geom_hex(): %ld cells is too many; give a larger "
                     "binwidth= or fewer bins=", lmax);
            return -1;
        }
        int *cnt = cp_xcalloc((size_t)lmax, sizeof(int));
        double c1 = xbins / (xb1 - xb0);
        double c2 = xbins * shape / ((yb1 - yb0) * sqrt(3.0));
        for (int r = p0; r < n; r++) {
            if (pan[r] != p0) continue;
            long cell = hb_cell(c1 * (xc->num[r] - xb0), c2 * (yc->num[r] - yb0), jmax);
            if (cell >= 0 && cell < lmax) cnt[cell]++;
        }
        /* hcell2xy: cell centres */
        double c3 = (xb1 - xb0) / xbins;
        double c4 = (yb1 - yb0) * sqrt(3.0) / (2 * shape * xbins);
        for (long cell = 0; cell < lmax; cell++) {
            if (!cnt[cell]) continue;
            long i = cell / jmax, j = cell % jmax;
            out->cols[0].num[nout] = c3 * (i % 2 == 0 ? j : j + 0.5) + xb0;
            out->cols[1].num[nout] = c4 * i + yb0;
            out->cols[2].num[nout] = cnt[cell];
            for (int k = 0; k < nf; k++) {
                if (fc[k]->type == COL_NUM) out->cols[3 + k].num[nout] = fc[k]->num[p0];
                else out->cols[3 + k].str[nout] = fc[k]->str[p0];
            }
            nout++;
        }
        free(cnt);
    }
    free(pan);
    out->nrow = nout;
    /* the input stays allocated: its string cells back the facet columns */

    /* draw as the identity form, sized from the bin widths (a sparse grid
     * would otherwise read a coarser pitch): ggplot2's GeomHex gets width =
     * binwidth x and a centre-to-vertex height of binwidth y / sqrt(3) */
    L->hex_identity = 1;
    L->hex_dx = bwx / 2;
    L->hex_ry = bwy / sqrt(3.0);
    spec->colour.col = cp_xstrdup("count");
    spec->colour.expr = cp_xstrdup("count");      /* the legend title */
    spec->colour.is_fill = 1;
    *dfp = out;
    return 0;
}
