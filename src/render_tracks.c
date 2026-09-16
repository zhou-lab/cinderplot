/* render_tracks.c — locus track-browser mode: a stack of heterogeneous
 * tracks (coverage / interval / gene-model / arc / matrix / signal) sharing
 * one genomic x-axis over a single region. A third assembler alongside render.c
 * (grammar) and heatmap.c (matrix); the gtable engine is unchanged.
 *
 * Genomic bp/kb/Mb axis; coverage/interval/arc renderers fill each track
 * row cell. Gene models (BED12/GFF) land in M2.3. */
#include "cinderplot.h"
#include <cairo-pdf.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Col C_TGRID = {0.898, 0.898, 0.898};   /* faint track gridline */
static const Col C_COV   = {0.271, 0.459, 0.706};   /* steelblue coverage */
static const Col C_IVAL  = {0.35, 0.35, 0.35};      /* grey interval box */
static const Col C_ARC   = {0.5, 0.3, 0.6};         /* purple arc */

static int cmp_iv(const void *a, const void *b) {
    long d = ((const Interval *)a)->start - ((const Interval *)b)->start;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}
static int cmp_gene(const void *a, const void *b) {
    long d = ((const GeneModel *)a)->tx_start - ((const GeneModel *)b)->tx_start;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

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

/* auto-fit tuning (pt): matrix sample-row height when rownames are hidden vs
 * shown (a line-height multiplier), the per-weight-unit fallback when no matrix
 * anchors the height, the default region panel width, and overall clamps. */
#define AUTO_MIN_CELL 3.2
#define AUTO_ROW_PAD  1.18
#define AUTO_STRIP_LINES 3.0   /* signal(): label lines per strip, at least */
#define AUTO_UNIT_H   46.0
#define AUTO_PANEL_W  (5.0 * 72)
#define AUTO_MIN_PT   (2.0 * 72)
#define AUTO_MAX_PT   (30.0 * 72)

/* copy s into out, inserting thousands commas in the integer part (44620 -> 44,620) */
static void commafy(char *out, size_t cap, const char *s) {
    const char *dot = strchr(s, '.');
    int intlen = dot ? (int)(dot - s) : (int)strlen(s);
    int start = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    size_t o = 0;
    for (int i = 0; i < intlen && o < cap - 1; i++) {
        if (i > start && (intlen - i) % 3 == 0) out[o++] = ',';
        if (o < cap - 1) out[o++] = s[i];
    }
    for (const char *p = s + intlen; *p && o < cap - 1; p++) out[o++] = *p;
    out[o] = 0;
}

/* colour -> cairo ARGB32 pixel (native-endian 0xAARRGGBB, opaque) */
static uint32_t col_argb(Col c) {
    int r = (int)(c.r * 255 + 0.5), g = (int)(c.g * 255 + 0.5), b = (int)(c.b * 255 + 0.5);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Pivoted matrix for the anchored-heatmap track: sample rows x probe cols.
 * Reads long/tidy (`chrom beg end Probe_ID <value> <sample>`) — detected by a
 * string column beyond chrom/Probe_ID — or wide (one column per sample). The
 * input is assumed pre-filtered to the region. */
typedef struct {
    int nr, nc;
    double *mv;        /* nr x nc row-major, NaN = missing */
    double *colpos;    /* nc probe genomic positions (midpoints, sorted) */
    double *colbeg, *colend;   /* nc probe spans, for matrix(x=genomic): a cell
                                * drawn at its coordinate wants the probe's own
                                * width, which the midpoint alone cannot give */
    char **colid;      /* nc probe IDs (may be NULL entries) */
    char **rowname;    /* nr sample names */
    int *roword;       /* nr display order (cluster or identity) */
    char *chrom;       /* chromosome (borrowed or owned) — for region() inference */
    long gbeg, gend;   /* min(beg), max(end) over probes — for region() inference */
    int multichrom;    /* input spans >1 chromosome (inference is ambiguous) */
} MatData;

/* matrix(rowgroup="SEP"): the part of a sample name after the LAST separator
 * is what distinguishes the row; everything before it is the group. Returns the
 * leaf, and reports the group's length through *glen. With no separator in the
 * name the whole thing is the leaf and the group is empty, so an ungrouped row
 * simply keeps its label. */
static const char *row_leaf(const char *name, const char *sep, size_t *glen) {
    *glen = 0;
    if (!sep || !*sep || !name) return name;
    const char *hit = NULL, *p = name;
    size_t n = strlen(sep);
    while ((p = strstr(p, sep))) { hit = p; p += n; }
    if (!hit) return name;
    *glen = (size_t)(hit - name);
    return hit + n;
}

/* Return a chromosome cell as text, formatting numeric chromosomes such as 1.
 * The caller supplies scratch space because text columns remain borrowed. */
static const char *matrix_chrom_value(const Column *c, int r,
                                      char *buf, size_t cap) {
    if (!c) return NULL;
    if (c->type == COL_STR) return c->str[r];
    if (!isfinite(c->num[r])) return NULL;
    snprintf(buf, cap, "%.15g", c->num[r]);
    return buf;
}

static int matrix_in_window(const Column *chr_c, const Column *sc,
                            const Column *ec, int r, const char *win_chrom,
                            long win_beg, long win_end) {
    if (!win_chrom) return 1;
    char buf[64];
    const char *chrom = matrix_chrom_value(chr_c, r, buf, sizeof buf);
    return chrom && !strcmp(chrom, win_chrom)
           && ec->num[r] > (double)win_beg
           && sc->num[r] < (double)win_end;
}

/* `win_chrom`/`win_beg`/`win_end` restrict the matrix to the plotted window.
 * NULL win_chrom keeps every row, which is what region() inference needs --
 * there is no window yet at that point, the rows define it.
 *
 * Without this a matrix() track drew every probe in the file regardless of
 * region(): invisible for a single-locus file (every row is in the window,
 * which is all the gallery example ever exercised) and plainly wrong for a
 * multi-region one, where a 7-CpG window rendered all 236 columns. */
static MatData *read_matrix(const TrackObj *t, const char *win_chrom,
                            long win_beg, long win_end, char *err) {
    const char *path = strcmp(t->data, "stdin") == 0 ? "-" : t->data;   /* '-'/stdin = pipe */
    DataFrame *mf = df_read_csv(path, err);
    if (!mf) return NULL;
    const Column *chr_c = df_col(mf, "chrom");
    const Column *sc = df_col(mf, "beg"); if (!sc) sc = df_col(mf, "start");
    const Column *ec = df_col(mf, "end");
    const Column *pid = df_col(mf, "Probe_ID");
    if (!sc || !ec || sc->type != COL_NUM || ec->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "matrix needs numeric position columns (beg/start, end)"); return NULL;
    }
    /* Probe IDs as text. A numeric Probe_ID column (1001, 1002) has no str[]
     * and dereferencing it crashed; format it the way a numeric chrom is. The
     * formatted strings are owned here, and leak with mf as the borrowed ones
     * do (see the return). */
    char **pidstr = NULL;
    if (pid) {
        pidstr = cp_xmalloc((size_t)mf->nrow * sizeof(char *));
        for (int r = 0; r < mf->nrow; r++) {
            if (pid->type == COL_STR) { pidstr[r] = pid->str[r]; continue; }
            if (!isfinite(pid->num[r])) { pidstr[r] = NULL; continue; }
            char buf[64]; snprintf(buf, sizeof buf, "%.15g", pid->num[r]);
            pidstr[r] = cp_xstrdup(buf);
        }
    }
    /* Long vs wide. A text column beyond chrom/Probe_ID used to mean long,
     * so one `n/a` in a sample column of a wide file silently turned that
     * column into the sample NAMES and the next into the values. A sample
     * column holds names, so a text column with numeric cells in it is a
     * numeric column with a bad cell, and that cell is the error. */
    const Column *samp = NULL;                        /* name col => long/tidy */
    for (int c = 0; c < mf->ncol; c++) {
        const Column *col = &mf->cols[c];
        if (col->type != COL_STR || col == chr_c || col == pid) continue;
        int nnum = 0, bad = -1;
        for (int r = 0; r < mf->nrow; r++) {
            const char *v = col->str[r];
            if (!*v || !strcmp(v, "NA") || !strcmp(v, "na") || !strcmp(v, "NaN"))
                continue;                             /* the CSV reader's own NA spellings */
            char *e2; strtod(v, &e2);
            if (e2 != v && !*e2) nnum++;
            else if (bad < 0) bad = r;
        }
        if (nnum > 0) {
            snprintf(err, CP_ERRLEN, "matrix `%s`: column `%s` row %d is \"%s\", not a "
                     "number; sample and value columns must be numeric throughout "
                     "(write NA for a missing value)", t->data, col->name,
                     bad < 0 ? 1 : bad + 2, bad < 0 ? "" : col->str[bad]);
            return NULL;
        }
        if (!samp) samp = col;
    }
    MatData *m = cp_xcalloc(1, sizeof *m);
    int nr = 0, nc = 0;
    if (samp) {                                       /* --- long/tidy --- */
        if (!pid) { snprintf(err, CP_ERRLEN, "long matrix needs a Probe_ID column"); return NULL; }
        const Column *val = NULL;
        for (int c = 0; c < mf->ncol; c++)
            if (mf->cols[c].type == COL_NUM && &mf->cols[c] != chr_c
                && &mf->cols[c] != sc && &mf->cols[c] != ec && &mf->cols[c] != pid)
                { val = &mf->cols[c]; break; }
        if (!val) { snprintf(err, CP_ERRLEN, "long matrix needs a numeric value column"); return NULL; }
        char **pn = cp_xmalloc(mf->nrow * sizeof(char *));
        double *pp = cp_xmalloc(mf->nrow * sizeof(double));
        double *pb_ = cp_xmalloc(mf->nrow * sizeof(double));
        double *pe_ = cp_xmalloc(mf->nrow * sizeof(double));
        char **sn = cp_xmalloc(mf->nrow * sizeof(char *));
#define IN_WIN(r) matrix_in_window(chr_c, sc, ec, r, win_chrom, \
                                    win_beg, win_end)
        for (int r = 0; r < mf->nrow; r++) {
            if (!IN_WIN(r)) continue;
            const char *id = pidstr[r]; int f = -1;
            if (!id) {
                snprintf(err, CP_ERRLEN, "matrix `%s`: Probe_ID is empty at row %d",
                         t->data, r + 2);
                return NULL;
            }
            for (int i = 0; i < nc; i++) if (!strcmp(pn[i], id)) { f = i; break; }
            if (f < 0) { pn[nc] = (char *)id; pb_[nc] = sc->num[r]; pe_[nc] = ec->num[r];
                         pp[nc] = (sc->num[r] + ec->num[r]) * 0.5; nc++; }
            const char *s = samp->str[r]; f = -1;
            for (int i = 0; i < nr; i++) if (!strcmp(sn[i], s)) { f = i; break; }
            if (f < 0) sn[nr++] = (char *)s;
        }
        if (nc < 1 || nr < 1) {
            /* name the window and the file: under regions() this is one of
             * several windows and "matrix is empty" said nothing about which */
            if (win_chrom)
                snprintf(err, CP_ERRLEN, "matrix is empty in the requested window "
                         "%s:%ld-%ld (%s)", win_chrom, win_beg, win_end, t->data);
            else
                snprintf(err, CP_ERRLEN, "matrix `%s` is empty", t->data);
            return NULL;
        }
        int *ord = cp_xmalloc(nc * sizeof(int));
        for (int i = 0; i < nc; i++) ord[i] = i;
        for (int a = 1; a < nc; a++) { int k = ord[a]; double kp = pp[k]; int b2 = a - 1;
            while (b2 >= 0 && pp[ord[b2]] > kp) { ord[b2+1] = ord[b2]; b2--; } ord[b2+1] = k; }
        m->colpos = cp_xmalloc(nc * sizeof(double)); m->colid = cp_xmalloc(nc * sizeof(char *));
        m->colbeg = cp_xmalloc(nc * sizeof(double)); m->colend = cp_xmalloc(nc * sizeof(double));
        for (int c = 0; c < nc; c++) { m->colpos[c] = pp[ord[c]]; m->colid[c] = pn[ord[c]];
                                       m->colbeg[c] = pb_[ord[c]]; m->colend[c] = pe_[ord[c]]; }
        m->rowname = sn;
        m->mv = cp_xmalloc((size_t)nr * nc * sizeof(double));
        for (size_t i = 0; i < (size_t)nr * nc; i++) m->mv[i] = NAN;
        for (int r = 0; r < mf->nrow; r++) {
            if (!IN_WIN(r)) continue;
            const char *id = pidstr[r], *s = samp->str[r];
            int col = -1; for (int c = 0; c < nc; c++) if (!strcmp(m->colid[c], id)) { col = c; break; }
            int row = -1; for (int i = 0; i < nr; i++) if (!strcmp(m->rowname[i], s)) { row = i; break; }
            if (col >= 0 && row >= 0) m->mv[(size_t)row * nc + col] = val->num[r];
        }
        free(pn); free(pp); free(ord);
#undef IN_WIN
    } else {                                          /* --- wide --- */
        int scol[2048], ns = 0;
        for (int c = 0; c < mf->ncol && ns < 2048; c++)
            if (mf->cols[c].type == COL_NUM && &mf->cols[c] != chr_c
                && &mf->cols[c] != sc && &mf->cols[c] != ec && &mf->cols[c] != pid)
                scol[ns++] = c;
        if (ns < 1) { snprintf(err, CP_ERRLEN, "matrix `%s` has no numeric sample columns", t->data); return NULL; }
        if (mf->nrow < 1) { snprintf(err, CP_ERRLEN, "matrix `%s` is empty", t->data); return NULL; }
        int *ord = cp_xmalloc(mf->nrow * sizeof(int));
        double *pp = cp_xmalloc(mf->nrow * sizeof(double));
        for (int r = 0; r < mf->nrow; r++) {
            if (!matrix_in_window(chr_c, sc, ec, r, win_chrom,
                                  win_beg, win_end)) continue;
            pp[r] = (sc->num[r] + ec->num[r]) * 0.5;
            ord[nc++] = r;
        }
        if (nc < 1) {
            snprintf(err, CP_ERRLEN, "matrix is empty in the requested window "
                     "%s:%ld-%ld (%s)", win_chrom ? win_chrom : "", win_beg, win_end,
                     t->data);
            free(ord); free(pp);
            return NULL;
        }
        for (int a = 1; a < nc; a++) { int k = ord[a]; double kp = pp[k]; int b2 = a - 1;
            while (b2 >= 0 && pp[ord[b2]] > kp) { ord[b2+1] = ord[b2]; b2--; } ord[b2+1] = k; }
        nr = ns;
        m->colpos = cp_xmalloc(nc * sizeof(double)); m->colid = cp_xmalloc(nc * sizeof(char *));
        m->colbeg = cp_xmalloc(nc * sizeof(double)); m->colend = cp_xmalloc(nc * sizeof(double));
        for (int c = 0; c < nc; c++) { m->colpos[c] = pp[ord[c]]; m->colid[c] = pidstr ? pidstr[ord[c]] : NULL;
                                       m->colbeg[c] = sc->num[ord[c]]; m->colend[c] = ec->num[ord[c]]; }
        m->rowname = cp_xmalloc(nr * sizeof(char *));
        for (int r = 0; r < nr; r++) m->rowname[r] = mf->cols[scol[r]].name;
        m->mv = cp_xmalloc((size_t)nr * nc * sizeof(double));
        for (int r = 0; r < nr; r++)
            for (int c = 0; c < nc; c++)
                m->mv[(size_t)r * nc + c] = mf->cols[scol[r]].num[ord[c]];
        free(ord); free(pp);
    }
    m->nr = nr; m->nc = nc;
    /* genomic extent + chromosome, so region() can infer the window from here */
    if (mf->nrow > 0) {
        double lo = sc->num[0], hi = ec->num[0];
        for (int r = 1; r < mf->nrow; r++) {
            if (sc->num[r] < lo) lo = sc->num[r];
            if (ec->num[r] > hi) hi = ec->num[r];
        }
        m->gbeg = (long)lo; m->gend = (long)hi;
        if (chr_c) {
            char buf[64];
            const char *first = matrix_chrom_value(chr_c, 0, buf, sizeof buf);
            if (first) m->chrom = chr_c->type == COL_STR
                                ? (char *)first : cp_xstrdup(first);
            for (int r = 1; m->chrom && r < mf->nrow; r++) {
                char rbuf[64];
                const char *value = matrix_chrom_value(chr_c, r, rbuf,
                                                       sizeof rbuf);
                if (!value || strcmp(value, m->chrom)) {
                    m->multichrom = 1;
                    break;
                }
            }
        }
    }
    m->roword = cp_xmalloc(nr * sizeof(int));
    for (int r = 0; r < nr; r++) m->roword[r] = r;
    if (t->cluster && nr >= 2) {                      /* cluster the sample rows */
        char cerr[256]; HClust *h = hclust_ward(m->mv, nr, nc, cerr);
        if (h) memcpy(m->roword, h->order, nr * sizeof(int));
    }
    return m;                          /* mf leaked: colid/rowname borrow from it */
}

