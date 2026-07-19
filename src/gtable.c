/* gtable.c — the layout engine: fixed/null units, one-pass resolver,
 * grobs positioned in npc coordinates within cell spans. */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

static double g_dpi = 96;                       /* PNG raster resolution */
void cp_set_dpi(double dpi) { if (dpi > 0) g_dpi = dpi; }

cairo_surface_t *cp_surface_create(const char *out, double w_pt, double h_pt) {
    const char *dot = strrchr(out, '.');
    if (dot && strcasecmp(dot, ".svg") == 0)
        return cairo_svg_surface_create(out, w_pt, h_pt);
    if (dot && strcasecmp(dot, ".png") == 0) {
        double sc = g_dpi / 72.0;               /* points -> pixels */
        int w = (int)(w_pt * sc + 0.5), h = (int)(h_pt * sc + 0.5);   /* round */
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_surface_set_device_scale(s, sc, sc);   /* keep drawing in points */
        cairo_t *bg = cairo_create(s);          /* opaque white ground, like ggsave */
        cairo_set_source_rgb(bg, 1, 1, 1); cairo_paint(bg); cairo_destroy(bg);
        return s;
    }
    return cairo_pdf_surface_create(out, w_pt, h_pt);
}

cairo_status_t cp_surface_emit(cairo_surface_t *surf, const char *out) {
    if (cairo_surface_get_type(surf) == CAIRO_SURFACE_TYPE_IMAGE) {
        cairo_surface_flush(surf);
        return cairo_surface_write_to_png(surf, out);
    }
    cairo_surface_finish(surf);
    return cairo_surface_status(surf);
}

static void set_col(cairo_t *cr, Col c) { cairo_set_source_rgb(cr, c.r, c.g, c.b); }

/* An axis label of the form "10^k" renders "10" full size then a smaller,
 * raised exponent. cp_label_w measures the rendered advance (used for both
 * centring and y-axis width reservation); draw_label draws it left-anchored
 * at baseline (bx, by). Plain labels (no '^') pass straight through. */
#define SUP_SCALE 0.72
#define SUP_RISE  0.34
double cp_label_w(cairo_t *cr, double size, const char *s) {
    cairo_set_font_size(cr, size);
    const char *car = strchr(s, '^');
    cairo_text_extents_t e;
    if (!car) { cairo_text_extents(cr, s, &e); return e.x_advance; }
    char base[32]; int bl = (int)(car - s); if (bl > 31) bl = 31;
    memcpy(base, s, bl); base[bl] = 0;
    cairo_text_extents(cr, base, &e);
    double w = e.x_advance;
    cairo_set_font_size(cr, size * SUP_SCALE);
    cairo_text_extents(cr, car + 1, &e);
    cairo_set_font_size(cr, size);
    return w + e.x_advance;
}
static void draw_label(cairo_t *cr, double bx, double by, double size, const char *s) {
    const char *car = strchr(s, '^');
    cairo_set_font_size(cr, size);
    if (!car) { cairo_move_to(cr, bx, by); cairo_show_text(cr, s); return; }
    char base[32]; int bl = (int)(car - s); if (bl > 31) bl = 31;
    memcpy(base, s, bl); base[bl] = 0;
    cairo_text_extents_t e; cairo_text_extents(cr, base, &e);
    cairo_move_to(cr, bx, by); cairo_show_text(cr, base);
    cairo_set_font_size(cr, size * SUP_SCALE);
    cairo_move_to(cr, bx + e.x_advance, by - size * SUP_RISE);
    cairo_show_text(cr, car + 1);
    cairo_set_font_size(cr, size);
}

Grob *gt_add(GTable *t, GType type, int r0, int c0, int r1, int c1) {
    if (t->ngrobs == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->grobs = realloc(t->grobs, t->cap * sizeof(Grob));
    }
    Grob *g = &t->grobs[t->ngrobs++];
    memset(g, 0, sizeof *g);
    g->type = type; g->r0 = r0; g->c0 = c0; g->r1 = r1; g->c1 = c1;
    return g;
}

double gt_fixed_w(const GTable *t) {
    double w = 0;
    for (int i = 0; i < t->ncol; i++) w += t->colw[i].v;
    return w;
}
double gt_fixed_h(const GTable *t) {
    double h = 0;
    for (int i = 0; i < t->nrow; i++) h += t->rowh[i].v;
    return h;
}

