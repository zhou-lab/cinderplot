/* csv.c — CSV loader with column typing.
 *
 * Reads a CSV (path or "-" for stdin) into a DataFrame. Handles quoted
 * fields ("" escapes, embedded commas/newlines), CRLF, and a header row.
 * A column is typed COL_NUM iff every non-empty, non-"NA" cell parses
 * fully as a number (empty/NA become NaN); otherwise COL_STR.
 * factor_make() builds sorted unique levels: numeric sources sort
 * numerically (labels via fmt_num), strings lexically — matching R's
 * default factor level order. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char **v; int n, cap; } StrVec;

static void sv_push(StrVec *s, char *p) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = cp_xrealloc(s->v, s->cap * sizeof(char *)); }
    s->v[s->n++] = p;
}

/* Grow to `want` slots up front. Callers know the row count before parsing
 * (records are counted by newline), so the per-column arrays are sized once
 * instead of doubling ~24 times on a 10^7-row input. */
static void sv_reserve(StrVec *s, int want) {
    if (s->cap >= want) return;
    s->cap = want;
    s->v = cp_xrealloc(s->v, (size_t)want * sizeof(char *));
}

static char *read_all(FILE *f) {
    size_t cap = 1 << 16, n = 0;
    struct stat st;
    /* A regular file's size is known, so allocate the image exactly once; the
     * doubling loop below then never fires. Pipes and stdin keep the old path. */
    if (!fstat(fileno(f), &st) && S_ISREG(st.st_mode) && st.st_size > 0)
        cap = (size_t)st.st_size + 1;
    char *buf = cp_xmalloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; buf = cp_xrealloc(buf, cap); }
    }
    buf[n] = 0;
    return buf;
}

/* Split one record starting at *p using `delim` (',' or '\t'), advancing *p
 * past the record's newline; returns the field count, 0 at end of input, -1 on
 * a malformed quoted field.
 *
 * Fields are *borrowed slices of the input image*, not copies: the terminator
 * after each field is overwritten with NUL in place, and a quoted field is
 * unescaped in place (unescaping only ever shrinks, so the write cursor stays
 * behind the read cursor). This removes one malloc and one memcpy per cell —
 * on a 10^7-row, 3-column input that is 3x10^7 allocations avoided. The cost
 * is that the image must outlive any COL_STR column, which DataFrame.backing
 * now owns. */
static int split_record(char **p, StrVec *fields, char delim) {
    char stop[4] = {delim, '\r', '\n', 0};
    char *s = *p;
    if (!*s) return 0;
    int nf = 0;
    for (;;) {
        char *out;
        if (*s == '"') {
            char *o = out = ++s;                 /* unescape in place */
            int closed = 0;
            while (*s) {
                if (*s == '"' && s[1] == '"') { *o++ = '"'; s += 2; }
                else if (*s == '"') { closed = 1; s++; break; }
                else *o++ = *s++;
            }
            if (!closed) return -1;
            if (*s && *s != delim && *s != '\r' && *s != '\n') return -1;
            *o = 0;                              /* o <= s, so this is safe */
        } else {
            out = s;
            s += strcspn(s, stop);
        }
        /* Read the terminator before clobbering it — after the NUL write the
         * character it replaced is gone, so branch on the saved copy. */
        char t = *s;
        if (t) *s = 0;
        sv_push(fields, out);
        nf++;
        if (t == delim) { s++; continue; }
        if (t == '\r') { s++; if (*s == '\n') s++; }
        else if (t == '\n') s++;
        break;
    }
    *p = s;
    return nf;
}

static int is_na(const char *s) {
    return !*s || !strcmp(s, "NA") || !strcmp(s, "na") || !strcmp(s, "NaN");
}

static int g_no_header = 0;                          /* treat inputs as headerless */
void cp_set_no_header(int on) { g_no_header = on; }

/* One-shot column filter naming the columns the next df_read_csv() must keep.
 * It is consumed and cleared by that call, so only the plot's primary data
 * file is pruned: per-layer data, seqinfo, cytoband and the heatmap/track
 * readers, which all consume whole matrices, still see every column. NULL or
 * a zero count keeps everything. Pruning matters because an unreferenced
 * column otherwise costs a pointer per row plus a strtod per cell, and on wide
 * genomic tables most columns go unplotted. */
static char *const *g_needed = NULL;
static int g_nneeded = 0;
void cp_set_needed_cols(char *const *names, int n) {
    g_needed = names;
    g_nneeded = n;
}