/* gene symbol = transcript name up to the last '-' (e.g. "PKIG-206" -> "PKIG") */
static void gene_symbol(const char *name, char *out, size_t cap) {
    if (!name) { out[0] = 0; return; }
    const char *dash = strrchr(name, '-');
    size_t n = dash ? (size_t)(dash - name) : strlen(name);
    if (n >= cap) n = cap - 1;
    memcpy(out, name, n); out[n] = 0;
}

/* read a gene track's BED12, sort by tx_start; by default keep one transcript per
 * gene (the longest; ties -> first) relabelled with the gene symbol. all=1 keeps
 * every isoform with its transcript name. Fresh array; ng_out set. */
static GeneModel *load_genes(const char *data, const char *chrom, long rs, long re,
                             int all, int *ng_out, char *err) {
    int ng; GeneModel *gm = bed12_read(data, chrom, rs, re, &ng, err);
    if (!gm) return NULL;
    qsort(gm, ng, sizeof *gm, cmp_gene);
    if (!all && ng > 0) {
        int m2 = 0;
        for (int k = 0; k < ng; k++) {
            /* BED name `.` means there is no symbol to group or draw. */
            if (!gm[k].name) { gm[m2++] = gm[k]; continue; }
            char sk[128]; gene_symbol(gm[k].name, sk, sizeof sk);
            long lk = gm[k].tx_end - gm[k].tx_start;
            int canon = 1;
            for (int j = 0; j < ng && canon; j++) {
                if (j == k || !gm[j].name) continue;
                char sj[128]; gene_symbol(gm[j].name, sj, sizeof sj);
                if (strcmp(sj, sk)) continue;
                long lj = gm[j].tx_end - gm[j].tx_start;
                if (lj > lk || (lj == lk && j < k)) canon = 0;
            }
            if (canon) { gm[k].name = cp_xstrdup(sk); gm[m2++] = gm[k]; }
        }
        ng = m2;
    }
    *ng_out = ng;
    return gm;
}


/* A start/end cell, whether the column typed numeric or (because of a header
 * row) text. Returns 0 when the cell is not a number. */
static int cell_num(const Column *c, int r, double *out) {
    if (c->type == COL_NUM) {
        if (isnan(c->num[r])) return 0;
        *out = c->num[r]; return 1;
    }
    char *end; double v = strtod(c->str[r], &end);
    if (end == c->str[r] || *end) return 0;
    *out = v; return 1;
}

/* One plotted window. regions() yields several; region() yields one. */
typedef struct { char chrom[64]; long beg, end; char *label; } Window;

/* Load regions("windows.bed"): chrom, start, end and an optional name column.
 * Windows are drawn in file order, each into its own column of the layout, so
 * the file is also the left-to-right ordering. */
static Window *load_windows(const char *path, int *n_out, char *err) {
    /* A BED has no header line, so read it headerless -- otherwise the first
     * region is silently consumed as column names and the figure quietly loses
     * a window. A file that does carry a header still works: its start/end
     * cells are not numbers, and that row is dropped below. */
    cp_set_no_header(1);
    DataFrame *df = df_read_csv(path, err);
    cp_set_no_header(0);
    if (!df) return NULL;
    if (df->ncol < 3) {
        snprintf(err, CP_ERRLEN, "regions `%s`: expected a headerless BED with at least chrom, start, end columns (an optional 4th column names the panel)", path);
        return NULL;
    }
    const Column *c0 = &df->cols[0], *c1 = &df->cols[1], *c2 = &df->cols[2];
    /* A numeric chromosome column (Ensembl-style 1, 2, X) is formatted as
     * text, as region("1:...") and matrix() already accept it. */
    /* start/end are read per cell rather than per column, because a header row
     * makes the whole column type as text and a column-level check would reject
     * the file outright -- which is exactly what a user gets for writing the
     * header the old error message implied. A row whose start or end does not
     * parse is skipped, so a header is simply ignored. */
    const Column *cn = df->ncol > 3 ? &df->cols[3] : NULL;
    if (df->nrow < 1) { snprintf(err, CP_ERRLEN, "regions `%s` is empty", path); return NULL; }
    Window *w = cp_xcalloc(df->nrow, sizeof(Window));
    int n = 0, skipped = 0;
    for (int r = 0; r < df->nrow; r++) {
        double b, e;
        if (!cell_num(c1, r, &b) || !cell_num(c2, r, &e)) { skipped++; continue; }
        if (e <= b) {
            snprintf(err, CP_ERRLEN, "regions `%s` row %d: end must exceed start",
                     path, r + 1);
            return NULL;
        }
        const char *cv = matrix_chrom_value(c0, r, w[n].chrom, sizeof w[n].chrom);
        if (!cv) {
            snprintf(err, CP_ERRLEN, "regions `%s` row %d: chromosome is empty", path, r + 1);
            return NULL;
        }
        if (cv != w[n].chrom) snprintf(w[n].chrom, sizeof w[n].chrom, "%s", cv);
        if (b < 0 || e < 0 || b != floor(b) || e != floor(e)) {
            snprintf(err, CP_ERRLEN, "regions `%s` row %d: start and end must be "
                     "non-negative integers", path, r + 1);
            return NULL;
        }
        w[n].beg = (long)b; w[n].end = (long)e;
        w[n].label = cn && cn->type == COL_STR ? cn->str[r] : NULL;
        n++;
    }
    if (n < 1) {
        snprintf(err, CP_ERRLEN, "regions `%s`: no rows with a numeric start and end; "
                 "expected a BED (chrom, start, end[, name])", path);
        return NULL;
    }
    if (skipped > 1)
        fprintf(stderr, "cinderplot: regions `%s`: skipped %d rows without numeric "
                "coordinates\n", path, skipped);
    *n_out = n;
    return w;
}

#define GAP_PT 10.0   /* whitespace between non-contiguous windows */


/* Rebuild `m` on `ref`'s rows, so sample i means the same thing in every window.
 *
 * Each window reads the matrix independently, so it knows only the samples that
 * have a probe inside it. Re-ordering what is present is not enough: a long
 * matrix missing one sample in one window yields fewer rows there, those rows
 * are spread over the same track height, and the row-label gutter -- written
 * once, by the left-most window -- then names the wrong band. Equal heights
 * would mean different samples, which is the one thing a shared axis promises.
 *
 * So take the reference row set wholesale and fill an absent sample with NaN;
 * the heatmap already paints a NaN cell in the missing-value colour, and every
 * band keeps its position. `ref` is the unfiltered pre-read, so its samples are
 * a superset of any window's and nothing is dropped. */
static void align_rows(MatData *m, const MatData *ref) {
    int nr = ref->nr, nc = m->nc;
    double *mv = cp_xmalloc((size_t)nr * nc * sizeof(double));
    char **rowname = cp_xmalloc(nr * sizeof(char *));
    for (int i = 0; i < nr; i++) {
        rowname[i] = ref->rowname[i];
        int src = -1;
        for (int j = 0; j < m->nr; j++)
            if (!strcmp(m->rowname[j], ref->rowname[i])) { src = j; break; }
        for (int c = 0; c < nc; c++)
            mv[(size_t)i * nc + c] = src >= 0 ? m->mv[(size_t)src * nc + c]
                                              : NAN;
    }
    int *roword = cp_xmalloc(nr * sizeof(int));
    memcpy(roword, ref->roword, nr * sizeof(int));
    free(m->mv); free(m->rowname); free(m->roword);   /* the name strings are
                                                       * borrowed; only the
                                                       * arrays are ours */
    m->mv = mv; m->rowname = rowname; m->roword = roword; m->nr = nr;
}

/* ---- highlight() boxes on a matrix() track: one sample row over a genomic
 * span. Spec-level calls and the file form are flattened into one list up
 * front; each window then draws the boxes that fall inside it. ---- */
typedef struct {
    const char *row, *target, *label;
    char chrom[64]; long beg, end;
    Col col; int dash;
    int placed;                    /* drawn in at least one window */
} TBox;

static int trk_boxes_load(const PlotSpec *spec, TBox **out, int *n_out, char *err) {
    int n = 0, cap = 0;
    TBox *bx = NULL;
    for (int i = 0; i < spec->nhls; i++) {
        const CellHighlight *h = &spec->hls[i];
        if (!h->file) {
            if (n == cap) bx = cp_xrealloc(bx, (cap = cap ? 2 * cap : 16) * sizeof *bx);
            TBox *b = &bx[n++];
            memset(b, 0, sizeof *b);
            b->row = h->row; b->target = h->target; b->label = h->label;
            snprintf(b->chrom, sizeof b->chrom, "%s", h->chrom);
            b->beg = h->beg; b->end = h->end; b->col = h->color; b->dash = h->dash;
            continue;
        }
        DataFrame *df = df_read_csv(h->file, err);
        if (!df) return -1;
        const Column *rc = df_col(df, "row"), *cc = df_col(df, "chrom");
        const Column *bc = df_col(df, "beg"), *ec = df_col(df, "end");
        const Column *kc = df_col(df, "colour"); if (!kc) kc = df_col(df, "color");
        const Column *lc = df_col(df, "label"), *tc = df_col(df, "linetype");
        if (!rc || !cc || !bc || !ec) {
            snprintf(err, CP_ERRLEN, "highlight(\"%s\"): needs columns row, chrom, beg, end "
                     "(optional: colour, label, linetype)", h->file);
            return -1;
        }
        if (bc->type != COL_NUM || ec->type != COL_NUM) {
            snprintf(err, CP_ERRLEN, "highlight(\"%s\"): beg/end must be numeric", h->file);
            return -1;
        }
        char scratch[64];
        for (int r = 0; r < df->nrow; r++) {
            if (n == cap) bx = cp_xrealloc(bx, (cap = cap ? 2 * cap : 16) * sizeof *bx);
            TBox *b = &bx[n++];
            memset(b, 0, sizeof *b);
            b->target = h->target;
            b->row = rc->type == COL_STR ? rc->str[r] : NULL;
            if (!b->row) {                  /* a numeric sample name column */
                char *t = cp_xmalloc(32);
                snprintf(t, 32, "%.15g", rc->num[r]);
                b->row = t;
            }
            snprintf(b->chrom, sizeof b->chrom, "%s", matrix_chrom_value(cc, r, scratch, sizeof scratch));
            if (isnan(bc->num[r]) || isnan(ec->num[r]) || bc->num[r] < 0 || ec->num[r] <= bc->num[r]) {
                snprintf(err, CP_ERRLEN, "highlight(\"%s\"): row %d has beg/end %g-%g "
                         "(need 0 <= beg < end)", h->file, r + 2, bc->num[r], ec->num[r]);
                return -1;
            }
            b->beg = (long)bc->num[r]; b->end = (long)ec->num[r];
            b->col = h->color;
            if (kc && kc->type == COL_STR && kc->str[r] && *kc->str[r]
                && parse_color(kc->str[r], &b->col)) {
                snprintf(err, CP_ERRLEN, "highlight(\"%s\"): row %d colour `%s` invalid "
                         "(names or #RRGGBB)", h->file, r + 2, kc->str[r]);
                return -1;
            }
            if (lc && lc->type == COL_STR && lc->str[r] && *lc->str[r]) b->label = lc->str[r];
            else if (lc && lc->type == COL_NUM && !isnan(lc->num[r])) {
                char *t = cp_xmalloc(32); snprintf(t, 32, "%.15g", lc->num[r]); b->label = t;
            }
            if (tc && tc->type == COL_STR && tc->str[r] && *tc->str[r]) {
                if (!strcmp(tc->str[r], "solid")) b->dash = 0;
                else if (!strcmp(tc->str[r], "dashed")) b->dash = 1;
                else if (!strcmp(tc->str[r], "dotted")) b->dash = 2;
                else {
                    snprintf(err, CP_ERRLEN, "highlight(\"%s\"): row %d linetype `%s` "
                             "(solid, dashed or dotted)", h->file, r + 2, tc->str[r]);
                    return -1;
                }
            }
        }
    }
    *out = bx; *n_out = n;
    return 0;
}

/* ---- matrix(rowcolour="file.tsv"): the colour table. Two shapes, chosen by
 * header:
 *   - `group colour` (with rowgroup=): one colour per group name; the group in
 *     the gutter is drawn in it with a swatch beside it. `column` stays NULL.
 *   - `column value colour` (with rowmeta=): a colour per (annotation column,
 *     value), so a cell_type band and a source band draw from one table.
 * A (column,value) the sheet form does not name gets the hue palette. A group
 * the 2-column form does not name is left black -- and with rowbar=on gets no
 * band, so the strip shows a gap there. A colour that does not parse is an
 * error, naming the row. ---- */
typedef struct { const char *column; const char *value; Col col; } RowCol;

/* swatch beside a coloured group name: a square about the x-height of the
 * label, and the gap it keeps from the name */
#define SWATCH_PT 5.0

static int trk_rowcolours_load(const TrackObj *t, RowCol **out, int *n_out, char *err) {
    DataFrame *df = df_read_csv(t->rowcolour, err);
    if (!df) return -1;
    const Column *kc = df_col(df, "colour"); if (!kc) kc = df_col(df, "color");
    const Column *colc = df_col(df, "column"), *valc = df_col(df, "value");
    const Column *gc = df_col(df, "group");
    int three = colc && valc && kc;          /* `column value colour` */
    int two = gc && kc;                      /* `group colour` */
    if (t->rowmeta && !three) {
        snprintf(err, CP_ERRLEN, "rowcolour(\"%s\"): with rowmeta= this table maps a "
                 "value in an annotation column to a colour, so it needs columns "
                 "column, value and colour", t->rowcolour);
        return -1;
    }
    if (!t->rowmeta && !two) {
        snprintf(err, CP_ERRLEN, "rowcolour(\"%s\"): needs columns group and colour "
                 "(one line per rowgroup= name)", t->rowcolour);
        return -1;
    }
    if (kc->type != COL_STR) {
        snprintf(err, CP_ERRLEN, "rowcolour(\"%s\"): column `colour` must be text "
                 "(names or #RRGGBB)", t->rowcolour);
        return -1;
    }
    RowCol *rc = cp_xcalloc(df->nrow > 0 ? df->nrow : 1, sizeof *rc);
    int n = 0;
    for (int r = 0; r < df->nrow; r++) {
        const char *column = NULL, *value;
        if (three) {
            column = colc->type == COL_STR ? colc->str[r] : NULL;
            value  = valc->type == COL_STR ? valc->str[r] : NULL;
            if (!value) {                    /* a numeric value column */
                char *tmp = cp_xmalloc(32);
                snprintf(tmp, 32, "%.15g", valc->num[r]);
                value = tmp;
            }
            if (!column || !*column || !*value) continue;
        } else {
            value = gc->type == COL_STR ? gc->str[r] : NULL;
            if (!value) {                    /* a numeric group column */
                char *tmp = cp_xmalloc(32);
                snprintf(tmp, 32, "%.15g", gc->num[r]);
                value = tmp;
            }
            if (!*value) continue;
        }
        if (!kc->str[r] || !*kc->str[r] || parse_color(kc->str[r], &rc[n].col)) {
            snprintf(err, CP_ERRLEN, "rowcolour(\"%s\"): row %d (%s`%s`) colour `%s` "
                     "invalid (names or #RRGGBB)", t->rowcolour, r + 2,
                     column ? "value " : "group ", value, kc->str[r] ? kc->str[r] : "");
            return -1;
        }
        rc[n].column = column; rc[n].value = value; n++;
    }
    *out = rc; *n_out = n;
    return 0;
}

