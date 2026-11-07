# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
# xymodem -- no_std Rust protocol engine wrapped as an ESP-IDF component.
#
#   core/  Rust workspace: `proto` (sans-IO state machine) + `ffi` (C ABI)
#   src/   the event-driven C driver on top of the generated C ABI
#   include/xymodem.h and the checked-in xymodem_core.h snapshot
#
# The Rust side (Corrosion, cargo and cbindgen) is wired up in
# project_include.cmake, because it has to be in place before the component
# library is registered.

idf_component_register(
    SRCS "src/xymodem_idf.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
)

# The API reports protocol outcomes as plain XYMODEM_ERR_* ints (see
# xymodem_core.h), not custom esp_err_t codes, so there is nothing to register
# with idf_define_esp_err_codes().

# Compile against the header cbindgen just generated in the build directory,
# not the checked-in snapshot. A build must reflect the current Rust source,
# while applications keep seeing the snapshot through INCLUDE_DIRS.
# Ordinary `idf.py build` never writes into include/; the snapshot is
# refreshed by the pre-commit hook (core/tools/update_header.sh).
target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "${XYMODEM_GENERATED_INCLUDE}")

# Plain archive semantics (no -Wl,--whole-archive): rustc emits one section per
# function, so --gc-sections drops the entry points nobody calls.
target_link_libraries(${COMPONENT_LIB} PUBLIC xymodem_ffi)

# Corrosion (project_include.cmake) runs cargo, which runs build.rs, which runs
# cbindgen. Depending on the imported target guarantees the generated header
# exists before src/xymodem_idf.c is compiled.
add_dependencies(${COMPONENT_LIB} xymodem_ffi)
