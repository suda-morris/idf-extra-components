# X/YMODEM Serial OTA Example (UHCI / UART DMA)

Updates the firmware over a plain UART using **YMODEM**, with the transfer
streamed through the UHCI controller (UART DMA). This is the UHCI showcase of
the xymodem driver: the receive path runs entirely inside a DMA + ISR chain
while the engine streams flash writes behind it.

## Why YMODEM (and not XMODEM)

- Block 0 declares the **exact image size**, which `esp_ota_begin()` needs.
  Classic XMODEM pads the tail with `0x1A` and the real length is unknowable
  on the wire, so an XMODEM updater cannot size the write correctly.
- 1K blocks quarter the ACK round trips: a ~200 KB image takes ~25 s at
  115200 instead of ~100 s.
- A YMODEM batch can carry a **second file** — the image's `sha256` — which
  this example verifies against the flashed bytes before switching the boot
  partition. (Classic XMODEM has no way to carry metadata.)

## Wiring and the UHCI paths

Same bench as `examples/xymodem_event`: console on USB Serial/JTAG, transfer
port on UART0 (`CONFIG_EXAMPLE_UART_TX_GPIO` / `..._RX_GPIO`, defaults
GPIO11/GPIO12 on ESP32-C5-DevKitC-1).

- **RX** — `uhci_start_receive_continuous()` keeps DMA running; every chunk
  fires the `on_rx_trans_event` callback **in ISR context**, which calls
  `xymodem_on_receive()` — the driver's ISR path (spinlock copy into its
  ingest ring). `sdkconfig.defaults` sets `CONFIG_UHCI_ISR_CACHE_SAFE=y`, so
  the UHCI/GDMA ISRs are IRAM variants; the callback here is `IRAM_ATTR` and
  the DMA storage buffer is internal RAM. That is what makes it legal for
  `esp_ota_write()` (which disables the flash cache) to block inside
  `data_sink` while bytes keep arriving.
- **TX** — `emit()` stages the bytes (the protocol's buffer is only valid
  during the call) into a static buffer, then `uhci_transmit()` +
  `uhci_wait_all_tx_transaction_done()`, so the bytes are on the wire before
  the protocol's timers are re-armed.
- **Engine** — the main loop calls `xymodem_poll()` every 10 ms; the ingest
  ring absorbs anything that arrives while a callback blocks.

## Batch layout

```
'C' ──▶ block 0 "xymodem_ota.bin"  <size>   ──▶ ACK, 'C'
        block 1..n image bytes (1K blocks)  ──▶ ACK ...
        EOT                                 ──▶ ACK, 'C'
        block 0 "xymodem_ota.sha256"        ──▶ ACK, 'C'
        block 1 digest text (64 hex + \n)   ──▶ ACK
        EOT                                 ──▶ ACK, 'C'
        block 0 empty (batch terminator)    ──▶ ACK → verify → boot
```

One `xymodem_start_recv()` receives the whole batch: `on_file` opens the OTA
slot for `*.bin` and switches to a digest buffer for `*.sha256`; the empty
terminator ends the session and the app verifies + boots. A missing digest
file skips verification (logged); a digest mismatch leaves the running slot
untouched and waits for the next upload.

## Build, flash, run

```bash
idf.py -p PORT flash monitor
```

Then send the batch from the host (transfer port, e.g. `/dev/ttyUSB0`):

```bash
sha256sum build/xymodem_ota.bin > ota_image.sha256
sb build/xymodem_ota.bin ota_image.sha256 > /dev/ttyUSB0 < /dev/ttyUSB0
```

or run the pytest, which sends the app's own binary with the `ymodem` Python
package and asserts that the board reboots from the *other* OTA slot:

```bash
pytest pytest_xymodem_ota.py --port /dev/ttyACM0
```

## Notes

- The partition table (`partitions.csv`) has two OTA slots sized for a 2 MB
  flash and no factory app: the flashed image runs from `ota_0`, updates land
  in the other slot. A failed or digest-mismatched upload never touches the
  running slot.
- `esp_ota_begin()` erases the target slot up front (inside the `on_file`
  callback, ~1 s for a 200 KB image); the peer simply re-sends block 0 if its
  ACK waits that long — the protocol recovers on its own.
- On targets without UHCI (or without the Rust engine's RV32 support) this
  example does not build; `xymodem_event` is the plain-UART variant.
