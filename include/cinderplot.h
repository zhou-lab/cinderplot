/* cinderplot.h — shared types for the Cinderplot proof-of-concept plotter. */
#ifndef CINDERPLOT_H
#define CINDERPLOT_H

#define CINDERPLOT_VERSION "0.25.0"

/* Size of the caller-supplied error buffer passed to every *_read / render /
 * dsl_parse entry point (see main.c: char err[CP_ERRLEN]). All error
 * formatting must be bounded to this length. Sized to hold the longest
 * message — the DSL "not implemented; supported: ..." list is ~730 bytes. */
#define CP_ERRLEN 1024

#include <cairo.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>       /* cp_logt below */

/* Fail-fast allocation wrappers. cinderplot is a one-shot CLI, so the sane
 * response to an allocation failure is a clear message and exit, not error
 * plumbing through every render path. Use these instead of raw
 * malloc/calloc/realloc/strdup so a NULL return can never be dereferenced.
 *
 * MEMORY POLICY: cinderplot renders one figure and exits, so it does not free.
 * DataFrames, Factors, cluster trees, grob arrays and label strings all live
 * until the process does, and several structures deliberately borrow slices of
 * others (a string cell points into the file image; a row name points into its
 * DataFrame) precisely because nothing outlives the process. This is a choice,
 * not an oversight -- but it does mean a leak checker reports thousands of
 * blocks, so use one only after freeing the big structures locally, or run it
 * against a single parse rather than a whole render. */
static inline void cp_oom(void) {
    fputs("cinderplot: out of memory\n", stderr);
    exit(1);
}
static inline void *cp_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);   if (!p) cp_oom();   return p;
}
static inline void *cp_xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);   if (!p) cp_oom();   return p;
}
static inline void *cp_xrealloc(void *old, size_t n) {
    void *p = realloc(old, n ? n : 1);   if (!p) cp_oom();   return p;
}
static inline char *cp_xstrdup(const char *s) {
    char *p = strdup(s);   if (!p) cp_oom();   return p;
}

typedef struct { double r, g, b; } Col;

/* ---------- base style constants (ggplot2 theme_gray, base_size 11, units:
 * PDF pt). theme_gray is the DEFAULT; see THEMES[] below for the others. ---- */
/* ggplot linewidth u -> lwd = u * 2.845276 in 1/96", -> pt */
static inline double lw_pt(double u) { return u * 2.845276 * 72.0 / 96.0; }

#define MARGIN       5.5
#define HALF_LINE    5.5
/* Base font size (PDF pt), a RUNTIME variable so --font-size / theme_*(base_size=)
 * can scale every label together (defined in main.c, default 11.0). The three
 * size macros derive from it, so all ~80 text sites pick up the runtime value
 * with no edit. cp_base_size = 11 reproduces the compile-time default exactly. */
extern double cp_base_size;
#define SZ_BASE      cp_base_size
#define SZ_AXIS_TEXT (0.8 * cp_base_size)
#define SZ_TITLE     (1.2 * cp_base_size)
/* track mode uses ONE flat label size (no title/axis/row hierarchy): 9pt at
 * the default base, scaling with --font-size / base_size like the rest. */
#define SZ_TRACK     (cp_base_size * 9.0 / 11.0)
#define TICK_LEN     (HALF_LINE / 2)
#define TXT_GAP      (0.8 * HALF_LINE / 2)
#define KEY_SIZE     17.3
#define PT_RADIUS    2.15
#define PANEL_SPACE  HALF_LINE
#define STRIP_PAD    (0.8 * HALF_LINE)
/* The figure font. A variable, not a constant: --font FAMILY overrides it
 * (main.c), and every cairo_select_font_face() site reads it, so one flag
 * reaches all four modes. Cairo substitutes silently when the family is
 * missing; main.c warns for that at startup (cp_font_resolves). */
#define FONT_FAMILY_DEFAULT "Arial"
extern const char *cp_font_family;
/* Chrome line-width scale: 1.0 reproduces ggplot2's base_line_size = 0.5.
 * theme_*(base_line_size=) sets it per figure; CINDERPLOT_BASE_LINE_SIZE in
 * the environment sets a personal default (main.c). Applies to theme
 * elements, axis ticks and frames — never to data strokes. */
extern double cp_line_scale;

static const Col C_PANEL  = {0.922, 0.922, 0.922};   /* grey92 */
static const Col C_WHITE  = {1, 1, 1};
static const Col C_AXTXT  = {0.302, 0.302, 0.302};   /* grey30 */
static const Col C_TICK   = {0.2, 0.2, 0.2};         /* grey20 */
static const Col C_BLACK  = {0, 0, 0};
static const Col C_KEYBG  = {0.949, 0.949, 0.949};   /* grey95 */
static const Col C_STRIP  = {0.851, 0.851, 0.851};   /* grey85 */
static const Col C_STRIPTXT = {0.102, 0.102, 0.102}; /* grey10 */

/* ---------- selectable themes (ggplot2 / ggthemes / ggpubr) --------------
 * A theme is pure data: colours + on/off flags + raw ggplot linewidth units
 * (lw_pt() applied at draw time so this table stays a constant literal).
 * THEME_GRAY (index 0) reproduces ggplot2's grey theme and is the memset
 * default. All themes use greys/white/black, so no new deps. */
typedef struct {
    Col panel_bg;   int panel_bg_on;
    Col grid_major; double grid_major_lw; int grid_major_on;
    Col grid_minor; double grid_minor_lw; int grid_minor_on;
    Col border;     double border_lw;     int border_on;
    Col axis_line;  double axis_line_lw;  int axis_line_on;
    Col tick;       int tick_on;
    Col axis_text;  int axis_text_on;
    Col axis_title; int axis_title_on;
    Col title;
    Col strip_bg;   int strip_bg_on; Col strip_text;
    Col key_bg;     int key_bg_on;
} Theme;

typedef enum { THEME_GRAY, THEME_BW, THEME_MINIMAL, THEME_CLASSIC, THEME_VOID,
               THEME_LINEDRAW, THEME_LIGHT, THEME_DARK, THEME_FEW } ThemeType;

