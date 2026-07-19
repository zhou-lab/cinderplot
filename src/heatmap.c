/* heatmap.c — matrix mode: a C port of wheatmap's anchor-canvas layout,
 * surfaced with ggplot-flavoured names.
 *
 * Architecture note: this is a second ASSEMBLER, not a second layout
 * engine. Placements resolve to plain npc rectangles on a canvas; the
 * canvas is one null cell of a trivial outer gtable (measured chrome),
 * and everything inside is emitted as ordinary grobs. The gtable engine
 * is unchanged.
 *
 * Placement semantics follow wheatmap R (dims.R): default anchor = the
 * previous object; auto size is DATA-PROPORTIONAL — 1/anchor_rows per
 * row of the new object, clamped by min.ratio = 0.02 — so a 1-row
 * annotation above a 20-row heatmap gets 1/20 of its height. pad is in
 * npc, as in wheatmap. After resolution the bounding box of all rects
 * is normalized back into [0,1]. */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_RATIO 0.02

/* Heatmap bodies with more than this many cells are drawn as ONE embedded
 * image (a pixel per cell, nearest-scaled) instead of N vector rectangles,
 * so the PDF and draw time stay flat for large matrices. Below it, stay
 * vector — keeps small heatmaps byte-identical. */
#define RASTER_MIN_CELLS 2000
#define RASTER_MAX_CELLS 25000000L      /* ~100 MB ARGB buffer ceiling */

