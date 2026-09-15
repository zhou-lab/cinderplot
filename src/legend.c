/* legend.c — the fill legend, drawn as rigid chrome: a discrete KEY (one
 * swatch and label per level) or a continuous COLOURBAR (a 64-step strip with
 * its breaks). Both are emitted as ordinary grobs into one gtable cell at npc
 * positions the caller chooses, sized in physical units (ComplexHeatmap's
 * 4 mm bar, 28 mm long) that never couple to the data.
 *
 * Shared by heatmap mode (legend() placed beside a heatmap or annotation) and
 * the track browser (legend() in the right margin, keyed off a matrix()
 * track), so the two modes cannot drift: one swatch geometry, one break
 * rule, one tick-and-label layout. The callers own the title and the
 * measured margin the block sits in. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static double text_w(cairo_t *cr, double size, const char *s) {
    cairo_text_extents_t e;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

/* colourbar breaks: the extended breaks that fall within [lo, hi]. The
 * fence is a hair wider than the range, because 3 * 0.1 is 0.30000000000000004
 * and a bar for [0, 0.3] lost its top label to that ulp. */
int cp_legend_breaks(double lo, double hi, double *br) {
    int nb = extended_breaks(lo, hi, 5, br, CP_LEG_MAXBR), nf = 0;
    double eps = 1e-9 * (hi - lo);
    for (int k = 0; k < nb; k++)
        if (br[k] >= lo - eps && br[k] <= hi + eps) br[nf++] = br[k];
    return nf;
}

/* A vertical key's extent across the reading direction, in points: the
 * swatch, the gap, and the widest label. */
double cp_key_across_pt(cairo_t *cr, char *const *labels, int nlev) {
    double wmax = 0;
    for (int k = 0; k < nlev; k++) {
        double tw = text_w(cr, SZ_AXIS_TEXT, labels[k]);
        if (tw > wmax) wmax = tw;
    }
    return LEG_GRID + TXT_GAP + wmax;
}

/* A vertical colourbar's extent across the bar, in points: the bar, its tick,
 * the gap, and the widest break label. */
double cp_colourbar_across_pt(cairo_t *cr, double lo, double hi) {
    double br[CP_LEG_MAXBR];
    int nf = cp_legend_breaks(lo, hi, br);
    int dec = axis_decimals(br, nf);
    double wmax = 0;
    for (int k = 0; k < nf; k++) {
        char b[32]; fmt_break(br[k], dec, b, sizeof b);
        double tw = text_w(cr, SZ_AXIS_TEXT, b);
        if (tw > wmax) wmax = tw;
    }
    return LEG_BAR + TICK_LEN + TXT_GAP + wmax;
}

/* The key: swatches stacked downward from `top`, their left edge at `sx`,
 * each label to the right of its swatch. Coordinates are npc of cell (r, c),
 * whose size in points (cw_pt x ch_pt) converts the physical swatch sizes. */
void cp_key_draw(GTable *T, int r, int c, char *const *labels, const Col *pal,
                 int nlev, double sx, double top, double cw_pt, double ch_pt) {
    double gw = LEG_GRID / cw_pt, gh = LEG_GRID / ch_pt, gap = LEG_GAP / ch_pt;
    for (int k = 0; k < nlev; k++) {
        double y1 = top - k * (gh + gap);
        Grob *g = gt_add(T, G_RECT, r, c, r, c);
        g->sub = 1; g->col = pal[k];
        g->x0 = sx; g->x1 = sx + gw; g->y1 = y1; g->y0 = y1 - gh;
        g = gt_add(T, G_TEXT, r, c, r, c);
        g->str = labels[k]; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
        g->tx = sx + gw + TXT_GAP / cw_pt; g->ty = y1 - gh / 2; g->hj = 0; g->va = V_INKCENTER;
    }
}

/* The colourbar: LEG_LEN long, LEG_BAR thick, its lower-left corner at
 * (bx0, by0) npc of cell (r, c). `vert` stands it up (values increase
 * upward) or lays it flat (values increase rightward); `dir` says which side
 * the ticks and labels go -- +1 right of a vertical bar or above a flat one,
 * -1 left or below. Painted by VALUE through the same mapping the cells use,
 * so a gradient2 bar (piecewise about its midpoint) puts white where the
 * matrix does; the ticks stay linear in value. */
