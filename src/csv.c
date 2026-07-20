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
#include <unistd.h>

typedef struct { char **v; int n, cap; } StrVec;

static void sv_push(StrVec *s, char *p) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = realloc(s->v, s->cap * sizeof(char *)); }
    s->v[s->n++] = p;
}

static char *read_all(FILE *f) {
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[n] = 0;
    return buf;
}

/* split one record starting at *p using `delim` (',' or '\t'); returns fields
 * (malloc'd), advances *p past the record's newline; 0 fields at end of input */
static int split_record(char **p, StrVec *fields, char delim) {
    char stop[4] = {delim, '\r', '\n', 0};
    char *s = *p;
    if (!*s) return 0;
    int nf = 0;
    for (;;) {
        char *out;
        if (*s == '"') {
            size_t len = 0;                      /* measure, then copy */
            int closed = 0;
            for (const char *q = s + 1; *q; ) {
                if (*q == '"' && q[1] == '"') { len++; q += 2; }
                else if (*q == '"') { closed = 1; break; }
                else { len++; q++; }
            }
            if (!closed) return -1;
            char *o = out = malloc(len + 1);
            s++;
            while (*s) {
                if (*s == '"' && s[1] == '"') { *o++ = '"'; s += 2; }
                else if (*s == '"') { s++; break; }
                else *o++ = *s++;
            }
            *o = 0;
            if (*s && *s != delim && *s != '\r' && *s != '\n') {
                free(out);
                return -1;
            }
        } else {
            size_t len = strcspn(s, stop);
            out = malloc(len + 1);
            memcpy(out, s, len);
            out[len] = 0;
            s += len;
        }
        sv_push(fields, out);
        nf++;
        if (*s == delim) { s++; continue; }
        if (*s == '\r') s++;
        if (*s == '\n') s++;
        break;
    }
    *p = s;
    return nf;
}

static int is_na(const char *s) {
    return !*s || !strcmp(s, "NA") || !strcmp(s, "na") || !strcmp(s, "NaN");
}

DataFrame *df_read_csv(const char *path, char *err) {
    if (!strcmp(path, "stdin")) path = "-";          /* alias for stdin */
    if (!strcmp(path, "-") && isatty(fileno(stdin))) {
        snprintf(err, 256, "no input: stdin is a terminal — pipe data in or name a file");
        return NULL;
    }
    char *buf;
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".gz")) {      /* gzip / bgzip TSV */
        buf = gz_read_all(path, err);
        if (!buf) return NULL;
    } else {
        FILE *f = !strcmp(path, "-") ? stdin : fopen(path, "rb");
        if (!f) { sprintf(err, "cannot open %s", path); return NULL; }
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
        snprintf(err, 256, "%s: malformed quoted field in header", path);
        free(buf);
        return NULL;
    }
    if (!hn || header.n == 0) {
        sprintf(err, "%s: empty file", path);
        return NULL;
    }
    int ncol = header.n;

    StrVec *cells = calloc(ncol, sizeof(StrVec));
    int nrow = 0;
    for (;;) {
        StrVec rec = {0};
        int nf = split_record(&p, &rec, delim);
        if (nf < 0) {
            snprintf(err, 256, "%s: malformed quoted field in row %d", path, nrow + 2);
            free(buf);
            return NULL;
        }
        if (nf == 0) break;
        if (nf == 1 && !*rec.v[0]) { free(rec.v[0]); free(rec.v); continue; } /* blank line */
        if (nf != ncol) {
            sprintf(err, "%s: row %d has %d fields, expected %d", path, nrow + 2, nf, ncol);
            return NULL;
        }
        for (int c = 0; c < ncol; c++) sv_push(&cells[c], rec.v[c]);
        free(rec.v);
        nrow++;
    }
    free(buf);

    DataFrame *df = malloc(sizeof *df);
    df->nrow = nrow; df->ncol = ncol;
    df->cols = calloc(ncol, sizeof(Column));
    for (int c = 0; c < ncol; c++) {
        Column *col = &df->cols[c];
        col->name = header.v[c];
        int numeric = 1;
        for (int r = 0; r < nrow && numeric; r++) {
            const char *s = cells[c].v[r];
            if (is_na(s)) continue;
            char *end;
            strtod(s, &end);
            if (end == s || *end) numeric = 0;
        }
        if (numeric) {
            col->type = COL_NUM;
            col->num = malloc(nrow * sizeof(double));
            for (int r = 0; r < nrow; r++)
                col->num[r] = is_na(cells[c].v[r]) ? NAN : strtod(cells[c].v[r], NULL);
        } else {
            col->type = COL_STR;
            col->str = cells[c].v;
            continue;                       /* strings keep the cell storage */
        }
        for (int r = 0; r < nrow; r++) free(cells[c].v[r]);
        free(cells[c].v);
    }
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
    Factor *f = malloc(sizeof *f);
    f->idx = malloc(df->nrow * sizeof(int));
    if (c->type == COL_NUM) {
        double *uniq = malloc(df->nrow * sizeof(double));
        int nu = 0;
        for (int r = 0; r < df->nrow; r++) {
            if (isnan(c->num[r])) continue;
            int seen = 0;
            for (int i = 0; i < nu; i++) if (uniq[i] == c->num[r]) { seen = 1; break; }
            if (!seen) uniq[nu++] = c->num[r];
        }
        qsort(uniq, nu, sizeof(double), cmp_dbl);
        f->nlev = nu;
        f->levels = malloc(nu * sizeof(char *));
        for (int i = 0; i < nu; i++) {
            f->levels[i] = malloc(32);
            fmt_num(uniq[i], f->levels[i]);
        }
        for (int r = 0; r < df->nrow; r++) {
            f->idx[r] = -1;
            for (int i = 0; i < nu; i++)
                if (!isnan(c->num[r]) && c->num[r] == uniq[i]) { f->idx[r] = i; break; }
        }
        free(uniq);
    } else {
        char **uniq = malloc(df->nrow * sizeof(char *));
        int nu = 0;
        for (int r = 0; r < df->nrow; r++) {
            if (is_na(c->str[r])) continue;
            int seen = 0;
            for (int i = 0; i < nu; i++) if (!strcmp(uniq[i], c->str[r])) { seen = 1; break; }
            if (!seen) uniq[nu++] = c->str[r];
        }
        qsort(uniq, nu, sizeof(char *), cmp_str);
        f->nlev = nu;
        f->levels = uniq;
        for (int r = 0; r < df->nrow; r++) {
            f->idx[r] = -1;
            for (int i = 0; i < nu; i++)
                if (!is_na(c->str[r]) && !strcmp(c->str[r], uniq[i])) { f->idx[r] = i; break; }
        }
    }
    return f;
}
