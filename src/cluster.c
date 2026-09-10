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
 *
 * The agglomeration is a port of R's hclust.f (Murtagh's nearest-neighbour
 * list algorithm), kept step for step -- including its floating-point
 * habits -- because that is what decides EXACT TIES, and integer or binary
 * matrices are nothing but ties. Two habits matter:
 *  - R takes dist()'s sqrt'd distances and squares them again for ward.D2,
 *    so sqrt(3)^2 = 2.9999999999999996 lands below sqrt(2)^2 + 1 and an
 *    exact tie is decided by rounding noise; the squares here are formed
 *    the same way.
 *  - a cluster's cached nearest neighbour is only replaced when a merge
 *    brings something STRICTLY nearer, so an equal distance to the new
 *    cluster does not win even when its index is lower. A fresh
 *    global-minimum scan picks the lowest index and merges differently.
 * Naive O(n^2) per step, O(n^3) overall: fine for heatmap-sized inputs. */
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
    if (used != p) s /= (double)used / p;   /* R's R_euclidean, operation for operation */
    return sqrt(s);
}

static void order_expand(const HClust *h, int node, int *order, int *no) {
    int a = h->merge[node][0], b = h->merge[node][1];
    if (a < 0) order[(*no)++] = -a - 1; else order_expand(h, a - 1, order, no);
    if (b < 0) order[(*no)++] = -b - 1; else order_expand(h, b - 1, order, no);
}

/* cluster n observations of length p (row-major x); NULL on failure */
HClust *hclust_ward(const double *x, int n, int p, char *err) {
    if (n < 2) { snprintf(err, CP_ERRLEN, "need at least 2 observations to cluster"); return NULL; }
    /* squared distances, formed as R does: dist() then DISS*DISS */
    double *D = cp_xmalloc((size_t)n * n * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            double d = eucl(x, p, i, j);
            if (isnan(d)) {
                snprintf(err, CP_ERRLEN, "cannot cluster: observations %d and %d share no complete values", i + 1, j + 1);
                free(D);
                return NULL;
            }
            D[(size_t)i * n + j] = D[(size_t)j * n + i] = d * d;
        }
#define DD(i, j) D[(size_t)(i) * n + (j)]

    HClust *h = cp_xmalloc(sizeof *h);
    h->n = n;
    h->merge = cp_xmalloc((n - 1) * sizeof *h->merge);
    h->height = cp_xmalloc((n - 1) * sizeof(double));
    h->order = cp_xmalloc(n * sizeof(int));

    int *active = cp_xmalloc(n * sizeof(int));
    double *membr = cp_xmalloc(n * sizeof(double));  /* cluster sizes (double, as in R) */
    int *cid = cp_xmalloc(n * sizeof(int));          /* R cluster id of slot */
    int *nn = cp_xmalloc(n * sizeof(int));           /* nearest neighbour j > i */
    double *disnn = cp_xmalloc(n * sizeof(double));  /* its distance */
    const double INF = 1e300;
    for (int i = 0; i < n; i++) { active[i] = 1; membr[i] = 1; cid[i] = -(i + 1); }
    for (int i = 0; i < n - 1; i++) {                /* initial NN list */
        double dmin = INF; int jm = i + 1;
        for (int j = i + 1; j < n; j++)
            if (DD(i, j) < dmin) { dmin = DD(i, j); jm = j; }
        nn[i] = jm; disnn[i] = dmin;
    }

    for (int step = 0; step < n - 1; step++) {
        /* least dissimilarity: first (lowest) i wins a tie, then its cached nn */
        double dmin = INF; int im = -1, jm = -1;
        for (int i = 0; i < n - 1; i++)
            if (active[i] && disnn[i] < dmin) { dmin = disnn[i]; im = i; jm = nn[i]; }
        int i2 = im < jm ? im : jm, j2 = im < jm ? jm : im;
        h->height[step] = sqrt(dmin);                /* CRIT = sqrt for D2 */
        int a = cid[i2], b = cid[j2];
        /* R convention: singletons before clusters; then ascending magnitude */
        int swap = (a < 0 && b < 0) ? (-a > -b)
                 : (a > 0 && b > 0) ? (a > b)
                 : (a > 0);                       /* cluster before singleton */
        h->merge[step][0] = swap ? b : a;
        h->merge[step][1] = swap ? a : b;
        active[j2] = 0;

        /* Lance-Williams (Ward) update of slot i2 against every other active
         * cluster; the new nn of i2 comes out of the same pass, as in R. */
        double xx = DD(i2, j2);
        dmin = INF; int jj = -1;
        for (int k = 0; k < n; k++) {
            if (!active[k] || k == i2) continue;
            double dk = (membr[i2] + membr[k]) * DD(i2, k)
                      + (membr[j2] + membr[k]) * DD(j2, k)
                      - membr[k] * xx;
            dk = dk / (membr[i2] + membr[j2] + membr[k]);
            DD(i2, k) = DD(k, i2) = dk;
            if (i2 < k) {
                if (dk < dmin) { dmin = dk; jj = k; }
            } else if (dk < disnn[k]) {           /* nearer than k's cached nn */
                disnn[k] = dk; nn[k] = i2;
            }
        }
        membr[i2] += membr[j2];
        disnn[i2] = dmin; nn[i2] = jj;
        cid[i2] = step + 1;
        /* clusters whose nn just merged away or changed: rescan */
        for (int i = 0; i < n - 1; i++) {
            if (!active[i] || (nn[i] != i2 && nn[i] != j2)) continue;
            dmin = INF; jj = -1;
            for (int j = i + 1; j < n; j++)
                if (active[j] && DD(i, j) < dmin) { dmin = DD(i, j); jj = j; }
            nn[i] = jj; disnn[i] = dmin;
        }
    }
#undef DD

    int no = 0;
    order_expand(h, n - 2, h->order, &no);

    free(D); free(active); free(membr); free(cid); free(nn); free(disnn);
    return h;
}
