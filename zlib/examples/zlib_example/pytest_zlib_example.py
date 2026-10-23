# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import pytest
from pytest_embedded import Dut


@pytest.mark.generic
@pytest.mark.parametrize(
    'port, flash_port',
    [pytest.param('/dev/ttyUSB0', '/dev/ttyUSB0', id='ttyUSB')],
    indirect=True,
)
@pytest.mark.parametrize('target', ['esp32s3', 'esp32c6'], indirect=['target'])
def test_zlib_example(dut: Dut) -> None:
    dut.expect(r'payload: 16384 bytes, adler32 0x[0-9a-f]{8}')
    dut.expect_exact('compressed: 16384 ->')
    dut.expect(r'ratio \d+\.\d%')
    dut.expect_exact('decompressed: 16384 bytes')
    dut.expect(r'adler32: payload 0x([0-9a-f]{8}) == restored 0x\1')
    dut.expect_exact('round trip verified: 16384 bytes match the original payload')
