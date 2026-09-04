/* dsl.c — parser for the verbatim-ggplot2 DSL subset.
 *
 *   expr  := data ('+' term)*
 *   data  := path            (first term; anything not followed by '(')
 *   term  := NAME '(' args ')'
 *
 * Supported: aes() with positional x,y and named x/y/colour/color,
 * values IDENT or factor(IDENT); geom_point(); labs(title/x/y/colour=
 * "string"); facet_wrap(~var). Anything else errors with the supported
 * subset listed, so unimplemented ggplot is a clear "not yet" rather
 * than a syntax error. */
#include "cinderplot.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *s; char *err; } P;

static void skip_ws(P *p) { while (isspace((unsigned char)*p->s)) p->s++; }

static int fail(P *p, const char *fmt, const char *a) {
    snprintf(p->err, CP_ERRLEN, fmt, a);
    return -1;
}

/* identifier: letters, digits, '_', '.' (R-style column names) */
static char *ident(P *p) {
    skip_ws(p);
    const char *s = p->s;
    while (isalnum((unsigned char)*p->s) || *p->s == '_' || *p->s == '.') p->s++;
    if (p->s == s) return NULL;
    return strndup(s, p->s - s);
}

static int expect(P *p, char c) {
    skip_ws(p);
    if (*p->s != c) {
        char buf[64];
        snprintf(buf, sizeof buf, "expected '%c' near \"%.20s\"", c, p->s);
        return fail(p, "%s", buf);
    }
    p->s++;
    return 0;
}

static char *string_lit(P *p) {
    skip_ws(p);
    if (*p->s != '"') return NULL;
    p->s++;
    size_t cap = strlen(p->s) + 1, n = 0;
    char *out = cp_xmalloc(cap);
    while (*p->s && *p->s != '"') {
        if (*p->s == '\\' && p->s[1]) {
            p->s++;
            if (*p->s == 'n') out[n++] = '\n';
            else if (*p->s == 't') out[n++] = '\t';
            else out[n++] = *p->s;
            p->s++;
        } else out[n++] = *p->s++;
    }
    if (*p->s != '"') { free(out); return NULL; }
    p->s++;
    out[n] = 0;
    return out;
}

/* raw value token: filename or bare word (until , ) or whitespace) */
static char *raw_token(P *p);

/* levels=c("a", "b", ...) — an explicit discrete order. Elements may be quoted
 * or bare (so numeric levels read as c(8, 4, 6)); they are matched against the
 * factor's level labels at render time. Leaves *out owning the strings. */
static int parse_levels(P *p, char ***out, int *n) {
    skip_ws(p);
    if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
    else return fail(p, "levels= expects c(\"a\", \"b\", ...)", "");
    int cap = 0;
    *out = NULL; *n = 0;
    for (;;) {
        skip_ws(p);
        if (*p->s == ')') { p->s++; break; }
        char *v = raw_token(p);
        if (!v || !*v) { free(v); return fail(p, "bad value in levels=c(...)", ""); }
        if (*n == cap) { cap = cap ? cap * 2 : 8; *out = cp_xrealloc(*out, cap * sizeof(char *)); }
        (*out)[(*n)++] = v;
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        if (*p->s == ')') { p->s++; break; }
        return fail(p, "expected , or ) in levels=c(...)", "");
    }
    if (*n == 0) return fail(p, "levels=c() is empty", "");
    return 0;
}

/* value in aes: IDENT or factor(IDENT[, levels=c(...)]); fills entry incl.
 * source text */
static int aes_value(P *p, AesEntry *e) {
    const char *start;
    skip_ws(p);
    start = p->s;
    char *id = ident(p);
    if (!id) return fail(p, "expected a column name near \"%.20s\"", p->s);
    if (!strcmp(id, "factor")) {
        if (expect(p, '(')) return -1;
        e->col = ident(p);
        if (!e->col) return fail(p, "expected a column name in factor() near \"%.20s\"", p->s);
        skip_ws(p);
        if (*p->s == ',') {                       /* factor(col, levels=c(...)) */
            p->s++;
            char *key = ident(p);
            if (!key || strcmp(key, "levels") || expect(p, '='))
                return fail(p, "factor() supports only levels=c(...)", "");
            free(key);
            if (parse_levels(p, &e->levels, &e->nlevels)) return -1;
        }
        if (expect(p, ')')) return -1;
        e->is_factor = 1;
        free(id);
    } else {
        e->col = id;
        e->is_factor = 0;
    }
    /* The source text becomes the default axis/legend title. A levels=c(...)
     * list is long enough to squeeze the panel to nothing, so elide it — the
     * title reads factor(col), and labs() still overrides it. */
    if (e->nlevels) {
        size_t n = strlen(e->col) + 10;
        e->expr = cp_xmalloc(n);
        snprintf(e->expr, n, "factor(%s)", e->col);
    } else e->expr = strndup(start, p->s - start);
    return 0;
}

static int parse_aes(P *p, PlotSpec *spec) {
    int pos = 0;
    skip_ws(p);
    if (*p->s == ')') { p->s++; return 0; }
    for (;;) {
        skip_ws(p);
        /* named arg? lookahead for IDENT '=' (but not '==') */
        const char *save = p->s;
        char *key = ident(p);
        AesEntry *e = NULL;
        skip_ws(p);
        if (key && *p->s == '=') {
            p->s++;
            if (!strcmp(key, "x") || !strcmp(key, "xmin")) e = &spec->x;
            else if (!strcmp(key, "y") || !strcmp(key, "ymin")) e = &spec->y;
            else if (!strcmp(key, "xend") || !strcmp(key, "xmax")) e = &spec->xend;
            else if (!strcmp(key, "yend") || !strcmp(key, "ymax")) e = &spec->yend;
            else if (!strcmp(key, "chrom") || !strcmp(key, "chr")) e = &spec->chrom;
            else if (!strcmp(key, "label")) e = &spec->label;
            else if (!strcmp(key, "size")) e = &spec->size;
            else if (!strcmp(key, "shape")) e = &spec->shape;
            else if (!strcmp(key, "colour") || !strcmp(key, "color")
                  || !strcmp(key, "fill")) {
                e = &spec->colour;
                spec->colour.is_fill = key[0] == 'f';
            }
            else return fail(p, "aes(%s=...) is not implemented; supported: x, y, xend, yend, label, size, shape, chrom, colour, fill", key);
            free(key);
        } else {
            p->s = save;                     /* positional: x then y */
            if (pos == 0) e = &spec->x;
            else if (pos == 1) e = &spec->y;
            else return fail(p, "too many positional aes() arguments near \"%.20s\"", p->s);
            pos++;
        }
        if (aes_value(p, e)) return -1;
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        return expect(p, ')');
    }
}

static int parse_labs(P *p, PlotSpec *spec) {
    skip_ws(p);
    if (*p->s == ')') { p->s++; return 0; }
    for (;;) {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "labs() takes key=\"value\" pairs", "");
        /* ggplot2 uses labs(x=NULL) to drop a title entirely, as distinct from
         * x="" which keeps the reserved space. Accept the spelling; we render
         * it as empty, which differs from ggplot2 only in that the blank line
         * is still reserved. */
        skip_ws(p);
        char *val;
        if (!strncmp(p->s, "NULL", 4) && !isalnum((unsigned char)p->s[4]) && p->s[4] != '_') {
            p->s += 4;
            val = cp_xstrdup("");
        } else if (!(val = string_lit(p)))
            return fail(p, "labs(%s=...) expects a quoted string or NULL", key);
        if (!strcmp(key, "title")) spec->lab_title = val;
        else if (!strcmp(key, "subtitle")) spec->lab_subtitle = val;
        else if (!strcmp(key, "caption")) spec->lab_caption = val;
        else if (!strcmp(key, "x")) spec->lab_x = val;
        else if (!strcmp(key, "y")) spec->lab_y = val;
        else if (!strcmp(key, "colour") || !strcmp(key, "color")) spec->lab_colour = val;
        else if (!strcmp(key, "fill")) spec->lab_fill = val;
        else return fail(p, "labs(%s=...) is not implemented; supported: title, subtitle, caption, x, y, colour, fill", key);
        free(key);
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        return expect(p, ')');
    }
}

/* raw value token: filename or bare word (until , ) or whitespace) */
static char *raw_token(P *p) {
    skip_ws(p);
    if (*p->s == '"') return string_lit(p);
    const char *s = p->s;
    while (*p->s && !strchr(",() \t\n", *p->s)) p->s++;
    if (p->s == s) return NULL;
    return strndup(s, p->s - s);
}

