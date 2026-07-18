#!/bin/bash
set -euo pipefail

# conda provides CC, CFLAGS, CPPFLAGS, LDFLAGS (with the sysroot) and a
# pkg-config that finds cairo in $PREFIX; the Makefile appends to them.
make -j"${CPU_COUNT:-1}" CC="${CC}"
make install PREFIX="${PREFIX}"
