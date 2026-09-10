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

The build jobs `needs: test`, so **nothing is built, and on a tag nothing is
published, unless the suite passed**. That gate is what lets step 7 trust the
channel: the package is the only artifact the lab gets, so a red suite must
never reach it.

One gap to know about: the two build jobs run with `fail-fast: false`, and each
publishes its own package at the end. If `linux-64` failed while `osx-arm64`
succeeded, the channel could end up with the mac package and not the Linux one.
`deploy` would then refuse (it checks the channel for this version on this
platform), so the lab is never left with a half-release — but the channel would
need the missing platform rebuilt before anyone on it can install.

The glibc guard is the one that matters most and is easiest to lose: the recipe
pins `sysroot_linux-64 2.17`, and 0.7.x once died on the cluster with
`GLIBC_2.38 not found` before printing anything, while the identical source
built locally needed only 2.34. The guard asserts the pin still takes on every
build (currently `ok: highest is GLIBC_2.14`) instead of trusting a note.

## 7. Deploy to the lab

```sh
scripts/release.sh deploy
```

CI does not do this, and it is the step most often skipped — the lab copy sat
at 0.7.1 through fourteen releases.

**There is one artifact: the conda package.** `conda install -c zhou-lab -c
conda-forge cinderplot` is the default install, and the lab gets the *same*
package — `deploy` installs it into the shared env
(`conda_2026/envs/cinderplot`) and points `/mnt/isilon/zhoulab/labbin/cinderplot`
at it with a **symlink**. Nothing is built a second time.

That is a deliberate change from how this used to work. The lab binary was
separately compiled here and copied to labbin, which meant two binaries that
could report the same version and behave differently — the failure mode that
cost a user a day, and that no amount of care prevented, because the copy step
was manual and easy to skip. A symlink cannot drift.

In order, `deploy`:

1. refuses unless the channel already has this version. CI publishes on the
   tag, so this enforces the order: tag, watch, then deploy;
2. installs `cinderplot=$VERSION` into the shared env and checks the env
   reports it;
3. runs the whole regression suite against the *installed package*;
4. repoints the labbin symlink and stamps `cinderplot.deployed`;
5. renders a figure through the deployed path with `env -i` — no conda
   activation, no variables — which is how a lab member invokes it.

Why no activation is needed: the package's RPATH is `$ORIGIN/../lib`, and the
loader resolves `$ORIGIN` against the binary's **real** path, so the symlink
still finds the env's cairo. (`ldd` on the symlink disagrees — it resolves
`$ORIGIN` against the link itself and shows the system cairo. `LD_DEBUG=libs`
and the rendered SVG both confirm the env's cairo is what actually loads. Trust
the render, not `ldd`, on a symlinked binary.)

Reach is unchanged: `labbin` and `conda_2026` are both `drwxrwx--- reslnusers`,
so the same people can use either.

Rollback: `/mnt/isilon/zhoulab/labbin/cinderplot.prev` is the last
independently built binary. Restore with
`mv cinderplot.prev cinderplot`, or pin an older package with
`conda install -p <env> cinderplot=X.Y.Z`.

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