/* placement: kind name already consumed; parse ([anchor][, k=v...]) */
static int parse_place(P *p, const char *kind, HPlace *pl) {
    pl->kind = !strcmp(kind, "top_of") ? PL_TOP_OF
             : !strcmp(kind, "beneath") ? PL_BENEATH
             : !strcmp(kind, "right_of") ? PL_RIGHT_OF : PL_LEFT_OF;
    pl->anchor = NULL; pl->pad = 0.01; pl->width = -1; pl->height = -1;
    if (expect(p, '(')) return -1;
    skip_ws(p);
    if (*p->s == ')') { p->s++; return 0; }
    for (;;) {
        skip_ws(p);
        const char *save = p->s;
        char *key = ident(p);
        skip_ws(p);
        if (key && *p->s == '=') {
            p->s++;
            skip_ws(p);
            double v = strtod(p->s, (char **)&p->s);
            if (!strcmp(key, "pad")) pl->pad = v;
            else if (!strcmp(key, "width")) pl->width = v;
            else if (!strcmp(key, "height")) pl->height = v;
            else return fail(p, "placement option `%s` not implemented; supported: pad, width, height", key);
        } else {
            p->s = save;
            pl->anchor = raw_token(p);
            if (!pl->anchor) return fail(p, "bad placement anchor near \"%.20s\"", p->s);
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        return expect(p, ')');
    }
}

/* ---- track (locus-browser) mode ---- */
static TrackObj *trk_new(P *p, PlotSpec *spec, TrackType t) {
    if (spec->ntracks == MAX_TRACKS) { fail(p, "too many tracks", ""); return NULL; }
    TrackObj *o = &spec->tobjs[spec->ntracks++];
    memset(o, 0, sizeof *o);
    o->type = t;
    return o;
}

static int parse_trk_args(P *p, TrackObj *o) {
    skip_ws(p);
    if (*p->s == ')') { p->s++; goto done; }
    for (;;) {
        skip_ws(p);
        const char *save = p->s;
        char *key = ident(p);
        skip_ws(p);
        if (key && *p->s == '=') {
            p->s++;
            if (!strcmp(key, "name")) {
                o->name = string_lit(p);
                if (!o->name) return fail(p, "name= expects a quoted string", "");
            } else if (!strcmp(key, "height")) {
                skip_ws(p); o->height = strtod(p->s, (char **)&p->s);
            } else if (!strcmp(key, "max")) {
                skip_ws(p); o->max_value = strtod(p->s, (char **)&p->s);
            } else if (!strcmp(key, "color") || !strcmp(key, "colour")) {
                char *v = string_lit(p);
                if (!v || parse_color(v, &o->color)) return fail(p, "bad track colour", "");
                o->has_color = 1;
            } else if (!strcmp(key, "data")) {
                o->data = string_lit(p);
                if (!o->data) return fail(p, "data= expects a quoted path", "");
            } else if (!strcmp(key, "cluster")) {
                char *v = ident(p);
                if (!v) return fail(p, "cluster= expects samples or none", "");
                if (!strcmp(v, "samples") || !strcmp(v, "rows")) o->cluster = 1;
                else if (!strcmp(v, "none")) o->cluster = 0;
                else return fail(p, "cluster=%s invalid; use samples or none", v);
            } else if (!strcmp(key, "rownames")) {
                char *v = ident(p);
                if (!v) return fail(p, "rownames= expects on or off", "");
                if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "hide")) o->hide_rownames = 1;
                else if (!strcmp(v, "on") || !strcmp(v, "show")) o->hide_rownames = 0;
                else return fail(p, "rownames=%s invalid; use on or off", v);
            } else if (!strcmp(key, "colnames")) {
                /* Symmetric with rownames=. For a CpG matrix the per-probe
                 * labels are almost always noise -- 127 of them collapse into
                 * an unreadable band about a third of the figure high -- so
                 * turning them off has to be reachable. */
                char *v = ident(p);
                if (!v) return fail(p, "colnames= expects on or off", "");
                if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "hide")) o->hide_colnames = 1;
                else if (!strcmp(v, "on") || !strcmp(v, "show")) o->hide_colnames = 0;
                else return fail(p, "colnames=%s invalid; use on or off", v);
            } else if (!strcmp(key, "transcripts")) {
                char *v = ident(p);
                if (!v) return fail(p, "transcripts= expects all or canonical", "");
                if (!strcmp(v, "all")) o->all_transcripts = 1;
                else if (!strcmp(v, "canonical") || !strcmp(v, "longest")) o->all_transcripts = 0;
                else return fail(p, "transcripts=%s invalid; use all or canonical", v);
            } else return fail(p, "track option `%s` not implemented; supported: name, "
                                  "height, max, color, data, cluster, rownames, transcripts", key);
        } else {
            p->s = save;
            char *v = raw_token(p);
            if (!v || o->data) return fail(p, "unexpected argument near \"%.20s\"", save);
            o->data = v;
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        if (expect(p, ')')) return -1;
        break;
    }
done:
    if (!o->data) return fail(p, "this track needs a data file", "");
    return 0;
}

static HMObj *hm_new(P *p, PlotSpec *spec, HMType t) {
    if (spec->nhobjs == MAX_HMOBJS) { fail(p, "too many heatmap objects", ""); return NULL; }
    HMObj *o = &spec->hobjs[spec->nhobjs++];
    memset(o, 0, sizeof *o);
    o->type = t;
    o->place.kind = spec->nhobjs == 1 ? PL_FULL : PL_TOP_OF;
    o->place.pad = 0.01; o->place.width = -1; o->place.height = -1;
    o->name = cp_xmalloc(8);
    sprintf(o->name, "h%d", spec->nhobjs);
    return o;
}

static int is_place_name(const char *s) {
    return !strcmp(s, "top_of") || !strcmp(s, "beneath")
        || !strcmp(s, "right_of") || !strcmp(s, "left_of");
}

/* heatmap(...) / annotation(file, ...) / legend(...) argument list */
static int parse_hm_args(P *p, HMObj *o, int want_data) {
    skip_ws(p);
    if (*p->s == ')') { p->s++; goto done; }
    for (;;) {
        skip_ws(p);
        const char *save = p->s;
        char *key = ident(p);
        skip_ws(p);
        if (key && *p->s == '(' && is_place_name(key)) {
            if (parse_place(p, key, &o->place)) return -1;
        } else if (key && *p->s == '=') {
            p->s++;
            if (!strcmp(key, "name")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "name= expects a quoted string", "");
                o->name = v;
            } else if (!strcmp(key, "data")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "data= expects a quoted path", "");
                o->data = v;
            } else if (!strcmp(key, "column")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "column= expects a quoted column name", "");
                o->column = v;
            } else if (!strcmp(key, "title")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "title= expects a quoted string", "");
                o->title = v;
            } else if (!strcmp(key, "cluster")) {
                char *v = ident(p);
                if (!v) return fail(p, "cluster= expects rows, cols, both, "
                                    "diagonal, symmetric, or none", "");
                if (!strcmp(v, "rows")) o->cluster = CL_ROWS;
                else if (!strcmp(v, "cols") || !strcmp(v, "columns")) o->cluster = CL_COLS;
                else if (!strcmp(v, "both")) o->cluster = CL_BOTH;
                else if (!strcmp(v, "diagonal")) o->cluster = CL_DIAGONAL;
                else if (!strcmp(v, "symmetric")) o->cluster = CL_SYMMETRIC;
                /* `off` because rownames=/colnames= spell it that way. */
                else if (!strcmp(v, "none") || !strcmp(v, "off")) o->cluster = CL_NONE;
                else return fail(p, "cluster=%s invalid; use rows, cols, both, "
                                 "diagonal, symmetric, or none", v);
            } else if (!strcmp(key, "rownames") || !strcmp(key, "colnames")) {
                int row = key[0] == 'r';
                char *v = ident(p);
                if (!v) return fail(p, "%s= expects left/right (rownames) or top/bottom (colnames), or none", key);
                Side s;
                /* `off`/`hide` because matrix() tracks spell the same idea that
                 * way; the two modes should not disagree on how to say it. */
                if (!strcmp(v, "none") || !strcmp(v, "off") || !strcmp(v, "hide"))
                    s = SIDE_NONE;
                else if (row && !strcmp(v, "left")) s = SIDE_LEFT;
                else if (row && !strcmp(v, "right")) s = SIDE_RIGHT;
                else if (!row && !strcmp(v, "top")) s = SIDE_TOP;
                else if (!row && !strcmp(v, "bottom")) s = SIDE_BOTTOM;
                else return fail(p, row ? "rownames= must be left, right, or none"
                                        : "colnames= must be top, bottom, or none", "");
                if (row) o->rownames = s; else o->colnames = s;
            } else if (!strcmp(key, "labels")) {
                char *v = ident(p);
                if (!v) return fail(p, "labels= expects data/on or none/off", "");
                if (!strcmp(v, "data") || !strcmp(v, "on") || !strcmp(v, "true")) o->label_data = 1;
                else if (!strcmp(v, "none") || !strcmp(v, "off") || !strcmp(v, "false")) o->label_data = 0;
                else return fail(p, "labels=%s invalid; use data/on or none/off", v);
            } else if (!strcmp(key, "aspect")) {
                skip_ws(p);
                char *end;
                double v = strtod(p->s, &end);
                if (end == p->s || v <= 0)
                    return fail(p, "aspect= expects a positive number "
                                "(1 = square)", "");
                p->s = end;
                o->aspect = v;
            } else if (!strcmp(key, "box")) {
                /* A quoted value is a colour and implies on, so the common case
                 * -- box="grey40" -- does not need box=on beside it. */
                skip_ws(p);
                if (*p->s == '"') {
                    char *v = string_lit(p);
                    if (!v || parse_color(v, &o->box_col))
                        return fail(p, "box= expects on/off or a quoted colour", "");
                    o->box = 1;
                } else {
                    char *v = ident(p);
                    if (!v) return fail(p, "box= expects on/off or a quoted colour", "");
                    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "TRUE")) {
                        o->box = 1;
                        Col grey = {0.4, 0.4, 0.4};   /* as matrix() tracks frame theirs */
                        o->box_col = grey;
                    }
                    else if (!strcmp(v, "none") || !strcmp(v, "off")
                             || !strcmp(v, "false") || !strcmp(v, "FALSE")) o->box = 0;
                    else return fail(p, "box=%s invalid; use on/off or a quoted colour", v);
                }
            } else if (!strcmp(key, "grid")) {
                /* Separators BETWEEN the cells, where box= frames the block.
                 * Same spelling rules as box=: a quoted value is a colour and
                 * implies on. geom_tile(colour="grey70") is the ggplot2 idea. */
                skip_ws(p);
                if (*p->s == '"') {
                    char *v = string_lit(p);
                    if (!v || parse_color(v, &o->grid_col))
                        return fail(p, "grid= expects on/off or a quoted colour", "");
                    o->grid = 1;
                } else {
                    char *v = ident(p);
                    if (!v) return fail(p, "grid= expects on/off or a quoted colour", "");
                    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "TRUE")) {
                        o->grid = 1;
                        Col grey = {0.702, 0.702, 0.702};   /* grey70: dim on white, legible on fills */
                        o->grid_col = grey;
                    }
                    else if (!strcmp(v, "none") || !strcmp(v, "off")
                             || !strcmp(v, "false") || !strcmp(v, "FALSE")) o->grid = 0;
                    else return fail(p, "grid=%s invalid; use on/off or a quoted colour", v);
                }
            } else return fail(p, "option `%s` not implemented; supported: name=, data=, "
                                  "cluster=, rownames=, colnames=, labels=, box=, grid=, "
                                  "aspect=, placements", key);
        } else {
            p->s = save;
            char *v = raw_token(p);
            if (!v || !want_data || o->data)
                return fail(p, "unexpected argument near \"%.20s\"", save);
            o->data = v;
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        if (expect(p, ')')) return -1;
        break;
    }
