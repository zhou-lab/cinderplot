# docs/ — the cinderplot documentation site

Plain static HTML, hosted directly on GitHub Pages at
`https://zhou-lab.github.io/cinderplot/`. Nothing runs when the site is served.

**Figures are not stored in this repo.** To keep the code repo free of growing
binary assets, the rendered figures live in the **cinderplot-examples** repo and
are served from *its* Pages site
(`https://zhou-lab.github.io/cinderplot-examples/figs/…`). The gallery links them
by absolute URL. This repo holds only the two small HTML pages.

## Pages

- `index.html` — landing page (its own compact, lab-style layout)
- `gallery.html` — the scatterplot gallery (cinderplot vs. ggplot2); `<img>`s
  point at the examples Pages site
- no `figs/` here — see `cinderplot-examples/docs/figs/`

`index.html` and `gallery.html` are **generated** by `build.py` — edit the
templates in that script, not the HTML.

## Rebuilding

```sh
python3 docs/build.py
```

Needs the built `./cinderplot` binary, `Rscript` + the `ggplot2` package, and
`pdftoppm` (poppler). It reads datasets from, and writes the rendered figures
into, a sibling checkout of
[cinderplot-examples](https://github.com/zhou-lab/cinderplot-examples):

```sh
# defaults, override as needed:
CINDERPLOT=./cinderplot \
CINDERPLOT_EXAMPLES=../cinderplot-examples \
python3 docs/build.py
```

Env overrides: `CINDERPLOT_FIGS` (where figures are written, default
`<examples>/docs/figs`) and `CINDERPLOT_FIG_BASE` (the absolute URL the gallery
links to). After a rebuild, commit `index.html` + `gallery.html` here **and**
the regenerated figures in the examples repo. `_preview.html` (self-contained,
images inlined — handy for local review) and `_gen/` are build scratch and
gitignored.

## Hosting

Two Pages sites, one origin (`zhou-lab.github.io`), so no cross-origin issues:

1. **This repo** → *Settings → Pages → Deploy from a branch*, `main`, `/docs`.
   Serves the HTML at `/cinderplot/`. `.nojekyll` makes Pages serve it verbatim.
2. **cinderplot-examples** → *Settings → Pages → Deploy from a branch*, `main`,
   `/docs`. Serves the figures at `/cinderplot-examples/figs/…`. Also has a
   `.nojekyll`.

The gallery page here depends on (2) being published for its images to load.
