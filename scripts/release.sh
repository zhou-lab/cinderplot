#!/bin/sh
# scripts/release.sh — the release SOP, enforced.
#
# Every step of a release that has ever been forgotten is a check or an action
# here, so that "release 0.x.0" is never a commit that bumps the header and
# nothing else. Run from anywhere; the script cds to the repo root.
#
#   scripts/release.sh check            everything that must be true before a
#                                       release (version strings, generated docs,
#                                       feature counts, build, test suite, clean
#                                       trees). Exit 1 with a reason otherwise.
#   scripts/release.sh bump X.Y.Z       write the new version into BOTH sources
#                                       (header + conda recipe) and regenerate
#                                       the docs; then run `check`.
#   scripts/release.sh deploy           build the PORTABLE binary (no rpath),
#                                       verify it links the system cairo, copy it
#                                       to the lab bin, stamp the deployed commit,
#                                       then rebuild the dev binary.
#   scripts/release.sh tag              create the annotated tag vX.Y.Z for the
#                                       current version (refuses on a dirty tree
#                                       or a failing `check`). Local only.
#   scripts/release.sh push             push BOTH repos in the order CI needs
#                                       (cinderplot-examples first), running
#                                       `check` in between; pushes the tag too
#                                       when one exists for this version.
#   scripts/release.sh watch            follow the CI run for the pushed commit
#                                       and print the failing log if it is red.
#   scripts/release.sh status           what is deployed where vs. HEAD.
#
# The whole sequence:
#   bump X.Y.Z -> (commit) -> check -> tag -> push -> watch -> deploy -> status
#
# Environment:
#   CINDERPLOT_BUILD_PREFIX   conda env holding cairo for the HPC build
#                             (default ~/tmp/cinderplot/build; unset on a machine
#                             with pkg-config cairo)
#   CINDERPLOT_DEPLOY_DIR     where `deploy` puts the binary
#                             (default /mnt/isilon/zhoulab/labbin if it exists)
#   CINDERPLOT_EXAMPLES       the sibling cinderplot-examples checkout
#                             (default ../cinderplot-examples)

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here"

EXAMPLES=${CINDERPLOT_EXAMPLES:-"$here/../cinderplot-examples"}
DEPLOY_DIR=${CINDERPLOT_DEPLOY_DIR:-/mnt/isilon/zhoulab/labbin}
P=${CINDERPLOT_BUILD_PREFIX:-"$HOME/tmp/cinderplot/build"}

red()   { printf '\033[1;31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[1;32m%s\033[0m\n' "$*"; }
fail()  { red "release: $*"; exit 1; }
step()  { printf '\n== %s\n' "$*"; }

header_version() {
    sed -n 's/^#define CINDERPLOT_VERSION "\([^"]*\)".*/\1/p' include/cinderplot.h
}
recipe_version() {
    sed -n 's/.*set version = "\([^"]*\)".*/\1/p' conda-recipe/meta.yaml
}

# The make flags for this machine. With pkg-config cairo present the Makefile
# finds it; otherwise point at the conda build env. RPATH=1 bakes the env's
# lib dir in (dev build); RPATH=0 leaves it out (portable build for the lab).
make_flags() {   # $1 = rpath 0/1
    if pkg-config --exists cairo 2>/dev/null; then
        echo 'CFLAGS=-std=gnu11 -Wall -Wextra -O2'
        return
    fi
    [ -d "$P/lib" ] || fail "no cairo: neither pkg-config nor CINDERPLOT_BUILD_PREFIX=$P"
    if [ "$1" = 1 ]; then libs="-L$P/lib -lcairo -Wl,-rpath,$P/lib"
    else                  libs="-L$P/lib -lcairo"; fi
    printf '%s\n' "CC=cc" "PKG_CONFIG=$P/bin/pkg-config" \
        "CAIRO_CFLAGS=-I$P/include/cairo -I$P/include" \
        "CAIRO_LIBS=$libs" "CFLAGS=-std=gnu11 -Wall -Wextra -O2"
}

