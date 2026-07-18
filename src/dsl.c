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
    sprintf(p->err, fmt, a);
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
    char *out = malloc(cap);
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

/* value in aes: IDENT or factor(IDENT); fills entry incl. source text */
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
        if (expect(p, ')')) return -1;
        e->is_factor = 1;
        free(id);
    } else {
        e->col = id;
        e->is_factor = 0;
    }
    e->expr = strndup(start, p->s - start);
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
            if (!strcmp(key, "x")) e = &spec->x;
            else if (!strcmp(key, "y")) e = &spec->y;
            else if (!strcmp(key, "xend")) e = &spec->xend;
            else if (!strcmp(key, "yend")) e = &spec->yend;
            else if (!strcmp(key, "chrom") || !strcmp(key, "chr")) e = &spec->chrom;
            else if (!strcmp(key, "colour") || !strcmp(key, "color")
                  || !strcmp(key, "fill")) e = &spec->colour;
            else return fail(p, "aes(%s=...) is not implemented; supported: x, y, xend, yend, chrom, colour, fill", key);
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
        char *val = string_lit(p);
        if (!val) return fail(p, "labs(%s=...) expects a quoted string", key);
        if (!strcmp(key, "title")) spec->lab_title = val;
        else if (!strcmp(key, "x")) spec->lab_x = val;
        else if (!strcmp(key, "y")) spec->lab_y = val;
        else if (!strcmp(key, "colour") || !strcmp(key, "color")) spec->lab_colour = val;
        else if (!strcmp(key, "fill")) spec->lab_fill = val;
        else return fail(p, "labs(%s=...) is not implemented; supported: title, x, y, colour, fill", key);
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

static HMObj *hm_new(P *p, PlotSpec *spec, HMType t) {
    if (spec->nhobjs == MAX_HMOBJS) { fail(p, "too many heatmap objects", ""); return NULL; }
    HMObj *o = &spec->hobjs[spec->nhobjs++];
    memset(o, 0, sizeof *o);
    o->type = t;
    o->place.kind = spec->nhobjs == 1 ? PL_FULL : PL_TOP_OF;
    o->place.pad = 0.01; o->place.width = -1; o->place.height = -1;
    o->name = malloc(8);
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
            } else if (!strcmp(key, "title")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "title= expects a quoted string", "");
                o->title = v;
            } else if (!strcmp(key, "cluster")) {
                char *v = ident(p);
                if (!v) return fail(p, "cluster= expects rows, cols, both, or none", "");
                if (!strcmp(v, "rows")) o->cluster = CL_ROWS;
                else if (!strcmp(v, "cols") || !strcmp(v, "columns")) o->cluster = CL_COLS;
                else if (!strcmp(v, "both")) o->cluster = CL_BOTH;
                else if (!strcmp(v, "none")) o->cluster = CL_NONE;
                else return fail(p, "cluster=%s invalid; use rows, cols, both, or none", v);
            } else if (!strcmp(key, "rownames") || !strcmp(key, "colnames")) {
                int row = key[0] == 'r';
                char *v = ident(p);
                if (!v) return fail(p, "%s= expects left/right (rownames) or top/bottom (colnames), or none", key);
                Side s;
                if (!strcmp(v, "none")) s = SIDE_NONE;
                else if (row && !strcmp(v, "left")) s = SIDE_LEFT;
                else if (row && !strcmp(v, "right")) s = SIDE_RIGHT;
                else if (!row && !strcmp(v, "top")) s = SIDE_TOP;
                else if (!row && !strcmp(v, "bottom")) s = SIDE_BOTTOM;
                else return fail(p, row ? "rownames= must be left, right, or none"
                                        : "colnames= must be top, bottom, or none", "");
                if (row) o->rownames = s; else o->colnames = s;
            } else return fail(p, "option `%s` not implemented; supported: name=, data=, "
                                  "cluster=, rownames=, colnames=, placements", key);
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
    else if (!strcmp(k, "bwr")) fs->kind = FILL_BWR;
    else if (!strcmp(k, "gradient")) {
        fs->kind = FILL_GRADIENT;
        parse_color("#132B43", &fs->low); parse_color("#56B1F7", &fs->high);
    } else if (!strcmp(k, "gradient2")) {
        fs->kind = FILL_GRADIENT2;
        parse_color("#832424", &fs->low); parse_color("white", &fs->mid);
        parse_color("#3A3A98", &fs->high); fs->midpoint = 0;
    } else {
        char msg[128];
        snprintf(msg, sizeof msg, "`%s%s()` not implemented; supported: "
                 "viridis, jet, bwr, gradient, gradient2", fn, k);
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

static int parse_term(P *p, PlotSpec *spec) {
    char *name = ident(p);
    if (!name) return fail(p, "expected a function call near \"%.20s\"", p->s);
    if (expect(p, '(')) return -1;

    if (!strcmp(name, "aes")) return parse_aes(p, spec);
    if (!strcmp(name, "labs")) return parse_labs(p, spec);

    GeomType gt = GEOM_POINT;
    int is_geom = 1;
    if (!strcmp(name, "geom_point")) gt = GEOM_POINT;
    else if (!strcmp(name, "geom_line")) gt = GEOM_LINE;
    else if (!strcmp(name, "geom_col")) gt = GEOM_COL;
    else if (!strcmp(name, "geom_histogram")) gt = GEOM_HISTOGRAM;
    else if (!strcmp(name, "geom_boxplot")) gt = GEOM_BOXPLOT;
    else if (!strcmp(name, "geom_bar")) gt = GEOM_BAR;
    else if (!strcmp(name, "geom_segment")) gt = GEOM_SEGMENT;
    else is_geom = 0;
    if (is_geom) {
        if (spec->nlayers == MAX_LAYERS) return fail(p, "too many layers", "");
        Layer *l = &spec->layers[spec->nlayers++];
        l->type = gt; l->bins = 30;
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
        }
        if (*p->s != ')')
            return fail(p, "`%s()` arguments are not implemented yet", name);
        p->s++;
        return 0;
    }
    if (!strcmp(name, "scale_x_log10") || !strcmp(name, "scale_y_log10")) {
        if (name[6] == 'x') spec->log_x = 1; else spec->log_y = 1;
        return expect(p, ')');
    }
    if (!strcmp(name, "scale_x_genome")) {
        char *v = string_lit(p);
        if (!v) return fail(p, "scale_x_genome() expects a quoted seqinfo TSV path", "");
        spec->genome_seqinfo = v;
        return expect(p, ')');
    }

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
    if (!strncmp(name, "scale_fill_", 11))
        return parse_grad_scale(p, &spec->fill, name + 11, "scale_fill_");
    if (!strncmp(name, "scale_colour_", 13) || !strncmp(name, "scale_color_", 12)) {
        const char *k = name + (name[10] == 'u' ? 13 : 12);   /* colour vs color */
        spec->has_colour_scale = 1;
        return parse_grad_scale(p, &spec->colour_scale, k, "scale_colour_");
    }
    if (!strcmp(name, "facet_wrap")) {
        skip_ws(p);
        if (*p->s != '~') return fail(p, "facet_wrap() expects a formula: facet_wrap(~var)", "");
        p->s++;
        spec->facet_var = ident(p);
        if (!spec->facet_var) return fail(p, "expected a column name after ~", "");
        return expect(p, ')');
    }
    return fail(p, "`%s()` is not implemented; supported: aes(), geom_point(), "
                   "geom_line(), geom_col(), geom_histogram(), geom_boxplot(), geom_bar(), labs(), "
                   "facet_wrap(~var), scale_x_log10(), scale_y_log10(), "
                   "heatmap(), annotation(), legend(), scale_fill_*()", name);
}

