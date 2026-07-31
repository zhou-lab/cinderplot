/* tree.c — Newick trees, drawn ggtree-style.
 *
 * The other three modes all build their structure from a table. A tree arrives
 * already built, in a format that carries the topology itself, so this mode
 * reads Newick rather than CSV and lays it out directly. That is the point of
 * having it: a curated ontology -- a cell-type taxonomy, a term hierarchy -- is
 * a structure the user already has, and drawing it should not require a Python
 * tree library plus a plotting library.
 *
 * Layout is rectangular, the shape that reads as a taxonomy: leaves stack down
 * the y axis in the order they appear, an internal node sits at the mean of its
 * children, and x is depth (or cumulative branch length when the Newick carries
 * one). Connectors are elbows -- a vertical spine at the parent's x spanning
 * its children, then a horizontal arm out to each child.
 */
#include "cinderplot.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

/* Local copies: the equivalents in render_tracks.c are file-static, and one
 * small helper each is cheaper than exporting them for a second caller. */
static double text_w(cairo_t *cr, double size, const char *s) {
    cairo_text_extents_t te;
    cairo_set_font_size(cr, size);
    cairo_text_extents(cr, s ? s : "", &te);
    return te.x_advance;
}
static double font_h(cairo_t *cr, double size) {
    cairo_font_extents_t fe;
    cairo_set_font_size(cr, size);
    cairo_font_extents(cr, &fe);
    return fe.ascent + fe.descent;
}

/* Slurp the whole input; a Newick tree is one line and always small. */
static char *read_all(const char *path, char *err) {
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) { snprintf(err, CP_ERRLEN, "cannot open %s", path); return NULL; }
    size_t cap = 1 << 14, n = 0;
    char *buf = cp_xmalloc(cap);
    for (;;) {
        if (n + 4096 > cap) { cap *= 2; buf = cp_xrealloc(buf, cap); }
        size_t got = fread(buf + n, 1, 4096, f);
        n += got;
        if (got < 4096) break;
    }
    if (f != stdin) fclose(f);
    buf[n] = 0;
    return buf;
}

typedef struct TNode {
    char *name;                 /* may be NULL: Newick allows unnamed nodes */
    double blen;                /* branch length to parent; NAN when absent */
    struct TNode **kid; int nkid, kidcap;
    double x, y;                /* laid-out position, data space */
    double lw;                  /* rendered label width, pt (0 if undrawn) */
} TNode;

static TNode *tn_new(void) {
    TNode *t = cp_xcalloc(1, sizeof *t);
    t->blen = NAN;
    return t;
}

static void tn_push(TNode *p, TNode *c) {
    if (p->nkid == p->kidcap) {
        p->kidcap = p->kidcap ? p->kidcap * 2 : 4;
        p->kid = cp_xrealloc(p->kid, p->kidcap * sizeof *p->kid);
    }
    p->kid[p->nkid++] = c;
}

/* A Newick name runs to the next structural character. Quoted names may hold
 * those characters, and underscores are spaces by the format's convention --
 * but a curated ontology uses underscores deliberately, so they are left as
 * typed and only quoting is honoured. */
static char *nw_name(const char **s) {
    const char *p = *s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '\'' || *p == '"') {
        /* Newick escapes a quote by doubling it, so '' inside a quoted name is
         * one literal quote and does not end the name. */
        char q = *p++;
        size_t cap = strlen(p) + 1, n = 0;
        char *out = cp_xmalloc(cap);
        while (*p) {
            if (*p == q && p[1] == q) { out[n++] = q; p += 2; continue; }
            if (*p == q) break;
            out[n++] = *p++;
        }
        out[n] = 0;
        if (*p == q) p++;
        *s = p;
        return out;
    }
    const char *b = p;
    while (*p && !strchr("(),:;[]", *p)) p++;
    while (p > b && isspace((unsigned char)p[-1])) p--;   /* trailing space */
    if (p == b) { *s = p; return NULL; }
    char *out = strndup(b, p - b);
    *s = p;
    return out;
}

/* Newick allows [...] comments -- NHX annotations use them heavily. Skip them
 * wherever whitespace would be skipped, so a comment-bearing file loads instead
 * of failing on a bracket the name scanner stopped at. */
