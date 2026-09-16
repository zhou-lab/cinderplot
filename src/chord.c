/* chord.c — circlize-style chord diagram (the chordDiagram() function's
 * defaults, not its whole option surface): sectors around a circle sized by
 * total flow, an outer band per sector with its name outside, and filled
 * translucent ribbons connecting source and target sub-arcs, width
 * proportional to value, coloured by source.
 *
 * Geometry conventions match the radar and the circular tree: angles run
 * CLOCKWISE from 12 o'clock, and every radius lives in points and divides
 * per axis, so the circle survives any canvas aspect. */
#include "cinderplot.h"
#include <math.h>
#include <string.h>

#define ARC_STEP 0.06            /* radians per arc sample (~3.4 degrees) */
#define BEZ_N    18              /* samples per ribbon bezier */

typedef struct { const char *name; double total, cursor; double a0, a1; } Sector;

static int sec_find(Sector *s, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].name, name)) return i;
    return -1;
}

/* append the arc from angle u0 to u1 at radius r (pt) to the path arrays */
static int arc_pts(double *px, double *py, int m, double u0, double u1,
                   double r, double cw_pt, double ch_pt) {
    int steps = (int)(fabs(u1 - u0) / ARC_STEP) + 2;
    for (int i = 0; i <= steps; i++) {
        double u = u0 + (u1 - u0) * i / steps;
        px[m] = 0.5 + r * sin(u) / cw_pt;
        py[m] = 0.5 + r * cos(u) / ch_pt;
        m++;
    }
    return m;
}

/* quadratic bezier from angle u0 to u1 (both at radius r) with the control
 * point at the circle centre — the classic chord shape */
static int bez_pts(double *px, double *py, int m, double u0, double u1,
                   double r, double cw_pt, double ch_pt) {
    double x0 = r * sin(u0), y0c = r * cos(u0);
    double x1 = r * sin(u1), y1c = r * cos(u1);
    for (int i = 1; i <= BEZ_N; i++) {
        double t = (double)i / BEZ_N, s = 1 - t;
        double x = s * s * x0 + t * t * x1;      /* control = (0, 0) */
        double y = s * s * y0c + t * t * y1c;
        px[m] = 0.5 + x / cw_pt;
        py[m] = 0.5 + y / ch_pt;
        m++;
    }
    return m;
}

