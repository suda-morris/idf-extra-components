# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# Rust toolchain wiring for xymodem.
#
# ESP-IDF includes this file *before* the component's own CMakeLists.txt (and
# before any other component builds), which is exactly where the imported
# `xymodem_ffi` target has to exist. Keeping it out of CMakeLists.txt means the
# Rust/Corrosion machinery runs once, early, instead of interleaving with the
# component registration and every downstream component's configure step.

# ---------------------------------------------------------------------------
# Rust target triple -- derived from IDF's own Kconfig, not from chip names.
#
# IDF decides the C toolchain's ABI in components/soc/project_include.cmake
# (older versions: tools/cmake/toolchain-esp32*.cmake). We read the SAME two
# generic Kconfig values it uses, so the Rust staticlib always matches the C
# ABI without maintaining a chip whitelist here:
#
#   CONFIG_IDF_TARGET_ARCH   string, "riscv" / "xtensa" / "" (linux)
#   CONFIG_SOC_CPU_HAS_FPU   bool, decides -mabi=ilp32f in IDF
#
# Both are generated into sdkconfig.cmake, which build.cmake includes before
# including any component's project_include.cmake (verified on v5.3 and master).
# A new RISC-V chip added to IDF therefore Just Works: it brings its own
# CONFIG_* values, and no edit to this component is needed.
#
# The mapping is deliberately ABI-based, not chip-based. Rust ships a finite
# set of riscv32 triples with prebuilt `core`; the only property that changes
# their ABI is the floating-point calling convention:
#
#   CONFIG_SOC_CPU_HAS_FPU=y -> ilp32f -> riscv32imafc-unknown-none-elf
#   otherwise               -> ilp32   -> riscv32imc-unknown-none-elf
#
# `riscv32imc` is the lowest common denominator of RV32: rustc builds it with
# `+forced-atomics`, so it links and runs on every RV32 core (imc, imac,
# imafc, ...) and the engine itself uses no atomics. The `imafc` branch only
# exists for the hard-float ABI, where floating-point arguments must be passed
# in FPU registers -- using an ilp32 triple there would corrupt the ABI at the
# C/Rust boundary. Both triples have shipped a prebuilt `core` since Rust 1.76,
# so no nightly and no `-Z build-std` are needed.
# ---------------------------------------------------------------------------

# Resolution order, highest priority first:
#   1. -DXYMODEM_RUST_TARGET=<triple>  explicit override (custom/untested chips)
#   2. -DRust_CARGO_TARGET=<triple>    Corrosion's own knob (parent project)
#   3. auto-detection below
#
# Both explicit overrides and the detected triple are authoritative for THIS
# configure run, so they are written with FORCE. Without it, a stale
# Rust_CARGO_TARGET left in the CMake cache by an earlier `idf.py set-target`
# would silently win and build the staticlib for the previous chip. Corrosion
# copies Rust_CARGO_TARGET into its own Rust_CARGO_TARGET_CACHED when
# FindRust runs, which happens after this file, so it always sees the fresh
# value. A parent project that wants a custom triple should use
# XYMODEM_RUST_TARGET (the component-level knob), not Rust_CARGO_TARGET.
#
# A FORCE-written cache value is indistinguishable from a caller-supplied one
# on the NEXT configure. We therefore record every value WE wrote in a separate
# cache marker; Rust_CARGO_TARGET counts as "caller-supplied" only when it is
# non-empty and does not match that marker.
set(XYMODEM_LAST_AUTO_TARGET "${XYMODEM_LAST_AUTO_TARGET}"
    CACHE INTERNAL "Last Rust target written by xymodem")

if(XYMODEM_RUST_TARGET)
    set(Rust_CARGO_TARGET "${XYMODEM_RUST_TARGET}"
        CACHE STRING "Rust target triple for Corrosion" FORCE)
    set(XYMODEM_LAST_AUTO_TARGET "${XYMODEM_RUST_TARGET}" CACHE INTERNAL "" FORCE)
    message(STATUS "xymodem: using explicit Rust target '${XYMODEM_RUST_TARGET}'")
