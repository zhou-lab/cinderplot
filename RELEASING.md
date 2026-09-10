# Releasing cinderplot

A release is five things that must move together, and every one of them has
been left behind at least once: the **version strings**, the **generated
docs**, the **tag** (which is what publishes the conda package), the **CI
result**, and the **lab binary**. `scripts/release.sh` performs or checks each
one. This page is the order, and the reason behind each step.

The whole sequence, once the work is committed in both repos:

```sh
scripts/release.sh bump 0.23.0     # 1. version, in both sources, + docs
git commit -am 'release 0.23.0: …' # 2. land the bump
scripts/release.sh check           # 3. everything that must be true
scripts/release.sh tag             # 4. annotated tag, local only
scripts/release.sh push            # 5. both repos, in the order CI needs
scripts/release.sh watch           # 6. CI, for THIS commit
scripts/release.sh deploy          # 7. the lab binary
scripts/release.sh status          # 8. confirm everything agrees
```

Rule of thumb: **a user-visible feature that lands means a release.** Two
binaries reporting the same version with different features is the one state
that cannot be diagnosed from the outside. `--version` prints the git revision
too, so a stale copy is at least identifiable — ask a bug reporter for it
before reading any code.

## 1. Land the work

- Code and its tests. Every fix or feature gets a case in
  `cinderplot-examples/tests/test.sh` written so it **fails on the previous
  binary** (exit 1, then exit 0 — run it both ways). The gallery byte-identity
  check cannot see a new code path, so it proves nothing about new work.
- **Write tests that do not depend on your shell.** `CINDERPLOT_EDITABLE_SVG`
  and `CINDERPLOT_BASE_LINE_SIZE` are documented personal defaults; the suite
  unsets both up front so it always exercises the tool's own behaviour, and a
  case that needs one sets it per invocation. Not theoretical: three cases that
  grepped an SVG for `<text>` passed on a machine with the variable exported
  and failed on the first push.
- Docs in the same commit, in all of these places or the count check fails:
  `skills/cinderplot/SKILL.md` (the source of `docs/llms.txt`), the `--help`
  text in `src/main.c`, `README.md`, and the `MODES/GEOMS/SCALES/POSITIONS/
  THEMES_CHIPS` lists in `docs/build.py`. Regenerate with `python3 docs/build.py`.
- Commit in **both** repos.

## 2. Bump

```sh
scripts/release.sh bump 0.23.0
```

Writes the version into both hand-edited sources — `include/cinderplot.h` and
`conda-recipe/meta.yaml` — and regenerates the docs. The recipe is the one that
gets forgotten, and it is what names the package: it sat at 0.7.1 for fourteen
releases, so the artifact built from the 0.17.0 commit is called
`cinderplot-0.7.1-h40d0619_0.conda` and holds a binary reporting 0.17.0. CI now
fails on that mismatch, but the bump is what prevents it.

Then commit, so the tag has something to point at.

## 3. Check

```sh
scripts/release.sh check
```

Fails, with the reason, on: header ≠ recipe version; the previous version
lingering anywhere in the tree; regenerated docs differing from the committed
ones; editing artefacts (`NOMATCH`, `TODO`) or broken numbering in the
published skill; a mode count that disagrees between SKILL.md, README,
`--help` and `docs/build.py`; new compiler warnings; a failing regression
suite; `cinderplot-examples` unpushed; uncommitted or untracked files in
either repo.

## 4. Tag

```sh
scripts/release.sh tag
```

Re-runs `check`, then creates the annotated `vX.Y.Z`. Local only — nothing is
published until it is pushed.

## 5. Push, in the right order

```sh
scripts/release.sh push
```

Pushes **`cinderplot-examples` first**, then runs `check`, then pushes this
repo and the tag if one exists. The order is not a preference: the CI test job
checks the examples repo out at its **default branch**, not at a matching
commit, so pushing the code first runs the new binary against the old suite.
There is no ref to pin — the two repos are coupled by push order, and that is
how the 0.22.0 push first went red.

Pushing the tag is what triggers publication to the `zhou-lab` channel, so
`push` says which of the two it is doing.

## 6. Watch CI

```sh
scripts/release.sh watch
```

Follows the run **for the pushed commit** and prints the failing log if it goes
red. It matches on the sha rather than taking the newest run, because the badge
shows the last run on `main` — a different commit whenever a push is pending,
and it reads green while the tree in front of you has never been tested.

Three guards run there, none of which existed before 0.22.0:

| job | what it protects |
|---|---|
| Version strings agree | a package whose name contradicts its contents |
| Regression suite | the suite lives in the other repo; CI never ran it |
| Packaged binary targets an old glibc | the package must start on the RHEL 9 cluster |

The glibc guard is the one that matters most and is easiest to lose: the recipe
pins `sysroot_linux-64 2.17`, and 0.7.x once died on the cluster with
`GLIBC_2.38 not found` before printing anything, while the identical source
built locally needed only 2.34. The guard asserts the pin still takes on every
build (currently `ok: highest is GLIBC_2.14`) instead of trusting a note.

## 7. Deploy the lab binary

```sh
scripts/release.sh deploy
```

CI does not do this. Builds **without** the conda rpath (the dev build's rpath
points into a personal, unreadable env, so a copy of it breaks for every other
user), verifies `ldd` resolves the system cairo, installs to
`/mnt/isilon/zhoulab/labbin/cinderplot`, writes `cinderplot.deployed`
(`version hash date`) beside it, then rebuilds the dev binary with the rpath so
the regression baseline matches again. The two link different cairo versions
and rasterise slightly differently; that is expected, and is the price of a
binary that runs for every lab user with no environment.

## 8. Confirm

```sh
scripts/release.sh status
```

Prints header/recipe version, HEAD, last tag, unpushed count, what the lab
binary reports and what the conda channel has. All of them should agree. Run it
*first* when coming back to the project after a break — it is what would have
shown the fourteen-release drift immediately.

## When something goes wrong

- **CI refuses the tag** ("does not match recipe version"): the bump was
  skipped. Run it, commit, delete and re-create the tag.
- **The test job fails right after a push that passes locally**: check that the
  examples repo was pushed first, and whether the failing case depends on an
  environment variable your shell exports.
- **A user reports a missing feature**: ask for `cinderplot --version` — the
  revision in it identifies the build — before reading any code.
- **The gallery differs after a change**: rasterise and compare PNGs, not PDFs.
  Cairo stamps a creation time inside a compressed stream, so `cmp` on PDFs
  reports differences that are only the clock. See the repo notes.
