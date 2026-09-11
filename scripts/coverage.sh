#!/bin/sh
# scripts/coverage.sh — line coverage of the tests (test.sh + gallery.sh).
#
#   scripts/coverage.sh            measure, print the table, rewrite docs/coverage.json
#   scripts/coverage.sh --print    measure and print only (leave the badge alone)
#   scripts/coverage.sh --check    measure and fail if docs/coverage.json is stale
#                                  by more than $TOLERANCE points (default 2.0)
#
# The number is gcov's own summary over src/*.c and include/cinderplot.h: it
# counts EXECUTABLE lines, not source lines, and it is LINE coverage, not
# branch coverage (branch coverage is lower and is the more telling number for
# the parser -- measure it before quoting it).
#
# Everything happens in a scratch copy: the instrumented build must never
# become the binary in the repo, or the gallery baseline silently shifts.
set -eu

# The measurement must not depend on the operator's shell. FONTCONFIG_FILE is
# the sharp one: main.c's quiet_fontconfig() early-returns when it is set and
# walks its candidate list when it is not, which moves main.c by two points and
# the total by a tenth. Unset it, and the personal CINDERPLOT_* defaults, for
# the same reason the suite does.
unset FONTCONFIG_FILE FONTCONFIG_PATH CINDERPLOT_EDITABLE_SVG CINDERPLOT_BASE_LINE_SIZE

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here"
EXAMPLES=${CINDERPLOT_EXAMPLES:-"$here/../cinderplot-examples"}
P=${CINDERPLOT_BUILD_PREFIX:-"$HOME/tmp/cinderplot/build"}
TOLERANCE=${TOLERANCE:-2.0}
mode=${1:---write}

[ -f "$EXAMPLES/tests/test.sh" ] || { echo "coverage: no suite at $EXAMPLES/tests/test.sh" >&2; exit 1; }
command -v gcov >/dev/null || { echo "coverage: gcov not found (it ships with gcc)" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
cp -r src include Makefile "$work/"

# -O0 so line numbers map 1:1 -- optimisation merges and elides lines and the
# annotation stops meaning anything.
cflags="-std=gnu11 -O0 -g --coverage"
if pkg-config --exists cairo 2>/dev/null; then
    ( cd "$work" && make -B CFLAGS="$cflags" LDFLAGS="--coverage" ) >"$work/build.log" 2>&1
else
    [ -d "$P/lib" ] || { echo "coverage: no cairo (set CINDERPLOT_BUILD_PREFIX)" >&2; exit 1; }
    ( cd "$work" && make -B CC=cc PKG_CONFIG="$P/bin/pkg-config" \
        CAIRO_CFLAGS="-I$P/include/cairo -I$P/include" \
        CAIRO_LIBS="-L$P/lib -lcairo -Wl,-rpath,$P/lib" \
        CFLAGS="$cflags" LDFLAGS="--coverage" ) >"$work/build.log" 2>&1
fi
[ -x "$work/cinderplot" ] || { tail -20 "$work/build.log" >&2; echo "coverage: build failed" >&2; exit 1; }

# Counters accumulate across processes, so clear them and let ONLY the suite
# run: a stray --version adds main.c's help path and moves the total.
rm -f "$work"/src/*.gcda
( cd "$EXAMPLES" && CINDERPLOT="$work/cinderplot" sh tests/test.sh ) >"$work/suite.log" 2>&1 \
    || { tail -20 "$work/suite.log" >&2; echo "coverage: the suite failed; coverage of a red suite is meaningless" >&2; exit 1; }
# the gallery is a test too, and covers ~200 lines the suite never reaches
( cd "$EXAMPLES" && CINDERPLOT="$work/cinderplot" CINDERPLOT_REPO="$here" sh tests/gallery.sh ) \
    >"$work/gallery.log" 2>&1 \
    || { tail -20 "$work/gallery.log" >&2; echo "coverage: the gallery failed" >&2; exit 1; }

( cd "$work" && gcov -n -o src src/*.c ) >"$work/gcov.txt" 2>/dev/null || true

pct=$(python3 - "$work/gcov.txt" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
rows = re.findall(r"File '([^']+)'\nLines executed:([0-9.]+)% of (\d+)", t)
rows = [(f, float(p), int(n)) for f, p, n in rows]
tot = sum(n for _, _, n in rows)
cov = sum(p / 100 * n for _, p, n in rows)
for f, p, n in sorted(rows, key=lambda r: r[1]):
    print(f"  {f:<26}{n:>6}{p:>8.1f}%", file=sys.stderr)
print(f"  {'TOTAL':<26}{tot:>6}{100*cov/tot:>8.1f}%", file=sys.stderr)
print(f"{100*cov/tot:.1f}")
PY
)

echo "coverage: ${pct}% of executable lines (gcov, line coverage: test.sh + gallery.sh)"

badge="$here/docs/coverage.json"
case "$mode" in
  --print) exit 0 ;;
  --check)
      [ -f "$badge" ] || { echo "coverage: $badge is missing; run scripts/coverage.sh" >&2; exit 1; }
      old=$(sed -n 's/.*"message" *: *"\([0-9.]*\)%".*/\1/p' "$badge")
      awk -v a="$old" -v b="$pct" -v t="$TOLERANCE" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d>t)}' \
          && { echo "coverage: badge says ${old}% but the suite measures ${pct}% (> ${TOLERANCE} points); run scripts/coverage.sh and commit docs/coverage.json" >&2; exit 1; }
      echo "coverage: badge (${old}%) is within ${TOLERANCE} points"
      exit 0 ;;
esac

colour=$(awk -v p="$pct" 'BEGIN{
  print (p>=90)?"brightgreen":(p>=80)?"green":(p>=70)?"yellowgreen":(p>=60)?"yellow":(p>=50)?"orange":"red"}')
cat > "$badge" <<JSON
{
  "schemaVersion": 1,
  "label": "coverage",
  "message": "${pct}%",
  "color": "${colour}"
}
JSON
echo "coverage: wrote docs/coverage.json (${pct}%, ${colour})"