elseif(NOT "${Rust_CARGO_TARGET}" STREQUAL "" AND
       NOT "${Rust_CARGO_TARGET}" STREQUAL "${XYMODEM_LAST_AUTO_TARGET}")
    # A parent project set Corrosion's knob directly (not a value we wrote last
    # run): respect it and skip probing.
    message(STATUS "xymodem: using caller-supplied Rust_CARGO_TARGET '${Rust_CARGO_TARGET}'")
elseif("${IDF_TARGET}" STREQUAL "linux")
    # Host build: let Corrosion pick the host triple (Rust_CARGO_TARGET stays
    # empty). The engine is tested this way with the host C compiler.
    set(Rust_CARGO_TARGET "" CACHE STRING "Rust target triple for Corrosion" FORCE)
    set(XYMODEM_LAST_AUTO_TARGET "" CACHE INTERNAL "" FORCE)
    message(STATUS "xymodem: host build, using the default Rust toolchain target")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/rust_target.cmake")
    xymodem_derive_rust_target()
    set(Rust_CARGO_TARGET "${XYMODEM_DETECTED_TARGET}"
        CACHE STRING "Rust target triple for Corrosion" FORCE)
    set(XYMODEM_LAST_AUTO_TARGET "${XYMODEM_DETECTED_TARGET}" CACHE INTERNAL "" FORCE)
    message(STATUS
        "xymodem: IDF arch='${CONFIG_IDF_TARGET_ARCH}', "
        "FPU='${CONFIG_SOC_CPU_HAS_FPU}' -> Rust target '${XYMODEM_DETECTED_TARGET}'")
endif()

# ---------------------------------------------------------------------------
# Corrosion
# ---------------------------------------------------------------------------
# Cross targets (e.g. riscv32imc-unknown-none-elf) are often missing from a
# fresh rustup install. Locally, let Corrosion add them at configure time.
# In CI the targets belong in the image: default OFF there so a build never
# silently depends on the network. No FORCE: an explicit
# -DRust_RUSTUP_INSTALL_MISSING_TARGET=ON/OFF from the caller always wins.
if(DEFINED ENV{CI})
    set(XYMODEM_INSTALL_RUST_TARGET_DEFAULT OFF)
else()
    set(XYMODEM_INSTALL_RUST_TARGET_DEFAULT ON)
endif()
set(Rust_RUSTUP_INSTALL_MISSING_TARGET ${XYMODEM_INSTALL_RUST_TARGET_DEFAULT}
    CACHE BOOL "Automatically install missing Rust targets via rustup")

include(FetchContent)
# EXCLUDE_FROM_ALL keeps Corrosion's own helper targets out of the default
# build (needs CMake >= 3.28); MakeAvailable also avoids the deprecated
# FetchContent_Populate() (CMP0169).
#
# GIT_TAG is a movable release tag, not an immutable commit. For hermetic CI
# either pin the exact commit hash here, or point FetchContent at a vendored
# checkout with -DFETCHCONTENT_SOURCE_DIR_CORROSION=<path> (required for
# offline / air-gapped builders, since GIT_REPOSITORY needs the network).
FetchContent_Declare(
    corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.6.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(corrosion)

# Only the FFI crate is imported. `core/Cargo.toml` is a workspace whose other
# member (`proto`) is a plain rlib used for host unit tests; building it as a
# staticlib here would be wrong.
corrosion_import_crate(
    MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/core/ffi/Cargo.toml"
    CRATES xymodem_ffi
    CRATE_TYPES staticlib
    PROFILE release
)

# ---------------------------------------------------------------------------
# cbindgen output
#
# build.rs always writes the header to cargo's OUT_DIR; this second path is
# where CMake expects it, so `#include "xymodem_core.h"` resolves to a build
# artifact. Nothing is ever written back into the source tree.
# ---------------------------------------------------------------------------
set(XYMODEM_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/rust_include")
file(MAKE_DIRECTORY "${XYMODEM_GENERATED_INCLUDE}")
corrosion_set_env_vars(xymodem_ffi
    "XYMODEM_HEADER_DIR=${XYMODEM_GENERATED_INCLUDE}")

set_source_files_properties("${XYMODEM_GENERATED_INCLUDE}/xymodem_core.h"
    PROPERTIES GENERATED TRUE)