/* colour -> cairo ARGB32 pixel (native-endian 0xAARRGGBB, opaque) */
static uint32_t col_argb(Col c) {
    int r = (int)(c.r * 255 + 0.5), g = (int)(c.g * 255 + 0.5), b = (int)(c.b * 255 + 0.5);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Legends are RIGID chrome, sized in physical units (never coupled to the
 * matrix), following ComplexHeatmap: bar 4mm thick, ~28mm long. */
#define MM       (72.0 / 25.4)
#define LEG_BAR  (4.0 * MM)         /* colorbar thickness  */
#define LEG_LEN  (28.0 * MM)        /* colorbar length     */
#define LEG_GRID (4.0 * MM)         /* discrete key square */
#define LEG_GAP  (2.0 * MM)         /* gap between keys    */

typedef struct {
    const HMObj *o;
    double l, b, w, h;              /* npc canvas rect (y up) */
    int nr, nc;
    Matrix *m;                      /* heatmap */
    int *roword, *coword;           /* heatmap: display slot -> data index */
    HClust *rowclust, *colclust;    /* heatmap: trees (NULL if unclustered) */
    int ann_n, ann_horiz;           /* annotation */
    Col *ann_col;                   /* data order */
    int *ann_ord;                   /* annotation: display slot -> data index */
    Factor *ann_f; Col *ann_pal;    /* annotation: discrete levels + colours */
    const char *ann_name;           /* annotation: source column name */
    int ann_continuous;             /* annotation: numeric (own colorbar)? */
    double ann_dmin, ann_dmax;      /* annotation: continuous scale range */
    FillScale ann_fill;             /* annotation: continuous colormap */
    int target, leg_discrete;       /* legend: source obj index; discrete key? */
    HClust *tree; int dir, nleaf;   /* dendrogram */
    int *slot;                      /* dendrogram: data index -> display slot */
} RObj;

/* dendrogram orientations */
enum { DEND_LEFT, DEND_RIGHT, DEND_TOP, DEND_BENEATH };

/* cluster the rows (dim==0) or cols (dim==1) of m; returns tree, fills
 * a freshly-malloc'd display-order array into *ord (display slot -> data) */
static HClust *cluster_dim(const Matrix *m, int dim, int **ord, char *err) {
    int n = dim == 0 ? m->nr : m->nc;
    int p = dim == 0 ? m->nc : m->nr;
    double *obs = malloc((size_t)n * p * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int k = 0; k < p; k++)
            obs[(size_t)i * p + k] = dim == 0 ? m->v[(size_t)i * m->nc + k]
                                              : m->v[(size_t)k * m->nc + i];
    HClust *h = hclust_ward(obs, n, p, err);
    free(obs);
    if (!h) return NULL;
    *ord = malloc(n * sizeof(int));
    memcpy(*ord, h->order, n * sizeof(int));
    return h;
}

static int *identity(int n) {
    int *v = malloc(n * sizeof(int));
    for (int i = 0; i < n; i++) v[i] = i;
    return v;
}

static Matrix *matrix_from_df(const DataFrame *df, char *err) {
    int c0 = df->ncol && df->cols[0].type == COL_STR ? 1 : 0;
    Matrix *m = malloc(sizeof *m);
    m->nr = df->nrow;
    m->nc = df->ncol - c0;
    if (m->nc < 1) { sprintf(err, "matrix csv has no numeric columns"); return NULL; }
    m->rn = c0 ? df->cols[0].str : NULL;
    m->cn = malloc(m->nc * sizeof(char *));
    m->v = malloc((size_t)m->nr * m->nc * sizeof(double));
    for (int c = 0; c < m->nc; c++) {
        const Column *col = &df->cols[c + c0];
        if (col->type != COL_NUM) {
            sprintf(err, "matrix column `%s` is not numeric", col->name);
            return NULL;
        }
        m->cn[c] = col->name;
        for (int r = 0; r < df->nrow; r++)
            m->v[(size_t)r * m->nc + c] = col->num[r];
    }
    return m;
}

static double text_w(cairo_t *cr, double size, const char *s) {
    cairo_text_extents_t e;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

static double clampr(double v) {
    return v < MIN_RATIO ? MIN_RATIO : v > 1.0 / MIN_RATIO ? 1.0 / MIN_RATIO : v;
}

/* legend title: explicit title= wins, else the fill label (continuous) or
 * the annotation column name (discrete); NULL = no title drawn */
static const char *legend_title(const PlotSpec *spec, const RObj *lg, const RObj *src) {
    if (lg->o->title) return lg->o->title;
    if (src->o->type == HM_ANNOTATION) return src->ann_name;
    return spec->lab_fill;
}

/* the scale a continuous legend draws: a numeric annotation uses its own
 * range + colormap; a heatmap target uses the shared fill scale */
static void legend_scale(const RObj *tg, const PlotSpec *spec,
                         double hmin, double hmax,
                         double *lo, double *hi, const FillScale **fs) {
    if (tg->o->type == HM_ANNOTATION && tg->ann_continuous) {
        *lo = tg->ann_dmin; *hi = tg->ann_dmax; *fs = &tg->ann_fill;
    } else {
        *lo = hmin; *hi = hmax; *fs = &spec->fill;
    }
}

/* Resolve an anchor reference. An explicit object name= always wins;
 * failing that, an annotation may be referenced by its data column name
 * (the ergonomic case). Column names are matched as exact bytes, so a
 * name with spaces or punctuation is fine as long as it was quoted in
 * the DSL (right_of("odd name (v2)")). Explicit-name-first keeps a
 * column that happens to collide with an object name unambiguous. */
static int find_obj(RObj *ro, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(ro[i].o->name, name)) return i;
    for (int i = 0; i < n; i++)
        if (ro[i].o->type == HM_ANNOTATION && ro[i].ann_name
            && !strcmp(ro[i].ann_name, name)) return i;
    return -1;
}

/* map (leaf fraction lf in [0,1], depth fraction df in [0,1]) to canvas
 * npc for a dendrogram of the given orientation and rect. df=0 is the
 * leaf edge (touching the heatmap); df=1 is the root. */
static void dend_xy(const RObj *r, double lf, double df, double *x, double *y) {
    switch (r->dir) {
    case DEND_LEFT:    *x = r->l + r->w * (1 - df); *y = r->b + r->h * (1 - lf); break;
    case DEND_RIGHT:   *x = r->l + r->w * df;       *y = r->b + r->h * (1 - lf); break;
    case DEND_TOP:     *x = r->l + r->w * lf;       *y = r->b + r->h * df;       break;
    default:           *x = r->l + r->w * lf;       *y = r->b + r->h * (1 - df); break;
    }
}

static void dend_seg(GTable *T, int CR, int CC, const RObj *r,
                     double lf0, double df0, double lf1, double df1) {
    double x0, y0, x1, y1;
    dend_xy(r, lf0, df0, &x0, &y0);
    dend_xy(r, lf1, df1, &x1, &y1);
    Grob *g = gt_add(T, G_LINE, CR, CC, CR, CC);
    g->col = C_BLACK; g->lw = lw_pt(0.5);
    g->x0 = x0; g->y0 = y0; g->x1 = x1; g->y1 = y1;
}

/* draw the tree as inverted-U brackets; positions bottom-up over merges */
static void draw_dendro(GTable *T, int CR, int CC, const RObj *r) {
    HClust *t = r->tree;
    int nn = t->n;
    double maxh = nn > 1 ? t->height[nn - 2] : 1;
    if (maxh <= 0) maxh = 1;
    double *npos = malloc((nn - 1) * sizeof(double));   /* node leaf-fraction */
    for (int s = 0; s < nn - 1; s++) {
        int a = t->merge[s][0], b = t->merge[s][1];
        double pa = a < 0 ? (r->slot[-a - 1] + 0.5) / nn : npos[a - 1];
        double pb = b < 0 ? (r->slot[-b - 1] + 0.5) / nn : npos[b - 1];
        double ha = a < 0 ? 0 : t->height[a - 1] / maxh;
        double hb = b < 0 ? 0 : t->height[b - 1] / maxh;
        double hs = t->height[s] / maxh;
        dend_seg(T, CR, CC, r, pa, ha, pa, hs);         /* riser A */
        dend_seg(T, CR, CC, r, pb, hb, pb, hs);         /* riser B */
        dend_seg(T, CR, CC, r, pa, hs, pb, hs);         /* crossbar */
        npos[s] = (pa + pb) / 2;
    }
    free(npos);
}

/* one legend's block height in npc (content + title), vertical stacking */
static double legend_block_h(const RObj *r, const RObj *tg, const char *title,
                             double baseH, double ch_pt) {
    double titleSpace = title ? (baseH + TXT_GAP) / ch_pt : 0;
    double content = r->leg_discrete
        ? tg->ann_f->nlev * LEG_GRID / ch_pt + (tg->ann_f->nlev - 1) * LEG_GAP / ch_pt
        : LEG_LEN / ch_pt;
    return content + titleSpace;
}

/* draw one legend. Vertical legends (right/left) are drawn from `blockTop`
 * (npc y of the block's top) downward, so a caller can stack them.
 * Horizontal legends (top/beneath) centre themselves on the target. `shift`
 * (pt) pushes the whole legend outward past same-side row/col labels. */
static void draw_one_legend(GTable *T, const RObj *r, const RObj *tg,
                            const PlotSpec *spec, double dmin, double dmax,
                            const char *title, double baseH,
                            double cw_pt, double ch_pt, double blockTop,
                            double shift) {
#define LPTX(pt) ((pt) / cw_pt)
#define LPTY(pt) ((pt) / ch_pt)
    Grob *g;
    PlaceKind pk = r->o->place.kind;
    int vert = pk == PL_RIGHT_OF || pk == PL_LEFT_OF;
    double gapx = HALF_LINE + shift;                /* gap from canvas edge, incl. label shift */
    double titleSpace = title ? LPTY(baseH + TXT_GAP) : 0;
    double topY, leftX;

    if (r->leg_discrete) {                          /* categorical key */
        Factor *f = tg->ann_f; Col *pal = tg->ann_pal;
        double gw = LPTX(LEG_GRID), gh = LPTY(LEG_GRID), gap = LPTY(LEG_GAP);
        double topEdge = blockTop - titleSpace;
        double sx = pk == PL_RIGHT_OF ? 1 + LPTX(gapx) : 0 - LPTX(gapx) - gw;
        for (int k = 0; k < f->nlev; k++) {
            double y1 = topEdge - k * (gh + gap);
            g = gt_add(T, G_RECT, T->nrow - 2, 1, T->nrow - 2, 1);
            g->sub = 1; g->col = pal[k];
            g->x0 = sx; g->x1 = sx + gw; g->y1 = y1; g->y0 = y1 - gh;
            g = gt_add(T, G_TEXT, T->nrow - 2, 1, T->nrow - 2, 1);
            g->str = f->levels[k]; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            g->tx = sx + gw + LPTX(TXT_GAP); g->ty = y1 - gh / 2; g->hj = 0; g->va = V_INKCENTER;
        }
        topY = blockTop; leftX = sx;
    } else {                                        /* continuous colorbar */
        double lo, hi; const FillScale *fs;
        legend_scale(tg, spec, dmin, dmax, &lo, &hi, &fs);
        double barT = vert ? LPTX(LEG_BAR) : LPTY(LEG_BAR);
        double barL = vert ? LPTY(LEG_LEN) : LPTX(LEG_LEN);
        const int NSTEP = 64, RR = T->nrow - 2, CCc = 1;
        double bx0, by0;
        if (vert) {
            by0 = blockTop - titleSpace - barL;
            bx0 = pk == PL_RIGHT_OF ? 1 + LPTX(gapx) : 0 - LPTX(gapx) - barT;
        } else {
            double xc = tg->l + tg->w / 2;
            double hts = title ? LPTY(baseH + TXT_GAP) : 0;
            bx0 = xc - barL / 2;
            by0 = pk == PL_BENEATH ? 0 - LPTY(gapx) - hts - barT
                                   : 1 + LPTY(gapx) + hts;
        }
        for (int k = 0; k < NSTEP; k++) {
            g = gt_add(T, G_RECT, RR, CCc, RR, CCc);
            g->sub = 1; g->col = fill_map(fs, (k + 0.5) / NSTEP);
            if (vert) { g->x0 = bx0; g->x1 = bx0 + barT;
                        g->y0 = by0 + barL * k / NSTEP; g->y1 = by0 + barL * (k + 1) / NSTEP; }
            else { g->y0 = by0; g->y1 = by0 + barT;
                   g->x0 = bx0 + barL * k / NSTEP; g->x1 = bx0 + barL * (k + 1) / NSTEP; }
        }
        double br[16];
        int nb = extended_breaks(lo, hi, 5, br, 16), nf = 0;
        for (int k = 0; k < nb; k++) if (br[k] >= lo && br[k] <= hi) br[nf++] = br[k];
        int dec = axis_decimals(br, nf);
        for (int k = 0; k < nf; k++) {
            double frac = hi > lo ? (br[k] - lo) / (hi - lo) : 0.5;
            char *lab = malloc(32);
            fmt_break(br[k], dec, lab);
            g = gt_add(T, G_LINE, RR, CCc, RR, CCc);
            g->col = C_TICK; g->lw = lw_pt(0.5);
            if (vert) {
                double y = by0 + frac * barL;
                double tx = pk == PL_RIGHT_OF ? bx0 + barT : bx0, dir = pk == PL_RIGHT_OF ? 1 : -1;
                g->x0 = tx; g->x1 = tx + dir * LPTX(TICK_LEN); g->y0 = g->y1 = y;
                g = gt_add(T, G_TEXT, RR, CCc, RR, CCc);
                g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                g->tx = tx + dir * LPTX(TICK_LEN + TXT_GAP); g->ty = y;
                g->hj = pk == PL_RIGHT_OF ? 0 : 1; g->va = V_INKCENTER;
            } else {
                double x = bx0 + frac * barL;
                double ty = pk == PL_BENEATH ? by0 : by0 + barT, dir = pk == PL_BENEATH ? -1 : 1;
                g->y0 = ty; g->y1 = ty + dir * LPTY(TICK_LEN); g->x0 = g->x1 = x;
                g = gt_add(T, G_TEXT, RR, CCc, RR, CCc);
                g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                g->tx = x; g->ty = ty + dir * LPTY(TICK_LEN + TXT_GAP);
                g->hj = 0.5; g->va = pk == PL_BENEATH ? V_TOP : V_BOTTOM;
            }
        }
        topY = vert ? blockTop : by0 + barT;
        leftX = bx0;
    }
    if (title) {
        g = gt_add(T, G_TEXT, T->nrow - 2, 1, T->nrow - 2, 1);
        g->str = title; g->size = SZ_BASE; g->col = C_BLACK;
        g->tx = leftX; g->ty = vert ? topY : topY + LPTY(TXT_GAP);
        g->hj = 0; g->va = vert ? V_TOP : V_BOTTOM;
    }
#undef LPTX
#undef LPTY
}

int render_heatmap(const PlotSpec *spec, const char *out,
                   double w_pt, double h_pt, char *err) {
    RObj ro[MAX_HMOBJS];
    memset(ro, 0, sizeof ro);
    int n = spec->nhobjs;

    /* ---- load data & resolve placements sequentially ---- */
    for (int i = 0; i < n; i++) {
        const HMObj *o = &spec->hobjs[i];
        ro[i].o = o;
        if (o->type == HM_HEATMAP) {
            const char *path = o->data ? o->data : spec->data_path;
            DataFrame *df = df_read_csv(path, err);
            if (!df) return -1;
            if (!(ro[i].m = matrix_from_df(df, err))) return -1;
            ro[i].nr = ro[i].m->nr;
            ro[i].nc = ro[i].m->nc;
            ro[i].roword = identity(ro[i].nr);
            ro[i].coword = identity(ro[i].nc);
            if (o->cluster == CL_ROWS || o->cluster == CL_BOTH) {
                free(ro[i].roword);
                if (!(ro[i].rowclust = cluster_dim(ro[i].m, 0, &ro[i].roword, err))) return -1;
            }
            if (o->cluster == CL_COLS || o->cluster == CL_BOTH) {
                free(ro[i].coword);
                if (!(ro[i].colclust = cluster_dim(ro[i].m, 1, &ro[i].coword, err))) return -1;
            }
        }

        /* anchor */
        RObj *a = NULL;
        const HPlace *pl = &o->place;
        if (pl->kind != PL_FULL) {
            if (pl->anchor) {
                int ai = find_obj(ro, i, pl->anchor);
                if (ai < 0) { sprintf(err, "unknown object `%s` in placement", pl->anchor); return -1; }
                a = &ro[ai];
            } else if (i > 0) a = &ro[i - 1];
            else { sprintf(err, "first object cannot have a relative placement"); return -1; }
        }

        if (o->type == HM_ANNOTATION) {
            DataFrame *df = df_read_csv(o->data, err);
            if (!df) return -1;
            const Column *col = &df->cols[df->ncol - 1];
            ro[i].ann_n = df->nrow;
            ro[i].ann_horiz = pl->kind == PL_TOP_OF || pl->kind == PL_BENEATH;
            int need = ro[i].ann_horiz ? a->nc : a->nr;
            if (df->nrow != need) {
                sprintf(err, "annotation `%s` has %d values; anchor `%s` needs %d",
                        o->data, df->nrow, a->o->name, need);
                return -1;
            }
            ro[i].ann_col = malloc(df->nrow * sizeof(Col));
            ro[i].ann_name = col->name;
            if (col->type == COL_STR) {
                Factor *f = factor_make(df, col);
                Col *pal = malloc(f->nlev * sizeof(Col));
                hue_palette(f->nlev, pal);
                for (int r = 0; r < df->nrow; r++)
                    ro[i].ann_col[r] = f->idx[r] >= 0 ? pal[f->idx[r]] : C_NA;
                ro[i].ann_f = f; ro[i].ann_pal = pal;   /* for a discrete legend */
            } else {
                double lo = 1e300, hi = -1e300;
                for (int r = 0; r < df->nrow; r++) {
                    if (isnan(col->num[r])) continue;
                    if (col->num[r] < lo) lo = col->num[r];
                    if (col->num[r] > hi) hi = col->num[r];
                }
                FillScale vir = {0};
                vir.kind = FILL_VIRIDIS;
                for (int r = 0; r < df->nrow; r++)
                    ro[i].ann_col[r] = isnan(col->num[r]) ? C_NA
                                     : fill_map_value(&vir, col->num[r], lo, hi);
                ro[i].ann_continuous = 1;           /* own colorbar scale */
                ro[i].ann_dmin = lo; ro[i].ann_dmax = hi; ro[i].ann_fill = vir;
            }
            ro[i].nr = ro[i].ann_horiz ? 1 : ro[i].ann_n;
            ro[i].nc = ro[i].ann_horiz ? ro[i].ann_n : 1;
            /* inherit the anchor heatmap's ordering (col order if the
             * annotation runs horizontally, row order if vertically) */
            if (a->o->type == HM_HEATMAP)
                ro[i].ann_ord = ro[i].ann_horiz ? a->coword : a->roword;
            if (!ro[i].ann_ord) ro[i].ann_ord = identity(ro[i].ann_n);
        }
        if (o->type == HM_DENDROGRAM) {
            if (!a || a->o->type != HM_HEATMAP) {
                sprintf(err, "dendrogram() must be placed relative to a heatmap");
                return -1;
            }
            int horiz = pl->kind == PL_TOP_OF || pl->kind == PL_BENEATH;
            HClust *tree = horiz ? a->colclust : a->rowclust;
            if (!tree) {
                sprintf(err, "dendrogram() needs the heatmap `%s` clustered on that axis "
                             "(add cluster=%s)", a->o->name, horiz ? "cols" : "rows");
                return -1;
            }
            ro[i].tree = tree;
            ro[i].nleaf = tree->n;
            ro[i].dir = pl->kind == PL_LEFT_OF ? DEND_LEFT
                      : pl->kind == PL_RIGHT_OF ? DEND_RIGHT
                      : pl->kind == PL_TOP_OF ? DEND_TOP : DEND_BENEATH;
            int *word = horiz ? a->coword : a->roword;
            ro[i].slot = malloc(tree->n * sizeof(int));
            for (int s = 0; s < tree->n; s++) ro[i].slot[word[s]] = s;
            ro[i].nr = 1; ro[i].nc = 1;
        }
        if (o->type == HM_LEGEND) {
            ro[i].nr = 1; ro[i].nc = 1;
            ro[i].target = -1;
            /* anchor decides what the legend describes: a categorical
             * annotation -> discrete key; a numeric annotation -> its own
             * colorbar; a heatmap -> the shared fill colorbar */
            if (a && a->o->type == HM_ANNOTATION && a->ann_f) {
                ro[i].target = (int)(a - ro); ro[i].leg_discrete = 1;
            } else if (a && a->o->type == HM_ANNOTATION && a->ann_continuous) {
                ro[i].target = (int)(a - ro);
            } else if (a && a->o->type == HM_HEATMAP) {
                ro[i].target = (int)(a - ro);
            } else {
                for (int k = 0; k < i; k++)
                    if (ro[k].o->type == HM_HEATMAP) { ro[i].target = k; break; }
            }
            if (ro[i].target < 0) {
                sprintf(err, "legend() found nothing to describe; place it relative to a "
                             "heatmap or an annotation");
                return -1;
            }
            if (ro[i].leg_discrete && (pl->kind == PL_TOP_OF || pl->kind == PL_BENEATH)) {
                sprintf(err, "discrete legends must be vertical; use right_of()/left_of()");
                return -1;
            }
            /* rigid chrome placed in the margin; not a canvas rect */
            continue;
        }

        /* rect (wheatmap generator semantics) */
        double W, H;
        switch (pl->kind) {
        case PL_FULL:
            ro[i].l = 0; ro[i].b = 0; ro[i].w = 1; ro[i].h = 1;
            break;
        case PL_TOP_OF:
        case PL_BENEATH:
            H = pl->height >= 0 ? pl->height
              : o->type == HM_LEGEND ? a->h
              : o->type == HM_DENDROGRAM ? 0.2 * a->h
              : clampr((double)ro[i].nr / a->nr) * a->h;
            ro[i].l = a->l; ro[i].w = a->w; ro[i].h = H;
            ro[i].b = pl->kind == PL_TOP_OF ? a->b + a->h + pl->pad
                                            : a->b - pl->pad - H;
            break;
        case PL_RIGHT_OF:
        case PL_LEFT_OF:
            W = pl->width >= 0 ? pl->width
              : o->type == HM_LEGEND ? 0.05
              : o->type == HM_DENDROGRAM ? 0.2 * a->w
              : clampr((double)ro[i].nc / a->nc) * a->w;
            ro[i].b = a->b; ro[i].h = a->h; ro[i].w = W;
            ro[i].l = pl->kind == PL_RIGHT_OF ? a->l + a->w + pl->pad
                                              : a->l - pl->pad - W;
            break;
        }
    }

    /* ---- normalize bounding box into [0,1] (legends are margin chrome) ---- */
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int i = 0; i < n; i++) {
        if (ro[i].o->type == HM_LEGEND) continue;
        if (ro[i].l < x0) x0 = ro[i].l;
        if (ro[i].l + ro[i].w > x1) x1 = ro[i].l + ro[i].w;
        if (ro[i].b < y0) y0 = ro[i].b;
        if (ro[i].b + ro[i].h > y1) y1 = ro[i].b + ro[i].h;
    }
    for (int i = 0; i < n; i++) {
        if (ro[i].o->type == HM_LEGEND) continue;
        ro[i].l = (ro[i].l - x0) / (x1 - x0);
        ro[i].w = ro[i].w / (x1 - x0);
        ro[i].b = (ro[i].b - y0) / (y1 - y0);
        ro[i].h = ro[i].h / (y1 - y0);
    }

    /* ---- shared fill scale range over all heatmaps ---- */
    double dmin = 1e300, dmax = -1e300;
    long ncells = 0;
    for (int i = 0; i < n; i++) {
        if (ro[i].o->type != HM_HEATMAP) continue;
        Matrix *m = ro[i].m;
        ncells += (long)m->nr * m->nc;
        for (long k = 0; k < (long)m->nr * m->nc; k++) {
            if (isnan(m->v[k])) continue;
            if (m->v[k] < dmin) dmin = m->v[k];
            if (m->v[k] > dmax) dmax = m->v[k];
        }
    }
    if (ncells > RASTER_MAX_CELLS) {
        sprintf(err, "matrix too large (%ld cells > %ld); downsample first", ncells, RASTER_MAX_CELLS);
        return -1;
    }
    if (dmin > dmax) { sprintf(err, "no finite values in matrix"); return -1; }

    /* ---- outer gtable: MEASURED chrome, canvas as the null cell ----
     * wheatmap's auto_margin: measure every outward-facing label and
     * reserve it as a page margin so nothing clips at the device edge.
     * Boundary test: a label reserves margin only when its object's edge
     * on that side sits at the normalized bbox boundary (0 or 1). */
    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_font_extents_t fe;
    cairo_set_font_size(cr, SZ_AXIS_TEXT); cairo_font_extents(cr, &fe);
    double axH = fe.ascent + fe.descent;         /* tick/label line height */
    cairo_set_font_size(cr, SZ_BASE); cairo_font_extents(cr, &fe);
    double baseH = fe.ascent + fe.descent;       /* legend title line height */
    cairo_set_font_size(cr, SZ_TITLE); cairo_font_extents(cr, &fe);
    double titleh = spec->lab_title ? fe.ascent + fe.descent : 0;

    double marL = MARGIN, marR = MARGIN, marT = MARGIN, marB = MARGIN;
    /* how far row/col name labels stick out on each side (pt). Legends on the
     * same side are then shifted OUTWARD past them (wheatmap's sibling-shift)
     * so a legend beside labelled rows clears the labels instead of overlapping. */
    double shiftL = 0, shiftR = 0, shiftT = 0, shiftB = 0;
    const double EPS = 1e-6;

    /* pass A: row/col name label extents (they sit against the heatmap edge) */
    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type != HM_HEATMAP) continue;
        Matrix *m = r->m;
        if (r->o->rownames && m->rn) {
            double wmax = 0;
            for (int k = 0; k < m->nr; k++) {
                double tw = text_w(cr, SZ_AXIS_TEXT, m->rn[k]);
                if (tw > wmax) wmax = tw;
            }
            double e = TXT_GAP + wmax;
            if (r->o->rownames == SIDE_RIGHT && fabs(r->l + r->w - 1) < EPS) shiftR = fmax(shiftR, e);
            if (r->o->rownames == SIDE_LEFT  && fabs(r->l) < EPS)            shiftL = fmax(shiftL, e);
        }
        if (r->o->colnames && m->cn) {
            double wmax = 0;
            for (int k = 0; k < m->nc; k++) {
                double tw = text_w(cr, SZ_AXIS_TEXT, m->cn[k]);
                if (tw > wmax) wmax = tw;
            }
            double e = TXT_GAP + wmax;   /* rotated 90 -> text width along the axis */
            if (r->o->colnames == SIDE_BOTTOM && fabs(r->b) < EPS)             shiftB = fmax(shiftB, e);
            if (r->o->colnames == SIDE_TOP    && fabs(r->b + r->h - 1) < EPS)  shiftT = fmax(shiftT, e);
        }
    }
    marL = fmax(marL, MARGIN + shiftL);
    marR = fmax(marR, MARGIN + shiftR);
    marT = fmax(marT, MARGIN + shiftT);
    marB = fmax(marB, MARGIN + shiftB);

    /* pass B: legend footprints, reserved OUTSIDE any labels on the same side */
    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type != HM_LEGEND) continue;
        PlaceKind pk = r->o->place.kind;
        const char *title = legend_title(spec, r, &ro[r->target]);
        double titleW = title ? text_w(cr, SZ_BASE, title) : 0;
        double across;                       /* extent perpendicular to bar */
        if (r->leg_discrete) {
            double wmax = 0;
            Factor *f = ro[r->target].ann_f;
            for (int k = 0; k < f->nlev; k++) {
                double tw = text_w(cr, SZ_AXIS_TEXT, f->levels[k]);
                if (tw > wmax) wmax = tw;
            }
            across = fmax(LEG_GRID + TXT_GAP + wmax, titleW);
        } else {
            double lo, hi; const FillScale *fs;
            legend_scale(&ro[r->target], spec, dmin, dmax, &lo, &hi, &fs);
            double br[16];
            int nb = extended_breaks(lo, hi, 5, br, 16), nf = 0;
            for (int k = 0; k < nb; k++) if (br[k] >= lo && br[k] <= hi) br[nf++] = br[k];
            int dec = axis_decimals(br, nf);
            double wmax = 0;
            for (int k = 0; k < nf; k++) {
                char b[32]; fmt_break(br[k], dec, b);
                double tw = text_w(cr, SZ_AXIS_TEXT, b);
                if (tw > wmax) wmax = tw;
            }
            double barW = LEG_BAR + TICK_LEN + TXT_GAP + wmax;
            across = (pk == PL_BENEATH || pk == PL_TOP_OF)
                   ? LEG_BAR + TICK_LEN + TXT_GAP + axH
                     + (title ? baseH + TXT_GAP : 0)                          /* height */
                   : fmax(barW, titleW);
        }
        double shift = pk == PL_RIGHT_OF ? shiftR : pk == PL_LEFT_OF ? shiftL
                     : pk == PL_BENEATH ? shiftB : shiftT;
        double ext = MARGIN + shift + HALF_LINE + across;
        if (pk == PL_RIGHT_OF) marR = fmax(marR, ext);
        else if (pk == PL_LEFT_OF) marL = fmax(marL, ext);
        else if (pk == PL_BENEATH) marB = fmax(marB, ext);
        else marT = fmax(marT, ext);
    }

    double titlegap = titleh ? HALF_LINE : 0;
    GTable *T = calloc(1, sizeof(GTable));
    T->ncol = 3;
    T->colw[0] = upt(marL);
    T->colw[1] = unull(1);
    T->colw[2] = upt(marR);
    T->nrow = 5;
    T->rowh[0] = upt(marT);                        /* top auto-margin */
    T->rowh[1] = upt(titleh);
    T->rowh[2] = upt(titlegap);
    T->rowh[3] = unull(1);                         /* canvas */
    T->rowh[4] = upt(marB);
    const int CR = 3, CC = 1;                      /* canvas cell */

    /* final canvas cell size in pt, for pt->npc conversions inside it */
    double cw_pt = w_pt - marL - marR;
    double ch_pt = h_pt - marT - titleh - titlegap - marB;
