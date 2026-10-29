<h1 align="center">Cinderplot</h1>

<p align="center">
<a href="https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml"><img alt="build" src="https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml/badge.svg"></a>
<a href="https://anaconda.org/zhou-lab/cinderplot"><img alt="conda" src="https://img.shields.io/conda/vn/zhou-lab/cinderplot?label=conda"></a>
<a href="LICENSE"><img alt="license" src="https://img.shields.io/badge/license-MIT-blue.svg"></a>
<a href="https://github.com/zhou-lab/cinderplot-examples/blob/main/tests/test.sh"><img alt="coverage" src="https://img.shields.io/endpoint?url=https%3A%2F%2Fzhou-lab.github.io%2Fcinderplot%2Fcoverage.json"></a>
<a href="https://zhou-lab.github.io/cinderplot/"><img alt="docs" src="https://img.shields.io/badge/docs-gallery-blueviolet"></a>
</p>

Cinderplot is a small, fast, grammar-inspired plotting tool written in C. It
turns a CSV/TSV into publication-ready PDF, SVG or PNG graphics with Cairo and
keeps the runtime and dependency footprint deliberately small — one binary, no
language runtime, Cairo and zlib the only link-time dependencies.

Seven modes, chosen by which verbs appear: the **grammar** (`aes()` +
`geom_*()`), a **heatmap** with clustering and anchor-placed objects, a
**genome locus browser** of stacked tracks over one or more windows, a
**Newick tree**, a **chord diagram** of from/to/value links, a **Venn
diagram** whose regions list their items with one dot per sample, and an
**UpSet plot** of intersection sizes over a set-membership matrix.

The project is an early prototype. Its current plotting grammar is inspired by
ggplot2, but Cinderplot is an independent implementation rather than a drop-in
replacement.

**Documentation & gallery:** https://zhou-lab.github.io/cinderplot/

## Install

From the `zhou-lab` conda channel, into an environment of its own (the package
brings a full Cairo/fontconfig stack you may not want in `base`):

```sh
conda create -n cinderplot -c zhou-lab -c conda-forge cinderplot
conda activate cinderplot
```

`conda install -c zhou-lab -c conda-forge cinderplot` puts it in the active
environment instead.

From source, with a C11 compiler, Cairo and pkg-config (`brew install cairo
pkg-config` on macOS):

```sh
git clone https://github.com/zhou-lab/cinderplot && cd cinderplot
make && make install PREFIX=$HOME/.local      # PREFIX defaults to /usr/local
```

## First plot

```sh
curl -LO https://raw.githubusercontent.com/zhou-lab/cinderplot-examples/main/data/mtcars.csv
cinderplot 'mtcars.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()' -o plot.pdf
```

The gallery's examples read their data from
[cinderplot-examples](https://github.com/zhou-lab/cinderplot-examples); the
genome examples also use [zhou-lab/genomes](https://github.com/zhou-lab/genomes).

## Using it from a coding agent

A condensed, agent-oriented reference lives at
[llms.txt](https://zhou-lab.github.io/cinderplot/llms.txt) — the grammar, the
seven modes and the behavioural traps in a fraction of the tokens the HTML docs
cost. Point your agent at that URL rather than the gallery.

## License

MIT — see [LICENSE](LICENSE).