void cp_colourbar_draw(GTable *T, int r, int c, const FillScale *fs,
                       double lo, double hi, int vert, int dir,
                       double bx0, double by0, double cw_pt, double ch_pt) {
    double barT = vert ? LEG_BAR / cw_pt : LEG_BAR / ch_pt;
    double barL = vert ? LEG_LEN / ch_pt : LEG_LEN / cw_pt;
    const int NSTEP = 64;
    Grob *g;
    for (int k = 0; k < NSTEP; k++) {
        double v = lo + (hi - lo) * (k + 0.5) / NSTEP;
        g = gt_add(T, G_RECT, r, c, r, c);
        g->sub = 1; g->col = fill_map_value(fs, v, lo, hi);
        if (vert) { g->x0 = bx0; g->x1 = bx0 + barT;
                    g->y0 = by0 + barL * k / NSTEP; g->y1 = by0 + barL * (k + 1) / NSTEP; }
        else { g->y0 = by0; g->y1 = by0 + barT;
               g->x0 = bx0 + barL * k / NSTEP; g->x1 = bx0 + barL * (k + 1) / NSTEP; }
    }
    double br[CP_LEG_MAXBR];
    int nf = cp_legend_breaks(lo, hi, br);
    int dec = axis_decimals(br, nf);
    for (int k = 0; k < nf; k++) {
        double frac = hi > lo ? (br[k] - lo) / (hi - lo) : 0.5;
        char *lab = cp_xmalloc(32);
        fmt_break(br[k], dec, lab, 32);
        g = gt_add(T, G_LINE, r, c, r, c);
        g->col = C_TICK; g->lw = lw_pt(0.5);
        if (vert) {
            double y = by0 + frac * barL;
            double tx = dir > 0 ? bx0 + barT : bx0;
            g->x0 = tx; g->x1 = tx + dir * (TICK_LEN / cw_pt); g->y0 = g->y1 = y;
            g = gt_add(T, G_TEXT, r, c, r, c);
            g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            g->tx = tx + dir * ((TICK_LEN + TXT_GAP) / cw_pt); g->ty = y;
            g->hj = dir > 0 ? 0 : 1; g->va = V_INKCENTER;
        } else {
            double x = bx0 + frac * barL;
            double ty = dir > 0 ? by0 + barT : by0;
            g->y0 = ty; g->y1 = ty + dir * (TICK_LEN / ch_pt); g->x0 = g->x1 = x;
            g = gt_add(T, G_TEXT, r, c, r, c);
            g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            g->tx = x; g->ty = ty + dir * ((TICK_LEN + TXT_GAP) / ch_pt);
            g->hj = 0.5; g->va = dir > 0 ? V_BOTTOM : V_TOP;
        }
    }
}

/* ---------- the discrete fill: levels, palette, key labels ---------- */

