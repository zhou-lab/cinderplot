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
make test
```

A conda recipe lives in [`conda-recipe/`](conda-recipe/) — see its README for building
and publishing to the channel.

## Examples

```sh
./cinderplot examples/data/mtcars.csv -x hp -y mpg -o plot.pdf

./cinderplot \
  'examples/data/mtcars.csv + aes(hp, mpg, colour=factor(cyl)) + geom_point()' \
  -o plot.pdf

./cinderplot 'examples/data/expr.csv + heatmap() + legend()' -o heatmap.pdf
```

## Layout

- `include/` — public interfaces
- `src/` — implementation and CLI entry point
- `examples/data/` — example CSV datasets
- `tests/` — regression tests
- `docs/` — design notes and reference material

## License

MIT — see [LICENSE](LICENSE).
