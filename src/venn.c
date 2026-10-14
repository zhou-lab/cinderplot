/* venn.c — a two-set Venn diagram whose regions LIST their items, each with
 * one dot per sample: the "which cell types does each platform profile"
 * schematic. Input is long: one row per (item, set) membership, with an
 * optional count n (dots) and an optional item colour.
 *
 *   A only  |  A and B  |  B only
 *   label ●●●  ●●● label ●●●  label ●●●
 *
 * In the overlap set A's dots sit left of the label and set B's right, with
 * the set names as small captions under the runs. The ellipses are sized to
 * their contents (see the solver below), so the layout is computed, not
 * placed by hand. Coordinates are worked out in "natural" points with y down
 * and mapped to the canvas at the end, scaled down uniformly when --size is
 * smaller than the natural size. */
#include "cinderplot.h"
#include <math.h>
#include <string.h>

#define ELL_N 160                       /* polygon samples per ellipse */

typedef struct {
    const char *key, *label;
    int in[3]; double n[3];             /* membership and dot count per set */
    Col col; int has_col;
    /* shape=: the rows of one (item, set) in input order, each its own
     * glyph level and count */
    int seg_lev[3][16]; double seg_n[3][16]; int nseg[3];
} VItem;

/* shape= levels and their glyphs, set once per render */
static int g_vn_shape, g_vn_glyph[16], g_vn_nslev;
static const char *g_vn_slev[16];

/* R pch -> cp_point_path glyph (filled forms; hollow pch draw filled) */
static int pch_glyph(int pch) {
    switch (pch) {
    case 15: case 0: case 22: return 2;          /* square */
    case 17: case 2: case 24: return 1;          /* triangle */
    case 18: case 5: case 23: return 3;          /* diamond */
    case 25: case 6: return 4;                   /* down-triangle */
    case 3: case 4: case 8: return 5;            /* plus */
    default: return 0;                           /* circle: 16 19 20 1 21 */
    }
}

/* the dots of one (item, set) run: count, and per-dot glyphs under shape= */
static int run_dots(const VItem *v, int s, int has_n, int **glyph) {
    *glyph = NULL;
    if (!g_vn_shape || !v->nseg[s])
        return has_n ? (int)floor(v->n[s] + 0.5) : v->in[s];
    int nd = 0;
    for (int k = 0; k < v->nseg[s]; k++) nd += has_n ? (int)floor(v->seg_n[s][k] + 0.5) : 1;
    if (nd > 0) {
        int *gl = cp_xmalloc(nd * sizeof(int)), q = 0;
        for (int k = 0; k < v->nseg[s]; k++) {
            int m = has_n ? (int)floor(v->seg_n[s][k] + 0.5) : 1;
            for (int j = 0; j < m; j++) gl[q++] = g_vn_glyph[v->seg_lev[s][k]];
        }
        *glyph = gl;
    }
    return nd;
}