done:
    if (want_data && !o->data && o->type == HM_ANNOTATION)
        return fail(p, "annotation() needs a data file: annotation(groups.csv, top_of(\"name\"))", "");
    return 0;
}

/* parse a gradient/viridis/jet/bwr scale body into `fs`; `k` is the suffix
 * after scale_(fill|colour)_ , `fn` the prefix for error messages */
static int parse_grad_scale(P *p, FillScale *fs, const char *k, const char *fn) {
    if (!strcmp(k, "viridis")) fs->kind = FILL_VIRIDIS;
    else if (!strcmp(k, "jet")) fs->kind = FILL_JET;
    else if (!strcmp(k, "parula")) fs->kind = FILL_PARULA;
    else if (!strcmp(k, "bwr")) fs->kind = FILL_BWR;
    else if (!strcmp(k, "turbo")) fs->kind = FILL_TURBO;
    else if (!strcmp(k, "coolwarm")) fs->kind = FILL_COOLWARM;
    else if (!strcmp(k, "magma")) fs->kind = FILL_MAGMA;
    else if (!strcmp(k, "inferno")) fs->kind = FILL_INFERNO;
    else if (!strcmp(k, "plasma")) fs->kind = FILL_PLASMA;
    else if (!strcmp(k, "cividis")) fs->kind = FILL_CIVIDIS;
    else if (!strcmp(k, "rocket")) fs->kind = FILL_ROCKET;
    else if (!strcmp(k, "mako")) fs->kind = FILL_MAKO;
    else if (!strcmp(k, "gradient")) {
        fs->kind = FILL_GRADIENT;
        parse_color("#132B43", &fs->low); parse_color("#56B1F7", &fs->high);
    } else if (!strcmp(k, "gradient2")) {
        fs->kind = FILL_GRADIENT2;
        parse_color("#832424", &fs->low); parse_color("white", &fs->mid);
        parse_color("#3A3A98", &fs->high); fs->midpoint = 0;
    } else {
        char msg[256];   /* the supported list is long now */
        snprintf(msg, sizeof msg, "`%s%s()` not implemented; supported: "
                 "viridis, magma, inferno, plasma, cividis, rocket, mako, "
                 "parula, turbo, coolwarm, bwr, jet, gradient, gradient2", fn, k);
        return fail(p, "%s", msg);
    }
    skip_ws(p);
    while (*p->s != ')') {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "bad scale argument", "");
        skip_ws(p);
        if (!strcmp(key, "midpoint")) {
            fs->midpoint = strtod(p->s, (char **)&p->s);
        } else if (!strcmp(key, "limits")) {         /* c(lo, hi) — domain + squish */
            skip_ws(p);
            if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
            else return fail(p, "limits= expects c(lo, hi)", "");
            fs->lim_lo = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ',') p->s++;
            fs->lim_hi = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ')') p->s++;
            if (!(fs->lim_lo < fs->lim_hi))
                return fail(p, "limits= expects lo < hi", "");
            fs->has_limits = 1;
        } else {
            char *v = string_lit(p); Col c;
            if (!v || parse_color(v, &c))
                return fail(p, "bad colour for `%s` (use names or #RRGGBB)", key);
            if (!strcmp(key, "low")) fs->low = c;
            else if (!strcmp(key, "mid")) fs->mid = c;
            else if (!strcmp(key, "high")) fs->high = c;
            else return fail(p, "scale option `%s` not implemented", key);
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; skip_ws(p); }
    }
    p->s++;
    return 0;
}

/* scale_(fill|colour)_manual(values=c(...)) — explicit discrete palette.
 * Elements are quoted colours, optionally named:  c("red","blue") or
 * c(setosa="red", virginica="#1b9e77"). Names map to factor levels; an
 * unnamed list maps by level order. */
/* scale_*_distiller(palette="YlOrBr"[, direction=-1][, limits=c(lo,hi)]):
 * a named ColorBrewer ramp as the continuous scale. direction follows
 * ggplot2: the DEFAULT -1 reverses the printed palette order so high values
 * take the light end; direction=1 reads the palette as printed (light low,
 * dark high) — usually what a light-to-dark manuscript figure wants. */
static int parse_distiller(P *p, FillScale *fs) {
    char *pname = NULL;
    int dir = -1;
    skip_ws(p);
    while (*p->s != ')') {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "bad scale_*_distiller argument", "");
        skip_ws(p);
        if (!strcmp(key, "palette")) {
            pname = string_lit(p);
            if (!pname) return fail(p, "palette= expects a quoted ColorBrewer name", "");
        } else if (!strcmp(key, "direction")) {
            char *end;
            double v = strtod(p->s, &end);
            if (end == p->s || (v != 1 && v != -1))
                return fail(p, "direction= expects 1 or -1", "");
            p->s = end; dir = (int)v;
        } else if (!strcmp(key, "limits")) {
            if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
            else return fail(p, "limits= expects c(lo, hi)", "");
            fs->lim_lo = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ',') p->s++;
            fs->lim_hi = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ')') p->s++;
            if (!(fs->lim_lo < fs->lim_hi))
                return fail(p, "limits= expects lo < hi", "");
            fs->has_limits = 1;
        } else return fail(p, "scale_*_distiller option `%s` not implemented "
                           "(palette=, direction=, limits=)", key);
        skip_ws(p);
        if (*p->s == ',') { p->s++; skip_ws(p); }
    }
    if (!pname) return fail(p, "scale_*_distiller needs palette=\"...\"", "");
    int nst, qual;
    if (brewer_lookup(pname, fs->stops, &nst, &qual))
        return fail(p, "unknown ColorBrewer palette `%s` (e.g. YlOrBr, YlGnBu, "
                    "Blues, Greys; RdBu, Spectral)", pname);
    if (qual)
        return fail(p, "`%s` is a qualitative set, not a ramp; use "
                    "scale_*_brewer(palette=) on a discrete aesthetic", pname);
    if (dir == -1)                       /* ggplot2's default orientation */
        for (int i = 0; i < nst / 2; i++) {
            Col t = fs->stops[i];
            fs->stops[i] = fs->stops[nst - 1 - i];
            fs->stops[nst - 1 - i] = t;
        }
    fs->nstops = nst;
    fs->kind = FILL_BREWER;
    return expect(p, ')');
}

/* scale_*_brewer(palette="Set2"): a qualitative ColorBrewer set for a
 * discrete colour/fill, routed through the manual-palette machinery so the
 * legend and level mapping come for free. Qualitative only: a sequential
 * palette's n-class sets are NOT its first n colours, so first-n would be a
 * silently different palette than R would draw. */
static int parse_brewer_discrete(P *p, PlotSpec *spec) {
    char *pname = NULL;
    skip_ws(p);
    while (*p->s != ')') {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "bad scale_*_brewer argument", "");
        skip_ws(p);
        if (!strcmp(key, "palette")) {
            pname = string_lit(p);
            if (!pname) return fail(p, "palette= expects a quoted ColorBrewer name", "");
        } else return fail(p, "scale_*_brewer option `%s` not implemented "
                           "(palette=)", key);
        skip_ws(p);
        if (*p->s == ',') { p->s++; skip_ws(p); }
    }
    if (!pname) return fail(p, "scale_*_brewer needs palette=\"...\"", "");
    Col stops[BREWER_MAX_STOPS]; int nst, qual;
    if (brewer_lookup(pname, stops, &nst, &qual))
        return fail(p, "unknown ColorBrewer palette `%s` (e.g. Set1, Set2, "
                    "Dark2, Paired)", pname);
    if (!qual)
        return fail(p, "`%s` is a continuous ramp; use "
                    "scale_*_distiller(palette=) instead", pname);
    for (int i = 0; i < nst; i++) {
        spec->manual_cols[i] = stops[i];
        spec->manual_names[i] = NULL;    /* positional, in factor level order */
    }
    spec->n_manual = nst;
    spec->has_manual = 1;
    spec->brewer_disc = pname;
    return expect(p, ')');
}

