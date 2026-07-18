/* cluster.c — hierarchical clustering for heatmap mode.
 *
 * Reproduces R's hclust(dist(x), method="ward.D2"):
 *  - euclidean distance with R's NA handling (pairwise-complete sums
 *    scaled up by p/#complete)
 *  - Ward.D2 via the Lance-Williams update on squared distances
 *  - merge rows in R's convention (negative = singleton, 1-based;
 *    positive = cluster formed at that step; smaller id first)
 *  - leaf order from recursive first-then-second expansion of the
 *    final merge, matching R's iorder
 * Naive O(n^3) agglomeration: fine for heatmap-sized inputs; merges by
 * global minimum, so heights come out ascending like R's sorted output. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* euclidean distance between observations a and b (rows of x, n x p) */
static double eucl(const double *x, int p, int a, int b) {
    double s = 0;
    int used = 0;
    for (int k = 0; k < p; k++) {
        double d = x[(size_t)a * p + k] - x[(size_t)b * p + k];
        if (isnan(d)) continue;
        s += d * d;
        used++;
    }
    if (!used) return NAN;
    return sqrt(s * ((double)p / used));
}

static void order_expand(const HClust *h, int node, int *order, int *no) {
    int a = h->merge[node][0], b = h->merge[node][1];
    if (a < 0) order[(*no)++] = -a - 1; else order_expand(h, a - 1, order, no);
    if (b < 0) order[(*no)++] = -b - 1; else order_expand(h, b - 1, order, no);
}

/* cluster n observations of length p (row-major x); NULL on failure */
HClust *hclust_ward(const double *x, int n, int p, char *err) {
    if (n < 2) { sprintf(err, "need at least 2 observations to cluster"); return NULL; }
    double *D = malloc((size_t)n * n * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double d = eucl(x, p, i, j);
            if (isnan(d)) {
                sprintf(err, "cannot cluster: observations %d and %d share no complete values", i + 1, j + 1);
                free(D);
                return NULL;
            }
            D[(size_t)i * n + j] = D[(size_t)j * n + i] = d;
        }

    HClust *h = malloc(sizeof *h);
    h->n = n;
    h->merge = malloc((n - 1) * sizeof *h->merge);
    h->height = malloc((n - 1) * sizeof(double));
    h->order = malloc(n * sizeof(int));

    int *active = malloc(n * sizeof(int));
    int *size = malloc(n * sizeof(int));
    int *cid = malloc(n * sizeof(int));          /* R cluster id of slot */
    for (int i = 0; i < n; i++) { active[i] = 1; size[i] = 1; cid[i] = -(i + 1); }

    for (int step = 0; step < n - 1; step++) {
        int bi = -1, bj = -1;
        double best = 1e300;
        for (int i = 0; i < n; i++) {
            if (!active[i]) continue;
            for (int j = i + 1; j < n; j++) {
                if (!active[j]) continue;
                if (D[(size_t)i * n + j] < best) { best = D[(size_t)i * n + j]; bi = i; bj = j; }
            }
        }
        h->height[step] = best;
        int a = cid[bi], b = cid[bj];
        /* R convention: singletons before clusters; then ascending magnitude */
        int swap = (a < 0 && b < 0) ? (-a > -b)
                 : (a > 0 && b > 0) ? (a > b)
                 : (a > 0);                       /* cluster before singleton */
        h->merge[step][0] = swap ? b : a;
        h->merge[step][1] = swap ? a : b;

        double dij = best;
        for (int k = 0; k < n; k++) {
            if (!active[k] || k == bi || k == bj) continue;
            double dik = D[(size_t)bi * n + k], djk = D[(size_t)bj * n + k];
            double d2 = ((size[bi] + size[k]) * dik * dik
                       + (size[bj] + size[k]) * djk * djk
                       - (double)size[k] * dij * dij)
                      / (double)(size[bi] + size[bj] + size[k]);
            D[(size_t)bi * n + k] = D[(size_t)k * n + bi] = sqrt(d2 > 0 ? d2 : 0);
        }
        size[bi] += size[bj];
        active[bj] = 0;
        cid[bi] = step + 1;
    }

    int no = 0;
    order_expand(h, n - 2, h->order, &no);

    free(D); free(active); free(size); free(cid);
    return h;
}
