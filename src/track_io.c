/* track_io.c — tab-separated, headerless, region-filtered readers for the
 * locus track-browser (BED, bedGraph, BEDPE). Coordinates are BED-style
 * 0-based half-open and kept as-is. Only records overlapping the requested
 * region are loaded. Comment / `track` / `browser` lines are skipped. */
#include "cinderplot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, char *err) {
    if (!strcmp(path, "stdin")) path = "-";          /* alias for stdin */
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) return gz_read_all(path, err);
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) { sprintf(err, "cannot open %s", path); return NULL; }
    size_t cap = 1 << 16, n = 0, r;
    char *b = malloc(cap);
    while ((r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r; if (n == cap) { cap *= 2; b = realloc(b, cap); }
    }
    b[n] = 0;
    if (f != stdin) fclose(f);
    return b;
}

/* split a line by tabs in place; returns field count (<= maxf) */
static int split_tab(char *line, char **f, int maxf) {
    int n = 0; char *s = line;
    while (n < maxf) {
        f[n++] = s;
        char *t = strchr(s, '\t');
        if (!t) break;
        *t = 0; s = t + 1;
    }
    return n;
}
static int skip_line(const char *l) {
    return !*l || *l == '#' || !strncmp(l, "track", 5) || !strncmp(l, "browser", 7);
}
static int overlaps(long s, long e, long rs, long re) { return s < re && e > rs; }

int region_parse(const char *s, char *chrom, long *start, long *end) {
    const char *colon = strchr(s, ':');
    const char *dash = colon ? strchr(colon, '-') : NULL;
    if (!colon || !dash) return -1;
    size_t n = colon - s;
    if (n == 0 || n >= 64) return -1;
    memcpy(chrom, s, n); chrom[n] = 0;
    char buf[32]; int j = 0;
    for (const char *q = colon + 1; q < dash && j < 31; q++) if (*q != ',') buf[j++] = *q;
    buf[j] = 0; *start = atol(buf);
    j = 0;
    for (const char *q = dash + 1; *q && j < 31; q++) if (*q != ',') buf[j++] = *q;
    buf[j] = 0; *end = atol(buf);
    return (*end > *start) ? 0 : -1;
}

/* line-iterator boilerplate shared by the readers */
#define FOR_LINES(buf) \
    for (char *line = (buf), *nl; line && *line; line = nl) \
        if ((nl = strchr(line, '\n')) ? (*nl = 0, nl++, 0) : 0, !skip_line(line)) \

Interval *bed_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 64, cnt = 0;
    Interval *out = malloc(cap * sizeof *out);
    FOR_LINES(buf) {
        char *f[12]; int nf = split_tab(line, f, 12);
        if (nf < 3 || strcmp(f[0], chrom)) continue;
        long s = atol(f[1]), e = atol(f[2]);
        if (!overlaps(s, e, rs, re)) continue;
        if (cnt == cap) { cap *= 2; out = realloc(out, cap * sizeof *out); }
        out[cnt].start = s; out[cnt].end = e;
        out[cnt].name = nf > 3 && strcmp(f[3], ".") ? strdup(f[3]) : NULL;
        out[cnt].score = nf > 4 ? atof(f[4]) : 0;
        out[cnt].strand = nf > 5 ? f[5][0] : '.';
        cnt++;
    }
    free(buf); *n = cnt; return out;
}

SigBin *bedgraph_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 256, cnt = 0;
    SigBin *out = malloc(cap * sizeof *out);
    FOR_LINES(buf) {
        char *f[5]; int nf = split_tab(line, f, 5);
        if (nf < 4 || strcmp(f[0], chrom)) continue;
        long s = atol(f[1]), e = atol(f[2]);
        if (!overlaps(s, e, rs, re)) continue;
        if (cnt == cap) { cap *= 2; out = realloc(out, cap * sizeof *out); }
        out[cnt].start = s; out[cnt].end = e; out[cnt].val = atof(f[3]);
        cnt++;
    }
    free(buf); *n = cnt; return out;
}

/* parse a comma-separated list of longs; returns count */
static int commalist(const char *s, long *out, int maxn) {
    int n = 0;
    for (const char *p = s; *p && n < maxn; ) {
        out[n++] = atol(p);
        const char *c = strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }
    return n;
}

GeneModel *bed12_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    /* A bgzip+tabix BED (genes.bed.gz + .tbi) is queried by region; any other
     * path (incl. a plain .gz) is slurped whole and scanned. */
    char *buf;
    size_t pl = strlen(path);
    int have_tbi = 0;
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) {
        char tbi[4096]; snprintf(tbi, sizeof tbi, "%s.tbi", path);
        FILE *tf = fopen(tbi, "rb");
        if (tf) { fclose(tf); have_tbi = 1; }
    }
    buf = have_tbi ? tabix_slurp_region(path, chrom, rs, re, err) : slurp(path, err);
    if (!buf) return NULL;
    int cap = 64, cnt = 0;
    GeneModel *out = malloc(cap * sizeof *out);
    FOR_LINES(buf) {
        /* 12-col BED, or Gencode-style BED12+ (…, gene_name, gene_type,
         * transcript_name, …) — use transcript_name (col 16) as the label when
         * present so gene-symbol grouping works; else the BED name (col 4). */
        char *f[16]; int nf = split_tab(line, f, 16);
        if (nf < 12 || strcmp(f[0], chrom)) continue;
        long s = atol(f[1]), e = atol(f[2]);
        if (!overlaps(s, e, rs, re)) continue;
        if (cnt == cap) { cap *= 2; out = realloc(out, cap * sizeof *out); }
        GeneModel *g = &out[cnt];
        g->tx_start = s; g->tx_end = e;
        g->cds_start = atol(f[6]); g->cds_end = atol(f[7]);
        const char *nm = (nf >= 16) ? f[15] : f[3];
        g->name = strcmp(nm, ".") ? strdup(nm) : NULL;
        g->strand = f[5][0];
        long sizes[1024], starts[1024];
        int bc = atoi(f[9]);
        int ns = commalist(f[10], sizes, 1024), nst = commalist(f[11], starts, 1024);
        g->nexon = bc < ns ? bc : ns; if (nst < g->nexon) g->nexon = nst;
        g->exons = malloc(g->nexon * sizeof(Exon));
        for (int j = 0; j < g->nexon; j++) {
            g->exons[j].start = s + starts[j];
            g->exons[j].end = s + starts[j] + sizes[j];
        }
        cnt++;
    }
    free(buf); *n = cnt; return out;
}

Link *bedpe_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 64, cnt = 0;
    Link *out = malloc(cap * sizeof *out);
    FOR_LINES(buf) {
        char *f[10]; int nf = split_tab(line, f, 10);
        if (nf < 6) continue;
        long as = atol(f[1]), ae = atol(f[2]), bs = atol(f[4]), be = atol(f[5]);
        int aok = !strcmp(f[0], chrom) && overlaps(as, ae, rs, re);
        int bok = !strcmp(f[3], chrom) && overlaps(bs, be, rs, re);
        if (!aok && !bok) continue;
        if (cnt == cap) { cap *= 2; out = realloc(out, cap * sizeof *out); }
        out[cnt].a_start = as; out[cnt].a_end = ae;
        out[cnt].b_start = bs; out[cnt].b_end = be;
        out[cnt].score = nf > 7 ? atof(f[7]) : 0;
        cnt++;
    }
    free(buf); *n = cnt; return out;
}