static void nw_skip(const char **s) {
    for (;;) {
        while (isspace((unsigned char)**s)) (*s)++;
        if (**s != '[') return;
        while (**s && **s != ']') (*s)++;
        if (**s == ']') (*s)++;
    }
}

static TNode *nw_node(const char **s, char *err);

static int nw_children(const char **s, TNode *parent, char *err) {
    (*s)++;                                     /* '(' */
    for (;;) {
        TNode *c = nw_node(s, err);
        if (!c) return -1;
        tn_push(parent, c);
        nw_skip(s);
        if (**s == ',') { (*s)++; continue; }
        if (**s == ')') { (*s)++; return 0; }
        snprintf(err, CP_ERRLEN, "malformed Newick: expected , or ) near \"%.20s\"", *s);
        return -1;
    }
}

static TNode *nw_node(const char **s, char *err) {
    nw_skip(s);
    TNode *t = tn_new();
    if (**s == '(') {
        if (nw_children(s, t, err)) return NULL;
    }
    t->name = nw_name(s);
    nw_skip(s);
    if (**s == ':') {                            /* branch length */
        (*s)++;
        nw_skip(s);
        char *end;
        t->blen = strtod(*s, &end);
        if (end == *s) {
            snprintf(err, CP_ERRLEN, "malformed Newick: bad branch length near \"%.20s\"", *s);
            return NULL;
        }
        /* A non-finite or negative length has no rendering: it would either
         * collapse the tree to a line or run it off the surface. */
        if (!isfinite(t->blen) || t->blen < 0) {
            snprintf(err, CP_ERRLEN, "branch length must be finite and non-negative, got %g", t->blen);
            return NULL;
        }
        *s = end;
    }
    nw_skip(s);
    return t;
}

static TNode *newick_parse(const char *text, char *err) {
    const char *s = text;
    nw_skip(&s);
    if (!*s) { snprintf(err, CP_ERRLEN, "Newick input is empty"); return NULL; }
    TNode *root = nw_node(&s, err);
    if (!root) return NULL;
    nw_skip(&s);
    if (*s == ';') s++;
    nw_skip(&s);
    if (*s) {
        snprintf(err, CP_ERRLEN, "trailing text after the Newick tree: \"%.20s\"", s);
        return NULL;
    }
    return root;
}

static int tn_leaf(const TNode *t) { return t->nkid == 0; }

/* Depth-first, assigning each leaf the next y. An internal node then sits at
 * the mean of its children, which is what makes a rectangular tree read as a
 * hierarchy rather than a tangle. x is depth, or the cumulative branch length
 * when the file carries lengths. */
/* `fit` widens a branch so the label at its far end clears the one behind it.
 * Only for a cladogram: there x is depth, an arbitrary unit, so stretching a
 * branch misrepresents nothing. On a phylogram the length is the datum and must
 * be left alone -- the labels crowd instead, which is the honest failure. */
static void layout(TNode *t, double px, int has_blen, double *next_y,
                   double *maxx, int *nleaf, double fit, const TNode *parent) {
    double step = 1;
    if (fit > 0 && parent) {
        /* A node label ends at its node and runs back along the incoming
         * branch, so the branch must hold that label whole, clear of where the
         * parent's own label ended. */
        (void)parent;
        double need = t->lw + fit;
        if (need > step) step = need;
    }
    t->x = has_blen ? px + t->blen : px + step;
    if (t->x > *maxx) *maxx = t->x;
    if (tn_leaf(t)) {
        t->y = (*next_y)++;
        (*nleaf)++;
        return;
    }
    for (int i = 0; i < t->nkid; i++)
        layout(t->kid[i], t->x, has_blen, next_y, maxx, nleaf, fit, t);
    t->y = (t->kid[0]->y + t->kid[t->nkid - 1]->y) / 2;
}

/* Count edges with and without a length, ignoring the root: a length on the
 * root is a distance to a parent that is not drawn, and honouring it would
 * shift the whole tree rightward for no reason. */
