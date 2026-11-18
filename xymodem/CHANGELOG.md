## 0.2.0

This is a deliberately breaking release: the 0.1.x blocking
`xmodem_send()`/`xmodem_recv()` API is gone, and the internal model and C
ABI were cleaned up together rather than carrying compatibility shims.

### Protocol

- **`CheckMode::Auto` retries `'C'` three times before falling back to SUM8**,
  matching common XMODEM-CRC practice. A YMODEM sender that starts a few
  seconds late is no longer knocked onto NAK, which it ignores.
- **Late EOT is ACKed** after `Complete` and in the YMODEM `AfterEot` wait, so
  a sender whose EOT ACK was lost can finish. If the sender's EOT retries are
  exhausted, it completes successfully: every data block was already ACKed.
- **XMODEM-1K mixed last block.** A 1K sender emits SOH/128 when the remaining
  payload is at most 128 bytes.
- **`Event::FileComplete` / `XYMODEM_EV_FILE_COMPLETE` (5)** fires at each
  YMODEM file EOT. `Complete` still marks the whole batch. Existing
  `XYMODEM_EV_COMPLETE` (3) and `XYMODEM_EV_FAILED` (4) values are unchanged.

### I/O errors

- **`Io` is fallible.** `emit` / `data_sink` / `data_source` return
  `Result<_, IoError>`. The engine aborts immediately with `LocalReason::Io`
  and does not ACK or send EOT afterwards. Best-effort CAN uses a raw emit so
  an I/O failure cannot recurse.
- **Core C ABI callbacks return `int` (0 = success).** `data_source` writes
  the byte count through an out-parameter. A failing callback aborts the
  engine inline (CAN + `XYMODEM_ERR_IO`); there is no separate report path.

### Design

- **Layered failures.** The flat `Reason` enum is replaced by
  `Failure { Wire(WireReason), Local(LocalReason) }`. Wire failures are
  protocol/peer/timeouts; local failures are the application or its `Io`.
  `Failure::peer_already_cancelled()` now drives the "don't echo CAN" rule,
  so a local cancel can never be confused with a peer cancel. The C ABI still
  flattens to one `XYMODEM_ERR_*` range via an exhaustive match.
- **Single source of truth.** `Session` is a tagged union
  (`Idle | Recv(RecvSession) | Send(SendSession) | Complete | Failed(Failure)`).
  Receiver-only and sender-only fields are physically unreachable from the
  opposite role's code paths, and a terminal outcome carries its own failure.
  `Status` is derived from `Session` on demand, never stored, so the old
  parallel `State`/`Status` pair (and their drift risk) is gone.
- **Machine-checked session start.** `start_recv`/`start_send` return
  `Result<(), StartError>` with a `Busy` variant instead of silently
  abandoning an in-flight session. `Engine::can_start()` exposes the same
  check. The FFI maps it to `XYMODEM_ERR_BUSY` (-9), the driver to
  `ESP_ERR_INVALID_STATE`.

### Breaking C ABI changes

- **C enums** (`xymodem_err_t`, `xymodem_status_t`, `xymodem_event_t`,
  `xymodem_check_mode_t`) replace the old `#define` soup.
  Numeric values are unchanged except `XYMODEM_ST_INVALID = 5` (NULL /
  deleted handle; `xymodem_status()` no longer returns
  `XYMODEM_ERR_INVALID_ARG`). `start_send` still infers classic / named
  file / batch terminator from `file` (`NULL`, a name, or `""`) — that
  empty pathname is the YMODEM wire token, not a protocol mode.