int dsl_parse(const char *src, PlotSpec *spec, char *err) {
    P p = {src, err};
    memset(spec, 0, sizeof *spec);
    spec->fill.kind = FILL_VIRIDIS;              /* default heatmap fill */

    /* data term: everything up to the first '+' that has no '(' */
    skip_ws(&p);
    const char *s = p.s;
    while (*p.s && *p.s != '+' && !isspace((unsigned char)*p.s)) {
        if (*p.s == '(') break;
        p.s++;
    }
    if (*p.s == '(')
        return fail(&p, "the first term must be the data file (e.g. data.csv + aes(...))", "");
    spec->data_path = strndup(s, p.s - s);
    if (!*spec->data_path) return fail(&p, "missing data file at start of spec", "");

    for (;;) {
        skip_ws(&p);
        if (!*p.s) break;
        if (expect(&p, '+')) return -1;
        if (parse_term(&p, spec)) return -1;
    }

    if (spec->nhobjs > 0) {                      /* matrix mode */
        if (spec->nlayers || spec->x.col || spec->facet_var)
            return fail(&p, "heatmap() cannot be mixed with aes()/geom_*/facet_wrap()", "");
        if (spec->hobjs[0].type != HM_HEATMAP)
            return fail(&p, "the first placed object must be a heatmap()", "");
        return 0;
    }
    if (spec->nlayers == 0)
        return fail(&p, "no geom given; add e.g. + geom_point()", "");
    int nstat = 0;   /* count stats (histogram, bar) compute y themselves */
    for (int i = 0; i < spec->nlayers; i++)
        if (spec->layers[i].type == GEOM_HISTOGRAM || spec->layers[i].type == GEOM_BAR) nstat++;
    if (nstat && nstat != spec->nlayers)
        return fail(&p, "geom_histogram()/geom_bar() cannot be combined with other geoms yet", "");
    if (!spec->x.col)
        return fail(&p, "aes() must map x", "");
    if (nstat && spec->y.col)
        return fail(&p, "geom_histogram()/geom_bar() compute y (count); do not map y", "");
    if (!nstat && !spec->y.col)
        return fail(&p, "aes() must map y", "");
    return 0;
}