#define GY(v) {v, v, v}
/* fields: panel,on | gridMaj,lw,on | gridMin,lw,on | border,lw,on |
 *         axisLine,lw,on | tick,on | axisText,on | axisTitle,on | title |
 *         strip,on,stripText | key,on   (greyNN = NN/100) */
static const Theme THEMES[] = {
/*GRAY    */ { GY(0.922),1, GY(1),0.5,1, GY(1),0.25,1, GY(0.2),0.5,0, GY(0),0.5,0, GY(0.2),1, GY(0.302),1, GY(0),1, GY(0), GY(0.851),1,GY(0.102), GY(0.949),1 },
/*BW      */ { GY(1),1, GY(0.922),0.5,1, GY(0.922),0.25,1, GY(0.2),0.5,1, GY(0),0.5,0, GY(0.2),1, GY(0.302),1, GY(0),1, GY(0), GY(0.851),1,GY(0.102), GY(1),1 },
/*MINIMAL */ { GY(1),0, GY(0.922),0.5,1, GY(0.922),0.25,1, GY(0.2),0.5,0, GY(0),0.5,0, GY(0.2),0, GY(0.302),1, GY(0),1, GY(0), GY(0.851),0,GY(0.102), GY(0.949),0 },
/*CLASSIC */ { GY(1),1, GY(1),0.5,0, GY(1),0.25,0, GY(0.2),0.5,0, GY(0),0.5,1, GY(0),1, GY(0.302),1, GY(0),1, GY(0), GY(1),0,GY(0.102), GY(1),1 },
/*VOID    */ { GY(1),0, GY(1),0.5,0, GY(1),0.25,0, GY(0.2),0.5,0, GY(0),0.5,0, GY(0.2),0, GY(0.302),0, GY(0),0, GY(0), GY(0.851),0,GY(0.102), GY(0.949),0 },
/*LINEDRAW*/ { GY(1),1, GY(0),0.1,1, GY(0),0.05,1, GY(0),0.1,1, GY(0),0.5,0, GY(0),1, GY(0.302),1, GY(0),1, GY(0), GY(0),1,GY(1), GY(1),1 },
/*LIGHT   */ { GY(1),1, GY(0.87),0.5,1, GY(0.87),0.25,1, GY(0.7),0.25,1, GY(0),0.5,0, GY(0.7),1, GY(0.302),1, GY(0),1, GY(0), GY(0.7),1,GY(1), GY(1),1 },
/*DARK    */ { GY(0.5),1, GY(0.42),0.5,1, GY(0.42),0.25,1, GY(0.2),0.5,0, GY(0),0.5,0, GY(0.2),1, GY(0.302),1, GY(0),1, GY(0), GY(0.15),1,GY(0.9), GY(1),1 },
/*FEW     */ { GY(1),1, GY(1),0.5,0, GY(1),0.25,0, GY(0.302),0.5,1, GY(0),0.5,0, GY(0.302),1, GY(0.302),1, GY(0.302),1, GY(0.302), GY(1),0,GY(0.302), GY(1),1 },
};
#undef GY

/* ---------- breaks.c: extended Wilkinson labeling ---------- */
int extended_breaks(double dmin, double dmax, int m, double *out, int max_out);
void fmt_num(double v, char *buf, size_t cap);
int axis_decimals(const double *br, int n);
void fmt_break(double v, int decimals, char *buf, size_t cap);
/* majors for a log scale of the given base, positions in TRANSFORMED (log)
 * space, labels in data space; returns count */
int log_breaks(int base, double tlo, double thi, double *tmaj, char **labs,
               int max_out);

/* ---------- smooth.c: LOESS (local quadratic, tricube weights) ----------
 * Fit y ~ x at `nout` evenly spaced x over the data's range with the nearest
 * `span` fraction of the n points; writes the curve to ox/oy (each at least
 * nout long) and returns how many points it holds, or -1 with `err` set.
 * geom_smooth() and the signal() track both draw this. */
int cp_loess(const double *x, const double *y, int n, double span, int nout,
             double *ox, double *oy, char *err);

/* Data value -> log axis space. base 0 is the identity, so call sites can pass
 * spec->log_x straight through instead of branching on whether it is set. */
static inline double cp_logt(int base, double v) {
    return base == 10 ? log10(v) : base == 2 ? log2(v) : v;
}

/* ---------- palette.c: scale_colour_hue for n levels ---------- */
void hue_palette(int n, Col *out);

/* continuous fill scales */
typedef enum { FILL_VIRIDIS, FILL_JET, FILL_BWR, FILL_GRADIENT, FILL_GRADIENT2,
               FILL_PARULA, FILL_TURBO, FILL_COOLWARM, FILL_MAGMA, FILL_INFERNO,
               FILL_PLASMA, FILL_CIVIDIS, FILL_ROCKET, FILL_MAKO,
               FILL_BREWER } FillKind;
typedef struct {
    FillKind kind;
    Col low, mid, high;      /* gradient / gradient2 */
    double midpoint;         /* gradient2, default 0 */
    double lim_lo, lim_hi;   /* limits: domain + out-of-range squish */
    int has_limits;
    /* FILL_BREWER interpolation stops (direction= reversal applied at parse).
     * 12 = the largest ColorBrewer class count (Paired/Set3); sizing this 11
     * dropped their 12th stop at compile (a warning) and made brewer_lookup's
     * memcpy a buffer overflow — the 0.13.0 scale_colour_brewer segfault. */
#define BREWER_MAX_STOPS 12
    Col stops[BREWER_MAX_STOPS]; int nstops;
} FillScale;
Col fill_map(const FillScale *fs, double t01);   /* t in [0,1] */
/* named ColorBrewer palette -> its max-class stops (from RColorBrewer).
 * is_qual: qualitative (Set/Paired/...) vs a seq/div ramp. 0 = found. */
