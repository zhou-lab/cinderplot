/* palette.c — ggplot2's scale_colour_hue for n discrete levels:
 * hues evenly spaced on the HCL (CIELUV) circle starting at 15deg,
 * chroma 100, luminance 65 — equivalent to R's hcl(h, 100, 65).
 * Values are quantized to 8-bit, matching R's hex output, so renders
 * are bit-identical to ggplot's. */
#include "cinderplot.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* D65 white point, u'v' chromaticity */
static const double UN = 0.1978398, VN = 0.4683363;

static double gamma_srgb(double c) {
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

static Col hcl(double h, double C, double L) {
    double rad = h * M_PI / 180.0;
    double u = C * cos(rad), v = C * sin(rad);
    double Y = L > 7.999592 ? pow((L + 16) / 116, 3.0) : L / 903.3;
    double up = u / (13 * L) + UN, vp = v / (13 * L) + VN;
    double X = 9 * Y * up / (4 * vp);
    double Z = Y * (12 - 3 * up - 20 * vp) / (4 * vp);
    double r =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    double b =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    Col c;
    c.r = gamma_srgb(r < 0 ? 0 : r > 1 ? 1 : r);
    c.g = gamma_srgb(g < 0 ? 0 : g > 1 ? 1 : g);
    c.b = gamma_srgb(b < 0 ? 0 : b > 1 ? 1 : b);
    c.r = round(c.r * 255) / 255;   /* quantize like R's hex colours */
    c.g = round(c.g * 255) / 255;
    c.b = round(c.b * 255) / 255;
    return c;
}

void hue_palette(int n, Col *out) {
    for (int i = 0; i < n; i++)
        out[i] = hcl(15.0 + i * 360.0 / n, 100, 65);
}

/* ---------------- continuous colormaps ---------------- */
static Col C(int r, int g, int b) { Col c = {r / 255.0, g / 255.0, b / 255.0}; return c; }

static void ramp(const Col *stops, int n, double t, Col *out) {
    if (t <= 0) { *out = stops[0]; return; }
    if (t >= 1) { *out = stops[n - 1]; return; }
    double p = t * (n - 1);
    int i = (int)p;
    double f = p - i;
    out->r = stops[i].r + f * (stops[i + 1].r - stops[i].r);
    out->g = stops[i].g + f * (stops[i + 1].g - stops[i].g);
    out->b = stops[i].b + f * (stops[i + 1].b - stops[i].b);
}

Col fill_map(const FillScale *fs, double t) {
    /* stop tables (viridis: the standard 10-colour rendering) */
    static Col vir[10], jet[9], bwr[3];
    static int init = 0;
    if (!init) {
        Col v[10] = {C(0x44,0x01,0x54), C(0x48,0x28,0x78), C(0x3E,0x49,0x89),
                     C(0x31,0x68,0x8E), C(0x26,0x82,0x8E), C(0x1F,0x9E,0x89),
                     C(0x35,0xB7,0x79), C(0x6D,0xCD,0x59), C(0xB4,0xDE,0x2C),
                     C(0xFD,0xE7,0x25)};
        Col j[9] = {C(0x00,0x00,0x7F), C(0x00,0x00,0xFF), C(0x00,0x7F,0xFF),
                    C(0x00,0xFF,0xFF), C(0x7F,0xFF,0x7F), C(0xFF,0xFF,0x00),
                    C(0xFF,0x7F,0x00), C(0xFF,0x00,0x00), C(0x7F,0x00,0x00)};
        Col b[3] = {C(0x00,0x00,0xFF), C(0xFF,0xFF,0xFF), C(0xFF,0x00,0x00)};
        memcpy(vir, v, sizeof v); memcpy(jet, j, sizeof j); memcpy(bwr, b, sizeof b);
        init = 1;
    }
    Col out;
    switch (fs->kind) {
    case FILL_JET: ramp(jet, 9, t, &out); break;
    case FILL_BWR: ramp(bwr, 3, t, &out); break;
    case FILL_GRADIENT: {
        Col st[2] = {fs->low, fs->high};
        ramp(st, 2, t, &out);
        break;
    }
    case FILL_GRADIENT2: {
        Col st[3] = {fs->low, fs->mid, fs->high};
        ramp(st, 3, t, &out);
        break;
    }
    default: ramp(vir, 10, t, &out); break;
    }
    return out;
}

Col fill_map_value(const FillScale *fs, double v, double dmin, double dmax) {
    double t;
    if (fs->kind == FILL_GRADIENT2) {
        /* midpoint maps to t = 0.5 (ggplot scale_fill_gradient2) */
        double m = fs->midpoint;
        if (v <= m) t = dmin >= m ? 0 : 0.5 * (v - dmin) / (m - dmin);
        else        t = dmax <= m ? 1 : 0.5 + 0.5 * (v - m) / (dmax - m);
    } else {
        t = dmax > dmin ? (v - dmin) / (dmax - dmin) : 0.5;
    }
    return fill_map(fs, t < 0 ? 0 : t > 1 ? 1 : t);
}

int parse_color(const char *s, Col *out) {
    static const struct { const char *n; int r, g, b; } named[] = {
        {"white",255,255,255}, {"black",0,0,0}, {"red",255,0,0},
        {"green",0,255,0}, {"blue",0,0,255}, {"yellow",255,255,0},
        {"orange",255,165,0}, {"purple",160,32,240}, {"grey",190,190,190},
        {"gray",190,190,190}, {"darkblue",0,0,139}, {"darkred",139,0,0},
        {"darkgreen",0,100,0}, {"steelblue",70,130,180},
    };
    if (s[0] == '#' && strlen(s) == 7) {
        int r, g, b;
        if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) { *out = C(r, g, b); return 0; }
        return -1;
    }
    /* greyNN / grayNN: NN in 0..100 (ggplot grey ramp) */
    if (!strncmp(s, "grey", 4) || !strncmp(s, "gray", 4)) {
        char *end; long pct = strtol(s + 4, &end, 10);
        if (*end == 0 && end != s + 4 && pct >= 0 && pct <= 100) {
            int v = (int)(pct * 255 / 100.0 + 0.5);
            *out = C(v, v, v); return 0;
        }
    }
    for (size_t i = 0; i < sizeof named / sizeof *named; i++)
        if (!strcmp(s, named[i].n)) { *out = C(named[i].r, named[i].g, named[i].b); return 0; }
    return -1;
}