static void count_lengths(const TNode *t, int root, int *with, int *without) {
    if (!root) { if (isnan(t->blen)) (*without)++; else (*with)++; }
    for (int i = 0; i < t->nkid; i++) count_lengths(t->kid[i], 0, with, without);
}

/* Measure the label each node will actually draw, so the layout can make room
 * for it. A node that draws nothing measures zero and constrains nothing. */
static void measure_labels(cairo_t *cr, TNode *t, int tips, int nodes, double sz) {
    int leaf = tn_leaf(t);
    t->lw = (t->name && (leaf ? tips : nodes)) ? text_w(cr, sz, t->name) : 0;
    for (int i = 0; i < t->nkid; i++) measure_labels(cr, t->kid[i], tips, nodes, sz);
}

/* widest tip / node label, for the margin they need */
static double label_w(cairo_t *cr, const TNode *t, int tips, double sz) {
    double w = 0;
    if (t->name && (tn_leaf(t) == !!tips)) w = text_w(cr, sz, t->name);
    for (int i = 0; i < t->nkid; i++) {
        double k = label_w(cr, t->kid[i], tips, sz);
        if (k > w) w = k;
    }
    return w;
}

/* ---------- joining a table to the tree ----------
 *
 * ggtree's `%<+%`: a table keyed on the node/tip NAME, one aesthetic mapped
 * from a column. Long form throughout, because several rows for one name is
 * not an error here -- it is the interesting case. A node belonging to three
 * taxonomy levels gets three marks and advertises itself as a degenerate
 * chain, where one mark means a level doing real work. */
typedef struct {
    const Column *key, *val;    /* name column, mapped column */
    const DataFrame *df;
    Factor *f;                  /* discrete: levels + per-row index */
    Col *pal;                   /* discrete: one colour per level */
    double dmin, dmax;          /* continuous: domain */
    FillScale fs;               /* continuous: ramp */
    int discrete;
} Join;

/* The key is the first text column; the mapped column is named by colour=. */
static int join_load(Join *j, const char *path, const char *colname,
                     const PlotSpec *spec, char *err) {
    memset(j, 0, sizeof *j);
    DataFrame *df = df_read_csv(path, err);
    if (!df) return -1;
    j->df = df;
    for (int c = 0; c < df->ncol; c++)
        if (df->cols[c].type == COL_STR) { j->key = &df->cols[c]; break; }
    if (!j->key) {
        snprintf(err, CP_ERRLEN, "%s has no text column to join on; the first "
                 "text column is matched against the node names", path);
        return -1;
    }
    if (!colname) {
        snprintf(err, CP_ERRLEN, "joining %s needs colour=<column>", path);
        return -1;
    }
    if (!(j->val = df_col(df, colname))) {
        snprintf(err, CP_ERRLEN, "column `%s` not found in %s", colname, path);
        return -1;
    }
    if (j->val == j->key) {
        snprintf(err, CP_ERRLEN, "colour=%s is the join key; map a different "
                 "column", colname);
        return -1;
    }
    j->discrete = j->val->type == COL_STR;
    if (j->discrete) {
        j->f = factor_make(df, j->val);
        j->pal = cp_xmalloc(j->f->nlev * sizeof(Col));
        hue_palette(j->f->nlev, j->pal);
    } else {
        j->dmin = 1e300; j->dmax = -1e300;
        for (int r = 0; r < df->nrow; r++) {
            double v = j->val->num[r];
            if (!isfinite(v)) continue;
            if (v < j->dmin) j->dmin = v;
            if (v > j->dmax) j->dmax = v;
        }
        if (j->dmin > j->dmax) { j->dmin = 0; j->dmax = 1; }
        if (j->dmax == j->dmin) j->dmax = j->dmin + 1;
        j->fs = spec->has_fill ? spec->fill : (FillScale){0};
        if (!spec->has_fill) j->fs.kind = FILL_VIRIDIS;
    }
    return 0;
}

/* Count the nodes carrying `name`, tips or internal. */
static int count_named(const TNode *t, const char *name, int leaves) {
    int n = (t->name && !strcmp(t->name, name) && (tn_leaf(t) == !!leaves)) ? 1 : 0;
    for (int i = 0; i < t->nkid; i++) n += count_named(t->kid[i], name, leaves);
    return n;
}

