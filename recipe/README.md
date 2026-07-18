# Conda recipe for cinderplot

Builds the `cinderplot` binary and publishes it to the **zhou-lab** channel.

## Install (end users)

```sh
conda install -c zhou-lab cinderplot
```

## Build & upload (maintainers)

Requires `conda-build` and `anaconda-client`:

```sh
conda install -n base conda-build anaconda-client

# Build (Linux + macOS; Windows is skipped)
conda build recipe/

# Upload the built package to the zhou-lab channel
anaconda login
anaconda upload -u zhou-lab $(conda build recipe/ --output)
```

To automatically upload on every successful build:

```sh
conda config --set anaconda_upload yes
```

## Notes

- The recipe builds the git tag `v{version}` from
  https://github.com/zhou-lab/cinderplot. Bump `version` in `meta.yaml`
  (and `CINDERPLOT_VERSION` in `include/cinderplot.h`) and push a matching
  tag before building a release.
- To build an unreleased local checkout, replace the `git_url`/`git_rev`
  lines in `meta.yaml` with `path: ..`.
- Only dependency is Cairo (pulled from conda-forge); make sure conda-forge
  is on your channel list when building:
  `conda build -c conda-forge recipe/`.
