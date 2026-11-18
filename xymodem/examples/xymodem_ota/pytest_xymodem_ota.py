# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
#
# End-to-end test: the board receives a YMODEM batch (the built app binary +
# its sha256 digest) over the UHCI/UART-DMA pipe, streams it into the OTA
# slot, verifies the digest against the flashed image and reboots into it.
# The host peer is the third-party `ymodem` PyPI package, so the wire
# behaviour is checked against an independent implementation.
import hashlib
import os

import pytest
from pytest_embedded import Dut

# The dut's own port (--port) is the board console: pytest-embedded flashes
# the app through it and matches the log lines below. The YMODEM bytes travel
# on a SEPARATE port behind the board's USB-UART bridge; runners export it,
# a plain two-tty bench defaults to the second device.
PEER_PORT = os.getenv('XYMODEM_PEER_PORT', '/dev/ttyUSB0')


@pytest.mark.parametrize(
    'port, flash_port',
    [pytest.param('/dev/ttyACM0', '/dev/ttyACM0', id='ttyUSB')],
    indirect=True,
)
@pytest.mark.parametrize('target', ['esp32c5'], indirect=True)
def test_xymodem_ota(dut: Dut) -> None:
    # OTA the app's own binary into the other slot, so "the running partition
    # changed" is the verdict, independent of slot labels and of how many
    # times the test ran before.
    binary_path = os.path.join(dut.app.binary_path, 'xymodem_ota.bin')
    assert os.path.exists(binary_path), f'OTA binary not found at {binary_path}'
    with open(binary_path, 'rb') as f:
        payload = f.read()
    digest = hashlib.sha256(payload).hexdigest() + '\n'

    src_bin = os.path.join(os.getcwd(), 'ota_image.bin')
    src_sha = os.path.join(os.getcwd(), 'ota_image.sha256')
    with open(src_bin, 'wb') as f:
        f.write(payload)
    with open(src_sha, 'wb') as f:
        f.write(digest.encode('ascii'))

    boot_match = dut.expect(r'running from partition: (\S+)', timeout=30)
    slot_before = boot_match.group(1).decode()

    dut.expect('waiting for the OTA image', timeout=30)

    import serial
    from ymodem.Socket import ModemSocket

    baudrate = int(dut.app.sdkconfig.get('EXAMPLE_UART_BAUD_RATE'))
    uart = serial.Serial(PEER_PORT, baudrate, timeout=1)

    def _read(size, timeout=3):
        uart.timeout = timeout
        return uart.read(size) or None

    def _write(data, timeout=3):
        return uart.write(data)

    # VMS_RB_SB = length field + 1K blocks, no date/mode fields.
    cli = ModemSocket(_read, _write, style_id='VMS_RB_SB')
    try:
        assert cli.send([src_bin, src_sha]), 'YMODEM batch send to the device failed'
    finally:
        uart.close()

    # esp_ota_write blocks in data_sink while the UHCI ISR keeps ingesting;
    # a healthy log here means the ring absorbed the whole stream.
    dut.expect('image verified, booting into', timeout=60)
    os.remove(src_bin)
    os.remove(src_sha)

    # The running partition after the reboot must differ from the one the
    # test flashed (autoflash targets the first app slot, ota_0).
    boot_match = dut.expect(r'running from partition: (\S+)', timeout=30)
    slot_after = boot_match.group(1).decode()
    assert slot_after != slot_before, (
        f'booted from {slot_after} again — the OTA slot was not switched'
    )