/* A name-keyed join is ambiguous the moment a name is not unique: every node
 * called A matches every row for A, which is what a name join means and never
 * what anybody wants. Any tie-break -- first match, by depth, all matches --
 * would be a guess at intent, so refuse and name the offender. The repeated
 * name is usually a defect in the tree anyway. */
static int join_check_unique(const Join *j, const TNode *root, int leaves,
                             char *err) {
    if (!j->df) return 0;
    for (int r = 0; r < j->df->nrow; r++) {
        const char *nm = j->key->str[r];
        if (!nm) continue;
        int seen = 0;                       /* only report each name once */
        for (int q = 0; q < r; q++)
            if (j->key->str[q] && !strcmp(j->key->str[q], nm)) { seen = 1; break; }
        if (seen) continue;
        int n = count_named(root, nm, leaves);
        if (n > 1) {
            snprintf(err, CP_ERRLEN, "`%s` names %d %s in the tree, so the join "
                     "is ambiguous; make the names unique", nm, n,
                     leaves ? "tips" : "internal nodes");
            return -1;
        }
    }
    return 0;
}

/* Colours for one name, in table order; returns how many rows matched. */
static int join_lookup(const Join *j, const char *name, Col *out, int cap) {
    if (!j->df || !name) return 0;
    int n = 0;
    for (int r = 0; r < j->df->nrow && n < cap; r++) {
        if (!j->key->str[r] || strcmp(j->key->str[r], name)) continue;
        if (j->discrete) {
            if (j->f->idx[r] < 0) continue;
            out[n++] = j->pal[j->f->idx[r]];
        } else {
            if (!isfinite(j->val->num[r])) continue;
            out[n++] = fill_map_value(&j->fs, j->val->num[r], j->dmin, j->dmax);
        }
    }
    return n;
}

