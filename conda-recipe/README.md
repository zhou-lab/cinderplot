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
conda build conda-recipe/

# Upload the built package to the zhou-lab channel
anaconda login
anaconda upload -u zhou-lab $(conda build conda-recipe/ --output)
```

To automatically upload on every successful build:

```sh
conda config --set anaconda_upload yes
```

## Automated builds (CI)

`.github/workflows/conda-build.yml` runs `conda build` on every push and PR
across **linux-64**, **osx-64**, and **osx-arm64**, uploading each package as
a build artifact. On a `vX.Y.Z` tag it also publishes to the `zhou-lab`
channel — this needs an `ANACONDA_TOKEN` repository secret (an anaconda.org
token with upload rights to the org). Cutting a release is then:

```sh
# bump version in conda-recipe/meta.yaml and include/cinderplot.h, commit
git tag -a v0.2.0 -m "cinderplot 0.2.0" && git push origin v0.2.0
```

The workflow guards that the tag matches the recipe version before uploading.

## Notes

- The recipe builds the checked-out tree (`source: path: ..`), so a release
  build is just: check out the `vX.Y.Z` tag, then `conda build`. Keep
  `version` in `meta.yaml` and `CINDERPLOT_VERSION` in
  `include/cinderplot.h` in sync with the tag.
- Only dependency is Cairo (pulled from conda-forge); make sure conda-forge
  is on your channel list when building:
  `conda build -c conda-forge conda-recipe/`.