- **Removed everything the application can compose from what it already
  has** (reviewed against the getc/putc-shaped APIs of the Python, Rust, Go
  and npm XMODEM libraries): `xymodem_role()` — the direction is the app's
  own `start_*` choice; `XYMODEM_EV_HANDSHAKE` and `XYMODEM_EV_BLOCK` — the
  first payload callback call and the completion events cover both;
  `xymodem_core_fail_io()` — every failure source is a callback, which aborts
  inline; `xymodem_core_state_size()` — `sizeof(xymodem_core_t)` is in the
  header. Progress is a one-line counter in `data_sink`/`data_source`
  (see the guide's recipe), not an engine feature.
- **Handle magic.** `xymodem_delete()` clears a `'XYMD'` magic so a
  subsequent call returns `ESP_ERR_INVALID_ARG` / `XYMODEM_ST_INVALID`
  instead of taking a freed mutex. Callers must still stop the ISR and
  other tasks before delete.
- `XYMODEM_ST_*` is now contiguous 0..5 (was `IDLE=4`, `HANDSHAKE=0`, ...).
  `XYMODEM_EV_*` stays 1-based so 0 remains a "no event" sentinel.
- `xymodem_core_io_t::on_file` returns `bool` (`true` = accept) instead of
  an `int` ESP_OK sentinel; this removes a pointless bool -> int -> bool
  inversion. All callback fields are now nullable; `xymodem_core_init()`
  rejects a table without `emit`, and a missing sink/source/on_file has a
  defined no-op/EOF/accept behaviour.
- `xymodem_core_cancel()` now returns `int` like every other entry point.
- `xymodem_file_info_t::size` is `uint64_t` (was `size_t`), matching
  `xymodem_on_file_fn_t` and the wire header so sizes > 4 GiB survive a
  32-bit build. The example logs it with `PRIu64`.
- `start_*` on an active session returns `XYMODEM_ERR_BUSY` instead of
  succeeding silently.
- `xymodem_core_io_t::{emit,data_sink}` return `int` (0 = success).
  `data_source` writes the byte count to an out-parameter and also returns
  `int`. Non-zero aborts immediately with `XYMODEM_ERR_IO`.
- Additive: `XYMODEM_EV_FILE_COMPLETE` = 5. Existing event numbers unchanged.
- **`xymodem_on_receive()` is safe from a UHCI / UART DMA ISR and from a
  task.** From an ISR it copies into an ingest ring (that copy *is* the
  copy-out-of-DMA); `xymodem_poll()` drains the ring and runs emit /
  data_sink. From a task it feeds the caller's slice in place, so a UART
  FIFO path that already copied out of the driver ring does not copy
  again. `xymodem_config_t::rx_buf_size` sizes the ring (0 = 4096).
- IDF callbacks take `xymodem_handle_t` first (the instance / `this`), then
  `void *ctx`: `emit`, `data_sink`, `data_source`, `on_file`, and `on_event`
  (`on_event` is now `(handle, ctx, event, arg)`). The handle is unused by
  a typical UART callback today; it is there so a later getter, or one
  callback serving several instances, does not need a new signature.
  Callbacks still must not re-enter the driver (the instance lock is held).

- Sender in `WaitAck` / `WaitEotAck` / `HeaderWaitAck` treats `'C'` like NAK
  (CRC-mode receivers re-request that way). A NAK/`'C'` that arrives after the
  sender session already completed is left unanswered: in a YMODEM batch that
  `'C'` is Forsberg figure 3's request for the NEXT file header (the receiver
  sends it immediately after ACKing EOT, before the application can start the
  next session), and an EOT answer corrupted the batch handshake. The receiver
  re-requests on its own timeout, and the next `start_send()` session answers
  that fresh request. A late EOT aimed at a completed receive session is still
  ACKed, matching the extra-EOT ACK on the send side.
- `SendSession::started` is set when a peer byte arrives, not when we emit
  the first block. Handshake silence after block 0 still counts retries
  (no infinite `WaitAgain`).
- YMODEM `AfterEot` waits `response_timeout_ms` × `max_retries` for the next
  header instead of two `packet_timeout_ms` ticks. A late empty block 0
  after `Complete` is ACKed; a duplicate last data block in `AfterEot` is
  ACKed instead of ignored.
- Framer: `CAN`, line-noise, `CAN` still cancels (not only two adjacent CANs).
- Removed unused `XYMODEM_ENABLE_DEBUG_LOG` Kconfig (it had no readers).

### Fixes and robustness

- **Derive the Rust target triple from IDF's generic Kconfig values
  (`CONFIG_IDF_TARGET_ARCH` / `CONFIG_SOC_CPU_HAS_FPU`) instead of a chip-name
  whitelist.** New RISC-V chips added in ESP-IDF now match automatically; no
  edit to this component is needed. `idf_component.yml` no longer lists
  `targets:`. Non-RV32 architectures fail at CMake configure time.
