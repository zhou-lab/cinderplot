# Releasing cinderplot

A release is four things that must move together, and each has been left
behind at least once: the **version strings**, the **generated docs**, the
**tag** (which is what publishes the conda package), and the **lab binary**.
`scripts/release.sh` checks or performs every step; this page is the order to
run them in and the reasons behind the checks.

Rule of thumb: **a user-visible feature that lands means a release**. Two
binaries reporting the same version with different features is the one state
that cannot be diagnosed from the outside, so never leave `main` ahead of the
last tag for long. (`--version` now prints the git revision as well, so a stale
copy is at least identifiable.)

## 1. Land the work

- Code and its tests. Every fix or feature gets a case in
  `cinderplot-examples/tests/test.sh` written so it **fails on the previous
  binary** (exit 1 then exit 0 — run it both ways). The gallery byte-identity
  check does not see new code paths.
- Docs in the same commit, in all of these places, or the count check fails:
  `skills/cinderplot/SKILL.md` (the source of `docs/llms.txt`), the `--help`
  text in `src/main.c`, `README.md`, and the `MODES/GEOMS/SCALES/POSITIONS/
  THEMES_CHIPS` lists in `docs/build.py`. Regenerate with `python3 docs/build.py`.
- Commit in **both** repos.

## 2. Bump

```sh
scripts/release.sh bump 0.22.0     # header + conda recipe + regenerated docs
git commit -am 'release 0.22.0: <what landed>'
```

The recipe version is what the CI tag guard compares against; it was left at
0.7.1 for fourteen releases, so no package was published for any of them.

## 3. Check

```sh
scripts/release.sh check
```

Fails, with the reason, on: header ≠ recipe; the previous version string still
present anywhere; regenerated docs differing from the committed ones; editing
artefacts (`NOMATCH`, `TODO`) or broken numbering in the published skill; a
mode count that disagrees between SKILL.md, README, `--help` and build.py; new
compiler warnings; a failing test suite; uncommitted or untracked files in
either repo.

## 4. Tag and push

```sh
cd ../cinderplot-examples && git push origin main    # FIRST — see below
cd ../cinderplot
scripts/release.sh tag             # runs check again, then git tag -a vX.Y.Z
git push origin main vX.Y.Z
```

**Push `cinderplot-examples` first.** The `test` job checks that repo out at its
default branch, not at a matching commit, so pushing the code first runs the new
binary against the old suite and fails on whatever assertion the release
changed. There is no ref to pin — the two repos are coupled by push order.

The tag triggers the conda build on GitHub and publishes to the `zhou-lab`
channel. The recipe pins `sysroot_linux-64 2.17` so the package starts on the
RHEL 9 cluster (glibc 2.34), and CI asserts that on every run — the linux-64
job fails if the packaged binary needs anything above 2.17, so a regression
cannot reach the channel quietly.

## 5. Deploy the lab binary

```sh
scripts/release.sh deploy
```

Builds **without** the conda rpath (the dev build's rpath points into a
personal, unreadable env), verifies `ldd` resolves the system cairo, installs
to `/mnt/isilon/zhoulab/labbin/cinderplot`, writes
`labbin/cinderplot.deployed` (`version hash date`), then rebuilds the dev
binary with the rpath so the regression baseline matches again. The two builds
link different cairo versions and rasterise slightly differently — expected.

## 6. Confirm

```sh
scripts/release.sh status
```

prints header/recipe version, HEAD, last tag, unpushed commits, what the lab
binary reports, and what the conda channel has. All of them should agree.

## The package name must match the binary

A CI artifact is named from `conda-recipe/meta.yaml`, not from the header, so a
skipped recipe bump produces a package whose name lies about its contents. That
is not hypothetical: the artifact from the 0.17.0 release commit is called
`cinderplot-0.7.1-h40d0619_0.conda` and the binary inside reports 0.17.0. The
`test` job now compares the two version strings on every push, so this fails
fast instead of shipping.

## When something goes wrong

- **CI refuses the tag** ("does not match recipe version"): the bump step was
  skipped; run it, commit, delete and re-create the tag.
- **A user reports a missing feature**: ask for `cinderplot --version` — the
  revision in it says which build they have — before reading the code.
- **The gallery differs after a change**: rasterise and compare PNGs, not PDFs
  (cairo stamps a creation time inside a compressed stream); see the repo notes.
