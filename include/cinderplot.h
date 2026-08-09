/* cinderplot.h — shared types for the Cinderplot proof-of-concept plotter. */
#ifndef CINDERPLOT_H
#define CINDERPLOT_H

#define CINDERPLOT_VERSION "0.6.0"

/* Size of the caller-supplied error buffer passed to every *_read / render /
 * dsl_parse entry point (see main.c: char err[CP_ERRLEN]). All error
 * formatting must be bounded to this length. Sized to hold the longest
 * message — the DSL "not implemented; supported: ..." list is ~730 bytes. */
#define CP_ERRLEN 1024

#include <cairo.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
#define SZ_BASE      11.0
#define SZ_AXIS_TEXT (0.8 * SZ_BASE)
#define SZ_TITLE     (1.2 * SZ_BASE)
#define TICK_LEN     (HALF_LINE / 2)
#define TXT_GAP      (0.8 * HALF_LINE / 2)
#define KEY_SIZE     17.3
#define PT_RADIUS    2.15
#define PANEL_SPACE  HALF_LINE
#define STRIP_PAD    (0.8 * HALF_LINE)
#define FONT_FAMILY  "Arial"

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
/* majors for a log10 scale, positions in TRANSFORMED (log10) space,
 * labels in data space; returns count */
int log10_breaks(double tlo, double thi, double *tmaj, char **labs, int max_out);

/* ---------- palette.c: scale_colour_hue for n levels ---------- */
void hue_palette(int n, Col *out);

/* continuous fill scales */
typedef enum { FILL_VIRIDIS, FILL_JET, FILL_BWR, FILL_GRADIENT, FILL_GRADIENT2,
               FILL_PARULA, FILL_TURBO, FILL_COOLWARM, FILL_MAGMA, FILL_INFERNO,
               FILL_PLASMA, FILL_CIVIDIS, FILL_ROCKET, FILL_MAKO } FillKind;
typedef struct {
    FillKind kind;
    Col low, mid, high;      /* gradient / gradient2 */
    double midpoint;         /* gradient2, default 0 */
    double lim_lo, lim_hi;   /* limits: domain + out-of-range squish */
    int has_limits;
} FillScale;
Col fill_map(const FillScale *fs, double t01);   /* t in [0,1] */
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

typedef enum { G_RECT, G_LINE, G_POLYLINE, G_TEXT, G_POINTS, G_TABLE,
               G_AXIS_X, G_AXIS_Y, G_IMAGE, G_IDEOGRAM } GType;
typedef enum { V_TOP, V_BOTTOM, V_INKCENTER } VAlign;

typedef struct GTable GTable;
typedef struct {
    GType type;
    int r0, c0, r1, c1;                        /* cell span, inclusive */
    int clip;
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
    int raster;                                /* G_POINTS: rasterize into an embedded image */
    int dash;                                  /* G_LINE/G_POLYLINE: 0 solid, 1 dashed, 2 dotted */
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
void cp_set_dpi(double dpi);                        /* PNG raster resolution (default 96) */
/* Emit the finished surface: write_to_png for image surfaces, surface_finish
 * for vector ones. Returns the resulting cairo status. */
cairo_status_t cp_surface_emit(cairo_surface_t *surf, const char *out);

/* ---------- dsl.c: verbatim ggplot subset ---------- */
/* col NULL = unset; levels (from factor(col, levels=c(...))) imposes the
 * discrete order instead of factor_make()'s sort */
typedef struct {
    char *col; int is_factor; char *expr;
    char **levels; int nlevels;
} AesEntry;

typedef enum { GEOM_POINT, GEOM_JITTER, GEOM_LINE, GEOM_COL, GEOM_HISTOGRAM, GEOM_BOXPLOT, GEOM_BAR,
               GEOM_SEGMENT, GEOM_RECT, GEOM_DENSITY, GEOM_TILE,
               GEOM_HLINE, GEOM_VLINE, GEOM_ABLINE, GEOM_TEXT, GEOM_LABEL } GeomType;
typedef struct {
    GeomType type;
    int bins;
    char *data;          /* per-layer data file (NULL = inherit) */
    char *ycol;          /* per-layer y column override (NULL = inherit) */
    Col color; int has_color;   /* constant layer colour override */
    double bw, adjust;          /* geom_density: bandwidth (0 = nrd0) x adjust */
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
    Side rownames, colnames;       /* heatmap label sides (SIDE_NONE = off) */
    int label_data;                /* annotation: label value runs in situ + bezier leaders */
    double aspect;                 /* heatmap: drawn width/height ratio (0 = free) */
    int box;                       /* heatmap/annotation: frame the cells (0 = none) */
    Col box_col;                   /* box= colour, when box is set */
    HPlace place;
} HMObj;
#define MAX_HMOBJS 16

/* track (locus-browser) mode: stacked tracks over one genomic region */
typedef enum { TRK_COVERAGE, TRK_INTERVAL, TRK_GENES, TRK_ARCS,
               TRK_MATRIX, TRK_CYTOBAND } TrackType;
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
} TrackObj;
#define MAX_TRACKS 12

typedef struct {
    char *data_path;
    AesEntry x, y, colour;          /* colour also accepts fill= */
    AesEntry xend, yend;            /* geom_segment endpoints */
    AesEntry label;                 /* geom_text/geom_label label column */
    AesEntry size;                  /* geom_point size: numeric -> point area */
    AesEntry chrom;                 /* genome scale: chromosome column */
    int coord_flip;                 /* coord_flip(): swap the x and y axes */
    char *genome_seqinfo;           /* scale_x_genome: seqinfo TSV path */
    char *ideogram_path;            /* ideogram(): cytoband TSV path */
    Layer layers[MAX_LAYERS];
    int nlayers;
    int log_x, log_y;
    double xlim_lo, xlim_hi, ylim_lo, ylim_hi;   /* user axis limits (data space) */
    int has_xlim, has_ylim;                       /* xlim()/ylim() or scale_*_log10(limits=) */
    int x_pct, y_pct;                             /* scale_*_continuous(labels=percent) */
    ThemeType theme;                              /* theme_*(); THEME_GRAY = 0 = default */
    int no_legend;                  /* guides(colour="none"|fill="none") or --no-legend */
    char *facet_var;
    char **facet_levels; int n_facet_levels;      /* facet_wrap(~v, levels=c(...)) */
    int free_x, free_y;                           /* facet_wrap(scales=): per-panel ranges */
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
    /* scale_colour/fill_manual(values=): discrete palette override */
    Col manual_cols[16]; char *manual_names[16];  /* names NULL = positional */
    int n_manual, has_manual;
    /* matrix mode */
    HMObj hobjs[MAX_HMOBJS];
    int nhobjs;
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

/* ---------- cluster.c: hclust ward.D2, R-compatible ---------- */
typedef struct {
    int n;
    int (*merge)[2];               /* R convention: <0 leaf (1-based), >0 step */
    double *height;                /* ascending */
    int *order;                    /* 0-based leaf indices, display order */
} HClust;
HClust *hclust_ward(const double *x, int n, int p, char *err);

/* ---------- heatmap.c: matrix mode (wheatmap port) ---------- */
typedef struct {
    int nr, nc;
    double *v;                     /* row-major, NaN = NA */
    char **rn, **cn;               /* may be NULL */
} Matrix;
int render_heatmap(const PlotSpec *spec, const char *out,
                   double w_pt, double h_pt, char *err);

/* ---------- tree.c: Newick trees, ggtree-style ---------- */
int render_tree(const PlotSpec *spec, const char *out,
                double w_pt, double h_pt, char *err);

#endif