#define PTX(pt) ((pt) / cw_pt)
#define PTY(pt) ((pt) / ch_pt)

    Grob *g;
    if (spec->lab_title) {
        g = gt_add(T, G_TEXT, 1, CC, 1, CC);
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = C_BLACK;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }

    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type == HM_HEATMAP) {
            Matrix *m = r->m;
            if ((long)m->nr * m->nc > RASTER_MIN_CELLS) {
                /* raster: fill an ARGB image (one pixel per display cell)
                 * and embed it as a single grob */
                int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, m->nc);
                unsigned char *buf = malloc((size_t)m->nr * stride);
                for (int rr = 0; rr < m->nr; rr++) {
                    uint32_t *row = (uint32_t *)(buf + (size_t)rr * stride);
                    for (int cc = 0; cc < m->nc; cc++) {
                        double v = m->v[(size_t)r->roword[rr] * m->nc + r->coword[cc]];
                        row[cc] = isnan(v) ? col_argb(C_NA)
                                : col_argb(fill_map_value(&spec->fill, v, dmin, dmax));
                    }
                }
                g = gt_add(T, G_IMAGE, CR, CC, CR, CC);
                g->img = buf; g->img_w = m->nc; g->img_h = m->nr; g->clip = 1;
                g->x0 = r->l; g->x1 = r->l + r->w;
                g->y0 = r->b; g->y1 = r->b + r->h;
            } else {
                double cw = r->w / m->nc, chh = r->h / m->nr;
                for (int rr = 0; rr < m->nr; rr++)
                    for (int cc = 0; cc < m->nc; cc++) {
                        /* display slot (rr,cc) shows the clustered-order cell */
                        double v = m->v[(size_t)r->roword[rr] * m->nc + r->coword[cc]];
                        g = gt_add(T, G_RECT, CR, CC, CR, CC);
                        g->sub = 1;
                        g->col = isnan(v) ? C_NA : fill_map_value(&spec->fill, v, dmin, dmax);
                        g->x0 = r->l + cc * cw; g->x1 = g->x0 + cw;
                        g->y1 = r->b + r->h - rr * chh; g->y0 = g->y1 - chh;
                    }
            }
        } else if (r->o->type == HM_DENDROGRAM) {
            draw_dendro(T, CR, CC, r);
        } else if (r->o->type == HM_ANNOTATION) {
            if (r->ann_n > RASTER_MIN_CELLS) {
                /* raster: a 1xN (horizontal) or Nx1 (vertical) image, same
                 * engine as the heatmap body */
                int iw = r->ann_horiz ? r->ann_n : 1;
                int ih = r->ann_horiz ? 1 : r->ann_n;
                int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, iw);
                unsigned char *buf = malloc((size_t)ih * stride);
                for (int k = 0; k < r->ann_n; k++) {
                    uint32_t px = col_argb(r->ann_col[r->ann_ord[k]]);
                    if (r->ann_horiz) ((uint32_t *)buf)[k] = px;   /* slot k -> column k */
                    else *(uint32_t *)(buf + (size_t)k * stride) = px;  /* slot 0 at top */
                }
                g = gt_add(T, G_IMAGE, CR, CC, CR, CC);
                g->img = buf; g->img_w = iw; g->img_h = ih; g->clip = 1;
                g->x0 = r->l; g->x1 = r->l + r->w;
                g->y0 = r->b; g->y1 = r->b + r->h;
            } else {
                for (int k = 0; k < r->ann_n; k++) {
                    g = gt_add(T, G_RECT, CR, CC, CR, CC);
                    g->sub = 1;
                    g->col = r->ann_col[r->ann_ord[k]];   /* inherited order */
                    if (r->ann_horiz) {
                        double cw = r->w / r->ann_n;
                        g->x0 = r->l + k * cw; g->x1 = g->x0 + cw;
                        g->y0 = r->b; g->y1 = r->b + r->h;
                    } else {
                        double chh = r->h / r->ann_n;
                        g->y1 = r->b + r->h - k * chh; g->y0 = g->y1 - chh;
                        g->x0 = r->l; g->x1 = r->l + r->w;
                    }
                }
            }
        }
        /* HM_LEGEND is drawn separately, as rigid margin chrome, below */
    }

    /* ---- rigid legends: vertical legends STACK in their side margin,
     * centred as a group on the canvas; horizontal legends centre on
     * the target (ComplexHeatmap-style legend packing) ---- */
    for (int pass = 0; pass < 2; pass++) {          /* right side, then left */
        PlaceKind want = pass == 0 ? PL_RIGHT_OF : PL_LEFT_OF;
        double shift = pass == 0 ? shiftR : shiftL;
        int idx[MAX_HMOBJS], ni = 0;
        for (int i = 0; i < n; i++)
            if (ro[i].o->type == HM_LEGEND && ro[i].o->place.kind == want) idx[ni++] = i;
        if (!ni) continue;
        double heights[MAX_HMOBJS], total = 0, inter = PTY(2 * HALF_LINE);
        for (int j = 0; j < ni; j++) {
            RObj *r = &ro[idx[j]];
            const char *title = legend_title(spec, r, &ro[r->target]);
            heights[j] = legend_block_h(r, &ro[r->target], title, baseH, ch_pt);
            total += heights[j];
        }
        total += (ni - 1) * inter;
        double top = 0.5 + total / 2;               /* top of the stack */
        for (int j = 0; j < ni; j++) {
            RObj *r = &ro[idx[j]];
            const char *title = legend_title(spec, r, &ro[r->target]);
            draw_one_legend(T, r, &ro[r->target], spec, dmin, dmax, title,
                            baseH, cw_pt, ch_pt, top, shift);
            top -= heights[j] + inter;
        }
    }
    for (int i = 0; i < n; i++) {                    /* horizontal legends */
        RObj *r = &ro[i];
        if (r->o->type != HM_LEGEND) continue;
        PlaceKind pk = r->o->place.kind;
        if (pk != PL_BENEATH && pk != PL_TOP_OF) continue;
        const char *title = legend_title(spec, r, &ro[r->target]);
        draw_one_legend(T, r, &ro[r->target], spec, dmin, dmax, title,
                        baseH, cw_pt, ch_pt, 0, pk == PL_BENEATH ? shiftB : shiftT);
    }

    /* ---- row/col name labels (into the reserved margins) ---- */
    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type != HM_HEATMAP) continue;
        Matrix *m = r->m;
        if (r->o->rownames && m->rn) {
            double chh = r->h / m->nr;
            int right = r->o->rownames == SIDE_RIGHT;
            for (int rr = 0; rr < m->nr; rr++) {
                double yc = r->b + r->h - (rr + 0.5) * chh;
                g = gt_add(T, G_TEXT, CR, CC, CR, CC);
                g->str = m->rn[r->roword[rr]];       /* data name at this slot */
                g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                g->tx = right ? r->l + r->w + PTX(TXT_GAP) : r->l - PTX(TXT_GAP);
                g->ty = yc; g->hj = right ? 0 : 1; g->va = V_INKCENTER;
            }
        }
        if (r->o->colnames && m->cn) {
            double cw = r->w / m->nc;
            int bottom = r->o->colnames == SIDE_BOTTOM;
            for (int cc = 0; cc < m->nc; cc++) {
                const char *s = m->cn[r->coword[cc]];
                double adv = text_w(cr, SZ_AXIS_TEXT, s);
                double xc = r->l + (cc + 0.5) * cw;
                g = gt_add(T, G_TEXT, CR, CC, CR, CC);
                g->str = s; g->size = SZ_AXIS_TEXT; g->col = C_BLACK; g->rot90 = 1;
                /* rot90 centres the text on ty; push its centre a half-length
                 * plus the gap past the heatmap edge so it hangs clear */
                g->tx = xc;
                g->ty = bottom ? r->b - PTY(TXT_GAP + adv / 2)
                               : r->b + r->h + PTY(TXT_GAP + adv / 2);
            }
        }
    }

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
