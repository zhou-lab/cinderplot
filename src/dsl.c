/* dsl.c — parser for the verbatim-ggplot2 DSL subset.
 *
 *   expr  := data ('+' term)*
 *   data  := path            (first term; anything not followed by '(')
 *   term  := NAME '(' args ')'
 *
 * Supported: aes() with positional x,y and named x/y/colour/color,
 * values IDENT or factor(IDENT); geom_point(); labs(title/x/y/colour=
 * "string"); facet_wrap(~var); facet_grid(row ~ col). Anything else
 * errors with the supported subset listed, so unimplemented ggplot is a
 * clear "not yet" rather than a syntax error. */
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

/* identifier: letters, digits, '_', '.' (R-style column names). Bytes >= 0x80
 * are accepted too, so a UTF-8 header ("β-value" minus the hyphen, "μ") can be
 * named bare; the parser only ever looks for ASCII punctuation, so multi-byte
 * sequences pass through intact. */
static char *ident(P *p) {
    skip_ws(p);
    const char *s = p->s;
    while (isalnum((unsigned char)*p->s) || *p->s == '_' || *p->s == '.'
           || (unsigned char)*p->s >= 0x80) p->s++;
    if (p->s == s) return NULL;
    return strndup(s, p->s - s);
}

/* column name: an identifier, or R's `backtick quoted` form for names with
 * spaces, hyphens or anything else the bare form cannot carry. */
static char *colname(P *p) {
    skip_ws(p);
    if (*p->s != '`') return ident(p);
    const char *s = ++p->s;
    while (*p->s && *p->s != '`') p->s++;
    if (*p->s != '`' || p->s == s) {
        snprintf(p->err, CP_ERRLEN, "unterminated or empty `backtick` column name near \"%.20s\"", s);
        return NULL;
    }
    char *out = strndup(s, p->s - s);
    p->s++;
    return out;
}

/* TRUE/true/T/1 and FALSE/false/F/0, the same everywhere a boolean is read;
 * a quoted spelling is accepted too. Returns -1 when the token is not one. */
