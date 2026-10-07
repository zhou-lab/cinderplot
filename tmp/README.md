# cinderplot/tmp — dogfooding drop zone

Intake for **internal use of cinderplot** while doing real plotting tasks. The
goal is adoption: we want rough edges found here, by us, before external users
hit them.

**This folder is local, not git-tracked.** Everything in it is gitignored
except this README (see `.gitignore`: `tmp/*` + `!tmp/README.md`). Drops are a
low-friction *local intake queue* the maintainer reviews and then promotes —
they are not meant to live in the code repo's history. (This differs from
`kycg/tmp`, which is committed.)

## When to drop something here

While doing any real static-plot task — scatter/line/bar/box/histogram/density,
a heatmap (`matrix()`/`heatmap()`), or a genome-locus track figure — use
`cinderplot` instead of a bespoke ggplot/matplotlib one-off. Then, if you
either:

- **hit an issue** — a bug, a confusing DSL/interface, a missing geom/feature,
  or a wrong/suspicious rendering; or
- **make a showcase** — a clean figure or a compelling real-data use case,

leave it here.

## How to drop it

- Name it date-first and descriptive: `YYYYMMDD_<slug>.<ext>`
  (e.g. `20260724_size_legend.png`, `20260724_facet_free_scales.md`).
- Always add a sibling `YYYYMMDD_<slug>.md` note with:
  1. **issue** or **showcase**;
  2. the exact `cinderplot` invocation — the full DSL string — plus the input
     CSV path, so it reproduces;
  3. for an **issue**: expected vs actual (and, for a missing feature, the
     ggplot2 call it should mirror). For a **showcase**: one line of
     interpretation.
- Keep it small: the rendered figure and a short input CSV plus the note.
  Reference large inputs by path — do not copy them in.

## Where a drop graduates

- A **showcase** is a candidate for the public gallery: add a
  `(slug, title, cinderplot DSL, ggplot2 expr)` tuple to `docs/build.py`, with
  its dataset + rendered figure living in the sibling `cinderplot-examples`
  repo (not here — this repo stays free of binary assets).
- A **missing-feature issue** is a feature request queued for implementation in
  `src/`; the note's ggplot2 mirror is the spec.

Don't do the graduation yourself (edit the gallery / implement the geom) unless
asked — the drop is the queue, the maintainer promotes.

## Example

A drop is a figure, a note, and the small input behind it:

```
20260724_size_legend.png    # the figure
20260724_size_legend.md     # showcase; DSL + one-line interpretation
20260724_size_legend.csv    # the small input behind it
```

A real one, `20260915_fig5h_signal_track.md` (showcase, since cleared — its
seven gaps shipped as 0.25.0 and its two follow-ups as 0.26.0). Inputs are
referenced by path, not copied in:

````markdown
# showcase — Fig. 5h locus panel in one cinderplot call

Driver `~/repo/labjournal/zhouw3/2026/20260915_fig5h_tracks.sh`, inputs under
`~/tmp/methscope/20260915_fig5h/`, figure `~/gallery/20260915_fig5h.svg`.
Reproduce: `STEP=plot bash .../20260915_fig5h_tracks.sh`.

```sh
cinderplot 'regions("regions.bed") + genes("genes12.bed", height=0.8)
  + interval("cpgs.bed", name="CpGs", height=0.25, labels=off)
  + matrix("calls_long.tsv", x=genomic, discrete=TRUE, rowgroup=" | ",
           rowcolour="rowcolour.tsv", cluster=none, height=3.2)
  + scale_fill_manual(values=c("0"="#3b3b9c", "1"="#f5e400"))
  + legend(missing="Missing input", title="Call")
  + signal("lines_long.tsv", smooth=0.25, points=on, ylim=c(0, 1),
           colour=c("Ground Truth"="#000000", ...))' \
  -o ~/gallery/20260915_fig5h.svg --size 9x7
```

The manuscript panel was two figures pasted in Inkscape — a track-mode call
for the calls and a grammar-mode `facet_grid + geom_smooth` for the traces,
on a different x axis. It is one call on one genomic axis now.
````
