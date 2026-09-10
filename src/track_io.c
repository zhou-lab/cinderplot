/* track_io.c — tab-separated, headerless, region-filtered readers for the
 * locus track-browser (BED, bedGraph, BEDPE). Coordinates are BED-style
 * 0-based half-open and kept as-is. Only records overlapping the requested
 * region are loaded. Comment / `track` / `browser` lines are skipped. */
#include "cinderplot.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static char *slurp(const char *path, char *err) {
    if (!strcmp(path, "stdin")) path = "-";          /* alias for stdin */
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) return gz_read_all(path, err);
    FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!f) { snprintf(err, CP_ERRLEN, "cannot open %s", path); return NULL; }
    size_t cap = 1 << 16, n = 0, r;
    char *b = cp_xmalloc(cap);
    while ((r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r; if (n == cap) { cap *= 2; b = cp_xrealloc(b, cap); }
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
    /* Lines are cut at '\n', so a CRLF file leaves the '\r' on the last field,
     * where it rendered as a tofu glyph after a 4-column BED's name. */
    size_t L = strlen(f[n - 1]);
    if (L && f[n - 1][L - 1] == '\r') f[n - 1][L - 1] = 0;
    return n;
}
static int skip_line(const char *l) {
    return !*l || (*l == '\r' && !l[1]) || *l == '#'
        || !strncmp(l, "track", 5) || !strncmp(l, "browser", 7);
}
static int overlaps(long s, long e, long rs, long re) { return s < re && e > rs; }

/* A BED coordinate: digits only, no sign, fully consumed. atol() used to turn
 * `abc` into 0 and accept `-50`, which drew a box nobody asked for and said
 * nothing. Returns 0 when the text is not such a number. */
static int parse_coord(const char *s, long *out) {
    if (*s < '0' || *s > '9') return 0;
    char *end; errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno) return 0;
    *out = v; return 1;
}

/* Start/end of one record, or an error naming the file, line and field. */
static int parse_span(const char *path, int ln, const char *what, char **f,
                      int is, int ie, long *s, long *e, char *err) {
    if (!parse_coord(f[is], s)) {
        snprintf(err, CP_ERRLEN, "%s %s %d: column %d `%s` is not a coordinate "
                 "(a non-negative integer)", path, what, ln, is + 1, f[is]);
        return -1;
    }
    if (!parse_coord(f[ie], e)) {
        snprintf(err, CP_ERRLEN, "%s %s %d: column %d `%s` is not a coordinate "
                 "(a non-negative integer)", path, what, ln, ie + 1, f[ie]);
        return -1;
    }
    if (*s > *e) {
        snprintf(err, CP_ERRLEN, "%s %s %d: start %ld exceeds end %ld", path, what,
                 ln, *s, *e);
        return -1;
    }
    return 0;
}

/* Report a track that overlaps nothing in the window. An empty lane looks like
 * a finding (no peaks here) when it is usually a chromosome-name or
 * coordinate mismatch, so say so once per track and window. */
static void warn_empty(const char *path, const char *chrom, long rs, long re) {
    fprintf(stderr, "cinderplot: warning: %s: 0 records overlap %s:%ld-%ld\n",
            path, chrom, rs, re);
}

int region_parse(const char *s, char *chrom, long *start, long *end) {
    const char *colon = strchr(s, ':');
    const char *dash = colon ? strchr(colon, '-') : NULL;
    if (!colon || !dash) return -1;
    size_t n = colon - s;
    if (n == 0 || n >= 64) return -1;
    memcpy(chrom, s, n); chrom[n] = 0;
    /* Thousands commas are dropped; anything else that is not a digit fails,
     * so `chr1:-50-100` is refused instead of quietly becoming chr1:0-50. */
    char buf[32]; int j = 0;
    for (const char *q = colon + 1; q < dash && j < 31; q++) if (*q != ',') buf[j++] = *q;
    buf[j] = 0;
    if (!parse_coord(buf, start)) return -1;
    j = 0;
    for (const char *q = dash + 1; *q && j < 31; q++) if (*q != ',') buf[j++] = *q;
    buf[j] = 0;
    if (!parse_coord(buf, end)) return -1;
    return (*end > *start) ? 0 : -1;
}

/* line-iterator boilerplate shared by the readers; `ln` counts every line
 * (skipped ones included) so an error can name the one it came from */
