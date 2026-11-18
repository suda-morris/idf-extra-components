/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "xymodem_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event-driven XMODEM driver handle.
 *
 * The driver owns no hardware. It wraps the pure Rust protocol state machine
 * and drives it from the caller's task; the caller supplies the physical link
 * and the payload through callbacks. See xymodem_config_t.
 */
typedef struct xymodem_dev_t *xymodem_handle_t;

/**
 * @brief Push protocol bytes onto the physical link (UART TX, ...).
 *
 * Called from xymodem_on_receive() (task path), xymodem_poll(), or
 * xymodem_start_*() — always in task context, never from the ISR path of
 * on_receive. Must not block for a response; only queue/transmit the bytes.
 * Must not call back into the driver: the instance lock is held.
 *
 * @param[in] handle The instance (`this`). Same pointer as every public API.
 * @param[in] ctx    User context from xymodem_config_t::user_ctx.
 * @param[in] data   Bytes to transmit.
 * @param[in] len    Number of bytes.
 * @return ESP_OK on success; any other value aborts the transfer with
 *         XYMODEM_ERR_IO (both the event callback and xymodem_error() report
 *         that same code) and emits CAN to the peer.
 */
typedef esp_err_t (*xymodem_emit_fn_t)(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len);

/**
 * @brief Data sink: hand one accepted data block's payload to the application.
 *
 * Called only for blocks that passed the checksum/CRC check and were not
 * duplicates. XMODEM cannot distinguish payload from trailing 0x1A padding,
 * so the whole block (padding included) is handed over, matching classic
 * XMODEM semantics. When the peer sent a file size, payload is trimmed to
 * exactly that size instead.
 *
 * @param[in] handle The instance (`this`).
 * @param[in] ctx    User context.
 * @param[in] data   Payload bytes (valid only during the call).
 * @param[in] len    Number of bytes.
 * @return ESP_OK on success; any other value aborts the transfer with
 *         XYMODEM_ERR_IO.
 */
typedef esp_err_t (*xymodem_data_sink_fn_t)(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len);

/**
 * @brief Data source: pull the next chunk of payload to transmit (sender role only).
 *
 * @param[in]  handle    The instance (`this`).
 * @param[in]  ctx       User context.
 * @param[out] buf       Buffer to fill.
 * @param[in]  len       Capacity of buf.
 * @param[out] read_len  Number of bytes actually written (0 = end of data).
 * @return ESP_OK on success; any other value aborts the transfer with
 *         XYMODEM_ERR_IO.
 */
typedef esp_err_t (*xymodem_data_source_fn_t)(xymodem_handle_t handle, void *ctx, uint8_t *buf, size_t len, size_t *read_len);

/**
 * @brief A block 0 (file header) was verified on receive.
 *
 * Called before any payload is delivered, so this is the place to open the
 * target file, check the size against your storage, or refuse unknown files.
 * Return false to reject: the transfer is cancelled with CAN and reported as
 * XYMODEM_ERR_FILE_REJECTED (the file was refused, not an I/O failure).
 * Classic (header-less) senders never emit block 0, so this is simply never
 * called for them.
 *
 * @param[in] handle    The instance (`this`).
 * @param[in] ctx       User context.
 * @param[in] name      File name (NUL-terminated; valid only during the call).
 * @param[in] size      Declared file size; meaningless when has_size is false.
 * @param[in] has_size  Whether the header carried a size field. Without one,
 *                      payload is delivered whole (classic semantics, SUB
 *                      padding included) and the exact file length is unknown.
 * @return true to accept the file; false rejects it and cancels the transfer.
 */
typedef bool (*xymodem_on_file_fn_t)(xymodem_handle_t handle, void *ctx, const char *name, uint64_t size, bool has_size);

/**
 * @brief File metadata for xymodem_start_send().
 *
 * The name is copied by the driver; the caller's buffer may be transient. It
 * must be NUL-terminated and, when non-empty, fit a 128-byte header block
 * together with the decimal size; the engine validates this and returns
 * ESP_ERR_INVALID_ARG if it does not. An empty name sends the empty block 0
 * that terminates a YMODEM batch (no payload) — that empty pathname is the
 * YMODEM wire token, not a separate protocol mode.
 */