static void emit(GTable *T, int R, int C, const TNode *t, const PlotSpec *spec,
                 double x0, double x1, double y0, double y1, double lw, int root,
                 const Join *jnode, const Join *jtip, const Join *jlab,
                 double aspect, double rmax, double cellw) {
#define TX(v) (((v) - x0) / (x1 - x0))
#define TY(v) (1.0 - ((v) - y0) / (y1 - y0))
/* Circular: the same (depth, leaf-index) layout read as (radius, angle). The
 * gap keeps the first and last tip from meeting, so the reader can see where
 * the tree starts. */
#define CIRC_GAP 0.06
#define THETA(v) ((((v) - y0) / (y1 - y0)) * (1 - CIRC_GAP) * 2 * M_PI)
#define RAD(v)   (rmax * ((v) - x0) / (x1 - x0))
/* npc x and y cover different numbers of points, so an uncorrected circle comes
 * out an ellipse. kx/ky scale the shorter axis to match the longer. */
#define CX(v, a) (0.5 + RAD(v) * cos(a) * kx)
#define CY(v, a) (0.5 + RAD(v) * sin(a) * ky)
    int circ = spec->tree_layout == 2, slant = spec->tree_layout == 1;
    double kx = aspect >= 1 ? 1.0 / aspect : 1.0;
    double ky = aspect >= 1 ? 1.0 : aspect;
    Grob *g;
    if (!tn_leaf(t)) {
        if (circ) {
            /* the spine is an arc at this node's radius, swept between its
             * first and last child */
            const int NA = 24;
            double a0 = THETA(t->kid[0]->y), a1 = THETA(t->kid[t->nkid - 1]->y);
            double *ax = cp_xmalloc(NA * sizeof(double));
            double *ay = cp_xmalloc(NA * sizeof(double));
            for (int k = 0; k < NA; k++) {
                double a = a0 + (a1 - a0) * k / (NA - 1);
                ax[k] = CX(t->x, a); ay[k] = CY(t->x, a);
            }
            g = gt_add(T, G_POLYLINE, R, C, R, C);
            g->n = NA; g->px = ax; g->py = ay; g->col = C_BLACK;
            g->lw = lw; g->clip = 1;
            for (int i = 0; i < t->nkid; i++) {   /* radial arm to each child */
                const TNode *c = t->kid[i];
                double a = THETA(c->y);
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_BLACK; g->lw = lw; g->clip = 1;
                g->x0 = CX(t->x, a); g->y0 = CY(t->x, a);
                g->x1 = CX(c->x, a); g->y1 = CY(c->x, a);
            }
        } else if (slant) {
            /* one straight line per child: no spine, which is what makes a
             * slanted tree read as a fan of descent rather than a circuit */
            for (int i = 0; i < t->nkid; i++) {
                const TNode *c = t->kid[i];
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_BLACK; g->lw = lw; g->clip = 1;
                g->x0 = TX(t->x); g->y0 = TY(t->y);
                g->x1 = TX(c->x); g->y1 = TY(c->y);
            }
        } else {
            /* the spine, spanning the first child to the last at this node's x */
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_BLACK; g->lw = lw; g->clip = 1;
            g->x0 = g->x1 = TX(t->x);
            g->y0 = TY(t->kid[0]->y); g->y1 = TY(t->kid[t->nkid - 1]->y);
            for (int i = 0; i < t->nkid; i++) {  /* an arm out to each child */
                const TNode *c = t->kid[i];
                g = gt_add(T, G_LINE, R, C, R, C);
                g->col = C_BLACK; g->lw = lw; g->clip = 1;
                g->x0 = TX(t->x); g->x1 = TX(c->x);
                g->y0 = g->y1 = TY(c->y);
            }
        }
    }
    if (t->name) {
        int leaf = tn_leaf(t);
        if (leaf ? spec->tree_tiplab : spec->tree_nodelab) {
            g = gt_add(T, G_TEXT, R, C, R, C);
            g->str = t->name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            if (leaf && jlab->df) {          /* geom_tiplab(data=, colour=) */
                Col c[1];
                if (join_lookup(jlab, t->name, c, 1) > 0) g->col = c[0];
            }
            if (circ) {
                /* Radiating outward, each at its own angle, and flipped on the
                 * left half so no label reads upside down. */
                double a = THETA(t->y), deg = a * 180 / M_PI;
                int flip = cos(a) < 0;
                double rr = RAD(t->x) + (leaf ? 0.012 : 0.006);
                g->tx = 0.5 + rr * cos(a) * kx;
                g->ty = 0.5 + rr * sin(a) * ky;
                g->rot = flip ? -deg + 180 : -deg;
                g->hj = flip ? 1 : 0;
                g->va = V_INKCENTER;
            } else {
                /* A tip label reads outward from its tip. An internal label
                 * sits above the branch running INTO its node and ends there:
                 * centring it on the node put a parent's label over its
                 * child's wherever the branch between them was short. The root
                 * has no incoming branch, so it reads forward instead of off
                 * the left edge. */
                int fwd = leaf || root;
                g->tx = TX(t->x) + (fwd ? 0.006 : -0.004);
                g->ty = TY(t->y) + (leaf ? 0 : 0.004);
                g->hj = fwd ? 0 : 1;
                g->va = leaf ? V_INKCENTER : V_BOTTOM;
            }
        }
    }
    {   /* geom_nodepoint()/geom_tippoint(): one mark per matching row, walked
         * back along the branch so several never sit on top of each other. */
        const Join *j = tn_leaf(t) ? jtip : jnode;
        Col cols[16];
        int n = j->df ? join_lookup(j, t->name, cols, 16) : 0;
        if (n > 0) {
            double *px = cp_xmalloc(n * sizeof(double));
            double *py = cp_xmalloc(n * sizeof(double));
            Col *pc = cp_xmalloc(n * sizeof(Col));
            for (int k = 0; k < n; k++) {
                if (circ) {                     /* stack back along the radius */
                    double a = THETA(t->y), rr = RAD(t->x) - k * 0.014;
                    px[k] = 0.5 + rr * cos(a) * kx;
                    py[k] = 0.5 + rr * sin(a) * ky;
                } else {
                    /* The nudge separating stacked marks is a fixed distance on
                     * the page, so it must be converted from points -- dividing
                     * by the DATA span made it grow with the canvas and threw
                     * the marks clear of their own node on a wide figure. */
                    px[k] = TX(t->x) - k * (PT_RADIUS * 2.6 / cellw);
                    py[k] = TY(t->y);
                }
                pc[k] = cols[k];
            }
            Grob *p = gt_add(T, G_POINTS, R, C, R, C);
            p->n = n; p->px = px; p->py = py; p->pcol = pc;
            p->radius = PT_RADIUS; p->clip = 1;
        }
    }
    for (int i = 0; i < t->nkid; i++)
        emit(T, R, C, t->kid[i], spec, x0, x1, y0, y1, lw, 0, jnode, jtip, jlab,
             aspect, rmax, cellw);
#undef TX
#undef TY
}

