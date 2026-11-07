# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
#
# End-to-end test for the event-driven XMODEM example. The board receives one
# file over the example's UART, then echoes it back on the same handle; the
# host peer below plays the other side of both transfers.
import hashlib
import os

import pytest
from pytest_embedded import Dut

# Host peer of the example. Blob size is the payload really transferred over
# XMODEM (a multiple of 1 KB so the last block is not SUB padded).
FILE_SIZE = int(os.getenv('XYMODEM_PYTEST_FILE_SIZE', '2048'))
FILE_NAME = 'payload.bin'
BAUDRATE = int(os.getenv('XYMODEM_PYTEST_BAUDRATE', '115200'))
# The dut's own port (--port) is the board console: pytest-embedded flashes the
# app through it and matches the log lines below. The XMODEM bytes travel on a
# SEPARATE port behind the board's USB-UART bridge; runners export it, a plain
# two-tty bench defaults to the second device.
PEER_PORT = os.getenv('XYMODEM_PEER_PORT', '/dev/ttyUSB0')


def _make_payload():
    # Deterministic, non-repeating data: a protocol bug that duplicates or
    # drops a block changes the digest instead of being masked by padding.
    seed = hashlib.sha256(b'xymodem-event-example').digest()
    out = bytearray()
    while len(out) < FILE_SIZE:
        seed = hashlib.sha256(seed).digest()
        out.extend(seed)
    return bytes(out[:FILE_SIZE])

@pytest.mark.parametrize(
    'port, flash_port',
    [pytest.param('/dev/ttyACM0', '/dev/ttyACM0', id='ttyUSB')],
    indirect=True,
)
@pytest.mark.parametrize('target', ['esp32c5'], indirect=True)
def test_xymodem_event_echo(dut: Dut) -> None:
    payload = _make_payload()
    src = os.path.join(os.getcwd(), FILE_NAME)
    with open(src, 'wb') as f:
        f.write(payload)
    sink = os.path.join(os.getcwd(), 'echo.bin')

    dut.expect('Waiting for an X/YMODEM upload', timeout=30)

    import serial
    from xmodem import XMODEM

    uart = serial.Serial(PEER_PORT, BAUDRATE, timeout=1)

    # The xmodem package's contract: getc(size, timeout) returns up to size
    # bytes or None on timeout; putc(data, timeout) must swallow the optional
    # timeout abort() passes as a second argument.
    def _getc(size, timeout=1):
        uart.timeout = timeout
        return uart.read(size) or None

    def _putc(data, timeout=None):
        return uart.write(data)

    def _send():
        modem = XMODEM(_getc, _putc)
        with open(src, 'rb') as f:
            assert modem.send(f, retry=10, timeout=5), 'XMODEM send to the device failed'

    def _recv():
        modem = XMODEM(_getc, _putc)
        with open(sink, 'wb') as f:
            assert modem.recv(f, retry=10, timeout=5), 'XMODEM receive from the device failed'
        with open(sink, 'rb') as f:
            return f.read()

    # The verdict is the protocol return codes plus the byte comparison.
    try:
        _send()
        # start_send() waits for 'C'/NAK. recv() emits that byte immediately,
        # so it must not run while the board is still idle between sessions
        # (xmodem would warn "getc timeout in start sequence" and retry).
        dut.expect('Echo sender ready', timeout=30)
        echoed = _recv()
    finally:
        uart.close()

    assert echoed[:len(payload)] == payload, (
        'echo mismatch: sent %d bytes, got %d' % (len(payload), len(echoed))
    )