#define FOR_LINES(buf, ln) \
    for (char *line = (buf), *nl; line && *line; line = nl, (ln)++) \
        if ((nl = strchr(line, '\n')) ? (*nl = 0, nl++, 0) : 0, !skip_line(line)) \

Interval *bed_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 64, cnt = 0;
    Interval *out = cp_xmalloc(cap * sizeof *out);
    int ln = 1;
    FOR_LINES(buf, ln) {
        char *f[12]; int nf = split_tab(line, f, 12);
        if (nf < 3) {
            snprintf(err, CP_ERRLEN, "%s line %d has %d column%s; interval() needs a BED "
                     "(chrom, start, end, ...)", path, ln, nf, nf == 1 ? "" : "s");
            free(buf); return NULL;
        }
        if (strcmp(f[0], chrom)) continue;
        long s, e;
        if (parse_span(path, ln, "line", f, 1, 2, &s, &e, err)) { free(buf); return NULL; }
        if (!overlaps(s, e, rs, re)) continue;
        if (cnt == cap) { cap *= 2; out = cp_xrealloc(out, cap * sizeof *out); }
        out[cnt].start = s; out[cnt].end = e;
        out[cnt].name = nf > 3 && strcmp(f[3], ".") ? cp_xstrdup(f[3]) : NULL;
        out[cnt].score = nf > 4 ? atof(f[4]) : 0;
        out[cnt].strand = nf > 5 ? f[5][0] : '.';
        cnt++;
    }
    free(buf); *n = cnt;
    if (!cnt) warn_empty(path, chrom, rs, re);
    return out;
}

SigBin *bedgraph_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 256, cnt = 0;
    SigBin *out = cp_xmalloc(cap * sizeof *out);
    int ln = 1;
    FOR_LINES(buf, ln) {
        char *f[5]; int nf = split_tab(line, f, 5);
        if (nf < 4) {
            snprintf(err, CP_ERRLEN, "%s line %d has %d column%s; coverage() needs a "
                     "bedGraph (chrom, start, end, value)", path, ln, nf, nf == 1 ? "" : "s");
            free(buf); return NULL;
        }
        if (strcmp(f[0], chrom)) continue;
        long s, e;
        if (parse_span(path, ln, "line", f, 1, 2, &s, &e, err)) { free(buf); return NULL; }
        /* The 4th column must be the signal. A BED's name column used to go
         * through atof() as 0 and draw an empty track. */
        char *vend; double v = strtod(f[3], &vend);
        if (vend == f[3] || *vend) {
            snprintf(err, CP_ERRLEN, "%s line %d: column 4 `%s` is not a number; "
                     "coverage() needs a bedGraph (chrom, start, end, value)", path, ln, f[3]);
            free(buf); return NULL;
        }
        if (!overlaps(s, e, rs, re)) continue;
        if (cnt == cap) { cap *= 2; out = cp_xrealloc(out, cap * sizeof *out); }
        out[cnt].start = s; out[cnt].end = e; out[cnt].val = v;
        cnt++;
    }
    free(buf); *n = cnt;
    if (!cnt) warn_empty(path, chrom, rs, re);
    return out;
}

/* parse a comma-separated list of non-negative longs (a trailing comma is
 * allowed, as BED writes it); returns count, or -1 on a token that is not one */
