#!/bin/bash
set -euo pipefail

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# Resolve cairo flags here and pass them to make explicitly. conda's
# pkg-config can set PKG_CONFIG_LIBDIR and ignore PKG_CONFIG_PATH, which made
# the Makefile fall back to a macOS include path ("cairo.h: No such file").
# Passing CAIRO_CFLAGS/CAIRO_LIBS on the command line overrides the Makefile's
# pkg-config probe; fall back to the known conda prefix layout if empty.
cairo_cflags="$(pkg-config --cflags cairo cairo-pdf 2>/dev/null || true)"
cairo_libs="$(pkg-config --libs cairo cairo-pdf 2>/dev/null || true)"
: "${cairo_cflags:=-I${PREFIX}/include/cairo}"
: "${cairo_libs:=-lcairo}"

# conda provides CC, CFLAGS, CPPFLAGS, LDFLAGS (with the sysroot); the
# Makefile appends to them.
make -j"${CPU_COUNT:-1}" CC="${CC}" \
     CAIRO_CFLAGS="${cairo_cflags}" CAIRO_LIBS="${cairo_libs}"
make install PREFIX="${PREFIX}"