int render_chord(const PlotSpec *spec, const char *data, const char *out,
                 double w_pt, double h_pt, char *err) {
    DataFrame *df = df_read_csv(data ? data : "-", err);
    if (!df) return -1;
    if (df->ncol < 3) {
        snprintf(err, CP_ERRLEN, "chord() needs a (from, to, value) table; "
                 "%s has %d column%s", data ? data : "stdin", df->ncol,
                 df->ncol == 1 ? "" : "s");
        return -1;
    }
    if (df->nrow == 0) {
        /* an empty table has no typed columns, so every check below would
         * blame the from/to types instead of the missing rows */
        snprintf(err, CP_ERRLEN, "chord(): %s has no rows", data ? data : "stdin");
        return -1;
    }
    const Column *cf2 = spec->chord_from ? df_col(df, spec->chord_from) : df_col(df, "from");
    const Column *ct2 = spec->chord_to ? df_col(df, spec->chord_to) : df_col(df, "to");
    const Column *cv2 = spec->chord_value ? df_col(df, spec->chord_value) : df_col(df, "value");
    if (!cf2 && !spec->chord_from) cf2 = &df->cols[0];   /* positional fallback */
    if (!ct2 && !spec->chord_to) ct2 = &df->cols[1];
    if (!cv2 && !spec->chord_value) cv2 = &df->cols[2];
    if (!cf2 || !ct2 || !cv2) {
        snprintf(err, CP_ERRLEN, "chord(): column `%s` not found",
                 !cf2 ? spec->chord_from : !ct2 ? spec->chord_to : spec->chord_value);
        return -1;
    }
    if (cf2->type != COL_STR || ct2->type != COL_STR) {
        /* name the column and how it was chosen: a positional pick landing on
         * a numeric column usually means the file has no from/to header */
        const Column *bad = cf2->type != COL_STR ? cf2 : ct2;
        int given = bad == cf2 ? spec->chord_from != NULL : spec->chord_to != NULL;
        snprintf(err, CP_ERRLEN, "chord(): %s column `%s` (%s) is not text; from/to "
                 "must name the sectors%s", bad == cf2 ? "from" : "to", bad->name,
                 given ? "given by name" : "taken by position, no `from`/`to` header",
                 given ? "" : " -- give from=/to=");
        return -1;
    }
    if (cv2->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "chord(): the value column `%s` must be numeric", cv2->name);
        return -1;
    }

    /* sectors: union of from/to names, in first appearance order */
    Sector *sec = cp_xcalloc(2 * df->nrow, sizeof(Sector));
    int ns = 0;
    double grand = 0;
    for (int r = 0; r < df->nrow; r++) {
        double v = cv2->num[r];
        if (isnan(v) || v <= 0) {
            snprintf(err, CP_ERRLEN, "chord(): values must be positive "
                     "(row with %s -> %s has %g)", cf2->str[r], ct2->str[r], v);
            return -1;
        }
        int a = sec_find(sec, ns, cf2->str[r]);
        if (a < 0) { sec[ns].name = cf2->str[r]; a = ns++; }
        int b = sec_find(sec, ns, ct2->str[r]);
        if (b < 0) { sec[ns].name = ct2->str[r]; b = ns++; }
        sec[a].total += v;
        sec[b].total += v;
        grand += 2 * v;
    }
    if (ns < 2) {
        snprintf(err, CP_ERRLEN, "chord() needs at least two sectors "
                 "(%d found)", ns);
        return -1;
    }

    /* ---- sector ordering: order=c(...) takes a complete permutation
     * (as levels= does — an incomplete list would silently drop flows);
     * bipartite=TRUE puts every from-sector first (file order), then every
     * to-sector, with a bigger gap between the two groups. ---- */
    int split_at = -1;                   /* first index of the to-group */
    if (spec->n_chord_order) {
        if (spec->n_chord_order != ns) {
            snprintf(err, CP_ERRLEN, "order=c(...) names %d sectors but the "
                     "data has %d; give the complete list",
                     spec->n_chord_order, ns);
            return -1;
        }
        Sector *ord2 = cp_xcalloc(ns, sizeof(Sector));
        int *seen = cp_xcalloc(ns, sizeof(int));
        for (int i = 0; i < ns; i++) {
            int j = sec_find(sec, ns, spec->chord_order[i]);
            if (j < 0) {
                snprintf(err, CP_ERRLEN, "order=c(...): `%s` is not a sector "
                         "in the data", spec->chord_order[i]);
                return -1;
            }
            /* the count check alone let a repeated name crowd out another
             * sector, whose later sec_find() == -1 indexed sec[-1] */
            if (seen[j]++) {
                snprintf(err, CP_ERRLEN, "order=c(...): `%s` is listed twice; "
                         "every sector goes exactly once", spec->chord_order[i]);
                return -1;
            }
            ord2[i] = sec[j];
        }
        free(seen);
        memcpy(sec, ord2, ns * sizeof(Sector));
        free(ord2);
    } else if (spec->chord_bipartite) {
        Sector *ord2 = cp_xcalloc(ns, sizeof(Sector));
        int m2 = 0;
        for (int r = 0; r < df->nrow; r++) {         /* from-group, file order */
            if (sec_find(ord2, m2, cf2->str[r]) >= 0) continue;
            int j = sec_find(sec, ns, cf2->str[r]);
            ord2[m2++] = sec[j];
        }
        split_at = m2;
        for (int r = 0; r < df->nrow; r++) {         /* to-group, file order */
            int at = sec_find(ord2, m2, ct2->str[r]);
            if (at >= 0) {
                if (at < split_at) {                 /* also a from-sector */
                    snprintf(err, CP_ERRLEN, "bipartite=TRUE needs disjoint "
                             "from/to names, but `%s` appears on both sides "
                             "(a self-link breaks the two groups)",
                             ct2->str[r]);
                    return -1;
                }
                continue;
            }
            int j = sec_find(sec, ns, ct2->str[r]);
            ord2[m2++] = sec[j];
        }
        if (split_at == 0 || split_at == ns) {
            snprintf(err, CP_ERRLEN, "bipartite=TRUE found only one group");
            return -1;
        }
        memcpy(sec, ord2, ns * sizeof(Sector));
        free(ord2);
    }

    /* angles: gaps between sectors, remaining circle split by total flow.
     * Under bipartite the two group boundaries take a bigger gap (circlize's
     * big.gap, 10 degrees). */
    double gap_deg = spec->chord_gap >= 0 ? spec->chord_gap : 2.0;   /* -1 = unset */
    double gap = gap_deg * M_PI / 180;
    double biggap = 10.0 * M_PI / 180;
    double gapsum = split_at > 0 ? (ns - 2) * gap + 2 * biggap : ns * gap;
    double avail = 2 * M_PI - gapsum;
    if (avail <= 0.2) {
        snprintf(err, CP_ERRLEN, "chord(): %d sectors with gap=%g degrees leave no "
                 "room for the circle (%d x %g = %g of 360)", ns, gap_deg, ns, gap_deg,
                 ns * gap_deg);
        return -1;
    }
    double a = split_at > 0 ? biggap / 2 : 0;   /* clockwise from 12 o'clock */
    for (int i = 0; i < ns; i++) {
        sec[i].a0 = a;
        sec[i].a1 = a + avail * sec[i].total / grand;
        sec[i].cursor = sec[i].a0;
        a = sec[i].a1
          + (split_at > 0 && (i + 1 == split_at || i + 1 == ns) ? biggap : gap);
    }

    /* sector colours: hue by default; scale_*_manual named values override,
     * positional lists apply in sector order */
    Col *pal = cp_xmalloc(ns * sizeof(Col));
    hue_palette(ns, pal);
    if (spec->has_manual) {
        int named = spec->n_manual > 0 && spec->manual_names[0] != NULL;
        for (int i = 0; i < ns; i++) {
            if (named) {
                for (int k = 0; k < spec->n_manual; k++)
                    if (spec->manual_names[k]
                        && !strcmp(spec->manual_names[k], sec[i].name))
                        { pal[i] = spec->manual_cols[k]; break; }
            } else if (i < spec->n_manual) pal[i] = spec->manual_cols[i];
        }
    }

    /* ---- canvas: square-ish, labels sized on a scratch surface ---- */
    cairo_surface_t *msurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(msurf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, SZ_AXIS_TEXT);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    double labh = fe.ascent + fe.descent, labw = 0;
    for (int i = 0; i < ns; i++) {
        cairo_text_extents_t e;
        cairo_text_extents(cr, sec[i].name, &e);
        if (e.x_advance > labw) labw = e.x_advance;
    }
    double titleh = spec->lab_title ? SZ_TITLE * 1.4 : 0;
    if (w_pt <= 0 && h_pt <= 0) { w_pt = 6 * 72 + 2 * labw; h_pt = 6 * 72 + 2 * labh + titleh; }
    else if (w_pt <= 0) w_pt = h_pt;
    else if (h_pt <= 0) h_pt = w_pt;
    cairo_destroy(cr); cairo_surface_destroy(msurf);

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    GTable *T = cp_xcalloc(1, sizeof(GTable));
    T->ncol = 1; T->colw[0] = unull(1);
    T->nrow = 2;
    T->rowh[0] = upt(titleh);
    T->rowh[1] = unull(1);
    double cw_pt = w_pt, ch_pt = h_pt - titleh;
    double R = fmin(cw_pt - 2 * (labw + 2 * MARGIN),
                    ch_pt - 2 * (labh + 2 * MARGIN)) / 2;
    double labsz = SZ_AXIS_TEXT;
    if (R < 40) {
        /* Too small a canvas for full-size labels around a useful circle.
         * Give the circle the least it needs (40pt radius, or less of the
         * half-size when that still leaves label room), then shrink the
         * label font to the room left, rather than draw past the canvas
         * edge. Below 5pt the labels are not legible, so stop there. */
        double half = fmin(cw_pt, ch_pt) / 2;
        R = fmax(40, fmin(half * 0.6, half - labw - 4 - MARGIN));
        double roomw = cw_pt / 2 - R - 4 - MARGIN, roomh = ch_pt / 2 - R - 4 - MARGIN;
        double f = 1;
        if (labw > roomw) f = fmin(f, roomw / labw);
        if (labh > roomh) f = fmin(f, roomh / labh);
        if (f < 1) { labsz *= f; labh *= f; }
        if (labsz < 5 || roomw <= 0 || roomh <= 0) {
            snprintf(err, CP_ERRLEN, "chord(): canvas too small for the sector labels "
                     "(widest is %.0fpt); use --size or shorter names", labw);
            return -1;
        }
    }
    double band = R * 0.07;              /* the outer sector band */
    double rin = R - band - R * 0.02;    /* ribbons end just under the band */

    Grob *g;
    if (spec->lab_title) {
        g = gt_add(T, G_TEXT, 0, 0, 0, 0);
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = C_BLACK;
        g->tx = 0.5; g->ty = 0.5; g->hj = 0.5; g->va = V_INKCENTER;
    }

    /* ---- ribbons, biggest first so small flows stay visible on top ---- */
    int *ord = cp_xmalloc(df->nrow * sizeof(int));
    for (int r = 0; r < df->nrow; r++) ord[r] = r;
    for (int i = 0; i < df->nrow; i++)          /* insertion sort, descending */
        for (int j = i; j > 0
             && cv2->num[ord[j]] > cv2->num[ord[j - 1]]; j--) {
            int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
        }
    /* sub-arc allocation must not depend on draw order: assign in INPUT
     * order first, then draw sorted */
    double *fa0 = cp_xmalloc(df->nrow * sizeof(double));
    double *fa1 = cp_xmalloc(df->nrow * sizeof(double));
    double *ta0 = cp_xmalloc(df->nrow * sizeof(double));
    double *ta1 = cp_xmalloc(df->nrow * sizeof(double));
    for (int r = 0; r < df->nrow; r++) {
        int sa = sec_find(sec, ns, cf2->str[r]);
        int sb = sec_find(sec, ns, ct2->str[r]);
        double wa = avail * cv2->num[r] / grand;
        fa0[r] = sec[sa].cursor; fa1[r] = sec[sa].cursor + wa;
        sec[sa].cursor = fa1[r];
        ta0[r] = sec[sb].cursor; ta1[r] = sec[sb].cursor + wa;
        sec[sb].cursor = ta1[r];
    }
    double alpha = spec->chord_alpha > 0 ? spec->chord_alpha : 0.6;
    for (int i = 0; i < df->nrow; i++) {
        int r = ord[i];
        int sa = sec_find(sec, ns, cf2->str[r]);
        int cap = 2 * ((int)(2 * M_PI / ARC_STEP) + 3) + 2 * BEZ_N + 4;
        double *px = cp_xmalloc(cap * sizeof(double));
        double *py = cp_xmalloc(cap * sizeof(double));
        int m = 0;
        m = arc_pts(px, py, m, fa0[r], fa1[r], rin, cw_pt, ch_pt);
        m = bez_pts(px, py, m, fa1[r], ta0[r], rin, cw_pt, ch_pt);
        m = arc_pts(px, py, m, ta0[r], ta1[r], rin, cw_pt, ch_pt);
        m = bez_pts(px, py, m, ta1[r], fa0[r], rin, cw_pt, ch_pt);
        g = gt_add(T, G_POLYGON, 1, 0, 1, 0);
        g->n = m; g->px = px; g->py = py;
        g->col = pal[sa]; g->alpha = alpha;
    }
    free(ord); free(fa0); free(fa1); free(ta0); free(ta1);

    /* ---- sector bands + labels, over the ribbon ends ---- */
    for (int i = 0; i < ns; i++) {
        int cap = 2 * ((int)((sec[i].a1 - sec[i].a0) / ARC_STEP) + 4);
        double *px = cp_xmalloc(cap * sizeof(double));
        double *py = cp_xmalloc(cap * sizeof(double));
        int m = 0;
        m = arc_pts(px, py, m, sec[i].a0, sec[i].a1, R, cw_pt, ch_pt);
        m = arc_pts(px, py, m, sec[i].a1, sec[i].a0, R - band, cw_pt, ch_pt);
        g = gt_add(T, G_POLYGON, 1, 0, 1, 0);
        g->n = m; g->px = px; g->py = py; g->col = pal[i];
        double mid = (sec[i].a0 + sec[i].a1) / 2;
        double sx2 = sin(mid), cy2 = cos(mid);
        g = gt_add(T, G_TEXT, 1, 0, 1, 0);
        g->str = sec[i].name; g->size = labsz; g->col = C_AXTXT;
        g->tx = 0.5 + (R + 4) * 1.0 * sx2 / cw_pt;
        g->ty = 0.5 + ((R + 4) * cy2 + labh * 0.55 * cy2) / ch_pt;
        g->hj = (1 - sx2) / 2;           /* right side left-anchors */
        g->va = V_INKCENTER;
    }

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