static int parse_bool(P *p, int *out) {
    skip_ws(p);
    const char *save = p->s;
    char *v = NULL;
    if (*p->s == '"' || *p->s == '\'') {
        p->s++;
        const char *s = p->s;
        while (*p->s && *p->s != save[0]) p->s++;
        if (*p->s != save[0]) { p->s = save; return -1; }
        v = strndup(s, p->s - s);
        p->s++;
    } else v = ident(p);
    if (!v) { p->s = save; return -1; }
    int r = 0;
    if (!strcmp(v, "TRUE") || !strcmp(v, "true") || !strcmp(v, "T") || !strcmp(v, "1")) *out = 1;
    else if (!strcmp(v, "FALSE") || !strcmp(v, "false") || !strcmp(v, "F") || !strcmp(v, "0")) *out = 0;
    else { r = -1; p->s = save; }
    free(v);
    return r;
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

/* "double" or 'single' quoted, R accepting both; the closer matches the
 * opener, so an apostrophe inside "..." is ordinary text. */
static char *string_lit(P *p) {
    skip_ws(p);
    if (*p->s != '"' && *p->s != '\'') return NULL;
    char q = *p->s++;
    size_t cap = strlen(p->s) + 1, n = 0;
    char *out = cp_xmalloc(cap);
    while (*p->s && *p->s != q) {
        if (*p->s == '\\' && p->s[1]) {
            p->s++;
            if (*p->s == 'n') out[n++] = '\n';
            else if (*p->s == 't') out[n++] = '\t';
            else out[n++] = *p->s;
            p->s++;
        } else out[n++] = *p->s++;
    }
    if (*p->s != q) { free(out); return NULL; }
    p->s++;
    out[n] = 0;
    return out;
}

static int is_quote(P *p) { skip_ws(p); return *p->s == '"' || *p->s == '\''; }

/* option value that may be bare or quoted: cluster=both and cluster="both"
 * both read, as scales=free / scales="free" already did. */
static char *word(P *p) { return is_quote(p) ? string_lit(p) : ident(p); }

/* raw value token: filename or bare word (until , ) or whitespace) */
static char *raw_token(P *p);
static int parse_lim_pair(P *p, const char *fn, double *lo, double *hi);

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
/* The R idioms a ggplot2 habit reaches for inside aes() that have no
 * equivalent here. Each gets a message naming the cinderplot spelling, since
 * the bare "expected ')'" they used to produce reads as a syntax slip. */
static int aes_r_idiom(P *p, const char *id) {
    skip_ws(p);
    if (*p->s == '(') {
        if (!strcmp(id, "as.factor") || !strcmp(id, "as.character") || !strcmp(id, "as.ordered"))
            return fail(p, "aes(): %s() is not implemented; use factor(col) (add levels=c(...) for an order)", id);
        if (!strcmp(id, "reorder") || !strcmp(id, "fct_reorder") || !strcmp(id, "fct_relevel")
            || !strcmp(id, "fct_rev") || !strcmp(id, "fct_infreq") || !strcmp(id, "forcats"))
            return fail(p, "aes(): %s() is not implemented; order the levels explicitly "
                        "with factor(col, levels=c(\"a\", \"b\", ...))", id);
        if (!strcmp(id, "log10") || !strcmp(id, "log2") || !strcmp(id, "log") || !strcmp(id, "log1p")
            || !strcmp(id, "sqrt") || !strcmp(id, "exp") || !strcmp(id, "abs") || !strcmp(id, "scale"))
            return fail(p, "aes(): %s() inside aes() is not implemented; use "
                        "scale_x_log10()/scale_y_log10() for a log axis, or add the "
                        "transformed column to the data (e.g. with tabl mutate)", id);
        if (!strcmp(id, "as.numeric") || !strcmp(id, "as.integer") || !strcmp(id, "as.double"))
            return fail(p, "aes(): %s() is not implemented; a numeric column is read as "
                        "numeric already -- check the column for non-numeric cells", id);
        if (!strcmp(id, "paste") || !strcmp(id, "paste0") || !strcmp(id, "interaction")
            || !strcmp(id, "sprintf") || !strcmp(id, "round") || !strcmp(id, "format")
            || !strcmp(id, "ifelse") || !strcmp(id, "cut"))
            return fail(p, "aes(): %s() is not implemented; add the derived column "
                        "to the data (e.g. with tabl mutate) and map it", id);
        return fail(p, "aes(): `%s()` is not a mapping; only a column name or "
                    "factor(col) is accepted here", id);
    }
    if (strchr("+-*/^%", *p->s) && p->s[1] != '=')
        return fail(p, "aes(): arithmetic (%.12s...) inside aes() is not implemented; "
                    "add the computed column to the data (e.g. with tabl mutate) and map it",
                    p->s);
    return 0;
}

static int aes_value(P *p, AesEntry *e) {
    const char *start;
    skip_ws(p);
    start = p->s;
    if (is_quote(p))
        return fail(p, "aes() maps a column name, not a string, near \"%.20s\"; "
                    "for a `name with spaces` use backticks", p->s);
    char *id = colname(p);
    if (!id) return *p->err ? -1 : fail(p, "expected a column name near \"%.20s\"", p->s);
    if (strcmp(id, "factor") && aes_r_idiom(p, id)) return -1;
    if (!strcmp(id, "factor")) {
        if (expect(p, '(')) return -1;
        e->col = colname(p);
        if (!e->col) return *p->err ? -1 : fail(p, "expected a column name in factor() near \"%.20s\"", p->s);
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
    /* Several ggplot2 keys share one slot here (xmin= is x, ymax= is yend,
     * fill= is colour). A second mapping onto an occupied slot used to win
     * silently -- aes(x=hp, xmin=wt) plotted wt -- so remember which key
     * claimed each slot and refuse the collision, naming both. */
    const char *claimed[11] = {0};
    skip_ws(p);
    if (*p->s == ')') { p->s++; return 0; }
    for (;;) {
        skip_ws(p);
        /* named arg? lookahead for IDENT '=' (but not '==') */
        const char *save = p->s;
        char *key = ident(p);
        AesEntry *e = NULL;
        int slot = -1;
        skip_ws(p);
        if (key && *p->s == '=') {
            p->s++;
            if (!strcmp(key, "x") || !strcmp(key, "xmin")) { e = &spec->x; slot = 0; }
            else if (!strcmp(key, "y")) { e = &spec->y; slot = 1; }
            else if (!strcmp(key, "xend") || !strcmp(key, "xmax")) { e = &spec->xend; slot = 2; }
            else if (!strcmp(key, "yend") || !strcmp(key, "ymax")) { e = &spec->yend; slot = 3; }
            else if (!strcmp(key, "ymin")) { e = &spec->ymin; slot = 4; }
            else if (!strcmp(key, "chrom") || !strcmp(key, "chr")) { e = &spec->chrom; slot = 5; }
            else if (!strcmp(key, "label")) { e = &spec->label; slot = 6; }
            else if (!strcmp(key, "size")) { e = &spec->size; slot = 7; }
            else if (!strcmp(key, "shape")) { e = &spec->shape; slot = 8; }
            else if (!strcmp(key, "colour") || !strcmp(key, "color")
                  || !strcmp(key, "fill")) {
                e = &spec->colour; slot = 9;
                spec->colour.is_fill = key[0] == 'f';
            }
            else if (!strcmp(key, "group")) { e = &spec->group; slot = 10; }
            else if (!strcmp(key, "linetype") || !strcmp(key, "alpha")
                     || !strcmp(key, "weight") || !strcmp(key, "linewidth"))
                return fail(p, "aes(%s=...) is not implemented as a mapping; a "
                            "discrete colour= groups lines and sets the legend, "
                            "group= groups without a legend, and linetype=/alpha=/"
                            "linewidth= are per-layer constants (geom_line(linewidth=0.3))", key);
            else return fail(p, "aes(%s=...) is not implemented; supported: x, y, xend, yend, ymin, ymax, label, size, shape, group, chrom, colour, fill", key);
        } else {
            /* positional: the first unclaimed of x, y -- R's matching, so
             * aes(x=factor(g), v) puts v on y */
            p->s = save;
            free(key); key = NULL;
            if (!claimed[0]) { e = &spec->x; slot = 0; }
            else if (!claimed[1]) { e = &spec->y; slot = 1; }
            else return fail(p, "too many positional aes() arguments near \"%.20s\"", p->s);
        }
        const char *k2 = key ? key : (slot == 0 ? "x" : "y");
        if (claimed[slot]) {
            char msg[CP_ERRLEN];
            if (!strcmp(claimed[slot], k2))
                snprintf(msg, sizeof msg, "aes(%s=) is given twice", k2);
            else if (slot == 9)
                snprintf(msg, sizeof msg, "aes(): %s= and %s= both map the colour "
                         "aesthetic (fill and colour are one aesthetic here; on "
                         "geom_boxplot the key chooses body vs chrome); give one",
                         claimed[slot], k2);
            else
                snprintf(msg, sizeof msg, "aes(): %s= and %s= both map the %s slot "
                         "(they are aliases here); give one", claimed[slot], k2,
                         slot == 0 ? "x" : slot == 2 ? "xend/xmax" : "yend/ymax");
            return fail(p, "%s", msg);
        }
        claimed[slot] = k2;                  /* key is kept alive by this */
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
    if (is_quote(p)) return string_lit(p);
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
    pl->given = 1;
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

/* Per-track-type option sets, mirroring what render_tracks.c reads: the
 * generic parser accepted coverage(cluster=samples) and dropped it. */
static const char *trk_name(TrackType t) {
    static const char *nm[] = { "coverage", "interval", "genes", "arcs", "matrix", "cytoband" };
    return nm[t];
}
static int trk_opt_ok(TrackType t, const char *key) {
    if (!strcmp(key, "name") || !strcmp(key, "height") || !strcmp(key, "data")) return 1;
    if (!strcmp(key, "max")) return t == TRK_COVERAGE;
    if (!strcmp(key, "color") || !strcmp(key, "colour"))
        return t == TRK_COVERAGE || t == TRK_INTERVAL || t == TRK_GENES || t == TRK_ARCS;
    if (!strcmp(key, "cluster") || !strcmp(key, "rownames") || !strcmp(key, "colnames")
        || !strcmp(key, "x") || !strcmp(key, "bar") || !strcmp(key, "background")
        || !strcmp(key, "rowgroup") || !strcmp(key, "rowcolour") || !strcmp(key, "rowcolor"))
        return t == TRK_MATRIX;
    if (!strcmp(key, "transcripts")) return t == TRK_GENES;
    if (!strcmp(key, "labels")) return t == TRK_INTERVAL;
    return 0;
}
static const char *trk_opt_menu(TrackType t) {
    switch (t) {
    case TRK_COVERAGE: return "name=, height=, data=, color=, max=";
    case TRK_INTERVAL: return "name=, height=, data=, color=, labels=";
    case TRK_ARCS: return "name=, height=, data=, color=";
    case TRK_GENES: return "name=, height=, data=, color=, transcripts=";
    case TRK_MATRIX: return "name=, height=, data=, cluster=, rownames=, colnames=, "
                            "x=, bar=, background=, rowgroup=, rowcolour=";
    default: return "name=, height=, data=";
    }
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
            if (!trk_opt_ok(o->type, key)) {
                char msg[CP_ERRLEN];
                int known = !strcmp(key, "max") || !strcmp(key, "color") || !strcmp(key, "colour")
                         || !strcmp(key, "cluster") || !strcmp(key, "rownames")
                         || !strcmp(key, "colnames") || !strcmp(key, "transcripts")
                         || !strcmp(key, "labels") || !strcmp(key, "rowgroup")
                         || !strcmp(key, "rowcolour") || !strcmp(key, "rowcolor");
                snprintf(msg, sizeof msg, "option `%s` is %s %s(); supported: %s", key,
                         known ? "not valid for" : "not implemented on",
                         trk_name(o->type), trk_opt_menu(o->type));
                return fail(p, "%s", msg);
            }
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
                char *v = word(p);
                if (!v) return fail(p, "cluster= expects samples or none", "");
                if (!strcmp(v, "samples") || !strcmp(v, "rows")) o->cluster = 1;
                else if (!strcmp(v, "none") || !strcmp(v, "off")) o->cluster = 0;
                else return fail(p, "cluster=%s invalid; use samples or none", v);
            } else if (!strcmp(key, "rownames")) {
                /* left/right read as on: the matrix track draws its sample
                 * labels on one side only, and heatmap() spells sides */
                char *v = word(p);
                if (!v) return fail(p, "rownames= expects on or off", "");
                if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "hide")
                    || !strcmp(v, "FALSE") || !strcmp(v, "false")) o->hide_rownames = 1;
                else if (!strcmp(v, "on") || !strcmp(v, "show") || !strcmp(v, "left")
                         || !strcmp(v, "right") || !strcmp(v, "TRUE") || !strcmp(v, "true"))
                    o->hide_rownames = 0;
                else return fail(p, "rownames=%s invalid; use on or off", v);
            } else if (!strcmp(key, "colnames")) {
                /* Symmetric with rownames=. For a CpG matrix the per-probe
                 * labels are almost always noise -- 127 of them collapse into
                 * an unreadable band about a third of the figure high -- so
                 * turning them off has to be reachable. */
                char *v = word(p);
                if (!v) return fail(p, "colnames= expects on or off", "");
                if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "hide")
                    || !strcmp(v, "FALSE") || !strcmp(v, "false")) o->hide_colnames = 1;
                else if (!strcmp(v, "on") || !strcmp(v, "show") || !strcmp(v, "top")
                         || !strcmp(v, "bottom") || !strcmp(v, "TRUE") || !strcmp(v, "true"))
                    o->hide_colnames = 0;
                else return fail(p, "colnames=%s invalid; use on or off", v);
            } else if (!strcmp(key, "x")) {
                /* x=genomic puts each cell at its own coordinate; x=index is
                 * the probe-index grid, and stays the default because it is
                 * what makes every column readable when there are few. */
                char *v = word(p);
                if (!v) return fail(p, "x= expects genomic or index", "");
                if (!strcmp(v, "genomic") || !strcmp(v, "coord") || !strcmp(v, "bp"))
                    o->genomic_x = 1;
                else if (!strcmp(v, "index") || !strcmp(v, "column") || !strcmp(v, "even"))
                    o->genomic_x = 0;
                else return fail(p, "x=%s invalid; use genomic or index", v);
            } else if (!strcmp(key, "bar")) {
                skip_ws(p);
                const char *save = p->s;
                double w = strtod(p->s, (char **)&p->s);
                if (p->s == save || !(w > 0))
                    return fail(p, "bar= expects a width in bp > 0", "");
                o->bar_bp = w;
            } else if (!strcmp(key, "rowgroup")) {
                o->rowgroup = string_lit(p);
                if (!o->rowgroup || !*o->rowgroup)
                    return fail(p, "rowgroup= expects the quoted separator that "
                                "splits a sample name, e.g. rowgroup=\" | \"", "");
            } else if (!strcmp(key, "rowcolour") || !strcmp(key, "rowcolor")) {
                o->rowcolour = string_lit(p);
                if (!o->rowcolour || !*o->rowcolour)
                    return fail(p, "rowcolour= expects the quoted path of a `group colour` "
                                "table, e.g. rowcolour=\"colours.tsv\"", "");
            } else if (!strcmp(key, "labels")) {
                /* The same spellings rownames= takes, so a reader who knows one
                 * knows the other. */
                char *v = word(p);
                if (!v) return fail(p, "labels= expects on or off", "");
                if (!strcmp(v, "off") || !strcmp(v, "none") || !strcmp(v, "hide")
                    || !strcmp(v, "FALSE") || !strcmp(v, "false")) o->labels = -1;
                else if (!strcmp(v, "on") || !strcmp(v, "show") || !strcmp(v, "all")
                         || !strcmp(v, "TRUE") || !strcmp(v, "true")) o->labels = 1;
                else if (!strcmp(v, "auto")) o->labels = 0;
                else return fail(p, "labels=%s invalid; use on or off", v);
            } else if (!strcmp(key, "background")) {
                char *v = string_lit(p);
                if (!v || parse_color(v, &o->bg_color))
                    return fail(p, "background= expects a colour (names or #RRGGBB)", "");
                o->has_bg = 1;
            } else if (!strcmp(key, "transcripts")) {
                char *v = word(p);
                if (!v) return fail(p, "transcripts= expects all or canonical", "");
                if (!strcmp(v, "all")) o->all_transcripts = 1;
                else if (!strcmp(v, "canonical") || !strcmp(v, "longest")) o->all_transcripts = 0;
                else return fail(p, "transcripts=%s invalid; use all or canonical", v);
            } else {
                char msg[CP_ERRLEN];
                snprintf(msg, sizeof msg, "track option `%s` not implemented; supported "
                         "on %s(): %s", key, trk_name(o->type), trk_opt_menu(o->type));
                return fail(p, "%s", msg);
            }
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
    if (o->rowcolour && !o->rowgroup)
        return fail(p, "rowcolour= colours the group names that rowgroup= splits off, "
                    "so it needs rowgroup=\"SEP\" too; without groups there is "
                    "nothing to colour", "");
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

/* Which options each placed object honours (heatmap.c reads them by type;
 * the rest parsed and vanished, so legend(cluster=both) was accepted). */
