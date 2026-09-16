---
name: cinderplot
description: Use cinderplot to render a static publication-quality figure — scatter/line/bar/box/histogram/density/tile/errorbar, a clustered heatmap, a genome locus-track view, a Newick tree, or a chord diagram — to PDF, SVG or PNG from a CSV/TSV, without R or Python. Also use when the user mentions cinderplot, asks to port a ggplot2 call to it, or hits a cinderplot error. Covers the invocation, the five modes, the supported grammar, and the behavioural traps.
---

# cinderplot — ggplot2-style figures from a small C binary

`cinderplot` turns a CSV/TSV into a publication-ready PDF using Cairo. One
binary, no language runtime; the only link-time dependencies are Cairo and zlib.
Because the whole figure is a single command-line argument, it composes with a
shell pipeline, a Makefile rule or a tool call without an intermediate script.

Docs and gallery: <https://zhou-lab.github.io/cinderplot/>
Source: <https://github.com/zhou-lab/cinderplot>

## Install and invoke

```sh
conda install -c zhou-lab -c conda-forge cinderplot   # or: make && make install

cinderplot '<expression>' out.pdf [--size WxH] [--dpi N]
cat data.tsv | cinderplot 'heatmap(cluster=both)' out.png    # data on stdin
cinderplot data.csv -x hp -y mpg -c cyl -f gear out.pdf      # shortcut flags
```

- **The output file is required** — a trailing filename or `-o FILE`. The format
  comes from the extension (`.pdf`, `.svg`, `.png`; anything else is an error,
  and so is writing over the input file); `--dpi` affects PNG only.
- Quote the whole expression in **single** quotes; string literals inside it use
  double quotes. Newlines inside the expression are fine. A column name with a
  space, hyphen or non-ASCII letters goes in backticks (`` aes(`my col`, y) ``,
  R-style); a data path with a space goes in double quotes as the leading token.
- `--size WxH` in inches. A partial `9x` or `x7` auto-fits the other axis.
  Omit it entirely and every mode fits the canvas to its own content.
- `--font FAMILY` sets the figure font in every mode (default Arial); use the
  family name `fc-list` reports. A family the backend cannot find warns on
  stderr and falls back rather than failing the render.