static int parse_manual_scale(P *p, PlotSpec *spec, const char *fn) {
    spec->n_manual = 0; spec->has_manual = 1;
    skip_ws(p);
    while (*p->s != ')') {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "%s expects values=c(...)", fn);
        if (strcmp(key, "values"))
            return fail(p, "scale_*_manual option `%s` not implemented (only values=)", key);
        skip_ws(p);
        if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
        else return fail(p, "values= expects c(\"#..\", ...)", "");
        for (;;) {
            skip_ws(p);
            if (*p->s == ')') { p->s++; break; }
            char *nm = NULL;
            const char *save = p->s;
            if (*p->s == '"') {                       /* "name" = "colour" ? */
                char *s = string_lit(p); skip_ws(p);
                if (*p->s == '=') { p->s++; nm = s; } else { free(s); p->s = save; }
            } else {                                   /* name = "colour" ? */
                char *id = ident(p); skip_ws(p);
                if (id && *p->s == '=') { p->s++; nm = id; } else { free(id); p->s = save; }
            }
            char *cv = string_lit(p); Col c;
            if (!cv || parse_color(cv, &c)) { free(nm); return fail(p, "bad colour in values=c(...)", ""); }
            free(cv);
            if (spec->n_manual >= 64) {
                free(nm);
                /* dropping the excess silently painted level 17 grey (or,
                 * before the positional-shortfall check, in another level's
                 * colour) with no warning -- the wrong-figure class. */
                return fail(p, "values=c(...) holds at most 64 colours", "");
            }
            spec->manual_cols[spec->n_manual] = c;
            spec->manual_names[spec->n_manual] = nm;
            spec->n_manual++;
            skip_ws(p);
            if (*p->s == ',') { p->s++; continue; }
            if (*p->s == ')') { p->s++; break; }
            return fail(p, "expected , or ) in values=c(...)", "");
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; skip_ws(p); }
    }
    p->s++;                                            /* consume ')' */
    return 0;
}