int brewer_lookup(const char *name, Col *stops, int *nstops, int *is_qual);
/* map v in [dmin,dmax] -> colour, honouring gradient2's midpoint */
Col fill_map_value(const FillScale *fs, double v, double dmin, double dmax);
int parse_color(const char *s, Col *out);        /* names + #RRGGBB; 0 = ok */
Col stain_color(const char *s);                  /* cytoband gieStain -> colour */
static const Col C_NA = {0.753, 0.753, 0.753};   /* #C0C0C0, wheatmap na.color */

/* ---------- csv.c: data frame ---------- */
typedef enum { COL_NUM, COL_STR } ColType;
typedef struct {
    char *name;
    ColType type;
    double *num;    /* COL_NUM: values, NaN for NA/empty */
    char **str;     /* COL_STR */
} Column;
/* `backing` owns the raw file image when any COL_STR column points into it;
 * it is NULL once every column typed numeric and the image could be released.
 * String cells are borrowed slices of it, never individually allocated. */
typedef struct { int nrow, ncol; Column *cols; char *backing; } DataFrame;

DataFrame *df_read_csv(const char *path, char *err);   /* "-" = stdin */
void cp_set_no_header(int on);   /* headerless input: name columns V1, V2, ... (R style) */
/* One-shot: restrict the *next* df_read_csv() to these column names. Cleared
 * by that call, so whole-matrix readers are unaffected. NULL/0 = keep all. */
void cp_set_needed_cols(char *const *names, int n);
const Column *df_col(const DataFrame *df, const char *name);

typedef struct { int nlev; char **levels; int *idx; } Factor; /* idx[row] or -1 */
Factor *factor_make(const DataFrame *df, const Column *c);
/* Reorder a factor's levels to `want`, the R `factor(x, levels=)` order.
 * `want` must be a complete permutation of the levels present in the data, so
 * levels= can only reorder — never silently drop rows; `what` names the
 * aesthetic in the error message. Returns 0, or -1 with `err` filled. */
int factor_relevel(Factor *f, int nrow, char *const *want, int nwant,
                   const char *what, char *err);

/* ---------- gtable.c: the layout engine ---------- */
typedef enum { U_PT, U_NULL } UKind;
typedef struct { UKind k; double v; } Unit;
static inline Unit upt(double v)   { Unit u = {U_PT, v};   return u; }
static inline Unit unull(double w) { Unit u = {U_NULL, w}; return u; }

typedef enum { G_RECT, G_LINE, G_POLYLINE, G_POLYGON, G_TEXT, G_POINTS, G_TABLE,
               G_AXIS_X, G_AXIS_Y, G_IMAGE, G_IDEOGRAM } GType;
typedef enum { V_TOP, V_BOTTOM, V_INKCENTER } VAlign;

typedef struct GTable GTable;
typedef struct {
    GType type;
    int r0, c0, r1, c1;                        /* cell span, inclusive */
    int clip;
    int band_clip; double band_y0, band_y1;    /* with clip: restrict the clip to
                                                * this npc y-band of the cell (a
                                                * strip inside a track row) */
    Col col;
    double x0, y0, x1, y1, lw;                 /* line / sub-rect, npc */
    int sub;                                   /* G_RECT: use x0..y1 sub-rect */
    int stroke;                                /* G_RECT: stroke (lw) not fill */
    const char *str; double size, tx, ty, hj;  /* text, npc anchor */
    VAlign va; int rot90;
    int text_box; Col box_fill, box_line;      /* G_TEXT: bg box (geom_label) */
    int n;                                     /* points / axis breaks */
    const double *px, *py; const Col *pcol; double radius;
    const double *pradius;                     /* G_POINTS: per-point radius (size aes); NULL = use `radius` */
    const int *pshape;                         /* G_POINTS: per-point glyph (shape aes); NULL = circles */
    int shape;                                 /* G_POINTS: one glyph for every point */
    int raster;                                /* G_POINTS: rasterize into an embedded image */
    int dash;                                  /* G_LINE/G_POLYLINE: 0 solid, 1 dashed, 2 dotted */
    int snap;                                  /* G_LINE: snap an axis-aligned hairline to the
                                                * pixel grid on raster surfaces (heatmap grid=) */
    double alpha;                              /* 0 = opaque (unset); else 0..1 fill/stroke alpha */
    char **labels;                             /* axis tick labels */
    const double *label_pos;                   /* optional label positions, else ticks */
    double label_angle;                        /* G_AXIS_X: degrees CCW, 0 = horizontal */
    double rot;                                /* G_TEXT: degrees CCW about (tx,ty) */
    const double *mtpos, *mtlen; int mtn;      /* minor axis ticks (log): npc pos + length (pt) */
    Col tick_col, text_col;                    /* G_AXIS_*: themed colours (opt-in) */
    int axis_styled, hide_ticks, hide_text;    /* 0 = legacy C_TICK/C_AXTXT (heatmap/tracks) */
    GTable *child;
    unsigned char *img; int img_w, img_h;      /* G_IMAGE: ARGB32 buffer */
} Grob;

/* Bound on a gtable's rows/columns. It leaks into user-facing limits -- a
 * discrete colour legend reserves 2*nlev+1 rows, so this was a 15-level cap on
 * a colour aesthetic, which 20 cell types is enough to hit. The arrays are
 * inside a heap-allocated GTable, so the cost of raising it is a few KB per
 * figure and no code walks the bound itself. */
#define GT_MAXDIM 256
struct GTable {
    int nrow, ncol;
    Unit rowh[GT_MAXDIM], colw[GT_MAXDIM];
    double rowy[GT_MAXDIM + 1], colx[GT_MAXDIM + 1];
    Grob *grobs; int ngrobs, cap;
};

Grob *gt_add(GTable *t, GType type, int r0, int c0, int r1, int c1);
double gt_fixed_w(const GTable *t);
double gt_fixed_h(const GTable *t);
void gt_resolve(GTable *t, double x, double y, double w, double h);
void gt_render(GTable *t, cairo_t *cr);
/* Rendered advance of an axis label, accounting for "10^k" superscripts. */
double cp_label_w(cairo_t *cr, double size, const char *s);