build() {   # $1 = rpath 0/1 ; rebuilds ./cinderplot from scratch, fails on new warnings
    flags=""
    make_flags "$1" > .release-make-flags
    while IFS= read -r f; do flags="$flags '$f'"; done < .release-make-flags
    rm -f .release-make-flags
    sh -c "make -B $flags" > .release-build.log 2>&1 || { cat .release-build.log >&2; rm -f .release-build.log; fail "build failed"; }
    if grep -E 'warning:' .release-build.log | grep -v 'Wformat-truncation' | grep -q .; then
        grep -E 'warning:' .release-build.log | grep -v 'Wformat-truncation' >&2
        rm -f .release-build.log
        fail "build has new warnings (only the two gzio.c -Wformat-truncation notes are accepted)"
    fi
    rm -f .release-build.log
    [ -x ./cinderplot ] || fail "build produced no binary"
}

# ---------------------------------------------------------------- check ----
do_check() {
    v=$(header_version); r=$(recipe_version)
    step "version strings"
    [ -n "$v" ] || fail "no CINDERPLOT_VERSION in include/cinderplot.h"
    [ "$v" = "$r" ] || fail "header says $v but conda-recipe/meta.yaml says $r — run: scripts/release.sh bump $v"
    echo "header = recipe = $v"

    step "previous versions must not linger anywhere"
    prev=$(git tag --list 'v*' | sed 's/^v//' | sort -V | tail -1)
    if [ -n "$prev" ] && [ "$prev" != "$v" ]; then
        lingering() {
            grep -rnI --exclude-dir=.git --exclude-dir=tmp --exclude=CLAUDE.md \
                 --exclude='*.o' --exclude=cinderplot -F "$prev" . \
               | grep -v 'docs/index.html\|docs/gallery.html\|release.sh\|RELEASING.md\|design-notes'
        }
        if lingering | grep -q .; then
            lingering >&2
            fail "the previous version string $prev still appears above"
        fi
    fi
    echo "ok (last tag: ${prev:-none})"

    step "generated docs are in sync with their sources"
    gen="docs/index.html docs/gallery.html docs/llms.txt"
    before=$(cat $gen | cksum)
    python3 docs/build.py >/dev/null 2>&1 || fail "docs/build.py failed"
    rm -rf docs/__pycache__
    after=$(cat $gen | cksum)
    [ "$before" = "$after" ] || fail "docs/ changed when regenerated: the committed generated files were stale (run python3 docs/build.py and commit the result)"
    grep -q "$v" docs/index.html || fail "docs/index.html does not carry version $v"
    echo ok

    step "no editing artefacts in the published skill"
    if grep -n 'NOMATCH\|TODO\|FIXME\|XXX' skills/cinderplot/SKILL.md docs/llms.txt | grep -q .; then
        grep -n 'NOMATCH\|TODO\|FIXME\|XXX' skills/cinderplot/SKILL.md docs/llms.txt >&2
        fail "artefact strings in SKILL.md / llms.txt (a bad search-and-replace once shipped 'NOMATCH2' for six releases)"
    fi
    # trap numbering must be contiguous
    nums=$(sed -n '/^## Traps/,$p' skills/cinderplot/SKILL.md | grep -o '^[0-9]*\. ' | tr -d '. ' | tr '\n' ' ')
    expect=$(seq 1 "$(echo "$nums" | wc -w)" | tr '\n' ' ')
    [ "$nums" = "$expect" ] || fail "trap numbering in SKILL.md is $nums"
    echo ok

    step "mode count agrees everywhere"
    n_skill=$(grep -c '^### [0-9]\+\. ' skills/cinderplot/SKILL.md)
    n_build=$(python3 -c "
import importlib.util,sys
s=importlib.util.spec_from_file_location('b','docs/build.py'); b=importlib.util.module_from_spec(s); s.loader.exec_module(b)
print(len(b.MODES))")
    word() { case "$1" in 1) echo one;; 2) echo two;; 3) echo three;; 4) echo four;; 5) echo five;; 6) echo six;; 7) echo seven;; *) echo "$1";; esac; }
    w=$(word "$n_skill"); W=$(echo "$w" | tr '[:lower:]' '[:upper:]'); Wc=$(echo "$w" | sed 's/^./\u&/')
    [ "$n_skill" = "$n_build" ] || fail "SKILL.md has $n_skill mode sections but docs/build.py MODES has $n_build"
    grep -q "^## $Wc modes" skills/cinderplot/SKILL.md || fail "SKILL.md heading is not '## $Wc modes'"
    grep -q "the $w modes" skills/cinderplot/SKILL.md || fail "SKILL.md frontmatter does not say 'the $w modes'"
    grep -q "^$Wc modes" README.md || fail "README.md does not start its mode paragraph with '$Wc modes'"
    grep -q "$w modes" README.md || fail "README.md llms.txt paragraph does not say '$w modes'"
    grep -q "$W MODES" src/main.c || fail "src/main.c --help does not say '$W MODES'"
    grep -q "the $w modes" docs/build.py || fail "docs/build.py AGENT_PROMPT does not say 'the $w modes'"
    echo "ok ($n_skill modes)"

    step "build (dev, with rpath) — no new warnings"
    build 1
    ./cinderplot --version | grep -q "cinderplot $v" || fail "--version does not print $v"
    ./cinderplot --help | grep -q "$v" || fail "--help does not print $v"
    echo ok

    step "regression suite ($EXAMPLES/tests/test.sh)"
    [ -f "$EXAMPLES/tests/test.sh" ] || fail "cinderplot-examples not found at $EXAMPLES (set CINDERPLOT_EXAMPLES)"
    ( cd "$EXAMPLES" && CINDERPLOT="$here/cinderplot" sh tests/test.sh >/dev/null 2>.release-test.err ) \
        || { tail -20 "$EXAMPLES/.release-test.err" >&2; rm -f "$EXAMPLES/.release-test.err"; fail "test suite failed"; }
    rm -f "$EXAMPLES/.release-test.err"
    echo ok

    step "cinderplot-examples is pushed (the test job reads its default branch)"
    if ( cd "$EXAMPLES" && git rev-parse --abbrev-ref @{u} >/dev/null 2>&1 ); then
        ahead=$( cd "$EXAMPLES" && git log --oneline @{u}..HEAD | wc -l )
        [ "$ahead" -eq 0 ] || fail "$EXAMPLES is $ahead commit(s) ahead of its remote — push it BEFORE this repo, or CI runs the new binary against the old suite"
        echo ok
    else
        echo "skipped (no upstream configured)"
    fi

    step "clean trees"
    git diff --quiet && git diff --cached --quiet || fail "uncommitted changes in $here"
    [ -z "$(git ls-files --others --exclude-standard)" ] || fail "untracked files in $here: $(git ls-files --others --exclude-standard | tr '\n' ' ')"
    ( cd "$EXAMPLES" && git diff --quiet && git diff --cached --quiet ) || fail "uncommitted changes in $EXAMPLES (the tests for this release must be committed too)"
    echo ok

    green "check passed for $v"
}

