# Cinderplot

[![build](https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml/badge.svg)](https://github.com/zhou-lab/cinderplot/actions/workflows/conda-build.yml)
[![conda](https://img.shields.io/conda/vn/zhou-lab/cinderplot?label=conda)](https://anaconda.org/zhou-lab/cinderplot)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![docs](https://img.shields.io/badge/docs-gallery-blueviolet)](https://zhou-lab.github.io/cinderplot/)

Cinderplot is a small, fast, grammar-inspired plotting tool written in C. It
turns CSV data into publication-ready PDF graphics with Cairo and keeps the
runtime and dependency footprint deliberately small.

The project is an early prototype. Its current plotting grammar is inspired by
ggplot2, but Cinderplot is an independent implementation rather than a drop-in
replacement.

**Documentation & gallery:** https://zhou-lab.github.io/cinderplot/

## Install

From the `zhou-lab` conda channel:

```sh
conda install -c zhou-lab cinderplot
```

## License

MIT — see [LICENSE](LICENSE).
