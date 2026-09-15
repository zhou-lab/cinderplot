/* smooth.c — the LOESS smoother shared by geom_smooth() and the signal()
 * track. One implementation, so the trace under a locus and the trace on a
 * grammar panel are the same curve for the same span. */
#include "cinderplot.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* LOESS, as ggplot2's default for n < 1000: at each of `nout` output x evenly
 * spaced over [min x, max x], take the nearest `span` fraction of the data,
 * weight them by the tricube of their scaled distance, and fit a local
 * quadratic. Degree 2 rather than 1 because a local line flattens peaks, and
 * a methylation trace is mostly peaks.
 *
 * The inputs need not be sorted. Writes up to nout (x, y) pairs into ox/oy
 * and returns how many; an output x whose neighbourhood is degenerate (every
 * x identical) takes the weighted mean, and one with no finite fit is
 * skipped, so the count can fall short of nout. Returns -1 with `err` set
 * when there is nothing to fit through (n < 4), the span is not positive, or
 * fewer than two outputs are asked for. A span above 1 is clamped to the
 * whole sample, as the neighbourhood cannot exceed the data. */
int cp_loess(const double *x, const double *y, int n, double span, int nout,
             double *ox, double *oy, char *err) {
    if (n < 4) {
        snprintf(err, CP_ERRLEN, "loess needs at least 4 points to fit through; got %d", n);
        return -1;
    }
    if (!(span > 0)) {
        snprintf(err, CP_ERRLEN, "loess span must be > 0 (got %g)", span);
        return -1;
    }
    if (nout < 2) {
        snprintf(err, CP_ERRLEN, "loess needs at least 2 output points (got %d)", nout);
        return -1;
    }
    double *sx = cp_xmalloc((size_t)n * sizeof(double));
    double *sy = cp_xmalloc((size_t)n * sizeof(double));
    memcpy(sx, x, (size_t)n * sizeof(double));
    memcpy(sy, y, (size_t)n * sizeof(double));
    /* sort by x: the neighbourhood is a window over sorted x. Insertion sort
     * is stable, so tied x keep their input order and the fit is
     * reproducible from the same input. */
    for (int a = 1; a < n; a++) {
        double kx = sx[a], ky = sy[a]; int b2 = a - 1;
        while (b2 >= 0 && sx[b2] > kx) {
            sx[b2+1] = sx[b2]; sy[b2+1] = sy[b2]; b2--;
        }
        sx[b2+1] = kx; sy[b2+1] = ky;
    }
    int q = (int)ceil(span * n);
    if (q < 3) q = 3;
    if (q > n) q = n;
    int npt = 0;
    for (int k = 0; k < nout; k++) {
        double x0v = sx[0] + (sx[n-1] - sx[0]) * k / (nout - 1.0);
        /* the q nearest by x, as a window [lo, lo+q) */
        int lo = 0, hi = n - q;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (x0v - sx[mid] > sx[mid + q] - x0v) lo = mid + 1;
            else hi = mid;
        }
        double dmax = fmax(fabs(x0v - sx[lo]), fabs(sx[lo+q-1] - x0v));
        if (dmax <= 0) dmax = 1e-12;
        /* weighted quadratic by normal equations on (x - x0) */
        double m[3][4] = {{0}};
        for (int i2 = lo; i2 < lo + q; i2++) {
            double d = fabs(sx[i2] - x0v) / dmax;
            if (d >= 1) continue;
            double t = 1 - d * d * d, w = t * t * t;
            double u = sx[i2] - x0v, u2 = u * u;
            double b0 = 1, b1 = u, b2v = u2;
            double bb[3] = {b0, b1, b2v};
            for (int a2 = 0; a2 < 3; a2++) {
                for (int c2 = 0; c2 < 3; c2++) m[a2][c2] += w * bb[a2] * bb[c2];
                m[a2][3] += w * bb[a2] * sy[i2];
            }
        }
        /* Gaussian elimination with partial pivoting; a degenerate
         * neighbourhood (every x identical) falls back to the weighted mean,
         * which is m[0][3]/m[0][0]. */
        double sol[3] = {0,0,0};
        int ok = 1;
        for (int c2 = 0; c2 < 3 && ok; c2++) {
            int piv = c2;
            for (int r2 = c2 + 1; r2 < 3; r2++)
                if (fabs(m[r2][c2]) > fabs(m[piv][c2])) piv = r2;
            if (fabs(m[piv][c2]) < 1e-12) { ok = 0; break; }
            if (piv != c2) for (int j2 = 0; j2 < 4; j2++) {
                double t2 = m[c2][j2]; m[c2][j2] = m[piv][j2]; m[piv][j2] = t2;
            }
            for (int r2 = c2 + 1; r2 < 3; r2++) {
                double f = m[r2][c2] / m[c2][c2];
                for (int j2 = c2; j2 < 4; j2++) m[r2][j2] -= f * m[c2][j2];
            }
        }
        double yv;
        if (ok) {
            for (int r2 = 2; r2 >= 0; r2--) {
                double acc = m[r2][3];
                for (int c2 = r2 + 1; c2 < 3; c2++) acc -= m[r2][c2] * sol[c2];
                sol[r2] = acc / m[r2][r2];
            }
            yv = sol[0];           /* the fit AT x0v, where u = 0 */
        } else if (fabs(m[0][0]) > 1e-12) yv = m[0][3] / m[0][0];
        else continue;
        if (!isfinite(yv)) continue;
        ox[npt] = x0v; oy[npt] = yv; npt++;
    }
    free(sx); free(sy);
    return npt;
}