/* THE resolver: sum fixed units, split the remainder among nulls by weight */
void gt_resolve(GTable *t, double x, double y, double w, double h) {
    double fix = 0, nul = 0;
    for (int i = 0; i < t->ncol; i++)
        if (t->colw[i].k == U_PT) fix += t->colw[i].v; else nul += t->colw[i].v;
    /* A tiny output must not turn flexible cells inside out. */
    double per_w = nul > 0 ? fmax(0, w - fix) / nul : 0;
    t->colx[0] = x;
    for (int i = 0; i < t->ncol; i++)
        t->colx[i + 1] = t->colx[i] +
            (t->colw[i].k == U_PT ? t->colw[i].v : t->colw[i].v * per_w);
    fix = nul = 0;
    for (int i = 0; i < t->nrow; i++)
        if (t->rowh[i].k == U_PT) fix += t->rowh[i].v; else nul += t->rowh[i].v;
    double per_h = nul > 0 ? fmax(0, h - fix) / nul : 0;
    t->rowy[0] = y;
    for (int i = 0; i < t->nrow; i++)
        t->rowy[i + 1] = t->rowy[i] +
            (t->rowh[i].k == U_PT ? t->rowh[i].v : t->rowh[i].v * per_h);
}

void gt_render(GTable *t, cairo_t *cr) {
    for (int gi = 0; gi < t->ngrobs; gi++) {
        Grob *g = &t->grobs[gi];
        double rx = t->colx[g->c0], rw = t->colx[g->c1 + 1] - rx;
        double ry = t->rowy[g->r0], rh = t->rowy[g->r1 + 1] - ry;
        /* npc -> device (npc y=0 is the cell bottom) */
#define DX(v) (rx + (v) * rw)
#define DY(v) (ry + rh - (v) * rh)
        cairo_save(cr);
        if (g->clip) { cairo_rectangle(cr, rx, ry, rw, rh); cairo_clip(cr); }
        switch (g->type) {
        case G_RECT:
            set_col(cr, g->col);
            if (g->sub)
                cairo_rectangle(cr, DX(g->x0), DY(g->y1),
                                (g->x1 - g->x0) * rw, (g->y1 - g->y0) * rh);
            else
                cairo_rectangle(cr, rx, ry, rw, rh);
            if (g->stroke) { cairo_set_line_width(cr, g->lw); cairo_stroke(cr); }
            else cairo_fill(cr);
            break;
        case G_IMAGE: {
            /* one image pixel per cell, scaled into the sub-rect with a
             * nearest filter so cell edges stay crisp (geom_raster) */
            int iw = g->img_w, ih = g->img_h;
            int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, iw);
            cairo_surface_t *img = cairo_image_surface_create_for_data(
                g->img, CAIRO_FORMAT_ARGB32, iw, ih, stride);
            double tx0 = DX(g->x0), ty0 = DY(g->y1);          /* top-left */
            double tw = (g->x1 - g->x0) * rw, th = (g->y1 - g->y0) * rh;
            cairo_save(cr);
            cairo_translate(cr, tx0, ty0);
            cairo_scale(cr, tw / iw, th / ih);
            cairo_set_source_surface(cr, img, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(img);
            break;
        }
        case G_POLYLINE:
            set_col(cr, g->col);
            cairo_set_line_width(cr, g->lw);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            for (int i = 0; i < g->n; i++) {
                if (i == 0) cairo_move_to(cr, DX(g->px[i]), DY(g->py[i]));
                else cairo_line_to(cr, DX(g->px[i]), DY(g->py[i]));
            }
            cairo_stroke(cr);
            break;
        case G_LINE:
            set_col(cr, g->col);
            cairo_set_line_width(cr, g->lw);
            cairo_move_to(cr, DX(g->x0), DY(g->y0));
            cairo_line_to(cr, DX(g->x1), DY(g->y1));
            cairo_stroke(cr);
            break;
        case G_POINTS:
            for (int i = 0; i < g->n; i++) {
                set_col(cr, g->pcol[i]);
                cairo_arc(cr, DX(g->px[i]), DY(g->py[i]), g->radius, 0, 2 * M_PI);
                cairo_fill(cr);
            }
            break;
        case G_TEXT: {
            cairo_text_extents_t e; cairo_font_extents_t fe;
            cairo_set_font_size(cr, g->size);
            cairo_text_extents(cr, g->str, &e);
            cairo_font_extents(cr, &fe);
            set_col(cr, g->col);
            if (g->rot90) {
                cairo_translate(cr, DX(g->tx) + (fe.ascent - fe.descent) / 2,
                                    DY(g->ty) + e.x_advance / 2);
                cairo_rotate(cr, -M_PI / 2);
                cairo_move_to(cr, 0, 0);
            } else {
                double bx = DX(g->tx) - g->hj * e.x_advance, by;
                switch (g->va) {
                case V_TOP:    by = DY(g->ty) + fe.ascent; break;
                case V_BOTTOM: by = DY(g->ty) - fe.descent; break;
                default:       by = DY(g->ty) - e.height / 2 - e.y_bearing; break;
                }
                if (g->text_box) {                 /* geom_label background box */
                    double pad = g->size * 0.25;
                    double lx = bx + e.x_bearing - pad, ly = by + e.y_bearing - pad;
                    double lw = e.width + 2 * pad, lh = e.height + 2 * pad;
                    cairo_rectangle(cr, lx, ly, lw, lh);
                    set_col(cr, g->box_fill); cairo_fill_preserve(cr);
                    set_col(cr, g->box_line); cairo_set_line_width(cr, lw_pt(0.25));
                    cairo_stroke(cr);
                    set_col(cr, g->col);
                }
                cairo_move_to(cr, bx, by);
            }
            cairo_show_text(cr, g->str);
            break;
        }
        case G_AXIS_X: {
            /* ticks + labels anchored to the TOP of the cell, pt units;
             * reusable both for the bottom axis row and for staircase
             * axes drawn into an absent panel's cell */
            cairo_font_extents_t fe;
            cairo_set_font_size(cr, SZ_AXIS_TEXT);
            cairo_font_extents(cr, &fe);
            if (!g->hide_ticks) {
                set_col(cr, g->axis_styled ? g->tick_col : C_TICK);
                cairo_set_line_width(cr, lw_pt(0.5));
                for (int i = 0; i < g->n; i++) {
                    cairo_move_to(cr, DX(g->px[i]), ry);
                    cairo_line_to(cr, DX(g->px[i]), ry + TICK_LEN);
                }
                for (int i = 0; i < g->mtn; i++) {          /* log minor ticks */
                    cairo_move_to(cr, DX(g->mtpos[i]), ry);
                    cairo_line_to(cr, DX(g->mtpos[i]), ry + g->mtlen[i]);
                }
                cairo_stroke(cr);
            }
            if (!g->hide_text) {
                set_col(cr, g->axis_styled ? g->text_col : C_AXTXT);
                for (int i = 0; i < g->n; i++) {
                    double w = cp_label_w(cr, SZ_AXIS_TEXT, g->labels[i]);
                    draw_label(cr, DX(g->px[i]) - w / 2,
                               ry + TICK_LEN + TXT_GAP + fe.ascent, SZ_AXIS_TEXT, g->labels[i]);
                }
            }
            break;
        }
        case G_AXIS_Y: {
            /* ticks + right-aligned labels anchored to the RIGHT edge */
            cairo_set_font_size(cr, SZ_AXIS_TEXT);
            double right = rx + rw;
            if (!g->hide_ticks) {
                set_col(cr, g->axis_styled ? g->tick_col : C_TICK);
                cairo_set_line_width(cr, lw_pt(0.5));
                for (int i = 0; i < g->n; i++) {
                    cairo_move_to(cr, right - TICK_LEN, DY(g->py[i]));
                    cairo_line_to(cr, right, DY(g->py[i]));
                }
                for (int i = 0; i < g->mtn; i++) {          /* log minor ticks */
                    cairo_move_to(cr, right - g->mtlen[i], DY(g->mtpos[i]));
                    cairo_line_to(cr, right, DY(g->mtpos[i]));
                }
                cairo_stroke(cr);
            }
            if (g->hide_text) break;
            set_col(cr, g->axis_styled ? g->text_col : C_AXTXT);
            for (int i = 0; i < g->n; i++) {
                const char *s = g->labels[i], *car = strchr(s, '^');
                char base[32];
                if (car) { int bl = (int)(car - s); if (bl > 31) bl = 31; memcpy(base, s, bl); base[bl] = 0; }
                cairo_text_extents_t e;
                cairo_text_extents(cr, car ? base : s, &e);   /* vertical metrics from the "10" part */
                double w = cp_label_w(cr, SZ_AXIS_TEXT, s);
                draw_label(cr, right - TICK_LEN - TXT_GAP - w,
                           DY(g->py[i]) - e.height / 2 - e.y_bearing, SZ_AXIS_TEXT, s);
            }
            break;
        }
        case G_TABLE: {
            double cw = gt_fixed_w(g->child), ch = gt_fixed_h(g->child);
            gt_resolve(g->child, rx + (rw - cw) / 2, ry + (rh - ch) / 2, cw, ch);
            gt_render(g->child, cr);
            break;
        }
        }
        cairo_restore(cr);
#undef DX
#undef DY
    }
}