typedef struct {
    const char *name;  /**< File name sent in block 0 (NUL-terminated). */
    uint64_t    size;  /**< Exact file size in bytes; 0 sends an empty file.
                        *   uint64_t matches xymodem_on_file_fn_t and the wire
                        *   header, so sizes > 4 GiB survive a 32-bit build. */
} xymodem_file_info_t;

/**
 * @brief Lifecycle notification (optional).
 *
 * @param[in] handle The instance (`this`) reporting the event.
 * @param[in] ctx    User context from xymodem_config_t::user_ctx.
 * @param[in] event  One of XYMODEM_EV_*.
 *                   XYMODEM_EV_FILE_COMPLETE fires at each YMODEM file EOT
 *                   (the session stays open for the next header);
 *                   XYMODEM_EV_COMPLETE still marks the whole batch / classic
 *                   transfer. The list is deliberately short: block arrivals
 *                   are exactly the data_sink calls, payload pulls are
 *                   exactly the data_source calls, a block-0 header is
 *                   on_file.
 * @param[in] arg    On XYMODEM_EV_FAILED, a negative XYMODEM_ERR_* code;
 *                   0 otherwise. Progress is not an event parameter: count
 *                   the bytes in your own data_sink / data_source callbacks
 *                   (they fire exactly once per accepted/pulled chunk) and
 *                   reset the counter in on_file for per-file progress.
 */
typedef void (*xymodem_event_fn_t)(xymodem_handle_t handle, void *ctx, xymodem_event_t event, xymodem_err_t arg);

/**
 * @brief Configuration for xymodem_new().
 *
 * The driver is deliberately transport-agnostic: it never calls a blocking
 * recv(). Feed bytes with xymodem_on_receive():
 *   - From a UHCI / UART DMA ISR: copies the DMA buffer into an ingest ring.
 *     That copy is the one you would do anyway before the descriptor is
 *     recycled; the protocol runs later in xymodem_poll().
 *   - From a task (uart_read_bytes, a unit-test buffer): feeds the slice
 *     in place, no extra copy. emit / data_sink may run before it returns.
 *
 * The I/O callbacks form a 2x2 matrix over two planes and two directions:
 *
 *                 incoming (peer -> app)    outgoing (app -> peer)
 *   wire bytes    xymodem_on_receive()      emit()
 *   payload       data_sink()               data_source()
 *
 * "wire" bytes are framed protocol traffic (SOH/STX, block numbers, CRC);
 * "payload" is the file data itself, which the engine strips from (receive)
 * or wraps into (send) that framing.
 *
 * XMODEM and YMODEM are not two modes: one session is classic XMODEM plus
 * one optional block-0 file header. The receiver recognises a header-ful
 * sender automatically (and accepts classic header-less senders just as
 * well); the sender opts into a header by passing xymodem_file_info_t to
 * xymodem_start_send(). No protocol field, no protocol-specific callbacks.
 *
 * Every callback's first argument is the handle — the instance, the `this`
 * of this object — then user_ctx. A UART emit that ignores the handle today
 * still receives it, so a later getter or a shared callback for several
 * instances does not need a new signature. Do not call back into the driver
 * from a callback: the instance lock is held.
 *
 * The callbacks declare what the application CAN do; the direction of each
 * session is chosen explicitly when it starts:
 *   - xymodem_start_recv() requires data_sink: it emits the handshake request
 *     immediately and hands every accepted block's payload to it.
 *   - xymodem_start_send() requires data_source: it waits for the peer's
 *     request byte, then pulls payload from it block by block.
 * Sessions are independent on the wire (block numbering restarts at 1), so an
 * echo server simply alternates the two calls; a device that only ever
 * receives can omit data_source altogether.
 */
