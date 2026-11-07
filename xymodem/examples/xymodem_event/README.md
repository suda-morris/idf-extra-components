# Event-driven XMODEM example

Demonstrates the driver model requested in issue #73:

- one loop in `app_main`: `uart_read_bytes` (timeout = poll period),
  `xymodem_on_receive`, `xymodem_poll`, `xymodem_status`. A UHCI DMA ISR
  can call the same `on_receive` (ISR path copies into the ingest ring)
  and the loop then only polls;
- the library never performs a blocking `recv()`. All output leaves through
  `emit` (UART TX) and all received payload arrives through `data_sink`.

The example receives a file into RAM, then sends it back (echo), using the
standard Python `xmodem` tool as the host peer.

One `xymodem_new()` at startup and the sessions alternate explicitly:
`xymodem_start_recv()` receives the upload, then
`xymodem_start_send(h, NULL)` serves the kept data back -- no delete/re-create,
and the application buffer survives the switch. That is also why the handle
is created once in `app_main()`, not per transfer.

There is no protocol setting. The receiving side recognises a block-0 header
(file name/size) automatically and accepts a classic header-less sender just
as well. On the send side, passing a `xymodem_file_info_t *` to
`xymodem_start_send()` adds block 0; `NULL` sends classic XMODEM. A YMODEM
batch is simply several `xymodem_start_send()` calls (one file each) followed
by one call with an empty name -- the example does exactly that when the
upload carried a header, so the host tool sees a well-formed batch.

## Configure

`sdkconfig.defaults` sets what the example itself needs:

| Option | Why |
| --- | --- |
| `CONFIG_FREERTOS_HZ=1000` | makes `POLL_PERIOD_MS=5` an actual 5 ms tick |
| `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` | keeps the console off UART0, which the example may use as its protocol port |

The protocol UART is picked under **XYMODEM Event Example** in `idf.py menuconfig`
(`CONFIG_EXAMPLE_UART_*`, default UART0 on GPIO4/5 at 115200).

The defaults match the CI runner's wiring. On a desktop bench whose USB-UART
bridge sits on UART0's default pins (ESP32-C6: TX=GPIO16, RX=GPIO17), override
`CONFIG_EXAMPLE_UART_PORT_NUM=0`, `CONFIG_EXAMPLE_UART_TX_GPIO=16` and
`CONFIG_EXAMPLE_UART_RX_GPIO=17` via `idf.py menuconfig` and rebuild.

## Build and run

```
idf.py set-target esp32c6
idf.py build flash monitor
```

## Testing

End-to-end on the board, against the **same firmware**:

- `pytest_xmodem_event.py` uploads a classic header-less file with the Python
  `xmodem` package and checks the echo comes back byte-identical.
- `pytest_ymodem_event.py` uploads a file with a block-0 header using the
  Python `ymodem` package, then checks the echo (same name) and the empty
  batch terminator.

Both need a second serial port for the example's own traffic (the console
stays on USB Serial/JTAG, see above): the dut's `--port` is the console --
the test auto-flashes through it and matches the log lines -- while the XMODEM
peer is opened directly by the test. The peer device defaults to `/dev/ttyUSB0`
(override with the `XYMODEM_PEER_PORT` env var; the CI runner exports
`/dev/serial_ports/ttyUSB-esp32`):

```
pip install xmodem ymodem pyserial
pytest --target esp32c6 --port /dev/ttyACM0
```

Knobs, if the default blob is too big for a slow board: `XYMODEM_PYTEST_FILE_SIZE`
(default 2048 bytes) and `XYMODEM_PYTEST_BAUDRATE` (default 115200).

There is no separate host test app -- the Rust engine's own unit tests
(`ci/cargo/run.sh`) cover everything that can be checked without
hardware (including the multi-file batch built from repeated start_send calls),
and this example covers the UART/IDF wrapper path on real silicon.