- Fix a silent data loss on 32-bit targets: a declared file size >= 4 GiB was
  narrowed to `usize` before clamping, producing a zero-length delivery that
  was still ACKed and reported as complete. The clamp now happens in the
  `u64` domain, guarded by a compile-time assertion on the firmware targets.
- Fix the `xymodem_start_recv()` doxygen example, which re-entered
  `xymodem_start_send()` from the `on_event` callback and would deadlock on the
  driver's non-recursive mutex. The example now switches phases from the task
  loop via `xymodem_status()`, matching the real example.
- Distinguish a refused block-0 header (`XYMODEM_ERR_FILE_REJECTED`, -7) from
  a broken I/O callback (`XYMODEM_ERR_IO`, -6); local cancellation gets
  `XYMODEM_ERR_CANCEL_BY_LOCAL` (-8).
- Shrink `xymodem_core_t` from 4096 to 2240 bytes (280 `uint64_t` words),
  saving ~1.8 KiB of RAM per handle. Compile-time size/alignment assertions
  replace the old runtime size check.
- Panic handler calls newlib's `abort()` (ESP-IDF panic + reboot) instead of
  hanging the chip in an infinite loop.
- Harden the FFI boundary: bounded, copy-in name scanning instead of an
  unbounded `CStr::from_ptr`; raw-pointer helpers instead of safe fns that
  produced unbounded-lifetime references; `to_config` destructures
  `xymodem_core_config_t` so a new field cannot be silently dropped.
- Remove dead code: unused `Framer::check()`, an unreachable `FramerError` /
  `Result` path, and the delivered-frame counter's error handling. Remove C
  wrapper validation duplicated from the core (`strlen<=100`, check mode,
  timeouts) and fix a mutex leak on `xymodem_core_init()` failure.
- Fix the `XY_LOCK` macro so a production build that defines `configASSERT()`
  away cannot drop the mutex take, and stop truncating `uint64_t` file sizes
  to `size_t` on 32-bit targets.
- Correct the `block_1k` documentation: only the sender chooses 1K blocks;
  the receiver accepts SOH and STX alike.
- 72 host-side Rust unit tests, including regressions for a lost 'C' after
  block 0 being re-requested with NAK, `Busy` on an in-flight session, and
  the peer/local cancel distinction. Cargo CI discovers this component via
  `ci/cargo/run.sh` (see `.github/workflows/cargo_tests.yml`).
- New `examples/xymodem_ota`: serial firmware update over YMODEM on a UHCI
  (UART DMA) pipe — the receive path runs in the UHCI ISR (`CONFIG_UHCI_ISR_CACHE_SAFE=y`,
  IRAM callback chain) while `esp_ota_write` blocks in `data_sink`; the batch
  carries the image plus its sha256, verified against the flashed bytes
  before the boot partition is switched. Host peer: `sb` or the `ymodem`
  Python package; board-level pytest included.
- Programming guide rewritten for beginners: XMODEM and YMODEM are
  separate case-driven chapters (`docs/src/xmodem.md`, `docs/src/ymodem.md`)
  with sequence diagrams, entered from `docs/src/index.md`. The 0.2
  event-driven API is what those chapters teach.
- Further host tests: three `'C'` then SUM8, NAK-of-EOT, late EOT ACK, EOT
  timeout as success, 1K last-block SOH, fallible Io (no EOT/ACK after
  failure), block-number wrap 255→0, and a deterministic framer garbage feed.
- Rewrite the protocol engine as a `no_std`, sans-IO Rust state machine with a
  generated C ABI (cbindgen) and a Corrosion-based CMake build. Stable Rust
  only: every supported triple has a prebuilt `core` since 1.76, so no
  nightly and no `-Z build-std`.
- The generated C header is a build artifact; `include/xymodem_core.h` is a
  checked-in snapshot refreshed by the `xymodem-cbindgen-header` pre-commit
  hook (`core/tools/update_header.sh`). There is no separate C host-test
  harness: crate sanity is `ci/cargo/run.sh`, everything that only shows
  up on silicon is a board-level pytest run.

## 0.1.1

- Add documentation.

## 0.1.0

- Initial version with XMODEM protocol support.
