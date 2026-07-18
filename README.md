# Cinderplot

Cinderplot is a small, fast, grammar-inspired plotting tool written in C. It
turns CSV data into publication-ready PDF graphics with Cairo and keeps the
runtime and dependency footprint deliberately small.

The project is an early prototype. Its current plotting grammar is inspired by
ggplot2, but Cinderplot is an independent implementation rather than a drop-in
replacement.

## Install

From the `zhou-lab` conda channel:

```sh
conda install -c zhou-lab cinderplot
```

## Build

Cinderplot requires a C11 compiler and Cairo.

```sh
make
```

A conda recipe lives in [`conda-recipe/`](conda-recipe/) — see its README for building
and publishing to the channel.

## Examples

```sh
./cinderplot data.csv -x hp -y mpg -o plot.pdf

./cinderplot \
  'data.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()' \
  -o plot.pdf

./cinderplot 'expr.csv + heatmap() + legend()' -o heatmap.pdf
```

Ready-to-run datasets, demo figures, and the regression test suite live in a
separate repo, [cinderplot-examples](https://github.com/zhou-lab/cinderplot-examples)
(kept out of this repo so it stays small).

## Layout

- `include/` — public interfaces
- `src/` — implementation and CLI entry point
- `docs/` — design notes and reference material

## License

MIT — see [LICENSE](LICENSE).
