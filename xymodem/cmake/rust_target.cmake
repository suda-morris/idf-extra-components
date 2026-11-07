# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# Pure target-triple derivation for the Rust engine. No FetchContent, no
# Corrosion, no component registration -- just IDF Kconfig values in, triple
# out. Included from project_include.cmake.
#
# Reads from the caller's scope:
#   CONFIG_IDF_TARGET_ARCH       "riscv" / "xtensa" / "" (linux)
#   CONFIG_SOC_CPU_HAS_FPU       bool: IDF's own -mabi=ilp32f switch
#   _CMAKE_TOOLCHAIN_PREFIX      e.g. "riscv32-esp-elf-" (RV32 guard)
#
# Sets in the caller's scope:
#   XYMODEM_DETECTED_TARGET
function(xymodem_derive_rust_target)
    # Only RV32 has a matching Rust triple. Guard on generic IDF values, never
    # on chip names, so a future RV64 chip fails loudly instead of silently
    # getting a riscv32 staticlib. Explicit -DXYMODEM_RUST_TARGET bypasses this
    # whole function in project_include.cmake.
    if(NOT "${CONFIG_IDF_TARGET_ARCH}" STREQUAL "")
        if(NOT "${CONFIG_IDF_TARGET_ARCH}" STREQUAL "riscv")
            message(FATAL_ERROR
                "xymodem: unsupported CONFIG_IDF_TARGET_ARCH='${CONFIG_IDF_TARGET_ARCH}' "
                "(RISC-V RV32 only). Pass -DXYMODEM_RUST_TARGET=<triple> to override.")
        endif()
        if(DEFINED _CMAKE_TOOLCHAIN_PREFIX
           AND NOT "${_CMAKE_TOOLCHAIN_PREFIX}" STREQUAL ""
           AND NOT "${_CMAKE_TOOLCHAIN_PREFIX}" MATCHES "^riscv32-")
            message(FATAL_ERROR
                "xymodem: toolchain prefix '${_CMAKE_TOOLCHAIN_PREFIX}' is not RV32; "
                "no matching Rust triple. Pass -DXYMODEM_RUST_TARGET=<triple> to override.")
        endif()
    endif()

    # The same switch IDF uses to pick -mabi=ilp32f vs the default ilp32.
    # riscv32imc is the RV32 lowest common denominator (rustc builds it with
    # +forced-atomics); the engine itself uses no atomics.
    if(CONFIG_SOC_CPU_HAS_FPU)
        set(_target "riscv32imafc-unknown-none-elf")
    else()
        set(_target "riscv32imc-unknown-none-elf")
    endif()

    set(XYMODEM_DETECTED_TARGET "${_target}" PARENT_SCOPE)
endfunction()
