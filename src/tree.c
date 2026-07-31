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

static void emit(GTable *T, int R, int C, const TNode *t, const PlotSpec *spec,
                 double x0, double x1, double y0, double y1, double lw, int root) {
#define TX(v) (((v) - x0) / (x1 - x0))
#define TY(v) (1.0 - ((v) - y0) / (y1 - y0))
    Grob *g;
    if (!tn_leaf(t)) {
        /* the spine, spanning the first child to the last at this node's x */
        g = gt_add(T, G_LINE, R, C, R, C);
        g->col = C_BLACK; g->lw = lw; g->clip = 1;
        g->x0 = g->x1 = TX(t->x);
        g->y0 = TY(t->kid[0]->y); g->y1 = TY(t->kid[t->nkid - 1]->y);
        for (int i = 0; i < t->nkid; i++) {      /* an arm out to each child */
            const TNode *c = t->kid[i];
            g = gt_add(T, G_LINE, R, C, R, C);
            g->col = C_BLACK; g->lw = lw; g->clip = 1;
            g->x0 = TX(t->x); g->x1 = TX(c->x);
            g->y0 = g->y1 = TY(c->y);
        }
    }
    if (t->name) {
        int leaf = tn_leaf(t);
        if (leaf ? spec->tree_tiplab : spec->tree_nodelab) {
            g = gt_add(T, G_TEXT, R, C, R, C);
            g->str = t->name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            /* A tip label reads outward from its tip. An internal label sits
             * above the branch running INTO its node and ends there: centring
             * it on the node put a parent's label over its child's wherever
             * the branch between them was short. */
            /* The root has no incoming branch to sit on, so its label reads
             * forward from the node instead of off the left edge. */
            int fwd = leaf || root;
            g->tx = TX(t->x) + (fwd ? 0.006 : -0.004);
            g->ty = TY(t->y) + (leaf ? 0 : 0.004);
            g->hj = fwd ? 0 : 1;
            g->va = leaf ? V_INKCENTER : V_BOTTOM;
        }
    }
    for (int i = 0; i < t->nkid; i++)
        emit(T, R, C, t->kid[i], spec, x0, x1, y0, y1, lw, 0);
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
        w_pt = fmin(30.0 * 72, fmax(3.0 * 72, 2 * MARGIN + tipw + body));
    }
    cairo_destroy(cr); cairo_surface_destroy(msurf);

    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);

    GTable *T = cp_xcalloc(1, sizeof *T);
    T->ncol = 3;
    T->colw[0] = upt(MARGIN);
    T->colw[1] = unull(1);
    T->colw[2] = upt(MARGIN + tipw);      /* room for the tip labels */
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
    emit(T, R, C, root, spec, x0, x1, y0, y1, lw_pt(0.5), 1);

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
