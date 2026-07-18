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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN_RATIO 0.02

/* Legends are RIGID chrome, sized in physical units (never coupled to the
 * matrix), following ComplexHeatmap: bar 4mm thick, ~28mm long. */
#define MM       (72.0 / 25.4)
#define LEG_BAR  (4.0 * MM)         /* colorbar thickness  */
#define LEG_LEN  (28.0 * MM)        /* colorbar length     */

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
    int target;                     /* legend: index of source heatmap */
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

static int find_obj(RObj *ro, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(ro[i].o->name, name)) return i;
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
            if (col->type == COL_STR) {
                Factor *f = factor_make(df, col);
                Col *pal = malloc(f->nlev * sizeof(Col));
                hue_palette(f->nlev, pal);
                for (int r = 0; r < df->nrow; r++)
                    ro[i].ann_col[r] = f->idx[r] >= 0 ? pal[f->idx[r]] : C_NA;
            } else {
                double lo = 1e300, hi = -1e300;
                for (int r = 0; r < df->nrow; r++) {
                    if (isnan(col->num[r])) continue;
                    if (col->num[r] < lo) lo = col->num[r];
                    if (col->num[r] > hi) hi = col->num[r];
                }
                FillScale vir = {FILL_VIRIDIS, {0}, {0}, {0}, 0};
                for (int r = 0; r < df->nrow; r++)
                    ro[i].ann_col[r] = isnan(col->num[r]) ? C_NA
                                     : fill_map_value(&vir, col->num[r], lo, hi);
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
            if (a && a->o->type == HM_HEATMAP) ro[i].target = (int)(a - ro);
            else
                for (int k = 0; k < i; k++)
                    if (ro[k].o->type == HM_HEATMAP) { ro[i].target = k; break; }
            if (ro[i].target < 0) { sprintf(err, "legend() found no heatmap to describe"); return -1; }
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
    if (ncells > 200000) {
        sprintf(err, "matrix too large for vector cells (%ld); raster mode not implemented yet", ncells);
        return -1;
    }
    if (dmin > dmax) { sprintf(err, "no finite values in matrix"); return -1; }

    /* ---- outer gtable: MEASURED chrome, canvas as the null cell ----
     * wheatmap's auto_margin: measure every outward-facing label and
     * reserve it as a page margin so nothing clips at the device edge.
     * Boundary test: a label reserves margin only when its object's edge
     * on that side sits at the normalized bbox boundary (0 or 1). */
    cairo_surface_t *surf = cairo_pdf_surface_create(out, w_pt, h_pt);
    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_font_extents_t fe;
    cairo_set_font_size(cr, SZ_TITLE);
    cairo_font_extents(cr, &fe);
    double titleh = spec->lab_title ? fe.ascent + fe.descent : 0;

    double marL = MARGIN, marR = MARGIN, marT = MARGIN, marB = MARGIN;
    const double EPS = 1e-6;
    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type == HM_HEATMAP) {
            Matrix *m = r->m;
            if (r->o->rownames && m->rn) {
                double wmax = 0;
                for (int k = 0; k < m->nr; k++) {
                    double tw = text_w(cr, SZ_AXIS_TEXT, m->rn[k]);
                    if (tw > wmax) wmax = tw;
                }
                double ext = MARGIN + TXT_GAP + wmax;
                if (r->o->rownames == SIDE_RIGHT && fabs(r->l + r->w - 1) < EPS)
                    marR = fmax(marR, ext);
                if (r->o->rownames == SIDE_LEFT && fabs(r->l) < EPS)
                    marL = fmax(marL, ext);
            }
            if (r->o->colnames && m->cn) {
                double wmax = 0;
                for (int k = 0; k < m->nc; k++) {
                    double tw = text_w(cr, SZ_AXIS_TEXT, m->cn[k]);
                    if (tw > wmax) wmax = tw;
                }
                double ext = MARGIN + TXT_GAP + wmax;   /* rotated 90 -> width along axis */
                if (r->o->colnames == SIDE_BOTTOM && fabs(r->b) < EPS)
                    marB = fmax(marB, ext);
                if (r->o->colnames == SIDE_TOP && fabs(r->b + r->h - 1) < EPS)
                    marT = fmax(marT, ext);
            }
        } else if (r->o->type == HM_LEGEND) {
            /* rigid footprint: fixed bar thickness + tick + widest label
             * (V), or bar + tick + one text line (H). Reserved on the
             * placement side; length is fixed (LEG_LEN), not the matrix. */
            double br[16];
            int nb = extended_breaks(dmin, dmax, 5, br, 16), nf = 0;
            for (int k = 0; k < nb; k++) if (br[k] >= dmin && br[k] <= dmax) br[nf++] = br[k];
            int dec = axis_decimals(br, nf);
            double wmax = 0;
            for (int k = 0; k < nf; k++) {
                char b[32]; fmt_break(br[k], dec, b);
                double tw = text_w(cr, SZ_AXIS_TEXT, b);
                if (tw > wmax) wmax = tw;
            }
            PlaceKind pk = r->o->place.kind;
            if (pk == PL_RIGHT_OF)
                marR = fmax(marR, MARGIN + HALF_LINE + LEG_BAR + TICK_LEN + TXT_GAP + wmax);
            else if (pk == PL_LEFT_OF)
                marL = fmax(marL, MARGIN + HALF_LINE + LEG_BAR + TICK_LEN + TXT_GAP + wmax);
            else if (pk == PL_BENEATH)
                marB = fmax(marB, MARGIN + HALF_LINE + LEG_BAR + TICK_LEN + TXT_GAP + (fe.ascent + fe.descent));
            else
                marT = fmax(marT, MARGIN + HALF_LINE + LEG_BAR + TICK_LEN + TXT_GAP + (fe.ascent + fe.descent));
        }
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
        } else if (r->o->type == HM_DENDROGRAM) {
            draw_dendro(T, CR, CC, r);
        } else if (r->o->type == HM_ANNOTATION) {
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
        /* HM_LEGEND is drawn separately, as rigid margin chrome, below */
    }

    /* ---- rigid colorbar legends (fixed pt size, in the margin) ---- */
    for (int i = 0; i < n; i++) {
        RObj *r = &ro[i];
        if (r->o->type != HM_LEGEND) continue;
        RObj *tg = &ro[r->target];
        PlaceKind pk = r->o->place.kind;
        int vert = pk == PL_RIGHT_OF || pk == PL_LEFT_OF;
        double barT = vert ? PTX(LEG_BAR) : PTY(LEG_BAR);   /* thickness, npc */
        double barL = vert ? PTY(LEG_LEN) : PTX(LEG_LEN);   /* length, npc    */
        const int NSTEP = 64;

        /* bar rectangle, centred on the target heatmap's cross-axis span */
        double bx0, by0;                                    /* bar low corner */
        if (vert) {
            double yc = tg->b + tg->h / 2;
            by0 = yc - barL / 2;
            bx0 = pk == PL_RIGHT_OF ? 1 + PTX(HALF_LINE)
                                    : 0 - PTX(HALF_LINE) - barT;
        } else {
            double xc = tg->l + tg->w / 2;
            bx0 = xc - barL / 2;
            by0 = pk == PL_BENEATH ? 0 - PTY(HALF_LINE) - barT
                                   : 1 + PTY(HALF_LINE);
        }
        for (int k = 0; k < NSTEP; k++) {
            g = gt_add(T, G_RECT, CR, CC, CR, CC);
            g->sub = 1;
            g->col = fill_map(&spec->fill, (k + 0.5) / NSTEP);
            if (vert) {
                g->x0 = bx0; g->x1 = bx0 + barT;
                g->y0 = by0 + barL * k / NSTEP; g->y1 = by0 + barL * (k + 1) / NSTEP;
            } else {
                g->y0 = by0; g->y1 = by0 + barT;
                g->x0 = bx0 + barL * k / NSTEP; g->x1 = bx0 + barL * (k + 1) / NSTEP;
            }
        }
        double br[16];
        int nb = extended_breaks(dmin, dmax, 5, br, 16), nf = 0;
        for (int k = 0; k < nb; k++) if (br[k] >= dmin && br[k] <= dmax) br[nf++] = br[k];
        int dec = axis_decimals(br, nf);
        for (int k = 0; k < nf; k++) {
            double frac = dmax > dmin ? (br[k] - dmin) / (dmax - dmin) : 0.5;
            char *lab = malloc(32);
            fmt_break(br[k], dec, lab);
            g = gt_add(T, G_LINE, CR, CC, CR, CC);
            g->col = C_TICK; g->lw = lw_pt(0.5);
            if (vert) {
                double y = by0 + frac * barL;
                double tx = pk == PL_RIGHT_OF ? bx0 + barT : bx0;
                double dir = pk == PL_RIGHT_OF ? 1 : -1;
                g->x0 = tx; g->x1 = tx + dir * PTX(TICK_LEN); g->y0 = g->y1 = y;
                g = gt_add(T, G_TEXT, CR, CC, CR, CC);
                g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                g->tx = tx + dir * PTX(TICK_LEN + TXT_GAP); g->ty = y;
                g->hj = pk == PL_RIGHT_OF ? 0 : 1; g->va = V_INKCENTER;
            } else {
                double x = bx0 + frac * barL;
                double ty = pk == PL_BENEATH ? by0 : by0 + barT;
                double dir = pk == PL_BENEATH ? -1 : 1;
                g->y0 = ty; g->y1 = ty + dir * PTY(TICK_LEN); g->x0 = g->x1 = x;
                g = gt_add(T, G_TEXT, CR, CC, CR, CC);
                g->str = lab; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                g->tx = x; g->ty = ty + dir * PTY(TICK_LEN + TXT_GAP);
                g->hj = 0.5; g->va = pk == PL_BENEATH ? V_TOP : V_BOTTOM;
            }
        }
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
    cairo_surface_finish(surf);
    cairo_status_t st = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        sprintf(err, "cairo: %s", cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