static double tw(cairo_t *cr, double size, int bold, const char *s) {
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

/* "{k}" -> number of items, "{n}" -> samples (sum of n) */
static char *subst(const char *t, int k, double n) {
    char *o = cp_xmalloc(strlen(t) + 64), *q = o;
    for (const char *s = t; *s; ) {
        if (!strncmp(s, "{k}", 3)) { q += sprintf(q, "%d", k); s += 3; }
        else if (!strncmp(s, "{n}", 3)) { q += sprintf(q, "%.0f", n); s += 3; }
        else *q++ = *s++;
    }
    *q = 0;
    return o;
}

/* ---- three sets ----
 * Three equal circles on an equilateral triangle (centre spacing = radius),
 * seven regions by membership mask (bit s = in set s). Each region's items
 * are a column: label, then one dot run per member set in set order, the
 * dots filled in the item colour and OUTLINED in that set's colour, so a run
 * names its set without a caption. The radius grows until every column fits
 * its region; a column sits at the in-region point nearest the region's
 * centroid where its whole box (corners and edge midpoints) stays inside. */
static int in_circ(double x, double y, double cx, double cy, double r) {
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}
static int region_of(double x, double y, const double *cx, const double *cy, double r) {
    int m = 0;
    for (int s = 0; s < 3; s++) if (in_circ(x, y, cx[s], cy[s], r)) m |= 1 << s;
    return m;
}
static int box_in(double x, double y, double hw, double hh, int mask,
                  const double *cx, const double *cy, double r) {
    for (int i = -2; i <= 2; i++) {               /* 5 points per edge */
        double fx = i / 2.0;
        if (region_of(x + fx * hw, y - hh, cx, cy, r) != mask) return 0;
        if (region_of(x + fx * hw, y + hh, cx, cy, r) != mask) return 0;
        if (region_of(x - hw, y + fx * hh, cx, cy, r) != mask) return 0;
        if (region_of(x + hw, y + fx * hh, cx, cy, r) != mask) return 0;
    }
    return 1;
}

/* The set ellipses: every fill first, then every outline, so an opaque
 * fill (fill_alpha=1) cannot hide the other set's edge in the lens. Fill
 * and outline default to the set colour; fill_alpha= defaults to 0.07 (the
 * translucent wash), 0 or fill="none" leaving the interior empty. */
static void ellipses(GTable *T, const PlotSpec *spec, int nset, double **px,
                     double **py, const Col *scol, double lw) {
    double fa = spec->venn_fill_alpha_set ? spec->venn_fill_alpha : 0.07;
    int nofill = spec->venn_no_fill || fa <= 0;
    for (int pass = nofill; pass < 2; pass++)
        for (int s = 0; s < nset; s++) {
            Grob *g = gt_add(T, G_POLYGON, 0, 0, 0, 0);
            g->n = ELL_N; g->px = px[s]; g->py = py[s];
            if (pass == 0) {
                g->col = spec->n_venn_fill
                    ? spec->venn_fill[s % spec->n_venn_fill] : scol[s];
                g->alpha = fa >= 1 ? 0 : fa;             /* 0 = opaque */
            } else {
                g->col = spec->n_venn_outline
                    ? spec->venn_outline[s % spec->n_venn_outline] : scol[s];
                g->poly_nofill = 1; g->stroke = 1; g->lw = lw;
            }
        }
}

static int venn3(const PlotSpec *spec, VItem *it, int nit, const char **setn,
                 const Col *scol, const int *cnt, const double *tot, int has_n,
                 const char *out, double w_pt, double h_pt, char *err) {
    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(ms);
    double FS = SZ_BASE;
    double DR = 0.283 * FS * (spec->venn_dot_size > 0 ? spec->venn_dot_size : 1);
    double DXS = 0.683 * FS * (spec->venn_dot_step > 0 ? spec->venn_dot_step : 1);
    double ROW = 1.75 * FS, GAP = 0.6 * FS;
    double RGAP = 0.8 * DXS, PAD = 0.5 * FS;
    double TSZ = 1.33 * FS, SSZ = 0.875 * FS, CSZ = 0.8 * FS;
    double KSZ = cp_legend_text_sz ? SZ_LEGEND_TEXT : CSZ * 1.1;   /* key labels; theme(legend.text=) */
    Col muted = {0.4, 0.4, 0.4};

    /* region contents: mask 1..7 */
    int *reg[8], nreg[8] = {0};
    double rw[8] = {0};                         /* widest row per region */
    for (int m = 0; m < 8; m++) reg[m] = cp_xmalloc((nit ? nit : 1) * sizeof(int));
    for (int i = 0; i < nit; i++) {
        int m = it[i].in[0] | it[i].in[1] << 1 | it[i].in[2] << 2;
        reg[m][nreg[m]++] = i;
        double w = tw(cr, FS, 0, it[i].label) + GAP;
        int runs = 0;
        for (int s = 0; s < 3; s++) {
            if (!it[i].in[s]) continue;
            int *gl; double nd = run_dots(&it[i], s, has_n, &gl); free(gl);
            w += nd * DXS + (runs++ ? RGAP : 0);
        }
        if (w > rw[m]) rw[m] = w;
    }
    char *sub[3];
    double wt[3];
    for (int s = 0; s < 3; s++) {
        const char *t = s < spec->n_venn_subtitles ? spec->venn_subtitles[s]
                      : has_n ? "{k} items \xc2\xb7 {n} samples" : "{k} items";
        sub[s] = subst(t, cnt[s], tot[s]);
        wt[s] = fmax(tw(cr, TSZ, 1, setn[s]), tw(cr, SSZ, 0, sub[s]));
    }
    cairo_destroy(cr); cairo_surface_destroy(ms);

    /* grow the radius until every region's column fits */
    double R = 4 * FS, cx[3], cy[3], ax[8], ay[8];
    int ok = 0;
    for (int iter = 0; iter < 200 && !ok; iter++, R *= 1.04) {
        double sp = R;                          /* centre spacing */
        cx[0] = -sp / 2; cy[0] = 0;
        cx[1] = sp / 2;  cy[1] = 0;
        cx[2] = 0;       cy[2] = sp * sqrt(3) / 2;
        double x0 = cx[0] - R, x1 = cx[1] + R, y0 = -R, y1 = cy[2] + R;
        double step = R / 40;
        ok = 1;
        for (int m = 1; m < 8 && ok; m++) {
            if (!nreg[m]) continue;
            double hw = rw[m] / 2 + PAD, hh = nreg[m] * ROW / 2 + PAD * 0.5;
            /* centroid of the region (grid sample) */
            double sx = 0, sy = 0; int k = 0;
            for (double y = y0; y <= y1; y += step)
                for (double x = x0; x <= x1; x += step)
                    if (region_of(x, y, cx, cy, R) == m) { sx += x; sy += y; k++; }
            if (!k) { ok = 0; break; }
            sx /= k; sy /= k;
            double best = 1e300; int found = 0;
            for (double y = y0; y <= y1; y += step)
                for (double x = x0; x <= x1; x += step) {
                    if (region_of(x, y, cx, cy, R) != m) continue;
                    double d2 = (x - sx) * (x - sx) + (y - sy) * (y - sy);
                    if (d2 >= best || !box_in(x, y, hw, hh, m, cx, cy, R)) continue;
                    best = d2; ax[m] = x; ay[m] = y; found = 1;
                }
            if (!found) ok = 0;
        }
    }
    if (!ok) {
        snprintf(err, CP_ERRLEN, "venn(): could not fit the item lists into a "
                 "three-set layout; shorten the labels or n");
        return -1;
    }

    /* natural frame: titles above A and B, below C */
    double th = TSZ * 1.25 + SSZ * 1.6;
    double left = fmin(cx[0] - R, fmin(cx[0] - wt[0] / 2, cx[2] - wt[2] / 2));
    double right = fmax(cx[1] + R, fmax(cx[1] + wt[1] / 2, cx[2] + wt[2] / 2));
    double ttl = spec->lab_title ? SZ_TITLE * 1.5 : 0;
    double offx = MARGIN - left, offy = MARGIN + ttl + th + PAD + R;
    double Wn = right - left + 2 * MARGIN;
    double keyh3 = g_vn_nslev && !spec->no_legend_shape ? 1.8 * fmax(FS, KSZ) : 0;
    double Hn = offy + cy[2] + R + PAD + th + keyh3 + MARGIN + (spec->lab_caption ? CSZ * 1.8 : 0);
    if (w_pt <= 0 && h_pt <= 0) { w_pt = Wn; h_pt = Hn; }
    else if (w_pt <= 0) w_pt = Wn * h_pt / Hn;
    else if (h_pt <= 0) h_pt = Hn * w_pt / Wn;
    double sc = fmin(1, fmin(w_pt / Wn, h_pt / Hn));
    double ox = (w_pt - sc * Wn) / 2, oy = (h_pt - sc * Hn) / 2;
    #define NX3(x) ((ox + sc * ((x) + offx)) / w_pt)
    #define NY3(y) (1 - (oy + sc * ((y) + offy)) / h_pt)

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    GTable *T = cp_xcalloc(1, sizeof(GTable));
    T->ncol = 1; T->colw[0] = unull(1);
    T->nrow = 1; T->rowh[0] = unull(1);
    Grob *g;
    #define TEXT3(s_, x_, y_, sz_, col_, hj_, bold_) do {                      \
        g = gt_add(T, G_TEXT, 0, 0, 0, 0);                                    \
        g->str = (s_); g->size = (sz_) * sc; g->col = (col_);                 \
        g->tx = NX3(x_); g->ty = NY3(y_); g->hj = (hj_); g->va = V_INKCENTER; \
        g->bold = (bold_); } while (0)

    if (spec->lab_title)
        TEXT3(spec->lab_title, (left + right) / 2, -offy + MARGIN + SZ_TITLE * 0.6,
              SZ_TITLE, C_BLACK, 0.5, 0);
    double *epx[3], *epy[3];
    for (int s = 0; s < 3; s++) {
        double *px = cp_xmalloc(ELL_N * sizeof(double)), *py = cp_xmalloc(ELL_N * sizeof(double));
        for (int i = 0; i < ELL_N; i++) {
            double u = 2 * M_PI * i / ELL_N;
            px[i] = NX3(cx[s] + R * cos(u)); py[i] = NY3(cy[s] + R * sin(u));
        }
        epx[s] = px; epy[s] = py;
    }
    ellipses(T, spec, 3, epx, epy, scol, 1.6 * sc);
    for (int s = 0; s < 3; s++) {                 /* A, B above; C below */
        double ty = s < 2 ? -R - PAD - th + TSZ * 0.6 : cy[2] + R + PAD + TSZ * 0.6;
        TEXT3(setn[s], cx[s], ty, TSZ, scol[s], 0.5, 1);
        if (sub[s][0]) TEXT3(sub[s], cx[s], ty + TSZ * 0.65 + SSZ, SSZ, muted, 0.5, 0);
    }
    for (int m = 1; m < 8; m++) {
        if (!nreg[m]) continue;
        double xl = ax[m] - rw[m] / 2;
        for (int j = 0; j < nreg[m]; j++) {
            VItem *v = &it[reg[m][j]];
            double y = ay[m] - (nreg[m] - 1) * ROW / 2 + j * ROW;
            TEXT3(v->label, xl, y, FS, C_BLACK, 0, 0);
            double x = xl + tw(cr, FS, 0, v->label) + GAP + DR;
            int runs = 0;
            for (int s = 0; s < 3; s++) {
                if (!v->in[s]) continue;
                int *gl; int nd = run_dots(v, s, has_n, &gl);
                if (runs++) x += RGAP;
                if (nd <= 0) continue;
                double *qx = cp_xmalloc(nd * sizeof(double)), *qy = cp_xmalloc(nd * sizeof(double));
                Col *qc = cp_xmalloc(nd * sizeof(Col)), *qf = cp_xmalloc(nd * sizeof(Col));
                for (int q = 0; q < nd; q++) {
                    qx[q] = NX3(x + q * DXS); qy[q] = NY3(y);
                    qc[q] = scol[s];
                    qf[q] = v->has_col ? v->col : scol[s];
                }
                g = gt_add(T, G_POINTS, 0, 0, 0, 0);
                g->n = nd; g->px = qx; g->py = qy; g->pcol = qc; g->pfill = qf;
                g->pshape = gl;
                g->radius = DR * sc; g->stroke_lw = 0.9 * sc;
                x += nd * DXS;
            }
        }
    }
    if (keyh3 > 0) {                              /* the shape key, under C's title */
        double kw[16], keyw = 0;
        for (int l = 0; l < g_vn_nslev; l++) {
            kw[l] = 2 * DR + 0.4 * FS + tw(cr, KSZ, 0, g_vn_slev[l]);
            keyw += kw[l] + (l ? FS : 0);
        }
        double kx = (left + right) / 2 - keyw / 2, ky = cy[2] + R + PAD + th + keyh3 * 0.55;
        for (int l = 0; l < g_vn_nslev; l++) {
            double *qx = cp_xmalloc(sizeof(double)), *qy = cp_xmalloc(sizeof(double));
            Col *qc = cp_xmalloc(sizeof(Col));
            qx[0] = NX3(kx + DR); qy[0] = NY3(ky); *qc = muted;
            g = gt_add(T, G_POINTS, 0, 0, 0, 0);
            g->n = 1; g->px = qx; g->py = qy; g->pcol = qc;
            g->radius = DR * sc; g->shape = g_vn_glyph[l];
            TEXT3(g_vn_slev[l], kx + 2 * DR + 0.4 * FS, ky, KSZ, muted, 0, 0);
            kx += kw[l] + FS;
        }
    }
    if (spec->lab_caption)
        TEXT3(spec->lab_caption, (left + right) / 2, cy[2] + R + PAD + th + keyh3 + CSZ * 1.2,
              CSZ, muted, 0.5, 0);

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

int render_venn(const PlotSpec *spec, const char *data, const char *out,
                double w_pt, double h_pt, char *err) {
    DataFrame *df = df_read_csv(data ? data : "-", err);
    if (!df) return -1;
    if (df->nrow == 0) {
        snprintf(err, CP_ERRLEN, "venn(): %s has no rows", data ? data : "stdin");
        return -1;
    }
    const char *nm_item = spec->venn_item ? spec->venn_item : "item";
    const char *nm_set = spec->venn_set ? spec->venn_set : "set";
    const Column *ci = df_col(df, nm_item), *cs = df_col(df, nm_set);
    const Column *cn = spec->venn_n ? df_col(df, spec->venn_n) : df_col(df, "n");
    const Column *cc = spec->venn_colour ? df_col(df, spec->venn_colour)
                                         : df_col(df, "colour");
    if (!ci || !cs) {
        snprintf(err, CP_ERRLEN, "venn(): column `%s` not found (give item=/set=)",
                 !ci ? nm_item : nm_set);
        return -1;
    }
    if ((spec->venn_n && !cn) || (spec->venn_colour && !cc)) {
        snprintf(err, CP_ERRLEN, "venn(): column `%s` not found",
                 spec->venn_n && !cn ? spec->venn_n : spec->venn_colour);
        return -1;
    }
    if (ci->type != COL_STR || cs->type != COL_STR) {
        snprintf(err, CP_ERRLEN, "venn(): the item and set columns must be text");
        return -1;
    }
    if (cn && cn->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "venn(): the n column `%s` must be numeric", cn->name);
        return -1;
    }
    if (cc && cc->type != COL_STR) cc = NULL;
    const Column *csh = spec->venn_shape ? df_col(df, spec->venn_shape) : NULL;
    if (spec->venn_shape && (!csh || csh->type != COL_STR)) {
        snprintf(err, CP_ERRLEN, "venn(): shape column `%s` %s", spec->venn_shape,
                 csh ? "must be text" : "not found");
        return -1;
    }
    const char *slev[16]; int nslev = 0;
    g_vn_shape = csh != NULL;

    /* ---- sets: sets=c(...) order, else first appearance ---- */
    const char *setn[4]; int nset = 0;
    for (int i = 0; i < spec->n_venn_sets && nset < 4; i++) setn[nset++] = spec->venn_sets[i];
    for (int r = 0; r < df->nrow; r++) {
        const char *v = cs->str[r];
        int k = -1;
        for (int i = 0; i < nset; i++) if (!strcmp(setn[i], v)) k = i;
        if (k >= 0) continue;
        if (spec->n_venn_sets) {
            snprintf(err, CP_ERRLEN, "venn(): set `%s` occurs in the data but not in "
                     "sets=c(...)", v);
            return -1;
        }
        if (nset == 4) { nset++; break; }
        setn[nset++] = v;
    }
    if (nset < 2 || nset > 3) {
        snprintf(err, CP_ERRLEN, nset > 3 ? "venn() with %d sets is not implemented; "
                 "two or three sets are" : "venn() needs two or three sets "
                 "(%d found)", nset);
        return -1;
    }

    /* ---- items, in first-appearance order ---- */
    VItem *it = cp_xcalloc(df->nrow, sizeof(VItem));
    int nit = 0;
    double tot[3] = {0, 0, 0};
    for (int r = 0; r < df->nrow; r++) {
        int s = 0;
        while (s < nset - 1 && strcmp(cs->str[r], setn[s])) s++;
        int k = -1;
        for (int i = 0; i < nit; i++) if (!strcmp(it[i].key, ci->str[r])) { k = i; break; }
        if (k < 0) {
            k = nit++;
            it[k].key = it[k].label = ci->str[r];
            for (int l = 0; l < spec->n_venn_labs; l++)
                if (spec->venn_lab_names[l] && !strcmp(spec->venn_lab_names[l], it[k].key))
                    it[k].label = spec->venn_labs[l];
        }
        double n = cn ? cn->num[r] : 1;
        if (!(n >= 0)) {
            snprintf(err, CP_ERRLEN, "venn(): n must be >= 0 (item `%s`, set `%s`)",
                     ci->str[r], cs->str[r]);
            return -1;
        }
        it[k].in[s] = 1;
        it[k].n[s] += n;
        tot[s] += n;
        if (csh) {
            int li = -1;
            for (int l = 0; l < nslev; l++) if (!strcmp(slev[l], csh->str[r])) li = l;
            if (li < 0) {
                if (nslev == 16) {
                    snprintf(err, CP_ERRLEN, "venn(): shape= has more than 16 levels");
                    return -1;
                }
                li = nslev; slev[nslev++] = csh->str[r];
            }
            if (it[k].nseg[s] == 16) {
                snprintf(err, CP_ERRLEN, "venn(): item `%s` has more than 16 rows in set `%s`",
                         ci->str[r], cs->str[r]);
                return -1;
            }
            it[k].seg_lev[s][it[k].nseg[s]] = li;
            it[k].seg_n[s][it[k].nseg[s]++] = n;
        }
        if (cc && cc->str[r] && *cc->str[r] && !it[k].has_col) {
            if (parse_color(cc->str[r], &it[k].col)) {
                snprintf(err, CP_ERRLEN, "venn(): bad colour `%s` for item `%s`",
                         cc->str[r], ci->str[r]);
                return -1;
            }
            it[k].has_col = 1;
        }
    }
    int cnt[3] = {0, 0, 0};
    for (int i = 0; i < nit; i++) for (int s = 0; s < nset; s++) cnt[s] += it[i].in[s];
    /* glyph per shape level: scale_shape_manual (named, else positional),
     * else ggplot's shape palette order (circle, triangle, square, ...) */
    g_vn_nslev = nslev;
    for (int l = 0; l < nslev; l++) {
        g_vn_slev[l] = slev[l];
        static const int dflt[6] = { 0, 1, 2, 3, 4, 5 };
        g_vn_glyph[l] = dflt[l % 6];
        for (int k = 0; k < spec->n_shape_pch; k++)
            if (spec->shape_pch_names[k] ? !strcmp(spec->shape_pch_names[k], slev[l]) : k == l)
                { g_vn_glyph[l] = pch_glyph(spec->shape_pch[k]); break; }
    }

    /* #3465a8 #3a7d3a #b5532b */
    Col scol[3] = { {0.204, 0.396, 0.659}, {0.227, 0.490, 0.227}, {0.710, 0.325, 0.169} };
    for (int s = 0; s < nset && s < spec->n_venn_set_cols; s++) scol[s] = spec->venn_set_cols[s];
    Col muted = {0.4, 0.4, 0.4};
    if (nset == 3) {
        int rc = venn3(spec, it, nit, setn, scol, cnt, tot, cn != NULL, out, w_pt, h_pt, err);
        return rc;
    }

    /* ---- region lists: 0 = A only, 1 = both, 2 = B only ---- */
    int *reg[3], nreg[3] = {0, 0, 0};
    for (int g = 0; g < 3; g++) reg[g] = cp_xmalloc((nit ? nit : 1) * sizeof(int));
    for (int i = 0; i < nit; i++) {
        int g = it[i].in[0] && it[i].in[1] ? 1 : it[i].in[0] ? 0 : 2;
        reg[g][nreg[g]++] = i;
    }

    /* ---- measure (natural points) ---- */
    cairo_surface_t *ms = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(ms);
    double FS = SZ_BASE;
    /* dot radius and step: dot_size=/dot_step= scale the defaults */
    double DR = 0.283 * FS * (spec->venn_dot_size > 0 ? spec->venn_dot_size : 1);
    double DXS = 0.683 * FS * (spec->venn_dot_step > 0 ? spec->venn_dot_step : 1);
    double ROW = 1.75 * FS, ROWO = ROW * 1.15, GAP = 0.6 * FS, PAD = 0.5 * FS;
    double TSZ = 1.33 * FS, SSZ = 0.875 * FS, CSZ = 0.8 * FS;
    double KSZ = cp_legend_text_sz ? SZ_LEGEND_TEXT : CSZ * 1.1;   /* key labels; theme(legend.text=) */
    double lab[3] = {0, 0, 0}, dmax[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    for (int g = 0; g < 3; g++)
        for (int j = 0; j < nreg[g]; j++) {
            VItem *v = &it[reg[g][j]];
            double w = tw(cr, FS, 0, v->label);
            if (w > lab[g]) lab[g] = w;
            for (int s = 0; s < 2; s++) {
                int *gl; double nd = run_dots(v, s, cn != NULL, &gl); free(gl);
                if (nd > dmax[g][s]) dmax[g][s] = nd;
            }
        }
    /* one-set regions (A only = 0, B only = 2) may wrap into columns:
     * ncol= of them, or enough to keep each under max_rows= rows. Column k
     * holds items k*rows .. ; each column is its own label + dots width. */
    double CGAP = 1.2 * FS;
    int nrw[3];
    double colx[3][16], colw[3][16], blockw[3] = {0, 0, 0};
    for (int g = 0; g < 3; g += 2) {
        int c = spec->venn_ncol > 0 ? spec->venn_ncol : 1;
        if (spec->venn_max_rows > 0 && nreg[g] > 0) {
            int c2 = (nreg[g] + spec->venn_max_rows - 1) / spec->venn_max_rows;
            if (c2 > c) c = c2;
        }
        if (c > nreg[g]) c = nreg[g] ? nreg[g] : 1;
        if (c > 16) c = 16;
        nrw[g] = nreg[g] ? (nreg[g] + c - 1) / c : 0;
        for (int k = 0; k < c; k++) {
            double lw = 0, dw = 0;
            for (int j = k * nrw[g]; j < (k + 1) * nrw[g] && j < nreg[g]; j++) {
                VItem *v = &it[reg[g][j]];
                double w = tw(cr, FS, 0, v->label);
                if (w > lw) lw = w;
                int *gl; double nd = run_dots(v, g ? 1 : 0, cn != NULL, &gl); free(gl);
                if (nd > dw) dw = nd;
            }
            colx[g][k] = blockw[g] + (k ? CGAP : 0);
            colw[g][k] = lw;                            /* label width */
            blockw[g] = colx[g][k] + lw + GAP + dw * DXS;
        }
    }
    nrw[1] = nreg[1];
    double wA = nreg[0] ? blockw[0] : 0;
    double wB = nreg[2] ? blockw[2] : 0;
    double half = lab[1] / 2 + GAP;                     /* label half + gap */
    double hwO = nreg[1] ? half + fmax(dmax[1][0], dmax[1][1]) * DXS : 0;
    double hA = nreg[0] ? (nrw[0] - 1) * ROW / 2 + 0.6 * FS : 0;
    double hB = nreg[2] ? (nrw[2] - 1) * ROW / 2 + 0.6 * FS : 0;
    double hO = nreg[1] ? (nreg[1] - 1) * ROWO / 2 + 0.6 * FS
                          + (spec->venn_no_captions ? 0 : 0.6 * ROW) : 0;

    /* ---- solve the ellipses: one height ry, each its own width ----
     * A-only items sit left-aligned against A's boundary at their outermost
     * row and must clear B's boundary at the centre row, where it intrudes
     * most: d >= wA + 2P - rxA fA + rxB. Mirrored for B only:
     * d >= wB + 2P + rxA - rxB fB. The overlap column must fit the lens at
     * its outermost row: (rxA + rxB) fO - d >= 2 hwO + 2P. Over a grid of
     * (rxA, rxB) take the narrowest total width rxA + d + rxB, then refit ry
     * to the new widths; a few rounds settle it. So each ellipse follows its
     * own region -- shorter labels on one side narrow that side. */
    double hmax = fmax(fmax(hA, hB), hO);
    double ry = fmax(hmax / 0.86, 3 * FS), rxA = ry * 1.3, rxB = rxA, d = rxA;
    #define FY(h) sqrt(fmax(0.05, 1 - ((h) / ry) * ((h) / ry)))
    double L = 2 * hwO + 2 * PAD;
    for (int round = 0; round < 12; round++) {
        double fA = FY(hA), fB = FY(hB), fO = FY(hO), best = 1e300;
        double lo = 4 * FS, step = FS / 4, hi = lo + 400 * step;
        for (double a = lo; a <= hi; a += step)
            for (double b = lo; b <= hi; b += step) {
                double dd = fmax(fmax(wA + 2 * PAD - a * fA + b, wB + 2 * PAD + a - b * fB),
                                 0.25 * fmin(a, b));
                if ((a + b) * fO - dd < L) continue;
                double W = a + b + dd;
                if (W < best) { best = W; rxA = a; rxB = b; d = dd; }
            }
        double nry = fmax(hmax / 0.86, 0.55 * fmax(rxA, rxB));
        if (fabs(nry - ry) < 0.5) { ry = nry; break; }
        ry = nry;
    }

    /* titles and subtitles */
    char *sub[2];
    for (int s = 0; s < 2; s++) {
        const char *t = s < spec->n_venn_subtitles ? spec->venn_subtitles[s]
                      : cn ? "{k} items \xc2\xb7 {n} samples" : "{k} items";
        sub[s] = subst(t, cnt[s], tot[s]);
    }
    double titleh = TSZ * 1.25 + (sub[0][0] || sub[1][0] ? SSZ * 1.6 : 0) + PAD;
    double topy = MARGIN + (spec->lab_title ? SZ_TITLE * 1.5 : 0) + titleh;
    double xa = MARGIN + rxA, xb = xa + d, cy = topy + ry;
    double tx[2] = { xa - 0.2 * rxA, xb + 0.2 * rxB };
    double wt[2];
    for (int s = 0; s < 2; s++)
        wt[s] = fmax(tw(cr, TSZ, 1, setn[s]), tw(cr, SSZ, 0, sub[s]));
    double shift = fmax(0, MARGIN - (tx[0] - wt[0] / 2));   /* keep titles on canvas */
    xa += shift; xb += shift; tx[0] += shift; tx[1] += shift;
    double Wn = fmax(xb + rxB, tx[1] + wt[1] / 2) + MARGIN;
    /* shape key: one row of glyph + level under the ellipses */
    double keyw = 0, kw[16];
    for (int l = 0; l < nslev; l++) {
        kw[l] = 2 * DR + 0.4 * FS + tw(cr, KSZ, 0, slev[l]);
        keyw += kw[l] + (l ? FS : 0);
    }
    int show_key = nslev && !spec->no_legend_shape;      /* guides(shape="none") */
    double keyh = show_key ? 1.8 * fmax(FS, KSZ) : 0;
    double Hn = cy + ry + keyh + MARGIN + (spec->lab_caption ? CSZ * 1.8 : 0);
    if (show_key && keyw + 2 * MARGIN > Wn) Wn = keyw + 2 * MARGIN;
    cairo_destroy(cr); cairo_surface_destroy(ms);

    /* ---- canvas and the natural -> canvas map ---- */
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
    #define TEXT(s_, x_, y_, sz_, col_, hj_, bold_) do {                       \
        g = gt_add(T, G_TEXT, 0, 0, 0, 0);                                    \
        g->str = (s_); g->size = (sz_) * sc; g->col = (col_);                 \
        g->tx = NX(x_); g->ty = NY(y_); g->hj = (hj_); g->va = V_INKCENTER;   \
        g->bold = (bold_); } while (0)

    if (spec->lab_title)
        TEXT(spec->lab_title, Wn / 2, MARGIN + SZ_TITLE * 0.6, SZ_TITLE, C_BLACK, 0.5, 0);

    /* ellipses: translucent fill, set-coloured outline */
    double *epx[2], *epy[2];
    for (int s = 0; s < 2; s++) {
        double *px = cp_xmalloc(ELL_N * sizeof(double)), *py = cp_xmalloc(ELL_N * sizeof(double));
        double cx = s ? xb : xa, rxs = s ? rxB : rxA;
        for (int i = 0; i < ELL_N; i++) {
            double u = 2 * M_PI * i / ELL_N;
            px[i] = NX(cx + rxs * cos(u)); py[i] = NY(cy + ry * sin(u));
        }
        epx[s] = px; epy[s] = py;
    }
    ellipses(T, spec, 2, epx, epy, scol, 1.6 * sc);
    for (int s = 0; s < 2; s++) {
        double ty0 = topy - titleh + TSZ * 0.6;
        TEXT(setn[s], tx[s], ty0, TSZ, scol[s], 0.5, 1);
        if (sub[s][0]) TEXT(sub[s], tx[s], ty0 + TSZ * 0.65 + SSZ, SSZ, muted, 0.5, 0);
    }

    /* dots: one run per (item, side) */
    #define DOTS(v_, s_, x0_, step_, y_) do {                                 \
        int *gl_; int nd_ = run_dots((v_), (s_), cn != NULL, &gl_);           \
        if (nd_ > 0) {                                                        \
            double *qx = cp_xmalloc(nd_ * sizeof(double));                    \
            double *qy = cp_xmalloc(nd_ * sizeof(double));                    \
            Col *qc = cp_xmalloc(nd_ * sizeof(Col));                          \
            for (int q = 0; q < nd_; q++) {                                   \
                qx[q] = NX((x0_) + (step_) * q); qy[q] = NY(y_);              \
                qc[q] = (v_)->has_col ? (v_)->col : scol[s_];                 \
            }                                                                 \
            g = gt_add(T, G_POINTS, 0, 0, 0, 0);                              \
            g->n = nd_; g->px = qx; g->py = qy; g->pcol = qc;                 \
            g->radius = DR * sc; g->pshape = gl_;                             \
        } } while (0)

    /* A only: left-aligned against A's boundary at the outermost row */
    double xl = xa - rxA * FY(hA) + PAD;
    for (int j = 0; j < nreg[0]; j++) {
        VItem *v = &it[reg[0][j]];
        int k = j / nrw[0], rr = j % nrw[0];
        double x0 = xl + colx[0][k];
        double y = cy - (nrw[0] - 1) * ROW / 2 + rr * ROW;
        TEXT(v->label, x0, y, FS, C_BLACK, 0, 0);
        DOTS(v, 0, x0 + colw[0][k] + GAP + DR, DXS, y);
    }
    /* B only: left-aligned just clear of A's boundary at the centre */
    double xr = fmax(xa + rxA + PAD, xb + rxB * FY(hB) - PAD - wB);
    for (int j = 0; j < nreg[2]; j++) {
        VItem *v = &it[reg[2][j]];
        int k = j / nrw[2], rr = j % nrw[2];
        double x0 = xr + colx[2][k];
        double y = cy - (nrw[2] - 1) * ROW / 2 + rr * ROW;
        TEXT(v->label, x0, y, FS, C_BLACK, 0, 0);
        DOTS(v, 1, x0 + colw[2][k] + GAP + DR, DXS, y);
    }
    /* overlap: label centred, A's dots leftward, B's rightward */
    double xm = ((xa + rxA * FY(hO)) + (xb - rxB * FY(hO))) / 2;   /* lens centre */
    double y0 = cy - (nreg[1] - 1) * ROWO / 2 - (spec->venn_no_captions ? 0 : 0.3 * ROW);
    for (int j = 0; j < nreg[1]; j++) {
        VItem *v = &it[reg[1][j]];
        double y = y0 + j * ROWO;
        TEXT(v->label, xm, y, FS, C_BLACK, 0.5, 0);
        DOTS(v, 0, xm - half - DR, -DXS, y);
        DOTS(v, 1, xm + half + DR, DXS, y);
    }
    if (nreg[1] && !spec->venn_no_captions) {
        double yc = y0 + (nreg[1] - 1) * ROWO + ROW * 0.9;
        TEXT(setn[0], xm - half, yc, CSZ, scol[0], 1, 0);
        TEXT(setn[1], xm + half, yc, CSZ, scol[1], 0, 0);
    }
    if (show_key) {                               /* the shape key */
        double kx = Wn / 2 - keyw / 2, ky = cy + ry + keyh * 0.55;
        for (int l = 0; l < nslev; l++) {
            double *qx = cp_xmalloc(sizeof(double)), *qy = cp_xmalloc(sizeof(double));
            Col *qc = cp_xmalloc(sizeof(Col));
            qx[0] = NX(kx + DR); qy[0] = NY(ky); *qc = muted;
            g = gt_add(T, G_POINTS, 0, 0, 0, 0);
            g->n = 1; g->px = qx; g->py = qy; g->pcol = qc;
            g->radius = DR * sc; g->shape = g_vn_glyph[l];
            TEXT(slev[l], kx + 2 * DR + 0.4 * FS, ky, KSZ, muted, 0, 0);
            kx += kw[l] + FS;
        }
    }
    if (spec->lab_caption)
        TEXT(spec->lab_caption, Wn / 2, Hn - MARGIN - CSZ * 0.6, CSZ, muted, 0.5, 0);

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