typedef struct {
    xymodem_core_config_t    core;       /**< Protocol tuning (check mode, timeouts). */
    xymodem_emit_fn_t        emit;       /**< Required: push bytes onto the wire. */
    xymodem_data_sink_fn_t   data_sink;  /**< Receive path; at least one of data_sink/data_source is required. */
    xymodem_data_source_fn_t data_source;/**< Send path; at least one of data_sink/data_source is required. */
    xymodem_on_file_fn_t     on_file;    /**< Receive path: block 0 accept/reject (may be NULL). */
    xymodem_event_fn_t       on_event;   /**< Optional lifecycle callback (may be NULL). */
    void                    *user_ctx;   /**< Opaque pointer passed to every callback. */
    size_t                   rx_buf_size;/**< Ingest ring for the ISR path of xymodem_on_receive(). 0 = 4096. */
} xymodem_config_t;

/** Default xymodem_config_t::rx_buf_size when that field is 0. */
#define XYMODEM_RX_BUF_DEFAULT 4096

/** Default core tuning: CRC-auto (three 'C's then SUM8), 128-byte blocks,
 *  10 retries, 3s/30s timeouts. `block_1k` senders drop the last block to
 *  SOH/128 when it is at most 128 bytes. */
#define XYMODEM_CORE_CONFIG_DEFAULT() { \
    .check_mode = XYMODEM_CHECK_AUTO, \
    .block_1k = false, \
    .max_retries = 10, \
    .packet_timeout_ms = 3000, \
    .response_timeout_ms = 30000, \
}

/**
 * @brief Create a driver instance. The handle is reusable for many transfers.
 *
 * The driver starts idle: nothing is emitted and no timer runs until the
 * first xymodem_start_recv() / xymodem_start_send(). A session that completed,
 * failed or was cancelled can simply be followed by the next one. Create one
 * handle at init and keep it for the lifetime of your task.
 *
 * @param[in]  config  Driver configuration (copied by value).
 * @param[out] out_handle  Returned handle.
 * @return
 *   - ESP_OK on success.
 *   - ESP_ERR_INVALID_ARG for missing/NULL callbacks or a bad config.
 *   - ESP_ERR_NO_MEM if the handle cannot be allocated.
 */
esp_err_t xymodem_new(const xymodem_config_t *config, xymodem_handle_t *out_handle);

/**
 * @brief Start a session as the receiving side.
 *
 * Requires xymodem_config_t::data_sink. Resets the protocol state, the block
 * counter and the negotiated check mode, then emits the handshake request
 * ('C' or NAK) right away; feed received bytes in with xymodem_on_receive().
 * A peer that sends block 0 (file name/size) is recognised automatically; a
 * classic header-less sender is detected from its first data block. The
 * application's own buffers are untouched -- reset those yourself.
 *
 * Typical echo server (see examples/xymodem_event). All start calls happen in
 * the task loop, never inside a callback: callbacks fire with the driver lock
 * held, so calling back into xymodem_start_*() from on_event would deadlock
 * on the non-recursive mutex.
 *
 *     xymodem_new(&cfg, &h);
 *     xymodem_start_recv(h);          // receive the upload
 *     for (;;) {
 *         vTaskDelay(pdMS_TO_TICKS(100));
 *         xymodem_status_t st = xymodem_status(h);
 *         if (st == XYMODEM_ST_COMPLETE && receiving) {
 *             xymodem_start_send(h, NULL);   // echo the kept data back
 *             receiving = false;
 *         } else if (st == XYMODEM_ST_COMPLETE) {
 *             xymodem_start_recv(h);        // ready for the next upload
 *             receiving = true;
 *         } else if (st == XYMODEM_ST_FAILED) {
 *             xymodem_start_recv(h);        // retry
 *             receiving = true;
 *         }
 *     }
 *
 * A session already in flight is NOT silently abandoned: the call returns
 * ESP_ERR_INVALID_STATE. Wait for XYMODEM_EV_COMPLETE / XYMODEM_EV_FAILED (or
 * poll xymodem_status()) before starting the next one.
 *
 * @param[in] handle  Driver handle.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a NULL handle, or
 *         ESP_ERR_INVALID_STATE when no data_sink callback was configured or
 *         a session is still in flight.
 */
esp_err_t xymodem_start_recv(xymodem_handle_t handle);