static int cmp_num(const void *a, const void *b) {
    double d = *(const double *)a - *(const double *)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

int cp_numeric_levels(double *v, long n, char ***labels, double **values,
                      const char *what, char *err) {
    double *uv = cp_xmalloc((size_t)(HM_MAXLEV + 1) * sizeof(double));
    int nl = 0;
    for (long i = 0; i < n; i++) {
        if (isnan(v[i])) continue;
        int seen = 0;
        for (int k = 0; k < nl && !seen; k++) seen = uv[k] == v[i];
        if (seen) continue;
        if (nl == HM_MAXLEV) {
            /* A categorical fill with this many keys is not a figure a
             * reader can use, and it is far more often a continuous matrix
             * that landed here by accident. Say both. */
            snprintf(err, CP_ERRLEN, "%s: the matrix has more than %d distinct values "
                     "(the cap, as for scale_*_manual); a categorical key that long is "
                     "unreadable -- collapse the rare categories, or drop discrete=TRUE "
                     "if the values are measurements", what, HM_MAXLEV);
            free(uv);
            return -1;
        }
        uv[nl++] = v[i];
    }
    if (nl == 0) {
        snprintf(err, CP_ERRLEN, "%s: every cell of the matrix is NA, so there are no "
                 "categories to colour", what);
        free(uv);
        return -1;
    }
    qsort(uv, nl, sizeof(double), cmp_num);
    char **lv = cp_xmalloc((size_t)nl * sizeof(char *));
    for (int k = 0; k < nl; k++) { lv[k] = cp_xmalloc(32); fmt_num(uv[k], lv[k], 32); }
    /* Keyed by VALUE, not by the printed label: two near-equal doubles can
     * print the same and would otherwise collapse into one level while the
     * key still listed two. */
    for (long i = 0; i < n; i++) {
        if (isnan(v[i])) continue;
        for (int k = 0; k < nl; k++) if (uv[k] == v[i]) { v[i] = k; break; }
    }
    *labels = lv; *values = uv;
    return nl;
}

int cp_levels_apply(double *v, long n, const double *values, int nlev,
                    const char *what, char *err) {
    for (long i = 0; i < n; i++) {
        if (isnan(v[i])) continue;
        int hit = -1;
        for (int k = 0; k < nlev && hit < 0; k++) if (values[k] == v[i]) hit = k;
        if (hit < 0) {
            char b[32]; fmt_num(v[i], b, sizeof b);
            snprintf(err, CP_ERRLEN, "%s: value %s is not one of the %d levels the "
                     "key was built from", what, b, nlev);
            return -1;
        }
        v[i] = hit;
    }
    return 0;
}

/* The palette a discrete fill paints with: ggplot's hue wheel, then whatever
 * scale_fill_manual(values=) says over the top. A NAMED values= list maps the
 * levels it names and leaves the rest on their hue colour -- so a two-category
 * highlight does not have to enumerate the other eight. A POSITIONAL list is
 * read level by level and must be long enough, because a short one would paint
 * the tail levels silently, which is the failure the grammar-mode scale
 * already refuses. */
int cp_discrete_palette(const PlotSpec *spec, char *const *levels, int nlev,
                        Col *pal, char *err) {
    hue_palette(nlev, pal);
    if (!spec->has_manual) return 0;
    int named = spec->n_manual > 0 && spec->manual_names[0] != NULL;
    if (!named) {
        if (spec->n_manual < nlev) {
            if (spec->brewer_disc)
                snprintf(err, CP_ERRLEN, "palette `%s` has %d colours; the matrix has %d "
                         "categories (`%s`, ...); pick a larger palette or name the "
                         "colours with scale_fill_manual(values=)", spec->brewer_disc,
                         spec->n_manual, nlev, levels[0]);
            else
                snprintf(err, CP_ERRLEN, "scale_fill_manual(values=) gives %d colours; the "
                         "matrix has %d categories (`%s`, ...); give one colour per "
                         "category, or name them -- values=c(\"%s\"=\"red\", ...) -- and "
                         "the rest keep the default palette", spec->n_manual, nlev,
                         levels[0], levels[0]);
            return -1;
        }
        for (int i = 0; i < nlev; i++) pal[i] = spec->manual_cols[i];
        return 0;
    }
    for (int i = 0; i < nlev; i++)
        for (int k = 0; k < spec->n_manual; k++)
            if (spec->manual_names[k] && !strcmp(spec->manual_names[k], levels[i])) {
                pal[i] = spec->manual_cols[k];
                break;
            }
    return 0;
}

/* labels= renames in the KEY only; the cells are still coloured by the level
 * as written in the data, so values= and labels= key off the same string. A
 * label for a level the data does not hold is an error, not a spare entry:
 * it is nearly always a typo for a level that then keeps its raw name. */
char **cp_key_labels(const PlotSpec *spec, char *const *levels, int nlev, char *err) {
    char **out = cp_xmalloc((size_t)nlev * sizeof(char *));
    for (int i = 0; i < nlev; i++) out[i] = levels[i];
    if (!spec->n_manual_labs) return out;
    int named = spec->manual_lab_names[0] != NULL;
    for (int k = 0; k < spec->n_manual_labs; k++)
        if ((spec->manual_lab_names[k] != NULL) != named) {
            snprintf(err, CP_ERRLEN, "scale_*_manual(labels=): name every label "
                     "(labels=c(\"%s\"=\"...\", ...)) or none (one per level in key "
                     "order), not a mixture", levels[0]);
            free(out);
            return NULL;
        }
    if (!named) {
        if (spec->n_manual_labs != nlev) {
            snprintf(err, CP_ERRLEN, "scale_*_manual(labels=) gives %d labels; the key has "
                     "%d levels (`%s`, ...); give one per level, or name them -- "
                     "labels=c(\"%s\"=\"...\", ...)", spec->n_manual_labs, nlev,
                     levels[0], levels[0]);
            free(out);
            return NULL;
        }
        for (int i = 0; i < nlev; i++) out[i] = spec->manual_labs[i];
        return out;
    }
    for (int k = 0; k < spec->n_manual_labs; k++) {
        int hit = 0;
        for (int i = 0; i < nlev; i++)
            if (!strcmp(spec->manual_lab_names[k], levels[i])) { out[i] = spec->manual_labs[k]; hit = 1; }
        if (!hit) {
            /* list the levels, so the typo is visible beside what it missed */
            char have[256]; size_t at = 0;
            have[0] = 0;
            for (int i = 0; i < nlev && at < sizeof have - 8; i++)
                at += (size_t)snprintf(have + at, sizeof have - at, "%s`%s`", i ? ", " : "", levels[i]);
            snprintf(err, CP_ERRLEN, "scale_*_manual(labels=): `%s` is not a level of the "
                     "key; the data holds %s", spec->manual_lab_names[k], have);
            free(out);
            return NULL;
        }
    }
    return out;
}