/* the colour for a group name of length gl (the rowgroup= path: only the
 * 2-column `group colour` entries), or NULL when the file does not name it */
static const Col *rowcolour_of(const RowCol *rc, int nrc, const char *name, size_t gl) {
    for (int k = 0; k < nrc; k++)
        if (!rc[k].column && strlen(rc[k].value) == gl && !strncmp(rc[k].value, name, gl))
            return &rc[k].col;
    return NULL;
}

/* the colour a (column, value) pair is named in the 3-column table, else NULL */
static const Col *rowcolour_lookup(const RowCol *rc, int nrc, const char *column,
                                   const char *value) {
    for (int k = 0; k < nrc; k++)
        if (rc[k].column && !strcmp(rc[k].column, column) && !strcmp(rc[k].value, value))
            return &rc[k].col;
    return NULL;
}

/* ---- matrix/signal(rowmeta=, rowbar="col,col2"): the metadata bands. For each
 * band (a rowbar= column) and each sample (indexed as in the track's name
 * array) the annotation value string and its resolved colour. Built once per
 * track; draw_row_gutter reads it. ---- */
typedef struct {
    int nband;
    char **bandcol;     /* [nband] rowmeta column name (borrowed from t->rowbar_cols) */
    char ***val;        /* [nband][nsamp] value string (borrowed from the sheet) */
    Col **col;          /* [nband][nsamp] resolved colour */
    int nsamp;
} RowAnno;

/* smallest a shrink-to-fit label is allowed to reach before it is dropped */
#define LABEL_MIN_PT 5.5

/* The size at which `s` fits in `avail_pt`, shrinking from `size` toward
 * LABEL_MIN_PT; 0 when it will not fit even at the floor (caller drops it).
 * Cairo advance widths scale linearly with the font size, so one division
 * lands the fit; the floor is re-checked defensively. */
static double fit_width(cairo_t *cr, double size, const char *s, double avail_pt) {
    if (avail_pt <= 0) return 0;
    double w = text_w(cr, size, s);
    if (w <= avail_pt) return size;
    double shrunk = size * avail_pt / w;
    if (shrunk >= LABEL_MIN_PT) return shrunk;
    return text_w(cr, LABEL_MIN_PT, s) <= avail_pt ? LABEL_MIN_PT : 0;
}

/* leaf/row labels shrink to their row's height but are never dropped. The
 * floor never exceeds the requested size: at --font-size 5 the track size is
 * 4.1pt, and "shrinking" a cramped label up to the 5.5pt floor would make it
 * larger than asked and overlap its neighbours -- the one thing this exists
 * to prevent. */
static double fit_height(cairo_t *cr, double size, double avail_pt) {
    if (avail_pt <= 0) return size;
    double h = font_h(cr, size);
    if (h <= avail_pt) return size;
    double shrunk = size * avail_pt / h;
    double floor_pt = size < LABEL_MIN_PT ? size : LABEL_MIN_PT;
    return shrunk < floor_pt ? floor_pt : shrunk;
}

static RowAnno *trk_rowmeta_load(const TrackObj *t, char **names, const int *ord,
                                 int nsamp, const RowCol *rc, int nrc, char *err) {
    DataFrame *df = df_read_csv(t->rowmeta, err);
    if (!df) return NULL;
    if (df->ncol < 2) {
        snprintf(err, CP_ERRLEN, "rowmeta(\"%s\"): needs a sample-key column and at "
                 "least one annotation column", t->rowmeta);
        return NULL;
    }
    const Column *keyc = &df->cols[0];
    char kbuf[64];
    int *mrow = cp_xmalloc((size_t)(nsamp > 0 ? nsamp : 1) * sizeof(int));
    for (int s = 0; s < nsamp; s++) {              /* every drawn sample must be present */
        int hit = -1;
        for (int r = 0; r < df->nrow; r++) {
            const char *k = keyc->type == COL_STR ? keyc->str[r]
                          : (snprintf(kbuf, sizeof kbuf, "%.15g", keyc->num[r]), kbuf);
            if (k && !strcmp(k, names[s])) { hit = r; break; }
        }
        if (hit < 0) {
            snprintf(err, CP_ERRLEN, "rowmeta(\"%s\"): sample \"%s\" is not in the sheet "
                     "(its first column, `%s`, is the sample key)", t->rowmeta,
                     names[s], keyc->name ? keyc->name : "sample");
            return NULL;
        }
        mrow[s] = hit;
    }
    RowAnno *a = cp_xcalloc(1, sizeof *a);
    a->nband = t->n_rowbar_cols; a->nsamp = nsamp;
    a->bandcol = cp_xmalloc((size_t)a->nband * sizeof(char *));
    a->val = cp_xmalloc((size_t)a->nband * sizeof(char **));
    a->col = cp_xmalloc((size_t)a->nband * sizeof(Col *));
    for (int b = 0; b < a->nband; b++) {
        const char *cn = t->rowbar_cols[b];
        const Column *c = df_col(df, cn);
        if (!c) {
            char cols[CP_ERRLEN]; size_t o = 0; cols[0] = 0;
            for (int j = 0; j < df->ncol && o < sizeof cols - 2; j++)
                o += (size_t)snprintf(cols + o, sizeof cols - o, "%s%s",
                                      j ? ", " : "", df->cols[j].name ? df->cols[j].name : "?");
            snprintf(err, CP_ERRLEN, "rowbar column \"%s\" is not in rowmeta(\"%s\") "
                     "(columns: %s)", cn, t->rowmeta, cols);
            return NULL;
        }
        a->bandcol[b] = (char *)cn;
        a->val[b] = cp_xmalloc((size_t)(nsamp > 0 ? nsamp : 1) * sizeof(char *));
        a->col[b] = cp_xmalloc((size_t)(nsamp > 0 ? nsamp : 1) * sizeof(Col));
        char vbuf[64];
        for (int s = 0; s < nsamp; s++) {
            const char *v = c->type == COL_STR ? c->str[mrow[s]]
                          : (snprintf(vbuf, sizeof vbuf, "%.15g", c->num[mrow[s]]),
                             cp_xstrdup(vbuf));
            a->val[b][s] = (char *)(v ? v : "");
        }
        /* listed (column,value) pairs keep their table colour; the rest get the
         * hue palette over this column's distinct values in first-appearance
         * (display) order. */
        char **unlisted = cp_xmalloc((size_t)(nsamp > 0 ? nsamp : 1) * sizeof(char *));
        int nunl = 0;
        for (int rr = 0; rr < nsamp; rr++) {
            const char *v = a->val[b][ord ? ord[rr] : rr];
            if (rowcolour_lookup(rc, nrc, cn, v)) continue;
            int seen = 0;
            for (int u = 0; u < nunl; u++) if (!strcmp(unlisted[u], v)) { seen = 1; break; }
            if (!seen) unlisted[nunl++] = (char *)v;
        }
        Col *hue = cp_xmalloc((size_t)(nunl > 0 ? nunl : 1) * sizeof(Col));
        hue_palette(nunl, hue);
        for (int s = 0; s < nsamp; s++) {
            const Col *lc = rowcolour_lookup(rc, nrc, cn, a->val[b][s]);
            if (lc) { a->col[b][s] = *lc; continue; }
            int hi = 0;
            for (int u = 0; u < nunl; u++) if (!strcmp(unlisted[u], a->val[b][s])) { hi = u; break; }
            a->col[b][s] = hue[hi];
        }
    }
    return a;
}

/* ---- The row-label gutter matrix() and signal() share: a leaf label per
 * row, and with rowgroup= the group name once beside each run of consecutive
 * rows, its rowcolour= swatch, and a rule between one run and the next. ---- */

/* a band column is ROWBAR_PT wide against the panel edge */
#define ROWBAR_PT 6.0

/* the width (pt) of the band strip against the panel: metadata bands
 * (rowbar="col,col2") reserve one ROWBAR_PT column per band with a gap between,
 * the single rowbar=on band reserves one, and a plain rowcolour= reserves none
 * (its swatch sits beside the group name, counted with the label widths). */
static double rowbar_strip_pt(const TrackObj *t, const RowAnno *ann) {
    int nband = ann ? ann->nband : ((t->rowbar && t->rowcolour) ? 1 : 0);
    if (nband <= 0) return 0;
    return nband * ROWBAR_PT + (nband - 1) * TXT_GAP + TXT_GAP * 2;
}

/* the gutter width a track's rows need: the band strip (always, so hidden row
 * names still leave room for the bands), plus -- when the labels are shown --
 * the leaf labels, the group name left of them, and a plain rowcolour= swatch. */
static double measure_gutter(cairo_t *cr, double sz, const TrackObj *t,
                             const RowAnno *ann, char **names, int n, int show_labels) {
    double w = rowbar_strip_pt(t, ann);
    if (!show_labels) return w;
    double leafw = 0, grpw = 0;
    int meta = ann && ann->nband > 0;          /* metadata mode: no leaf labels */
    for (int r = 0; !meta && r < n; r++) {
        size_t gl = 0;
        const char *leaf = row_leaf(names[r], t->rowgroup, &gl);
        double lw = text_w(cr, sz, leaf);
        if (lw > leafw) leafw = lw;
        if (gl) {
            char g8[256];
            snprintf(g8, sizeof g8, "%.*s", (int)(gl < sizeof g8 ? gl : sizeof g8 - 1), names[r]);
            double gw = text_w(cr, sz, g8);
            if (gw > grpw) grpw = gw;
        }
    }
    if (ann && ann->nband > 0)                 /* group label = the first band's value */
        for (int s = 0; s < ann->nsamp; s++) {
            double gw = text_w(cr, sz, ann->val[0][s]);
            if (gw > grpw) grpw = gw;
        }
    w += leafw + (grpw > 0 ? grpw + TXT_GAP * 2 : 0);
    if (!ann && grpw > 0 && t->rowcolour && !t->rowbar)
        w += SWATCH_PT + TXT_GAP;              /* swatch beside a plain rowcolour name */
    return w;
}

/* Rows rr = 0..n-1 run top to bottom over the band [top - hh, top] (npc, y
 * up) of track row R; names[ord[rr]] is row rr's name. Labels are written
 * only when write_labels (the left-most window, rownames shown): the gutter
 * is shared by every window, and each would otherwise stamp the same names
 * over the last. The rules between runs are per window, in column CC.
 *
 * Three annotation shapes share this:
 *   - rowmeta=/rowbar="col,col2" (ann != NULL): one filled band per column
 *     against the panel edge (leftmost = first in the list), the group label
 *     and the run rules from the FIRST band's column;
 *   - rowbar=on + rowgroup= (band-only): one band per run, no swatch;
 *   - plain rowcolour= + rowgroup=: the coloured group name with a swatch.
 *
 * Runs are CONSECUTIVE rows equal in the grouping value -- the display order
 * decides them, so a hand-ordered or clustered file groups exactly as the
 * reader sees it, and a scattered value legitimately shows up as several runs
 * rather than being merged behind the reader's back. */
static void draw_row_gutter(GTable *T, cairo_t *cr, int R, int CC, const TrackObj *t,
                            const RowCol *rc, int nrc, const RowAnno *ann,
                            char **names, const int *ord,
                            int n, double top, double hh, double labw, double cell_pt,
                            double sz, int write_labels, int first_win) {
    Grob *g;
    if (n < 1) return;
    double strip = rowbar_strip_pt(t, ann);          /* band column(s) against the panel */
    double bshift = labw > 0 ? strip / labw : 0;
    double rowh_pt = cell_pt > 0 ? hh * cell_pt / n : 0;   /* one row's height */
    double leafw = 0;                                /* widest leaf label, for group x */
    for (int rr = 0; !(ann && ann->nband > 0) && rr < n; rr++) {
        size_t gl = 0;
        const char *leaf = row_leaf(names[ord[rr]], t->rowgroup, &gl);
        double w = text_w(cr, sz, leaf);
        if (w > leafw) leafw = w;
    }
    /* per-row leaf labels, shrunk to their row height so a tall label in a
     * short row does not overlap its neighbours (shrunk, never dropped). In
     * metadata mode (rowmeta=) the grouping is the annotation, not the row
     * name -- the leaves are "truth" and invisible reconstruction markers, a
     * smear -- so only the coloured group label below is drawn. */
    if (write_labels && !(ann && ann->nband > 0))
        for (int rr = 0; rr < n; rr++) {
            size_t gl = 0;
            const char *leaf = row_leaf(names[ord[rr]], t->rowgroup, &gl);
            g = gt_add(T, G_TEXT, R, 1, R, 1);
            g->str = (char *)leaf; g->size = fit_height(cr, sz, rowh_pt); g->col = C_BLACK;
            g->tx = 1 - bshift; g->ty = top - (rr + 0.5) / n * hh; g->hj = 1; g->va = V_INKCENTER;
        }
    double gx = labw > 0 ? 1 - bshift - (leafw + TXT_GAP * 2) / labw : 0;

    /* ---- metadata bands (rowmeta=, rowbar="col,col2") ---- */
    if (ann && ann->nband > 0) {
        /* the bands live in the shared gutter column, so one window stamps
         * them (regions() would otherwise draw N identical copies); they are
         * drawn whether or not row names are, since measure_gutter reserves
         * the strip either way */
        if (labw > 0 && first_win)
            for (int b = 0; b < ann->nband; b++) {
                /* band 0 (leftmost of the strip) furthest from the panel; the
                 * last band butts the panel edge */
                int bi = ann->nband - 1 - b;
                double bx1 = 1 - (double)bi * (ROWBAR_PT + TXT_GAP) / labw;
                double bx0 = bx1 - ROWBAR_PT / labw;
                for (int rr = 0; rr < n; rr++) {
                    g = gt_add(T, G_RECT, R, 1, R, 1);
                    g->col = ann->col[b][ord[rr]]; g->sub = 1;
                    g->x0 = bx0; g->x1 = bx1;
                    g->y0 = top - (double)(rr + 1) / n * hh;
                    g->y1 = top - (double)rr / n * hh;
                }
            }
        int rs = 0;                                  /* runs on the FIRST band's column */
        while (rs < n) {
            const char *gv = ann->val[0][ord[rs]];
            int re = rs;
            while (re + 1 < n && !strcmp(ann->val[0][ord[re + 1]], gv)) re++;
            if (write_labels) {
                double gy = top - (rs + re + 1) / 2.0 / n * hh;
                g = gt_add(T, G_TEXT, R, 1, R, 1);
                g->str = (char *)gv; g->size = sz; g->col = ann->col[0][ord[rs]];
                g->tx = gx; g->ty = gy; g->hj = 1; g->va = V_INKCENTER;
            }
            if (re + 1 < n) {                        /* rule below this run */
                g = gt_add(T, G_LINE, R, CC, R, CC);
                g->col = C_BLACK; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
                g->x0 = 0; g->x1 = 1;
                g->y0 = g->y1 = top - (double)(re + 1) / n * hh;
            }
            rs = re + 1;
        }
        return;
    }

    /* ---- rowgroup= path: one grouping value split out of the sample name ---- */
    if (!t->rowgroup) return;
    int bar = t->rowbar && t->rowcolour && labw > 0;   /* band-only when rowbar=on */
    int rs = 0;
    while (rs < n) {
        size_t gl = 0;
        const char *nm = names[ord[rs]];
        row_leaf(nm, t->rowgroup, &gl);
        int re = rs;
        while (re + 1 < n) {
            size_t gl2 = 0;
            const char *nm2 = names[ord[re + 1]];
            row_leaf(nm2, t->rowgroup, &gl2);
            if (gl2 != gl || (gl && strncmp(nm, nm2, gl))) break;
            re++;
        }
        const Col *gc = gl ? rowcolour_of(rc, nrc, nm, gl) : NULL;
        /* the band is gutter, not label: measure_gutter reserves its strip
         * whether or not row names are shown, so it is drawn on rownames=off
         * too, once, from the left-most window. The name and swatch are labels. */
        if (gl && bar && gc && labw > 0 && cell_pt > 0 && first_win) {
            g = gt_add(T, G_RECT, R, 1, R, 1);
            g->col = *gc; g->sub = 1;
            g->x1 = 1; g->x0 = 1 - ROWBAR_PT / labw;
            g->y0 = top - (double)(re + 1) / n * hh;
            g->y1 = top - (double)rs / n * hh;
        }
        if (gl && write_labels) {
            char *gname = cp_xmalloc(gl + 1);
            memcpy(gname, nm, gl); gname[gl] = 0;
            double gy = top - (rs + re + 1) / 2.0 / n * hh;
            g = gt_add(T, G_TEXT, R, 1, R, 1);
            g->str = gname; g->size = sz; g->col = gc ? *gc : C_BLACK;
            g->tx = gx; g->ty = gy;
            g->hj = 1; g->va = V_INKCENTER;
            if (!bar && gc && labw > 0 && cell_pt > 0) { /* swatch beside the group name */
                double gw = text_w(cr, sz, gname);
                g = gt_add(T, G_RECT, R, 1, R, 1);
                g->col = *gc; g->sub = 1;
                g->x1 = gx - (gw + TXT_GAP) / labw;
                g->x0 = g->x1 - SWATCH_PT / labw;
                g->y0 = gy - SWATCH_PT / 2 / cell_pt;
                g->y1 = gy + SWATCH_PT / 2 / cell_pt;
            }
        }
        if (re + 1 < n) {            /* rule below this run */
            g = gt_add(T, G_LINE, R, CC, R, CC);
            g->col = C_BLACK; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
            g->x0 = 0; g->x1 = 1;
            g->y0 = g->y1 = top - (double)(re + 1) / n * hh;
        }
        rs = re + 1;
    }
}