/* Create the output surface, choosing the backend from the file extension:
 * ".svg" -> SVG, ".png" -> raster image (at the current DPI), else PDF.
 * Dimensions in points; PNG surfaces carry a device scale so callers keep
 * drawing in points. */
cairo_surface_t *cp_surface_create(const char *out, double w_pt, double h_pt);
/* --editable-svg: emit every label as its own <text> element with a single
 * x/y (svglite-style; no per-glyph dx/dy lists), instead of Cairo's glyph
 * outlines, so Inkscape/Illustrator can retype labels. */
void cp_set_svg_text(int on);
void cp_set_dpi(double dpi);                        /* PNG raster resolution (default 96) */
/* Emit the finished surface: write_to_png for image surfaces, surface_finish
 * for vector ones. Returns the resulting cairo status. */
cairo_status_t cp_surface_emit(cairo_surface_t *surf, const char *out);
/* one point glyph, path only -- caller fills. shape 0..5, see cp_point_path. */
void cp_point_path(cairo_t *cr, int shape, double cx, double cy, double r);

/* ---------- dsl.c: verbatim ggplot subset ---------- */
/* col NULL = unset; levels (from factor(col, levels=c(...))) imposes the
 * discrete order instead of factor_make()'s sort */
typedef struct {
    char *col; int is_factor; char *expr;
    char **levels; int nlevels;
    int is_fill;         /* the colour aes only: written as fill= (not colour=).
                          * geom_boxplot() renders the two differently, as
                          * ggplot2 does: fill= colours the box body, colour=
                          * the outline. */
} AesEntry;

typedef enum { GEOM_POINT, GEOM_JITTER, GEOM_LINE, GEOM_COL, GEOM_HISTOGRAM, GEOM_BOXPLOT, GEOM_BAR,
               GEOM_SEGMENT, GEOM_RECT, GEOM_DENSITY, GEOM_SMOOTH, GEOM_TILE,
               GEOM_HLINE, GEOM_VLINE, GEOM_ABLINE, GEOM_TEXT, GEOM_LABEL,
               GEOM_ERRORBAR, GEOM_LINERANGE } GeomType;
typedef struct {
    GeomType type;
    int bins;
    char *data;          /* per-layer data file (NULL = inherit) */
    char *ycol;          /* per-layer y column override (NULL = inherit) */
    Col color; int has_color;   /* constant layer colour override */
    int color_is_fill;          /* ... written as fill= (geom_boxplot draws
                                 * fill= on the body, colour= on the chrome) */
    double bw, adjust;          /* geom_density: bandwidth (0 = nrd0) x adjust */
    double span;                /* geom_smooth: loess span (0 = ggplot's 0.75) */
    int se_given;               /* geom_smooth: se= was written out */
    double slope, intercept;    /* geom_abline; hline/vline store value in intercept */
    int has_slope, has_intercept;
    double txt_size;            /* geom_text/label font size (ggplot mm; 0 = default) */
    double point_size;          /* geom_point size (ggplot units; 0 = default 1.5) */
    int repel;                  /* geom_text_repel/geom_label_repel: force placement */
    double nudge_x, nudge_y;    /* geom_text/label: constant offset (data units) */
    int dash;                   /* linetype=: 0 solid, 1 dashed, 2 dotted */
    double alpha;               /* alpha=: 0 = unset (opaque), else 0..1 */
    /* geom_jitter: half-range of the random offset, in DATA units, so on a
     * discrete axis (spacing 1) width=0.2 spans one fifth of the slot either
     * side. `seed` keeps a figure reproducible across renders. */
    int no_outliers;            /* geom_boxplot(outlier.shape=NA): the points are
                                 * already drawn by a jitter layer over the box */
    double eb_width;     /* geom_errorbar(width=): cap width in x-axis units
                          * (transformed space; 0 = the 0.25 default) */
    double tile_lw;      /* geom_tile(linewidth=): border stroke width in
                          * ggplot linewidth units (0 = the 0.1 default) */
    double line_lw;      /* linewidth= (size= alias) on the stroke geoms --
                          * line, smooth, segment, hline/vline/abline,
                          * errorbar/linerange -- in ggplot linewidth units
                          * (0 = the geom's default: 0.5, or 1 for smooth) */
    double txt_angle;    /* geom_text(angle=): degrees CCW; hjust= anchors in
                          * the rotated frame. Not on geom_label (the box does
                          * not rotate) or the repel geoms (they measure
                          * unrotated extents). */
    double txt_hjust; int has_txt_hjust;
    double jitter_w, jitter_h;
    unsigned jitter_seed; int has_jitter_seed;
    int raster;                 /* geom_point(raster=TRUE): draw the layer into an
                                 * embedded image instead of per-point vector marks
                                 * (ggrastr's idea); axes and text stay vector */
} Layer;
#define MAX_LAYERS 8

