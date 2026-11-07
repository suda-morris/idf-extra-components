#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# Promote the cbindgen-generated header into the git-tracked snapshot.
#
# Invoked by the `xymodem-cbindgen-header` pre-commit hook when the Rust C
# API changes. A normal `idf.py build` never writes into the source tree;
# this is the single writer of include/xymodem_core.h. Safe to run by hand.
set -euo pipefail

# Locate the crate from the git work tree, not $0: pre-commit may invoke this
# from a cached copy or from the repo root.
repo="$(git rev-parse --show-toplevel)"
core_dir="$repo/xymodem/core"
component_dir="$repo/xymodem"
cd "$core_dir"

# cbindgen is invoked by ffi/build.rs during a build. Setting XYMODEM_HEADER_DIR
# makes build.rs ALSO write the header into this build-only directory; we then
# copy it into include/. Reproducible and byte-identical to what CMake uses.
out="${TMPDIR:-/tmp}/xymodem_header_$$"
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT

XYMODEM_HEADER_DIR="$out" cargo build --release -p xymodem_ffi --quiet

generated="$out/xymodem_core.h"
dest="$component_dir/include/xymodem_core.h"
if [[ ! -f "$generated" ]]; then
    echo "error: cbindgen did not produce $generated" >&2
    exit 1
fi

mkdir -p "$component_dir/include"
if [[ -f "$dest" ]] && cmp -s "$generated" "$dest"; then
    echo "$dest is up to date"
    exit 0
fi
cp "$generated" "$dest"
echo "updated $dest"
