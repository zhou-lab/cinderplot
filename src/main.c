/* cinderplot.c — CLI entry point.
 *
 * Two front doors, one code path: a DSL expression (verbatim ggplot2
 * subset), or shortcut flags that DESUGAR into the same DSL string.
 * --dump-spec prints the desugared spec.
 *
 *   cinderplot 'mtcars.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()'
 *   cinderplot mtcars.csv -x hp -y mpg -c cyl -f gear -t "Title" -o fig.pdf
 *   q "select ..." | cinderplot -x date -y value
 */
#include "cinderplot.h"
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *USAGE =
    "usage: cinderplot [DSL-expr | flags] [data.csv] [-o out.pdf|.svg|.png] [--size WxH] [--dpi N] [--dump-spec]\n"
    "         (output format is chosen from the -o extension: .svg -> SVG, .png -> PNG, else PDF)\n"
    "  DSL:   'data.csv + aes(x, y, colour=factor(g)) + geom_point()\n"
    "          + labs(title=\"...\") + facet_wrap(~g)'\n"
    "  flags: -x COL -y COL [-c COL] [-f COL] [-t TITLE] [-m point|line|col|histogram]\n"
    "         [--log x|y|xy]                              (desugar to the DSL)\n"
    "  data \"-\" or omitted = stdin\n"
    "  --version, --help\n";

static int appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return -1;
    *len += (size_t)n;
    return 0;
}

static int append_quoted(char *buf, size_t cap, size_t *len, const char *s) {
    if (appendf(buf, cap, len, "\"")) return -1;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            if (appendf(buf, cap, len, "\\%c", *s)) return -1;
        } else if (*s == '\n') {
            if (appendf(buf, cap, len, "\\n")) return -1;
        } else if (appendf(buf, cap, len, "%c", *s)) return -1;
    }
    return appendf(buf, cap, len, "\"");
}

int main(int argc, char **argv) {
    const char *out = "plot.pdf", *expr = NULL, *data = NULL;
    const char *fx = NULL, *fy = NULL, *fc = NULL, *ff = NULL, *ft = NULL;
    const char *fm = "point", *flog = NULL, *fregion = NULL;
    double w_in = 6, h_in = 4, dpi = 96;
    int dump = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "-x") && i + 1 < argc) fx = argv[++i];
        else if (!strcmp(a, "-y") && i + 1 < argc) fy = argv[++i];
        else if (!strcmp(a, "-c") && i + 1 < argc) fc = argv[++i];
        else if (!strcmp(a, "-f") && i + 1 < argc) ff = argv[++i];
        else if (!strcmp(a, "-t") && i + 1 < argc) ft = argv[++i];
        else if (!strcmp(a, "-m") && i + 1 < argc) fm = argv[++i];
        else if (!strcmp(a, "--log") && i + 1 < argc) flog = argv[++i];
        else if ((!strcmp(a, "-r") || !strcmp(a, "--region")) && i + 1 < argc) fregion = argv[++i];
        else if (!strcmp(a, "--size") && i + 1 < argc) {
            char tail;
            if (sscanf(argv[++i], "%lfx%lf%c", &w_in, &h_in, &tail) != 2
                    || !isfinite(w_in) || !isfinite(h_in) || w_in <= 0 || h_in <= 0) {
                fprintf(stderr, "cinderplot: bad --size, expected WxH in inches\n%s", USAGE);
                return 1;
            }
        }
        else if (!strcmp(a, "--dpi") && i + 1 < argc) {
            dpi = atof(argv[++i]);
            if (!(dpi > 0) || dpi > 2400) {
                fprintf(stderr, "cinderplot: bad --dpi, expected 1..2400\n%s", USAGE);
                return 1;
            }
        }
        else if (!strcmp(a, "--dump-spec")) dump = 1;
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) { printf("cinderplot %s\n", CINDERPLOT_VERSION); return 0; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { printf("%s", USAGE); return 0; }
        else if (strchr(a, '(')) expr = a;
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "cinderplot: unknown flag %s\n%s", a, USAGE); return 1; }
        else data = a;
    }

    char buf[4096];
    if (!expr) {
        int hist = !strcmp(fm, "histogram");
        int box = !strcmp(fm, "boxplot");
        int bar = !strcmp(fm, "bar");
        if (!fx || (!fy && !hist && !bar)) { fprintf(stderr, "%s", USAGE); return 1; }
        size_t n = 0;
        /* boxplot and bar need a discrete x — wrap the column in factor() */
        int bad = (box || bar) ? appendf(buf, sizeof buf, &n, "%s + aes(x=factor(%s)", data ? data : "-", fx)
                               : appendf(buf, sizeof buf, &n, "%s + aes(x=%s", data ? data : "-", fx);
        if (!bad && fy) bad = appendf(buf, sizeof buf, &n, ", y=%s", fy);
        if (!bad && fc) bad = appendf(buf, sizeof buf, &n, ", colour=factor(%s)", fc);
        if (!bad) bad = appendf(buf, sizeof buf, &n, ") + geom_%s()", fm);
        if (!bad && ff) bad = appendf(buf, sizeof buf, &n, " + facet_wrap(~%s)", ff);
        if (!bad && flog && strchr(flog, 'x')) bad = appendf(buf, sizeof buf, &n, " + scale_x_log10()");
        if (!bad && flog && strchr(flog, 'y')) bad = appendf(buf, sizeof buf, &n, " + scale_y_log10()");
        if (!bad && ft) {
            bad = appendf(buf, sizeof buf, &n, " + labs(title=");
            if (!bad) bad = append_quoted(buf, sizeof buf, &n, ft);
            if (!bad) bad = appendf(buf, sizeof buf, &n, ")");
        }
        if (bad) {
            fprintf(stderr, "cinderplot: generated plot expression is too long\n");
            return 1;
        }
        expr = buf;
    }
    if (dump) printf("%s\n", expr);
    cp_set_dpi(dpi);

    char err[256] = "";
    PlotSpec spec;
    if (dsl_parse(expr, &spec, err)) {
        fprintf(stderr, "cinderplot: %s\n", err);
        return 1;
    }
    if (spec.ntracks > 0) {                      /* track (locus-browser) mode */
        if (!spec.region) spec.region = (char *)fregion;   /* --region flag */
        if (render_tracks(&spec, out, w_in * 72, h_in * 72, err)) {
            fprintf(stderr, "cinderplot: %s\n", err);
            return 1;
        }
        fprintf(stderr, "wrote %s\n", out);
        return 0;
    }
    if (spec.nhobjs > 0) {                       /* matrix (wheatmap) mode */
        if (render_heatmap(&spec, out, w_in * 72, h_in * 72, err)) {
            fprintf(stderr, "cinderplot: %s\n", err);
            return 1;
        }
        fprintf(stderr, "wrote %s\n", out);
        return 0;
    }
    DataFrame *df = df_read_csv(spec.data_path, err);
    if (!df) { fprintf(stderr, "cinderplot: %s\n", err); return 1; }
    if (render_plot(&spec, df, out, w_in * 72, h_in * 72, err)) {
        fprintf(stderr, "cinderplot: %s\n", err);
        return 1;
    }
    fprintf(stderr, "wrote %s\n", out);
    return 0;
}
