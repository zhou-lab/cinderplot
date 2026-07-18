#!/bin/bash
set -euo pipefail

# Point pkg-config at cairo's .pc in the conda host prefix. Without this the
# Makefile's `pkg-config --cflags cairo` can come up empty on Linux and fall
# back to a wrong (macOS) include path -> "cairo.h: No such file".
export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# conda provides CC, CFLAGS, CPPFLAGS, LDFLAGS (with the sysroot); the
# Makefile appends to them.
make -j"${CPU_COUNT:-1}" CC="${CC}"
make install PREFIX="${PREFIX}"
