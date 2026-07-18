/* cluster_check — prints hclust order/heights for a matrix CSV, for
 * comparison against R: hclust(dist(m), method="ward.D2").
 * Usage: cluster_check matrix.csv rows|cols */
#include "cinderplot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    char err[256] = "";
    if (argc != 3) { fprintf(stderr, "usage: cluster_check m.csv rows|cols\n"); return 2; }
    DataFrame *df = df_read_csv(argv[1], err);
    if (!df) { fprintf(stderr, "%s\n", err); return 1; }
    int c0 = df->cols[0].type == COL_STR ? 1 : 0;
    int nr = df->nrow, nc = df->ncol - c0;
    double *m = malloc((size_t)nr * nc * sizeof(double));
    for (int c = 0; c < nc; c++)
        for (int r = 0; r < nr; r++)
            m[(size_t)r * nc + c] = df->cols[c + c0].num[r];

    int rows = !strcmp(argv[2], "rows");
    int n = rows ? nr : nc, p = rows ? nc : nr;
    double *obs = malloc((size_t)n * p * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int k = 0; k < p; k++)
            obs[(size_t)i * p + k] = rows ? m[(size_t)i * nc + k] : m[(size_t)k * nc + i];

    HClust *h = hclust_ward(obs, n, p, err);
    if (!h) { fprintf(stderr, "%s\n", err); return 1; }
    printf("order:");
    for (int i = 0; i < n; i++) printf(" %d", h->order[i] + 1);
    printf("\nheights:");
    for (int i = 0; i < n - 1; i++) printf(" %.4f", h->height[i]);
    printf("\n");
    return 0;
}