int render_tree(const PlotSpec *spec, const char *out,
                double w_pt, double h_pt, char *err) {
    char *text = read_all(spec->data_path, err);
    if (!text) return -1;
    TNode *root = newick_parse(text, err);
    if (!root) return -1;

    /* All the edges carry a length, or none do. A partly annotated tree used to
     * take the metric path and read every missing length as zero, which put the
     * unannotated tips on top of their own parent -- a picture that says they
     * branched at the root. Neither reading is safe to guess, so say so. */
    int nwith = 0, nwithout = 0;
    count_lengths(root, 1, &nwith, &nwithout);
    if (nwith && nwithout) {
        snprintf(err, CP_ERRLEN, "%d branches have a length and %d do not; give "
                 "every branch one, or none (a cladogram drawn from depth)",
                 nwith, nwithout);
        return -1;
    }
    int has_blen = nwith > 0;
    root->blen = 0;                        /* the root has no parent to measure to */
    cairo_surface_t *msurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(msurf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    /* Joined tables, loaded before layout so the legend can size the margin. */
    Join jnode, jtip, jlab;
    memset(&jnode, 0, sizeof jnode);
    memset(&jtip, 0, sizeof jtip);
    memset(&jlab, 0, sizeof jlab);
    if (spec->tree_nodepoint
        && join_load(&jnode, spec->tree_np_data, spec->tree_np_col, spec, err)) return -1;
    if (spec->tree_tippoint
        && join_load(&jtip, spec->tree_tp_data, spec->tree_tp_col, spec, err)) return -1;
    if (spec->tree_tl_data
        && join_load(&jlab, spec->tree_tl_data, spec->tree_tl_col, spec, err)) return -1;

    if (join_check_unique(&jnode, root, 0, err)) return -1;
    if (join_check_unique(&jtip, root, 1, err)) return -1;
    if (join_check_unique(&jlab, root, 1, err)) return -1;

    /* Measure before laying out: on a cladogram the branch lengths depend on
     * how wide the labels are. */
    measure_labels(cr, root, spec->tree_tiplab, spec->tree_nodelab, SZ_AXIS_TEXT);
    double tipw = spec->tree_tiplab ? label_w(cr, root, 1, SZ_AXIS_TEXT) : 0;
    double labh = font_h(cr, SZ_AXIS_TEXT);

    /* With fitting on, x is measured in points, so a branch is a real width
     * rather than an abstract 1. */
    double fit = (!has_blen && spec->tree_nodelab) ? TXT_GAP * 2 : 0;
    double next_y = 0, maxx = 0;
    int nleaf = 0;
    layout(root, 0, has_blen, &next_y, &maxx, &nleaf, fit, NULL);
    if (nleaf < 1) { snprintf(err, CP_ERRLEN, "the tree has no tips"); return -1; }

    /* Auto-fit: one readable row per tip, and enough width that the branching
     * is legible next to however long the tip labels are. */
    if (spec->tree_layout == 2) {          /* circular: a square, sized by the ring */
        /* tips sit around the circumference, so the radius follows from how
         * much arc each one needs */
        /* Each tip needs roughly a line-height of arc, plus slack: labels
         * radiate, so neighbours converge as they approach the ring. */
        double ring = nleaf * labh * 1.7;
        double rtree = ring / (2 * M_PI);
        /* Must invert rmax below, which is (mincell/2 - tipw - gap), or the
         * canvas grows without the drawn radius following it. */
        double side = 2 * (rtree + tipw + TXT_GAP * 2) + 2 * MARGIN;
        side = fmin(30.0 * 72, fmax(4.0 * 72, side));
        if (w_pt <= 0) w_pt = side;
        if (h_pt <= 0) h_pt = side;
    }
    if (h_pt <= 0) {
        double want = 2 * MARGIN + nleaf * labh * 1.25;
        h_pt = fmin(60.0 * 72, fmax(2.0 * 72, want));
        /* The cap is a safety limit, not a layout choice, so past it the
         * one-row-per-tip promise stops holding and the tips crowd. */
        if (want > 60.0 * 72)
            fprintf(stderr, "cinderplot: warning: %d tips need %.0f in; capped at "
                    "60 in, so tip labels will crowd (set --size to override)\n",
                    nleaf, want / 72);
    }
    if (w_pt <= 0) {
        /* A fitted cladogram measured its branches in points already; anything
         * else gets a readable width per unit of depth. */
        double body = fit > 0 ? maxx : fmax(120.0, maxx * 26.0);
        double legroom = (spec->tree_nodepoint || spec->tree_tippoint
                          || spec->tree_tl_data) ? 90 : 0;
        w_pt = fmin(30.0 * 72, fmax(3.0 * 72, 2 * MARGIN + tipw + body + legroom));
    }
    cairo_destroy(cr); cairo_surface_destroy(msurf);

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    GTable *T = cp_xcalloc(1, sizeof *T);
    /* A joined aesthetic needs a key, or the colours say nothing. One legend,
     * for whichever join is present -- a tree carrying two different mapped
     * tables at once is not a figure worth encouraging. */
    const Join *leg = jnode.df ? &jnode : jtip.df ? &jtip : jlab.df ? &jlab : NULL;
    const char *leg_title = leg == &jnode ? spec->tree_np_col
                          : leg == &jtip  ? spec->tree_tp_col
                          : leg == &jlab  ? spec->tree_tl_col : NULL;
    double legw = 0;
    if (leg) {
        double w = leg_title ? text_w(cr, SZ_BASE, leg_title) : 0;
        if (leg->discrete)
            for (int k = 0; k < leg->f->nlev; k++) {
                double kw = text_w(cr, SZ_AXIS_TEXT, leg->f->levels[k]);
                if (kw > w) w = kw;
            }
        else {
            char b[32];
            fmt_num(leg->dmax, b, sizeof b);
            double kw = text_w(cr, SZ_AXIS_TEXT, b);
            if (kw > w) w = kw;
        }
        legw = 2 * HALF_LINE + KEY_SIZE + TXT_GAP + w;
    }

    T->ncol = 4;
    T->colw[0] = upt(MARGIN);
    T->colw[1] = unull(1);
    /* Rectangular and slanted trees put every tip label on the right, so the
     * room goes in a column. A circular tree radiates them all round, so the
     * room comes out of the radius instead (rmax below). */
    T->colw[2] = upt(spec->tree_layout == 2 ? MARGIN : MARGIN + tipw);
    T->colw[3] = upt(legw ? legw + MARGIN : 0);
    T->nrow = 3;
    T->rowh[0] = upt(MARGIN + (spec->lab_title ? font_h(cr, SZ_TITLE) : 0));
    T->rowh[1] = unull(1);
    T->rowh[2] = upt(MARGIN);
    const int R = 1, C = 1;

    if (spec->lab_title) {
        Grob *g = gt_add(T, G_TEXT, 0, C, 0, C);
        g->str = spec->lab_title; g->size = SZ_TITLE; g->col = C_BLACK;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }

    /* Half a row of padding top and bottom so the first and last tips are not
     * flush against the panel edge. */
    double y0 = -0.5, y1 = (nleaf - 1) + 0.5;
    double x0 = 0, x1 = maxx > 0 ? maxx : 1;
    /* the canvas cell, in points, so the circular mapping can stay round */
    double cellw = w_pt - 2 * MARGIN - (spec->tree_layout == 2 ? 0 : tipw)
                 - (legw ? legw + MARGIN : 0);
    double cellh = h_pt - 2 * MARGIN - (spec->lab_title ? font_h(cr, SZ_TITLE) : 0);
    /* The circle is measured against the shorter side, so leave the labels
     * their width there and the ring cannot run off the surface. */
    double mincell = fmin(cellw, cellh);
    double rmax = 0.5;
    if (spec->tree_layout == 2 && mincell > 0)
        rmax = fmax(0.12, 0.5 - (tipw + TXT_GAP * 2) / mincell);
    emit(T, R, C, root, spec, x0, x1, y0, y1, lw_pt(0.5), 1,
         &jnode, &jtip, &jlab, cellh > 0 ? cellw / cellh : 1, rmax,
         cellw > 0 ? cellw : 1);

    if (leg) {                        /* key, centred in its own column */
        double ch = h_pt - 2 * MARGIN;
        int nk = leg->discrete ? leg->f->nlev : 0;
        double lh = font_h(cr, SZ_AXIS_TEXT);
        double blockh = leg->discrete ? nk * fmax(KEY_SIZE, lh) : 6 * KEY_SIZE;
        double titleh2 = leg_title ? font_h(cr, SZ_BASE) + HALF_LINE / 2 : 0;
        double top = 0.5 + (blockh + titleh2) / 2 / ch;
        if (leg_title) {
            Grob *g2 = gt_add(T, G_TEXT, R, 3, R, 3);
            g2->str = (char *)leg_title; g2->size = SZ_BASE; g2->col = C_BLACK;
            g2->tx = 0; g2->ty = top; g2->hj = 0; g2->va = V_TOP;
        }
        double y = top - titleh2 / ch;
        /* npc inside the legend CELL, which is legw wide -- not the figure. */
        double cw = legw > 0 ? legw : 1;
        if (leg->discrete) {
            double kh = fmax(KEY_SIZE, lh) / ch;
            for (int k = 0; k < nk; k++) {
                double yc = y - (k + 0.5) * kh;
                double *px = cp_xmalloc(sizeof(double)), *py = cp_xmalloc(sizeof(double));
                Col *pc = cp_xmalloc(sizeof(Col));
                px[0] = (KEY_SIZE / 2) / cw; py[0] = yc; pc[0] = leg->pal[k];
                Grob *g2 = gt_add(T, G_POINTS, R, 3, R, 3);
                g2->n = 1; g2->px = px; g2->py = py; g2->pcol = pc;
                g2->radius = PT_RADIUS * 1.4;
                g2 = gt_add(T, G_TEXT, R, 3, R, 3);
                g2->str = leg->f->levels[k]; g2->size = SZ_AXIS_TEXT; g2->col = C_BLACK;
                g2->tx = (KEY_SIZE + TXT_GAP) / cw; g2->ty = yc;
                g2->hj = 0; g2->va = V_INKCENTER;
            }
        } else {
            const int NB = 48;               /* the ramp, sampled into bands */
            double barh = blockh / ch, bw = KEY_SIZE * 0.55 / cw;
            for (int k = 0; k < NB; k++) {
                double f0 = (double)k / NB, f1 = (double)(k + 1) / NB;
                Grob *g2 = gt_add(T, G_RECT, R, 3, R, 3);
                g2->sub = 1;                 /* without this a rect fills the cell */
                g2->col = fill_map(&leg->fs, f0);
                g2->x0 = 0; g2->x1 = bw;
                g2->y0 = y - barh + f0 * barh; g2->y1 = y - barh + f1 * barh;
            }
            char b[32];
            for (int k = 0; k < 2; k++) {
                fmt_num(k ? leg->dmax : leg->dmin, b, sizeof b);
                Grob *g2 = gt_add(T, G_TEXT, R, 3, R, 3);
                g2->str = cp_xstrdup(b); g2->size = SZ_AXIS_TEXT; g2->col = C_BLACK;
                g2->tx = bw + TXT_GAP / cw;
                g2->ty = k ? y : y - barh;
                g2->hj = 0; g2->va = V_INKCENTER;
            }
        }
    }

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);
    cairo_destroy(cr);
    cairo_status_t st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        snprintf(err, CP_ERRLEN, "could not write %s: %s", out, cairo_status_to_string(st));
        return -1;
    }
    return 0;
}
