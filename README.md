# Cinderplot

[![build](https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml/badge.svg)](https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml)
[![conda](https://img.shields.io/conda/vn/zhou-lab/cinderplot?label=conda)](https://anaconda.org/zhou-lab/cinderplot)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![docs](https://img.shields.io/badge/docs-gallery-blueviolet)](https://zhou-lab.github.io/cinderplot/)

Cinderplot is a small, fast, grammar-inspired plotting tool written in C. It
turns a CSV/TSV into publication-ready PDF, SVG or PNG graphics with Cairo and
keeps the runtime and dependency footprint deliberately small — one binary, no
language runtime, Cairo and zlib the only link-time dependencies.

Four modes, chosen by which verbs appear: the **grammar** (`aes()` +
`geom_*()`), a **heatmap** with clustering and anchor-placed objects, a
**genome locus browser** of stacked tracks over one or more windows, and a
**Newick tree**.

The project is an early prototype. Its current plotting grammar is inspired by
ggplot2, but Cinderplot is an independent implementation rather than a drop-in
replacement.

**Documentation & gallery:** https://zhou-lab.github.io/cinderplot/

## Install

From the `zhou-lab` conda channel:

```sh
conda install -c zhou-lab -c conda-forge cinderplot
```

## Using it from a coding agent

A condensed, agent-oriented reference lives at
[llms.txt](https://zhou-lab.github.io/cinderplot/llms.txt) — the grammar, the
four modes and the behavioural traps in a fraction of the tokens the HTML docs
cost. Point your agent at that URL rather than the gallery.

## License

MIT — see [LICENSE](LICENSE).
