#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pexpect
import pytest
from pytest_embedded import Dut


@pytest.mark.disable_test(
    'IDF_TARGET == "esp32p4"',
    reason='The example wires the ST7789 to the ESP32-S3-BOX pinout; no P4 runner carries this panel',
)
@pytest.mark.generic
@pytest.mark.parametrize('target', ['esp32s3', 'esp32p4'], indirect=['target'])
def test_raylib_fireworks(dut: Dut) -> None:
    """
    The show starts, hands its first frame to the panel from PSRAM, and keeps
    rendering without tripping the watchdog.

    The frame loop itself logs nothing (SUPPORT_TRACELOG is off in the
    component), so the last assertion is the absence of a crash or of a
    platform-layer error for one full stretch of the show.
    """
    # InitPlatform() reports the runtime display against the compile-time
    # framebuffer format (RGB565, 16 bpp).
    dut.expect('display 320x240, software renderer, 16 bpp framebuffer', timeout=60)

    # The first SwapScreenBuffer() reports where the framebuffer landed. The
    # example's sdkconfig.defaults requires PSRAM; internal RAM here would
    # mean the allocator's PSRAM preference broke.
    dut.expect('framebuffer is in PSRAM', timeout=30)

    # Crash and platform-error patterns; matching any of them is a failure,
    # so the timeout that ends this block is the pass condition. Thirty
    # seconds covers dozens of bursts and would trip the task watchdog
    # several times over if the renderer ever stopped blocking.
    with pytest.raises(pexpect.TIMEOUT):
        dut.expect(
            r'Guru Meditation'
            r'|Task watchdog got triggered'
            r'|abort\(\) was called'
            r'|no memory for a \d+ row band'
            r'|could not start the present task'
            r'|renderer has no colour buffer'
            r'|esp_raylib_set_display\(\) failed',
            timeout=30,
        )