- `--font-size PT` sets the base label size in points for every mode (1..100,
  default 11 — ggplot2's `base_size`). It scales all text together, so text
  size and panel size stop being the same knob (shrinking `--size` was the only
  way before). `theme_*(base_size=PT)` is ggplot2's own spelling and, when
  given, overrides the flag — exactly as `theme_*(base_line_size=)` overrides
  the chrome line-width default. In grammar/heatmap/tree/chord modes the size
  hierarchy (axis title 1.2x, axis text 0.8x of the base) scales from it; **in
  track mode every label is one size** (see below).
- `--editable-svg` (with an `.svg` output) writes every label as its own
  `<text>` element with a single x/y — svglite-style, retypable in Inkscape/
  Illustrator, never a per-glyph dx/dy list. Superscript axis labels become a
  base + raised-exponent pair; rotated labels carry one `matrix()` transform.
  Set `CINDERPLOT_EDITABLE_SVG=1` in the environment to make it your personal
  default; `--outline-svg` overrides the env var for one render. The tool
  default keeps Cairo's glyph outlines, which render identically without the
  font installed — the trade is portability vs editability. A rasterized
  heatmap body in SVG carries `image-rendering:pixelated` so viewers keep
  its cell edges crisp (cairo drops the nearest-filter hint otherwise);
  smooth-scaled raster point layers are deliberately left interpolated.
- `--dump-spec` prints the desugared expression without rendering.
- The delimiter is **sniffed from the first line** — a tab anywhere means TSV,
  otherwise CSV. The extension is ignored, except that `.gz` (gzip or bgzip) is
  decompressed.
- Rows dropped for NA, or for non-positive values on a log axis, are reported on
  stderr, ggplot2-style.
- **`data="path"` overrides the leading data source for one object**, so a
  figure can combine several files. It is a keyword, never positional
  (`heatmap("f.tsv", ...)` is an error). Accepted on `heatmap()`, on every track
  verb, and on `geom_segment()`/`geom_rect()`; other geoms share the leading
  source. So a grid of panels is `f1.tsv + heatmap(name="a") +
  heatmap(data="f2.tsv", right_of("a"), name="b")`. A `geom_rect(data=)` file
  holding all four corner columns (x/xend + y/yend) draws one rect per row,
  with a mapped colour/fill read from that file through the same scale as the
  main data; without y/yend it is a region-highlight band spanning the panel
  height.

## Five modes, chosen by which verbs appear

Mixing them is an error. The distinction is the data model, not the appearance.

### 1. Grammar — `aes()` + `geom_*()`

```sh
cinderplot 'sweep.csv + aes(coverage, mae, colour=model) + geom_line() + geom_point()
  + facet_wrap(~panel, levels=c("continuous","binary")) + scale_x_log10()
  + scale_colour_manual(values=c("#2a78d6","#eb6834"))
  + labs(title="MAE vs coverage", x="CpGs per cell", y="MAE") + theme_bw()' fig.pdf
```

R's own spelling is accepted too, so a call written from ggplot2 memory runs:

```sh
cinderplot 'ggplot("mtcars.csv", aes(hp, mpg, colour=factor(cyl))) + geom_point()' f.pdf
```

`data=` and `mapping=` keywords work, and a trailing `ggsave(...)` is accepted
and ignored (the output path is a command-line argument here).

- **aes**: `x`, `y` positionally, then named — `colour`/`color`/`fill`, `size`,
  `shape`, `group`, `label`, `xend`/`xmax`, `ymin`, `yend`/`ymax`, `chrom`. `shape=` maps a
  **discrete** column to point glyphs (circle, triangle, square, diamond,
  triangle-down, plus) with its own legend, so a scatter can carry two factors
  at once — colour for one, shape for the other. Six levels maximum, as in
  ggplot2: past that the glyphs stop being tellable apart, so it errors rather
  than inventing a seventh. Needs a point layer. A value is a bare column name
  or `factor(col[, levels=c(...)])`.
- **`aes(group=col)`** partitions the rows into series for `geom_line()` and
  `geom_smooth()` WITHOUT touching colour or the legend, as in ggplot2 — five
  reconstructions in one colour, each its own line or loess fit, with no
  palette hack. With a discrete `colour=` too, a series is one (colour level,
  group level) pair. The column must be discrete (`factor(col)` for a numeric
  id); it needs a line or smooth layer, and errors beside the stat geoms
  (boxplot, bar, col, histogram, density, tile), which group by colour only.
- **geoms**: `point jitter line smooth col histogram boxplot bar segment rect
  density tile raster hline vline abline text label errorbar linerange`, plus
  `text_repel`/`label_repel`.
  Up to 8 layers.
- `geom_errorbar([width=])` / `geom_linerange()` draw `aes(ymin=, ymax=)`
  intervals — the mean-and-CI bar chart is `aes(x=factor(g), y=m, ymin=lo,
  ymax=hi) + geom_col() + geom_errorbar(width=0.4)`. `width=` is the cap in
  x-axis units (default 0.9 of the x resolution, as ggplot2). Both follow
  `coord_flip()`, log axes and facets; a bound that is not positive on a log
  axis drops that row with a stderr note.
- **layer options**: `size=`, `alpha=`, `linetype="solid"|"dashed"|"dotted"`,
  `linewidth=` (`size=` is its pre-3.4 alias) on every stroke geom — line,
  smooth, segment, hline/vline/abline, errorbar/linerange — in ggplot
  linewidth units (0.5 is `geom_line()`'s default, 1 is `geom_smooth()`'s;
  must be > 0),
  `bins=` (histogram; ggplot2's default binning — right-closed bins whose
  edges sit on the (k+½)·width lattice — so counts match `ggplot_build()`),
  `bw=`/`adjust=` (density, R's `bw.nrd0` scaled by `adjust`),
  `nudge_x=`/`nudge_y=` (text),
  `geom_point(raster=TRUE)` — see trap 9 — and
  `geom_boxplot(outlier.shape=NA)` to hide the outlier marks when a jitter
  layer already draws those points.
- `geom_tile(colour="white"[, linewidth=])` strokes each cell's border over
  the mapped fill, as in ggplot2 — the thin white separators of a manuscript
  heatmap; linewidth defaults to 0.1. Pair with
  `scale_x_discrete(expand=c(0,0)) + scale_y_discrete(expand=c(0,0))` — or
  the shorthand `coord_cartesian(expand=FALSE)` — to make the panel frame
  hug the tile grid. Discrete expansion measures from the TILE EDGE
  (category ± 0.5), so `c(0,0)` leaves the outer cells full-sized and
  flush, never cut in half; `expand=c(mult, add)` on any scale overrides
  the defaults (continuous 5%%, discrete 0.6 from the centre) generally.
- `geom_boxplot()` renders `fill=` and `colour=` differently, as ggplot2
  does: `fill=` colours the box body under dark chrome (outline, whiskers,
  median, outliers); `colour=` colours the chrome over a white body.
- `coord_polar([start=])` draws a radar ("spider") chart: the discrete x's
  categories become labelled spokes, y the radius, each colour series a
  CLOSED polyline, with dashed rings at the y breaks (pin them with
  `scale_y_continuous(breaks=c(0.5, 1))` + `ylim(0,1)`). geom_line()/
  geom_point() only; bars under polar (pie/rose) error — general polar
  coordinates are on the roadmap. `colour=` implies the series grouping,
  as in ggplot2; `aes(group=)` partitions further without a legend.
- `annotate("text", x=, y=, label=[, colour=][, size=][, hjust=][, vjust=])`
  places one literal mark, ggplot2's verb — also `"segment"` (x/y/xend/yend)
  and `"rect"` (xmin/xmax/ymin/ymax; colour= sets the fill, default grey85).
  `hjust`/`vjust` anchor the text as in ggplot (0.5 default = centred on the
  point; `hjust=0, vjust=0` starts the text at x and rests it above y — the
  reference-line-label placement); vjust takes 0/0.5/1. `angle=` rotates the
  text (degrees CCW), with hjust applied in the rotated frame — the standing
  per-bar label; vjust with angle errors. `geom_text(angle=, hjust=)` takes
  the same; `geom_label()` and the repel geoms refuse rotation (the box does
  not rotate; repel measures unrotated extents). Tilted axis TICK labels are
  a different verb: `scale_x_discrete(angle=45)`, and crowded discrete labels
  already auto-rotate. Marks take
  part in scale training, work on log axes and under coord_flip, repeat the
  verb for several, and draw OVER the geoms (a shading rect behind points is
  not expressible yet). The usual use: labelling a geom_hline() reference.
- **`geom_smooth(se=FALSE, span=[, linewidth=])`** fits a LOESS (local quadratic,
tricube weights) per series — colour level, and `aes(group=)` when given — per
panel: the points-plus-trend idiom for a noisy
signal. `span=` is the fraction of the data in each local fit, default `0.75` as
in ggplot2. **Treat the default as a starting point, not an answer**: over a
wide window it will flatten real structure into one broad arc. On a genomic
trace `0.1`–`0.15` keeps the per-CpG detail while still showing the trend. **`se=FALSE` is required**:
ggplot2 defaults it to `TRUE`, the confidence ribbon is not implemented, and
drawing the line without saying so would show less uncertainty than you asked
for.

**`geom_jitter(width=, height=, size=, alpha=, seed=)`** is `geom_point()`
  with a random offset, layerable over `geom_boxplot()` — the usual
  box-plus-observations figure. On a discrete axis plain points stack on the
  category centre, so 400 look like 40. `width=`/`height=` are half-ranges in
  data units (so on a discrete axis `width=0.2` spans a fifth of the slot each
  way; default `0.4`). **Two deliberate deviations from ggplot2**: vertical
  jitter defaults to **0**, because y is usually a measured value and moving it
  invents data — pass `height=` for ggplot's behaviour; and the offset is
  **deterministic**, keyed off the row, so a figure regenerates identically.
  `seed=` picks a different arrangement.
- **scales**: `scale_x_log10()` `scale_y_log10()` `scale_x_log2()` `scale_y_log2()`
  (each takes `limits=c(lo,hi)` in data space. Reach for log2 when the variable
  was *sampled* at powers of two — a coverage or subsampling ladder: log10 spaces
  those points correctly but its 10^k ticks land between the rungs you measured.)
  `scale_*_continuous(labels=percent,
  labels=c("..."), limits=, breaks=c(...))` — `labels=c(...)` is explicit tick
  text paired 1:1 with `breaks=c(...)` (mismatched lengths error), so a
  category label row can BE a numeric axis. `xlim()` `ylim()`
  `scale_*_manual(values=c(...))`
  `scale_(fill|colour)_distiller(palette="YlOrBr"[, direction=][, limits=])` —
  any named ColorBrewer sequential/diverging ramp (YlOrBr, YlGnBu, Blues,
  Greys, RdBu, Spectral, ...), stops taken verbatim from RColorBrewer.
  `direction` follows ggplot2: the default -1 puts the LIGHT end at high
  values; `direction=1` reads the palette as printed (light low, dark high),
  which is usually what a light-to-dark manuscript figure wants.
  `scale_(fill|colour)_brewer(palette="Set2")` — a qualitative Brewer set for
  a discrete aesthetic (Set1/2/3, Dark2, Paired, ...); errors when the factor
  has more levels than the set has colours. Each errors if handed the other
  kind of palette, because a sequential palette's n-class subsets are not its
  first n colours.
  `scale_(fill|colour)_identity()` — the mapped column's values ARE the
  colours (names or #RRGGBB); no legend, and a non-colour value errors. The
  stopgap when one figure needs several colour meanings across facets.
  `scale_(fill|colour)_*()` continuous palettes: `viridis magma inferno plasma
  cividis rocket mako` (perceptually uniform; `cividis` is also colour-blind and
  greyscale safe), `parula` (the lab default for methylation beta), `turbo` (a
  rainbow without `jet`'s false banding), `coolwarm`/`bwr` (diverging —
  `coolwarm`'s grey midpoint survives print where `bwr`'s white does not), `jet`
  (legacy, banding is an artefact), plus `gradient`/`gradient2` for your own
  endpoints,
  `scale_x_genome("seqinfo.tsv.gz")`, `ideogram("cytoband.tsv.gz")`.
- **also**: `facet_wrap(~v[, levels=][, scales=][, ncol=][, nrow=])`,
  `facet_grid(row ~ col[, scales=])`, `coord_flip()`, `guides(colour="none")`
  (per aesthetic: `guides(size="none")` keeps the colour key; `--no-legend`
  and `theme(legend.position="none")` drop them all), `labs()`/`xlab()`/`ylab()`/`ggtitle()` — `labs()` also
  takes `subtitle=` (under the title) and `caption=` (bottom right) — and
  `theme_{gray,bw,minimal,classic,void,linedraw,light,dark,few}()`. Every
  theme selector takes `base_line_size=` (ggplot's own argument): the width
  of the chrome lines — border, axis line, grid, ticks, heatmap/track
  frames — where 0.5 is the ggplot2 default and 0.25 gives half-weight
  axes; data strokes are untouched. `CINDERPLOT_BASE_LINE_SIZE=0.25` in the
  environment makes it a personal default; the spec argument overrides it.
  It also takes `base_size=PT` (ggplot's base font size, default 11): the same
  knob as `--font-size`, and the spec value overrides the flag. The presets
  themselves are grammar-mode: in heatmap and track modes only
  `theme_*(base_line_size=)` and `base_size=` apply (a bare preset errors),
  chord mode refuses `theme_*()` altogether, and tree mode takes only
  `base_size=` (no panel chrome to weight).
- `annotation("meta.tsv"[, column="ighv"])` draws a categorical metadata band
  under the panel — one chip per x category (bar-width, so chips align under
  stacked `geom_col()` bars), with its own palette and its own legend block,
  so cohort metadata stops riding the fill scale and polluting its legend.
  The file's first column names the x categories; `column=` picks the value
  column (default: the last). Needs a discrete x; a category with no row
  warns and draws in the missing-value grey; repeat the verb to stack
  several bands. Placements (`left_of()` etc.) are heatmap-mode and error
  here.

`ncol=`/`nrow=` fix the grid shape; give both and a grid too small for the
panels is an error. Worth reaching for whenever the panels cross two factors —
the automatic shape wraps 8 panels 3-per-row, which puts the pairs you meant to
compare in different rows, and `levels=` cannot fix that because it controls
order and not wrap width.

`facet_wrap(scales=)` takes `fixed` (default), `free_x`, `free_y`, `free`,
or `free_colour` — the last frees the COLOUR scale per facet (no ggplot2
equivalent; patchwork territory): each facet builds its own discrete palette
from its own rows and gets its own legend block titled by the facet name,
placed directly under its panel when the facets sit in one row (multi-row
grids stack the blocks in the right margin). Needs a discrete colour/fill;
named `scale_*_manual(values=)` maps per level, positional lists and brewer
sets apply per facet. The use: one figure, same points, three colour
meanings. `guides(colour=guide_legend(ncol=N))` folds a long discrete legend
over N columns, column-major; `nrow=N` caps the rows instead;
`reverse=TRUE` flips the key order without touching the factor (the stacked
`geom_col()` legend read top-down), and under
free_colour each block then derives its own column count (a 4/6/10-level trio
with nrow=6 folds only the 10). Without either, blocks now AUTO-fold to as
many columns as fit their panel's width. `labs(colour="")` drops the block
titles when they would duplicate the strips, and
`theme(legend.position="inside", legend.position.inside=c(x, y))` anchors
each block inside its own panel at those npc coordinates (default lower
right) — no legend row at all, the manuscript ladder-panel look. The same
theme() pair places a single unfaceted legend inside its panel; every other
theme() key still errors (presets only). A
freed axis is trained per panel; the axis you did not free stays shared. On a
discrete axis a panel drops the categories it has no rows for and renumbers the
rest, so column 3 means a different thing in each panel — that is ggplot2's
`drop = TRUE`. Histograms re-bin per panel under `free_x`, so bin widths differ
between panels. `scale_x_genome()` cannot be freed (use `regions()` in track
mode for several windows).

`facet_grid(rowvar ~ colvar)` is the two-way grid: the row variable's levels
become panel rows, the column variable's become panel columns, and **every
(row, column) combination gets a panel, including the ones the data never
uses** — which is the reason to ask for a grid rather than a wrap, since the
gap is the finding. Column strips run along the top, row strips down the
right (rotated, ggplot2-style), and the axes sit only on the outer edges: the
y axis on the left column, the x axis under the bottom row. Either side may
be `.` — `facet_grid(. ~ col)` is one row of panels, `facet_grid(row ~ .)`
one column. `scales=` takes `fixed` (default), `free_x`, `free_y` and `free`
with ggplot2's facet_grid meaning: a freed x is shared down a COLUMN and a
freed y across a ROW, so the axes stay on the edges. The shape is the two
level counts, so `ncol=`/`nrow=` are refused; so are `levels=` (use the
factor order) and `scales="free_colour"` (a facet_wrap extension).

Crowded discrete tick labels **rotate automatically** (45°, then 90°) when the
measured labels do not fit the panel. Override with `scale_x_discrete(angle=N)`
or `scale_y_discrete(angle=N)`, `0..90`; `angle=0` forces horizontal.

There is **no category limit** on a discrete axis. Omitting `--size` fits the
canvas to the figure (panels, categories, label lengths) rather than defaulting
to 6x4; an explicit `--size` is always honoured as given.

### 2. Heatmap — a matrix with clustering, anchor-placed objects

```sh
cinderplot 'm.tsv + heatmap(name="m", cluster=both, rownames=right)
  + dendrogram(top_of("m"))
  + annotation("groups.tsv", column="diagnosis", left_of("m"), name="dx")
  + dendrogram(left_of("dx"))
  + legend(right_of("m"))
  + scale_fill_gradient2(low="#2166ac", mid="#f7f7f7", high="#b2182b")' out.pdf
```

Input is a **wide matrix** (first column may be row names). Placements are
`top_of` / `beneath` / `left_of` / `right_of`, each taking an anchor name plus
`pad=`/`width=`/`height=` (both size keys work on any placement — the one the
placement would otherwise inherit from its anchor included). The first placed object must be a `heatmap()`.
`rownames=` takes `left|right|none` and `colnames=` takes `top|bottom|none`
(`off`/`hide` also read as `none`); **both default to none**, so a heatmap with
no `colnames=` renders that axis unlabelled. Clustering is hclust ward.D2,
matching R. `annotation()` takes `column=` to pick
which column colours the bar (default: the last). Its rows are matched to the
matrix by the first (name) column when that column is text and every name is
found; a file with no name column is taken in matrix order, and a partial match
is an error naming the first unknown key. An annotation may be anchored
by its data column name, e.g. `legend(right_of("diagnosis"))`. **One object per
slot**: a second `left_of("m")` errors and says which object holds the slot —
chain the next one off that object (`dendrogram(left_of("dx"))`), as the
example above does; legends are exempt and stack.

`labs(x=)`/`labs(y=)` name the axes; `labs(title=)` titles the figure, and
`title=` on a placed object names *that panel* — which is how a stack of
`heatmap()`s gets per-panel labels. Booleans accept `TRUE`/`FALSE` as well as
`on`/`off`.

`aspect=` couples the figure's two dimensions so the **matrix** comes out at
that ratio — `aspect=1` makes the matrix square, which for a non-square matrix
means oblong cells (it is the matrix's shape, not a cell's). It chooses the plot
dimensions, so it works with no `--size` or a partial one (`--size 6x` fixes the
width and derives the height); with a full `--size` both proportions are already
set and it errors rather than being ignored.

`box=on` frames the cells and only the cells — the row/column names stay outside
it. `box="grey40"` sets the colour and implies on. Off by default; works on
`heatmap()` and `annotation()`. (`matrix()` tracks are always framed.)

`grid=` strokes the separators *between* cells, so a row can be traced across a
near-white matrix — same spellings as `box=` (`on`/`off`, or a quoted colour
that implies on; default grey70). It drops itself with a stderr warning when
cells fall under ~4pt, where the lines would out-ink the fills.

`labels=on` prints each cell's value — the confusion-matrix / parameter-grid
staple (`geom_tile() + geom_text(aes(label=))`). One font size is fitted to the
cells, and each number switches black/white against its fill's luminance; under
4pt of text the labels drop with a warning.

`highlight("row","col"[, colour="red"][, name="m"])` draws a bounding box on one
cell, addressed by its row and column names (so clustering cannot move the box
off the intended cell); an unknown name errors. Repeat the verb for several
cells; `name=` picks the heatmap when the figure has more than one. (The track
form of the same verb boxes a `matrix()` track row — see mode 3.)

`scale_fill_*(limits=c(lo,hi))` pins the fill domain, so several figures (a
multi-page grid, say) share one ramp; out-of-range values squish to the ends
and the colourbar shows the pinned range.

`cluster=` takes `rows cols both none diagonal symmetric` (`off` = `none`). The
last two are for a **confusion matrix**, where the axes share a vocabulary and
the diagonal is the point: `cluster=both` orders the axes independently and
destroys it, so use `diagonal` (columns follow the row names, no clustering) or
`symmetric` (cluster the rows, then the columns follow them — related classes
group *and* the diagonal survives). Both need row names, and columns no row
claims are appended in input order.

**Discrete fill.** A matrix whose cells are **text** (a mutation/call matrix, a
label grid, a per-sample class assignment) is categorical: each distinct value
gets a colour, and `legend()` draws a key — one swatch and label per level —
instead of a colourbar. Nothing has to be said to switch it on; the cell types
decide. `heatmap(discrete=TRUE)` is the opt-in for a matrix of numeric **codes**
(a 0/1/2 call matrix), which is otherwise indistinguishable from measurements,
and `discrete=FALSE` pins the continuous reading. Levels sort as R's factors do:
lexically for text, numerically for codes. A matrix that is *part* text and part
numeric is ambiguous and errors, naming a column of each kind.

`scale_fill_manual(values=c("MUT"="#b2182b", "WT"="grey90"))` colours the levels
by name, and levels it does not name keep their default hue — so a two-category
highlight need not enumerate the rest. A positional
`values=c("red","blue",...)` is read level by level and must be at least as long
as the level count (a short list errors with both counts); `scale_fill_brewer(palette=)`
works the same way. `scale_fill_manual(labels=c("0"="WT", "1"="MUT"))` renames
the levels in the key only (the same table `matrix(discrete=TRUE)` reads in
track mode); a label for a level the data lacks errors, and `labels=` over a
numeric matrix errors, since a colourbar has no levels. A *continuous* `scale_fill_*()` over a categorical matrix
errors, as does a manual palette over a numeric one. The cap is 64 levels.

Two things a discrete fill does **not** do. `cluster=rows|cols|both|symmetric`
errors: ward.D2 is Euclidean and there is no distance between two categories, so
the alternatives are `cluster=none`, `cluster=diagonal` (which matches column
names to row names and computes no distance), or ordering the file yourself.
And every heatmap in one figure shares a single fill scale, so a categorical one
and a numeric one cannot be stacked — several categorical heatmaps can, and
share one key, a colour meaning the same in all of them. `labels=on` prints the
category rather than a number. Remember the leading-text-column rule: the first
column is read as row names, so a text matrix meant to be all data loses its
first column to them — give it a name column.

Grammar mode's `geom_tile()` remains the alternative when the layout wants
facets or continuous axes rather than clustering and anchor-placed objects.

### 3. Newick tree — a hierarchy you already have

```sh
cinderplot 'taxonomy.tre + geom_tree() + geom_tiplab() + geom_nodelab()' tree.pdf
```

Input is **Newick**, not a table: `((a,b)AB,(c,d)CD)root;`. `[...]` comments
(including `[&&NHX:...]`) are skipped, and a quoted name may hold the reserved
characters — `'Neuron, inhibitory'`, `'b''s cell'` for a literal quote. Branch
lengths are **all-or-nothing**: give every branch one and x is cumulative
length, or give none and x is depth (a cladogram). A mix is an error rather than
a guess, and a length on the root is ignored. `geom_tiplab()` draws the
leaf names, `geom_nodelab()` the internal ones — each sits on the branch running
into its node, and on a **cladogram** the branches widen so the labels do not
overprint (x is arbitrary there, so nothing is misrepresented). A phylogram's
lengths are the datum and are never stretched, so its labels can crowd.

`geom_tree(layout=)` takes `rectangular`
(default), `slanted` (straight parent-to-child lines, no spine) or `circular`
(a fan; the canvas becomes square and tip labels radiate, flipped on the left
half so none reads upside down). Omitting `--size` fits one readable row per
tip, or a ring that holds them all when circular.

**Join a table on node/tip name**, ggtree's `%<+%` idiom — the first text column
is the key, `colour=` names the mapped column, and a discrete column gets a key
while a numeric one gets a colourbar:

```sh
cinderplot 'tax.tre + geom_tree() + geom_tiplab()
  + geom_nodepoint(data="levels.tsv", colour=level)' out.pdf      # internal nodes
cinderplot 'tax.tre + geom_tree()
  + geom_tiplab(data="acc.tsv", colour=accuracy)' out.pdf         # colour the labels
```

`geom_tippoint()` does the same at the tips. Input is **long form**: several
rows for one name draw several marks stacked back along its branch, so a node
belonging to three categories shows three dots. Names with no row get no mark.

The key column's **type decides how it joins**, as ggtree's first column does:

- **text** → joins by node/tip *name*. The name must identify exactly one node;
  if it occurs on more than one the join errors rather than matching all of
  them, because every node called `A` matching every `A` row produces a figure
  that reads as correct and is not. A repeated name the table never mentions is
  fine.
- **numeric** → joins by node *id*, which is unique by construction and is the
  only way to address a tree whose names repeat (a taxonomy that carries the
  same name down a single-child chain).

Ids follow ape's `phylo` convention: tips are `1..Ntip` in the order they appear
reading the Newick left to right, then internal nodes from `Ntip+1` with the
root first and the rest in preorder. See them with
`geom_nodelab(label=id)` / `geom_tiplab(label=id)`, which is also how you check
them against `ape` before generating a table in R.


### 4. Genome locus browser — stacked tracks over one or more windows

```sh
cinderplot 'region("chr20:44616522-44655233")
  + cytoband("cytoband.tsv.gz", height=0.5) + genes("genes.bed.gz", height=1.5)
  + matrix("betas_long.tsv", name="b", cluster=samples, rownames=off,
           colnames=off, height=10)' locus.pdf
```

Track verbs: `coverage() interval() genes() arcs() matrix() signal() cytoband()`.
Each takes a file plus the three every track shares: `name= height= data=`. The
rest are per-verb, and an option the verb does not take is refused (the error
lists the ones it does): `coverage()` adds `color= max=`, `interval()`
`color= labels=`, `genes()` `color= transcripts=`, `arcs()` `color=`,
`cytoband()` nothing further, `matrix()` `cluster= rownames= colnames= x= bar=
background= rowgroup= rowmeta= rowcolour= rowbar= discrete=`, and `signal()`
`rowgroup= rowmeta= rowcolour= rowbar= smooth= points= colour=c(...)
ylim=c(lo, hi) linewidth= gap=`. Inputs are BED/bedGraph/BEDPE/BED12/matrix TSV, tabix
**`matrix(x=genomic)` puts each cell at its own coordinate** instead of in
probe-index space. The default (`x=index`) gives every probe an equal-width
column and draws a leader fan to its true position — right when the columns
are the point, wrong when the *locus* is: 550 CpGs in a 20 kb window become a
uniform grid that no longer lines up with the gene models above it. Genomic
space draws only the cells that exist, so a dense CpG cluster reads dense and
a gap reads as a gap; there is no fan and no per-probe label band, because
there are no columns to lead to or label. A CpG is 2 bp in a window tens of kb
wide, far under a pixel, so a cell is widened to stay visible — `bar=N` sets
the width in bp explicitly. `background=` colours the stretches with no probe
(default the missing-value grey); an NA cell draws nothing, so the background
shows through.

**`matrix(discrete=TRUE)` reads the cell values as category codes** — a 0/1
call matrix is indistinguishable from betas without being told — and is
`heatmap(discrete=TRUE)`'s twin: each distinct value becomes a level (sorted
numerically, keyed by value), coloured by `scale_fill_manual(values=c("0"="#4575b4",
"1"="#d73027"))` by level name (unnamed levels keep their hue) or the hue wheel,
and the cells paint the level's colour. A continuous `scale_fill_*()` over
`discrete=TRUE` errors, as does a manual palette over a numeric matrix, both
naming the other reading. **`legend()` is the track browser's one heatmap-mode
verb**: it takes no placement (it sits in the right margin, vertically centred
on the matrix band, and `right_of()` etc. error saying so), needs a `matrix()`
track (one — two matrices error), and draws the key for a discrete matrix or
the 0..1 colourbar for a continuous one. `title=` names it (default
`labs(fill=)`). `scale_fill_manual(labels=c("0"="Unmethylated", "1"="Methylated"))`
renames the levels in the key only — colours are still keyed by the level as
the file writes it, and a label for a level the data lacks errors naming it. A
discrete key ends with a swatch in the background colour whenever the matrix
has `background=` or NA cells, labelled by `legend(missing="Missing")` (default
`NA`; `missing=none` drops it):

```sh
cinderplot 'regions("windows.bed") + genes("genes.bed.gz", height=0.8)
  + matrix("calls_long.tsv", name="m", x=genomic, bar=8, background="#bdbdbd",
           rowgroup=" | ", cluster=none, discrete=TRUE, height=3.2)
  + scale_fill_manual(values=c("0"="#4575b4", "1"="#d73027"),
                      labels=c("0"="Unmethylated", "1"="Methylated"))
  + legend(missing="Missing", title="Call")' calls.pdf
```

**`rowgroup="SEP"` splits each sample name** on the separator: the part before
it is a group, the part after is that row's label. Sixty rows of
`Bladder-Epithelial | truth` are a stack of repeated prefixes; grouped, the
cell type is written once beside its run, each row keeps only what
distinguishes it, and a rule separates one run from the next. Runs are
*consecutive* rows, so the display order decides them — a scattered group
shows as several runs rather than being silently merged. A name without the
separator keeps its whole label and forms no group.

```sh
cinderplot 'region("chr1:202004435-202019137") + genes("genes.bed.gz", height=0.6)
  + matrix("betas_long.tsv", x=genomic, rowgroup=" | ", cluster=none, height=5)' locus.pdf
```

**`rowcolour="colours.tsv"` colours the annotations** (also `rowcolor=`), in one
of two shapes chosen by the header:

- with `rowgroup=`, a two-column `group colour` (or `color`) table, one line per
  group: the group name is written in that colour with a filled swatch beside
  it (band-only when `rowbar=on`, see below), so a lineage reads at a glance.
- with `rowmeta=`, a three-column `column value colour` table mapping a value in
  any annotation column to a colour, so a `cell_type` band and a `source` band
  draw from one file.

A group/value the table does not name gets the hue palette (per column, over
that column's distinct values in first-appearance order); a colour that does not
parse is an error naming the row. `rowcolour=` needs `rowgroup=` or `rowmeta=` —
without either there is nothing to colour.

**`rowmeta="samples.tsv"` reads annotations from a sample metadata sheet**
(`matrix()` and `signal()`). Its first column is the sample key (matching the
`sample` column of the long input); every other column is an annotation
(`cell_type`, `source`, …). Every sample the track draws must be in the sheet,
or it errors naming the first missing one. `rowmeta=` and `rowgroup=` are
mutually exclusive — one splits an annotation out of the name, the other reads a
sheet.

**`rowbar=` draws annotation bands** against the left edge of the panel — the
band's *extent*, the way heatmap mode's `annotation(left_of())` reads — in two
forms:

- **`rowbar=on`** (default off) with `rowgroup=` + `rowcolour=`: the single band
  that follows the split group. This is now *band-only* — it replaces the swatch
  rather than adding to it (plain `rowcolour=` without `rowbar=` still draws the
  swatch beside the name).
- **`rowbar="cell_type,source"`** with `rowmeta=`: a comma-separated, ordered
  list of metadata columns, each drawn as its own adjacent band (leftmost band =
  first name in the list). Each band spans the consecutive run of rows sharing
  that column's value; the **first** column also gives the group label written
  once per run and the run-separator rules (so a `source` band alternates
  truth/reconstruction within a `cell_type` run). A column not in the sheet
  errors naming it and listing the sheet's columns.

```sh
# single band, band-only (rowgroup path)
printf 'group\tcolour\nNeuron\t#d62728\nHepatocyte\tsteelblue\n' > colours.tsv
cinderplot 'region("chr1:202004435-202019137")
  + matrix("betas_long.tsv", x=genomic, rowgroup=" | ", rowcolour="colours.tsv",
           rowbar=on, cluster=none, height=5)' locus.pdf

# two bands from a metadata sheet
printf 'sample\tcell_type\tsource\nBreast|truth\tBreast\tGround truth\n...' > samples.tsv
printf 'column\tvalue\tcolour\ncell_type\tBreast\t#a6b727\nsource\tGround truth\t#000000\nsource\tReconstruction\t#e69f00\n' > rowcolours.tsv
cinderplot 'region("chr1:202004435-202019137")
  + matrix("calls_long.tsv", x=genomic, discrete=TRUE, rowmeta="samples.tsv",
           rowbar="cell_type,source", rowcolour="rowcolours.tsv", cluster=none, height=5)' locus.pdf
```

**Track mode renders every label at one size** — row/strip labels, the track
`name=`, the `regions()` panel titles, the kb-axis numbers and the legend title
all share the axis-text size (0.8 × the base), and `--font-size` /
`theme_*(base_size=)` scale that one size. A locus figure does not rank a panel
title above a row label the way a grammar figure ranks its axis title above axis
text; the browser's labels are peers. (Grammar, heatmap and tree modes keep
their hierarchy.) The one exception is **shrink-to-fit**: a feature label that
would not fit at the flat size — a `genes()` or `interval()` name that would run
past its lane or the panel edge, a `matrix()`/`signal()` row label taller than
its row — is shrunk toward a ~5.5pt floor until it fits, and only dropped if it
will not fit even at the floor. Lane packing reserves the full-size width, so a
shrunk name leaves a gap rather than overlapping its neighbour.

**`interval()` names each feature from its 4th BED column**, to the right of
the box — but only where the name fits before the next feature in its lane.
A CpG tick track of 380 named 2-bp features would otherwise print 380 names
over one another; by default a crowded name is shrunk toward the ~5.5pt floor to
fit the gap and only dropped if it still will not (the way `genes()` keeps names
from colliding), and the boxes stay. `labels=on` draws every name regardless
(still shrunk to the panel edge), `labels=off` draws none; a 3-column BED has no
names to draw.

**`signal()` draws continuous traces under the same axis** — the manuscript's
bottom panel: per cell type one strip, the ground truth as a black line over N
reconstructions in orange, points behind, loess-smoothed, on the genomic x of
the `matrix()` above it. It reads the long shape `matrix()` reads,
`chrom beg end value sample [series]` (`beta` heads the value column just as
well): one strip per `sample`, stacked top to bottom in file order like matrix
rows, and within a strip one line per `series` through that series' rows in
position order (no `series` column: one line per strip). `smooth=SPAN` draws
each series as its loess with that span, the fit `geom_smooth(span=)` draws;
`smooth=0` (the default) is the raw polyline. `points=on` puts the raw points
behind the lines, small and translucent. `ylim=c(lo, hi)` pins every strip to
one value range (betas want `c(0, 1)`); otherwise each strip spans its own
min..max, and a value outside `ylim=` is clipped to its strip, never rescaled.
`colour=c("Ground Truth"="#000000", "Reconstruction 1"="#e69f00", ...)` names a
colour per series, the list `scale_colour_manual(values=)` takes; series it
does not name keep a hue, a name it gives that is not in the data is an error,
and a positional list must cover every series. A single `colour="grey40"`
paints every series alike. The first series — in `colour=c(...)` when it names
them, else in the file — is drawn on top, so a reference trace listed first
does not vanish under the rest. `rowgroup=`/`rowcolour=`/`rowbar=` and the
`rowmeta=` metadata bands work exactly as on `matrix()` (the group once beside
its run, a rule between runs, the coloured name and swatch or the annotation
bands); a `name=` stands rotated at the gutter's left edge, since the strips own
the label column. `height=` is a row weight like the matrix's, and
the strips share it equally; `linewidth=` (or `size=`) is in ggplot units,
default 0.5. Under `regions()` each window draws the rows inside it, and the
strip order and ranges come from the whole file so a strip means the same row
in every window. A faint baseline marks each strip's low end. `gap=PT` is the fixed blank between strips (default 2 pt), so adjacent lanes read as two lanes rather than one trace crossing a baseline; `gap=0` removes it.

```sh
cinderplot 'regions("windows.bed") + genes("genes.bed.gz", height=0.8)
  + matrix("calls_long.tsv", x=genomic, rowgroup=" | ", cluster=none, colnames=off, height=3)
  + signal("betas_long.tsv", height=2.5, smooth=0.25, points=on, ylim=c(0, 1),
           colour=c("Ground Truth"="#000000", "Reconstruction 1"="#e69f00"))' fig.pdf
```

`.gz` allowed. `region()` with no argument infers the window from `matrix()`,
and `-r chr:start-end` / `--region` on the command line supplies it from
outside the expression (a shell loop over loci). Region strings are 0-based,
half-open like BED (`chr1:100-200` covers BED bases 100..199).

**Several windows on one axis** — `regions("windows.bed")` lays each window in
its own panel with a real gap between them, sharing the sample rows, colour
scale and legend:

```sh
cinderplot 'regions("windows.bed") + genes("genes.bed.gz", height=1.4)
  + matrix("betas_long.tsv", name="b", cluster=samples, colnames=off, height=9)' multi.pdf
```

`windows.bed` is `chrom start end [name]`; the optional name titles the panel.
A header row is tolerated. Panel widths follow the matrix column count when a
matrix track is present, so a cell is about the same width in every window.
Supported inside `regions()` so far: `matrix()`, `genes()`, `interval()`,
`signal()`.

**Marking calls on a `matrix()` track** — `highlight()` again, addressed by
sample row and genomic span rather than row and column:

```sh
cinderplot 'region("chr10:131089928-131099667")
  + matrix("betas_long.tsv", name="beta", cluster=none, height=5)
  + highlight(name="beta", row="RCC-KI258T", region="chr10:131092575-131093386",
              colour="#d73027", label="F")
  + highlight(name="beta", row="RCC-KI258T", region="chr10:131096625-131097007",
              colour="#e08214", linetype="dashed")' out.pdf
```

The box spans the probe *columns* whose position falls in the span (the
heatmap's x is probe-index space, so its edges are column edges), over that
sample's row band. `linetype=` takes solid/dashed/dotted, `label=` writes a
small tag at the top-right corner in the box colour, and `name=` picks the
matrix track when there is more than one.

For many boxes — a classifier's output over a `regions()` grid — give a file
instead: `highlight("boxes.tsv", name="beta")` with columns
`row chrom beg end [colour] [label] [linetype]`, one box per line and no cap
(the inline form is capped at 64 calls). Under `regions()` each box lands in
whichever panel contains its span; a box outside every panel, or covering no
probe column, warns on stderr rather than failing the figure. An unknown
sample row is an error.

### 5. Chord diagram — circlize's chordDiagram() defaults, its own mode

```sh
cinderplot 'links.csv + chord([gap=2][, alpha=0.6])
  + scale_fill_manual(values=c("LIHC"="#1b9e77", ...))' out.pdf --size 6x6
```

Input is a long (from, to, value) table (`from=`/`to=`/`value=` name other
columns; defaults are those names, else the first three columns). Sectors
are the union of from/to names in first-appearance order, arc length
proportional to each sector's total flow, with an outer colour band and the
name outside. Ribbons connect source and target sub-arcs, width
proportional to value, coloured by SOURCE with `alpha=` translucency, drawn
largest-first so small flows stay visible; self-links work. Values must be
positive; sector colours default to the hue palette, with named
`scale_fill_manual(values=)` overriding per sector. `order=c(...)` gives an
explicit complete sector order (circlize's order=), and `bipartite=TRUE`
puts every from-sector first, then every to-sector, with a 10-degree gap
between the two groups — the readable layout for source-to-target flows;
a name on both sides (a self-link) errors there. Directional arrows and
circlize's scaling options are not implemented.

## When unsure, try it

Every unimplemented verb, aesthetic or option fails with a message that
**enumerates the supported subset**, so a wrong guess costs one fast run and
returns a menu rather than a syntax error.

Known absent: the `geom_smooth()` confidence ribbon (`se=TRUE`),
an arbitrary `theme()` (presets only), dodged bars (stacking exists), and
statistical transformations beyond binning, density and loess.

## Traps

1. **`y` must be numeric except with `geom_tile()`.** A category-vs-value bar
   chart is `aes(x=factor(cat), y=value) + geom_col() + coord_flip()`, not
   `aes(x=value, y=cat)`.
2. **`geom_boxplot()` and `geom_bar()` need a discrete x** — wrap it:
   `aes(x=factor(g), ...)`. A text column is discrete already.
3. **`geom_col()` stacks** (ggplot's default position): duplicated x rows are
   summed, with or without a fill; with a varying discrete fill
   `aes(x=donor, y=pct, fill=class) + geom_col()` stacks the groups with the
   last factor level at the bottom.
   Stacking needs a discrete x and refuses negative values. A *continuous*
   `fill=` maps each bar through the gradient scale instead (a colourbar
   legend, as `geom_rect()` has). `geom_histogram()` still requires its fill
   constant per panel; `geom_bar()` refuses a continuous fill (it counts rows
   itself); dodging is not implemented.
4. **`geom_histogram()`/`geom_bar()`/`geom_density()` compute y themselves** —
   do not map `y`, and do not combine them with other data geoms.
5. **Factor level order is sorted, R-style.** Impose an order with `levels=`:
   `facet_wrap(~panel, levels=c("b","a"))` or `aes(colour=factor(m, levels=c("B","A")))`.
   Unlike R, `levels=` must name **every** value present — an unlisted value is
   an error, not a silent drop to NA that would delete rows from the figure.
6. **A long default axis or legend title can squeeze the panel.** The default
   title is the verbatim `aes()` text; set it with `labs()` if it is long.
7. **Caps error instead of lying.** `scale_*_manual(values=)` holds 64
   colours; more errors, and a positional list shorter than the factor (or the
   heatmap's category count) errors naming both counts. A discrete heatmap fill
   caps at 64 levels for the same reason. There is no discrete-category or legend-level cap any
   more. Still hard limits: 8 layers, 12 tracks, 16 heatmap objects, 256
   breaks/labels. A legend stack taller than the figure grows the auto-fit
   canvas; under an explicit `--size` it warns before clipping.
8. **`coord_flip()` is grammar-mode only** — it errors with `scale_x_genome()`
   and `ideogram()`, and is ignored in heatmap and track modes.
9. **A dense scatter is the one slow case — use `geom_point(raster=TRUE)`.**
   A vector PDF spends ~136 bytes and one fill per point, so 10^6 points takes
   ~18 s and a 31 MB file. `raster=TRUE` draws that layer into an embedded
   300-dpi image (as R's ggrastr does), leaving axes, ticks and legend as vector
   text: ~9 s and 0.35 MB. Aggregating geoms do not need it.
10. **The default font is Arial.** If it is missing, Cairo substitutes and the
    figure will not match the gallery.
11. **A categorical heatmap cannot be clustered.** Text cells (or
    `heatmap(discrete=TRUE)`) switch the fill to a discrete key, and `cluster=`
    then errors: there is no Euclidean distance between two categories. Use
    `cluster=diagonal`, `cluster=none`, or order the rows in the file. The
    first column is still read as row names, so an all-text matrix meant to be
    all data loses its first column.
