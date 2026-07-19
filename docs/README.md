# docs/ — the cinderplot documentation site

Plain static HTML, hosted directly on GitHub Pages. The figures under `figs/`
are pre-rendered and committed, so **nothing runs when the site is served** —
Pages just hands out these files.

## Pages

- `index.html` — landing page
- `gallery.html` — the scatterplot gallery (cinderplot vs. ggplot2)
- `figs/` — rendered `*-cp.svg` (cinderplot) and `*-gg.png` (ggplot2 reference)

`index.html` and `gallery.html` are **generated** by `build.py` — edit the
templates in that script, not the HTML.

## Rebuilding

```sh
python3 docs/build.py
```

This needs the built `./cinderplot` binary, `Rscript` + the `ggplot2` package,
and `pdftoppm` (poppler). It reads the example datasets from a sibling checkout
of [cinderplot-examples](https://github.com/zhou-lab/cinderplot-examples):

```sh
# defaults, override as needed:
CINDERPLOT=./cinderplot \
CINDERPLOT_EXAMPLES=../cinderplot-examples \
python3 docs/build.py
```

Commit the regenerated `index.html`, `gallery.html`, and `figs/`.
`_preview.html` (a self-contained copy with images inlined) and `_gen/` are
build scratch and are gitignored.

## Hosting

**Settings → Pages** is set to *Deploy from a branch*, branch `main`, folder
`/docs`. The `.nojekyll` file here tells GitHub to serve the folder verbatim
instead of running it through Jekyll — required, since this is hand-authored
static HTML, not a Jekyll site.

Every push to `main` that changes `/docs` republishes automatically. The site
lands at `https://zhou-lab.github.io/cinderplot/`.