static int commalist(const char *s, long *out, int maxn) {
    int n = 0;
    for (const char *p = s; *p && n < maxn; ) {
        const char *c = strchr(p, ',');
        char tok[32]; size_t L = c ? (size_t)(c - p) : strlen(p);
        if (L >= sizeof tok) return -1;
        memcpy(tok, p, L); tok[L] = 0;
        if (!parse_coord(tok, &out[n])) return -1;
        n++;
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
    GeneModel *out = cp_xmalloc(cap * sizeof *out);
    /* a tabix query returns only the window's records, so its count is not a
     * file line number; say which it is */
    const char *what = have_tbi ? "record" : "line";
    int ln = 1;
    FOR_LINES(buf, ln) {
        /* 12-col BED, or Gencode-style BED12+ (…, gene_name, gene_type,
         * transcript_name, …) — use transcript_name (col 16) as the label when
         * present so gene-symbol grouping works; else the BED name (col 4). */
        char *f[16]; int nf = split_tab(line, f, 16);
        if (nf < 12) {
            snprintf(err, CP_ERRLEN, "%s %s %d has %d column%s; genes() needs a BED12 "
                     "(12 or more columns)", path, what, ln, nf, nf == 1 ? "" : "s");
            free(buf); return NULL;
        }
        if (strcmp(f[0], chrom)) continue;
        long s, e, cs, ce;
        if (parse_span(path, ln, what, f, 1, 2, &s, &e, err)
            || parse_span(path, ln, what, f, 6, 7, &cs, &ce, err)) { free(buf); return NULL; }
        if (!overlaps(s, e, rs, re)) continue;
        long sizes[1024], starts[1024], bc;
        int ns = commalist(f[10], sizes, 1024), nst = commalist(f[11], starts, 1024);
        if (!parse_coord(f[9], &bc) || ns < 0 || nst < 0) {
            snprintf(err, CP_ERRLEN, "%s %s %d: blockCount/blockSizes/blockStarts (columns "
                     "10-12) must be non-negative integers", path, what, ln);
            free(buf); return NULL;
        }
        if (bc != ns || bc != nst) {
            snprintf(err, CP_ERRLEN, "%s %s %d: blockCount is %ld but there are %d "
                     "blockSizes and %d blockStarts", path, what, ln, bc, ns, nst);
            free(buf); return NULL;
        }
        if (cnt == cap) { cap *= 2; out = cp_xrealloc(out, cap * sizeof *out); }
        GeneModel *g = &out[cnt];
        g->tx_start = s; g->tx_end = e;
        g->cds_start = cs; g->cds_end = ce;
        const char *nm = (nf >= 16) ? f[15] : f[3];
        g->name = strcmp(nm, ".") ? cp_xstrdup(nm) : NULL;
        g->strand = f[5][0];
        g->nexon = (int)bc;
        g->exons = cp_xmalloc(g->nexon * sizeof(Exon));
        for (int j = 0; j < g->nexon; j++) {
            g->exons[j].start = s + starts[j];
            g->exons[j].end = s + starts[j] + sizes[j];
            /* A block past chromEnd is a corrupt record, not a long exon. */
            if (g->exons[j].end > e) {
                snprintf(err, CP_ERRLEN, "%s %s %d: block %d ends at %ld, beyond "
                         "chromEnd %ld", path, what, ln, j + 1, g->exons[j].end, e);
                free(buf); return NULL;
            }
        }
        cnt++;
    }
    free(buf); *n = cnt;
    if (!cnt) warn_empty(path, chrom, rs, re);
    return out;
}

Link *bedpe_read(const char *path, const char *chrom, long rs, long re, int *n, char *err) {
    char *buf = slurp(path, err); if (!buf) return NULL;
    int cap = 64, cnt = 0;
    Link *out = cp_xmalloc(cap * sizeof *out);
    int ln = 1, ntrans = 0;
    FOR_LINES(buf, ln) {
        char *f[10]; int nf = split_tab(line, f, 10);
        if (nf < 6) {
            snprintf(err, CP_ERRLEN, "%s line %d has %d column%s; arcs() needs a BEDPE "
                     "(chrom1, start1, end1, chrom2, start2, end2, ...)", path, ln, nf,
                     nf == 1 ? "" : "s");
            free(buf); return NULL;
        }
        int achr = !strcmp(f[0], chrom), bchr = !strcmp(f[3], chrom);
        if (!achr && !bchr) continue;
        long as, ae, bs, be;
        if (parse_span(path, ln, "line", f, 1, 2, &as, &ae, err)
            || parse_span(path, ln, "line", f, 4, 5, &bs, &be, err)) { free(buf); return NULL; }
        int aok = achr && overlaps(as, ae, rs, re);
        int bok = bchr && overlaps(bs, be, rs, re);
        if (!aok && !bok) continue;
        /* An arc needs both feet on this chromosome: with one end elsewhere the
         * far coordinate is from another sequence and the arc would land it
         * here, on whatever happens to sit at that number. */
        if (!achr || !bchr) { ntrans++; continue; }
        if (cnt == cap) { cap *= 2; out = cp_xrealloc(out, cap * sizeof *out); }
        out[cnt].a_start = as; out[cnt].a_end = ae;
        out[cnt].b_start = bs; out[cnt].b_end = be;
        out[cnt].score = nf > 7 ? atof(f[7]) : 0;
        cnt++;
    }
    free(buf); *n = cnt;
    if (ntrans)
        fprintf(stderr, "cinderplot: warning: %s: skipped %d inter-chromosomal link%s\n",
                path, ntrans, ntrans == 1 ? "" : "s");
    if (!cnt) warn_empty(path, chrom, rs, re);
    return out;
}