/* matrix (wheatmap) mode: anchor-placed objects */
typedef enum { PL_FULL, PL_TOP_OF, PL_BENEATH, PL_RIGHT_OF, PL_LEFT_OF } PlaceKind;
typedef struct {
    PlaceKind kind;
    int given;                     /* 1 = the spec wrote a placement verb; the
                                    * default kind for a 2nd+ object is TOP_OF,
                                    * and grammar-mode annotation() must tell
                                    * that default from an explicit one */
    char *anchor;                  /* NULL = previous object */
    double pad, width, height;     /* npc; width/height < 0 = auto */
} HPlace;
typedef enum { HM_HEATMAP, HM_ANNOTATION, HM_LEGEND, HM_DENDROGRAM } HMType;
typedef enum { CL_NONE, CL_ROWS, CL_COLS, CL_BOTH,
               CL_DIAGONAL,    /* columns follow the row names; no clustering */
               CL_SYMMETRIC    /* cluster rows, then columns follow them */
} ClusterMode;
typedef enum { SIDE_NONE, SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM } Side;
typedef struct {
    HMType type;
    char *name;                    /* auto-assigned if absent */
    char *data;                    /* heatmap/annotation: csv path */
    char *title;                   /* legend title (NULL = default/none) */
    char *column;                  /* annotation: which column to colour by
                                    * (NULL = the last, the historical default) */
    ClusterMode cluster;           /* heatmap only */
    int discrete;                  /* heatmap: fill trigger. 0 = decide from the
                                    * cell types (text => categorical), 1 =
                                    * discrete=TRUE (numeric codes ARE levels),
                                    * -1 = discrete=FALSE (numbers, so text
                                    * cells are an error as they always were) */
    Side rownames, colnames;       /* heatmap label sides (SIDE_NONE = off) */
    int label_data;                /* annotation: in-situ value runs + bezier leaders;
                                    * heatmap: print each cell's value */
    double aspect;                 /* heatmap: drawn width/height ratio (0 = free) */
    int box;                       /* heatmap/annotation: frame the cells (0 = none) */
    Col box_col;                   /* box= colour, when box is set */
    int grid;                      /* heatmap/annotation: stroke cell separators (0 = none) */
    Col grid_col;                  /* grid= colour, when grid is set */
    HPlace place;
    int name_given;                /* name= was written (hm_new assigns one otherwise) */
    /* legend(missing=) on the track browser: the label of the final swatch a
     * discrete matrix() key adds for its background/NA colour ("NA" when
     * unset); missing=none drops that swatch. */
    char *missing; int missing_off;
} HMObj;
#define MAX_HMOBJS 16

/* highlight("row","col"): a bounding box on one heatmap cell — the reader-
 * visible "this is the picked cell" for a parameter grid (ggplot2's
 * geom_tile(subset, colour=, fill=NA) idiom). */
typedef struct {
    char *row, *col;               /* the cell's row/column name */
    char *target;                  /* name= of the heatmap (NULL = the sole one) */
    Col color;                     /* box colour, default red */
    /* track-mode form: highlight(name="beta", row="S1", region="chr:beg-end"
     * [, colour=][, linetype=][, label=]) boxes one sample row of a matrix()
     * track over a genomic span (the probe columns falling in it); or the
     * file form highlight("boxes.tsv", name="beta") with columns
     * `row chrom beg end [colour] [label] [linetype]`, one box per line,
     * read at render time so a classifier's output drops in unbounded. */
    char *region;                  /* the span as written; NULL = heatmap form */
    char chrom[64]; long beg, end;
    int dash;                      /* 0 solid, 1 dashed, 2 dotted */
    char *label;                   /* tiny corner label, NULL = none */
    char *file;                    /* file form; row/region are then per line */
} CellHighlight;
#define MAX_HIGHLIGHTS 64

/* annotate("text"|"segment"|"rect", x=, y=, ...): a one-off mark at literal
 * data coordinates, grammar mode only — ggplot2's annotate(). */
typedef enum { ANNO_TEXT, ANNO_SEGMENT, ANNO_RECT } AnnoKind;
typedef struct {
    AnnoKind kind;
    double x, y, xend, yend;       /* rect also spells these xmin/xmax/ymin/ymax */
    int has_x, has_y, has_xend, has_yend;
    char *label;                   /* text */
    Col color; int has_color;      /* text/segment default black; rect grey85 */
    double size;                   /* text size in pt (0 = axis-text default) */
    double hjust, vjust;           /* text anchoring, ggplot semantics:
                                    * hjust 0 = text starts at x, 1 = ends at x;
                                    * vjust 0 = text sits above y, 1 = below.
                                    * Default 0.5/0.5 = centred on the point. */
    int has_hjust, has_vjust;
    double angle;                  /* text rotation, degrees CCW (ggplot);
                                    * hjust applies in the rotated frame,
                                    * vjust does not (and errors with it) */
} Annotate;
#define MAX_ANNOTATES 16

/* track (locus-browser) mode: stacked tracks over one genomic region */
typedef enum { TRK_COVERAGE, TRK_INTERVAL, TRK_GENES, TRK_ARCS,
               TRK_MATRIX, TRK_CYTOBAND, TRK_SIGNAL } TrackType;
