/* render_tracks.c — locus track-browser mode: a stack of heterogeneous
 * tracks (coverage / interval / gene-model / arc) sharing one genomic
 * x-axis over a single region. A third assembler alongside render.c
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
    double *colpos;    /* nc probe genomic positions (sorted) */
    char **colid;      /* nc probe IDs (may be NULL entries) */
    char **rowname;    /* nr sample names */
    int *roword;       /* nr display order (cluster or identity) */
    char *chrom;       /* chromosome (borrowed or owned) — for region() inference */
    long gbeg, gend;   /* min(beg), max(end) over probes — for region() inference */
    int multichrom;    /* input spans >1 chromosome (inference is ambiguous) */
} MatData;

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
    const Column *samp = NULL;                        /* string col => long/tidy */
    for (int c = 0; c < mf->ncol; c++)
        if (mf->cols[c].type == COL_STR && &mf->cols[c] != chr_c && &mf->cols[c] != pid)
            { samp = &mf->cols[c]; break; }
    MatData *m = cp_xcalloc(1, sizeof *m);
    int nr = 0, nc = 0;
    if (samp) {                                       /* --- long/tidy --- */
        if (!pid) { snprintf(err, CP_ERRLEN, "long matrix needs a Probe_ID column"); return NULL; }
        const Column *val = NULL;
        for (int c = 0; c < mf->ncol; c++)
            if (mf->cols[c].type == COL_NUM && &mf->cols[c] != chr_c
                && &mf->cols[c] != sc && &mf->cols[c] != ec)
                { val = &mf->cols[c]; break; }
        if (!val) { snprintf(err, CP_ERRLEN, "long matrix needs a numeric value column"); return NULL; }
        char **pn = cp_xmalloc(mf->nrow * sizeof(char *));
        double *pp = cp_xmalloc(mf->nrow * sizeof(double));
        char **sn = cp_xmalloc(mf->nrow * sizeof(char *));
#define IN_WIN(r) matrix_in_window(chr_c, sc, ec, r, win_chrom, \
                                    win_beg, win_end)
        for (int r = 0; r < mf->nrow; r++) {
            if (!IN_WIN(r)) continue;
            const char *id = pid->str[r]; int f = -1;
            for (int i = 0; i < nc; i++) if (!strcmp(pn[i], id)) { f = i; break; }
            if (f < 0) { pn[nc] = (char *)id; pp[nc] = (sc->num[r] + ec->num[r]) * 0.5; nc++; }
            const char *s = samp->str[r]; f = -1;
            for (int i = 0; i < nr; i++) if (!strcmp(sn[i], s)) { f = i; break; }
            if (f < 0) sn[nr++] = (char *)s;
        }
        if (nc < 1 || nr < 1) { snprintf(err, CP_ERRLEN, "matrix is empty"); return NULL; }
        int *ord = cp_xmalloc(nc * sizeof(int));
        for (int i = 0; i < nc; i++) ord[i] = i;
        for (int a = 1; a < nc; a++) { int k = ord[a]; double kp = pp[k]; int b2 = a - 1;
            while (b2 >= 0 && pp[ord[b2]] > kp) { ord[b2+1] = ord[b2]; b2--; } ord[b2+1] = k; }
        m->colpos = cp_xmalloc(nc * sizeof(double)); m->colid = cp_xmalloc(nc * sizeof(char *));
        for (int c = 0; c < nc; c++) { m->colpos[c] = pp[ord[c]]; m->colid[c] = pn[ord[c]]; }
        m->rowname = sn;
        m->mv = cp_xmalloc((size_t)nr * nc * sizeof(double));
        for (size_t i = 0; i < (size_t)nr * nc; i++) m->mv[i] = NAN;
        for (int r = 0; r < mf->nrow; r++) {
            if (!IN_WIN(r)) continue;
            const char *id = pid->str[r], *s = samp->str[r];
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
                && &mf->cols[c] != sc && &mf->cols[c] != ec)
                scol[ns++] = c;
        if (ns < 1) { snprintf(err, CP_ERRLEN, "matrix has no numeric sample columns"); return NULL; }
        if (mf->nrow < 1) { snprintf(err, CP_ERRLEN, "matrix is empty"); return NULL; }
        int *ord = cp_xmalloc(mf->nrow * sizeof(int));
        double *pp = cp_xmalloc(mf->nrow * sizeof(double));
        for (int r = 0; r < mf->nrow; r++) {
            if (!matrix_in_window(chr_c, sc, ec, r, win_chrom,
                                  win_beg, win_end)) continue;
            pp[r] = (sc->num[r] + ec->num[r]) * 0.5;
            ord[nc++] = r;
        }
        if (nc < 1) {
            snprintf(err, CP_ERRLEN, "matrix is empty in the requested window");
            free(ord); free(pp);
            return NULL;
        }
        for (int a = 1; a < nc; a++) { int k = ord[a]; double kp = pp[k]; int b2 = a - 1;
            while (b2 >= 0 && pp[ord[b2]] > kp) { ord[b2+1] = ord[b2]; b2--; } ord[b2+1] = k; }
        nr = ns;
        m->colpos = cp_xmalloc(nc * sizeof(double)); m->colid = cp_xmalloc(nc * sizeof(char *));
        for (int c = 0; c < nc; c++) { m->colpos[c] = pp[ord[c]]; m->colid[c] = pid ? pid->str[ord[c]] : NULL; }
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
    if (c0->type != COL_STR) {
        snprintf(err, CP_ERRLEN, "regions `%s`: first column must be the chromosome "
                 "name", path);
        return NULL;
    }
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
        snprintf(w[n].chrom, sizeof w[n].chrom, "%s", c0->str[r]);
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