/* ---- signal("long.tsv"): continuous traces. The long shape matrix() reads,
 * `chrom beg end value sample [series]` (value may be headed beta, as a
 * methylation table is), with an optional series column telling the lines
 * within a strip apart. One strip per sample, one line per series.
 *
 * The file is read once and kept unfiltered: the strip order (first
 * appearance of each sample), the series order and each strip's value range
 * must be the same in every regions() window, so they come from the whole
 * file; each window then draws the rows inside it. ---- */
typedef struct {
    int n;
    const char **chrom;     /* per row; borrowed from the file, or formatted */
    double *beg, *end, *val;
    int *strip, *series;    /* per row */
    int nstrip; char **stripname;
    int nser; char **sername;   /* a lone "" series when there is no column */
    Col *sercol;
    int *draworder;         /* series indices, the one drawn ON TOP first */
    double *lo, *hi;        /* per strip: value range over the whole file */
} SigData;

/* a text-or-number cell as text, so a numeric sample/series column works */
static const char *cell_text(const Column *c, int r, char *buf, size_t cap) {
    if (c->type == COL_STR) return c->str[r];
    if (!isfinite(c->num[r])) return NULL;
    snprintf(buf, cap, "%.15g", c->num[r]);
    return buf;
}

static int name_index(char **names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i;
    return -1;
}

static SigData *read_signal(const TrackObj *t, char *err) {
    const char *path = strcmp(t->data, "stdin") == 0 ? "-" : t->data;
    DataFrame *df = df_read_csv(path, err);
    if (!df) return NULL;
    const Column *chr_c = df_col(df, "chrom");
    const Column *sc = df_col(df, "beg"); if (!sc) sc = df_col(df, "start");
    const Column *ec = df_col(df, "end");
    const Column *pid = df_col(df, "Probe_ID");
    const Column *vc = df_col(df, "value"); if (!vc) vc = df_col(df, "beta");
    const Column *samp = df_col(df, "sample");
    const Column *ser = df_col(df, "series");
    if (!chr_c) {
        snprintf(err, CP_ERRLEN, "signal `%s`: needs a chrom column (the columns are "
                 "chrom beg end value sample [series])", t->data);
        return NULL;
    }
    if (!sc || !ec || sc->type != COL_NUM || ec->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "signal `%s`: needs numeric position columns beg (or "
                 "start) and end", t->data);
        return NULL;
    }
    /* the value: `value` or `beta` by name, else the first numeric column
     * that is not a position, as matrix() takes it */
    if (!vc)
        for (int c = 0; c < df->ncol; c++) {
            const Column *col = &df->cols[c];
            if (col->type == COL_NUM && col != chr_c && col != sc && col != ec
                && col != pid && col != samp && col != ser) { vc = col; break; }
        }
    if (!vc) {
        snprintf(err, CP_ERRLEN, "signal `%s`: needs a numeric value column (`value` or "
                 "`beta`)", t->data);
        return NULL;
    }
    if (vc->type != COL_NUM) {
        snprintf(err, CP_ERRLEN, "signal `%s`: column `%s` must be numeric throughout "
                 "(write NA for a missing value)", t->data, vc->name);
        return NULL;
    }
    /* the strip: `sample` by name, else the first text column that is not
     * chrom / Probe_ID / series */
    if (!samp)
        for (int c = 0; c < df->ncol; c++) {
            const Column *col = &df->cols[c];
            if (col->type == COL_STR && col != chr_c && col != pid && col != ser
                && col != vc) { samp = col; break; }
        }
    if (!samp) {
        snprintf(err, CP_ERRLEN, "signal `%s`: needs a sample column naming each row's "
                 "strip", t->data);
        return NULL;
    }
    SigData *d = cp_xcalloc(1, sizeof *d);
    int nrow = df->nrow;
    d->chrom = cp_xmalloc((size_t)nrow * sizeof(char *));
    d->beg = cp_xmalloc((size_t)nrow * sizeof(double));
    d->end = cp_xmalloc((size_t)nrow * sizeof(double));
    d->val = cp_xmalloc((size_t)nrow * sizeof(double));
    d->strip = cp_xmalloc((size_t)nrow * sizeof(int));
    d->series = cp_xmalloc((size_t)nrow * sizeof(int));
    d->stripname = cp_xmalloc((size_t)(nrow ? nrow : 1) * sizeof(char *));
    d->sername = cp_xmalloc((size_t)(nrow ? nrow : 1) * sizeof(char *));
    if (!ser) { d->sername[0] = ""; d->nser = 1; }
    for (int r = 0; r < nrow; r++) {
        if (isnan(vc->num[r])) continue;                 /* a missing value: no point */
        if (isnan(sc->num[r]) || isnan(ec->num[r])) {
            snprintf(err, CP_ERRLEN, "signal `%s`: row %d has no position (beg/end)",
                     t->data, r + 2);
            return NULL;
        }
        char buf[64];
        const char *cv = matrix_chrom_value(chr_c, r, buf, sizeof buf);
        if (!cv) {
            snprintf(err, CP_ERRLEN, "signal `%s`: row %d has an empty chrom", t->data, r + 2);
            return NULL;
        }
        char sbuf[64], ebuf[64];
        const char *sn = cell_text(samp, r, sbuf, sizeof sbuf);
        if (!sn || !*sn) {
            snprintf(err, CP_ERRLEN, "signal `%s`: row %d has an empty sample", t->data, r + 2);
            return NULL;
        }
        const char *en = ser ? cell_text(ser, r, ebuf, sizeof ebuf) : "";
        if (!en) {
            snprintf(err, CP_ERRLEN, "signal `%s`: row %d has an empty series", t->data, r + 2);
            return NULL;
        }
        int k = d->n;
        d->chrom[k] = cv == buf ? cp_xstrdup(buf) : cv;
        d->beg[k] = sc->num[r]; d->end[k] = ec->num[r]; d->val[k] = vc->num[r];
        int si = name_index(d->stripname, d->nstrip, sn);
        if (si < 0) { si = d->nstrip++; d->stripname[si] = sn == sbuf ? cp_xstrdup(sn) : (char *)sn; }
        int ei = ser ? name_index(d->sername, d->nser, en) : 0;
        if (ei < 0) { ei = d->nser++; d->sername[ei] = en == ebuf ? cp_xstrdup(en) : (char *)en; }
        d->strip[k] = si; d->series[k] = ei;
        d->n++;
    }
    if (d->n < 1) {
        snprintf(err, CP_ERRLEN, "signal `%s` has no rows with a value", t->data);
        return NULL;
    }
    d->lo = cp_xmalloc((size_t)d->nstrip * sizeof(double));
    d->hi = cp_xmalloc((size_t)d->nstrip * sizeof(double));
    for (int k = 0; k < d->nstrip; k++) { d->lo[k] = INFINITY; d->hi[k] = -INFINITY; }
    for (int k = 0; k < d->n; k++) {
        if (d->val[k] < d->lo[d->strip[k]]) d->lo[d->strip[k]] = d->val[k];
        if (d->val[k] > d->hi[d->strip[k]]) d->hi[d->strip[k]] = d->val[k];
    }
    return d;                          /* df leaked: names borrow from it */
}

/* Series colours: colour="one" paints every series alike; colour=c(...)
 * names them (a name absent from the data is an error, a series absent from
 * the list keeps its hue) or lists them positionally, one per series in
 * file order; with neither, hue_palette(). The draw order is settled here
 * too: the first series -- in colour=c(...) when it names them, else in the
 * file -- is drawn on top, since the reference trace is the one listed
 * first and it must not vanish under the others. */
static int sig_colours(const TrackObj *t, SigData *d, char *err) {
    int ns = d->nser;
    d->sercol = cp_xmalloc((size_t)ns * sizeof(Col));
    d->draworder = cp_xmalloc((size_t)ns * sizeof(int));
    for (int i = 0; i < ns; i++) d->draworder[i] = i;
    hue_palette(ns, d->sercol);
    if (t->has_color) {
        for (int i = 0; i < ns; i++) d->sercol[i] = t->color;
        return 0;
    }
    if (t->nser_col == 0) return 0;
    int named = 0, positional = 0;
    for (int k = 0; k < t->nser_col; k++) {
        if (t->ser_name[k]) named++; else positional++;
    }
    if (named && positional) {
        snprintf(err, CP_ERRLEN, "signal(colour=c(...)): name every colour or none; "
                 "%d of %d are named", named, t->nser_col);
        return -1;
    }
    if (positional) {
        if (t->nser_col < ns) {
            snprintf(err, CP_ERRLEN, "signal(colour=c(...)) gives %d colours; `%s` has %d "
                     "series", t->nser_col, t->data, ns);
            return -1;
        }
        for (int i = 0; i < ns; i++) d->sercol[i] = t->ser_col[i];
        return 0;
    }
    int nord = 0;
    for (int k = 0; k < t->nser_col; k++) {
        int si = name_index(d->sername, ns, t->ser_name[k]);
        if (si < 0) {
            char have[512]; size_t o = 0;
            for (int i = 0; i < ns && o < sizeof have - 4; i++) {
                int w = snprintf(have + o, sizeof have - o, "%s`%s`", i ? ", " : "", d->sername[i]);
                if (w < 0 || (size_t)w >= sizeof have - o) { snprintf(have + o, sizeof have - o, "..."); break; }
                o += (size_t)w;
            }
            if (ns == 1 && !*d->sername[0])
                snprintf(err, CP_ERRLEN, "signal(colour=): series `%s` is not in `%s`, "
                         "which has no series column", t->ser_name[k], t->data);
            else
                snprintf(err, CP_ERRLEN, "signal(colour=): series `%s` is not in `%s`; "
                         "its series are %s", t->ser_name[k], t->data, have);
            return -1;
        }
        d->sercol[si] = t->ser_col[k];
        int seen = 0;
        for (int i = 0; i < nord; i++) if (d->draworder[i] == si) seen = 1;
        if (!seen) d->draworder[nord++] = si;
    }
    for (int i = 0; i < ns; i++) {              /* the unnamed follow, in file order */
        int seen = 0;
        for (int j = 0; j < nord; j++) if (d->draworder[j] == i) seen = 1;
        if (!seen) d->draworder[nord++] = i;
    }
    return 0;
}