static int parse_term(P *p, PlotSpec *spec) {
    char *name = ident(p);
    if (!name) return fail(p, "expected a function call near \"%.20s\"", p->s);
    if (expect(p, '(')) return -1;

    if (!strcmp(name, "aes")) return parse_aes(p, spec);
    if (!strcmp(name, "labs")) return parse_labs(p, spec);
    if (!strcmp(name, "xlab") || !strcmp(name, "ylab") || !strcmp(name, "ggtitle")) {
        char *val = string_lit(p);
        if (!val) return fail(p, "%s() expects a quoted string", name);
        if (name[0] == 'x') spec->lab_x = val;
        else if (name[0] == 'y') spec->lab_y = val;
        else spec->lab_title = val;
        return expect(p, ')');
    }

    GeomType gt = GEOM_POINT;
    int is_geom = 1, is_repel = 0;
    if (!strcmp(name, "geom_point")) gt = GEOM_POINT;
    else if (!strcmp(name, "geom_jitter")) gt = GEOM_JITTER;
    else if (!strcmp(name, "geom_smooth")) gt = GEOM_SMOOTH;
    else if (!strcmp(name, "geom_line")) gt = GEOM_LINE;
    else if (!strcmp(name, "geom_col")) gt = GEOM_COL;
    else if (!strcmp(name, "geom_histogram")) gt = GEOM_HISTOGRAM;
    else if (!strcmp(name, "geom_boxplot")) gt = GEOM_BOXPLOT;
    else if (!strcmp(name, "geom_bar")) gt = GEOM_BAR;
    else if (!strcmp(name, "geom_tile")) gt = GEOM_TILE;
    /* geom_raster() is ggplot2's equal-sized-cell special case of geom_tile;
     * the distinction is a rendering optimisation there, so accept it as a
     * synonym rather than making the caller care. */
    else if (!strcmp(name, "geom_raster")) gt = GEOM_TILE;
    else if (!strcmp(name, "geom_segment")) gt = GEOM_SEGMENT;
    else if (!strcmp(name, "geom_rect")) gt = GEOM_RECT;
    else if (!strcmp(name, "geom_density")) gt = GEOM_DENSITY;
    else if (!strcmp(name, "geom_hline")) gt = GEOM_HLINE;
    else if (!strcmp(name, "geom_vline")) gt = GEOM_VLINE;
    else if (!strcmp(name, "geom_abline")) gt = GEOM_ABLINE;
    else if (!strcmp(name, "geom_text")) gt = GEOM_TEXT;
    else if (!strcmp(name, "geom_label")) gt = GEOM_LABEL;
    else if (!strcmp(name, "geom_text_repel")) { gt = GEOM_TEXT; is_repel = 1; }
    else if (!strcmp(name, "geom_label_repel")) { gt = GEOM_LABEL; is_repel = 1; }
    else is_geom = 0;
    if (is_geom) {
        if (spec->nlayers == MAX_LAYERS) return fail(p, "too many layers", "");
        Layer *l = &spec->layers[spec->nlayers++];
        l->type = gt; l->bins = 30; l->adjust = 1;   /* adjust default = 1 (density) */
        l->slope = 1;                                /* geom_abline default slope */
        l->repel = is_repel;
        skip_ws(p);
        if (gt == GEOM_HISTOGRAM && *p->s != ')') {
            char *key = ident(p);
            if (!key || strcmp(key, "bins") || expect(p, '='))
                return fail(p, "geom_histogram() supports only bins=N", "");
            skip_ws(p);
            l->bins = (int)strtol(p->s, (char **)&p->s, 10);
            if (l->bins < 1 || l->bins > 10000)
                return fail(p, "geom_histogram(bins=...) must be 1..10000", "");
            skip_ws(p);
        } else {
            /* generic layer args: color=/colour=/fill= sets a constant colour
             * for any geom (overriding the colour aesthetic, as ggplot does when
             * the aesthetic is set outside aes()); data=/y= give a second data
             * source and are only meaningful for segment/rect. */
            int se = (gt == GEOM_SEGMENT || gt == GEOM_RECT);
            while (*p->s != ')') {
                char *key = ident(p);
                if (!key || expect(p, '='))
                    return fail(p, "geom args: color=/fill= (segment/rect also data=, y=)", "");
                skip_ws(p);
                if (!strcmp(key, "color") || !strcmp(key, "colour") || !strcmp(key, "fill")) {
                    char *v = string_lit(p);
                    if (!v || parse_color(v, &l->color))
                        return fail(p, "bad colour for `%s`", key);
                    l->has_color = 1;
                } else if (se && !strcmp(key, "data")) {
                    l->data = string_lit(p);
                    if (!l->data) return fail(p, "data= expects a quoted path", "");
                } else if (se && !strcmp(key, "y")) {
                    l->ycol = ident(p);
                    if (!l->ycol) return fail(p, "y= expects a column name", "");
                } else if (gt == GEOM_SMOOTH && !strcmp(key, "span")) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || v <= 0 || v > 1)
                        return fail(p, "span= expects a fraction in (0, 1]", "");
                    p->s = end; l->span = v;
                } else if (gt == GEOM_SMOOTH && !strcmp(key, "se")) {
                    char *v = string_lit(p);
                    if (!v) v = ident(p);
                    if (!v) return fail(p, "se= expects TRUE or FALSE", "");
                    /* ggplot defaults se=TRUE. The ribbon is not implemented, and
                     * quietly drawing the line without it would be a figure that
                     * claims less uncertainty than the caller asked to see. */
                    if (strcmp(v, "FALSE") && strcmp(v, "false") && strcmp(v, "F"))
                        return fail(p, "geom_smooth(se=TRUE) is not implemented -- "
                                    "the confidence ribbon is missing, not hidden; "
                                    "pass se=FALSE for the fitted line alone", "");
                    l->se_given = 1;
                } else if (gt == GEOM_DENSITY && !strcmp(key, "bw")) {
                    l->bw = strtod(p->s, (char **)&p->s);
                    if (l->bw <= 0) return fail(p, "geom_density(bw=...) must be > 0", "");
                } else if (gt == GEOM_DENSITY && !strcmp(key, "adjust")) {
                    l->adjust = strtod(p->s, (char **)&p->s);
                    if (l->adjust <= 0) return fail(p, "geom_density(adjust=...) must be > 0", "");
                } else if (gt == GEOM_HLINE && !strcmp(key, "yintercept")) {
                    l->intercept = strtod(p->s, (char **)&p->s); l->has_intercept = 1;
                } else if (gt == GEOM_VLINE && !strcmp(key, "xintercept")) {
                    l->intercept = strtod(p->s, (char **)&p->s); l->has_intercept = 1;
                } else if (gt == GEOM_ABLINE && !strcmp(key, "slope")) {
                    l->slope = strtod(p->s, (char **)&p->s); l->has_slope = 1;
                } else if (gt == GEOM_ABLINE && !strcmp(key, "intercept")) {
                    l->intercept = strtod(p->s, (char **)&p->s); l->has_intercept = 1;
                } else if ((gt == GEOM_TEXT || gt == GEOM_LABEL) && !strcmp(key, "size")) {
                    l->txt_size = strtod(p->s, (char **)&p->s);
                    if (l->txt_size <= 0) return fail(p, "geom_text(size=...) must be > 0", "");
                } else if (gt == GEOM_BOXPLOT
                           && (!strcmp(key, "outlier.shape")
                               || !strcmp(key, "outliers"))) {
                    /* ggplot spells it outlier.shape=NA; the only value that
                     * changes anything here is "no outliers", so accept NA and
                     * the booleans and reject a shape we cannot draw. */
                    char *v = string_lit(p);
                    if (!v) v = ident(p);
                    if (!v) return fail(p, "outlier.shape= expects NA, TRUE or FALSE", "");
                    if (!strcmp(v, "NA") || !strcmp(v, "FALSE") || !strcmp(v, "false")
                        || !strcmp(v, "none") || !strcmp(v, "off"))
                        l->no_outliers = 1;
                    else if (!strcmp(v, "TRUE") || !strcmp(v, "true")
                             || !strcmp(v, "on"))
                        l->no_outliers = 0;
                    else return fail(p, "outlier.shape=%s not understood; "
                                     "cinderplot draws one outlier shape, so only "
                                     "NA/FALSE (hide) and TRUE (show) apply", v);
                } else if (gt == GEOM_JITTER
                           && (!strcmp(key, "width") || !strcmp(key, "height"))) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || v < 0)
                        return fail(p, "%s= expects a non-negative number", key);
                    p->s = end;
                    if (key[0] == 'w') l->jitter_w = v; else l->jitter_h = v;
                } else if (gt == GEOM_JITTER && !strcmp(key, "seed")) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s) return fail(p, "seed= expects a number", "");
                    p->s = end;
                    l->jitter_seed = (unsigned)v; l->has_jitter_seed = 1;
                } else if ((gt == GEOM_POINT || gt == GEOM_JITTER)
                           && !strcmp(key, "size")) {
                    l->point_size = strtod(p->s, (char **)&p->s);
                    if (l->point_size <= 0) return fail(p, "geom_point(size=...) must be > 0", "");
                } else if ((gt == GEOM_POINT || gt == GEOM_JITTER)
                           && (!strcmp(key, "raster")
                                             || !strcmp(key, "rasterise")
                                             || !strcmp(key, "rasterize"))) {
                    /* ggrastr spelling, plus both -ise/-ize, since the point of
                     * the grammar is that ggplot2 habits transfer. */
                    char *v = ident(p);
                    if (!v) return fail(p, "geom_point(raster=) expects TRUE or FALSE", "");
                    if (!strcmp(v, "TRUE") || !strcmp(v, "T")) l->raster = 1;
                    else if (!strcmp(v, "FALSE") || !strcmp(v, "F")) l->raster = 0;
                    else { free(v); return fail(p, "geom_point(raster=) expects TRUE or FALSE", ""); }
                    free(v);
                } else if ((gt == GEOM_TEXT || gt == GEOM_LABEL) && !strcmp(key, "nudge_x")) {
                    l->nudge_x = strtod(p->s, (char **)&p->s);
                } else if ((gt == GEOM_TEXT || gt == GEOM_LABEL) && !strcmp(key, "nudge_y")) {
                    l->nudge_y = strtod(p->s, (char **)&p->s);
                } else if (!strcmp(key, "alpha")) {
                    /* ggplot2's alpha: the first thing anyone reaches for on a
                     * large scatter, where an opaque overplot hides where the
                     * mass actually is. Applies to the layer's marks/lines/bars. */
                    l->alpha = strtod(p->s, (char **)&p->s);
                    if (!(l->alpha > 0 && l->alpha <= 1))
                        return fail(p, "alpha= must be in (0, 1]", "");
                } else if (!strcmp(key, "linetype")) {
                    /* "dashed"/"dotted" carry a convention -- a dashed rule reads
                     * as an annotation rather than as fitted data, which matters
                     * for a y=x reference on a Q-Q plot. */
                    char *v = *p->s == '"' ? string_lit(p) : ident(p);
                    if (!v) return fail(p, "linetype= expects \"solid\", \"dashed\" or \"dotted\"", "");
                    if (!strcmp(v, "solid")) l->dash = 0;
                    else if (!strcmp(v, "dashed")) l->dash = 1;
                    else if (!strcmp(v, "dotted")) l->dash = 2;
                    else { free(v); return fail(p, "linetype= supports \"solid\", \"dashed\", \"dotted\"", ""); }
                    free(v);
                } else return fail(p, "layer option `%s` not implemented", key);
                skip_ws(p);
                if (*p->s == ',') { p->s++; skip_ws(p); }
            }
        }
        if (*p->s != ')')
            return fail(p, "`%s()` arguments are not implemented yet", name);
        p->s++;
        return 0;
    }
    if (!strcmp(name, "scale_x_log10") || !strcmp(name, "scale_y_log10") ||
        !strcmp(name, "scale_x_log2")  || !strcmp(name, "scale_y_log2")) {
        int isx = (name[6] == 'x');
        int base = name[strlen(name) - 1] == '2' ? 2 : 10;
        if (isx) spec->log_x = base; else spec->log_y = base;
        skip_ws(p);
        while (*p->s != ')') {                        /* optional limits=c(lo, hi) */
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad scale_*_log argument", "");
            skip_ws(p);
            if (strcmp(key, "limits")) return fail(p, "scale_*_log option `%s` not implemented (only limits=)", key);
            if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
            else return fail(p, "limits= expects c(lo, hi)", "");
            double lo = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ',') p->s++;
            double hi = strtod(p->s, (char **)&p->s);
            skip_ws(p); if (*p->s == ')') p->s++;
            if (isx) { spec->xlim_lo = lo; spec->xlim_hi = hi; spec->has_xlim = 1; }
            else     { spec->ylim_lo = lo; spec->ylim_hi = hi; spec->has_ylim = 1; }
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "scale_x_continuous") || !strcmp(name, "scale_y_continuous")) {
        int isx = (name[6] == 'x');
        skip_ws(p);
        while (*p->s != ')') {                        /* labels=percent, limits=c(lo,hi) */
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad scale_*_continuous argument", "");
            skip_ws(p);
            if (!strcmp(key, "labels")) {
                skip_ws(p);
                if (p->s[0] == 'c' && p->s[1] == '(') {
                    /* labels=c("B01", ...): explicit tick text, paired 1:1
                     * with breaks= — so a category label row can BE the axis
                     * instead of duplicating a meaningless numeric one. The
                     * pairing is checked once the whole scale is parsed,
                     * since the two keys may come in either order. */
                    p->s += 2;
                    char **la = isx ? spec->x_break_labs : spec->y_break_labs;
                    int *nla = isx ? &spec->n_x_break_labs : &spec->n_y_break_labs;
                    *nla = 0;
                    for (;;) {
                        skip_ws(p);
                        if (*p->s == ')') { p->s++; break; }
                        char *v = string_lit(p);
                        if (!v) return fail(p, "labels=c(...) expects quoted strings", "");
                        if (*nla >= MAX_BREAKS)
                            return fail(p, "labels=c(...) holds at most 256 values", "");
                        la[(*nla)++] = v;
                        skip_ws(p);
                        if (*p->s == ',') { p->s++; continue; }
                        if (*p->s == ')') { p->s++; break; }
                        return fail(p, "expected , or ) in labels=c(...)", "");
                    }
                    if (*nla == 0) return fail(p, "labels=c() is empty", "");
                } else {
                    char *v = ident(p);
                    if (!v) return fail(p, "labels= expects percent or c(...)", "");
                    if (!strcmp(v, "percent") || !strcmp(v, "scales_percent")) {
                        if (isx) spec->x_pct = 1; else spec->y_pct = 1;
                    } else return fail(p, "labels=%s not implemented; supported: "
                                       "percent, c(\"...\", ...)", v);
                }
            } else if (!strcmp(key, "limits")) {
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "limits= expects c(lo, hi)", "");
                double lo = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ',') p->s++;
                double hi = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ')') p->s++;
                if (isx) { spec->xlim_lo = lo; spec->xlim_hi = hi; spec->has_xlim = 1; }
                else     { spec->ylim_lo = lo; spec->ylim_hi = hi; spec->has_ylim = 1; }
            } else if (!strcmp(key, "breaks")) {
                /* Explicit tick positions. The automatic ones are chosen for a
                 * readable count, not a readable WIDTH -- a genomic coordinate
                 * prints 202004000 and the labels collide, which is otherwise
                 * only fixable by rescaling the data. */
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "breaks= expects c(a, b, ...)", "");
                double *br = isx ? spec->x_breaks : spec->y_breaks;
                int *nbr = isx ? &spec->n_x_breaks : &spec->n_y_breaks;
                *nbr = 0;
                for (;;) {
                    skip_ws(p);
                    if (*p->s == ')') { p->s++; break; }
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s) return fail(p, "bad number in breaks=c(...)", "");
                    p->s = end;
                    if (*nbr >= MAX_BREAKS)
                        return fail(p, "breaks=c(...) holds at most 256 values", "");
                    br[(*nbr)++] = v;
                    skip_ws(p);
                    if (*p->s == ',') { p->s++; continue; }
                    if (*p->s == ')') { p->s++; break; }
                    return fail(p, "expected , or ) in breaks=c(...)", "");
                }
                if (*nbr == 0) return fail(p, "breaks=c() is empty", "");
            } else return fail(p, "scale_*_continuous option `%s` not implemented "
                               "(labels=percent, labels=c(...), limits=, breaks=)", key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        /* labels=c(...) is text FOR the breaks; without a matching breaks=
         * there is nothing to pin each label to. Silently recycling or
         * truncating would mislabel ticks, which reads as wrong data. */
        {
            int nbr = isx ? spec->n_x_breaks : spec->n_y_breaks;
            int nla = isx ? spec->n_x_break_labs : spec->n_y_break_labs;
            if (nla && nla != nbr)
                return fail(p, "labels=c(...) needs a breaks=c(...) of the same "
                            "length in the same scale_*_continuous()", "");
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "xlim") || !strcmp(name, "ylim")) {   /* xlim(lo, hi) / ylim(lo, hi) */
        double lo = strtod(p->s, (char **)&p->s);
        skip_ws(p); if (*p->s == ',') p->s++; skip_ws(p);
        double hi = strtod(p->s, (char **)&p->s);
        if (name[0] == 'x') { spec->xlim_lo = lo; spec->xlim_hi = hi; spec->has_xlim = 1; }
        else                { spec->ylim_lo = lo; spec->ylim_hi = hi; spec->has_ylim = 1; }
        return expect(p, ')');
    }
    if (!strcmp(name, "scale_x_genome")) {
        char *v = string_lit(p);
        if (!v) return fail(p, "scale_x_genome() expects a quoted seqinfo TSV path", "");
        spec->genome_seqinfo = v;
        return expect(p, ')');
    }
    if (!strcmp(name, "ideogram")) {
        char *v = string_lit(p);
        if (!v) return fail(p, "ideogram() expects a quoted cytoband TSV path", "");
        spec->ideogram_path = v;
        return expect(p, ')');
    }

    /* ---- track (locus-browser) mode ---- */
    if (!strcmp(name, "region")) {
        spec->region = raw_token(p);       /* chr:start-end; empty => infer from matrix() */
        return expect(p, ')');
    }
    if (!strcmp(name, "coverage")) { TrackObj *o = trk_new(p, spec, TRK_COVERAGE); return o ? parse_trk_args(p, o) : -1; }
    if (!strcmp(name, "interval")) { TrackObj *o = trk_new(p, spec, TRK_INTERVAL); return o ? parse_trk_args(p, o) : -1; }
    if (!strcmp(name, "genes"))    { TrackObj *o = trk_new(p, spec, TRK_GENES);    return o ? parse_trk_args(p, o) : -1; }
    if (!strcmp(name, "arcs"))     { TrackObj *o = trk_new(p, spec, TRK_ARCS);     return o ? parse_trk_args(p, o) : -1; }
    if (!strcmp(name, "matrix"))   { TrackObj *o = trk_new(p, spec, TRK_MATRIX);   return o ? parse_trk_args(p, o) : -1; }
    if (!strcmp(name, "cytoband")) { TrackObj *o = trk_new(p, spec, TRK_CYTOBAND); return o ? parse_trk_args(p, o) : -1; }

    /* ---- matrix (wheatmap) mode ---- */
    if (!strcmp(name, "heatmap")) {
        HMObj *o = hm_new(p, spec, HM_HEATMAP);
        if (!o) return -1;
        return parse_hm_args(p, o, 0);
    }
    if (!strcmp(name, "annotation")) {
        HMObj *o = hm_new(p, spec, HM_ANNOTATION);
        if (!o) return -1;
        return parse_hm_args(p, o, 1);
    }
    if (!strcmp(name, "legend")) {
        HMObj *o = hm_new(p, spec, HM_LEGEND);
        if (!o) return -1;
        o->place.kind = PL_RIGHT_OF;             /* sensible default */
        return parse_hm_args(p, o, 0);
    }
    if (!strcmp(name, "dendrogram")) {
        HMObj *o = hm_new(p, spec, HM_DENDROGRAM);
        if (!o) return -1;
        o->place.kind = PL_LEFT_OF;              /* default: row tree left */
        return parse_hm_args(p, o, 0);
    }
    if (!strcmp(name, "annotate")) {
        /* annotate("text", x=0.78, y=0.925, label="0.9"[, colour=][, size=])
         * / annotate("segment", x=, y=, xend=, yend=) / annotate("rect",
         * xmin=, xmax=, ymin=, ymax=): one literal mark, ggplot2's verb. */
        if (spec->nannos >= MAX_ANNOTATES)
            return fail(p, "too many annotate() calls (max 16)", "");
        Annotate *a = &spec->annos[spec->nannos];
        skip_ws(p);
        char *k = string_lit(p);
        if (!k || (strcmp(k, "text") && strcmp(k, "segment") && strcmp(k, "rect")))
            return fail(p, "annotate() expects (\"text\"|\"segment\"|\"rect\", ...)", "");
        a->kind = k[0] == 't' ? ANNO_TEXT : k[0] == 's' ? ANNO_SEGMENT : ANNO_RECT;
        skip_ws(p);
        while (*p->s == ',') {
            p->s++;
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad annotate() argument", "");
            skip_ws(p);
            if (!strcmp(key, "label")) {
                a->label = string_lit(p);
                if (!a->label) return fail(p, "label= expects a quoted string", "");
            } else if (!strcmp(key, "colour") || !strcmp(key, "color")) {
                char *v = string_lit(p);
                if (!v || parse_color(v, &a->color))
                    return fail(p, "annotate() colour invalid (use names or #RRGGBB)", "");
                a->has_color = 1;
            } else {
                char *end;
                double v = strtod(p->s, &end);
                if (end == p->s) return fail(p, "annotate() %s= expects a number", key);
                p->s = end;
                if (!strcmp(key, "x") || !strcmp(key, "xmin")) { a->x = v; a->has_x = 1; }
                else if (!strcmp(key, "y") || !strcmp(key, "ymin")) { a->y = v; a->has_y = 1; }
                else if (!strcmp(key, "xend") || !strcmp(key, "xmax")) { a->xend = v; a->has_xend = 1; }
                else if (!strcmp(key, "yend") || !strcmp(key, "ymax")) { a->yend = v; a->has_yend = 1; }
                else if (!strcmp(key, "size")) a->size = v;
                else if (!strcmp(key, "hjust")) {
                    if (v < 0 || v > 1) return fail(p, "hjust= takes 0..1", "");
                    a->hjust = v; a->has_hjust = 1;
                } else if (!strcmp(key, "vjust")) {
                    /* the text grob's vertical anchor is three-valued */
                    if (v != 0 && v != 0.5 && v != 1)
                        return fail(p, "vjust= takes 0, 0.5 or 1", "");
                    a->vjust = v; a->has_vjust = 1;
                }
                else return fail(p, "annotate() option `%s` not implemented; supported: "
                                 "x=, y=, xend=/xmax=, yend=/ymax=, label=, colour=, "
                                 "size=, hjust=, vjust=", key);
            }
            skip_ws(p);
        }
        if (expect(p, ')')) return -1;
        if (a->kind == ANNO_TEXT && (!a->has_x || !a->has_y || !a->label))
            return fail(p, "annotate(\"text\") needs x=, y= and label=", "");
        if (a->kind != ANNO_TEXT
            && (!a->has_x || !a->has_y || !a->has_xend || !a->has_yend))
            return fail(p, a->kind == ANNO_SEGMENT
                        ? "annotate(\"segment\") needs x=, y=, xend= and yend="
                        : "annotate(\"rect\") needs xmin=, xmax=, ymin= and ymax=", "");
        if (a->kind != ANNO_TEXT && a->label)
            return fail(p, "label= belongs on annotate(\"text\")", "");
        if (a->kind != ANNO_TEXT && (a->has_hjust || a->has_vjust))
            return fail(p, "hjust=/vjust= belong on annotate(\"text\")", "");
        spec->nannos++;
        return 0;
    }
    if (!strcmp(name, "highlight")) {
        /* highlight("row","col"[, color="red"][, name="m"]): a bounding box
         * on one heatmap cell, addressed by its row and column names. */
        if (spec->nhls >= MAX_HIGHLIGHTS)
            return fail(p, "too many highlight() calls (max 16)", "");
        CellHighlight *h = &spec->hls[spec->nhls];
        Col red = {1, 0, 0};
        h->color = red;
        skip_ws(p);
        h->row = string_lit(p);
        skip_ws(p);
        if (!h->row || *p->s != ',')
            return fail(p, "highlight() expects (\"row\", \"col\", ...)", "");
        p->s++;
        h->col = string_lit(p);
        if (!h->col)
            return fail(p, "highlight() expects (\"row\", \"col\", ...)", "");
        skip_ws(p);
        while (*p->s == ',') {
            p->s++;
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "bad highlight() argument", "");
            if (!strcmp(key, "color") || !strcmp(key, "colour")) {
                char *v = string_lit(p);
                if (!v || parse_color(v, &h->color))
                    return fail(p, "highlight() colour invalid "
                                "(use names or #RRGGBB)", "");
            } else if (!strcmp(key, "name")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "name= expects a quoted string", "");
                h->target = v;
            } else return fail(p, "option `%s` not implemented on highlight(); "
                               "supported: color=, name=", key);
            skip_ws(p);
        }
        if (expect(p, ')')) return -1;
        spec->nhls++;
        return 0;
    }
    if (!strcmp(name, "scale_fill_manual") || !strcmp(name, "scale_colour_manual")
        || !strcmp(name, "scale_color_manual"))
        return parse_manual_scale(p, spec, name);
    if (!strcmp(name, "scale_fill_distiller") || !strcmp(name, "scale_colour_distiller")
        || !strcmp(name, "scale_color_distiller")) {
        FillScale *fs = name[6] == 'f' ? &spec->fill : &spec->colour_scale;
        if (name[6] == 'f') spec->has_fill = 1; else spec->has_colour_scale = 1;
        return parse_distiller(p, fs);
    }
    if (!strcmp(name, "scale_fill_brewer") || !strcmp(name, "scale_colour_brewer")
        || !strcmp(name, "scale_color_brewer"))
        return parse_brewer_discrete(p, spec);
    if (!strcmp(name, "scale_fill_identity") || !strcmp(name, "scale_colour_identity")
        || !strcmp(name, "scale_color_identity")) {
        /* the mapped column's values are the colours themselves — the
         * stopgap for one figure wanting several colour meanings until real
         * plot composition exists */
        spec->identity_scale = 1;
        return expect(p, ')');
    }
    if (!strncmp(name, "scale_fill_", 11)) {
        spec->has_fill = 1;
        return parse_grad_scale(p, &spec->fill, name + 11, "scale_fill_");
    }
    if (!strncmp(name, "scale_colour_", 13) || !strncmp(name, "scale_color_", 12)) {
        const char *k = name + (name[10] == 'u' ? 13 : 12);   /* colour vs color */
        spec->has_colour_scale = 1;
        return parse_grad_scale(p, &spec->colour_scale, k, "scale_colour_");
    }
    if (!strcmp(name, "coord_flip")) {          /* swap the x and y axes */
        spec->coord_flip = 1;
        return expect(p, ')');
    }
    if (!strcmp(name, "geom_tree") || !strcmp(name, "geom_tiplab")
        || !strcmp(name, "geom_nodelab") || !strcmp(name, "geom_nodepoint")
        || !strcmp(name, "geom_tippoint")) {
        char **jdata = NULL, **jcol = NULL;
        if (!strcmp(name, "geom_tree")) spec->tree_mode = 1;
        else if (!strcmp(name, "geom_tiplab")) {
            spec->tree_tiplab = 1;
            jdata = &spec->tree_tl_data; jcol = &spec->tree_tl_col;
        } else if (!strcmp(name, "geom_nodepoint")) {
            spec->tree_nodepoint = 1;
            jdata = &spec->tree_np_data; jcol = &spec->tree_np_col;
        } else if (!strcmp(name, "geom_tippoint")) {
            spec->tree_tippoint = 1;
            jdata = &spec->tree_tp_data; jcol = &spec->tree_tp_col;
        } else spec->tree_nodelab = 1;
        skip_ws(p);
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "%s() takes key=value arguments", name);
            skip_ws(p);
            if (!strcmp(key, "layout")) {
                char *v = string_lit(p);
                if (!v) v = ident(p);
                if (!v) return fail(p, "layout= expects rectangular, slanted "
                                    "or circular", "");
                if (!strcmp(v, "rectangular")) spec->tree_layout = 0;
                else if (!strcmp(v, "slanted")) spec->tree_layout = 1;
                else if (!strcmp(v, "circular") || !strcmp(v, "fan"))
                    spec->tree_layout = 2;
                else return fail(p, "layout=%s invalid; use rectangular, slanted "
                                 "or circular", v);
            } else if (!strcmp(key, "label")) {
                char *v = string_lit(p);
                if (!v) v = ident(p);
                if (!v || strcmp(v, "id"))
                    return fail(p, "label= expects id (the node number)", "");
                spec->tree_lab_id = 1;
            } else if (!strcmp(key, "data")) {
                if (!jdata) return fail(p, "%s() takes no data=", name);
                if (!(*jdata = string_lit(p)))
                    return fail(p, "data= expects a quoted path", "");
            } else if (!strcmp(key, "colour") || !strcmp(key, "color")) {
                if (!jcol) return fail(p, "%s() takes no colour=", name);
                if (!(*jcol = ident(p)))
                    return fail(p, "colour= expects a column name", "");
            } else return fail(p, "tree geom option `%s` not implemented; "
                               "supported: layout=, data=, colour=, label=", key);
            free(key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "scale_x_discrete") || !strcmp(name, "scale_y_discrete")) {
        int isx = name[6] == 'x';
        skip_ws(p);
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "%s() supports angle=", name);
            if (strcmp(key, "angle"))
                return fail(p, "scale_*_discrete() option `%s` not implemented; "
                            "supported: angle=", key);
            free(key);
            skip_ws(p);
            double v = strtod(p->s, (char **)&p->s);
            if (v < 0 || v > 90)
                return fail(p, "angle= must be between 0 and 90", "");
            if (isx) spec->x_angle = v; else spec->y_angle = v;
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "facet_wrap")) {
        skip_ws(p);
        if (*p->s != '~') return fail(p, "facet_wrap() expects a formula: facet_wrap(~var)", "");
        p->s++;
        spec->facet_var = ident(p);
        if (!spec->facet_var) return fail(p, "expected a column name after ~", "");
        skip_ws(p);
        while (*p->s == ',') {   /* levels=c(...) panel order, scales= free axes */
            p->s++;
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "facet_wrap() supports levels=c(...) and scales=", "");
            if (!strcmp(key, "levels")) {
                if (parse_levels(p, &spec->facet_levels, &spec->n_facet_levels)) return -1;
            } else if (!strcmp(key, "ncol") || !strcmp(key, "nrow")) {
                skip_ws(p);
                char *end;
                double v = strtod(p->s, &end);
                if (end == p->s || v < 1 || v != (int)v)
                    return fail(p, "%s= expects a positive whole number", key);
                p->s = end;
                if (key[1] == 'c') spec->facet_ncol = (int)v;
                else spec->facet_nrow = (int)v;
            } else if (!strcmp(key, "scales")) {
                char *v = string_lit(p);
                if (!v) v = ident(p);          /* scales=free reads as well as "free" */
                if (!v) return fail(p, "scales= expects fixed, free_x, free_y, or free", "");
                if (!strcmp(v, "fixed")) { spec->free_x = 0; spec->free_y = 0; }
                else if (!strcmp(v, "free_x")) { spec->free_x = 1; spec->free_y = 0; }
                else if (!strcmp(v, "free_y")) { spec->free_x = 0; spec->free_y = 1; }
                else if (!strcmp(v, "free"))   { spec->free_x = 1; spec->free_y = 1; }
                else if (!strcmp(v, "free_colour") || !strcmp(v, "free_color")) {
                    /* each facet builds its own colour scale and legend block
                     * — one figure, several colour meanings (no ggplot2
                     * equivalent; patchwork territory) */
                    spec->free_x = 0; spec->free_y = 0; spec->free_colour = 1;
                }
                else return fail(p, "scales=%s invalid; use fixed, free_x, "
                                 "free_y, free, or free_colour", v);
            } else return fail(p, "facet_wrap() option `%s` not implemented; "
                               "supported: levels=c(...), scales=, ncol=, nrow=", key);
            free(key);
            skip_ws(p);
        }
        return expect(p, ')');
    }
    if (!strncmp(name, "theme_", 6)) {           /* no-arg theme selector */
        const char *t = name + 6;
        if      (!strcmp(t, "gray") || !strcmp(t, "grey")) spec->theme = THEME_GRAY;
        else if (!strcmp(t, "bw"))              spec->theme = THEME_BW;
        else if (!strcmp(t, "minimal"))         spec->theme = THEME_MINIMAL;
        else if (!strcmp(t, "classic"))         spec->theme = THEME_CLASSIC;
        else if (!strcmp(t, "void"))            spec->theme = THEME_VOID;
        else if (!strcmp(t, "linedraw"))        spec->theme = THEME_LINEDRAW;
        else if (!strcmp(t, "light"))           spec->theme = THEME_LIGHT;
        else if (!strcmp(t, "dark"))            spec->theme = THEME_DARK;
        else if (!strcmp(t, "few"))             spec->theme = THEME_FEW;
        else return fail(p, "theme `%s()` is not implemented; supported: theme_gray, "
                            "theme_bw, theme_minimal, theme_classic, theme_void, theme_linedraw, "
                            "theme_light, theme_dark, theme_few", name);
        return expect(p, ')');
    }
    if (!strcmp(name, "ggplot")) {
        /* ggplot(data, aes(...)) -- R's own spelling, accepted as sugar for
         * cinderplot's `data + aes(...)`. The grammar is already ggplot2's; the
         * one thing that does NOT transfer from a model's prior is the
         * invocation shape, so accepting the R form means a caller writing from
         * memory produces something that runs, with no documentation in
         * context. data= and mapping= keywords are honoured too. */
        skip_ws(p);
        while (*p->s != ')') {
            skip_ws(p);
            const char *save = p->s;
            char *key = ident(p);
            skip_ws(p);
            if (key && *p->s == '=') p->s++;
            else { p->s = save; free(key); key = NULL; }
            skip_ws(p);
            if (!strncmp(p->s, "aes", 3) || (key && !strcmp(key, "mapping"))) {
                if (!strncmp(p->s, "aes", 3)) p->s += 3;
                if (expect(p, '(')) { free(key); return -1; }
                if (parse_aes(p, spec)) { free(key); return -1; }
            } else {
                char *v = raw_token(p);
                if (!v || !*v) { free(v); free(key);
                    return fail(p, "ggplot() expects a data path", ""); }
                free(spec->data_path);
                spec->data_path = v;
            }
            free(key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; continue; }
            break;
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "ggsave")) {
        /* Accepted and ignored: the output path and size are CLI arguments
         * here, but a spec written from ggplot2 memory tends to end with a
         * ggsave(), and erroring on it would reject an otherwise correct
         * figure over a detail cinderplot has already been told. */
        int depth = 1;
        while (*p->s && depth) {
            if (*p->s == '(') depth++;
            else if (*p->s == ')') depth--;
            else if (*p->s == '"') { char *q = string_lit(p); free(q); continue; }
            if (depth) p->s++;
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "regions")) {
        /* regions("windows.bed") -- several genomic windows on ONE axis, with a
         * physical gap between them. Not a montage and not a grid: the tracks
         * share the axis, so every track maps its own data into the same
         * segmented space and the boundaries line up down the whole stack. */
        char *v = raw_token(p);
        if (!v || !*v) { free(v); return fail(p, "regions() expects a BED path", ""); }
        spec->regions_path = v;
        return expect(p, ')');
    }
    if (!strcmp(name, "guides")) {
        /* guides(colour="none", fill="none") -- ggplot2's spelling for dropping
         * a legend. Only "none" is meaningful here: cinderplot has no guide
         * customization to select, so any other value is an error rather than a
         * silently ignored argument. */
        skip_ws(p);
        if (*p->s == ')') return fail(p, "guides() needs an argument, e.g. "
                                         "guides(colour=\"none\")", "");
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad guides() argument", "");
            int ok = !strcmp(key, "colour") || !strcmp(key, "color")
                  || !strcmp(key, "fill") || !strcmp(key, "size");
            if (!ok) { fail(p, "guides() supports colour=, color=, fill=, size=", ""); free(key); return -1; }
            free(key);
            skip_ws(p);
            char *v = *p->s == '"' ? string_lit(p) : ident(p);  /* "none" / guide_legend */
            if (!v) return fail(p, "guides() values must be \"none\" or guide_legend(...)", "");
            if (!strcmp(v, "guide_legend")) {
                /* guide_legend(ncol=N) / (nrow=N): fold a long discrete
                 * legend over columns, column-major as in ggplot2. nrow=
                 * caps the rows instead — under scales="free_colour" each
                 * block then derives its own column count, which is what a
                 * 4/6/10-level trio wants. */
                free(v);
                skip_ws(p);
                if (*p->s != '(') return fail(p, "guide_legend expects (ncol=N) or (nrow=N)", "");
                p->s++;
                skip_ws(p);
                while (*p->s != ')') {
                    char *gk = ident(p);
                    if (!gk || expect(p, '=')) return fail(p, "bad guide_legend() argument", "");
                    char *end;
                    double n2 = strtod(p->s, &end);
                    if (end == p->s || n2 < 1 || n2 != (int)n2)
                        return fail(p, "guide_legend %s= expects a positive integer", gk);
                    p->s = end;
                    if (!strcmp(gk, "ncol")) spec->legend_ncol = (int)n2;
                    else if (!strcmp(gk, "nrow")) spec->legend_nrow = (int)n2;
                    else return fail(p, "guide_legend option `%s` not implemented "
                                     "(ncol=, nrow=)", gk);
                    skip_ws(p);
                    if (*p->s == ',') { p->s++; skip_ws(p); }
                }
                p->s++;
                if (spec->legend_ncol && spec->legend_nrow)
                    return fail(p, "guide_legend: give ncol= or nrow=, not both", "");
            } else if (!strcmp(v, "none")) {
                free(v);
                spec->no_legend = 1;
            } else {
                free(v);
                return fail(p, "guides() supports \"none\" or guide_legend(ncol=/nrow=)", "");
            }
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    return fail(p, "`%s()` is not implemented; supported: aes(), geom_point(), "
                   "geom_jitter(), geom_line(), geom_col(), geom_histogram(), geom_boxplot(), geom_bar(), "
                   "geom_density(), geom_tile()/geom_raster(), geom_segment(), geom_rect(), "
                   "geom_hline(), geom_vline(), geom_abline(), "
                   "geom_text()/geom_text_repel(), geom_label()/geom_label_repel(), "
                   "labs()/xlab()/ylab()/ggtitle(), "
                   "facet_wrap(~var[, levels=c(...)]), coord_flip(), scale_x_log10()/scale_x_log2(), scale_y_log10()/scale_y_log2(), scale_*_continuous(), xlim(), ylim(), "
                   "scale_*_manual(), theme_bw()/theme_minimal()/theme_classic()/..., guides(colour=\"none\"), "
                   "heatmap(), annotation(), legend(), highlight(), scale_fill_*(), "
                   "region()/regions(), coverage(), interval(), genes(), arcs(), matrix(), cytoband()", name);
}

