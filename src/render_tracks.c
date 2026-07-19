/* render_tracks.c — locus track-browser mode: a stack of heterogeneous
 * tracks (coverage / interval / gene-model / arc) sharing one genomic
 * x-axis over a single region. A third assembler alongside render.c
 * (grammar) and heatmap.c (matrix); the gtable engine is unchanged.
 *
 * Genomic bp/kb/Mb axis; coverage/interval/arc renderers fill each track
 * row cell. Gene models (BED12/GFF) land in M2.3. */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Col C_TGRID = {0.898, 0.898, 0.898};   /* faint track gridline */
static const Col C_COV   = {0.271, 0.459, 0.706};   /* steelblue coverage */
static const Col C_IVAL  = {0.35, 0.35, 0.35};      /* grey interval box */
static const Col C_ARC   = {0.5, 0.3, 0.6};         /* purple arc */

static int cmp_iv(const void *a, const void *b) {
    long d = ((const Interval *)a)->start - ((const Interval *)b)->start;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}
static int cmp_gene(const void *a, const void *b) {
    long d = ((const GeneModel *)a)->tx_start - ((const GeneModel *)b)->tx_start;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

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

int render_tracks(const PlotSpec *spec, const char *out,
                  double w_pt, double h_pt, char *err) {
    if (!spec->region) {
        sprintf(err, "track mode needs a region: region(chr:start-end) or --region"); return -1;
    }
    char chrom[64]; long rstart, rend;
    if (region_parse(spec->region, chrom, &rstart, &rend)) {
        sprintf(err, "bad region `%s`; expected chr:start-end", spec->region); return -1;
    }
    int ntr = spec->ntracks;
    double x0 = rstart, x1 = rend;
#define NPCX(v) (((v) - x0) / (x1 - x0))

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cairo_t *cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    /* ---- genomic axis: pick unit, nice breaks, format with suffix ---- */
    double span = x1 - x0;
    double unit = span >= 2e6 ? 1e6 : span >= 2e3 ? 1e3 : 1;
    const char *usuf = unit == 1e6 ? " Mb" : unit == 1e3 ? " kb" : " bp";
    double ubr[16];
    int nb = extended_breaks(x0 / unit, x1 / unit, 5, ubr, 16), nx = 0;
    double xpos[16]; char *xlab[16];
    int dec = axis_decimals(ubr, nb);
    for (int i = 0; i < nb; i++) {
        double bp = ubr[i] * unit;
        if (bp < x0 || bp > x1) continue;
        xpos[nx] = NPCX(bp);
        char num[32]; fmt_break(ubr[i], dec, num);
        xlab[nx] = malloc(40); snprintf(xlab[nx], 40, "%s%s", num, usuf);
        nx++;
    }

    /* ---- measure left label column from track names ---- */
    double labw = 0;
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].name) {
            double w = text_w(cr, SZ_AXIS_TEXT, spec->tobjs[i].name);
            if (w > labw) labw = w;
        }
    double titleh = spec->lab_title ? font_h(cr, SZ_TITLE) : 0;
    double axh = font_h(cr, SZ_AXIS_TEXT);
    double panel_w = w_pt - 2 * MARGIN - labw - (labw > 0 ? HALF_LINE : 0);  /* pt */

    /* ---- outer gtable: [MARGIN|label|gap|PANEL|MARGIN] cols;
     * [MARGIN|title|gap| tracks+gaps |axis|MARGIN] rows ---- */
    GTable *T = calloc(1, sizeof(GTable));
    T->ncol = 5;
    T->colw[0] = upt(MARGIN);
    T->colw[1] = upt(labw);
    T->colw[2] = upt(labw > 0 ? HALF_LINE : 0);
    T->colw[3] = unull(1);
    T->colw[4] = upt(MARGIN);
    const int CC = 3;
