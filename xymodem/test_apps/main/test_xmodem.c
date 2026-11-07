/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Classic XMODEM on two HP UARTs. Check-mode fallback, NAK/ACK recovery and
 * framing are covered by the Rust unit tests; these cases exercise the IDF
 * wrapper (callbacks, poll task, handle reuse) against real UART FIFOs.
 */
#include "unity.h"
#include "xymodem.h"
#include "test_xymodem_loop.h"

static void assert_classic_payload(size_t payload_len, bool block_1k)
{
    size_t padded = xymodem_loop_padded_len(payload_len, block_1k);
    TEST_ASSERT_EQUAL(padded, xymodem_loop_rx_len());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(xymodem_loop_tx(), xymodem_loop_rx(), payload_len);
    for (size_t i = payload_len; i < padded; i++) {
        TEST_ASSERT_EQUAL_HEX8(XYMODEM_LOOP_SUB, xymodem_loop_rx()[i]);
    }
}

TEST_CASE("XMODEM CRC16 multi-block loopback", "[xmodem]")
{
    xymodem_loop_open(XYMODEM_CHECK_CRC16, false);
    xymodem_loop_start_recv();
    xymodem_loop_start_send(300, NULL);
    xymodem_loop_wait_both();
    assert_classic_payload(300, false);
}

TEST_CASE("XMODEM 1K blocks over UART", "[xmodem]")
{
    const size_t len = 2048;
    xymodem_loop_open(XYMODEM_CHECK_CRC16, true);
    xymodem_loop_start_recv();
    xymodem_loop_start_send(len, NULL);
    xymodem_loop_wait_both();
    assert_classic_payload(len, true);
}

TEST_CASE("XMODEM reuses handles for a second session", "[xmodem]")
{
    xymodem_loop_open(XYMODEM_CHECK_CRC16, false);
    xymodem_loop_start_recv();
    xymodem_loop_start_send(180, NULL);
    xymodem_loop_wait_both();
    assert_classic_payload(180, false);

    xymodem_loop_start_recv();
    xymodem_loop_start_send(64, NULL);
    xymodem_loop_wait_both();
    assert_classic_payload(64, false);
}
