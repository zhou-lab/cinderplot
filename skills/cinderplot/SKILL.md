---
name: cinderplot
description: Use cinderplot to render a static publication-quality figure — scatter/line/bar/box/histogram/density/tile, a clustered heatmap, or a genome locus-track view — to PDF, SVG or PNG from a CSV/TSV, without R or Python. Also use when the user mentions cinderplot, asks to port a ggplot2 call to it, or hits a cinderplot error. Covers the invocation, the three modes, the supported grammar, and the behavioural traps.
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
  comes from the extension (`.pdf`, `.svg`, `.png`); `--dpi` affects PNG only.
- Quote the whole expression in **single** quotes; string literals inside it use
  double quotes. Newlines inside the expression are fine.
- `--size WxH` in inches. A partial `9x` or `x7` auto-fits the other axis.
  Omit it entirely and every mode fits the canvas to its own content.
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
  heatmap(data="f2.tsv", right_of("a"), name="b")`.

## Four modes, chosen by which verbs appear

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
  `label`, `xend`/`xmax`, `yend`/`ymax`, `chrom`. A value is a bare column name
  or `factor(col[, levels=c(...)])`.
- **geoms**: `point line col histogram boxplot bar segment rect density tile
  raster hline vline abline text label`, plus `text_repel`/`label_repel`.
  Up to 8 layers.
- **layer options**: `size=`, `alpha=`, `linetype="solid"|"dashed"|"dotted"`,
  `bins=` (histogram), `nudge_x=`/`nudge_y=` (text), and
  `geom_point(raster=TRUE)` — see trap 9.
- **scales**: `scale_x_log10()` `scale_y_log10()` `scale_*_continuous(labels=percent,
  limits=)` `xlim()` `ylim()` `scale_*_manual(values=c(...))`
  `scale_(fill|colour)_(viridis|jet|parula|bwr|gradient|gradient2)()`,
  `scale_x_genome("seqinfo.tsv.gz")`, `ideogram("cytoband.tsv.gz")`.
- **also**: `facet_wrap(~v[, levels=c(...)][, scales=])`, `coord_flip()`, `guides(colour="none")`
  (or `--no-legend`), `labs()`/`xlab()`/`ylab()`/`ggtitle()`, and
  `theme_{gray,bw,minimal,classic,void,linedraw,light,dark,few}()`.

`facet_wrap(scales=)` takes `fixed` (default), `free_x`, `free_y`, `free`. A
freed axis is trained per panel; the axis you did not free stays shared. On a
discrete axis a panel drops the categories it has no rows for and renumbers the
rest, so column 3 means a different thing in each panel — that is ggplot2's
`drop = TRUE`. Histograms re-bin per panel under `free_x`, so bin widths differ
between panels. `scale_x_genome()` cannot be freed (use `regions()` in track
mode for several windows).

Crowded discrete tick labels **rotate automatically** (45°, then 90°) when the
measured labels do not fit the panel. Override with `scale_x_discrete(angle=N)`
or `scale_y_discrete(angle=N)`, `0..90`; `angle=0` forces horizontal.

There is **no category limit** on a discrete axis. Omitting `--size` fits the
canvas to the figure (panels, categories, label lengths) rather than defaulting
to 6x4; an explicit `--size` is always honoured as given.

### 2. Heatmap — a matrix with clustering, anchor-placed objects

```sh
cinderplot 'm.tsv + heatmap(name="m", cluster=both, rownames=right)
  + dendrogram(top_of("m")) + dendrogram(left_of("m"))
  + annotation("groups.tsv", column="diagnosis", left_of("m"))
  + legend(right_of("m"))
  + scale_fill_gradient2(low="#2166ac", mid="#f7f7f7", high="#b2182b")' out.pdf
