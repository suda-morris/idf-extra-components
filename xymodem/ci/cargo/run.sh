#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# This component's cargo recipe. Repo CI discovers every
# <component>/ci/cargo/run.sh (see .github/workflows/cargo_tests.yml) and
# runs it from the component directory.
#
# Not an IDF host test: those still go through idf.py and Unity. This file
# is cargo fmt / test / clippy. The cbindgen snapshot is refreshed by the
# pre-commit hook (core/tools/update_header.sh), not here.
set -euo pipefail
core="$(cd "$(dirname "$0")/../../core" && pwd)"
cd "$core"

cargo fmt --all -- --check
cargo test -p xymodem_proto "$@"
cargo test -p xymodem_ffi --lib "$@"
cargo clippy --workspace --all-targets -- -D warnings
