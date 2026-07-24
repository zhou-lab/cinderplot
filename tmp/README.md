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

```
20260724_size_legend.png    # the figure
20260724_size_legend.md     # showcase; DSL + one-line interpretation
20260724_size_legend.csv    # the small input behind it
```