/**
 * @brief Start a session as the sending side.
 *
 * Requires xymodem_config_t::data_source. Resets the protocol state, the
 * block counter and the negotiated check mode, then waits for the peer's
 * request byte (XMODEM lets the receiver talk first); payload is then pulled
 * from data_source block by block.
 *
 * @p file selects the session's optional block 0:
 *   - NULL: classic XMODEM, header-less (no file name/size on the wire).
 *   - non-NULL: block 0 carries file->name and file->size first (YMODEM);
 *     payload is then pulled from data_source. An empty file->name sends the
 *     empty block 0 that terminates a YMODEM batch, with no payload.
 *
 * A YMODEM batch (several files over one wire session) is just several
 * xymodem_start_send() calls with one file each, followed by one empty-name
 * terminator. The library keeps no batch bookkeeping: each call waits for the
 * receiver's fresh 'C', which is exactly the YMODEM batch wire timing. Call
 * xymodem_start_send() again only after the previous session completed.
 *
 * As with xymodem_start_recv(), starting on top of an in-flight session is
 * rejected with ESP_ERR_INVALID_STATE rather than silently dropping it.
 *
 * @param[in] handle  Driver handle.
 * @param[in] file    Optional file metadata (NULL = classic XMODEM).
 * @return ESP_OK, ESP_ERR_INVALID_ARG for a NULL handle or a name that does
 *         not fit the header block, ESP_ERR_INVALID_STATE when no data_source
 *         callback was configured or a session is still in flight.
 */
esp_err_t xymodem_start_send(xymodem_handle_t handle, const xymodem_file_info_t *file);

/**
 * @brief Feed bytes received from the hardware into the protocol state machine.
 *
 * Call this from either context; the copy happens only when it has to:
 *
 *   - UHCI / UART DMA ISR: copies `data` into an ingest ring (spinlock, no
 *     mutex, no callbacks). This is the copy-out-of-DMA you would do before
 *     the descriptor is recycled. Protocol work runs in xymodem_poll().
 *     A full ring returns ESP_ERR_NO_MEM (raise rx_buf_size or poll more
 *     often).
 *   - Task (`uart_read_bytes`, a unit-test buffer): feeds the caller's
 *     slice in place. The UART FIFO driver already copied out of hardware;
 *     this path does not copy again. emit / data_sink may run before return.
 *
 * Any slice is valid -- half a packet, several packets, a stray ACK mixed
 * with data. Time is taken from esp_timer, so no clock is passed in.
 *
 * @param[in] handle  Driver handle.
 * @param[in] data    Received bytes (may be NULL when len == 0).
 * @param[in] len     Number of bytes.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM (ISR path, ring full).
 */
esp_err_t xymodem_on_receive(xymodem_handle_t handle, const uint8_t *data, size_t len);

/**
 * @brief Drive the protocol state machine (timeouts, retransmits).
 *
 * Call from the same loop that reads the UART (the read timeout *is* the
 * poll period), never from an ISR. Drains bytes ingested from an ISR, then
 * advances timers. A UHCI-only setup needs no RX task: the ISR calls
 * on_receive, this call runs the protocol.
 *
 * @param[in] handle  Driver handle.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG for a NULL handle.
 */
esp_err_t xymodem_poll(xymodem_handle_t handle);

/**
 * @brief Current transfer status (XYMODEM_ST_*).
 * @return A status code, or XYMODEM_ST_INVALID for a NULL / deleted handle.
 */
xymodem_status_t xymodem_status(xymodem_handle_t handle);

/**
 * @brief Failure reason (XYMODEM_ERR_*) when failed, XYMODEM_OK otherwise.
 *        XYMODEM_ERR_INVALID_ARG for a NULL / deleted handle.
 */
xymodem_err_t xymodem_error(xymodem_handle_t handle);

/**
 * @brief Abort a transfer from the application side.
 *
 * Emits CAN bytes to the peer and reports XYMODEM_ERR_CANCEL_BY_LOCAL through
 * the event callback and xymodem_error(). A peer-initiated cancel is reported
 * as XYMODEM_ERR_CANCEL_BY_PEER instead. No-op once the session is terminal.
 */
esp_err_t xymodem_cancel(xymodem_handle_t handle);

/** Tear down a driver instance and release its memory. */
esp_err_t xymodem_delete(xymodem_handle_t handle);

#ifdef __cplusplus
}
#endif
