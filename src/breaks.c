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
    /* degenerate range: a constant-valued axis (dmax <= dmin) makes delta 0
     * (log10(0) = -inf, UB in the ceil below) and step 0 (an infinite emit
     * loop). R's extended() returns the single value; do the same. */
    if (!(dmax > dmin)) {
        if (max_out < 1) return 0;
        out[0] = dmin;
        return 1;
    }
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

void fmt_num(double v, char *buf, size_t cap) {
    if (fabs(v - round(v)) < 1e-9) snprintf(buf, cap, "%.0f", v);
    else snprintf(buf, cap, "%g", v);
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

void fmt_break(double v, int decimals, char *buf, size_t cap) {
    snprintf(buf, cap, "%.*f", decimals, v);
}

/* log scale majors: integer powers of the base within the range, labelled
 * base^k (rendered with a superscript exponent by the axis renderer). This is
 * the scientific-notation style (R's trans_breaks("log10", 10^x) +
 * math_format); cinderplot uses it as the default for scale_x/y_log10,
 * deviating from ggplot's default value labels. The sub-structure within each
 * step is shown by the log tick marks (see log_ticks/log_minors in render.c).
 * Crowded ranges are thinned so labels stay legible. Labels are "base^k" for
 * the renderer to split.
 *
 * Base 2 rungs are only ~0.3 decades apart, so a range wide enough to be worth
 * a log2 axis holds several times more of them than it would decades -- hence
 * the higher thinning threshold, which keeps a 2^10..2^22 coverage ladder at
 * every other power rather than collapsing it to six labels. */
int log_breaks(int base, double tlo, double thi, double *tmaj, char **labs,
               int max_out) {
    int klo = (int)ceil(tlo - 1e-9), khi = (int)floor(thi + 1e-9);
    int nstep = khi - klo + 1;
    if (nstep < 1) return 0;                   /* no whole power in range */
    int keep = base == 2 ? 12 : 7;             /* labels to show before thinning */
    int by = 1;
    if (nstep > keep) by = (nstep - 1) / (keep - 1) + 1;
    int n = 0;
    for (int k = klo; k <= khi && n < max_out; k += by) {
        tmaj[n] = k;
        labs[n] = cp_xmalloc(16);
        sprintf(labs[n], "%d^%d", base ? base : 10, k);
        n++;
    }
    return n;
}
