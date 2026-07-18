/* breaks.c — Wilkinson extended labeling (Talbot, Lin & Hanrahan 2010),
 * same algorithm and weights as R's labeling::extended() / ggplot2.
 * Breaks are computed on the EXPANDED limits; callers drop breaks that
 * fall outside them. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const double QQ[] = {1, 5, 2, 2.5, 4, 3};
#define NQ 6
static const double W1 = 0.25, W2 = 0.2, W3 = 0.5, W4 = 0.05;

static double pmod(double a, double b) { double r = fmod(a, b); return r < 0 ? r + b : r; }

static double simplicity(int i, int j, double lmin, double lmax, double step) {
    double eps = 1e-10;
    int v = ((pmod(lmin, step) < eps || step - pmod(lmin, step) < eps)
             && lmin <= 0 && lmax >= 0) ? 1 : 0;
    return 1.0 - (double)i / (NQ - 1) - j + v;
}
static double simplicity_max(int i, int j) { return 1.0 - (double)i / (NQ - 1) - j + 1.0; }
static double coverage(double dmin, double dmax, double l, double h) {
    double r = dmax - dmin;
    return 1.0 - 0.5 * ((dmax - h) * (dmax - h) + (dmin - l) * (dmin - l)) / (0.01 * r * r);
}
static double coverage_max(double dmin, double dmax, double span) {
    double r = dmax - dmin;
    if (span <= r) return 1.0;
    double half = (span - r) / 2;
    return 1.0 - half * half / (0.01 * r * r);
}
static double density(int k, int m, double dmin, double dmax, double l, double h) {
    double r = (k - 1) / (h - l);
    double rt = (m - 1) / (fmax(h, dmax) - fmin(l, dmin));
    return 2.0 - fmax(r / rt, rt / r);
}
static double density_max(int k, int m) {
    return k >= m ? 2.0 - (double)(k - 1) / (m - 1) : 1.0;
}

int extended_breaks(double dmin, double dmax, int m, double *out, int max_out) {
    double best_score = -2, blmin = dmin, blmax = dmax, bstep = dmax - dmin;
    for (int j = 1; j <= 2; j++)
        for (int i = 0; i < NQ; i++) {
            double q = QQ[i], sm = simplicity_max(i, j);
            if (W1 * sm + W2 + W3 + W4 < best_score) continue;
            for (int k = 2; k <= 12; k++) {
                double dm = density_max(k, m);
                if (W1 * sm + W2 + W3 * dm + W4 < best_score) break;
                double delta = (dmax - dmin) / (k + 1) / j / q;
                int z0 = (int)ceil(log10(delta));
                for (int z = z0; z <= z0 + 4; z++) {
                    double step = j * q * pow(10, z);
                    double cm = coverage_max(dmin, dmax, step * (k - 1));
                    if (W1 * sm + W2 * cm + W3 * dm + W4 < best_score) break;
                    int min_start = (int)(floor(dmax / step) * j - (k - 1) * j);
                    int max_start = (int)(ceil(dmin / step) * j);
                    for (int start = min_start; start <= max_start; start++) {
                        double lmin = start * step / j, lmax = lmin + step * (k - 1);
                        double score = W1 * simplicity(i, j, lmin, lmax, step)
                                     + W2 * coverage(dmin, dmax, lmin, lmax)
                                     + W3 * density(k, m, dmin, dmax, lmin, lmax) + W4;
                        if (score > best_score) {
                            best_score = score; blmin = lmin; blmax = lmax; bstep = step;
                        }
                    }
                }
            }
        }
    int n = 0;
    for (double b = blmin; b <= blmax + 1e-9 && n < max_out; b += bstep) out[n++] = b;
    return n;
}

void fmt_num(double v, char *buf) {
    if (fabs(v - round(v)) < 1e-9) sprintf(buf, "%.0f", v);
    else sprintf(buf, "%g", v);
}

/* ggplot labels an axis with UNIFORM decimals: the smallest d such that
 * every break is exact at d decimal places (so 0.0, 2.5, 5.0 — not 0, 2.5, 5) */
int axis_decimals(const double *br, int n) {
    for (int d = 0; d < 6; d++) {
        double s = pow(10, d);
        int ok = 1;
        for (int i = 0; i < n; i++)
            if (fabs(br[i] * s - round(br[i] * s)) > 1e-6 * s) { ok = 0; break; }
        if (ok) return d;
    }
    return 6;
}

void fmt_break(double v, int decimals, char *buf) {
    sprintf(buf, "%.*f", decimals, v);
}

/* log10 scale majors, following scales::breaks_log(n = 5):
 * integer powers of 10 thinned by `by = (ceil-floor)/n + 1` (decremented
 * until >= 3 land in range); label format follows R's vector-format width
 * heuristic (scientific 1e-03 iff a common fixed format would be wider).
 * If powers alone can't give 3 breaks, densify per-decade with {1,5} then
 * {1,2,5}; inside a fraction of a decade, fall back to linear extended
 * breaks on the untransformed range. */
int log10_breaks(double tlo, double thi, double *tmaj, char **labs, int max_out) {
    int klo = (int)floor(tlo), khi = (int)ceil(thi);
    if (khi > klo) {
        int by = (khi - klo) / 5 + 1;
        for (; by > 1; by--) {
            int cnt = 0;
            for (int k = klo; k <= khi; k += by)
                if (k >= tlo - 1e-9 && k <= thi + 1e-9) cnt++;
            if (cnt >= 3) break;
        }
        int n = 0, kmin = 0, kmax = 0;
        for (int k = klo; k <= khi; k += by)
            if (k >= tlo - 1e-9 && k <= thi + 1e-9) {
                if (!n || k < kmin) kmin = k;
                if (!n || k > kmax) kmax = k;
                n++;
            }
        if (n >= 3 && n <= max_out) {
            int dec = kmin < 0 ? -kmin : 0;
            int fixedw = (kmax > 0 ? kmax + 1 : 1) + (dec ? dec + 1 : 0);
            n = 0;
            for (int k = klo; k <= khi && n < max_out; k += by)
                if (k >= tlo - 1e-9 && k <= thi + 1e-9) {
                    tmaj[n] = k;
                    labs[n] = malloc(32);
                    if (fixedw > 5) sprintf(labs[n], "1e%+03d", k);
                    else fmt_break(pow(10, k), dec, labs[n]);
                    n++;
                }
            return n;
        }
    }
    static const double sets[2][3] = {{1, 5, 0}, {1, 2, 5}};
    static const int setn[2] = {2, 3};
    for (int s = 0; s < 2; s++) {
        int n = 0;
        for (int k = (int)floor(tlo) - 1; k <= (int)ceil(thi) + 1 && n < max_out; k++)
            for (int i = 0; i < setn[s] && n < max_out; i++) {
                double t = k + log10(sets[s][i]);
                if (t >= tlo - 1e-9 && t <= thi + 1e-9) {
                    tmaj[n] = t;
                    labs[n] = malloc(32);
                    fmt_num(sets[s][i] * pow(10, k), labs[n]);
                    n++;
                }
            }
        if (n >= 3) return n;
        while (n-- > 0) free(labs[n]);
    }
    double br[16];
    int nb = extended_breaks(pow(10, tlo), pow(10, thi), 5, br, 16), n = 0;
    for (int i = 0; i < nb && n < max_out; i++) {
        if (br[i] <= 0) continue;
        double t = log10(br[i]);
        if (t < tlo - 1e-9 || t > thi + 1e-9) continue;
        tmaj[n] = t;
        labs[n] = malloc(32);
        fmt_num(br[i], labs[n]);
        n++;
    }
    return n;
}