static const char *hm_obj_name(HMType t) {
    return t == HM_HEATMAP ? "heatmap" : t == HM_ANNOTATION ? "annotation"
         : t == HM_LEGEND ? "legend" : "dendrogram";
}
static int hm_opt_ok(HMType t, const char *key) {
    if (!strcmp(key, "name")) return 1;
    if (!strcmp(key, "title")) return t != HM_DENDROGRAM;
    if (!strcmp(key, "data")) return t == HM_HEATMAP || t == HM_ANNOTATION;
    if (!strcmp(key, "column")) return t == HM_ANNOTATION;
    if (!strcmp(key, "cluster") || !strcmp(key, "rownames") || !strcmp(key, "colnames")
        || !strcmp(key, "aspect") || !strcmp(key, "discrete")) return t == HM_HEATMAP;
    if (!strcmp(key, "labels") || !strcmp(key, "box") || !strcmp(key, "grid"))
        return t == HM_HEATMAP || t == HM_ANNOTATION;
    return 0;
}
static const char *hm_opt_menu(HMType t) {
    switch (t) {
    case HM_HEATMAP: return "name=, data=, title=, cluster=, rownames=, colnames=, "
                            "labels=, discrete=, aspect=, box=, grid=, placements";
    case HM_ANNOTATION: return "name=, data=, title=, column=, labels=, box=, grid=, placements";
    case HM_LEGEND: return "name=, title=, placements (e.g. right_of(\"m\"))";
    default: return "name=, placements (e.g. left_of(\"m\"))";
    }
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
            if (!hm_opt_ok(o->type, key)) {
                char msg[CP_ERRLEN];
                const char *where = hm_opt_ok(HM_HEATMAP, key) && hm_opt_ok(HM_ANNOTATION, key)
                                  ? "heatmap() and annotation()"
                                  : hm_opt_ok(HM_HEATMAP, key) ? "heatmap()"
                                  : hm_opt_ok(HM_ANNOTATION, key) ? "annotation()"
                                  : hm_opt_ok(HM_LEGEND, key) ? "heatmap(), annotation() and legend()"
                                  : NULL;
                if (where)
                    snprintf(msg, sizeof msg, "option `%s` is not valid for %s(); %s= applies "
                             "to %s. %s() takes: %s", key, hm_obj_name(o->type), key, where,
                             hm_obj_name(o->type), hm_opt_menu(o->type));
                else
                    snprintf(msg, sizeof msg, "option `%s` not implemented; supported for %s(): %s",
                             key, hm_obj_name(o->type), hm_opt_menu(o->type));
                return fail(p, "%s", msg);
            }
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
                char *v = word(p);           /* both and "both" alike */
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
                char *v = word(p);
                if (!v) return fail(p, "%s= expects left/right (rownames) or top/bottom (colnames), or none", key);
                Side s;
                /* `off`/`hide`/`on` because matrix() tracks spell the same idea
                 * that way; the two modes should not disagree on how to say
                 * it. `on` takes the side the default layout puts labels on. */
                if (!strcmp(v, "none") || !strcmp(v, "off") || !strcmp(v, "hide")
                    || !strcmp(v, "FALSE") || !strcmp(v, "false"))
                    s = SIDE_NONE;
                else if (!strcmp(v, "on") || !strcmp(v, "show") || !strcmp(v, "TRUE")
                         || !strcmp(v, "true"))
                    s = row ? SIDE_RIGHT : SIDE_BOTTOM;
                else if (row && !strcmp(v, "left")) s = SIDE_LEFT;
                else if (row && !strcmp(v, "right")) s = SIDE_RIGHT;
                else if (!row && !strcmp(v, "top")) s = SIDE_TOP;
                else if (!row && !strcmp(v, "bottom")) s = SIDE_BOTTOM;
                else return fail(p, row ? "rownames= must be left, right, on, or none"
                                        : "colnames= must be top, bottom, on, or none", "");
                if (row) o->rownames = s; else o->colnames = s;
            } else if (!strcmp(key, "labels")) {
                char *v = word(p);
                if (!v) return fail(p, "labels= expects data/on or none/off", "");
                if (!strcmp(v, "data") || !strcmp(v, "on") || !strcmp(v, "true")
                    || !strcmp(v, "TRUE") || !strcmp(v, "T") || !strcmp(v, "1")) o->label_data = 1;
                else if (!strcmp(v, "none") || !strcmp(v, "off") || !strcmp(v, "false")
                         || !strcmp(v, "FALSE") || !strcmp(v, "F") || !strcmp(v, "0")) o->label_data = 0;
                else return fail(p, "labels=%s invalid; use data/on or none/off", v);
            } else if (!strcmp(key, "discrete")) {
                /* The fill kind. Text cells already say "categorical" on their
                 * own, so this exists for the matrix of numeric CODES -- a 0/1/2
                 * call matrix -- which is indistinguishable from measurements
                 * without being told. discrete=FALSE is the way back: it pins
                 * the continuous reading, so text cells stay the error they
                 * have always been rather than quietly changing meaning. */
                char *v = word(p);
                if (!v) return fail(p, "discrete= expects TRUE or FALSE", "");
                if (!strcmp(v, "TRUE") || !strcmp(v, "true") || !strcmp(v, "T")
                    || !strcmp(v, "on") || !strcmp(v, "1")) o->discrete = 1;
                else if (!strcmp(v, "FALSE") || !strcmp(v, "false") || !strcmp(v, "F")
                         || !strcmp(v, "off") || !strcmp(v, "0")) o->discrete = -1;
                else return fail(p, "discrete=%s invalid; use TRUE or FALSE", v);
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
                    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "TRUE")
                        || !strcmp(v, "T") || !strcmp(v, "1")) {
                        o->box = 1;
                        Col grey = {0.4, 0.4, 0.4};   /* as matrix() tracks frame theirs */
                        o->box_col = grey;
                    }
                    else if (!strcmp(v, "none") || !strcmp(v, "off")
                             || !strcmp(v, "false") || !strcmp(v, "FALSE")
                             || !strcmp(v, "F") || !strcmp(v, "0")) o->box = 0;
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
                    if (!strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "TRUE")
                        || !strcmp(v, "T") || !strcmp(v, "1")) {
                        o->grid = 1;
                        Col grey = {0.702, 0.702, 0.702};   /* grey70: dim on white, legible on fills */
                        o->grid_col = grey;
                    }
                    else if (!strcmp(v, "none") || !strcmp(v, "off")
                             || !strcmp(v, "false") || !strcmp(v, "FALSE")
                             || !strcmp(v, "F") || !strcmp(v, "0")) o->grid = 0;
                    else return fail(p, "grid=%s invalid; use on/off or a quoted colour", v);
                }
            } else {
                char msg[CP_ERRLEN];
                snprintf(msg, sizeof msg, "option `%s` not implemented; supported for %s(): %s",
                         key, hm_obj_name(o->type), hm_opt_menu(o->type));
                return fail(p, "%s", msg);
            }
        } else {
            p->s = save;
            char *v = raw_token(p);
            if (v && *v && !want_data && o->type == HM_HEATMAP)
                /* the heatmap's table is the leading data term (or stdin);
                 * a bare path here was reported as a mystery argument */
                return fail(p, "heatmap() takes no positional file; put it first "
                            "(`%s + heatmap()`) or give data=\"...\"", v);
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
    int is_grad = fs->kind == FILL_GRADIENT || fs->kind == FILL_GRADIENT2;
    skip_ws(p);
    while (*p->s != ')') {
        char *key = ident(p);
        if (!key || expect(p, '=')) return fail(p, "bad scale argument", "");
        skip_ws(p);
        /* low=/mid=/high=/midpoint= are gradient() stops; a fixed palette
         * (viridis, jet, ...) ignored them and painted its own ramp */
        if (!is_grad && (!strcmp(key, "low") || !strcmp(key, "mid")
                         || !strcmp(key, "high") || !strcmp(key, "midpoint"))) {
            char msg[CP_ERRLEN];
            snprintf(msg, sizeof msg, "%s= is not valid for %s%s(): the palette is "
                     "fixed; use %sgradient(low=, high=) or %sgradient2(low=, mid=, "
                     "high=, midpoint=)", key, fn, k, fn, fn);
            return fail(p, "%s", msg);
        }
        if (!strcmp(key, "midpoint")) {
            if (fs->kind != FILL_GRADIENT2)
                return fail(p, "midpoint= belongs to %sgradient2()", fn);
            char *end;
            fs->midpoint = strtod(p->s, &end);
            if (end == p->s) return fail(p, "midpoint= expects a number", "");
            p->s = end;
        } else if (!strcmp(key, "limits")) {         /* c(lo, hi) — domain + squish */
            if (parse_lim_pair(p, "limits=", &fs->lim_lo, &fs->lim_hi)) return -1;
            fs->has_limits = 1;
        } else {
            char *v = string_lit(p); Col c;
            if (!v || parse_color(v, &c))
                return fail(p, "bad colour for `%s` (use names or #RRGGBB)", key);
            if (!strcmp(key, "low")) fs->low = c;
            else if (!strcmp(key, "mid")) {
                if (fs->kind != FILL_GRADIENT2)
                    return fail(p, "mid= belongs to %sgradient2()", fn);
                fs->mid = c;
            }
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
    spec->brewer_disc = NULL;    /* a later manual palette replaces a brewer one */
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

/* xlim(lo, hi) / ylim(lo, hi) / limits=c(lo, hi): two numbers, lo < hi.
 * Used to read whatever strtod made of the text, so xlim(300, 50), xlim(5)
 * and xlim() all reached the renderer and died there with a message about
 * expand=. R's c(...) wrapper is accepted on xlim() too, since that is the
 * form ggplot2 users type; NA (the one-sided idiom) is refused by name. */
static int parse_lim_pair(P *p, const char *fn, double *lo, double *hi) {
    skip_ws(p);
    int wrapped = 0;
    if (p->s[0] == 'c' && p->s[1] == '(') { p->s += 2; wrapped = 1; }
    double v[2]; int na[2] = {0, 0}, n = 0;
    for (; n < 2; n++) {
        skip_ws(p);
        if (!strncmp(p->s, "NA_real_", 8)) { p->s += 8; na[n] = 1; v[n] = 0; }
        else if (!strncmp(p->s, "NA", 2) && !isalnum((unsigned char)p->s[2]) && p->s[2] != '_') {
            p->s += 2; na[n] = 1; v[n] = 0;
        } else {
            char *end;
            v[n] = strtod(p->s, &end);
            if (end == p->s) break;
            p->s = end;
        }
        skip_ws(p);
        if (*p->s == ',') { p->s++; continue; }
        n++; break;
    }
    if (n != 2) return fail(p, "%s expects two numbers, lo and hi", fn);
    if (wrapped && expect(p, ')')) return -1;
    if (na[0] || na[1])
        return fail(p, "%s one-sided limits (NA) are not implemented; give both ends", fn);
    if (!(v[0] < v[1])) {
        char got[96];
        snprintf(got, sizeof got, "%s: lo must be < hi (got %g, %g)", fn, v[0], v[1]);
        return fail(p, v[0] == v[1] ? "%s; equal ends leave no range"
                    : "%s; a reversed range is not implemented", got);
    }
    *lo = v[0]; *hi = v[1];
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
        skip_ws(p);
        if (*p->s == ',' && name[0] == 'g') {          /* ggtitle("T", subtitle="S") */
            p->s++;
            char *key = ident(p);
            if (!key || strcmp(key, "subtitle") || expect(p, '='))
                return fail(p, "ggtitle() takes (\"title\"[, subtitle=\"...\"])", "");
            free(key);
            if (!(spec->lab_subtitle = string_lit(p)))
                return fail(p, "ggtitle(subtitle=) expects a quoted string", "");
        }
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
    else if (!strcmp(name, "geom_errorbar")) gt = GEOM_ERRORBAR;
    else if (!strcmp(name, "geom_linerange")) gt = GEOM_LINERANGE;
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
        {
            /* generic layer args: color=/colour=/fill= sets a constant colour
             * for any geom (overriding the colour aesthetic, as ggplot does when
             * the aesthetic is set outside aes()); data=/y= give a second data
             * source and are only meaningful for segment/rect. */
            int se = (gt == GEOM_SEGMENT || gt == GEOM_RECT);
            while (*p->s != ')') {
                char *key = ident(p);
                skip_ws(p);
                if (key && !strcmp(key, "aes") && *p->s == '(') {
                    /* ggplot2's per-layer mapping. There is one aes() here,
                     * so say where the mapping goes instead of the generic
                     * argument menu, which reads as if aes() were a typo. */
                    if (gt == GEOM_HLINE || gt == GEOM_VLINE)
                        return fail(p, "%s(aes(...)) -- one line per data row -- is "
                                    "not implemented; give a literal intercept "
                                    "(yintercept=20 / xintercept=20) and repeat the "
                                    "layer for several", name);
                    return fail(p, "%s(aes(...)) is not implemented: layer-level "
                                "mappings go in the one top-level aes(), e.g. "
                                "`aes(x, y, colour=g) + geom_point()`", name);
                }
                if (!key || expect(p, '='))
                    return fail(p, "%s() takes key=value arguments: colour=/fill=, alpha=, "
                                "linetype=, and the geom's own options", name);
                skip_ws(p);
                if (gt == GEOM_HISTOGRAM && !strcmp(key, "bins")) {
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || v < 1 || v > 10000 || v != (int)v)
                        return fail(p, "geom_histogram(bins=...) must be a whole number 1..10000", "");
                    p->s = end;
                    l->bins = (int)v;
                } else if (gt == GEOM_HISTOGRAM
                           && (!strcmp(key, "binwidth") || !strcmp(key, "breaks")
                               || !strcmp(key, "boundary") || !strcmp(key, "center")
                               || !strcmp(key, "closed") || !strcmp(key, "position"))) {
                    return fail(p, "geom_histogram(%s=) is not implemented; only bins=N "
                                "chooses the binning", key);
                } else if ((gt == GEOM_LINE || gt == GEOM_SMOOTH || gt == GEOM_SEGMENT
                            || gt == GEOM_HLINE || gt == GEOM_VLINE || gt == GEOM_ABLINE
                            || gt == GEOM_ERRORBAR || gt == GEOM_LINERANGE)
                           && (!strcmp(key, "size") || !strcmp(key, "linewidth"))) {
                    /* the stroke width of a line geom, in ggplot's linewidth
                     * units (0.5 = geom_line's default, 1 = geom_smooth's).
                     * size= is the pre-3.4 spelling and means the same. */
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || !(v > 0)) {
                        char msg[CP_ERRLEN];
                        snprintf(msg, sizeof msg, "%s(%s=) expects a number > 0, in "
                                 "ggplot linewidth units (0.5 is geom_line's default)",
                                 name, key);
                        return fail(p, "%s", msg);
                    }
                    p->s = end;
                    l->line_lw = v;
                } else if (!strcmp(key, "color") || !strcmp(key, "colour") || !strcmp(key, "fill")) {
                    char *v = string_lit(p);
                    if (!v || parse_color(v, &l->color))
                        return fail(p, "bad colour for `%s`", key);
                    l->has_color = 1;
                    l->color_is_fill = key[0] == 'f';
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
                    int b;
                    if (parse_bool(p, &b)) return fail(p, "se= expects TRUE or FALSE", "");
                    /* ggplot defaults se=TRUE. The ribbon is not implemented, and
                     * quietly drawing the line without it would be a figure that
                     * claims less uncertainty than the caller asked to see. */
                    if (b)
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
                } else if ((gt == GEOM_HLINE && !strcmp(key, "yintercept"))
                           || (gt == GEOM_VLINE && !strcmp(key, "xintercept"))) {
                    /* an empty value read as 0 and a c(...) as its first
                     * element -- both drew a line nobody asked for */
                    if (p->s[0] == 'c' && p->s[1] == '(')
                        return fail(p, "%s= takes one value; repeat the layer for "
                                    "several (+ geom_hline(yintercept=10) + geom_hline(yintercept=20))", key);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s) return fail(p, "%s= needs a number", key);
                    p->s = end;
                    l->intercept = v; l->has_intercept = 1;
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
                    char *v = word(p);
                    if (!v) return fail(p, "outlier.shape= expects NA, TRUE or FALSE", "");
                    if (!strcmp(v, "NA") || !strcmp(v, "FALSE") || !strcmp(v, "false")
                        || !strcmp(v, "F") || !strcmp(v, "0")
                        || !strcmp(v, "none") || !strcmp(v, "off"))
                        l->no_outliers = 1;
                    else if (!strcmp(v, "TRUE") || !strcmp(v, "true")
                             || !strcmp(v, "T") || !strcmp(v, "1") || !strcmp(v, "on"))
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
                    if (parse_bool(p, &l->raster))
                        return fail(p, "geom_point(raster=) expects TRUE or FALSE", "");
                } else if (gt == GEOM_ERRORBAR && !strcmp(key, "width")) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || !(v > 0))
                        return fail(p, "geom_errorbar(width=) expects a number > 0", "");
                    p->s = end;
                    l->eb_width = v;
                } else if (gt == GEOM_TILE && !strcmp(key, "linewidth")) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || !(v > 0))
                        return fail(p, "geom_tile(linewidth=) expects a number > 0", "");
                    p->s = end;
                    l->tile_lw = v;
                } else if (gt == GEOM_TEXT && !strcmp(key, "angle")) {
                    if (l->repel)
                        return fail(p, "geom_text_repel(angle=) is not "
                                    "implemented: repel measures unrotated "
                                    "extents", "");
                    l->txt_angle = strtod(p->s, (char **)&p->s);
                } else if (gt == GEOM_LABEL && !strcmp(key, "angle")) {
                    return fail(p, "geom_label(angle=) is not implemented: the "
                                "background box does not rotate; use "
                                "geom_text(angle=)", "");
                } else if (gt == GEOM_TEXT && !strcmp(key, "hjust")) {
                    skip_ws(p);
                    char *end;
                    double v = strtod(p->s, &end);
                    if (end == p->s || v < 0 || v > 1)
                        return fail(p, "hjust= takes 0..1", "");
                    p->s = end;
                    l->txt_hjust = v; l->has_txt_hjust = 1;
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
        if (expect(p, ')')) return -1;
        /* a reference line with no position parsed and drew nothing */
        if (gt == GEOM_HLINE && !l->has_intercept)
            return fail(p, "geom_hline() needs yintercept=", "");
        if (gt == GEOM_VLINE && !l->has_intercept)
            return fail(p, "geom_vline() needs xintercept=", "");
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
            if (!(p->s[0] == 'c' && p->s[1] == '(')) return fail(p, "limits= expects c(lo, hi)", "");
            double lo, hi;
            if (parse_lim_pair(p, "limits=", &lo, &hi)) return -1;
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
                if (!(p->s[0] == 'c' && p->s[1] == '(')) return fail(p, "limits= expects c(lo, hi)", "");
                double lo, hi;
                if (parse_lim_pair(p, "limits=", &lo, &hi)) return -1;
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
            } else if (!strcmp(key, "expand")) {
                skip_ws(p);
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "expand= expects c(mult, add)", "");
                double m2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ',') p->s++;
                double a2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ')') p->s++;
                if (m2 < 0 || a2 < 0)
                    return fail(p, "expand= values must be >= 0", "");
                if (isx) { spec->x_exp_mult = m2; spec->x_exp_add = a2; spec->has_x_expand = 1; }
                else     { spec->y_exp_mult = m2; spec->y_exp_add = a2; spec->has_y_expand = 1; }
            } else return fail(p, "scale_*_continuous option `%s` not implemented "
                               "(labels=percent, labels=c(...), limits=, breaks=, expand=)", key);
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
        double lo, hi;
        if (parse_lim_pair(p, name[0] == 'x' ? "xlim()" : "ylim()", &lo, &hi))
            return -1;
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
                } else if (!strcmp(key, "angle")) {
                    a->angle = v;        /* degrees CCW, ggplot semantics */
                }
                else return fail(p, "annotate() option `%s` not implemented; supported: "
                                 "x=, y=, xend=/xmax=, yend=/ymax=, label=, colour=, "
                                 "size=, hjust=, vjust=, angle=", key);
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
        if (a->kind != ANNO_TEXT && (a->has_hjust || a->has_vjust || a->angle != 0))
            return fail(p, "hjust=/vjust=/angle= belong on annotate(\"text\")", "");
        if (a->angle != 0 && a->has_vjust)
            return fail(p, "vjust= with angle= is not implemented (the rotated "
                        "frame centres vertically); nudge the coordinate instead", "");
        spec->nannos++;
        return 0;
    }
    if (!strcmp(name, "highlight")) {
        /* Three spellings, one verb:
         *   highlight("row","col"[, color=][, name=])      heatmap cell
         *   highlight(name=, row=, region=[, colour=][, linetype=][, label=])
         *                                                  matrix() track box
         *   highlight("boxes.tsv"[, name=])                 file of track boxes
         * The mode check at the end of dsl_parse pairs each form with its
         * mode; here only the shape is settled. */
        if (spec->nhls >= MAX_HIGHLIGHTS)
            return fail(p, "too many highlight() calls (max 64; the file form "
                        "highlight(\"boxes.tsv\") has no cap)", "");
        CellHighlight *h = &spec->hls[spec->nhls];
        memset(h, 0, sizeof *h);
        Col red = {1, 0, 0};
        h->color = red;
        skip_ws(p);
        if (is_quote(p)) {                     /* positional: cell form or file form */
            char *first = string_lit(p);
            skip_ws(p);
            if (*p->s == ',') {
                const char *save = p->s;
                p->s++; skip_ws(p);
                if (is_quote(p)) {             /* ("row","col", ...) */
                    h->row = first;
                    h->col = string_lit(p);
                    skip_ws(p);
                } else p->s = save;            /* ("file", key=...) */
            }
            if (!h->row) h->file = first;
        }
        while (*p->s == ',' || (h->row == NULL && h->file == NULL && *p->s != ')')) {
            if (*p->s == ',') p->s++;
            skip_ws(p);
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "bad highlight() argument; forms: (\"row\",\"col\"), "
                            "(name=, row=, region=) or (\"boxes.tsv\", name=)", "");
            skip_ws(p);
            if (!strcmp(key, "color") || !strcmp(key, "colour")) {
                char *v = string_lit(p);
                if (!v || parse_color(v, &h->color))
                    return fail(p, "highlight() colour invalid "
                                "(use names or #RRGGBB)", "");
            } else if (!strcmp(key, "name")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "name= expects a quoted string", "");
                h->target = v;
            } else if (!strcmp(key, "row")) {
                if (h->row) return fail(p, "highlight(): row given twice", "");
                char *v = string_lit(p);
                if (!v) return fail(p, "row= expects a quoted sample/row name", "");
                h->row = v;
            } else if (!strcmp(key, "region")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "region= expects a quoted \"chr:beg-end\"", "");
                if (region_parse(v, h->chrom, &h->beg, &h->end))
                    return fail(p, "highlight(region=\"%s\"): expected chr:beg-end", v);
                h->region = v;
            } else if (!strcmp(key, "linetype")) {
                char *v = *p->s == '"' ? string_lit(p) : ident(p);
                if (!v) return fail(p, "linetype= expects solid, dashed or dotted", "");
                if (!strcmp(v, "solid")) h->dash = 0;
                else if (!strcmp(v, "dashed")) h->dash = 1;
                else if (!strcmp(v, "dotted")) h->dash = 2;
                else return fail(p, "linetype `%s` not implemented on highlight(); "
                                 "solid, dashed or dotted", v);
            } else if (!strcmp(key, "label")) {
                char *v = string_lit(p);
                if (!v) return fail(p, "label= expects a quoted string", "");
                h->label = v;
            } else return fail(p, "option `%s` not implemented on highlight(); "
                               "supported: colour=, name=, row=, region=, "
                               "linetype=, label=", key);
            skip_ws(p);
        }
        if (expect(p, ')')) return -1;
        if (h->file && (h->row || h->region || h->label))
            return fail(p, "highlight(\"file\") takes only name=; the rows, spans, "
                        "colours and labels come from the file", "");
        if (!h->file && !h->col && !h->region)
            return fail(p, "highlight() needs a target: (\"row\",\"col\") for a "
                        "heatmap cell, or row= and region= for a matrix() track", "");
        if (!h->file && h->region && !h->row)
            return fail(p, "highlight(region=) needs row= (the sample to box)", "");
        if (h->col && (h->region || h->label || h->dash))
            return fail(p, "highlight(\"row\",\"col\") is the heatmap form; "
                        "region=/label=/linetype= belong to the track form", "");
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
    if (!strcmp(name, "chord")) {
        /* chord("links.csv"[, from=][, to=][, value=][, gap=][, alpha=]) —
         * a circlize-style chord diagram; its own mode, like the tree.
         * Sectors are the union of the from/to names, arc length
         * proportional to each sector's total flow; ribbons connect
         * sub-arcs, width proportional to value, coloured by source. */
        spec->chord_mode = 1;
        skip_ws(p);
        if (*p->s == '"') {
            spec->data_path = string_lit(p);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad chord() argument", "");
            skip_ws(p);
            if (!strcmp(key, "from")) {
                spec->chord_from = string_lit(p);
                if (!spec->chord_from) return fail(p, "from= expects a quoted column name", "");
            } else if (!strcmp(key, "to")) {
                spec->chord_to = string_lit(p);
                if (!spec->chord_to) return fail(p, "to= expects a quoted column name", "");
            } else if (!strcmp(key, "value")) {
                spec->chord_value = string_lit(p);
                if (!spec->chord_value) return fail(p, "value= expects a quoted column name", "");
            } else if (!strcmp(key, "gap")) {
                char *end;
                double v = strtod(p->s, &end);
                if (end == p->s || v < 0 || v >= 90)
                    return fail(p, "gap= expects degrees in [0, 90)", "");
                p->s = end;
                spec->chord_gap = v;
            } else if (!strcmp(key, "alpha")) {
                char *end;
                double v = strtod(p->s, &end);
                if (end == p->s || !(v > 0 && v <= 1))
                    return fail(p, "alpha= must be in (0, 1]", "");
                p->s = end;
                spec->chord_alpha = v;
            } else if (!strcmp(key, "order")) {
                /* order=c("A", ...): the full sector list, circlize's order= */
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "order= expects c(\"...\", ...)", "");
                spec->chord_order = cp_xmalloc(256 * sizeof(char *));
                spec->n_chord_order = 0;
                for (;;) {
                    skip_ws(p);
                    if (*p->s == ')') { p->s++; break; }
                    char *v = string_lit(p);
                    if (!v) return fail(p, "order=c(...) expects quoted names", "");
                    if (spec->n_chord_order >= 256)
                        return fail(p, "order=c(...) holds at most 256 sectors", "");
                    for (int i = 0; i < spec->n_chord_order; i++)
                        if (!strcmp(spec->chord_order[i], v))
                            return fail(p, "order=c(...): `%s` is listed twice; every "
                                        "sector goes exactly once", v);
                    spec->chord_order[spec->n_chord_order++] = v;
                    skip_ws(p);
                    if (*p->s == ',') { p->s++; continue; }
                    if (*p->s == ')') { p->s++; break; }
                    return fail(p, "expected , or ) in order=c(...)", "");
                }
            } else if (!strcmp(key, "bipartite")) {
                if (parse_bool(p, &spec->chord_bipartite))
                    return fail(p, "bipartite= expects TRUE or FALSE", "");
            } else return fail(p, "chord() option `%s` not implemented; supported: "
                               "from=, to=, value=, gap=, alpha=, order=, bipartite=", key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        if (expect(p, ')')) return -1;
        /* both prescribe the sector order; order= used to win silently */
        if (spec->n_chord_order && spec->chord_bipartite)
            return fail(p, "chord(): order= and bipartite=TRUE both fix the sector "
                        "order; give one (order= can list the from-group first)", "");
        return 0;
    }
    if (!strcmp(name, "coord_cartesian")) {
        /* only the expansion control is meaningful here (there is no zoom
         * yet): expand=FALSE zeroes both axes' expansion at once, the
         * frame-hugs-the-tiles spelling. */
        skip_ws(p);
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad coord_cartesian() argument", "");
            if (strcmp(key, "expand"))
                return fail(p, "coord_cartesian option `%s` not implemented "
                            "(expand=FALSE)", key);
            int b;
            if (parse_bool(p, &b) || b)
                return fail(p, "coord_cartesian supports only expand=FALSE", "");
            spec->x_exp_mult = spec->x_exp_add = 0; spec->has_x_expand = 1;
            spec->y_exp_mult = spec->y_exp_add = 0; spec->has_y_expand = 1;
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "coord_polar")) {
        /* radar ("spider") charts: the supported subset is a discrete x
         * whose categories become spokes, y as radius, geom_line() series
         * drawn CLOSED, dashed rings at the y breaks. General polar (pie /
         * rose bars) is on the roadmap. */
        spec->polar = 1;
        skip_ws(p);
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad coord_polar() argument", "");
            if (strcmp(key, "start"))
                return fail(p, "coord_polar option `%s` not implemented (start=)", key);
            skip_ws(p);
            char *end;
            double v = strtod(p->s, &end);
            if (end == p->s) return fail(p, "start= expects radians", "");
            p->s = end;
            spec->polar_start = v;
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
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
            if (!strcmp(key, "expand")) {
                free(key);
                skip_ws(p);
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "expand= expects c(mult, add)", "");
                double m2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ',') p->s++;
                double a2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ')') p->s++;
                if (m2 < 0 || a2 < 0)
                    return fail(p, "expand= values must be >= 0", "");
                if (isx) { spec->x_exp_mult = m2; spec->x_exp_add = a2; spec->has_x_expand = 1; }
                else     { spec->y_exp_mult = m2; spec->y_exp_add = a2; spec->has_y_expand = 1; }
            } else if (!strcmp(key, "angle")) {
                free(key);
                skip_ws(p);
                double v = strtod(p->s, (char **)&p->s);
                if (v < 0 || v > 90)
                    return fail(p, "angle= must be between 0 and 90", "");
                if (isx) spec->x_angle = v; else spec->y_angle = v;
            } else
                return fail(p, "scale_*_discrete() option `%s` not implemented; "
                            "supported: angle=, expand=", key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "facet_wrap")) {
        skip_ws(p);
        if (*p->s != '~') return fail(p, "facet_wrap() expects a formula: facet_wrap(~var)", "");
        p->s++;
        spec->facet_var = colname(p);
        if (!spec->facet_var) return *p->err ? -1 : fail(p, "expected a column name after ~", "");
        skip_ws(p);
        if (*p->s == '+' || *p->s == '~')
            return fail(p, "facet_wrap() takes one variable; for two-way facets "
                        "use facet_grid(rowvar ~ colvar)", "");
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
    /* facet_grid(rowvar ~ colvar) -- the two-way grid. One panel per (row,
     * col) combination, including the combinations the data never uses, which
     * is the whole point of asking for a grid rather than a wrap. Either side
     * may be `.`, as in ggplot2, for a one-row or one-column grid. */
    if (!strcmp(name, "facet_grid")) {
        const char *form = "facet_grid() expects a two-sided formula: "
                           "facet_grid(rowvar ~ colvar), facet_grid(. ~ colvar) "
                           "or facet_grid(rowvar ~ .)";
        skip_ws(p);
        if (*p->s == '~') return fail(p, form, "");
        char *rv = colname(p);
        if (!rv) return *p->err ? -1 : fail(p, form, "");
        skip_ws(p);
        if (*p->s == '+') return fail(p, "facet_grid() takes one variable per "
                                      "side; rowvar + rowvar2 ~ colvar is not "
                                      "implemented", "");
        if (expect(p, '~')) return fail(p, form, "");
        char *cv = colname(p);
        if (!cv) return *p->err ? -1 : fail(p, form, "");
        skip_ws(p);
        if (*p->s == '+') return fail(p, "facet_grid() takes one variable per "
                                      "side; rowvar ~ colvar + colvar2 is not "
                                      "implemented", "");
        if (!strcmp(rv, ".")) { free(rv); rv = NULL; }
        if (!strcmp(cv, ".")) { free(cv); cv = NULL; }
        if (!rv && !cv)
            return fail(p, "facet_grid(. ~ .) names no faceting variable; put a "
                        "column on at least one side", "");
        if (rv && cv && !strcmp(rv, cv))
            return fail(p, "facet_grid() has `%s` on both sides; a grid needs two "
                        "variables (facet_wrap(~var) is the one-way form)", rv);
        spec->facet_grid = 1;
        spec->facet_rowvar = rv;
        spec->facet_colvar = cv;
        skip_ws(p);
        while (*p->s == ',') {
            p->s++;
            char *key = ident(p);
            if (!key || expect(p, '='))
                return fail(p, "facet_grid() supports scales=", "");
            if (!strcmp(key, "scales")) {
                /* per ggplot2's facet_grid: a freed x is shared down a COLUMN
                 * and a freed y across a ROW, not per panel */
                char *v = string_lit(p);
                if (!v) v = ident(p);
                if (!v) { free(key);
                    return fail(p, "scales= expects fixed, free_x, free_y, or free", ""); }
                if (!strcmp(v, "fixed")) { spec->free_x = 0; spec->free_y = 0; }
                else if (!strcmp(v, "free_x")) { spec->free_x = 1; spec->free_y = 0; }
                else if (!strcmp(v, "free_y")) { spec->free_x = 0; spec->free_y = 1; }
                else if (!strcmp(v, "free"))   { spec->free_x = 1; spec->free_y = 1; }
                else if (!strcmp(v, "free_colour") || !strcmp(v, "free_color")) {
                    free(v); free(key);
                    return fail(p, "scales=\"free_colour\" is a facet_wrap() "
                                "extension (one colour scale per panel) and is not "
                                "implemented for facet_grid(); use fixed, free_x, "
                                "free_y or free", "");
                } else { free(key);
                    return fail(p, "scales=%s invalid; use fixed, free_x, free_y, "
                                "or free", v); }
                free(v);
            } else if (!strcmp(key, "levels")) {
                free(key);
                return fail(p, "facet_grid() option `levels=` is not implemented "
                            "(the row and column orders are the factor orders); "
                            "facet_wrap(~var, levels=c(...)) takes it", "");
            } else if (!strcmp(key, "ncol") || !strcmp(key, "nrow")) {
                char msg[CP_ERRLEN];
                snprintf(msg, sizeof msg, "facet_grid() option `%s=` is not "
                         "implemented; the grid shape is the two variables' level "
                         "counts (facet_wrap(~var, %s=) takes it)", key, key);
                free(key);
                return fail(p, "%s", msg);
            } else {
                char msg[CP_ERRLEN];
                snprintf(msg, sizeof msg, "facet_grid() option `%s` not "
                         "implemented; supported: scales=", key);
                free(key);
                return fail(p, "%s", msg);
            }
            free(key);
            skip_ws(p);
        }
        return expect(p, ')');
    }
    if (!strcmp(name, "theme")) {
        /* an arbitrary theme() stays unimplemented (presets only) — except
         * the legend-position pair, which has no preset spelling:
         * theme(legend.position="inside", legend.position.inside=c(x, y))
         * draws the legend block(s) inside the panel(s), where the data
         * leave room. Under scales="free_colour" every facet's block sits
         * at the same (x, y) of its own panel. */
        skip_ws(p);
        while (*p->s != ')') {
            char key[64];
            size_t kl = 0;
            for (;;) {                            /* dotted key, ggplot-style */
                char *seg = ident(p);
                if (!seg) return fail(p, "bad theme() argument", "");
                size_t sl = strlen(seg);
                if (kl + sl + 2 >= sizeof key) return fail(p, "theme() key too long", "");
                memcpy(key + kl, seg, sl); kl += sl; key[kl] = 0;
                free(seg);
                if (*p->s == '.') { key[kl++] = '.'; key[kl] = 0; p->s++; continue; }
                break;
            }
            if (expect(p, '=')) return fail(p, "bad theme() argument", "");
            skip_ws(p);
            if (!strcmp(key, "legend.position")) {
                char *v = word(p);
                if (!v) return fail(p, "theme(legend.position=) expects \"inside\" or \"none\"", "");
                if (!strcmp(v, "none")) {          /* the commonest ggplot2 legend line */
                    free(v);
                    spec->no_legend = spec->no_legend_size = spec->no_legend_shape = 1;
                    skip_ws(p);
                    if (*p->s == ',') { p->s++; skip_ws(p); }
                    continue;
                }
                if (strcmp(v, "inside")) {
                    char msg[CP_ERRLEN];
                    snprintf(msg, sizeof msg, "theme(legend.position=\"%s\") is not "
                             "implemented: only \"inside\" and \"none\" are (the legend "
                             "sits in the right margin otherwise); to drop one guide use "
                             "guides(colour=\"none\") or --no-legend", v);
                    return fail(p, "%s", msg);
                }
                free(v);
                spec->legend_inside = 1;
                if (spec->leg_ix == 0 && spec->leg_iy == 0) {
                    spec->leg_ix = 0.85; spec->leg_iy = 0.15;   /* lower right */
                }
            } else if (!strcmp(key, "legend.position.inside")) {
                if (p->s[0] == 'c' && p->s[1] == '(') p->s += 2;
                else return fail(p, "legend.position.inside= expects c(x, y)", "");
                double x2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ',') p->s++;
                double y2 = strtod(p->s, (char **)&p->s);
                skip_ws(p); if (*p->s == ')') p->s++;
                if (x2 < 0 || x2 > 1 || y2 < 0 || y2 > 1)
                    return fail(p, "legend.position.inside= wants npc "
                                "coordinates in [0, 1]", "");
                spec->leg_ix = x2; spec->leg_iy = y2;
                spec->legend_inside = 1;
            } else return fail(p, "theme(%s=) is not implemented; the presets "
                               "(theme_bw() etc., with base_line_size=) cover "
                               "the rest — supported here: legend.position, "
                               "legend.position.inside", key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    if (!strncmp(name, "theme_", 6)) {           /* preset theme selector */
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
        /* optional base_line_size= (ggplot's own theme argument): the width
         * of every chrome line — border, axis line, grid, ticks — as a
         * linewidth value where 0.5 is the ggplot2 default. Data strokes
         * are untouched. */
        skip_ws(p);
        while (*p->s != ')') {
            char *key = ident(p);
            if (!key || expect(p, '=')) return fail(p, "bad theme argument", "");
            if (strcmp(key, "base_line_size"))
                return fail(p, "theme option `%s` not implemented "
                            "(base_line_size=)", key);
            skip_ws(p);
            char *end;
            double v = strtod(p->s, &end);
            if (end == p->s || !(v > 0))
                return fail(p, "base_line_size= expects a number > 0 "
                            "(0.5 = the ggplot2 default)", "");
            p->s = end;
            spec->base_line_size = v;
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
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
            int is_aes = !strncmp(p->s, "aes", 3)
                       && (p->s[3] == '(' || isspace((unsigned char)p->s[3]));
            if (is_aes || (key && !strcmp(key, "mapping"))) {
                if (is_aes) p->s += 3;
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
            /* each aesthetic has its own guide; "none" on one used to drop
             * them all, and guide_legend() options only reach the colour one */
            int is_col = !strcmp(key, "colour") || !strcmp(key, "color") || !strcmp(key, "fill");
            int is_size = !strcmp(key, "size"), is_shape = !strcmp(key, "shape");
            if (!is_col && !is_size && !is_shape) {
                fail(p, "guides() supports colour=, color=, fill=, size=, shape=", "");
                free(key); return -1;
            }
            skip_ws(p);
            char *v = word(p);                       /* "none" / guide_legend */
            if (!v) { free(key); return fail(p, "guides() values must be \"none\" or guide_legend(...)", ""); }
            if (!strcmp(v, "guide_legend") && !is_col) {
                free(v);
                fail(p, "guides(%s=guide_legend(...)): guide_legend() options apply to "
                        "the colour/fill legend only; \"none\" is the one setting the "
                        "size/shape guides take", key);
                free(key); return -1;
            }
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
                    skip_ws(p);
                    if (!strcmp(gk, "reverse")) {
                        if (parse_bool(p, &spec->legend_reverse))
                            return fail(p, "reverse= expects TRUE or FALSE", "");
                    } else if (!strcmp(gk, "ncol") || !strcmp(gk, "nrow")) {
                        char *end;
                        double n2 = strtod(p->s, &end);
                        if (end == p->s || n2 < 1 || n2 != (int)n2)
                            return fail(p, "guide_legend %s= expects a positive integer", gk);
                        p->s = end;
                        if (gk[1] == 'c') spec->legend_ncol = (int)n2;
                        else spec->legend_nrow = (int)n2;
                    } else return fail(p, "guide_legend option `%s` not implemented "
                                     "(ncol=, nrow=, reverse=)", gk);
                    skip_ws(p);
                    if (*p->s == ',') { p->s++; skip_ws(p); }
                }
                p->s++;
                if (spec->legend_ncol && spec->legend_nrow)
                    return fail(p, "guide_legend: give ncol= or nrow=, not both", "");
            } else if (!strcmp(v, "none")) {
                free(v);
                if (is_col) spec->no_legend = 1;
                else if (is_size) spec->no_legend_size = 1;
                else spec->no_legend_shape = 1;
            } else {
                free(v); free(key);
                return fail(p, "guides() supports \"none\" or guide_legend(ncol=/nrow=/reverse=)", "");
            }
            free(key);
            skip_ws(p);
            if (*p->s == ',') { p->s++; skip_ws(p); }
        }
        return expect(p, ')');
    }
    /* a few near-misses get a pointer instead of the whole menu */
    if (!strcmp(name, "geom_path") || !strcmp(name, "geom_step") || !strcmp(name, "geom_area")
        || !strcmp(name, "geom_ribbon") || !strcmp(name, "geom_violin") || !strcmp(name, "geom_crossbar")
        || !strcmp(name, "geom_pointrange") || !strcmp(name, "geom_bin2d") || !strcmp(name, "geom_hex")
        || !strcmp(name, "geom_polygon") || !strcmp(name, "geom_dotplot") || !strcmp(name, "geom_rug")
        || !strcmp(name, "geom_freqpoly") || !strcmp(name, "geom_curve") || !strcmp(name, "geom_count"))
        return fail(p, "%s() is not implemented (see the geom list in --help)", name);
    if (!strcmp(name, "scale_colour_gradientn") || !strcmp(name, "scale_color_gradientn")
        || !strcmp(name, "scale_fill_gradientn"))
        return fail(p, "%s() is not implemented; use gradient2() (three stops) or a "
                    "named ramp (viridis, magma, ..., scale_*_distiller(palette=))", name);
    if (!strcmp(name, "theme_set") || !strcmp(name, "library") || !strcmp(name, "print")
        || !strcmp(name, "dev.off") || !strcmp(name, "pdf") || !strcmp(name, "png"))
        return fail(p, "%s() is R session plumbing, not part of a plot; the spec is "
                    "`data + aes() + geom_*()` and the output is the CLI argument", name);
    return fail(p, "`%s()` is not implemented; supported: "
                   "aes(), ggplot(), ggsave(); geom_point/jitter/line/smooth/col/bar/histogram/"
                   "boxplot/density/tile/raster/segment/rect/hline/vline/abline/errorbar/linerange/"
                   "text/label/text_repel/label_repel(), annotate(); labs()/xlab()/ylab()/ggtitle(); "
                   "facet_wrap(~var), facet_grid(row ~ col); coord_flip/polar/cartesian(); scale_x|y_log10/log2/continuous/"
                   "discrete(), xlim(), ylim(), scale_x_genome(); scale_colour|fill_manual/brewer/"
                   "distiller/identity/gradient/gradient2/viridis/magma/...(); theme_bw/minimal/"
                   "classic/...(), theme(legend.position=), guides(); heatmap(), annotation(), "
                   "legend(), dendrogram(), highlight(); region()/regions(), coverage(), interval(), "
                   "genes(), arcs(), matrix(), cytoband(), ideogram(); geom_tree/tiplab/nodelab/"
                   "tippoint/nodepoint(); chord()", name);
}

int dsl_parse(const char *src, PlotSpec *spec, char *err) {
    P p = {src, err};
    memset(spec, 0, sizeof *spec);
    /* <0 means "decide from the measured labels"; 0 is a caller asking for
     * horizontal, which is a different thing and must survive. */
    spec->x_angle = spec->y_angle = -1;
    spec->chord_gap = -1;                        /* unset; gap=0 is a real request */
    spec->fill.kind = FILL_VIRIDIS;              /* default heatmap fill */
    /* default theme = THEME_GRAY (enum 0, the memset default) */

    /* leading data term, unless the first term is a function call (track
     * mode starts with region()/coverage() — no top-level data file) */
    skip_ws(&p);
    const char *s = p.s;
    if (*p.s == '"' || *p.s == '\'') {   /* "my data.csv" + ...: a path with spaces */
        spec->data_path = string_lit(&p);
        if (!spec->data_path) return fail(&p, "unterminated quoted data path at start of spec", "");
        if (!*spec->data_path) return fail(&p, "missing data file at start of spec", "");
    } else {
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
    }

    for (;;) {
        skip_ws(&p);
        if (!*p.s) break;
        if (expect(&p, '+')) return -1;
        if (parse_term(&p, spec)) return -1;
    }

    int is_hm = spec->nhobjs > 0 && !spec->nlayers && !spec->x.col
              && !spec->facet_var && !spec->facet_grid;
    int is_trk = spec->ntracks > 0;
    /* aes(fill=) is stored in spec->colour (fill and colour are one aesthetic
     * here), but scale_fill_*() writes spec->fill, which only heatmap mode
     * reads. In grammar mode that made every scale_fill_gradient/viridis/jet a
     * silent no-op: the spec parsed, the run succeeded, and the default ramp
     * came out. Alias it onto the colour scale instead. scale_colour_*() still
     * wins if both are given. */
    if (!is_hm && !is_trk && spec->has_fill && !spec->has_colour_scale) {
        spec->colour_scale = spec->fill;
        spec->has_colour_scale = 1;
    }
    /* ... and the mirror image in heatmap mode: scale_colour_gradient2() on a
     * heatmap() parsed and changed nothing, because heatmap.c reads only the
     * fill scale. One aesthetic, so alias the other way there. */
    if (is_hm && spec->has_colour_scale && !spec->has_fill) {
        spec->fill = spec->colour_scale;
        spec->has_fill = 1;
    }
    /* labs(fill=) titles that same aesthetic, so honour it when labs(colour=)
     * was not given. */
    if (!is_hm && !is_trk && spec->lab_fill && !spec->lab_colour)
        spec->lab_colour = spec->lab_fill;

    /* ---- the range geoms and the ymin alias ----
     * aes(ymin=) used to alias y (the rect-corner spelling). It is its own
     * aesthetic now (geom_errorbar needs y AND ymin distinct); a spec with
     * ymin= and no y= keeps the old meaning -- UNLESS a range geom is present,
     * where ggplot's canonical aes(x, ymin=lo, ymax=hi) + geom_errorbar() has
     * no y at all. Then y takes the same column as ymin: the renderer needs a
     * y for its row filter and range, the rows it would drop (NA ymin) have
     * no bar anyway, and ggplot titles that axis by the first y-family
     * aesthetic, which is ymin. */
    int nrange = 0, nother = 0;
    for (int i = 0; i < spec->nlayers; i++) {
        GeomType t = spec->layers[i].type;
        if (t == GEOM_ERRORBAR || t == GEOM_LINERANGE) nrange++;
        else if (t != GEOM_HLINE && t != GEOM_VLINE && t != GEOM_ABLINE) nother++;
    }
    if (!spec->y.col && spec->ymin.col) {
        if (nrange && !nother) spec->y = spec->ymin;
        else if (nrange)
            return fail(&p, "geom_errorbar()/geom_linerange() beside another geom "
                        "need aes(y=) for that geom (aes(x, y=mean, ymin=lo, ymax=hi))", "");
        else {
            spec->y = spec->ymin;
            memset(&spec->ymin, 0, sizeof spec->ymin);
        }
    }

    /* ---- mode mixing ---- */
    int any_tree = spec->tree_mode || spec->tree_tiplab || spec->tree_nodelab
                || spec->tree_nodepoint || spec->tree_tippoint;
    int any_grammar = spec->nlayers || spec->x.col || spec->facet_var || spec->facet_grid;
    if (spec->chord_mode) {
        if (any_grammar || spec->nhobjs || spec->ntracks || any_tree || spec->polar
            || spec->nannos || spec->coord_flip)
            return fail(&p, "chord() is its own mode and cannot be mixed with "
                        "aes()/geom_*, facet_wrap(), coord_*(), heatmap(), tracks, "
                        "trees or annotate()", "");
        if (spec->theme || spec->base_line_size > 0)
            return fail(&p, "theme_*() has no effect on a chord diagram (no panel, "
                        "no axes) and is refused rather than ignored", "");
        if (spec->lab_x || spec->lab_y || spec->lab_colour || spec->lab_fill
            || spec->lab_subtitle || spec->lab_caption)
            return fail(&p, "chord() draws no axes or legend; only labs(title=) applies", "");
        if (spec->has_xlim || spec->has_ylim || spec->log_x || spec->log_y
            || spec->has_colour_scale || spec->has_fill || spec->no_legend
            || spec->legend_inside)
            return fail(&p, "chord() takes no scales or guides beyond "
                        "scale_*_manual(values=) for the sector colours", "");
        return 0;                                /* chord mode: nothing else to check */
    }
    if (any_tree && (spec->nhobjs || spec->ntracks || any_grammar || spec->polar
                     || spec->nannos || spec->coord_flip || spec->nhls))
        return fail(&p, spec->tree_mode
                    ? "geom_tree() is its own mode and cannot be mixed with aes()/geom_*, "
                      "heatmap(), the track verbs, coord_*() or annotate()"
                    : "the tree geoms (geom_tiplab() etc.) belong to geom_tree() and "
                      "cannot be mixed with aes()/geom_*, heatmap() or the track verbs", "");
    /* highlight() pairs its form with its mode: the ("row","col") cell form
     * is heatmap()'s, the row=/region= and file forms are matrix() track's.
     * A form in the wrong mode renders nothing it could mean, so it errors. */
    for (int i = 0; i < spec->nhls; i++) {
        const CellHighlight *h = &spec->hls[i];
        int trackform = h->file || h->region;
        if (trackform && spec->ntracks == 0)
            return fail(&p, "highlight(row=, region=) and highlight(\"boxes.tsv\") box a "
                        "matrix() track and need track mode; a heatmap() cell is "
                        "highlight(\"row\",\"col\")", "");
        if (!trackform && spec->nhobjs == 0)
            return fail(&p, spec->ntracks
                        ? "highlight(\"row\",\"col\") boxes a heatmap() cell; on a matrix() "
                          "track write highlight(name=, row=, region=\"chr:beg-end\")"
                        : "highlight() marks a heatmap() cell and needs heatmap mode", "");
    }
    if (spec->ntracks > 0 && spec->nhls > 0) {
        int nmat = 0;
        for (int i = 0; i < spec->ntracks; i++) nmat += spec->tobjs[i].type == TRK_MATRIX;
        if (nmat == 0)
            return fail(&p, "highlight() on the track browser boxes a matrix() track; "
                        "there is none in this spec", "");
    }
    if (spec->polar && spec->coord_flip)
        return fail(&p, "coord_polar() and coord_flip() contradict each other", "");
    if (spec->nannos > 0 && !any_grammar)
        return fail(&p, "annotate() places a mark on a grammar panel and needs "
                    "aes()/geom_*", "");

    if (spec->ntracks > 0) {           /* track (locus-browser) mode */
        if (any_grammar || spec->nhobjs)
            return fail(&p, "track functions cannot be mixed with grammar/heatmap", "");
        if (spec->polar || spec->coord_flip)
            return fail(&p, "coord_polar()/coord_flip() do not apply to the track browser", "");
        /* the preset itself (panel, grid, strips) has nothing to act on here,
         * but base_line_size= does: it scales the track frames and ticks
         * through cp_line_scale, so theme_*(base_line_size=) is honoured */
        if (spec->theme && !(spec->base_line_size > 0))
            return fail(&p, "theme_*() presets have no effect on the track browser; "
                        "only theme_*(base_line_size=) applies (frame and tick width)", "");
        return 0;
    }

    if (spec->nhobjs > 0 && any_grammar) {
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
                            : "heatmap() cannot be mixed with aes()/geom_*/facet_wrap()/facet_grid()", "");
            if (o->place.given)
                return fail(&p, "annotation() under a grammar panel always draws "
                            "beneath it; placements (left_of/right_of/...) are "
                            "heatmap-mode", "");
            /* a 2nd+ band inherits hm_new's TOP_OF default (a heatmap-mode
             * convention); under a grammar panel every band is a full-width
             * strip, so normalise before render.c looks */
            ((HMObj *)o)->place.kind = PL_FULL;
            if (!o->data)
                return fail(&p, "annotation() needs a data file: "
                            "annotation(\"meta.tsv\"[, column=\"...\"])", "");
        }
        /* falls through to the grammar-mode checks below */
    } else if (spec->nhobjs > 0) {               /* matrix mode */
        /* scale_*_manual() used to be refused outright here, because the cells
         * were always mapped through a continuous FillScale. They no longer
         * are: a categorical matrix takes the manual palette. Whether THIS
         * matrix is categorical is only known once its file is read, so the
         * pairing check (manual vs numbers, a ramp vs categories) lives in
         * heatmap.c beside the data, and still errors rather than ignoring. */
        if (spec->hobjs[0].type != HM_HEATMAP)
            return fail(&p, "the first placed object must be a heatmap()", "");
        if (spec->polar || spec->coord_flip)
            return fail(&p, "coord_polar()/coord_flip() do not apply to heatmap mode "
                        "(cluster=diagonal/symmetric and placements set the layout)", "");
        /* as for tracks: the preset is inert (box=/grid= frame the cells) but
         * base_line_size= scales the frames through cp_line_scale */
        if (spec->theme && !(spec->base_line_size > 0))
            return fail(&p, "theme_*() presets have no effect in heatmap mode (box=/grid= "
                        "frame the cells); only theme_*(base_line_size=) applies", "");
        return 0;
    }
    if (spec->tree_mode) {          /* tree mode: the topology is the data */
        if (spec->theme || spec->base_line_size > 0)
            return fail(&p, "theme_*() has no effect on a tree (no panel chrome) and "
                        "is refused rather than ignored", "");
        return 0;
    }
    if (any_tree)
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
        return fail(&p, nrange ? "geom_errorbar()/geom_linerange() need aes(ymin=, ymax=)"
                    : "aes() must map y", "");
    for (int i = 0; i < spec->nlayers; i++)
        if ((spec->layers[i].type == GEOM_TEXT || spec->layers[i].type == GEOM_LABEL)
            && !spec->label.col)
            return fail(&p, "geom_text()/geom_label() need aes(label=...)", "");

    /* fill= and colour= are one aesthetic here, so on a geom that only
     * strokes (points, lines) fill= paints the stroke, and on one that only
     * fills (bars, tiles) colour= paints the fill. ggplot2 would draw the
     * other thing (a hollow point, an outline). Say so on stderr rather
     * than reinterpret silently; the figure is still what a reader expects. */
    if (spec->colour.col && spec->nlayers - nref > 0) {
        int stroke = 0, area = 0;
        for (int i = 0; i < spec->nlayers; i++) {
            GeomType t = spec->layers[i].type;
            if (t == GEOM_POINT || t == GEOM_JITTER || t == GEOM_LINE || t == GEOM_SMOOTH
                || t == GEOM_TEXT || t == GEOM_LABEL || t == GEOM_SEGMENT
                || t == GEOM_ERRORBAR || t == GEOM_LINERANGE || t == GEOM_DENSITY) stroke++;
            else if (t == GEOM_COL || t == GEOM_BAR || t == GEOM_HISTOGRAM
                     || t == GEOM_TILE || t == GEOM_RECT) area++;
        }
        const char *g0 = NULL;
        for (int i = 0; i < spec->nlayers && !g0; i++) {
            GeomType t = spec->layers[i].type;
            if (t == GEOM_HLINE || t == GEOM_VLINE || t == GEOM_ABLINE || t == GEOM_BOXPLOT) continue;
            g0 = t == GEOM_POINT ? "geom_point" : t == GEOM_JITTER ? "geom_jitter"
               : t == GEOM_LINE ? "geom_line" : t == GEOM_SMOOTH ? "geom_smooth"
               : t == GEOM_TEXT ? "geom_text" : t == GEOM_LABEL ? "geom_label"
               : t == GEOM_SEGMENT ? "geom_segment" : t == GEOM_ERRORBAR ? "geom_errorbar"
               : t == GEOM_LINERANGE ? "geom_linerange" : t == GEOM_DENSITY ? "geom_density"
               : t == GEOM_COL ? "geom_col" : t == GEOM_BAR ? "geom_bar"
               : t == GEOM_HISTOGRAM ? "geom_histogram" : t == GEOM_TILE ? "geom_tile"
               : t == GEOM_RECT ? "geom_rect" : NULL;
        }
        if (g0 && spec->colour.is_fill && stroke && !area)
            fprintf(stderr, "cinderplot: warning: aes(fill=) on %s() is drawn as "
                    "colour= (the mark has no separate fill)\n", g0);
        else if (g0 && !spec->colour.is_fill && area && !stroke)
            fprintf(stderr, "cinderplot: warning: aes(colour=) on %s() is drawn as "
                    "fill= (the bars/cells are filled, not outlined)\n", g0);
    }
    return 0;
}
