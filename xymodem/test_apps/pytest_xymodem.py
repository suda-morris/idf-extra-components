# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
#
# On-target Unity suite. Two HP UARTs are crossed through the GPIO matrix, so
# the host only needs the console port (USB Serial/JTAG, typically ttyACM0)
# for flash and the Unity menu. Pass it with --port; do not steal the board's
# USB-UART -- those pins are not used by this app.
import pytest
from pytest_embedded_idf.utils import idf_parametrize

@pytest.mark.parametrize(
    'port, flash_port',
    [pytest.param('/dev/ttyACM0', '/dev/ttyACM0', id='ttyUSB')],
    indirect=True,
)
@pytest.mark.generic
@idf_parametrize('target', ['esp32c5', 'esp32s31'], indirect=['target'])
def test_xymodem(dut) -> None:
    dut.run_all_single_board_cases(timeout=60)