```

Input is a **wide matrix** (first column may be row names). Placements are
`top_of` / `beneath` / `left_of` / `right_of`, each taking an anchor name plus
`pad=`/`width=`/`height=`. The first placed object must be a `heatmap()`.
Clustering is hclust ward.D2, matching R. `annotation()` takes `column=` to pick
which column colours the bar (default: the last). An annotation may be anchored
by its data column name, e.g. `legend(right_of("diagnosis"))`.

`labs(x=)`/`labs(y=)` name the axes; `labs(title=)` titles the figure.

`box=on` frames the cells and only the cells — the row/column names stay outside
it. `box="grey40"` sets the colour and implies on. Off by default; works on
`heatmap()` and `annotation()`. (`matrix()` tracks are always framed.)

`cluster=` takes `rows cols both none diagonal symmetric` (`off` = `none`). The
last two are for a **confusion matrix**, where the axes share a vocabulary and
the diagonal is the point: `cluster=both` orders the axes independently and
destroys it, so use `diagonal` (columns follow the row names, no clustering) or
`symmetric` (cluster the rows, then the columns follow them — related classes
group *and* the diagonal survives). Both need row names, and columns no row
claims are appended in input order.

The heatmap's own fill is **continuous only** — for a categorical grid use
`geom_tile()` in grammar mode, which takes a discrete fill and a keyed legend.

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
lengths are the datum and are never stretched, so its labels can crowd. `geom_tree(layout=)` takes `rectangular`
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
belonging to three categories shows three dots. Names with no row simply get no mark.


### 4. Genome locus browser — stacked tracks over one or more windows

```sh
cinderplot 'region("chr20:44616522-44655233")
  + cytoband("cytoband.tsv.gz", height=0.5) + genes("genes.bed.gz", height=1.5)
  + matrix("betas_long.tsv", name="b", cluster=samples, rownames=off,
           colnames=off, height=10)' locus.pdf
```

Track verbs: `coverage() interval() genes() arcs() matrix() cytoband()`, each
taking a file plus `name= height= max= color= data= cluster= rownames=
colnames= transcripts=`. Inputs are BED/bedGraph/BEDPE/BED12/matrix TSV, tabix
`.gz` allowed. `region()` with no argument infers the window from `matrix()`.

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
Supported inside `regions()` so far: `matrix()`, `genes()`, `interval()`.

## When unsure, try it

Every unimplemented verb, aesthetic or option fails with a message that
**enumerates the supported subset**, so a wrong guess costs one fast run and
returns a menu rather than a syntax error.

Known absent: `geom_smooth()`, `facet_grid()`, `facet_wrap(scales=)`, an
arbitrary `theme()` (presets only), stacked/dodged bars, and statistical
transformations beyond binning and density.

## Traps

1. **`y` must be numeric except with `geom_tile()`.** A category-vs-value bar
   chart is `aes(x=factor(cat), y=value) + geom_col() + coord_flip()`, not
   `aes(x=value, y=cat)`.
2. **`geom_boxplot()` and `geom_bar()` need a discrete x** — wrap it:
   `aes(x=factor(g), ...)`. A text column is discrete already.
3. **Colour on bars must be constant per panel.** `geom_col()`/`geom_histogram()`
   accept `fill=` only when every bar in a panel shares a value — the common
   faceted case. Genuinely mixed fills need stacking or dodging, which is not
   implemented.
4. **`geom_histogram()`/`geom_bar()`/`geom_density()` compute y themselves** —
   do not map `y`, and do not combine them with other data geoms.
5. **Factor level order is sorted, R-style.** Impose an order with `levels=`:
   `facet_wrap(~panel, levels=c("b","a"))` or `aes(colour=factor(m, levels=c("B","A")))`.
   Unlike R, `levels=` must name **every** value present — an unlisted value is
   an error, not a silent drop to NA that would delete rows from the figure.
6. **A long default axis or legend title can squeeze the panel.** The default
   title is the verbatim `aes()` text; set it with `labs()` if it is long.
7. **Silent and hard caps.** `scale_*_manual(values=)` keeps the first 16
   colours. Hard errors above 40 discrete x categories, 15 legend levels,
   8 layers, 12 tracks, 16 heatmap objects.
8. **`coord_flip()` is grammar-mode only** — it errors with `scale_x_genome()`
   and `ideogram()`, and is ignored in heatmap and track modes.
9. **A dense scatter is the one slow case — use `geom_point(raster=TRUE)`.**
   A vector PDF spends ~136 bytes and one fill per point, so 10^6 points takes
   ~18 s and a 31 MB file. `raster=TRUE` draws that layer into an embedded
   300-dpi image (as R's ggrastr does), leaving axes, ticks and legend as vector
   text: ~9 s and 0.35 MB. Aggregating geoms do not need it.
10. **The default font is Arial.** If it is missing, Cairo substitutes and the
    figure will not match the gallery.