# ----------------------------------------------------------------- bump ----
do_bump() {
    new=${1:-}
    echo "$new" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "bump needs X.Y.Z"
    old=$(header_version)
    git tag --list "v$new" | grep -q . && fail "tag v$new already exists"
    step "bump $old -> $new"
    sed -i "s/^#define CINDERPLOT_VERSION \"$old\"/#define CINDERPLOT_VERSION \"$new\"/" include/cinderplot.h
    sed -i "s/set version = \"[^\"]*\"/set version = \"$new\"/" conda-recipe/meta.yaml
    [ "$(header_version)" = "$new" ] || fail "header edit failed"
    [ "$(recipe_version)" = "$new" ] || fail "recipe edit failed"
    python3 docs/build.py >/dev/null || fail "docs/build.py failed"
    git --no-pager diff --stat
    echo
    echo "Now: review the --help text, README and SKILL.md feature lists for anything"
    echo "that landed since $old (scripts/release.sh check verifies the counts, not the"
    echo "prose), commit everything as 'release $new: ...', then scripts/release.sh check."
}

# --------------------------------------------------------------- deploy ----
do_deploy() {
    v=$(header_version)
    [ -d "$DEPLOY_DIR" ] || fail "deploy dir $DEPLOY_DIR does not exist (set CINDERPLOT_DEPLOY_DIR)"
    git diff --quiet && git diff --cached --quiet || fail "deploy from a clean, committed tree only — the deployed stamp records HEAD"
    step "portable build (no rpath) for $DEPLOY_DIR"
    build 0
    if ldd ./cinderplot | grep -q "$P/lib"; then
        fail "portable binary still links cairo from $P — the rpath is in; other users cannot read that env"
    fi
    ldd ./cinderplot | grep cairo
    rev=$(git rev-parse --short HEAD)

    # This binary is about to become every lab user's cinderplot, and the dev
    # build proves nothing about it: it links a different cairo. Run the whole
    # suite against THIS binary, exactly as a user would run it.
    step "regression suite against the portable binary"
    ( cd "$EXAMPLES" && CINDERPLOT="$here/cinderplot" sh tests/test.sh >/dev/null 2>&1 ) \
        || fail "the portable binary fails the suite — not installing it"
    echo ok

    step "deploying a commit nobody can identify is worse than deploying nothing"
    if git rev-parse --abbrev-ref @{u} >/dev/null 2>&1; then
        [ "$(git log --oneline @{u}..HEAD | wc -l)" -eq 0 ] \
            || fail "HEAD is not pushed; --version would name a revision no one else can fetch"
        echo ok
    else
        echo "no upstream configured; skipping"
    fi

    step "install"
    # Keep the outgoing binary next to the new one. A bad deploy is otherwise
    # unrecoverable without a rebuild, and the people who notice are users.
    if [ -x "$DEPLOY_DIR/cinderplot" ]; then
        cp -p "$DEPLOY_DIR/cinderplot" "$DEPLOY_DIR/cinderplot.prev" \
            && echo "previous binary kept as cinderplot.prev ($("$DEPLOY_DIR/cinderplot" --version 2>/dev/null))"
    fi
    cp ./cinderplot "$DEPLOY_DIR/cinderplot.new" && mv -f "$DEPLOY_DIR/cinderplot.new" "$DEPLOY_DIR/cinderplot"
    chmod 755 "$DEPLOY_DIR/cinderplot"
    printf '%s %s %s\n' "$v" "$rev" "$(date +%Y-%m-%dT%H:%M)" > "$DEPLOY_DIR/cinderplot.deployed"

    step "verify the installed copy renders"
    "$DEPLOY_DIR/cinderplot" --version
    tmpd=$(mktemp -d)
    printf 'x,y,g\n1,2,a\n2,4,b\n3,1,a\n' > "$tmpd/t.csv"
    if "$DEPLOY_DIR/cinderplot" "$tmpd/t.csv + aes(x,y,colour=factor(g)) + geom_point()" \
         -o "$tmpd/t.pdf" >/dev/null 2>&1 && [ -s "$tmpd/t.pdf" ]; then
        echo "ok — rendered from $DEPLOY_DIR with no environment"
        rm -rf "$tmpd"
    else
        rm -rf "$tmpd"
        fail "the installed binary did not render; roll back with: mv $DEPLOY_DIR/cinderplot.prev $DEPLOY_DIR/cinderplot"
    fi

    step "rebuild the dev binary (with rpath) so the regression baseline matches again"
    build 1
    green "deployed $v ($rev) to $DEPLOY_DIR"
}

