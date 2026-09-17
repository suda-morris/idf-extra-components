# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Unlicense OR CC0-1.0
#
# Runs on the target: the panel is driven through SDL, so the test needs the
# same privileges as the example.

import pytest
from pytest_embedded import Dut

PDU_HOST = 'esp_lcd_host'


@pytest.mark.host_test
@pytest.mark.parametrize('target', ['linux'], indirect=['target'])
def test_esp_lcd_host(dut: Dut) -> None:
    dut.run_all_single_board_cases(group=PDU_HOST)