/* Re-order `m`'s display rows so sample i of `ref` is drawn at row i here.
 * Windows are read independently, so each would otherwise order rows by
 * whatever it encountered first; across one figure the samples must line up.
 * Samples absent from this window keep their own slots at the end. */
static void match_row_order(MatData *m, const MatData *ref) {
    int *ord = cp_xmalloc(m->nr * sizeof(int));
    char *taken = cp_xcalloc(m->nr, 1);
    int n = 0;
    for (int i = 0; i < ref->nr && n < m->nr; i++) {
        const char *want = ref->rowname[ref->roword[i]];
        for (int j = 0; j < m->nr; j++)
            if (!taken[j] && !strcmp(m->rowname[j], want)) { ord[n++] = j; taken[j] = 1; break; }
    }
    for (int j = 0; j < m->nr && n < m->nr; j++) if (!taken[j]) ord[n++] = j;
    memcpy(m->roword, ord, m->nr * sizeof(int));
    free(ord); free(taken);
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
    int have_pre = spec->region
                   && region_parse(spec->region, pre_chrom, &pre_beg, &pre_end) == 0;
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].type == TRK_MATRIX) {
            md[i] = read_matrix(&spec->tobjs[i], have_pre ? pre_chrom : NULL,
                                pre_beg, pre_end, err);
            if (!md[i]) return -1;
            rowref[i] = md[i];
            has_matrix = 1;
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
                && spec->tobjs[i].type != TRK_INTERVAL) {
                snprintf(err, CP_ERRLEN, "regions() supports matrix(), genes() and "
                         "interval() tracks so far; the remaining track types are "
                         "being added one at a time");
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
            snprintf(err, CP_ERRLEN, "bad region `%s`; expected chr:start-end", spec->region); return -1;
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
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

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

    /* font levels within ~1.5x: title 9.5, medium SZ_AXIS_TEXT (8.8), small 6.5. */
    double sz_title = 9.5, sz_samp = 6.5;             /* per-row sample/probe labels */

    /* widest gene label -> reserved right margin, so transcript names always fit
     * to the right of their model (no left-flip collisions at narrow widths) */
    double gene_labw = 0;
    for (int i = 0; i < ntr; i++)
        if (spec->tobjs[i].type == TRK_GENES) {
            int ng; GeneModel *gm = load_genes(spec->tobjs[i].data, chrom, rstart, rend,
                                               spec->tobjs[i].all_transcripts, &ng, err);
            if (gm) for (int k = 0; k < ng; k++)
                if (gm[k].name) {
                    double w = text_w(cr, SZ_AXIS_TEXT, gm[k].name);
                    if (w > gene_labw) gene_labw = w;
                }
        }
    double rmargin = gene_labw > 0 ? gene_labw + HALF_LINE : MARGIN;

    /* ---- measure left label column from track names + matrix sample names ---- */
    double labw = 0;
    for (int i = 0; i < ntr; i++) {
        if (spec->tobjs[i].name && spec->tobjs[i].type != TRK_MATRIX) {
            double w = text_w(cr, SZ_AXIS_TEXT, spec->tobjs[i].name);
            if (w > labw) labw = w;
        }
        if (md[i] && !spec->tobjs[i].hide_rownames)
            for (int r = 0; r < md[i]->nr; r++) {
                double w = text_w(cr, sz_samp, md[i]->rowname[r]);
                if (w > labw) labw = w;
            }
    }
    const char *title = spec->lab_title ? spec->lab_title : (spec->region ? spec->region : rgn_disp);
    double titleh = title ? font_h(cr, sz_title) : 0;
    double axh = font_h(cr, SZ_AXIS_TEXT);
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
                double bands = font_h(cr, SZ_AXIS_TEXT) + TICK_LEN + TXT_GAP + 42 + lbl_pt + lab_pad;
                double row_h = spec->tobjs[i].hide_rownames ? AUTO_MIN_CELL : samp_line * AUTO_ROW_PAD;
                double u = (bands + md[i]->nr * row_h) / wgt;
                if (u > per_u) per_u = u;
                break;
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
    cairo_select_font_face(cr, FONT_FAMILY, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

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

    Grob *g;
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
                  match_row_order(md[i], rowref[i]);
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
              double npc = NPCX(bp), half = text_w(cr, SZ_AXIS_TEXT, lab) / 2;
              double centre = npc * win_pt;
              /* A tick near the edge would centre its label half outside the
               * panel -- in the gap or the next window. Keep the tick where it
               * belongs and slide the text inward, as a genome browser does,
               * rather than dropping a narrow panel's only coordinate. The
               * overlap test then has to run on the *slid* position, or two
               * nudged labels collide in the middle. */
              double tc = centre;
              if (wins) {
                  if (tc - half < 0) tc = half;
                  if (tc + half > win_pt) tc = win_pt - half;
              }
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
        if (spec->tobjs[i].name && tt != TRK_MATRIX) {           /* left label */
            g = gt_add(T, G_TEXT, R, 1, R, 1);
            g->str = spec->tobjs[i].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
            g->tx = 1; g->ty = 0.5; g->hj = 1; g->va = V_INKCENTER;
        }

        /* ---- track content ---- */
        const TrackObj *t = &spec->tobjs[i];
        if (t->type == TRK_COVERAGE) {
            int nsb; SigBin *sb = bedgraph_read(t->data, chrom, rstart, rend, &nsb, err);
            if (!sb) return -1;
            double ymax = t->max_value > 0 ? t->max_value : 0;
            if (ymax <= 0) for (int k = 0; k < nsb; k++) if (sb[k].val > ymax) ymax = sb[k].val;
            if (ymax <= 0) ymax = 1;
            Col col = t->has_color ? t->color : C_COV;
            for (int k = 0; k < nsb; k++) {
                if (sb[k].val <= 0) continue;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(sb[k].start); g->x1 = NPCX(sb[k].end);
                g->y0 = 0; g->y1 = sb[k].val / ymax;
            }
            char *rd = cp_xmalloc(32); snprintf(rd, 32, "[0 - %g]", ymax);   /* readout */
            g = gt_add(T, G_TEXT, R, CC, R, CC);
            g->str = rd; g->size = SZ_AXIS_TEXT; g->col = C_AXTXT;
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
                    ? (long)((text_w(cr, SZ_AXIS_TEXT, gm[k].name) + HALF_LINE) * bp_per_pt) : 0;
                int L = -1;
                for (int j = 0; j < nlanes; j++) if (laneend[j] <= gm[k].tx_start) { L = j; break; }
                if (L < 0 && nlanes < 64) L = nlanes++;
                if (L < 0) L = nlanes - 1;
                lane[k] = L; laneend[L] = gm[k].tx_end + lw_bp;
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
                    /* With one window the name may run into the reserved right
                     * margin unclipped. With several it must not: the margin
                     * belongs to the next window, and an overflowing name lands
                     * inside its neighbour's panel. Skip it when it cannot fit,
                     * and clip what does. */
                    double tx = xb + HALF_LINE / win_pt;
                    double need = text_w(cr, SZ_AXIS_TEXT, gm[k].name) / win_pt;
                    if (!wins || tx + need <= 1.0) {
                        g = gt_add(T, G_TEXT, R, CC, R, CC);
                        g->str = gm[k].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
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
            Col col = t->has_color ? t->color : C_IVAL;
            double lh = 1.0 / nlanes;
            for (int k = 0; k < ni; k++) {
                double yb = 1 - (lane[k] + 1) * lh;
                g = gt_add(T, G_RECT, R, CC, R, CC);
                g->col = col; g->sub = 1; g->clip = 1;
                g->x0 = NPCX(iv[k].start); g->x1 = NPCX(iv[k].end);
                g->y0 = yb + 0.18 * lh; g->y1 = yb + 0.82 * lh;
                if (iv[k].name) {                        /* name to the right */
                    double tx = NPCX(iv[k].end) + 0.004;
                    double need = text_w(cr, SZ_AXIS_TEXT, iv[k].name) / win_pt;
                    if (!wins || tx + need <= 1.0) {       /* else it lands in the neighbour */
                        g = gt_add(T, G_TEXT, R, CC, R, CC);
                        g->str = iv[k].name; g->size = SZ_AXIS_TEXT; g->col = C_BLACK;
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
                snprintf(err, CP_ERRLEN, "cytoband needs chrom,start,end,stain columns"); return -1;
            }
            double clen = 0; int nband = 0;
            for (int r2 = 0; r2 < cb->nrow; r2++)
                if (!strcmp(bc->str[r2], chrom)) { nband++; if (be->num[r2] > clen) clen = be->num[r2]; }
            if (clen <= 0) { snprintf(err, CP_ERRLEN, "chromosome %s not in cytoband file", chrom); return -1; }
            double *bst = cp_xmalloc(nband * sizeof(double)), *ben = cp_xmalloc(nband * sizeof(double));
            Col *bcol = cp_xmalloc(nband * sizeof(Col));
            double cen_lo = 1, cen_hi = 0;                   /* centromere (acen) extent, npc */
            int nb2 = 0;
            for (int r2 = 0; r2 < cb->nrow; r2++) {
                if (strcmp(bc->str[r2], chrom)) continue;
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
        } else if (t->type == TRK_MATRIX) {
            /* genome-anchored heatmap (rows = samples, cols = probes evenly
             * placed), read up front into md[i]. Cell bands (npc, y up):
             * [axline,1] kb axis, [hmtop,axline] map lines, [lblband,hmtop]
             * heatmap, [0,lblband] probe IDs. */
            MatData *m = md[i];
            int nr = m->nr, nc = m->nc;
            FillScale fs = spec->has_fill ? spec->fill : (FillScale){0};
            if (!spec->has_fill) fs.kind = FILL_PARULA;       /* beta default */
            /* bands sized in FIXED points (constant gaps at any figure size). */
            double cell_pt = (spec->tobjs[i].height > 0 ? spec->tobjs[i].height : 1) * per_h;
            double axtop_pt = font_h(cr, SZ_AXIS_TEXT) + TICK_LEN + TXT_GAP;   /* kb axis */
            double mapband_pt = 42;                                            /* bezier band */
            double lbl_pt = 0;                                                 /* rotated probe IDs */
            if (!spec->tobjs[i].hide_colnames)
                for (int c = 0; c < nc; c++)
                    if (m->colid[c]) { double w = text_w(cr, sz_samp, m->colid[c]); if (w > lbl_pt) lbl_pt = w; }
            double axline  = cell_pt > 0 ? 1 - axtop_pt / cell_pt : 0.94;
            double hmtop   = cell_pt > 0 ? axline - mapband_pt / cell_pt : 0.80;
            double lblband = cell_pt > 0 ? (lbl_pt + lab_pad) / cell_pt : 0.10;  /* labels + lab_pad gap */
            double lbltop  = cell_pt > 0 ? lbl_pt / cell_pt : lblband * 0.85;    /* label tops = lab_pad below heatmap */
            int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, nc);
            unsigned char *buf = cp_xmalloc((size_t)nr * stride);
            for (int rr = 0; rr < nr; rr++) {
                uint32_t *row = (uint32_t *)(buf + (size_t)rr * stride);
                for (int c = 0; c < nc; c++) {
                    double v = m->mv[(size_t)m->roword[rr] * nc + c];
                    row[c] = isnan(v) ? col_argb(C_NA)
                           : col_argb(fill_map_value(&fs, v, 0, 1));
                }
            }
            g = gt_add(T, G_IMAGE, R, CC, R, CC);
            g->img = buf; g->img_w = nc; g->img_h = nr; g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = lblband; g->y1 = hmtop;
            g = gt_add(T, G_RECT, R, CC, R, CC);             /* heatmap bounding box */
            Col bbh = {0.4, 0.4, 0.4};
            g->col = bbh; g->sub = 1; g->stroke = 1; g->lw = lw_pt(0.5); g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = lblband; g->y1 = hmtop;
            g = gt_add(T, G_LINE, R, CC, R, CC);              /* kb axis baseline (top) */
            g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
            g->x0 = 0; g->x1 = 1; g->y0 = g->y1 = axline;
            double tick_npc = cell_pt > 0 ? TICK_LEN / cell_pt : 0.02;   /* fixed-pt */
            double txtoff = cell_pt > 0 ? (TXT_GAP * 0.5) / cell_pt : 0.008;
            for (int b = 0; b < nx; b++) {                    /* ticks below, numbers above */
                g = gt_add(T, G_LINE, R, CC, R, CC);
                g->col = C_TICK; g->lw = lw_pt(0.5); g->clip = 1;
                g->x0 = g->x1 = xpos[b]; g->y0 = axline; g->y1 = axline - tick_npc;
                g = gt_add(T, G_TEXT, R, CC, R, CC);
                g->str = xlab[b]; g->size = SZ_AXIS_TEXT; g->col = C_AXTXT;
                g->tx = xtxt[b]; g->ty = axline + txtoff; g->hj = 0.5; g->va = V_BOTTOM;
            }
            Col mapc = {0.45, 0.45, 0.45};
            const int NB = 24;                                /* map lines: cubic-bezier
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
            for (int c = 0; c < nc && !t->hide_colnames; c++) {   /* probe IDs (rotated) */
                if (!m->colid[c]) continue;
                double lwn = cell_pt > 0 ? text_w(cr, sz_samp, m->colid[c]) / cell_pt : 0;
                g = gt_add(T, G_TEXT, R, CC, R, CC);
                g->str = m->colid[c]; g->size = sz_samp; g->col = C_BLACK;
                g->tx = (c + 0.5) / nc; g->ty = lbltop - lwn / 2; g->rot90 = 1;
            }
            double hh = hmtop - lblband;                      /* sample labels (left) */
            /* The row-label gutter is shared by every window, so only the
             * left-most one writes into it -- otherwise each window stamps the
             * same names over the last. */
            if (!t->hide_rownames && wi == 0)
                for (int rr = 0; rr < nr; rr++) {
                    g = gt_add(T, G_TEXT, R, 1, R, 1);
                    g->str = m->rowname[m->roword[rr]]; g->size = sz_samp; g->col = C_BLACK;
                    g->tx = 1; g->ty = hmtop - (rr + 0.5) / nr * hh; g->hj = 1; g->va = V_INKCENTER;
                }
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
          g->n = nx; g->px = axis_pos; g->label_pos = axis_txt;
          g->labels = axis_lab;
      }
    }

    gt_resolve(T, 0, 0, w_pt, h_pt);
    gt_render(T, cr);

    cairo_destroy(cr);
    cairo_status_t st = cp_surface_emit(surf, out);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) { snprintf(err, CP_ERRLEN, "cairo: %s", cairo_status_to_string(st)); return -1; }
    return 0;
}