/* a (position, value) pair, sorted by position for the raw polyline */
typedef struct { double x, y; } SigPt;
static int cmp_sigpt(const void *a, const void *b) {
    double d = ((const SigPt *)a)->x - ((const SigPt *)b)->x;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

int render_tracks(const PlotSpec *spec, const char *out,
                  double w_pt, double h_pt, char *err) {
    int ntr = spec->ntracks;

    /* ---- pre-read matrix tracks: they size the sample-label column below, and
     * an empty region() infers its window from the first one. ---- */
    MatData *md[MAX_TRACKS] = {0};
    MatData *rowref[MAX_TRACKS] = {0};   /* shared sample order for regions() */
    int has_matrix = 0;
    /* An explicit region() is known before the data is read, so the matrix can
     * be filtered as it loads. An inferred one is not -- the rows define the
     * window -- so load unfiltered and re-load below once the window is known. */
    char pre_chrom[64] = ""; long pre_beg = 0, pre_end = 0;
    if (spec->regions_path && spec->region) {
        snprintf(err, CP_ERRLEN, "region() and regions() both give the window; "
                 "use one or the other");
        return -1;
    }
    /* Under regions() the pre-read must stay unfiltered whatever region() says:
     * it is the reference row set every window is aligned to (align_rows). */
    int have_pre = !spec->regions_path && spec->region
                   && region_parse(spec->region, pre_chrom, &pre_beg, &pre_end) == 0;
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].type == TRK_MATRIX) {
            md[i] = read_matrix(&spec->tobjs[i], have_pre ? pre_chrom : NULL,
                                pre_beg, pre_end, err);
            if (!md[i]) return -1;
            rowref[i] = md[i];
            has_matrix = 1;
        }
    /* signal tracks: read whole, once; each window draws its own rows */
    SigData *sd[MAX_TRACKS] = {0};
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].type == TRK_SIGNAL) {
            sd[i] = read_signal(&spec->tobjs[i], err);
            if (!sd[i] || sig_colours(&spec->tobjs[i], sd[i], err)) return -1;
        }
    /* rowcolour= tables, read once: they colour the bands / group names below */
    RowCol *rcs[MAX_TRACKS] = {0}; int nrcs[MAX_TRACKS] = {0};
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].rowcolour
            && trk_rowcolours_load(&spec->tobjs[i], &rcs[i], &nrcs[i], err)) return -1;

    /* rowmeta= metadata bands, built once per track from the sample display
     * order (matrix: the clustered/file roword; signal: file order). */
    RowAnno *ann[MAX_TRACKS] = {0};
    for (int i = 0; i < ntr; i++) {
        const TrackObj *t = &spec->tobjs[i];
        if (!t->rowmeta) continue;
        if (md[i]) {
            ann[i] = trk_rowmeta_load(t, md[i]->rowname, md[i]->roword, md[i]->nr,
                                      rcs[i], nrcs[i], err);
        } else if (sd[i]) {
            int *ident = cp_xmalloc((size_t)(sd[i]->nstrip > 0 ? sd[i]->nstrip : 1) * sizeof(int));
            for (int k = 0; k < sd[i]->nstrip; k++) ident[k] = k;
            ann[i] = trk_rowmeta_load(t, sd[i]->stripname, ident, sd[i]->nstrip,
                                      rcs[i], nrcs[i], err);
        }
        if (!ann[i]) return -1;
    }

    /* ---- the discrete fill: matrix(discrete=TRUE) reads the cell values as
     * category codes. The levels come from the pre-read (the whole file under
     * regions(), so every window keys the same set) through the routine
     * heatmap(discrete=TRUE) uses, and the palette through its palette rule,
     * so a 0/1 file keys identically in both modes. The scale-vs-data pairing
     * is checked here beside the data, as heatmap.c does: a ramp over codes
     * and a manual palette over measurements are both refused by name. ---- */
    char **lev[MAX_TRACKS] = {0}; double *levv[MAX_TRACKS] = {0}; int nlev[MAX_TRACKS] = {0};
    Col *pal[MAX_TRACKS] = {0}; int any_na[MAX_TRACKS] = {0};
    int ndisc = 0, ncont = 0, nmat = 0, i_disc = -1;
    for (int i = 0; i < ntr; i++) {
        if (!md[i]) continue;
        nmat++;
        if (spec->tobjs[i].discrete) { ndisc++; if (i_disc < 0) i_disc = i; } else ncont++;
    }
    if (ndisc && !ncont && spec->has_fill) {
        snprintf(err, CP_ERRLEN, "scale_fill_gradient()/viridis()/parula()/... is a continuous "
                 "ramp and matrix() `%s` is categorical (discrete=TRUE); colour the "
                 "categories with scale_fill_manual(values=c(\"0\"=\"...\", ...)) or "
                 "scale_fill_brewer(palette=), or drop discrete=TRUE to read the cells "
                 "as numbers", spec->tobjs[i_disc].name ? spec->tobjs[i_disc].name
                                                          : spec->tobjs[i_disc].data);
        return -1;
    }
    if (nmat && !ndisc && spec->has_manual) {
        snprintf(err, CP_ERRLEN, "scale_*_manual()/scale_*_brewer() is a discrete palette, "
                 "and this matrix() track is numeric. Use scale_fill_gradient()/"
                 "gradient2()/viridis()/parula() for a ramp, or matrix(discrete=TRUE) "
                 "to read the numbers as category codes");
        return -1;
    }
    if (nmat && !ndisc && spec->n_manual_labs) {
        snprintf(err, CP_ERRLEN, "scale_*_manual(labels=) renames the levels of a discrete "
                 "key, and this matrix() track is numeric (a colourbar has no levels); "
                 "say matrix(discrete=TRUE), or drop labels=");
        return -1;
    }
    for (int i = 0; i < ntr; i++) {
        if (!md[i] || !spec->tobjs[i].discrete) continue;
        long ncell = (long)md[i]->nr * md[i]->nc;
        for (long k = 0; k < ncell && !any_na[i]; k++) any_na[i] = isnan(md[i]->mv[k]);
        nlev[i] = cp_numeric_levels(md[i]->mv, ncell, &lev[i], &levv[i],
                                    "discrete matrix() fill", err);
        if (nlev[i] < 0) return -1;
        pal[i] = cp_xmalloc(HM_MAXLEV * sizeof(Col));
        if (cp_discrete_palette(spec, lev[i], nlev[i], pal[i], err)) return -1;
    }
    /* legend(): the one heatmap-mode object the parser lets through here. It
     * keys THE matrix track -- with two there is no saying which band it
     * should centre on, so that is refused rather than guessed. */
    const HMObj *lg = NULL; int li = -1;
    for (int k = 0; k < spec->nhobjs; k++)
        if (spec->hobjs[k].type == HM_LEGEND) lg = &spec->hobjs[k];
    if (lg) {
        if (nmat > 1) {
            snprintf(err, CP_ERRLEN, "legend() with %d matrix() tracks: the track legend "
                     "describes one matrix and sits beside it; keep one matrix() track, "
                     "or draw the figure per matrix", nmat);
            return -1;
        }
        for (int i = 0; i < ntr; i++) if (md[i]) { li = i; break; }
    }

    /* ---- windows: regions() gives several, region() one ---- */
    Window *wins = NULL; int nwin = 1;
    if (spec->regions_path) {
        wins = load_windows(spec->regions_path, &nwin, err);
        if (!wins) return -1;
        /* Step 1 covers matrix() only; the other track types still load for a
         * single window and are added one at a time. Refuse rather than draw
         * something misleading. */
        /* Track types cleared for regions() so far. Each loads its own data for
         * the rebound window inside the loop, so adding one is mostly a matter
         * of confirming it does not cache anything across windows. */
        for (int i = 0; i < ntr; i++)
            if (spec->tobjs[i].type != TRK_MATRIX
                && spec->tobjs[i].type != TRK_GENES
                && spec->tobjs[i].type != TRK_INTERVAL
                && spec->tobjs[i].type != TRK_SIGNAL) {
                snprintf(err, CP_ERRLEN, "regions() supports matrix(), genes(), "
                         "interval() and signal() tracks so far; the remaining track "
                         "types are being added one at a time");
                return -1;
            }
        if (3 + 2 * nwin >= GT_MAXDIM) {
            snprintf(err, CP_ERRLEN, "regions(): %d windows exceeds the layout's "
                     "%d-column limit", nwin, GT_MAXDIM);
            return -1;
        }
    }

    /* Window widths. bp span is the obvious weight, but it is the wrong one when
     * a matrix track is present: the heatmap places its columns evenly across
     * the panel regardless of bp, so a window holding few CpGs over a wide span
     * gets a broad panel of fat cells while a dense narrow one is squeezed.
     * Weight by probe count instead, so a cell is about the same width in every
     * window. Falls back to bp span when there is no matrix. */
    double *wweight = NULL;
    if (wins) {
        wweight = cp_xmalloc(nwin * sizeof(double));
        int mi = -1;
        for (int i = 0; i < ntr; i++) if (spec->tobjs[i].type == TRK_MATRIX) { mi = i; break; }
        for (int wi = 0; wi < nwin; wi++) {
            wweight[wi] = (double)(wins[wi].end - wins[wi].beg);
            if (mi >= 0) {
                MatData *probe = read_matrix(&spec->tobjs[mi], wins[wi].chrom,
                                             wins[wi].beg, wins[wi].end, err);
                if (probe && probe->nc > 0) wweight[wi] = probe->nc;
            }
        }
    }

    /* ---- resolve the region window: explicit chr:start-end, or inferred from a
     * matrix track (5% pad each end) when region() is empty / omitted ---- */
    char chrom[64]; long rstart, rend;
    if (wins) {
        /* regions() supplies the windows; the loop below rebinds these per
         * window, so this just seeds the shared setup (axis unit, sizing). */
        snprintf(chrom, sizeof chrom, "%s", wins[0].chrom);
        rstart = wins[0].beg; rend = wins[0].end;
    } else if (spec->region) {
        if (region_parse(spec->region, chrom, &rstart, &rend)) {
            snprintf(err, CP_ERRLEN, "bad region `%s`; expected chr:start-end with "
                     "non-negative integers and start < end", spec->region); return -1;
        }
    } else {
        MatData *src = NULL;
        for (int i = 0; i < ntr; i++) if (md[i]) { src = md[i]; break; }
        if (!src) { snprintf(err, CP_ERRLEN, "region() needs coordinates or a matrix() track to infer from"); return -1; }
        if (!src->chrom) { snprintf(err, CP_ERRLEN, "region() cannot infer: matrix has no chrom column; use region(chr:start-end)"); return -1; }
        if (src->multichrom) { snprintf(err, CP_ERRLEN, "region() cannot infer: matrix spans multiple chromosomes; use region(chr:start-end)"); return -1; }
        snprintf(chrom, sizeof chrom, "%s", src->chrom);
        long pad = (long)((src->gend - src->gbeg) * 0.05 + 0.5);
        if (pad < 1) pad = 1;
        rstart = src->gbeg - pad; if (rstart < 0) rstart = 0;
        rend = src->gend + pad;
    }
    char rgn_disp[96];
    snprintf(rgn_disp, sizeof rgn_disp, "%s:%ld-%ld", chrom, rstart, rend);
    double x0 = rstart, x1 = rend;
