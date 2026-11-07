/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * UART0/UART1 GPIO-matrix loopback for the event-driven driver. Protocol
 * logic (retries, check-mode fallback, trimming, ...) lives in the Rust
 * #[cfg(test)] suite; this fixture only puts two C handles on real UARTs.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "xymodem.h"

#define XYMODEM_LOOP_RX_CAP 4096
#define XYMODEM_LOOP_SUB    0x1A

/** Install the crossed UART pair. Idempotent; call from setUp(). */
void xymodem_loop_uart_init(void);

/** Drop any bytes left in the UART driver buffers. */
void xymodem_loop_flush(void);

/**
 * Create both drivers and the RX-pump / poll tasks. Does not start a session.
 * check_mode / block_1k go into both configs. There is no protocol setting:
 * a session's optional block 0 is chosen in xymodem_loop_start_send().
 */
void xymodem_loop_open(uint8_t check_mode, bool block_1k);

/** Stop the tasks and delete the handles. Safe if nothing is open. */
void xymodem_loop_close(void);

/** Reset the receive buffer and file list, then xymodem_start_recv(). */
void xymodem_loop_start_recv(void);

/**
 * Fill the source with a deterministic pattern of `len` bytes and start the
 * sender. `name == NULL` is classic (header-less); a non-NULL name sends
 * block 0 with that name and size `len`. An empty name sends the batch
 * terminator (no payload).
 */
void xymodem_loop_start_send(size_t len, const char *name);

/** Wait until the sender reaches XYMODEM_ST_COMPLETE. */
void xymodem_loop_wait_sender(void);

/** Wait until both ends reach XYMODEM_ST_COMPLETE. */
void xymodem_loop_wait_both(void);

xymodem_handle_t xymodem_loop_sender(void);
xymodem_handle_t xymodem_loop_receiver(void);

const uint8_t *xymodem_loop_tx(void);
size_t xymodem_loop_tx_len(void);
const uint8_t *xymodem_loop_rx(void);
size_t xymodem_loop_rx_len(void);

int xymodem_loop_file_count(void);
const char *xymodem_loop_file_name(int index);
size_t xymodem_loop_file_size(int index);
bool xymodem_loop_file_has_size(int index);

/** Classic XMODEM delivers whole blocks; round `payload_len` up to 128 or 1024. */
size_t xymodem_loop_padded_len(size_t payload_len, bool block_1k);