#define TRK(i) (3 + 2 * (i))
    int axisrow = 3 + (ntr > 0 ? 2 * ntr - 1 : 0);
    T->nrow = axisrow + 2;
    T->rowh[0] = upt(MARGIN);
    T->rowh[1] = upt(titleh);
    T->rowh[2] = upt(spec->lab_title ? HALF_LINE : 0);
    for (int i = 0; i < ntr; i++) {
        double wgt = spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1;
        T->rowh[TRK(i)] = unull(wgt);
        if (i < ntr - 1) T->rowh[TRK(i) + 1] = upt(HALF_LINE * 0.6);
    }
    T->rowh[axisrow] = upt(TICK_LEN + TXT_GAP + axh);
    T->rowh[axisrow + 1] = upt(MARGIN);

    Grob *g;
    if (spec->lab_title) {
        g = gt_add(T, G_TEXT, 1, CC, 1, CC);
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = C_BLACK;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    for (int i = 0; i < ntr; i++) {
        int R = TRK(i);
        g = gt_add(T, G_RECT, R, CC, R, CC); g->col = C_WHITE;   /* track background */
        for (int b = 0; b < nx; b++) {                           /* faint gridlines */
            g = gt_add(T, G_LINE, R, CC, R, CC);
            g->col = C_TGRID; g->lw = lw_pt(0.5); g->clip = 1;
            g->x0 = g->x1 = xpos[b]; g->y0 = 0; g->y1 = 1;
        }
        if (spec->tobjs[i].name) {                               /* left label */
            g = gt_add(T, G_TEXT, R, 1, R, 1);
            g->str = spec->tobjs[i].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            g->tx = 1; g->ty = 0.5; g->hj = 1; g->va = V_INKCENTER;
        }

        /* ---- track content ---- */
        const TrackObj *t = &spec->tobjs[i];
        if (t->type == TRK_COVERAGE) {
            int nsb; SigBin *sb = bedgraph_read(t->data, chrom, rstart, rend, &nsb, err);
            if (!sb) return -1;
            double ymax = t->max_value > 0 ? t->max_value : 0;
            if (ymax <= 0) for (int k = 0; k < nsb; k++) if (sb[k].val > ymax) ymax = sb[k].val;
            if (ymax <= 0) ymax = 1;
            Col col = t->has_color ? t->color : C_COV;
            for (int k = 0; k < nsb; k++) {
                if (sb[k].val <= 0) continue;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(sb[k].start); g->x1 = NPCX(sb[k].end);
                g->y0 = 0; g->y1 = sb[k].val / ymax;
            }
            char *rd = malloc(32); snprintf(rd, 32, "[0 - %g]", ymax);   /* readout */
            g = gt_add(T, G_TEXT, R, CC, R, CC);
            g->str = rd; g->size = SZ_AXIS_TEXT; g->col = C_AXTXT;
            g->tx = 0.004; g->ty = 0.98; g->hj = 0; g->va = V_TOP;
        } else if (t->type == TRK_GENES) {
            /* gene models (BED12): exon boxes (thin UTR / thick CDS), intron
             * center line, strand chevrons, transcript name; lane-packed */
            int ng; GeneModel *gm = bed12_read(t->data, chrom, rstart, rend, &ng, err);
            if (!gm) return -1;
            qsort(gm, ng, sizeof *gm, cmp_gene);
            long laneend[64]; int nlanes = 0, *lane = malloc(ng * sizeof(int));
            for (int k = 0; k < ng; k++) {
                int L = -1;
                for (int j = 0; j < nlanes; j++) if (laneend[j] <= gm[k].tx_start) { L = j; break; }
                if (L < 0 && nlanes < 64) L = nlanes++;
                if (L < 0) L = nlanes - 1;
                lane[k] = L; laneend[L] = gm[k].tx_end;
            }
            Col col = t->has_color ? t->color : C_IVAL;
            double lh = 1.0 / nlanes, utr = 0.26 * lh, cds = 0.52 * lh;
            for (int k = 0; k < ng; k++) {
                double yc = 1 - (lane[k] + 0.5) * lh;
                double xa = NPCX(gm[k].tx_start), xb = NPCX(gm[k].tx_end);
                g = gt_add(T, G_LINE, R, CC, R, CC);         /* intron line */
                g->col = col; g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = xa; g->x1 = xb; g->y0 = g->y1 = yc;
                double achx = 3.0 / panel_w;                 /* chevron half-width (npc) */
                double dir = gm[k].strand == '-' ? -1 : 1;
                for (double xc = xa + 0.02; xc < xb - 0.01; xc += 40.0 / panel_w) {
                    g = gt_add(T, G_LINE, R, CC, R, CC);
                    g->col = col; g->lw = lw_pt(0.4); g->clip = 1;
                    g->x0 = xc - dir * achx; g->x1 = xc; g->y0 = yc + 0.12 * lh; g->y1 = yc;
                    g = gt_add(T, G_LINE, R, CC, R, CC);
                    g->col = col; g->lw = lw_pt(0.4); g->clip = 1;
                    g->x0 = xc - dir * achx; g->x1 = xc; g->y0 = yc - 0.12 * lh; g->y1 = yc;
                }
                for (int j = 0; j < gm[k].nexon; j++) {      /* exon boxes */
                    long es = gm[k].exons[j].start, ee = gm[k].exons[j].end;
                    g = gt_add(T, G_RECT, R, CC, R, CC);     /* UTR (thin) */
                    g->col = col; g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(es); g->x1 = NPCX(ee); g->y0 = yc - utr / 2; g->y1 = yc + utr / 2;
                    long cs = es > gm[k].cds_start ? es : gm[k].cds_start;
                    long ce = ee < gm[k].cds_end ? ee : gm[k].cds_end;
                    if (ce > cs) {                           /* CDS (thick) */
                        g = gt_add(T, G_RECT, R, CC, R, CC);
                        g->col = col; g->sub = 1; g->clip = 1;
                        g->x0 = NPCX(cs); g->x1 = NPCX(ce); g->y0 = yc - cds / 2; g->y1 = yc + cds / 2;
                    }
                }
                if (gm[k].name) {                            /* name to the right */
                    g = gt_add(T, G_TEXT, R, CC, R, CC);
                    g->str = gm[k].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                    g->tx = xb + 0.004; g->ty = yc; g->hj = 0; g->va = V_INKCENTER;
                }
            }
        } else if (t->type == TRK_INTERVAL) {
            /* interval blocks packed into non-overlapping lanes */
            int ni; Interval *iv = bed_read(t->data, chrom, rstart, rend, &ni, err);
            if (!iv) return -1;
            qsort(iv, ni, sizeof *iv, cmp_iv);
            long laneend[64]; int nlanes = 0, *lane = malloc(ni * sizeof(int));
            for (int k = 0; k < ni; k++) {
                int L = -1;
                for (int j = 0; j < nlanes; j++) if (laneend[j] <= iv[k].start) { L = j; break; }
                if (L < 0 && nlanes < 64) { L = nlanes++; }
                if (L < 0) L = nlanes - 1;               /* cap: pile into last lane */
                lane[k] = L; laneend[L] = iv[k].end;
            }
            Col col = t->has_color ? t->color : C_IVAL;
            double lh = 1.0 / nlanes;
            for (int k = 0; k < ni; k++) {
                double yb = 1 - (lane[k] + 1) * lh;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(iv[k].start); g->x1 = NPCX(iv[k].end);
                g->y0 = yb + 0.18 * lh; g->y1 = yb + 0.82 * lh;
                if (iv[k].name) {                        /* name to the right */
                    g = gt_add(T, G_TEXT, R, CC, R, CC);
                    g->str = iv[k].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
                    g->tx = NPCX(iv[k].end) + 0.004; g->ty = yb + 0.5 * lh;
                    g->hj = 0; g->va = V_INKCENTER;
                }
            }
        } else if (t->type == TRK_ARCS) {
            int nl; Link *lk = bedpe_read(t->data, chrom, rstart, rend, &nl, err);
            if (!lk) return -1;
            Col col = t->has_color ? t->color : C_ARC;
            const int NS = 40;
            for (int k = 0; k < nl; k++) {
                double xa = NPCX((lk[k].a_start + lk[k].a_end) / 2.0);
                double xb = NPCX((lk[k].b_start + lk[k].b_end) / 2.0);
                double h = fmin(0.92, fabs(xb - xa) + 0.08);     /* wider span -> taller */
                double *px = malloc(NS * sizeof(double)), *py = malloc(NS * sizeof(double));
                for (int s = 0; s < NS; s++) {
                    double f = (double)s / (NS - 1);
                    px[s] = xa + f * (xb - xa); py[s] = h * sin(M_PI * f);
                }
                g = gt_add(T, G_POLYLINE, R, CC, R, CC);
                g->n = NS; g->px = px; g->py = py; g->col = col; g->lw = lw_pt(0.6); g->clip = 1;
            }
        }
    }
    g = gt_add(T, G_AXIS_X, axisrow, CC, axisrow, CC);
    g->n = nx; g->px = xpos; g->labels = xlab;

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_status_t st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) { sprintf(err, "cairo: %s", cairo_status_to_string(st)); return -1; }
    return 0;
}
