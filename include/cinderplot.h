/* cinderplot.h — shared types for the Cinderplot proof-of-concept plotter. */
#ifndef CINDERPLOT_H
#define CINDERPLOT_H

#define CINDERPLOT_VERSION "0.2.1"

#include <cairo.h>

typedef struct { double r, g, b; } Col;

/* ---------- theme: ggplot2 theme_gray, base_size 11 (units: PDF pt) ---- */
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
#define FONT_FAMILY  "Helvetica"

static const Col C_PANEL  = {0.922, 0.922, 0.922};   /* grey92 */
static const Col C_WHITE  = {1, 1, 1};
static const Col C_AXTXT  = {0.302, 0.302, 0.302};   /* grey30 */
static const Col C_TICK   = {0.2, 0.2, 0.2};         /* grey20 */
static const Col C_BLACK  = {0, 0, 0};
static const Col C_KEYBG  = {0.949, 0.949, 0.949};   /* grey95 */
static const Col C_STRIP  = {0.851, 0.851, 0.851};   /* grey85 */
static const Col C_STRIPTXT = {0.102, 0.102, 0.102}; /* grey10 */

/* ---------- breaks.c: extended Wilkinson labeling ---------- */
int extended_breaks(double dmin, double dmax, int m, double *out, int max_out);
void fmt_num(double v, char *buf);
int axis_decimals(const double *br, int n);
void fmt_break(double v, int decimals, char *buf);
/* majors for a log10 scale, positions in TRANSFORMED (log10) space,
 * labels in data space; returns count */
int log10_breaks(double tlo, double thi, double *tmaj, char **labs, int max_out);

/* ---------- palette.c: scale_colour_hue for n levels ---------- */
void hue_palette(int n, Col *out);

/* continuous fill scales */
typedef enum { FILL_VIRIDIS, FILL_JET, FILL_BWR, FILL_GRADIENT, FILL_GRADIENT2 } FillKind;
typedef struct {
    FillKind kind;
    Col low, mid, high;      /* gradient / gradient2 */
    double midpoint;         /* gradient2, default 0 */
} FillScale;
Col fill_map(const FillScale *fs, double t01);   /* t in [0,1] */
/* map v in [dmin,dmax] -> colour, honouring gradient2's midpoint */
Col fill_map_value(const FillScale *fs, double v, double dmin, double dmax);
int parse_color(const char *s, Col *out);        /* names + #RRGGBB; 0 = ok */
static const Col C_NA = {0.753, 0.753, 0.753};   /* #C0C0C0, wheatmap na.color */

/* ---------- csv.c: data frame ---------- */
typedef enum { COL_NUM, COL_STR } ColType;
typedef struct {
    char *name;
    ColType type;
    double *num;    /* COL_NUM: values, NaN for NA/empty */
    char **str;     /* COL_STR */
} Column;
typedef struct { int nrow, ncol; Column *cols; } DataFrame;

DataFrame *df_read_csv(const char *path, char *err);   /* "-" = stdin */
const Column *df_col(const DataFrame *df, const char *name);

typedef struct { int nlev; char **levels; int *idx; } Factor; /* idx[row] or -1 */
Factor *factor_make(const DataFrame *df, const Column *c);

/* ---------- gtable.c: the layout engine ---------- */
typedef enum { U_PT, U_NULL } UKind;
typedef struct { UKind k; double v; } Unit;
static inline Unit upt(double v)   { Unit u = {U_PT, v};   return u; }
static inline Unit unull(double w) { Unit u = {U_NULL, w}; return u; }

typedef enum { G_RECT, G_LINE, G_POLYLINE, G_TEXT, G_POINTS, G_TABLE,
               G_AXIS_X, G_AXIS_Y, G_IMAGE } GType;
typedef enum { V_TOP, V_BOTTOM, V_INKCENTER } VAlign;

typedef struct GTable GTable;
typedef struct {
    GType type;
    int r0, c0, r1, c1;                        /* cell span, inclusive */
    int clip;
    Col col;
    double x0, y0, x1, y1, lw;                 /* line / sub-rect, npc */
    int sub;                                   /* G_RECT: use x0..y1 sub-rect */
    const char *str; double size, tx, ty, hj;  /* text, npc anchor */
    VAlign va; int rot90;
    int n;                                     /* points / axis breaks */
    const double *px, *py; const Col *pcol; double radius;
    char **labels;                             /* axis tick labels */
    GTable *child;
    unsigned char *img; int img_w, img_h;      /* G_IMAGE: ARGB32 buffer */
} Grob;

#define GT_MAXDIM 32
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

/* ---------- dsl.c: verbatim ggplot subset ---------- */
typedef struct { char *col; int is_factor; char *expr; } AesEntry; /* col NULL = unset */

typedef enum { GEOM_POINT, GEOM_LINE, GEOM_COL, GEOM_HISTOGRAM } GeomType;
typedef struct { GeomType type; int bins; } Layer;
#define MAX_LAYERS 8

/* matrix (wheatmap) mode: anchor-placed objects */
typedef enum { PL_FULL, PL_TOP_OF, PL_BENEATH, PL_RIGHT_OF, PL_LEFT_OF } PlaceKind;
typedef struct {
    PlaceKind kind;
    char *anchor;                  /* NULL = previous object */
    double pad, width, height;     /* npc; width/height < 0 = auto */
} HPlace;
typedef enum { HM_HEATMAP, HM_ANNOTATION, HM_LEGEND, HM_DENDROGRAM } HMType;
typedef enum { CL_NONE, CL_ROWS, CL_COLS, CL_BOTH } ClusterMode;
typedef enum { SIDE_NONE, SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM } Side;
typedef struct {
    HMType type;
    char *name;                    /* auto-assigned if absent */
    char *data;                    /* heatmap/annotation: csv path */
    char *title;                   /* legend title (NULL = default/none) */
    ClusterMode cluster;           /* heatmap only */
    Side rownames, colnames;       /* heatmap label sides (SIDE_NONE = off) */
    HPlace place;
} HMObj;
#define MAX_HMOBJS 16

typedef struct {
    char *data_path;
    AesEntry x, y, colour;          /* colour also accepts fill= */
    Layer layers[MAX_LAYERS];
    int nlayers;
    int log_x, log_y;
    char *facet_var;
    char *lab_title, *lab_x, *lab_y, *lab_colour, *lab_fill;
    /* matrix mode */
    HMObj hobjs[MAX_HMOBJS];
    int nhobjs;
    FillScale fill;
} PlotSpec;

int dsl_parse(const char *src, PlotSpec *spec, char *err);  /* 0 = ok */

/* ---------- render.c ---------- */
int render_plot(const PlotSpec *spec, const DataFrame *df, const char *out,
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

#endif