typedef struct {
    TrackType type;
    char *data;          /* input file (BED/bedGraph/GFF/BEDPE/matrix TSV/cytoband) */
    char *name;          /* left-margin label */
    double height;       /* row weight (<=0 = auto) */
    double max_value;    /* coverage y-max (<=0 = auto) */
    Col color; int has_color;
    int cluster;         /* matrix track: 1 = cluster the sample rows */
    int hide_rownames;   /* matrix track: 1 = don't draw sample row labels */
    int hide_colnames;   /* matrix track: 1 = don't draw per-probe column labels */
    int all_transcripts; /* genes track: 1 = all isoforms; 0 = canonical (longest/gene) */
    /* matrix(x=genomic): draw each cell at the probe's own coordinate instead
     * of in probe-index space. Index space gives every column equal width and
     * a fan of leader lines to its true position -- readable when the columns
     * are the point, useless when the LOCUS is: 550 CpGs in 20 kb become a
     * uniform grid that no longer lines up with the gene models above it.
     * Genomic space draws only the cells that exist, so a dense CpG cluster
     * reads dense and a gap reads as a gap. */
    int genomic_x;       /* 1 = cells at their coordinate; 0 = probe-index grid */
    double bar_bp;       /* x=genomic: cell width in bp (<=0 = the probe's own
                          * span, widened to stay visible at the panel's scale) */
    Col bg_color; int has_bg;   /* background where there is no probe */
    /* matrix(rowgroup="SEP"): split each sample name on SEP -- the part before
     * is the group (a cell type), the part after is the row's own label. Sixty
     * rows of "Bladder-Epithelial | truth" is an unreadable stack of repeated
     * prefixes; the group is written once beside its run, each row keeps only
     * what distinguishes it, and a rule separates one run from the next. */
    char *rowgroup;
    /* matrix(rowcolour="file.tsv"): a `group colour` table. Each group name in
     * the gutter is written in its colour with a filled swatch beside it, so a
     * lineage reads at a glance; a group absent from the file stays black. */
    char *rowcolour;
    /* matrix/signal(rowbar=on): instead of the small swatch beside each group
     * name, draw a filled rectangle spanning that group's whole run of rows, in
     * the group's colour, in a thin column against the left edge of the panel --
     * the band's extent, the way heatmap mode's annotation(left_of()) reads.
     * Needs rowcolour= (nothing to colour the band otherwise). Default off.
     * This is the single-band back-compat form (the band follows rowgroup=). */
    int rowbar;
    /* matrix/signal(rowmeta="samples.tsv"): a sample metadata sheet. The first
     * column is the sample key (matching the `sample` column of the long input);
     * every other column is an annotation (cell_type, source, ...) that
     * rowbar="col,col2" can draw as its own band. Mutually exclusive with
     * rowgroup= (one describes the annotation, the other splits a name). */
    char *rowmeta;
    /* matrix/signal(rowbar="col,col2"): the ORDERED list of rowmeta= columns to
     * draw as adjacent bands against the panel's left edge (leftmost = first).
     * Each band spans the consecutive run of rows sharing that column's value;
     * the FIRST column also drives the group label text and the run rules.
     * Requires rowmeta=. Empty when rowbar= is the on/off back-compat form. */
    char **rowbar_cols; int n_rowbar_cols;
    /* interval(labels=): 0 = auto -- a feature's name is drawn only where it
     * fits before the next feature in its lane, so 380 CpG ticks do not print
     * 380 names over one another; 1 = on draws every name regardless;
     * -1 = off draws none. */
    int labels;
    /* signal("long.tsv"): continuous traces, one strip per sample stacked
     * like matrix() rows, one line per series inside a strip, on the same
     * genomic x as the tracks above. The manuscript case is a ground-truth
     * trace over N reconstructions per cell type, under the binary matrix. */
    double gap_pt;       /* signal(gap=): blank space between adjacent strips,
                          * in pt (<0 = the 2pt default). Two lanes that butt
                          * read as one trace crossing a baseline; a hairline of
                          * white between them reads as two lanes. */
    double smooth;       /* loess span in (0, 1]; 0 = the raw polyline */
    int points;          /* 1 = the raw points behind the lines (small, faint) */
    double ylim_lo, ylim_hi; int has_ylim;   /* pin every strip's value range;
                                              * else each strip's own min..max */
    Col *ser_col; char **ser_name; int nser_col;   /* colour=c("series"="#..", ...);
                                                    * names NULL = positional */
    double line_lw;      /* linewidth= (size=), ggplot units; 0 = 0.5 */
    /* matrix(discrete=TRUE): the numeric cell values are category CODES (a
     * 0/1 call matrix), not measurements -- each distinct value becomes a
     * level with its own colour (scale_fill_manual(values=) by level name,
     * else the hue wheel), and legend() draws a key instead of a colourbar.
     * The same switch heatmap(discrete=TRUE) is; the levels come from the
     * same routine (cp_numeric_levels). */
    int discrete;
} TrackObj;
#define MAX_TRACKS 12
/* colour=c(...) on a track, and scale_*_manual(values=): the list cap */
#define MAX_MANUAL_COLS 64