# ------------------------------------------------------------------ tag ----
do_tag() {
    v=$(header_version)
    do_check
    step "tag v$v"
    git tag -a "v$v" -m "release $v"
    green "tagged v$v (local — nothing is published until it is pushed)"
    echo
    echo "Next:  scripts/release.sh push     # both repos, in the order CI needs"
}

# ----------------------------------------------------------------- push ----
# Order matters and is not expressible in the workflow file: the test job
# checks cinderplot-examples out at its DEFAULT BRANCH, not at a matching
# commit, so pushing this repo first runs the new binary against the old
# suite. That is not hypothetical -- it is how the 0.22.0 push first failed.
do_push() {
    v=$(header_version)
    step "both trees must be committed"
    git diff --quiet && git diff --cached --quiet || fail "uncommitted changes in $here"
    ( cd "$EXAMPLES" && git diff --quiet && git diff --cached --quiet ) \
        || fail "uncommitted changes in $EXAMPLES"
    echo ok

    step "push $EXAMPLES first (CI reads its default branch)"
    if ( cd "$EXAMPLES" && git rev-parse --abbrev-ref @{u} >/dev/null 2>&1 ); then
        ( cd "$EXAMPLES" && git push origin HEAD )
    else
        echo "no upstream configured; skipping"
    fi

    do_check          # now that the examples repo is pushed, this can pass

    step "push $here"
    if git rev-parse "v$v" >/dev/null 2>&1; then
        echo "tag v$v exists — pushing it too (this publishes the conda package)"
        git push origin main "v$v"
    else
        echo "no v$v tag — pushing main only (no package will be published)"
        git push origin main
    fi
    green "pushed"
    echo
    echo "Next:  scripts/release.sh watch    # follow the CI run for this commit"
}