DataFrame *df_read_csv(const char *path, char *err) {
    if (!strcmp(path, "stdin")) path = "-";          /* alias for stdin */
    if (!strcmp(path, "-") && isatty(fileno(stdin))) {
        snprintf(err, CP_ERRLEN, "no input: stdin is a terminal — pipe data in or name a file");
        return NULL;
    }
    char *buf;
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) {      /* gzip / bgzip TSV */
        buf = gz_read_all(path, err);
        if (!buf) return NULL;
    } else {
        FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
        if (!f) { snprintf(err, CP_ERRLEN, "cannot open %s", path); return NULL; }
        buf = read_all(f);
        if (f != stdin) fclose(f);
    }

    /* sniff the delimiter: tab in the first line => TSV, else comma */
    char delim = ',';
    for (const char *q = buf; *q && *q != '\n'; q++)
        if (*q == '\t') { delim = '\t'; break; }

    char *p = buf;
    StrVec header = {0};
    int hn = split_record(&p, &header, delim);
    if (hn < 0) {
        snprintf(err, CP_ERRLEN, "%s: malformed quoted field in header", path);
        free(header.v); free(buf);
        return NULL;
    }
    if (!hn || header.n == 0) {
        snprintf(err, CP_ERRLEN, "%s: empty file", path);
        free(buf); free(header.v);
        return NULL;
    }
    int ncol_file = header.n;            /* columns present in the file */
    int noh = g_no_header;               /* headerless: names V1.. + first record is data */
    int rowbase = noh ? 1 : 2;           /* file row number of the first *data* line */

    /* Column names are copied out of the image, because the image itself may
     * be released below once every column has typed numeric. */
    char **fname = cp_xcalloc(ncol_file, sizeof(char *));
    for (int c = 0; c < ncol_file; c++) {
        if (noh) { fname[c] = cp_xmalloc(16); snprintf(fname[c], 16, "V%d", c + 1); }
        else       fname[c] = cp_xstrdup(header.v[c]);
    }

    /* Consume the one-shot filter and mark which columns survive. Records are
     * still split in full — the field boundaries are needed to find the end of
     * the row — but a dropped column is never stored, typed, or converted. */
    char *const *needed = g_needed;
    int nneeded = g_nneeded;
    g_needed = NULL; g_nneeded = 0;
    char *keep = cp_xmalloc(ncol_file);
    int ncol = 0;
    for (int c = 0; c < ncol_file; c++) {
        int want = 1;
        if (needed && nneeded) {
            want = 0;
            for (int i = 0; i < nneeded; i++)
                if (needed[i] && !strcmp(fname[c], needed[i])) { want = 1; break; }
        }
        keep[c] = (char)want;
        ncol += want;
    }

    /* One newline per record is an upper bound on the row count (a trailing
     * line without a newline is covered by the +1), so every column array is
     * sized once here rather than grown by repeated doubling. */
    int est = 1 + (noh ? 1 : 0);     /* headerless pushes the first record too */
    for (const char *q = p; (q = strchr(q, '\n')) != NULL; q++) est++;

    StrVec *cells = cp_xcalloc(ncol_file, sizeof(StrVec));
    for (int c = 0; c < ncol_file; c++) if (keep[c]) sv_reserve(&cells[c], est);
    int nrow = 0;
    if (noh) {                           /* the first record is data, not column names */
        for (int c = 0; c < ncol_file; c++) if (keep[c]) sv_push(&cells[c], header.v[c]);
        nrow = 1;
    }
    StrVec rec = {0};                    /* reused across records, not per row */
    sv_reserve(&rec, ncol_file);
    for (;;) {
        rec.n = 0;
        int nf = split_record(&p, &rec, delim);
        if (nf < 0) {
            snprintf(err, CP_ERRLEN, "%s: malformed quoted field in row %d", path, nrow + rowbase);
            free(rec.v); free(buf);
            return NULL;
        }
        if (nf == 0) break;
        if (nf == 1 && !*rec.v[0]) continue;                        /* blank line */
        if (nf != ncol_file) {
            snprintf(err, CP_ERRLEN, "%s: row %d has %d fields, expected %d", path, nrow + rowbase, nf, ncol_file);
            free(rec.v); free(buf);
            return NULL;
        }
        for (int c = 0; c < ncol_file; c++) if (keep[c]) sv_push(&cells[c], rec.v[c]);
        nrow++;
    }
    free(rec.v);

    DataFrame *df = cp_xmalloc(sizeof *df);
    df->nrow = nrow; df->ncol = ncol;
    df->cols = cp_xcalloc(ncol, sizeof(Column));
    int any_str = 0;
    int k = 0;
    for (int c = 0; c < ncol_file; c++) {
        if (!keep[c]) { free(fname[c]); continue; }   /* pruned: never stored */
        Column *col = &df->cols[k++];
        col->name = fname[c];
        /* Type and convert in a single pass: write into the double array while
         * checking, and abandon it at the first non-numeric cell. The old code
         * ran strtod over every cell twice, once to decide the type and once to
         * convert. A string column bails at its first cell, so the speculative
         * allocation is cheap. */
        col->num = cp_xmalloc((size_t)nrow * sizeof(double));
        int numeric = 1;
        for (int r = 0; r < nrow; r++) {
            const char *s = cells[c].v[r];
            if (is_na(s)) { col->num[r] = NAN; continue; }
            char *end;
            double d = strtod(s, &end);
            if (end == s || *end) { numeric = 0; break; }
            col->num[r] = d;
        }
        if (numeric) {
            col->type = COL_NUM;
            free(cells[c].v);           /* the slices are no longer referenced */
        } else {
            free(col->num);
            col->num = NULL;
            col->type = COL_STR;
            col->str = cells[c].v;      /* slices into the image; see backing */
            any_str = 1;
        }
    }
    free(cells);        /* per-column .v arrays are transferred to df or freed above */
    free(header.v);
    free(fname);        /* the surviving name pointers are owned by the columns */
    free(keep);
    /* Nothing points into the image once every column is numeric, so hand the
     * memory back rather than holding the whole file for the render. */
    if (any_str) df->backing = buf;
    else { free(buf); df->backing = NULL; }
    return df;
}

