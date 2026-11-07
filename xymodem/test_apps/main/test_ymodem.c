/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Block-0 (YMODEM) path on two HP UARTs. Header parsing, padding trim and
 * reject/cancel are covered by the Rust unit tests; these cases exercise
 * xymodem_start_send(h, &file) and the empty-name batch terminator through
 * the C wrapper on a real crossed UART pair.
 */
#include <string.h>
#include "unity.h"
#include "xymodem.h"
#include "test_xymodem_loop.h"

TEST_CASE("Block-0 single file loopback", "[ymodem]")
{
    const size_t len = 200;

    xymodem_loop_open(XYMODEM_CHECK_CRC16, false);
    xymodem_loop_start_recv();
    xymodem_loop_start_send(len, "loop.bin");
    xymodem_loop_wait_sender();
    /* The receiver waits for the empty block 0 after EOT. A second
     * start_send with an empty name sends exactly that -- a batch of one
     * file is just two start_send calls. */
    xymodem_loop_start_send(0, "");
    xymodem_loop_wait_both();

    TEST_ASSERT_EQUAL(1, xymodem_loop_file_count());
    TEST_ASSERT_EQUAL_STRING("loop.bin", xymodem_loop_file_name(0));
    TEST_ASSERT_TRUE(xymodem_loop_file_has_size(0));
    TEST_ASSERT_EQUAL(len, xymodem_loop_file_size(0));
    TEST_ASSERT_EQUAL(len, xymodem_loop_rx_len());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(xymodem_loop_tx(), xymodem_loop_rx(), len);
}

TEST_CASE("Classic sender is accepted without a header", "[ymodem]")
{
    /* Same receive config, no protocol switch: a header-less sender is
     * detected from its first data block and the session ends at EOT. */
    const size_t len = 180;
    xymodem_loop_open(XYMODEM_CHECK_CRC16, false);
    xymodem_loop_start_recv();
    xymodem_loop_start_send(len, NULL);
    xymodem_loop_wait_both();

    TEST_ASSERT_EQUAL(0, xymodem_loop_file_count());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(xymodem_loop_tx(), xymodem_loop_rx(), len);
}
