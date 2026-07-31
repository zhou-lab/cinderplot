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
    if (!isfinite(t)) t = 0.5;       /* defensive: never cast NaN to an index */
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
    static Col vir[10], jet[9], bwr[3], parula[9];
    static Col turbo[11], coolwarm[9], magma[9], inferno[9];
    static Col plasma[9], cividis[9], rocket[9], mako[9];
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
        /* MATLAB/pals parula: indigo -> blue -> cyan -> green -> yellow */
        Col pa[9] = {C(0x35,0x2A,0x87), C(0x0F,0x5C,0xDD), C(0x12,0x7D,0xD8),
                     C(0x06,0x9C,0xC5), C(0x21,0xB4,0x9E), C(0x79,0xBD,0x69),
                     C(0xC2,0xBD,0x4A), C(0xFA,0xC4,0x2C), C(0xF9,0xFB,0x0E)};
        /* Google's turbo: a rainbow with none of jet's false banding, for
         * when a diverging or sequential ramp genuinely is not wanted. */
        Col tu[11] = {C(0x30,0x12,0x3B), C(0x41,0x45,0x87), C(0x46,0x77,0xCC),
                      C(0x2C,0xA7,0xE9), C(0x22,0xCE,0xC0), C(0x4B,0xE5,0x86),
                      C(0x9A,0xF3,0x3E), C(0xD1,0xE5,0x21), C(0xF9,0xB0,0x1F),
                      C(0xEB,0x62,0x0C), C(0xA0,0x08,0x02)};
        /* Moreland's cool-warm: the diverging default in ParaView/matplotlib,
         * and a better one than blue-white-red because its midpoint is a light
         * grey rather than pure white, so zero does not vanish on paper. */
        Col cw[9] = {C(0x3B,0x4C,0xC0), C(0x68,0x83,0xE1), C(0x9A,0xB0,0xF2),
                     C(0xC6,0xD4,0xF0), C(0xDC,0xDC,0xDC), C(0xF2,0xCB,0xB7),
                     C(0xEE,0x91,0x74), C(0xD6,0x5B,0x4A), C(0xB4,0x04,0x26)};
        Col mg[9] = {C(0x00,0x00,0x04), C(0x1C,0x10,0x44), C(0x4F,0x12,0x7B),
                     C(0x81,0x25,0x81), C(0xB6,0x37,0x79), C(0xE1,0x51,0x5B),
                     C(0xF8,0x76,0x5C), C(0xFE,0xB1,0x7B), C(0xFC,0xFD,0xBF)};
        Col inf[9] = {C(0x00,0x00,0x04), C(0x1F,0x0C,0x48), C(0x55,0x0F,0x6D),
                      C(0x88,0x22,0x6A), C(0xB8,0x3B,0x5B), C(0xE3,0x5B,0x33),
                      C(0xF9,0x89,0x0F), C(0xF9,0xC0,0x32), C(0xFC,0xFF,0xA4)};
        Col pl[9] = {C(0x0D,0x08,0x87), C(0x47,0x02,0x9F), C(0x72,0x02,0x9E),
                     C(0x9C,0x17,0x9E), C(0xBD,0x3F,0x86), C(0xD8,0x57,0x6B),
                     C(0xED,0x79,0x53), C(0xFA,0x9E,0x3B), C(0xF0,0xF9,0x21)};
        /* cividis: viridis re-tuned so it survives red-green colour blindness
         * and greyscale printing with the same ordering. */
        Col cv[9] = {C(0x00,0x20,0x51), C(0x0C,0x38,0x6B), C(0x3B,0x4D,0x6E),
                     C(0x57,0x62,0x6D), C(0x70,0x77,0x6E), C(0x8A,0x8D,0x6C),
                     C(0xA8,0xA5,0x64), C(0xC8,0xBE,0x53), C(0xFD,0xEA,0x45)};
        Col rk[9] = {C(0x03,0x05,0x1A), C(0x35,0x11,0x3A), C(0x69,0x14,0x4B),
                     C(0x9B,0x1B,0x4A), C(0xC6,0x36,0x3B), C(0xE0,0x60,0x3E),
                     C(0xEC,0x8C,0x64), C(0xF3,0xB5,0x92), C(0xFA,0xEB,0xDD)};
        Col mk[9] = {C(0x0B,0x04,0x05), C(0x2B,0x1B,0x3E), C(0x38,0x3A,0x6E),
                     C(0x35,0x5F,0x8D), C(0x31,0x82,0x93), C(0x36,0xA5,0x93),
                     C(0x59,0xC8,0x87), C(0xA5,0xE2,0x8F), C(0xDE,0xF5,0xE5)};
        memcpy(vir, v, sizeof v); memcpy(jet, j, sizeof j);
        memcpy(bwr, b, sizeof b); memcpy(parula, pa, sizeof pa);
        memcpy(turbo, tu, sizeof tu); memcpy(coolwarm, cw, sizeof cw);
        memcpy(magma, mg, sizeof mg); memcpy(inferno, inf, sizeof inf);
        memcpy(plasma, pl, sizeof pl); memcpy(cividis, cv, sizeof cv);
        memcpy(rocket, rk, sizeof rk); memcpy(mako, mk, sizeof mk);
        init = 1;
    }
    Col out;
    switch (fs->kind) {
    case FILL_JET: ramp(jet, 9, t, &out); break;
    case FILL_PARULA: ramp(parula, 9, t, &out); break;
    case FILL_BWR: ramp(bwr, 3, t, &out); break;
    case FILL_TURBO: ramp(turbo, 11, t, &out); break;
    case FILL_COOLWARM: ramp(coolwarm, 9, t, &out); break;
    case FILL_MAGMA: ramp(magma, 9, t, &out); break;
    case FILL_INFERNO: ramp(inferno, 9, t, &out); break;
    case FILL_PLASMA: ramp(plasma, 9, t, &out); break;
    case FILL_CIVIDIS: ramp(cividis, 9, t, &out); break;
    case FILL_ROCKET: ramp(rocket, 9, t, &out); break;
    case FILL_MAKO: ramp(mako, 9, t, &out); break;
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
        unsigned int r, g, b;
        if (sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
            *out = C((int)r, (int)g, (int)b);
            return 0;
        }
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

/* cytoband gieStain -> colour: grey ramp for gpos*, red centromere. Shared by
 * the grammar-mode ideogram (render.c) and the cytoband track (render_tracks.c).
 * Values match the former static in render.c exactly (byte-identical output). */
Col stain_color(const char *s) {
    if (!strcmp(s, "acen")) { Col c = {0.878, 0, 0}; return c; }      /* #E00000 */
    if (!strcmp(s, "gneg")) return C_WHITE;
    if (!strcmp(s, "gpos25")) { Col c = {0.753, 0.753, 0.753}; return c; }
    if (!strcmp(s, "gpos50")) { Col c = {0.565, 0.565, 0.565}; return c; }
    if (!strcmp(s, "gpos75")) { Col c = {0.376, 0.376, 0.376}; return c; }
    if (!strcmp(s, "gpos100")) { Col c = {0, 0, 0}; return c; }
    Col c = {0.502, 0.502, 0.502}; return c;                          /* gvar/stalk */
}
