// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//! Generates the C header with cbindgen at BUILD time only.
//!
//! This is the piece that keeps a user's git checkout clean:
//!
//! * The header is always written into cargo's `OUT_DIR` (inside `target/`,
//!   which is gitignored) and into the directory CMake names through
//!   `XYMODEM_HEADER_DIR` (also inside the build directory).
//! * Nothing here EVER writes into the source tree's `include/`. That checked-in
//!   copy is a snapshot refreshed by the `xymodem-cbindgen-header` pre-commit
//!   hook (`tools/update_header.sh`) when the Rust C API changes.
//!
//! In other words: automatic generation for the compiler, pre-commit promotion
//! for git. PR #70 wrote into `include/` from build.rs on every build, which
//! makes an ordinary `idf.py build` show up as an uncommitted change. We don't.
//!
//! # Environment contract (build.rs <-> cbindgen.toml <-> CMake)
//!
//! * `CARGO_MANIFEST_DIR` (cargo) -- root of the `ffi` crate; cbindgen reads
//!   `cbindgen.toml` from here and parses `src/lib.rs`. Run cargo from the
//!   workspace root or the crate root; both set this correctly.
//! * `OUT_DIR` (cargo) -- always receives a copy of the generated header.
//! * `XYMODEM_HEADER_DIR` (optional, set by `project_include.cmake` via
//!   `corrosion_set_env_vars`) -- a build-only directory that also receives a
//!   copy, so the C wrapper can `#include "xymodem_core.h"` from the include
//!   path. Never point this at the source tree.
//! * `cbindgen.toml` `parse_deps = false` -- the whole C ABI lives in
//!   `ffi/src/lib.rs`, so the `proto` crate is never parsed.

use std::{env, path::PathBuf};

use cbindgen::{Config, Language};

fn main() {
    let crate_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // cbindgen parses the whole workspace starting at the ffi crate; the public
    // surface is entirely in ffi/src/lib.rs, so this is enough.
    let config = Config::from_root_or_default(&crate_dir);
    let bindings = cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .with_language(Language::C)
        .generate()
        .unwrap_or_else(|err| panic!("cbindgen failed on {}: {err}", crate_dir.display()));

    // 1. Always: cargo's own out dir.
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings.write_to_file(out_dir.join("xymodem_core.h"));

    // 2. Optional: the directory CMake put on the include path. Still a build
    //    artifact, never a source file.
    if let Some(dir) = env::var_os("XYMODEM_HEADER_DIR") {
        let dir = PathBuf::from(dir);
        let _ = std::fs::create_dir_all(&dir);
        bindings.write_to_file(dir.join("xymodem_core.h"));
    }

    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=cbindgen.toml");
    println!("cargo:rerun-if-changed=../proto/src");
    println!("cargo:rerun-if-env-changed=XYMODEM_HEADER_DIR");
}
