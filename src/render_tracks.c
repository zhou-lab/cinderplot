/* render_tracks.c — locus track-browser mode: a stack of heterogeneous
 * tracks (coverage / interval / gene-model / arc) sharing one genomic
 * x-axis over a single region. A third assembler alongside render.c
 * (grammar) and heatmap.c (matrix); the gtable engine is unchanged.
 *
 * M2.0: scaffold — region parsing, the genomic bp/kb/Mb coordinate axis,
 * and empty stacked track frames with left-margin labels. Data renderers
 * land in M2.1/M2.2. */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Col C_TGRID = {0.898, 0.898, 0.898};   /* faint track gridline */

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

    cairo_surface_t *surf = cairo_pdf_surface_create(out, w_pt, h_pt);
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
    }
    g = gt_add(T, G_AXIS_X, axisrow, CC, axisrow, CC);
    g->n = nx; g->px = xpos; g->labels = xlab;

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_surface_finish(surf);
    cairo_status_t st = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) { sprintf(err, "cairo: %s", cairo_status_to_string(st)); return -1; }
    return 0;
}
