# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
#
# End-to-end test: the board receives a file WITH a block-0 header, then
# echoes it back under the same name and closes the batch with an empty
# header (two xymodem_start_send calls). The host peer is the third-party
# `ymodem` PyPI package, so the wire behaviour is checked against an
# independent implementation.
#
# This runs the SAME firmware as pytest_xmodem_event.py (the classic case):
# there is no protocol build variant, and the receiver needs no protocol
# setting to recognise block 0.
import hashlib
import os

import pytest
from pytest_embedded import Dut

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
    seed = hashlib.sha256(b'xymodem-ymodem-example').digest()
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
def test_ymodem_event_echo(dut: Dut) -> None:
    payload = _make_payload()
    src = os.path.join(os.getcwd(), FILE_NAME)
    with open(src, 'wb') as f:
        f.write(payload)
    sink_dir = os.getcwd()

    dut.expect('Waiting for an X/YMODEM upload', timeout=30)

    import serial
    from ymodem.Socket import ModemSocket

    uart = serial.Serial(PEER_PORT, BAUDRATE, timeout=1)

    def _read(size, timeout=3):
        uart.timeout = timeout
        return uart.read(size) or None

    def _write(data, timeout=3):
        return uart.write(data)

    # VMS_RB_SB = length field + 1K, no date/mode fields. The default
    # Pro-YAM style re-parses a size-only header's length as an octal date.
    cli = ModemSocket(_read, _write, style_id='VMS_RB_SB')

    # The verdict is the protocol return codes plus the byte comparison; the
    # board's own log lines are not part of the contract and matching them
    # races the host-side exchange.
    try:
        # Host as the YMODEM sender: upload with a block-0 header. The board
        # echoes the file back under the same name, then ends the batch with
        # an empty header -- two start_send sessions on one handle.
        assert cli.send([src]), 'YMODEM send to the device failed'
        dut.expect('Echo sender ready', timeout=30)
        assert cli.recv(sink_dir), 'YMODEM receive from the device failed'
    finally:
        uart.close()

    echoed_path = os.path.join(sink_dir, FILE_NAME)
    with open(echoed_path, 'rb') as f:
        echoed = f.read()
    os.remove(echoed_path)
    assert echoed[:len(payload)] == payload, (
        'echo mismatch: sent %d bytes, got %d' % (len(payload), len(echoed))
    )
