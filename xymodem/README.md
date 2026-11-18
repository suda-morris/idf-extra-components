# XMODEM / YMODEM Protocol Layer

[![Component Registry](https://components.espressif.com/components/espressif/xymodem/badge.svg)](https://components.espressif.com/components/espressif/xymodem)

XMODEM is a classic serial file-transfer protocol. This component implements
the protocol as a **pure, event-driven state machine** written in `no_std` Rust,
exposed through a C ABI and wrapped as an ESP-IDF component.

> **Version 0.2.0 is a rewrite.** The old blocking `xmodem_send()/xmodem_recv()`
> API has been replaced. See [Migrating from 0.1.x](#migrating-from-01x).

## The programming model

The library never reads the wire and never blocks. The application owns the
hardware and the clock:

```
  UHCI DMA ISR  -> xymodem_on_receive(h, dma_buf, n)   // copies into ingest ring
  one loop      -> uart_read_bytes / on_receive       // in place, no extra copy
                -> xymodem_poll(h)                    // drain ring + timeouts
                -> xymodem_status(h)                  // COMPLETE / FAILED
                                          |
              +---------------------------+---------------------------+
              |                           |                           |
        emit() -> UART TX     data_sink() -> app     data_source() <- app source
```

`xymodem_on_receive()` copies only from an ISR — that copy *is* the
copy-out-of-DMA before the descriptor is recycled. From a task
(`uart_read_bytes`) the slice goes into the engine in place, because the
UART FIFO driver already copied. `xymodem_poll()` drains the ISR ring and
advances timers. A UHCI-only setup needs no RX task. `on_receive` accepts
arbitrary slices -- half a packet, several packets, or control bytes mixed
together.

## Features (first version)

- Classic XMODEM, sender and receiver
- An optional block 0 (file name/size) per session: YMODEM single-file transfer
- SUM8 and CRC-16 (auto-negotiated: three `'C'` attempts, then CRC->SUM8 fallback)
- 128-byte (SOH) and 1K (STX) blocks; a 1K sender drops the last block to SOH when it is at most 128 bytes
- Retries, timeouts, duplicate-block handling, CAN cancel, out-of-sync detection
- `XYMODEM_EV_FILE_COMPLETE` at each YMODEM file EOT (the session stays open for the next header)

XMODEM and YMODEM are not two protocols here. A session is classic XMODEM
plus one optional block 0. The receiver recognises a header-ful sender
automatically and accepts a classic header-less one just as well; the sender
opts into a header by passing file metadata to `xymodem_start_send()`. There
is no protocol field, no protocol-specific callbacks, and no batch state
machine -- a YMODEM batch is simply several `xymodem_start_send()` calls with
one file each, followed by one call with an empty name.

Not implemented yet (the architecture leaves room for them):

- YMODEM-G streaming (the stop-and-wait state machine would need a redesign)
- Xtensa targets (RISC-V only for now; see [Supported targets](#supported-targets))

The state machine is a sans-IO design: a single `Session` enum is the source
of truth (receiver-only and sender-only state live in separate variants), and
every side effect goes through an `Io` trait. `Status` is derived from the
session, never stored. Keeping protocol logic pure is what makes the whole
machine host-testable without a board.

## Add to Your Project

```bash
idf.py add-dependency "espressif/xymodem"
```

Then create one handle and pick each session's direction as it starts:

```c
xymodem_config_t cfg = {
    .core = XYMODEM_CORE_CONFIG_DEFAULT(),
    .emit = my_emit,        /* required: bytes onto the wire */
    .data_sink = my_store,      /* receiving hands accepted blocks here */
    .data_source = my_load,     /* sending pulls payload from here */
    .on_event = my_event,
    .user_ctx = &app,
};
ESP_ERROR_CHECK(xymodem_new(&cfg, &app.handle));   /* starts idle */
ESP_ERROR_CHECK(xymodem_start_recv(app.handle));   /* first session: receive */
/* ... one loop: on_receive + poll + status (or UHCI ISR + the same loop) */
ESP_ERROR_CHECK(xymodem_start_send(app.handle, NULL)); /* next session, classic */
```

Callbacks only declare capabilities; the direction of each session is chosen
explicitly with `xymodem_start_recv()` / `xymodem_start_send()` -- an echo
server simply alternates the two calls. Sessions are independent on the wire
(block numbering restarts at 1), so to the peer this looks like separate
transfers. See [`examples/xymodem_event`](examples/xymodem_event) for the full
echo demo, and [`examples/xymodem_ota`](examples/xymodem_ota) for serial
firmware updates streamed through UHCI (UART DMA) with a sha256-verified boot
switch.

### Block 0: file name and size

The same session model carries one optional block 0. There is no protocol
setting to flip: the receiver starts every session by waiting for either a
data block (classic sender) or block 0 (header-ful sender), and `on_file`
fires only when block 0 actually arrives.

```c
bool on_file(xymodem_handle_t handle, void *ctx, const char *name, uint64_t size, bool has_size) {
    (void)handle;
    return size <= my_storage_cap;   /* reject -> the transfer is cancelled */
}

xymodem_start_recv(h);   /* header-ful and classic senders both accepted */
/* on_file("fw.bin", 2048) -> payload is trimmed to exactly 2048 bytes;
 * a header without a size field keeps classic whole-block delivery */

xymodem_file_info_t file = { .name = "fw.bin", .size = 2048 };
xymodem_start_send(h, &file);        /* session with a block 0 (YMODEM) */
xymodem_start_send(h, &(xymodem_file_info_t){ .name = "", .size = 0 });
                                     /* empty block 0: batch terminator */
```

A YMODEM batch (several files over one wire session) is built by the
application: call `xymodem_start_send()` once per file, wait for each
session to complete, then once more with an empty name. Each call waits for
the receiver's fresh `C` (a `C` that landed between two sessions is
re-requested by the receiver on its own timeout), which is exactly the
YMODEM batch wire timing, so the library needs no batch bookkeeping of its
own. YMODEM-G (streaming, no per-block ACK) is out of scope.

## Supported targets

The Rust engine is built for **RISC-V RV32** parts only. The target triple is
**derived from IDF's own Kconfig values, not from a chip-name whitelist**:

| IDF Kconfig value | Rust triple |
|---|---|
| `CONFIG_IDF_TARGET_ARCH=riscv`, `CONFIG_SOC_CPU_HAS_FPU` unset | `riscv32imc-unknown-none-elf` |
| `CONFIG_IDF_TARGET_ARCH=riscv`, `CONFIG_SOC_CPU_HAS_FPU=y` | `riscv32imafc-unknown-none-elf` |
| `IDF_TARGET=linux` | host triple (host unit tests) |

Those are the same two generic values IDF reads in
`components/soc/project_include.cmake` to decide `-mabi=ilp32f` vs the default
`ilp32`, so the Rust staticlib always matches the C ABI. `riscv32imc` is the
RV32 lowest common denominator (rustc builds it with `+forced-atomics`, and the
engine uses no atomics), so it covers soft-float RV32 cores of any ISA variant;
the `imafc` triple exists only for the hard-float calling convention.

**A new RISC-V chip added to ESP-IDF needs no change here**: it brings its own
`CONFIG_*` values and is matched automatically. `idf_component.yml` deliberately
has no `targets:` list (that would become `REQUIRED_IDF_TARGETS` and reject
future chips). Non-RV32 architectures (Xtensa, future RV64) fail loudly at
CMake configure time; Xtensa's Rust ecosystem is not mature enough yet. Pass
`-DXYMODEM_RUST_TARGET=<triple>` to override the derivation for a custom or
not-yet-tested ABI.

Both triples have shipped a prebuilt `core` since Rust 1.76, so the component
builds with a plain **stable** toolchain (pinned in
`core/rust-toolchain.toml`) -- no `nightly`, no `rust-src`, no
`-Z build-std`. CMake pulls [Corrosion](https://github.com/corrosion-rs/corrosion)
v0.6.1 via `FetchContent` from `project_include.cmake` at configure time, which
also `rustup target add`s whichever triple your `IDF_TARGET` needs if it is not
installed yet (opt out with
`-DRust_RUSTUP_INSTALL_MISSING_TARGET=OFF`, e.g. on air-gapped CI; you then
have to install the target yourself).

## Build chain and the generated C header

```
Corrosion -> cargo builds core/ffi (staticlib)
ffi/build.rs -> cbindgen writes xymodem_core.h into the CMake BUILD directory
src/xymodem_idf.c -> the event-driven C driver on top of that C ABI
```

**Building never writes into the source tree.** The header cbindgen generates
lives only in `target/` and the CMake build dir, both gitignored. The copy in
`include/xymodem_core.h` is a checked-in *snapshot* for readers without a Rust
toolchain. Changing anything under `xymodem/` triggers the
`xymodem-cbindgen-header` pre-commit hook, which runs
`core/tools/update_header.sh` and stages the new snapshot. Skipping the hook
(`--no-verify`) is caught by the same hook on CI: it rewrites the file and
the job fails until the snapshot is committed. No ordinary `idf.py build`
can dirty a user's checkout.

## Tests

The protocol engine's unit tests live in the module they cover (`#[cfg(test)]
mod tests` at the bottom of `frame.rs` / `modem.rs`) and run on a host:

```bash
xymodem/ci/cargo/run.sh     # cargo fmt / test / clippy
```

They cover checksums, streaming reassembly, session restart, both directions,
timeouts, retries, cancellation, block-0 handling, a wired sender/receiver
loopback, the multi-file batch built from repeated `start_send` calls,
block-number wrap, 1K last-block SOH, late EOT, fallible I/O, and a
deterministic garbage feed through the framer. That is what catches problems
without hardware.

Everything that only shows up on real silicon (UART timing, FIFO behaviour, the
IDF wrapper) is a board-level end-to-end test: `examples/xymodem_event` plus
its two pytest files drive a full receive-then-echo cycle against host-side
`xmodem` (classic) and `ymodem` (block 0) peers, both against the same
firmware. There is deliberately no separate C host-test harness.

## Migrating from 0.1.x

The 0.1.x API was blocking: you supplied a `xymodem_transport_t` with a
`recv()` function and called `xmodem_send()`/`xmodem_recv()`. That model is
gone. In 0.2.0 you:

- register `emit`/`data_sink`/`data_source`/`on_event` callbacks in `xymodem_config_t`,
- call `xymodem_on_receive()` from a UHCI DMA ISR or from your loop,
- call `xymodem_poll()` from the same loop (the UART read timeout is the period),
- observe completion through `xymodem_status()`/`xymodem_error()`/`on_event`.

See [`include/xymodem.h`](include/xymodem.h) for the API reference and
the programming guide under [`docs/src/`](docs/src/index.md).

## Documentation

- [Start here](https://espressif.github.io/idf-extra-components/latest/xymodem/index.html)
- [XMODEM](https://espressif.github.io/idf-extra-components/latest/xymodem/xmodem.html)
- [YMODEM](https://espressif.github.io/idf-extra-components/latest/xymodem/ymodem.html)
- [API Reference](https://espressif.github.io/idf-extra-components/latest/xymodem/api.html)

## Appendix

- [Xmodem specification](https://www.menie.org/georges/embedded/xmodem_specification.html)
- [XMODEM/YMODEM Protocol Reference](https://techheap.packetizer.com/communications/modems/xmodem-ymodem_reference.html)