typedef struct {
    char *data_path;
    AesEntry x, y, colour;          /* colour also accepts fill= */
    AesEntry xend, yend;            /* geom_segment endpoints; yend also = ymax */
    AesEntry ymin;                  /* geom_errorbar/linerange lower bound */
    AesEntry label;                 /* geom_text/geom_label label column */
    AesEntry size;                  /* geom_point size: numeric -> point area */
    AesEntry shape;                 /* geom_point shape: discrete -> point glyph */
    AesEntry group;                 /* geom_line/geom_smooth series key: discrete,
                                     * no legend; crossed with a discrete colour */
    AesEntry chrom;                 /* genome scale: chromosome column */
    int coord_flip;                 /* coord_flip(): swap the x and y axes */
    /* chord mode: chord("links.csv") — a circlize-style chord diagram from a
     * (from, to, value) table; its own mode, like the tree */
    int chord_mode;
    char *chord_from, *chord_to, *chord_value;   /* column names (NULL = default) */
    double chord_gap;               /* gap between sectors, degrees (-1 = unset -> 2; 0 is 0) */
    char **chord_order; int n_chord_order;   /* order=c(...): explicit sector order */
    int chord_bipartite;            /* bipartite=TRUE: from-group then to-group,
                                     * with a bigger gap between the groups */
    double chord_alpha;             /* ribbon alpha (0 = 0.6) */
    int polar;                      /* coord_polar(): radar chart — discrete-x
                                     * categories become spokes, y the radius;
                                     * geom_line()/geom_point() only */
    double polar_start;             /* start angle, radians CW from 12 o'clock */
    char *genome_seqinfo;           /* scale_x_genome: seqinfo TSV path */
    char *ideogram_path;            /* ideogram(): cytoband TSV path */
    Layer layers[MAX_LAYERS];
    int nlayers;
    /* Log axis base: 0 = linear, else the base (10 from scale_*_log10(), 2 from
     * scale_*_log2()). Truthy means "this axis is logged", which is how most
     * call sites read it; only the transform and the break/tick generators
     * care which base it is. */
    int log_x, log_y;
    double xlim_lo, xlim_hi, ylim_lo, ylim_hi;   /* user axis limits (data space) */
    int has_xlim, has_ylim;                       /* xlim()/ylim() or scale_*_log10(limits=) */
    int x_pct, y_pct;                             /* scale_*_continuous(labels=percent) */
    /* Sized for a labelled category axis built by hand (a 63-class panel is
     * an ordinary figure); a few KB in the one PlotSpec, nothing walks it. */
#define MAX_BREAKS 256
    /* scale_*_(discrete|continuous)(expand=c(mult, add)): axis expansion
     * override. ggplot defaults: continuous c(0.05, 0), discrete c(0, 0.6);
     * expand=c(0,0) makes the panel frame hug a tile grid.
     * coord_cartesian(expand=FALSE) zeroes both. */
    double x_exp_mult, x_exp_add, y_exp_mult, y_exp_add;
    int has_x_expand, has_y_expand;
    double x_breaks[MAX_BREAKS], y_breaks[MAX_BREAKS];   /* scale_*_continuous(breaks=c(...)) */
    int n_x_breaks, n_y_breaks;                   /* 0 = choose them automatically */
    char *x_break_labs[MAX_BREAKS], *y_break_labs[MAX_BREAKS];   /* labels=c(...): tick
                                                   * text, paired 1:1 with breaks= */
    int n_x_break_labs, n_y_break_labs;           /* 0 = format the break numbers */
    ThemeType theme;                              /* theme_*(); THEME_GRAY = 0 = default */
    int no_legend;                  /* guides(colour="none"|fill="none") or --no-legend:
                                     * drop the colour/fill guide */
    int no_legend_size, no_legend_shape;   /* guides(size="none") / guides(shape="none"):
                                     * the size and shape guides are separate builders
                                     * in render.c and are dropped separately */
    char *facet_var;
    char **facet_levels; int n_facet_levels;      /* facet_wrap(~v, levels=c(...)) */
    int free_x, free_y;                           /* facet_wrap(scales=): per-panel ranges */
    int free_colour;                              /* scales="free_colour": each facet builds
                                                   * its own colour scale + legend block */
    int facet_ncol, facet_nrow;                   /* facet_wrap(ncol=/nrow=); 0 = auto */
    /* facet_grid(rowvar ~ colvar): a true 2-D grid. Every (row, col)
     * combination gets a panel, the empty ones included -- that is what
     * separates it from facet_wrap, which lays out only the combinations the
     * data holds. Either side may be "." (one-sided), leaving it NULL. */
    int facet_grid;
    char *facet_rowvar, *facet_colvar;
    double x_angle, y_angle;                      /* scale_*_discrete(angle=); <0 = auto */
    int tree_layout;                              /* 0 rectangular, 1 slanted, 2 circular */
    int tree_mode, tree_tiplab, tree_nodelab;     /* geom_tree()/geom_tiplab()/geom_nodelab() */
    int tree_nodepoint, tree_tippoint;            /* geom_nodepoint()/geom_tippoint() */
    int tree_lab_id;                              /* geom_*lab(label=id): show numbers */
    /* Tables joined to the tree on node/tip NAME. Several rows for one name
     * draw several marks, which is how a node belonging to more than one
     * category advertises itself. */
    char *tree_np_data, *tree_np_col;             /* internal nodes */
    char *tree_tp_data, *tree_tp_col;             /* tips */
    char *tree_tl_data, *tree_tl_col;             /* tip LABEL colour */
    char *lab_title, *lab_x, *lab_y, *lab_colour, *lab_fill;
    char *lab_subtitle, *lab_caption;             /* labs(subtitle=, caption=) */
    /* scale_colour/fill_manual(values=): discrete palette override. 64 slots:
     * 16 silently dropped the rest, and a 20-level figure painted its tail
     * levels grey with no warning. */
    Col manual_cols[MAX_MANUAL_COLS]; char *manual_names[MAX_MANUAL_COLS];   /* names NULL = positional */
    int n_manual, has_manual;
    /* scale_*_manual(labels=c("0"="Unmethylated", ...)): what the KEY prints
     * for a level, keyed by the level as written in the data (values= still
     * keys colours by that); names NULL = positional, one per level. Read by
     * the discrete keys of heatmap and track mode (cp_key_labels). */
    char *manual_labs[MAX_MANUAL_COLS]; char *manual_lab_names[MAX_MANUAL_COLS];
    int n_manual_labs;
    char *brewer_disc;              /* scale_*_brewer(palette=): the set's name,
                                     * for a level-count-vs-palette-size check */
    int identity_scale;             /* scale_*_identity(): the mapped column's
                                     * values ARE the colours; no legend */
    double base_line_size;          /* theme_*(base_line_size=): chrome line
                                     * width, 0 = unset (env var or 0.5) */
    double base_size;               /* theme_*(base_size=): base font size in pt,
                                     * 0 = unset (--font-size or the 11.0 default);
                                     * a spec value overrides the CLI flag */
    int legend_inside;              /* theme(legend.position="inside"): draw the
                                     * legend block(s) INSIDE the panel(s) at
                                     * (leg_ix, leg_iy) npc instead of reserving
                                     * a margin or a row */
    double leg_ix, leg_iy;
    int legend_reverse;             /* guide_legend(reverse=TRUE): flip the key
                                     * order without flipping the factor */
    int legend_ncol, legend_nrow;   /* guides(colour=guide_legend(ncol=/nrow=)):
                                     * fold a discrete legend over columns; nrow
                                     * caps rows (each free_colour block derives
                                     * its own column count). 0 = one column. */
    /* matrix mode */
    HMObj hobjs[MAX_HMOBJS];
    int nhobjs;
    CellHighlight hls[MAX_HIGHLIGHTS];
    int nhls;
    Annotate annos[MAX_ANNOTATES];
    int nannos;
    FillScale fill;
    int has_fill;                   /* a scale_fill_*() was given (else default) */
    /* grammar mode: continuous colour scale (scale_colour_gradient*) */
    FillScale colour_scale;
    int has_colour_scale;
    /* track mode (locus browser) */
    TrackObj tobjs[MAX_TRACKS];
    int ntracks;
    char *region;                   /* chr:start-end */
    char *regions_path;             /* regions(): BED of windows laid side by side
                                     * on one broken axis, gaps between them */
} PlotSpec;

int dsl_parse(const char *src, PlotSpec *spec, char *err);  /* 0 = ok */

/* ---------- render.c ---------- */
int render_plot(const PlotSpec *spec, const DataFrame *df, const char *out,
                double w_pt, double h_pt, char *err);