int dsl_parse(const char *src, PlotSpec *spec, char *err) {
    P p = {src, err};
    memset(spec, 0, sizeof *spec);
    /* <0 means "decide from the measured labels"; 0 is a caller asking for
     * horizontal, which is a different thing and must survive. */
    spec->x_angle = spec->y_angle = -1;
    spec->fill.kind = FILL_VIRIDIS;              /* default heatmap fill */
    /* default theme = THEME_GRAY (enum 0, the memset default) */

    /* leading data term, unless the first term is a function call (track
     * mode starts with region()/coverage() — no top-level data file) */
    skip_ws(&p);
    const char *s = p.s;
    while (*p.s && *p.s != '+' && !isspace((unsigned char)*p.s)) {
        if (*p.s == '(') break;
        p.s++;
    }
    if (*p.s == '(') {                 /* function-first: no data file */
        p.s = s;
        if (parse_term(&p, spec)) return -1;
    } else {
        spec->data_path = strndup(s, p.s - s);
        if (!*spec->data_path) return fail(&p, "missing data file at start of spec", "");
    }

    for (;;) {
        skip_ws(&p);
        if (!*p.s) break;
        if (expect(&p, '+')) return -1;
        if (parse_term(&p, spec)) return -1;
    }

    /* aes(fill=) is stored in spec->colour (fill and colour are one aesthetic
     * here), but scale_fill_*() writes spec->fill, which only heatmap mode
     * reads. In grammar mode that made every scale_fill_gradient/viridis/jet a
     * silent no-op: the spec parsed, the run succeeded, and the default ramp
     * came out. Alias it onto the colour scale instead. scale_colour_*() still
     * wins if both are given. */
    if (spec->nhobjs == 0 && spec->ntracks == 0
        && spec->has_fill && !spec->has_colour_scale) {
        spec->colour_scale = spec->fill;
        spec->has_colour_scale = 1;
    }
    /* labs(fill=) titles that same aesthetic, so honour it when labs(colour=)
     * was not given. */
    if (spec->nhobjs == 0 && spec->ntracks == 0
        && spec->lab_fill && !spec->lab_colour)
        spec->lab_colour = spec->lab_fill;

    if (spec->nhls > 0 && spec->nhobjs == 0)
        return fail(&p, "highlight() marks a heatmap() cell and needs heatmap mode", "");
    if (spec->nannos > 0 && spec->nlayers == 0 && !spec->x.col)
        return fail(&p, "annotate() places a mark on a grammar panel and needs "
                    "aes()/geom_*", "");

    if (spec->ntracks > 0) {           /* track (locus-browser) mode */
        if (spec->nlayers || spec->nhobjs || spec->x.col)
            return fail(&p, "track functions cannot be mixed with grammar/heatmap", "");
        return 0;
    }

    if (spec->nhobjs > 0 && (spec->nlayers || spec->x.col || spec->facet_var)) {
        /* Grammar mode + annotation(): a categorical metadata band under the
         * panel, keyed by x category, with its own palette and legend. Only
         * annotation() crosses this line — heatmap()/legend()/dendrogram()
         * stay heatmap-mode verbs. */
        for (int i = 0; i < spec->nhobjs; i++) {
            const HMObj *o = &spec->hobjs[i];
            if (o->type != HM_ANNOTATION)
                /* legend() is a heatmap-mode primitive, but the supported-verbs
                 * list advertises it, so someone reaching for it to control a
                 * grammar legend lands here. Say so, and point at what they
                 * actually want. */
                return fail(&p, o->type == HM_LEGEND
                            ? "legend() places a legend beside a heatmap() and cannot be "
                              "used with aes()/geom_*; in grammar mode the legend is "
                              "automatic — suppress it with guides(colour=\"none\")"
                            : "heatmap() cannot be mixed with aes()/geom_*/facet_wrap()", "");
            if (o->place.kind != PL_FULL)
                return fail(&p, "annotation() under a grammar panel always draws "
                            "beneath it; placements (left_of/right_of/...) are "
                            "heatmap-mode", "");
            if (!o->data)
                return fail(&p, "annotation() needs a data file: "
                            "annotation(\"meta.tsv\"[, column=\"...\"])", "");
        }
        /* falls through to the grammar-mode checks below */
    } else if (spec->nhobjs > 0) {               /* matrix mode */
        if (spec->has_manual) {
            /* heatmap.c maps cell values through a continuous FillScale and never
             * consults manual_cols, so this used to parse, run, exit 0 and render
             * the default ramp -- the caller only found out by noticing the output
             * never changed. Every other unsupported thing here errors with a menu,
             * which is what makes a wrong guess cheap; silent acceptance breaks that
             * contract, so refuse until a discrete heatmap fill exists. */
            return fail(&p, "scale_*_manual() is not supported in heatmap mode: the "
                            "heatmap fill is a continuous scale. Use "
                            "scale_fill_gradient()/gradient2()/viridis()/jet(), or "
                            "encode the categories as an annotation(), which does "
                            "take a discrete palette and legend", "");
        }
        if (spec->hobjs[0].type != HM_HEATMAP)
            return fail(&p, "the first placed object must be a heatmap()", "");
        return 0;
    }
    if (spec->tree_mode) {          /* tree mode: the topology is the data */
        if (spec->nlayers || spec->x.col || spec->nhobjs || spec->ntracks)
            return fail(&p, "geom_tree() cannot be mixed with aes()/geom_*, "
                        "heatmap() or the track verbs", "");
        return 0;
    }
    if (spec->tree_tiplab || spec->tree_nodelab
        || spec->tree_nodepoint || spec->tree_tippoint)
        return fail(&p, "the tree geoms need a geom_tree()", "");
    for (int i = 0; i < spec->nlayers; i++)
        if (spec->layers[i].type == GEOM_SMOOTH && !spec->layers[i].se_given)
            return fail(&p, "geom_smooth() defaults to se=TRUE in ggplot2 and the "
                        "confidence ribbon is not implemented here; say se=FALSE "
                        "to ask for the fitted line alone", "");
    if (spec->nlayers == 0)
        return fail(&p, "no geom given; add e.g. + geom_point()", "");
    int nstat = 0, nref = 0;   /* stats compute y; reference lines are overlays */
    for (int i = 0; i < spec->nlayers; i++) {
        GeomType t = spec->layers[i].type;
        if (t == GEOM_HISTOGRAM || t == GEOM_BAR || t == GEOM_DENSITY) nstat++;
        else if (t == GEOM_HLINE || t == GEOM_VLINE || t == GEOM_ABLINE) nref++;
    }
    if (nstat && spec->nlayers - nstat - nref > 0)
        return fail(&p, "geom_histogram()/geom_bar()/geom_density() cannot be combined with other data geoms yet", "");
    if (!spec->x.col)
        return fail(&p, "aes() must map x", "");
    if (nstat && spec->y.col)
        return fail(&p, "geom_histogram()/geom_bar()/geom_density() compute y; do not map y", "");
    if (!nstat && !spec->y.col)
        return fail(&p, "aes() must map y", "");
    for (int i = 0; i < spec->nlayers; i++)
        if ((spec->layers[i].type == GEOM_TEXT || spec->layers[i].type == GEOM_LABEL)
            && !spec->label.col)
            return fail(&p, "geom_text()/geom_label() need aes(label=...)", "");
    return 0;
}
