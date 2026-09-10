/* breaks.c — Wilkinson extended labeling (Talbot, Lin & Hanrahan 2010),
 * same algorithm and weights as R's labeling::extended() / ggplot2.
 * Breaks are computed on the EXPANDED limits; callers drop breaks that
 * fall outside them. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    double best_score = -2, blmin = dmin, bstep = dmax - dmin;
    int bk = 2;
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
                            best_score = score; blmin = lmin; bstep = step; bk = k;
                        }
                    }
                }
            }
        }
    /* The k breaks are start + i*step, as R's seq(from, to, by) forms them.
     * Accumulating b += step instead drifted by an ulp per step, and the
     * absolute 1e-9 slack that then let the top break through also let a
     * range under 1e-8 wide run on to max_out breaks. A break that lands
     * within rounding of a limit IS that limit (-3 * 0.1 is not below -0.3),
     * so callers' strict [lo, hi] fences keep it. */
    int n = 0;
    double eps = 1e-9 * (dmax - dmin);
    for (int i = 0; i < bk && n < max_out; i++) {
        double b = blmin + i * bstep;
        if (fabs(b - dmin) <= eps) b = dmin;
        else if (fabs(b - dmax) <= eps) b = dmax;
        out[n++] = b;
    }
    return n;
}

/* "-0", "-0.0", "-0.00": a rounding residue printed with its sign. Nothing
 * a reader can act on, so print the zero it is. */
static void drop_negzero(char *buf) {
    if (buf[0] != '-') return;
    for (const char *p = buf + 1; *p; p++)
        if (*p != '0' && *p != '.') return;
    memmove(buf, buf + 1, strlen(buf));
}

/* A value counts as an integer within 1e-9 RELATIVE to itself: 3.0000000000000004
 * prints as 3, while 1e-12 -- which an absolute slack called 0 -- prints as
 * itself, so two tiny levels or labels never collapse into one "0". */
void fmt_num(double v, char *buf, size_t cap) {
    double r = round(v);
    if (fabs(v - r) <= 1e-9 * fabs(v)) {
        if (r == 0) r = 0.0;                        /* -0 -> 0 */
        snprintf(buf, cap, "%.0f", r);
    } else snprintf(buf, cap, "%g", v);
    drop_negzero(buf);
}

/* ggplot labels an axis with UNIFORM decimals: the smallest d such that
 * every break is exact at d decimal places (so 0.0, 2.5, 5.0 — not 0, 2.5, 5).
 * "Exact" is judged relative to the break STEP, not by an absolute 1e-6: an
 * axis whose breaks are all under 1e-6 used to read 0 0 0 0 0. Nice breaks
 * need at most six decimals down to a step of 1e-6. Past that, breaks
 * hugging zero return -1 and fmt_break prints them %g-style (1e-06,
 * 1.5e-06, ...), which is compact and what R's format() would do; breaks
 * with a real offset (1, 1.000000025, ...) keep counting decimals, because
 * %g's six significant digits would print them all as 1. */
int axis_decimals(const double *br, int n) {
    double step = 0, big = 0;                     /* smallest gap; largest |break| */
    for (int i = 1; i < n; i++) {
        double g = fabs(br[i] - br[i - 1]);
        if (g > 0 && (step == 0 || g < step)) step = g;
    }
    for (int i = 0; i < n; i++) if (fabs(br[i]) > big) big = fabs(br[i]);
    if (step == 0) step = big > 0 ? big : 1;
    for (int d = 0; d <= 12; d++) {
        if (d == 7 && big < 1e-3) return -1;      /* tiny throughout: %g reads better */
        double s = pow(10, d);
        int ok = 1;
        for (int i = 0; i < n; i++) {
            /* plus a few ulps of the break itself, so 1e9 + 0.1 is still 0.1 */
            double tol = 1e-6 * step + 1e-15 * fabs(br[i]);
            if (fabs(br[i] - round(br[i] * s) / s) > tol) { ok = 0; break; }
        }
        if (ok) return d;
    }
    return -1;
}

void fmt_break(double v, int decimals, char *buf, size_t cap) {
    if (decimals < 0) { fmt_num(v, buf, cap); return; }
    snprintf(buf, cap, "%.*f", decimals, v);
    drop_negzero(buf);
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
    if (!base) base = 10;
    int klo = (int)ceil(tlo - 1e-9), khi = (int)floor(thi + 1e-9);
    int nstep = khi - klo + 1;
    if (nstep < 1) {
        /* Sub-decade range (a fold change, a 20..80% band): no power to
         * label, and a bare axis with only minor ticks reads as broken. Lay
         * ordinary linear breaks over the data-space span and place them on
         * the log axis -- 2, 4, 6, 8 rather than 10^0.3 -- the way ggplot
         * falls back to value labels when the powers run out. */
        double dlo = pow(base, tlo), dhi = pow(base, thi);
        double *lin = cp_xmalloc((size_t)max_out * sizeof(double));
        int nl = extended_breaks(dlo, dhi, 5, lin, max_out), n = 0;
        for (int i = 0; i < nl; i++)
            if (lin[i] > 0 && lin[i] >= dlo * (1 - 1e-9) && lin[i] <= dhi * (1 + 1e-9))
                lin[n++] = lin[i];
        int dec = n ? axis_decimals(lin, n) : 0;
        for (int i = 0; i < n; i++) {
            tmaj[i] = cp_logt(base, lin[i]);
            labs[i] = cp_xmalloc(32);
            fmt_break(lin[i], dec, labs[i], 32);
        }
        free(lin);
        return n;
    }
    int keep = base == 2 ? 12 : 7;             /* labels to show before thinning */
    int by = 1;
    if (nstep > keep) by = (nstep - 1) / (keep - 1) + 1;
    /* Thin on multiples of `by` (..., -by, 0, by, ...) rather than counting
     * from the low end, so 10^0 is always a candidate: 10^-12..10^12 used to
     * label -12, -7, -2, 3, 8 and skip the one power every reader anchors on. */
    int k0 = klo;
    if (by > 1) { k0 = (int)floor((double)klo / by) * by; if (k0 < klo) k0 += by; }
    int n = 0;
    for (int k = k0; k <= khi && n < max_out; k += by) {
        tmaj[n] = k;
        labs[n] = cp_xmalloc(16);
        sprintf(labs[n], "%d^%d", base, k);
        n++;
    }
    return n;
}