# ---------------------------------------------------------------- watch ----
# "Green" has to mean green for THIS commit. The badge shows the last run on
# main, which is a different thing whenever a push is pending, so match on the
# pushed sha rather than trusting the newest run.
do_watch() {
    command -v gh >/dev/null || fail "watch needs the gh CLI (https://cli.github.com)"
    sha=$(git rev-parse HEAD)
    step "waiting for a run on ${sha%${sha#????????}}"
    id=""
    i=0
    while [ "$i" -lt 30 ]; do
        id=$(gh run list --workflow conda-build.yml --limit 15 \
              --json databaseId,headSha --jq \
              "[.[] | select(.headSha==\"$sha\")] | .[0].databaseId // empty" 2>/dev/null || true)
        [ -n "$id" ] && break
        i=$((i + 1)); sleep 10
    done
    [ -n "$id" ] || fail "no conda-build run appeared for this commit after 5 minutes"
    echo "run $id"
    gh run watch "$id" --exit-status --interval 20 || {
        red "CI FAILED — the log for the failing steps:"
        gh run view "$id" --log-failed 2>/dev/null | tail -40
        exit 1
    }
    step "result"
    gh run view "$id" --json conclusion,jobs \
       --jq '"run: \(.conclusion)", (.jobs[] | "  \(.conclusion)\t\(.name)")'
    green "CI is green for $(git rev-parse --short HEAD)"
    echo
    echo "Next:  scripts/release.sh deploy   # the lab binary; CI does not do this"
}

# --------------------------------------------------------------- status ----
do_status() {
    v=$(header_version)
    echo "header/recipe:  $v / $(recipe_version)"
    echo "HEAD:           $(git rev-parse --short HEAD) $(git log -1 --format=%cd --date=short)"
    echo "last tag:       $(git describe --tags --abbrev=0 2>/dev/null || echo none)"
    echo "unpushed:       $(git log --oneline @{u}.. 2>/dev/null | wc -l) commits"
    if [ -x "$DEPLOY_DIR/cinderplot" ]; then
        printf 'lab binary:     %s' "$("$DEPLOY_DIR/cinderplot" --version 2>/dev/null)"
        [ -f "$DEPLOY_DIR/cinderplot.deployed" ] && printf '   [%s]' "$(cat "$DEPLOY_DIR/cinderplot.deployed")"
        echo
    else
        echo "lab binary:     (none at $DEPLOY_DIR)"
    fi
    echo "conda channel:  $(command -v conda >/dev/null && conda search -c zhou-lab --override-channels cinderplot 2>/dev/null | awk 'END{print $2}' || echo '?')"
}

case "${1:-}" in
    check)  do_check ;;
    bump)   do_bump "${2:-}" ;;
    deploy) do_deploy ;;
    tag)    do_tag ;;
    push)   do_push ;;
    watch)  do_watch ;;
    status) do_status ;;
    *) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