/* ---------- track_io.c: BED / bedGraph / BEDPE (region-filtered) ---------- */
typedef struct { long start, end; char *name; char strand; double score; } Interval;
typedef struct { long start, end; double val; } SigBin;
typedef struct { long a_start, a_end, b_start, b_end; double score; } Link;
typedef struct { long start, end; } Exon;
typedef struct {
    long tx_start, tx_end, cds_start, cds_end;   /* thick = CDS */
    char *name; char strand;
    Exon *exons; int nexon;
} GeneModel;
int region_parse(const char *s, char *chrom, long *start, long *end);   /* 0 = ok */
Interval *bed_read(const char *path, const char *chrom, long rs, long re, int *n, char *err);
SigBin   *bedgraph_read(const char *path, const char *chrom, long rs, long re, int *n, char *err);
Link     *bedpe_read(const char *path, const char *chrom, long rs, long re, int *n, char *err);
GeneModel *bed12_read(const char *path, const char *chrom, long rs, long re, int *n, char *err);

/* ---------- gzio.c: gzip / BGZF+tabix input ---------- */
char *gz_read_all(const char *path, char *err);           /* whole-file gzip inflate */
char *tabix_slurp_region(const char *path, const char *chrom, long beg, long end, char *err);

/* ---------- render_tracks.c: locus track-browser mode ---------- */
int render_tracks(const PlotSpec *spec, const char *out,
                  double w_pt, double h_pt, char *err);

/* ---------- legend.c: the fill legend, shared by heatmap and track modes ----
 * Legends are RIGID chrome, sized in physical units (never coupled to the
 * matrix), following ComplexHeatmap: bar 4mm thick, ~28mm long. */
#define MM       (72.0 / 25.4)
#define LEG_BAR  (4.0 * MM)         /* colorbar thickness  */
#define LEG_LEN  (28.0 * MM)        /* colorbar length     */
#define LEG_GRID (4.0 * MM)         /* discrete key square */
#define LEG_GAP  (2.0 * MM)         /* gap between keys    */
#define CP_LEG_MAXBR 16             /* colourbar break slots */
/* the extended breaks that fall within [lo, hi]; br holds CP_LEG_MAXBR */
int cp_legend_breaks(double lo, double hi, double *br);
/* extent across the reading direction (pt) of a vertical key / colourbar */
double cp_key_across_pt(cairo_t *cr, char *const *labels, int nlev);
double cp_colourbar_across_pt(cairo_t *cr, double lo, double hi);
/* Emit a key (swatches down from `top`, left edge `sx`) or a colourbar
 * (lower-left corner at bx0,by0; `vert` stands it up; `dir` +1 puts the
 * labels right/above, -1 left/below) as grobs in cell (r, c), whose size in
 * points converts the physical dimensions to that cell's npc. */
void cp_key_draw(GTable *T, int r, int c, char *const *labels, const Col *pal,
                 int nlev, double sx, double top, double cw_pt, double ch_pt);
void cp_colourbar_draw(GTable *T, int r, int c, const FillScale *fs,
                       double lo, double hi, int vert, int dir,
                       double bx0, double by0, double cw_pt, double ch_pt);
/* The discrete fill over NUMERIC codes, shared by heatmap(discrete=TRUE) and
 * matrix(discrete=TRUE): the distinct finite values of v[0..n) become the
 * levels, sorted ascending and labelled as fmt_num prints them, and every
 * cell of v is rewritten to its level index (NaN stays NaN). Capped at
 * HM_MAXLEV; `what` heads the messages. Returns the level count or -1. */
int cp_numeric_levels(double *v, long n, char ***labels, double **values,
                      const char *what, char *err);
/* Rewrite a second array of the same codes onto an existing level set (a
 * matrix re-read for another window); a value outside it is an error. */
int cp_levels_apply(double *v, long n, const double *values, int nlev,
                    const char *what, char *err);
/* The palette a discrete fill paints with: the hue wheel, overridden by
 * scale_*_manual(values=) / scale_*_brewer(); pal holds nlev. */
int cp_discrete_palette(const PlotSpec *spec, char *const *levels, int nlev,
                        Col *pal, char *err);
/* The labels a key prints for `levels`: the levels themselves, renamed by
 * scale_*_manual(labels=) where it names them. A new array, or NULL with
 * err set (a label for a level the data does not hold). */
char **cp_key_labels(const PlotSpec *spec, char *const *levels, int nlev, char *err);

/* ---------- cluster.c: hclust ward.D2, R-compatible ---------- */
typedef struct {
    int n;
    int (*merge)[2];               /* R convention: <0 leaf (1-based), >0 step */
    double *height;                /* ascending */
    int *order;                    /* 0-based leaf indices, display order */
} HClust;
HClust *hclust_ward(const double *x, int n, int p, char *err);

/* ---------- heatmap.c: matrix mode (wheatmap port) ---------- */
/* A DISCRETE matrix keeps the same `v` array: each cell holds its LEVEL INDEX
 * as a double (NaN = NA), so clustering, rasterising and cell addressing stay
 * one code path and only the value -> colour step forks. `levels` are the
 * category labels in level order (lexical for text cells, numeric for codes),
 * and the indices are global across every heatmap in the figure -- they are
 * remapped onto a unified level set once all matrices are loaded, so one
 * discrete key describes the whole figure. */
typedef struct {
    int nr, nc;
    double *v;                     /* row-major, NaN = NA; discrete: level index */
    char **rn, **cn;               /* may be NULL */
    int discrete;                  /* cells are categories, not numbers */
    int nlev; char **levels;       /* discrete: level labels, level order */
} Matrix;
#define HM_MAXLEV 64               /* discrete fill: level cap (= the manual_cols cap) */
int render_heatmap(const PlotSpec *spec, const char *out,
                   double w_pt, double h_pt, char *err);
/* chord.c: circlize-style chord diagram (chord() mode) */
int render_chord(const PlotSpec *spec, const char *data, const char *out,
                 double w_pt, double h_pt, char *err);

/* ---------- tree.c: Newick trees, ggtree-style ---------- */
int render_tree(const PlotSpec *spec, const char *out,
                double w_pt, double h_pt, char *err);

#endif