const Column *df_col(const DataFrame *df, const char *name) {
    for (int c = 0; c < df->ncol; c++)
        if (!strcmp(df->cols[c].name, name)) return &df->cols[c];
    return NULL;
}

/* ---------------- factor ---------------- */
static int cmp_dbl(const void *a, const void *b) {
    double d = *(const double *)a - *(const double *)b;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

Factor *factor_make(const DataFrame *df, const Column *c) {
    Factor *f = cp_xmalloc(sizeof *f);
    f->idx = cp_xmalloc(df->nrow * sizeof(int));
    if (c->type == COL_NUM) {
        double *uniq = cp_xmalloc(df->nrow * sizeof(double));
        int nu = 0;
        for (int r = 0; r < df->nrow; r++) {
            if (isnan(c->num[r])) continue;
            int seen = 0;
            for (int i = 0; i < nu; i++) if (uniq[i] == c->num[r]) { seen = 1; break; }
            if (!seen) uniq[nu++] = c->num[r];
        }
        qsort(uniq, nu, sizeof(double), cmp_dbl);
        f->nlev = nu;
        f->levels = cp_xmalloc(nu * sizeof(char *));
        for (int i = 0; i < nu; i++) {
            f->levels[i] = cp_xmalloc(32);
            fmt_num(uniq[i], f->levels[i], 32);
        }
        for (int r = 0; r < df->nrow; r++) {
            f->idx[r] = -1;
            for (int i = 0; i < nu; i++)
                if (!isnan(c->num[r]) && c->num[r] == uniq[i]) { f->idx[r] = i; break; }
        }
        free(uniq);
    } else {
        char **uniq = cp_xmalloc(df->nrow * sizeof(char *));
        int nu = 0;
        for (int r = 0; r < df->nrow; r++) {
            if (is_na(c->str[r])) continue;
            int seen = 0;
            for (int i = 0; i < nu; i++) if (!strcmp(uniq[i], c->str[r])) { seen = 1; break; }
            if (!seen) uniq[nu++] = c->str[r];
        }
        qsort(uniq, nu, sizeof(char *), cmp_str);
        f->nlev = nu;
        /* own the level strings (strdup), matching the numeric branch above, so
         * a Factor never borrows into the DataFrame's column storage */
        f->levels = cp_xmalloc(nu * sizeof(char *));
        for (int i = 0; i < nu; i++) f->levels[i] = cp_xstrdup(uniq[i]);
        for (int r = 0; r < df->nrow; r++) {
            f->idx[r] = -1;
            for (int i = 0; i < nu; i++)
                if (!is_na(c->str[r]) && !strcmp(c->str[r], uniq[i])) { f->idx[r] = i; break; }
        }
        free(uniq);
    }
    return f;
}

/* R's factor(x, levels=c(...)): impose an explicit level order. `want` must
 * name every level that occurs in the data and nothing else — R would turn an
 * unlisted value into NA, which here would silently delete rows from a plot,
 * so an incomplete or misspelled list is an error naming the offender. */
int factor_relevel(Factor *f, int nrow, char *const *want, int nwant,
                   const char *what, char *err) {
    int *pos = cp_xmalloc(f->nlev * sizeof(int));   /* old level -> new slot */
    for (int i = 0; i < f->nlev; i++) {
        pos[i] = -1;
        for (int j = 0; j < nwant; j++)
            if (!strcmp(f->levels[i], want[j])) { pos[i] = j; break; }
        if (pos[i] < 0) {
            snprintf(err, CP_ERRLEN, "levels= for %s does not list `%s`, which "
                     "occurs in the data; levels= must name every value", what,
                     f->levels[i]);
            free(pos);
            return -1;
        }
    }
    for (int j = 0; j < nwant; j++) {               /* every slot filled? */
        int seen = 0;
        for (int i = 0; i < f->nlev; i++) if (pos[i] == j) { seen = 1; break; }
        if (!seen) {
            snprintf(err, CP_ERRLEN, "levels= for %s names `%s`, which does not "
                     "occur in the data", what, want[j]);
            free(pos);
            return -1;
        }
    }
    char **nl = cp_xmalloc(f->nlev * sizeof(char *));
    for (int i = 0; i < f->nlev; i++) nl[pos[i]] = f->levels[i];
    free(f->levels);
    f->levels = nl;
    for (int r = 0; r < nrow; r++)
        if (f->idx[r] >= 0) f->idx[r] = pos[f->idx[r]];
    free(pos);
    return 0;
}