#define NPCX(v) (((v) - x0) / (x1 - x0))

    /* measure text on a scratch context; the real output surface is created
     * only after auto-fit (below) resolves any auto (0) size axis. */
    cairo_surface_t *msurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    cairo_t *cr = cairo_create(msurf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    /* ---- genomic axis: pick unit, nice breaks, format with suffix ---- */
    double span = x1 - x0;
    double unit = span >= 2e6 ? 1e6 : span >= 2e3 ? 1e3 : 1;
    const char *usuf; usuf = unit == 1e6 ? " Mb" : unit == 1e3 ? " kb" : " bp";
    double ubr[16];
    int nb = extended_breaks(x0 / unit, x1 / unit, 5, ubr, 16), nx = 0;
    double xpos[16], xtxt[16]; char *xlab[16];
    int dec = axis_decimals(ubr, nb);
    for (int i = 0; i < nb; i++) {
        double bp = ubr[i] * unit;
        if (bp < x0 || bp > x1) continue;
        xpos[nx] = NPCX(bp); xtxt[nx] = xpos[nx];
        char num[32]; fmt_break(ubr[i], dec, num, sizeof num);                 /* number, with commas */
        xlab[nx] = cp_xmalloc(64); commafy(xlab[nx], 64, num);
        nx++;
    }
    if (nx > 0) {                              /* unit suffix once, on the last tick */
        char buf[64]; snprintf(buf, sizeof buf, "%s%s", xlab[nx - 1], usuf);
        snprintf(xlab[nx - 1], 64, "%s", buf);
    }

    /* ONE size for every track label. A track figure does not rank a panel
     * title above a row label the way a grammar figure ranks axis title above
     * axis text; the browser's labels are all peers, so they share the single
     * axis-text size and scale together with --font-size / theme_*(base_size=).
     * (The other four modes keep their hierarchy; only track mode flattens.) */
    /* One flat size for every track label -- SZ_TRACK, 9pt at the default base
     * -- so a panel title does not outrank a row label. */
    double sz_title = SZ_TRACK, sz_samp = SZ_TRACK;

    /* widest gene label -> reserved right margin, so transcript names always fit
     * to the right of their model (no left-flip collisions at narrow widths) */
    double gene_labw = 0;
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].type == TRK_GENES) {
            int ng; GeneModel *gm = load_genes(spec->tobjs[i].data, chrom, rstart, rend,
                                               spec->tobjs[i].all_transcripts, &ng, err);
            if (gm) for (int k = 0; k < ng; k++)
                if (gm[k].name) {
                    double w = text_w(cr, SZ_TRACK, gm[k].name);
                    if (w > gene_labw) gene_labw = w;
                }
        }
    double rmargin = gene_labw > 0 ? gene_labw + HALF_LINE : MARGIN;

    /* ---- legend(): measured now so the right margin can hold it. A discrete
     * matrix gets the key (one swatch per level, renamed by labels=, plus the
     * background swatch when the track has one to explain); a continuous one
     * the colourbar over the track's fixed 0..1 domain. Drawn after the
     * tracks, centred on the matrix band, below. ---- */
    /* the legend title shares the single track label size, not the grammar
     * SZ_BASE — track mode renders every label at one size (see sz_samp above) */
    double leg_across = 0, leg_h = 0, baseH = font_h(cr, SZ_TRACK);
    const char *leg_title = NULL;
    char **klab = NULL; Col *kpal = NULL; int nk = 0, leg_disc = 0;
    if (lg) {
        const TrackObj *lt = &spec->tobjs[li];
        leg_title = lg->title ? lg->title : spec->lab_fill;
        if (lt->discrete) {
            leg_disc = 1;
            char **base = cp_key_labels(spec, lev[li], nlev[li], err);
            if (!base) return -1;
            /* the background shows wherever a cell is NA, and between probes
             * when x=genomic painted it -- a colour on the figure the key
             * would otherwise leave unexplained */
            int miss = !lg->missing_off && ((lt->genomic_x && lt->has_bg) || any_na[li]);
            nk = nlev[li] + miss;
            klab = cp_xmalloc((size_t)nk * sizeof(char *));
            kpal = cp_xmalloc((size_t)nk * sizeof(Col));
            for (int k = 0; k < nlev[li]; k++) { klab[k] = base[k]; kpal[k] = pal[li][k]; }
            if (miss) {
                klab[nk - 1] = lg->missing ? lg->missing : "NA";
                kpal[nk - 1] = lt->genomic_x && lt->has_bg ? lt->bg_color : C_NA;
            }
            leg_across = cp_key_across_pt(cr, klab, nk);
            leg_h = nk * LEG_GRID + (nk - 1) * LEG_GAP;
        } else {
            leg_across = cp_colourbar_across_pt(cr, 0, 1);
            leg_h = LEG_LEN;
        }
        if (leg_title) {
            leg_across = fmax(leg_across, text_w(cr, SZ_TRACK, leg_title));
            leg_h += baseH + TXT_GAP;
        }
        rmargin = fmax(rmargin, HALF_LINE + leg_across + MARGIN);
    }

    /* ---- measure left label column from track names + matrix sample names ---- */
    double labw = 0;
    for (int i = 0; i < ntr; i++) {
        const TrackObj *t = &spec->tobjs[i];
        if (t->name && t->type != TRK_MATRIX && t->type != TRK_SIGNAL) {
            double w = text_w(cr, SZ_TRACK, t->name);
            if (w > labw) labw = w;
        }
        if (md[i]) {
            double w = measure_gutter(cr, sz_samp, t, ann[i], md[i]->rowname,
                                      md[i]->nr, !t->hide_rownames);
            if (w > labw) labw = w;
        }
        if (sd[i]) {
            /* strip labels as the matrix's rows; a name= stands rotated at the
             * gutter's left edge, since the strips own the horizontal space */
            double w = measure_gutter(cr, sz_samp, t, ann[i], sd[i]->stripname,
                                      sd[i]->nstrip, 1);
            if (t->name) w += font_h(cr, SZ_TRACK) + TXT_GAP;
            if (w > labw) labw = w;
        }
    }
    const char *title = spec->lab_title ? spec->lab_title : (spec->region ? spec->region : rgn_disp);
    double titleh = title ? font_h(cr, sz_title) : 0;
    double axh = font_h(cr, SZ_TRACK);
    double lab_pad = HALF_LINE * 0.5;      /* row & column label -> heatmap gap (fixed pt) */

    /* ---- auto-fit: fill any auto (0) size axis from the measured content.
     * Width = chrome + a default panel. Height anchors the per-weight unit on
     * the matrix track's rows (so it grows taller when sample names show), which
     * keeps height= a relative weight rather than an absolute size. ---- */
    if (w_pt <= 0 || h_pt <= 0) {
        double aw = MARGIN + labw + (labw > 0 ? lab_pad : 0) + AUTO_PANEL_W + rmargin;
        double per_u = AUTO_UNIT_H, sumw = 0, samp_line = font_h(cr, sz_samp);
        for (int i = 0; i < ntr; i++)
            sumw += spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1;
        for (int i = 0; i < ntr; i++)
            if (md[i]) {                                 /* first matrix anchors height */
                double wgt = spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1, lbl_pt = 0;
                for (int c = 0; c < md[i]->nc; c++)
                    if (md[i]->colid[c]) { double w = text_w(cr, sz_samp, md[i]->colid[c]); if (w > lbl_pt) lbl_pt = w; }
                double bands = font_h(cr, SZ_TRACK) + TICK_LEN + TXT_GAP + 42 + lbl_pt + lab_pad;
                double row_h = spec->tobjs[i].hide_rownames ? AUTO_MIN_CELL : samp_line * AUTO_ROW_PAD;
                double rows_pt = md[i]->nr * row_h;
                if (lg && i == li && rows_pt < leg_h) rows_pt = leg_h;   /* the key beside it fits */
                double u = (bands + rows_pt) / wgt;
                if (u > per_u) per_u = u;
                break;
            }
        for (int i = 0; i < ntr; i++)
            if (sd[i]) {                                 /* a strip is a few label lines tall */
                double wgt = spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1;
                double u = sd[i]->nstrip * samp_line * AUTO_STRIP_LINES / wgt;
                if (u > per_u) per_u = u;
            }
        double gaps = ntr > 1 ? (ntr - 1) * HALF_LINE * 0.6 : 0;
        double axisr = has_matrix ? HALF_LINE : TICK_LEN + TXT_GAP + axh;
        double fixed = MARGIN + titleh + (title ? HALF_LINE * 0.3 : 0) + gaps + axisr + MARGIN;
        double ah = fixed + per_u * sumw;
        if (w_pt <= 0) w_pt = fmin(AUTO_MAX_PT, fmax(AUTO_MIN_PT, aw));
        if (h_pt <= 0) h_pt = fmin(AUTO_MAX_PT, fmax(AUTO_MIN_PT, ah));
    }

    /* switch from the scratch measuring context to the real output surface */
    cairo_destroy(cr); cairo_surface_destroy(msurf);
    cairo_surface_t *surf = cp_surface_create(out, w_pt, h_pt);
    cr = cairo_create(surf);
    cairo_select_font_face(cr, cp_font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    double panel_w = w_pt - MARGIN - labw - (labw > 0 ? lab_pad : 0) - rmargin;  /* pt */

    /* ---- outer gtable: [MARGIN|label|gap|PANEL|MARGIN] cols;
     * [MARGIN|title|gap| tracks+gaps |axis|MARGIN] rows ---- */
    GTable *T = cp_xcalloc(1, sizeof(GTable));
    /* [MARGIN|label|gap| panel (gap panel)* |MARGIN]. Each window owns a
     * column and the gaps between them are real, empty columns -- that is what
     * makes the break visible, and it is why every track's boundaries line up:
     * they all span the same column set. */
    T->ncol = 3 + 2 * nwin;                   /* ... + trailing right margin */
    T->colw[0] = upt(MARGIN);
    T->colw[1] = upt(labw);
    T->colw[2] = upt(labw > 0 ? lab_pad : 0);
    for (int wi = 0; wi < nwin; wi++) {
        T->colw[3 + 2 * wi] = unull(wweight ? wweight[wi] : (double)(rend - rstart));
        if (wi < nwin - 1) T->colw[4 + 2 * wi] = upt(GAP_PT);
    }
    T->colw[T->ncol - 1] = upt(rmargin);
    int CC = 3;                               /* set per window in the loop below */
#define TRK(i) (3 + 2 * (i))
    int axisrow = 3 + (ntr > 0 ? 2 * ntr - 1 : 0);
    T->nrow = axisrow + 2;
    T->rowh[0] = upt(MARGIN);
    T->rowh[1] = upt(titleh);
    T->rowh[2] = upt(title ? HALF_LINE * 0.3 : 0);   /* small gap: title close to ideogram */
    for (int i = 0; i < ntr; i++) {
        double wgt = spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1;
        T->rowh[TRK(i)] = unull(wgt);
        if (i < ntr - 1) T->rowh[TRK(i) + 1] = upt(HALF_LINE * 0.6);
    }
    /* the anchored-heatmap draws its own kb axis at the top of its panel, so
     * the shared bottom axis row is dropped when a matrix track is present */
    T->rowh[axisrow] = upt(has_matrix ? HALF_LINE : TICK_LEN + TXT_GAP + axh);
    T->rowh[axisrow + 1] = upt(MARGIN);

    /* per-null-row height in pt, so a matrix track can size its kb-axis / map-line
     * / label bands in FIXED points (constant gaps regardless of figure size) */
    double per_h;
    {
        double fixed_h = 0, null_h = 0;
        for (int r = 0; r < T->nrow; r++)
            if (T->rowh[r].k == U_PT) fixed_h += T->rowh[r].v; else null_h += T->rowh[r].v;
        per_h = null_h > 0 ? fmax(0, h_pt - fixed_h) / null_h : 0;
    }

    /* highlight() boxes, flattened once; drawn per window below */
    TBox *boxes = NULL; int nboxes = 0;
    if (spec->nhls > 0) {
        if (trk_boxes_load(spec, &boxes, &nboxes, err)) return -1;
        int nmat = 0; const char *mname = NULL;
        for (int i = 0; i < ntr; i++)
            if (spec->tobjs[i].type == TRK_MATRIX) { nmat++; mname = spec->tobjs[i].name; }
        for (int b = 0; b < nboxes; b++) {
            if (!boxes[b].target && nmat > 1) {
                snprintf(err, CP_ERRLEN, "highlight(): %d matrix() tracks in the spec; "
                         "say which with name=", nmat);
                return -1;
            }
            if (boxes[b].target) {
                int hit = 0;
                for (int i = 0; i < ntr; i++)
                    if (spec->tobjs[i].type == TRK_MATRIX && spec->tobjs[i].name
                        && !strcmp(spec->tobjs[i].name, boxes[b].target)) hit = 1;
                if (!hit) {
                    snprintf(err, CP_ERRLEN, "highlight(name=\"%s\"): no matrix() track has "
                             "that name%s%s%s", boxes[b].target,
                             mname ? " (the matrix is named `" : "", mname ? mname : "",
                             mname ? "`)" : "");
                    return -1;
                }
            }
        }
    }

    Grob *g;
    double leg_top = 1, leg_bot = 0;          /* the matrix band, npc of its row */
    if (title && !wins) {
        g = gt_add(T, G_TEXT, 1, CC, 1, CC);
        g->str = title; g->size = sz_title; g->col = C_BLACK;
        g->tx = 0; g->ty = 1; g->hj = 0; g->va = V_TOP;
    }
    for (int wi = 0; wi < nwin; wi++) {
      /* Rebind the window: CC selects this window's column and x0/x1 -- which
       * NPCX closes over -- its coordinate span. Every track below therefore
       * draws into the same segmented axis without knowing about the others,
       * which is what makes the boundaries align down the stack. */
      CC = 3 + 2 * wi;
      if (wins) {
          snprintf(chrom, sizeof chrom, "%s", wins[wi].chrom);
          rstart = wins[wi].beg; rend = wins[wi].end;
          x0 = (double)rstart; x1 = (double)rend;
          for (int i = 0; i < ntr; i++)
              if (spec->tobjs[i].type == TRK_MATRIX) {
                  md[i] = read_matrix(&spec->tobjs[i], chrom, rstart, rend, err);
                  if (!md[i]) return -1;
                  align_rows(md[i], rowref[i]);
                  if (spec->tobjs[i].discrete
                      && cp_levels_apply(md[i]->mv, (long)md[i]->nr * md[i]->nc,
                                         levv[i], nlev[i], "discrete matrix() fill", err))
                      return -1;
              }
      }
      if (wins) {
          /* Each window is titled in its own column: the BED's name column when
           * it has one, else the coordinates. A single shared title cannot name
           * three different loci, and the name is what a reader needs -- the
           * coordinates are already on the axis below. */
          char *wt = cp_xmalloc(128);
          if (wins[wi].label && *wins[wi].label)
              snprintf(wt, 128, "%s", wins[wi].label);
          else
              snprintf(wt, 128, "%s:%ld-%ld", wins[wi].chrom, wins[wi].beg, wins[wi].end);
          g = gt_add(T, G_TEXT, 1, CC, 1, CC);
          g->str = wt; g->size = sz_title; g->col = C_BLACK;
          g->tx = 0.5; g->ty = 1; g->hj = 0.5; g->va = V_TOP;
      }

      /* This window's drawn width in points, so ticks and labels can be fitted
       * to the space they actually get rather than to the figure as a whole. */
      double win_pt = panel_w;
      if (wins) {
          double wsum = 0;
          for (int k = 0; k < nwin; k++) wsum += wweight[k];
          win_pt = wsum > 0 ? (panel_w - GAP_PT * (nwin - 1)) * wweight[wi] / wsum
                            : panel_w;
      }

      /* Axis breaks belong to the window, not the figure: each panel carries
       * its own coordinates, so recompute them here rather than once outside. */
      {
          double wspan = x1 - x0;
          double wunit = wspan >= 2e6 ? 1e6 : wspan >= 2e3 ? 1e3 : 1;
          const char *wsuf = wunit == 1e6 ? " Mb" : wunit == 1e3 ? " kb" : " bp";
          double wbr[16];
          /* A narrow panel cannot carry five labelled ticks. Ask for a count
           * the width can hold, then drop any that would still overlap: with
           * several windows on one axis the small ones otherwise collide into
           * an unreadable run. */
          int want = (int)(win_pt / 72.0) + 1;
          if (want < 2) want = 2;
          if (want > 5) want = 5;
          int wnb = extended_breaks(x0 / wunit, x1 / wunit, want, wbr, 16);
          int wdec = axis_decimals(wbr, wnb);
          nx = 0;
          double last_r = -1e9;                  /* right edge of the last kept label */
          for (int k = 0; k < wnb; k++) {
              double bp = wbr[k] * wunit;
              if (bp < x0 || bp > x1) continue;
              char num[32]; fmt_break(wbr[k], wdec, num, sizeof num);
              char *lab = cp_xmalloc(64); commafy(lab, 64, num);
              double npc = NPCX(bp), half = text_w(cr, SZ_TRACK, lab) / 2;
              double centre = npc * win_pt;
              /* A tick near the edge would centre its label half outside the
               * panel -- in the gap or the next window, or with one window
               * past the page margin, where it was cut in half. Keep the tick
               * where it belongs and slide the text inward, as a genome
               * browser does, rather than dropping a narrow panel's only
               * coordinate. The overlap test then has to run on the *slid*
               * position, or two nudged labels collide in the middle. */
              double tc = centre;
              if (tc - half < 0) tc = half;
              if (tc + half > win_pt) tc = win_pt - half;
              if (tc - half < last_r + 3) { free(lab); continue; }   /* would touch */
              last_r = tc + half;
              xtxt[nx] = win_pt > 0 ? tc / win_pt : npc;
              xpos[nx] = npc; xlab[nx] = lab; nx++;
          }
          if (nx > 0) {
              char buf[64]; snprintf(buf, sizeof buf, "%s%s", xlab[nx - 1], wsuf);
              snprintf(xlab[nx - 1], 64, "%s", buf);
          }
      }
    for (int i = 0; i < ntr; i++) {
        int R = TRK(i);
        TrackType tt = spec->tobjs[i].type;
        g = gt_add(T, G_RECT, R, CC, R, CC); g->col = C_WHITE;   /* track background */
        if (tt != TRK_CYTOBAND && tt != TRK_MATRIX)              /* region-scale grid */
            for (int b = 0; b < nx; b++) {                       /* faint gridlines */
                g = gt_add(T, G_LINE, R, CC, R, CC);
                g->col = C_TGRID; g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = g->x1 = xpos[b]; g->y0 = 0; g->y1 = 1;
            }
        if (spec->tobjs[i].name && tt != TRK_MATRIX && tt != TRK_SIGNAL) {   /* left label */
            g = gt_add(T, G_TEXT, R, 1, R, 1);
            g->str = spec->tobjs[i].name; g->size = SZ_TRACK; g->col = C_BLACK;
            g->tx = 1; g->ty = 0.5; g->hj = 1; g->va = V_INKCENTER;
        }

        /* ---- track content ---- */
        const TrackObj *t = &spec->tobjs[i];
        if (t->type == TRK_COVERAGE) {
            int nsb; SigBin *sb = bedgraph_read(t->data, chrom, rstart, rend, &nsb, err);
            if (!sb) return -1;
            /* The range is [min(0, min), max]: a positive-only bedGraph keeps
             * its zero baseline and [0 - max] readout, and a minus-strand or
             * log-ratio file draws its bars downward from a zero line instead
             * of an empty lane reading [0 - 1]. */
            double ymax = t->max_value > 0 ? t->max_value : 0, ymin = 0;
            if (ymax <= 0) for (int k = 0; k < nsb; k++) if (sb[k].val > ymax) ymax = sb[k].val;
            for (int k = 0; k < nsb; k++) if (sb[k].val < ymin) ymin = sb[k].val;
            if (ymax <= ymin) ymax = ymin + 1;
            double yspan = ymax - ymin, yzero = -ymin / yspan;
            Col col = t->has_color ? t->color : C_COV;
            for (int k = 0; k < nsb; k++) {
                if (sb[k].val == 0) continue;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(sb[k].start); g->x1 = NPCX(sb[k].end);
                g->y0 = yzero; g->y1 = (sb[k].val - ymin) / yspan;
            }
            char *rd = cp_xmalloc(32);                                       /* readout */
            if (ymin < 0) snprintf(rd, 32, "[%g - %g]", ymin, ymax);
            else          snprintf(rd, 32, "[0 - %g]", ymax);
            g = gt_add(T, G_TEXT, R, CC, R, CC);
            g->str = rd; g->size = SZ_TRACK; g->col = C_AXTXT;
            g->tx = 0.004; g->ty = 0.98; g->hj = 0; g->va = V_TOP;
        } else if (t->type == TRK_GENES) {
            /* gene models (BED12): exon boxes (thin UTR / thick CDS), intron
             * center line, strand chevrons, name; lane-packed. Default keeps the
             * canonical (longest) transcript per gene; transcripts=all shows all. */
            int ng; GeneModel *gm = load_genes(t->data, chrom, rstart, rend,
                                               t->all_transcripts, &ng, err);
            if (!gm) return -1;
            /* lane-pack, reserving the transcript's LABEL width so names don't
             * collide (as pyGenomeTracks / plotgardener do). The label sits to the
             * right of tx_end, so a lane is free only past tx_end + label width. */
            double bp_per_pt = (double)(rend - rstart) / panel_w;
            long laneend[64]; int nlanes = 0, *lane = cp_xmalloc(ng * sizeof(int));
            for (int k = 0; k < ng; k++) {
                long lw_bp = gm[k].name
                    ? (long)((text_w(cr, SZ_TRACK, gm[k].name) + HALF_LINE) * bp_per_pt) : 0;
                int L = -1;
                for (int j = 0; j < nlanes; j++) if (laneend[j] <= gm[k].tx_start) { L = j; break; }
                if (L < 0 && nlanes < 64) L = nlanes++;
                if (L < 0) L = nlanes - 1;
                lane[k] = L; laneend[L] = gm[k].tx_end + lw_bp;
            }
            /* the next same-lane feature's start (npc), so a name shrinks to the
             * room before it rather than overprinting it. Genes are tx_start
             * sorted (load_genes), so a backward sweep per lane gives it. */
            double *g_nextx = cp_xmalloc((ng > 0 ? ng : 1) * sizeof(double));
            {
                double lanenext[64];
                for (int j = 0; j < 64; j++) lanenext[j] = 2.0;
                for (int k = ng - 1; k >= 0; k--) {
                    g_nextx[k] = lanenext[lane[k]];
                    lanenext[lane[k]] = NPCX(gm[k].tx_start);
                }
            }
            Col col = t->has_color ? t->color : C_IVAL;
            Col cds_col = {0.77, 0.20, 0.16};                /* coding regions (red) */
            double lh = 1.0 / nlanes, utr = 0.30 * lh, cds = 0.62 * lh;
            for (int k = 0; k < ng; k++) {
                double yc = 1 - (lane[k] + 0.5) * lh;
                double xa = NPCX(gm[k].tx_start), xb = NPCX(gm[k].tx_end);
                g = gt_add(T, G_LINE, R, CC, R, CC);         /* thin intron line */
                g->col = col; g->lw = lw_pt(0.25); g->clip = 1;
                g->x0 = xa; g->x1 = xb; g->y0 = g->y1 = yc;
                double dir = gm[k].strand == '-' ? -1 : 1;   /* periodic strand chevrons */
                double achx = 2.6 / panel_w, step = 30.0 / panel_w;
                for (double xc = xa + step * 0.5; xc < xb; xc += step)
                    for (int s = -1; s <= 1; s += 2) {
                        g = gt_add(T, G_LINE, R, CC, R, CC);
                        g->col = col; g->lw = lw_pt(0.3); g->clip = 1;
                        g->x0 = xc - dir * achx; g->y0 = yc + s * 0.20 * lh;
                        g->x1 = xc;              g->y1 = yc;
                    }
                for (int j = 0; j < gm[k].nexon; j++) {      /* exon boxes */
                    long es = gm[k].exons[j].start, ee = gm[k].exons[j].end;
                    g = gt_add(T, G_RECT, R, CC, R, CC);     /* UTR (thin) */
                    g->col = col; g->sub = 1; g->clip = 1;
                    g->x0 = NPCX(es); g->x1 = NPCX(ee); g->y0 = yc - utr / 2; g->y1 = yc + utr / 2;
                    long cs = es > gm[k].cds_start ? es : gm[k].cds_start;
                    long ce = ee < gm[k].cds_end ? ee : gm[k].cds_end;
                    if (ce > cs) {                           /* CDS (thick, red) */
                        g = gt_add(T, G_RECT, R, CC, R, CC);
                        g->col = cds_col; g->sub = 1; g->clip = 1;
                        g->x0 = NPCX(cs); g->x1 = NPCX(ce); g->y0 = yc - cds / 2; g->y1 = yc + cds / 2;
                    }
                }
                if (gm[k].name) {                            /* name to the right of tx_end */
                    /* Shrink-to-fit: the name is drawn to the right of tx_end
                     * at the flat size when it fits, and shrunk toward a floor
                     * (LABEL_MIN_PT) when it will not -- dropped only if even the
                     * floor overflows. "Fits" means before the next same-lane
                     * feature and before the right edge: the window edge with
                     * several windows (the margin belongs to the next window),
                     * else the surface edge including the reserved right margin,
                     * so a lone single-window name still uses that margin.
                     * Packing reserved the FULL-size width, so a shrunk name can
                     * only leave a gap, never overlap its neighbour. */
                    double tx = xb + HALF_LINE / win_pt;
                    double edge = wins ? 1.0 : 1.0 + rmargin / win_pt;
                    double limit = g_nextx[k] < edge ? g_nextx[k] : edge;
                    double fs = fit_width(cr, SZ_TRACK, gm[k].name, (limit - tx) * win_pt);
                    if (fs > 0) {
                        g = gt_add(T, G_TEXT, R, CC, R, CC);
                        g->str = gm[k].name; g->size = fs; g->col = C_BLACK;
                        g->tx = tx; g->hj = 0; g->ty = yc; g->va = V_INKCENTER;
                        g->clip = wins ? 1 : 0;
                    }
                }
            }
        } else if (t->type == TRK_INTERVAL) {
            /* interval blocks packed into non-overlapping lanes */
            int ni; Interval *iv = bed_read(t->data, chrom, rstart, rend, &ni, err);
            if (!iv) return -1;
            qsort(iv, ni, sizeof *iv, cmp_iv);
            long laneend[64]; int nlanes = 0, *lane = cp_xmalloc(ni * sizeof(int));
            for (int k = 0; k < ni; k++) {
                int L = -1;
                for (int j = 0; j < nlanes; j++) if (laneend[j] <= iv[k].start) { L = j; break; }
                if (L < 0 && nlanes < 64) { L = nlanes++; }
                if (L < 0) L = nlanes - 1;               /* cap: pile into last lane */
                lane[k] = L; laneend[L] = iv[k].end;
            }
            /* Where the next feature in the same lane begins (npc), so a
             * name is drawn only where it fits before it. genes() reserves
             * the label width while lane-packing; interval boxes keep their
             * packing -- a CpG tick track is one lane of 380 boxes, and the
             * tick spacing, not the names, is what it shows -- and drop the
             * names that would overprint instead. labels=on forces them. */
            double *nextx = cp_xmalloc((ni > 0 ? ni : 1) * sizeof(double));
            {
                double lanenext[64];
                for (int j = 0; j < 64; j++) lanenext[j] = 2.0;   /* nothing follows */
                for (int k = ni - 1; k >= 0; k--) {
                    nextx[k] = lanenext[lane[k]];
                    lanenext[lane[k]] = NPCX(iv[k].start);
                }
            }
            Col col = t->has_color ? t->color : C_IVAL;
            double lh = 1.0 / nlanes;
            for (int k = 0; k < ni; k++) {
                double yb = 1 - (lane[k] + 1) * lh;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(iv[k].start); g->x1 = NPCX(iv[k].end);
                g->y0 = yb + 0.18 * lh; g->y1 = yb + 0.82 * lh;
                if (iv[k].name && t->labels >= 0) {      /* name to the right */
                    double tx = NPCX(iv[k].end) + 0.004;
                    /* fit before the next same-lane feature (labels=auto only)
                     * and the right edge (the window edge with several windows),
                     * shrinking toward the floor before dropping, as genes() do.
                     * labels=on ignores the neighbour and only respects the edge. */
                    double limit = wins ? 1.0 : 2.0;
                    if (t->labels == 0 && nextx[k] < limit) limit = nextx[k];
                    double fs = fit_width(cr, SZ_TRACK, iv[k].name, (limit - tx) * win_pt);
                    if (fs > 0) {
                        g = gt_add(T, G_TEXT, R, CC, R, CC);
                        g->str = iv[k].name; g->size = fs; g->col = C_BLACK;
                        g->tx = tx; g->ty = yb + 0.5 * lh;
                        g->hj = 0; g->va = V_INKCENTER;
                        g->clip = wins ? 1 : 0;
                    }
                }
            }
        } else if (t->type == TRK_ARCS) {
            int nl; Link *lk = bedpe_read(t->data, chrom, rstart, rend, &nl, err);
            if (!lk) return -1;
            Col col = t->has_color ? t->color : C_ARC;
            const int NS = 40;
            for (int k = 0; k < nl; k++) {
                double xa = NPCX((lk[k].a_start + lk[k].a_end) / 2.0);
                double xb = NPCX((lk[k].b_start + lk[k].b_end) / 2.0);
                double h = fmin(0.92, fabs(xb - xa) + 0.08);     /* wider span -> taller */
                double *px = cp_xmalloc(NS * sizeof(double)), *py = cp_xmalloc(NS * sizeof(double));
                for (int s = 0; s < NS; s++) {
                    double f = (double)s / (NS - 1);
                    px[s] = xa + f * (xb - xa); py[s] = h * sin(M_PI * f);
                }
                g = gt_add(T, G_POLYLINE, R, CC, R, CC);
                g->n = NS; g->px = px; g->py = py; g->col = col; g->lw = lw_pt(0.6); g->clip = 1;
            }
        } else if (t->type == TRK_CYTOBAND) {
            /* whole-chromosome ideogram (its OWN 0..chromLen scale) as a rounded,
             * centromere-pinched chromosome, plotgardener-style: light-grey Giemsa
             * ramp, light-green centromere, with a red marker at the viewed region */
            DataFrame *cb = df_read_csv(t->data, err);
            if (!cb) return -1;
            const Column *bc = df_col(cb, "chrom"), *bs = df_col(cb, "start"),
                         *be = df_col(cb, "end"), *bt = df_col(cb, "stain");
            if (!bc || !bs || !be || !bt) {
                snprintf(err, CP_ERRLEN, "cytoband `%s` needs chrom,start,end,stain columns", t->data); return -1;
            }
            /* Type-check before touching str[]/num[]: a text cell in start/end
             * (or a numeric stain) has no num[] and dereferenced NULL. */
            if (bs->type != COL_NUM || be->type != COL_NUM) {
                snprintf(err, CP_ERRLEN, "cytoband `%s`: column `%s` must be numeric",
                         t->data, bs->type != COL_NUM ? "start" : "end"); return -1;
            }
            if (bt->type != COL_STR) {
                snprintf(err, CP_ERRLEN, "cytoband `%s`: column `stain` must be text "
                         "(gneg, gpos50, acen, ...)", t->data); return -1;
            }
            /* a numeric chrom column (1, 2, ...) is formatted, as matrix() does */
#define CB_CHROM(r2) matrix_chrom_value(bc, (r2), cbchr, sizeof cbchr)
            char cbchr[64]; const char *cv;
            double clen = 0; int nband = 0;
            for (int r2 = 0; r2 < cb->nrow; r2++)
                if ((cv = CB_CHROM(r2)) && !strcmp(cv, chrom)) { nband++; if (be->num[r2] > clen) clen = be->num[r2]; }
            if (clen <= 0) { snprintf(err, CP_ERRLEN, "chromosome %s not in cytoband file %s", chrom, t->data); return -1; }
            double *bst = cp_xmalloc(nband * sizeof(double)), *ben = cp_xmalloc(nband * sizeof(double));
            Col *bcol = cp_xmalloc(nband * sizeof(Col));
            double cen_lo = 1, cen_hi = 0;                   /* centromere (acen) extent, npc */
            int nb2 = 0;
            for (int r2 = 0; r2 < cb->nrow; r2++) {
                if (!(cv = CB_CHROM(r2)) || strcmp(cv, chrom)) continue;
                double a = bs->num[r2] / clen, b = be->num[r2] / clen;
                const char *s = bt->str[r2];
                Col c;
                if      (!strcmp(s, "acen"))    c = (Col){0.55, 0.78, 0.52};   /* light green */
                else if (!strcmp(s, "gneg"))    c = (Col){0.96, 0.96, 0.96};   /* near-white */
                else if (!strcmp(s, "gpos25"))  c = (Col){0.82, 0.82, 0.82};
                else if (!strcmp(s, "gpos50"))  c = (Col){0.62, 0.62, 0.62};
                else if (!strcmp(s, "gpos75"))  c = (Col){0.42, 0.42, 0.42};
                else if (!strcmp(s, "gpos100")) c = (Col){0.20, 0.20, 0.20};
                else                            c = (Col){0.75, 0.75, 0.75};   /* gvar/stalk */
                if (!strcmp(s, "acen")) { if (a < cen_lo) cen_lo = a; if (b > cen_hi) cen_hi = b; }
                bst[nb2] = a; ben[nb2] = b; bcol[nb2] = c; nb2++;
            }
            g = gt_add(T, G_IDEOGRAM, R, CC, R, CC);
            g->n = nb2; g->px = bst; g->py = ben; g->pcol = bcol;
            g->x0 = cen_hi > cen_lo ? cen_lo : 0; g->x1 = cen_hi > cen_lo ? cen_hi : 0;
            g = gt_add(T, G_LINE, R, CC, R, CC);              /* red region marker */
            Col red = {0.85, 0, 0};
            g->col = red; g->lw = lw_pt(1.3); g->clip = 1;
            g->x0 = g->x1 = (rstart + rend) / 2.0 / clen;
            g->y0 = 0.12; g->y1 = 0.88;
#undef CB_CHROM
        } else if (t->type == TRK_MATRIX) {
            /* genome-anchored heatmap (rows = samples, cols = probes evenly
             * placed), read up front into md[i]. Cell bands (npc, y up):
             * [axline,1] kb axis, [hmtop,axline] map lines, [lblband,hmtop]
             * heatmap, [0,lblband] probe IDs. */
            MatData *m = md[i];
            int nr = m->nr, nc = m->nc;
            FillScale fs = spec->has_fill ? spec->fill : (FillScale){0};
            if (!spec->has_fill) fs.kind = FILL_PARULA;       /* beta default */
            /* discrete=TRUE: a cell holds its level index and paints the
             * level's colour, the same table the key draws from */
            const Col *dpal = spec->tobjs[i].discrete ? pal[i] : NULL;
#define CELL_COL(v) (dpal ? dpal[(int)(v)] : fill_map_value(&fs, (v), 0, 1))
            /* bands sized in FIXED points (constant gaps at any figure size). */
            double cell_pt = (spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1) * per_h;
            double axtop_pt = font_h(cr, SZ_TRACK) + TICK_LEN + TXT_GAP;   /* kb axis */
            double mapband_pt = 42;                                            /* bezier band */
            double lbl_pt = 0;                                                 /* rotated probe IDs */
            if (!spec->tobjs[i].hide_colnames)
                for (int c = 0; c < nc; c++)
                    if (m->colid[c]) { double w = text_w(cr, sz_samp, m->colid[c]); if (w > lbl_pt) lbl_pt = w; }
            /* x=genomic needs no leader fan (the cells are already at their
             * coordinate) and no per-probe label band (there is no column to
             * label), so the heatmap takes the space both would have used. */
            int gx_mode = spec->tobjs[i].genomic_x;
            if (gx_mode) { mapband_pt = 0; lbl_pt = 0; }
            double axline  = cell_pt > 0 ? 1 - axtop_pt / cell_pt : 0.94;
            double hmtop   = cell_pt > 0 ? axline - mapband_pt / cell_pt : 0.80;
            double lblband = cell_pt > 0 ? (lbl_pt + lab_pad) / cell_pt : 0.10;  /* labels + lab_pad gap */
            double lbltop  = cell_pt > 0 ? lbl_pt / cell_pt : lblband * 0.85;    /* label tops = lab_pad below heatmap */
            if (lg && i == li) { leg_top = hmtop; leg_bot = lblband; }
            if (gx_mode) {
                /* Draw ONLY the cells that exist, each at its own coordinate.
                 * Not a full-width raster: a 20 kb window is mostly not-a-CpG,
                 * and painting every pixel column would be both wasteful and a
                 * lie about where the data is. The background shows through the
                 * gaps, so density reads as density. */
                double bgspan = hmtop - lblband;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = spec->tobjs[i].has_bg ? spec->tobjs[i].bg_color : C_NA;
                g->sub = 1; g->clip = 1;
                g->x0 = 0; g->x1 = 1; g->y0 = lblband; g->y1 = hmtop;
                /* A CpG is 2 bp in a window tens of kb wide -- far under a
                 * pixel -- so a cell gets at least a hairline of width or the
                 * track renders blank. bar= overrides in bp. */
                double span = x1 - x0;
                double minw = span > 0 ? (0.75 / fmax(win_pt, 1.0)) * span : 1;
                for (int c = 0; c < nc; c++) {
                    double pb, pe;
                    if (spec->tobjs[i].bar_bp > 0) {
                        pb = m->colpos[c] - spec->tobjs[i].bar_bp / 2;
                        pe = pb + spec->tobjs[i].bar_bp;
                    } else {
                        pb = m->colbeg ? m->colbeg[c] : m->colpos[c];
                        pe = m->colend ? m->colend[c] : m->colpos[c] + 1;
                        if (pe - pb < minw) {
                            double mid = (pb + pe) / 2;
                            pb = mid - minw / 2; pe = mid + minw / 2;
                        }
                    }
                    double cx0 = NPCX(pb), cx1 = NPCX(pe);
                    if (cx1 <= 0 || cx0 >= 1) continue;          /* outside the window */
                    for (int rr = 0; rr < nr; rr++) {
                        double v = m->mv[(size_t)m->roword[rr] * nc + c];
                        if (isnan(v)) continue;                  /* background shows through */
                        g = gt_add(T, G_RECT, R, CC, R, CC);
                        g->col = CELL_COL(v);
                        g->sub = 1; g->clip = 1;
                        g->x0 = cx0; g->x1 = cx1;
                        g->y1 = hmtop - (double)rr / nr * bgspan;
                        g->y0 = hmtop - (double)(rr + 1) / nr * bgspan;
                    }
                }
            } else {
            int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, nc);
            unsigned char *buf = cp_xmalloc((size_t)nr * stride);
            for (int rr = 0; rr < nr; rr++) {
                uint32_t *row = (uint32_t *)(buf + (size_t)rr * stride);
                for (int c = 0; c < nc; c++) {
                    double v = m->mv[(size_t)m->roword[rr] * nc + c];
                    row[c] = isnan(v) ? col_argb(C_NA)
                           : col_argb(CELL_COL(v));
                }
            }
            g = gt_add(T, G_IMAGE, R, CC, R, CC);
            g->img = buf; g->img_w = nc; g->img_h = nr; g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = lblband; g->y1 = hmtop;
            }
#undef CELL_COL
            g = gt_add(T, G_RECT, R, CC, R, CC);             /* heatmap bounding box */
            Col bbh = {0.4, 0.4, 0.4};
            g->col = bbh; g->sub = 1; g->stroke = 1; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = lblband; g->y1 = hmtop;
            /* highlight() boxes: the sample's row band over the probe columns
             * whose genomic position falls in the span. The heatmap's x is
             * probe-index space (columns are evenly placed), so the box edges
             * are column edges, not the span's coordinates. */
            for (int b = 0; b < nboxes; b++) {
                TBox *bx = &boxes[b];
                if (bx->target && (!t->name || strcmp(bx->target, t->name))) continue;
                if (strcmp(bx->chrom, chrom) || bx->end <= rstart || bx->beg >= rend) continue;
                int kr = -1;
                for (int k = 0; k < nr; k++) if (!strcmp(m->rowname[k], bx->row)) { kr = k; break; }
                if (kr < 0) {
                    snprintf(err, CP_ERRLEN, "highlight(row=\"%s\"): not a sample row of "
                             "matrix `%s` (rows are the matrix's sample names)",
                             bx->row, t->name ? t->name : t->data);
                    return -1;
                }
                int rr = 0;
                for (int k = 0; k < nr; k++) if (m->roword[k] == kr) { rr = k; break; }
                int c0 = -1, c1 = -1;
                for (int c = 0; c < nc; c++)
                    if (m->colpos[c] >= bx->beg && m->colpos[c] < bx->end) {
                        if (c0 < 0) c0 = c;
                        c1 = c;
                    }
                if (c0 < 0) continue;              /* no probe in the span here */
                bx->placed = 1;
                double bx0 = (double)c0 / nc, bx1 = (double)(c1 + 1) / nc;
                double by1 = hmtop - (double)rr / nr * (hmtop - lblband);
                double by0 = hmtop - (double)(rr + 1) / nr * (hmtop - lblband);
                double *px = cp_xmalloc(5 * sizeof(double)), *py = cp_xmalloc(5 * sizeof(double));
                px[0] = bx0; py[0] = by0; px[1] = bx1; py[1] = by0;
                px[2] = bx1; py[2] = by1; px[3] = bx0; py[3] = by1;
                px[4] = bx0; py[4] = by0;
                g = gt_add(T, G_POLYLINE, R, CC, R, CC);
                g->n = 5; g->px = px; g->py = py; g->col = bx->col;
                g->lw = lw_pt(1); g->dash = bx->dash; g->clip = 1;
                if (bx->label) {                   /* tiny corner tag, box colour */
                    double lsz = sz_samp * 0.8;
                    double lh = cell_pt > 0 ? font_h(cr, lsz) / cell_pt : 0.02;
                    g = gt_add(T, G_TEXT, R, CC, R, CC);
                    g->str = (char *)bx->label; g->size = lsz; g->col = bx->col;
                    g->tx = bx1 + 0.002; g->ty = by1 - lh * 0.1;
                    g->hj = 0; g->va = V_TOP; g->clip = 1;
                }
            }
            g = gt_add(T, G_LINE, R, CC, R, CC);              /* kb axis baseline (top) */
            g->col = C_TICK; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = axline;
            double tick_npc = cell_pt > 0 ? TICK_LEN / cell_pt : 0.02;   /* fixed-pt */
            double txtoff = cell_pt > 0 ? (TXT_GAP * 0.5) / cell_pt : 0.008;
            for (int b = 0; b < nx; b++) {                    /* ticks below, numbers above */
                g = gt_add(T, G_LINE, R, CC, R, CC);
                g->col = C_TICK; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
                g->x0 = g->x1 = xpos[b]; g->y0 = axline; g->y1 = axline - tick_npc;
                g = gt_add(T, G_TEXT, R, CC, R, CC);
                g->str = xlab[b]; g->size = SZ_TRACK; g->col = C_AXTXT;
                g->tx = xtxt[b]; g->ty = axline + txtoff; g->hj = 0.5; g->va = V_BOTTOM;
            }
            Col mapc = {0.45, 0.45, 0.45};
            const int NB = 24;
            if (gx_mode) goto gx_no_fan;   /* nothing to lead to: cells sit at their own x */                                /* map lines: cubic-bezier
                * flow from the probe's genomic position to its column, with vertical
                * tangents at both ends (control points stacked below/above each end). */
            for (int c = 0; c < nc; c++) {
                double gx = NPCX(m->colpos[c]), cx = (c + 0.5) / nc, ym = (axline + hmtop) / 2;
                double *px = cp_xmalloc(NB * sizeof(double)), *py = cp_xmalloc(NB * sizeof(double));
                for (int s = 0; s < NB; s++) {
                    double u = 1.0 - (double)s / (NB - 1), tt = 1.0 - u;
                    double b0 = u*u*u, b1 = 3*u*u*tt, b2 = 3*u*tt*tt, b3 = tt*tt*tt;
                    px[s] = (b0 + b1) * gx + (b2 + b3) * cx;  /* P0,P1 at gx; P2,P3 at cx */
                    py[s] = b0 * axline + (b1 + b2) * ym + b3 * hmtop;
                }
                g = gt_add(T, G_POLYLINE, R, CC, R, CC);
                g->n = NB; g->px = px; g->py = py; g->col = mapc; g->lw = lw_pt(0.4); g->clip = 1;
            }
gx_no_fan:
            /* Probe IDs label COLUMNS; in genomic space there are none -- 550
             * ids over a 20 kb window would be an unreadable band regardless. */
            for (int c = 0; c < nc && !t->hide_colnames && !gx_mode; c++) {   /* probe IDs (rotated) */
                if (!m->colid[c]) continue;
                double lwn = cell_pt > 0 ? text_w(cr, sz_samp, m->colid[c]) / cell_pt : 0;
                g = gt_add(T, G_TEXT, R, CC, R, CC);
                g->str = m->colid[c]; g->size = sz_samp; g->col = C_BLACK;
                g->tx = (c + 0.5) / nc; g->ty = lbltop - lwn / 2; g->rot90 = 1;
            }
            /* sample labels (left), group names, swatches and rules: the
             * gutter shared with signal(); the left-most window writes it */
            draw_row_gutter(T, cr, R, CC, t, rcs[i], nrcs[i], ann[i], m->rowname, m->roword, nr,
                            hmtop, hmtop - lblband, labw, cell_pt, sz_samp,
                            !t->hide_rownames && wi == 0, wi == 0);
        } else if (t->type == TRK_SIGNAL) {
            /* Strips stacked like matrix() rows, one per sample, top to
             * bottom in file order; inside a strip one line per series at
             * the row's genomic midpoint, the raw polyline or its loess.
             * Each strip is its own lane: a value range of its own (or the
             * ylim= every strip shares), a faint baseline at the range's
             * low end, and a clip so an overshoot stays in its lane. */
            const SigData *d = sd[i];
            int ns = d->nstrip;
            double cell_pt = (t->height > 0 ? t->height : 1) * per_h;
            double sh = 1.0 / ns;
            /* A proportional inner pad keeps a trace off its own lane's edges;
             * the gap= is a FIXED blank between one lane and the next, so two
             * strips read as two lanes rather than one trace crossing a
             * baseline -- and it stays the same hairline at any track height. */
            double gap_npc = cell_pt > 0 ? (t->gap_pt >= 0 ? t->gap_pt : 2.0) / cell_pt : 0;
            if (gap_npc > sh * 0.4) gap_npc = sh * 0.4;         /* never eat the lane */
            double pad = 0.12 * sh + gap_npc / 2;
            double lw = lw_pt(t->line_lw > 0 ? t->line_lw : 0.5);
            if (t->name) {                                    /* rotated, gutter's left edge */
                double fh = font_h(cr, SZ_TRACK);
                g = gt_add(T, G_TEXT, R, 1, R, 1);
                g->str = t->name; g->size = SZ_TRACK; g->col = C_BLACK;
                g->tx = labw > 0 ? fh / 2 / labw : 0; g->ty = 0.5; g->rot90 = 1;
            }
            int *ident = cp_xmalloc((size_t)ns * sizeof(int));
            for (int k = 0; k < ns; k++) ident[k] = k;
            SigPt *pts = cp_xmalloc((size_t)(d->n ? d->n : 1) * sizeof(SigPt));
            const int NS = 200;                                /* loess output resolution */
            double *sx = cp_xmalloc((size_t)(d->n ? d->n : 1) * sizeof(double));
            double *sy = cp_xmalloc((size_t)(d->n ? d->n : 1) * sizeof(double));
            for (int k = 0; k < ns; k++) {
                double ybot = 1 - (k + 1) * sh, lo, hi;
                if (t->has_ylim) { lo = t->ylim_lo; hi = t->ylim_hi; }
                else { lo = d->lo[k]; hi = d->hi[k]; }
                if (!(hi > lo)) hi = lo + 1;                   /* a flat strip still has a lane */
#define SIG_Y(v) (ybot + pad + ((v) - lo) / (hi - lo) * (sh - 2 * pad))
                g = gt_add(T, G_LINE, R, CC, R, CC);           /* faint baseline */
                g->col = C_TGRID; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
                g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = SIG_Y(lo);
                for (int oi = d->nser - 1; oi >= 0; oi--) {   /* draworder[0] lands on top */
                    int si = d->draworder[oi];
                    int np = 0;
                    for (int r = 0; r < d->n; r++) {
                        if (d->strip[r] != k || d->series[r] != si) continue;
                        if (strcmp(d->chrom[r], chrom) || d->end[r] <= (double)rstart
                            || d->beg[r] >= (double)rend) continue;
                        pts[np].x = (d->beg[r] + d->end[r]) * 0.5;
                        pts[np].y = d->val[r]; np++;
                    }
                    if (np == 0) continue;                     /* nothing of it in this window */
                    /* stable: tied positions keep file order, so a rerun
                     * from the same input draws the same line */
                    for (int a = 1; a < np; a++) {
                        SigPt kp = pts[a]; int b2 = a - 1;
                        while (b2 >= 0 && cmp_sigpt(&pts[b2], &kp) > 0) { pts[b2+1] = pts[b2]; b2--; }
                        pts[b2+1] = kp;
                    }
                    Col col = d->sercol[si];
                    if (t->points) {                           /* the raw points, behind */
                        double *px = cp_xmalloc((size_t)np * sizeof(double));
                        double *py = cp_xmalloc((size_t)np * sizeof(double));
                        Col *pc = cp_xmalloc((size_t)np * sizeof(Col));
                        for (int q = 0; q < np; q++) { px[q] = NPCX(pts[q].x); py[q] = SIG_Y(pts[q].y); pc[q] = col; }
                        g = gt_add(T, G_POINTS, R, CC, R, CC);
                        g->n = np; g->px = px; g->py = py; g->pcol = pc;
                        g->radius = PT_RADIUS * 0.4; g->alpha = 0.35; g->clip = 1;
                        g->band_clip = 1; g->band_y0 = ybot; g->band_y1 = ybot + sh;
                    }
                    double *lx, *ly; int nl;
                    if (t->smooth > 0) {
                        if (np < 4) {
                            snprintf(err, CP_ERRLEN, "signal(smooth=%g): series `%s` of strip "
                                     "`%s` has %d point%s in %s:%ld-%ld and loess needs 4; "
                                     "use smooth=0 for the raw line", t->smooth,
                                     d->sername[si], d->stripname[k], np, np == 1 ? "" : "s",
                                     chrom, rstart, rend);
                            return -1;
                        }
                        for (int q = 0; q < np; q++) { sx[q] = pts[q].x; sy[q] = pts[q].y; }
                        double *ox = cp_xmalloc(NS * sizeof(double));
                        double *oy = cp_xmalloc(NS * sizeof(double));
                        nl = cp_loess(sx, sy, np, t->smooth, NS, ox, oy, err);
                        if (nl < 0) return -1;
                        lx = ox; ly = oy;
                    } else {
                        if (np < 2) {
                            fprintf(stderr, "cinderplot: signal `%s`: series `%s` of strip `%s` "
                                    "has one point in %s:%ld-%ld, so no line to draw%s\n",
                                    t->data, d->sername[si], d->stripname[k], chrom, rstart,
                                    rend, t->points ? "" : " (points=on would show it)");
                            continue;
                        }
                        nl = np;
                        lx = cp_xmalloc((size_t)nl * sizeof(double));
                        ly = cp_xmalloc((size_t)nl * sizeof(double));
                        for (int q = 0; q < nl; q++) { lx[q] = pts[q].x; ly[q] = pts[q].y; }
                    }
                    if (nl < 2) { free(lx); free(ly); continue; }
                    for (int q = 0; q < nl; q++) { lx[q] = NPCX(lx[q]); ly[q] = SIG_Y(ly[q]); }
                    g = gt_add(T, G_POLYLINE, R, CC, R, CC);
                    g->n = nl; g->px = lx; g->py = ly; g->col = col; g->lw = lw; g->clip = 1;
                    g->band_clip = 1; g->band_y0 = ybot; g->band_y1 = ybot + sh;
                }
#undef SIG_Y
            }
            free(pts); free(sx); free(sy);
            g = gt_add(T, G_RECT, R, CC, R, CC);           /* panel frame, like the matrix box */
            { Col bbh = {0.4, 0.4, 0.4};
              g->col = bbh; g->sub = 1; g->stroke = 1; g->lw = lw_pt(0.5) * cp_line_scale; g->clip = 1;
              g->x0 = 0; g->x1 = 1; g->y0 = 0; g->y1 = 1; }
            draw_row_gutter(T, cr, R, CC, t, rcs[i], nrcs[i], ann[i], d->stripname, ident, ns,
                            1.0, 1.0, labw, cell_pt, sz_samp, wi == 0, wi == 0);
        }
    }
      if (!has_matrix) {
          /* Each window needs its own axis arrays: xpos/xtxt/xlab are scratch
           * arrays reused when the next window is laid out. Labels near an edge
           * use xtxt so their final glyph remains inside the output surface. */
          double *axis_pos = cp_xmalloc(nx * sizeof(double));
          double *axis_txt = cp_xmalloc(nx * sizeof(double));
          char **axis_lab = cp_xmalloc(nx * sizeof(char *));
          memcpy(axis_pos, xpos, nx * sizeof(double));
          memcpy(axis_txt, xtxt, nx * sizeof(double));
          memcpy(axis_lab, xlab, nx * sizeof(char *));
          g = gt_add(T, G_AXIS_X, axisrow, CC, axisrow, CC);
          g->size = SZ_TRACK;            /* one flat size, the axis numbers included */
          g->n = nx; g->px = axis_pos; g->label_pos = axis_txt;
          g->labels = axis_lab;
      }
    }

    /* ---- the legend, in the right-margin column of the matrix track's row,
     * centred on its heatmap band; the same key/colourbar grobs heatmap mode
     * emits, so the two modes cannot disagree on what a key looks like ---- */
    if (lg) {
        int R = TRK(li), C = T->ncol - 1;
        double wgt = spec->tobjs[li].height > 0 ? spec->tobjs[li].height : 1;
        double ch_pt = wgt * per_h > 0 ? wgt * per_h : 1, cw_pt = rmargin;
        double top = (leg_top + leg_bot) / 2 + leg_h / ch_pt / 2;
        double sx = HALF_LINE / cw_pt;
        double titleSpace = leg_title ? (baseH + TXT_GAP) / ch_pt : 0;
        if (leg_disc) {
            cp_key_draw(T, R, C, klab, kpal, nk, sx, top - titleSpace, cw_pt, ch_pt);
        } else {
            FillScale fs = spec->has_fill ? spec->fill : (FillScale){0};
            if (!spec->has_fill) fs.kind = FILL_PARULA;
            cp_colourbar_draw(T, R, C, &fs, 0, 1, 1, 1, sx,
                              top - titleSpace - LEG_LEN / ch_pt, cw_pt, ch_pt);
        }
        if (leg_title) {
            g = gt_add(T, G_TEXT, R, C, R, C);
            g->str = leg_title; g->size = SZ_TRACK; g->col = C_BLACK;
            g->tx = sx; g->ty = top; g->hj = 0; g->va = V_TOP;
        }
    }

    if (nboxes) {
        int lost = 0;
        for (int b = 0; b < nboxes; b++) lost += !boxes[b].placed;
        if (lost)
            fprintf(stderr, "cinderplot: warning: highlight(): %d of %d box%s outside "
                    "every panel, or covering no probe column, %s not drawn\n",
                    lost, nboxes, nboxes == 1 ? "" : "es",
                    lost == 1 ? "was" : "were");
    }

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_status_t st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) { snprintf(err, CP_ERRLEN, "cairo: %s", cairo_status_to_string(st)); return -1; }
    return 0;
}
