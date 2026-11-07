/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "xymodem.h"

static const char *TAG __attribute__((unused)) = "xymodem";

/**
 * The driver is a thin adapter over the sans-IO Rust core:
 *
 *   UHCI / UART DMA ISR --> xymodem_on_receive() --> ingest ring (the DMA copy)
 *   RX task             --> xymodem_on_receive() --> core.on_receive() in place
 *   your task           --> xymodem_poll()       --> drain ring + core.poll()
 *   core.emit()         --> cfg.emit()           --> UART TX  (task context)
 *   core.data_sink()    --> cfg.data_sink()      --> application
 *   core.data_source()  --> cfg.data_source()    --> application data source
 *   core.on_file()      --> cfg.on_file()        --> application
 *   core.on_event()     --> cfg.on_event()
 *
 * Called from an ISR, on_receive only copies into the ring (spinlock, no
 * mutex, no callbacks). Called from a task, it feeds the engine in place
 * so a UART FIFO path that already copied out of hardware does not copy
 * again. poll() always drains the ring first.
 */
#define XY_MAGIC 0x58594D44u /* 'XYMD' */

struct xymodem_dev_t {
    uint32_t            magic;      /**< XY_MAGIC while the handle is live. */
    SemaphoreHandle_t   lock;       /**< Serializes task-context entry points. */
    portMUX_TYPE        rx_mux;     /**< ISR-safe lock for the ingest ring. */
    uint8_t            *rx;         /**< Ingest ring: ISR writes, poll/task reads. */
    size_t              rx_cap;
    size_t              rx_head;
    size_t              rx_tail;
    size_t              rx_used;
    xymodem_core_t      core;       /**< Rust state machine (opaque, caller-owned). */
    xymodem_config_t    cfg;        /**< Copy of the caller's config. */
    xymodem_core_io_t   io;         /**< Callback table handed to the Rust core. */
    bool                sinked;     /**< data_sink callback present: we can receive. */
    bool                sourced;    /**< data_source callback present: we can send. */
};

/* Task-context APIs (poll, start, the task path of on_receive, ...) hold
 * this mutex. Callbacks fire with it held, so they must not call back into
 * the driver. The ISR path of on_receive does NOT take it. */
/* The take must NOT live inside configASSERT(): production builds often
 * define configASSERT() away, which would silently remove the lock itself. */
#define XY_LOCK(dev)   do { \
        configASSERT((dev)->lock); \
        (void)xSemaphoreTake((dev)->lock, portMAX_DELAY); \
    } while (0)
#define XY_UNLOCK(dev) (void)xSemaphoreGive((dev)->lock)

#define XY_RX_DRAIN 256

static inline bool xy_live(const struct xymodem_dev_t *dev)
{
    return dev && dev->magic == XY_MAGIC;
}

// ---- adapters: esp_err_t-returning C callbacks -> core's int callbacks ----

static int dev_emit(void *user, const uint8_t *data, size_t len)
{
    struct xymodem_dev_t *dev = user;
    return (dev->cfg.emit(dev, dev->cfg.user_ctx, data, len) == ESP_OK) ? 0 : -1;
}

static int dev_data_sink(void *user, const uint8_t *data, size_t len)
{
    struct xymodem_dev_t *dev = user;
    if (!dev->cfg.data_sink) {
        return 0;
    }
    return (dev->cfg.data_sink(dev, dev->cfg.user_ctx, data, len) == ESP_OK) ? 0 : -1;
}

static int dev_data_source(void *user, uint8_t *buf, size_t len, size_t *written)
{
    struct xymodem_dev_t *dev = user;
    size_t n = 0;
    if (!dev->cfg.data_source ||
            dev->cfg.data_source(dev, dev->cfg.user_ctx, buf, len, &n) != ESP_OK) {
        return -1;
    }
    if (written) {
        *written = n;
    }
    return 0;
}

static bool dev_on_file(void *user, const char *name, uint64_t size, bool has_size)
{
    struct xymodem_dev_t *dev = user;
    if (!dev->cfg.on_file) {
        return true; /* accepted */
    }
    /* Both the public callback and the core ABI use `bool` (true = accept),
     * so this is a straight pass-through -- no bool -> int -> bool inversion.
     * Keep `size` as uint64_t: casting to size_t would truncate files > 4 GiB
     * on 32-bit targets. */
    return dev->cfg.on_file(dev, dev->cfg.user_ctx, name, size, has_size);
}

static void dev_event(void *user, xymodem_event_t event, xymodem_err_t arg)
{
    struct xymodem_dev_t *dev = user;
    if (dev->cfg.on_event) {
        dev->cfg.on_event(dev, dev->cfg.user_ctx, event, arg);
    }
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/** One place that turns a core return code into an esp_err_t, so every entry
 *  point reports the same condition the same way. */
static esp_err_t core_rc_to_esp(xymodem_err_t rc)
{
    switch (rc) {
    case XYMODEM_OK:
        return ESP_OK;
    case XYMODEM_ERR_BUSY:
        return ESP_ERR_INVALID_STATE; /* a session is still in flight */
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void IRAM_ATTR rx_push(struct xymodem_dev_t *dev, const uint8_t *src, size_t n)
{
    size_t cap = dev->rx_cap;
    size_t head = dev->rx_head;
    size_t first = cap - head;
    if (first > n) {
        first = n;
    }
    memcpy(dev->rx + head, src, first);
    if (n > first) {
        memcpy(dev->rx, src + first, n - first);
    }
    dev->rx_head = (head + n) % cap;
    dev->rx_used += n;
}

static size_t rx_pop(struct xymodem_dev_t *dev, uint8_t *dst, size_t max)
{
    size_t n = dev->rx_used < max ? dev->rx_used : max;
    if (n == 0) {
        return 0;
    }
    size_t cap = dev->rx_cap;
    size_t tail = dev->rx_tail;
    size_t first = cap - tail;
    if (first > n) {
        first = n;
    }
    memcpy(dst, dev->rx + tail, first);
    if (n > first) {
        memcpy(dst + first, dev->rx, n - first);
    }
    dev->rx_tail = (tail + n) % cap;
    dev->rx_used -= n;
    return n;
}

static esp_err_t IRAM_ATTR rx_ingest(struct xymodem_dev_t *dev, const uint8_t *data, size_t len)
{
    portENTER_CRITICAL_SAFE(&dev->rx_mux);
    esp_err_t err = ESP_OK;
    if (dev->rx_used + len > dev->rx_cap) {
        err = ESP_ERR_NO_MEM;
    } else {
        rx_push(dev, data, len);
    }
    portEXIT_CRITICAL_SAFE(&dev->rx_mux);
    return err;
}

static void rx_flush(struct xymodem_dev_t *dev)
{
    portENTER_CRITICAL_SAFE(&dev->rx_mux);
    dev->rx_head = 0;
    dev->rx_tail = 0;
    dev->rx_used = 0;
    portEXIT_CRITICAL_SAFE(&dev->rx_mux);
}

/** Caller holds XY_LOCK. Drains ISR-ingested bytes into the engine. */
static void drain_rx_locked(struct xymodem_dev_t *dev)
{
    uint8_t tmp[XY_RX_DRAIN];
    for (;;) {
        portENTER_CRITICAL_SAFE(&dev->rx_mux);
        size_t n = rx_pop(dev, tmp, sizeof(tmp));
        portEXIT_CRITICAL_SAFE(&dev->rx_mux);
        if (n == 0) {
            break;
        }
        (void)xymodem_core_on_receive(&dev->core, tmp, n, now_ms());
    }
}

static esp_err_t xymodem_on_receive_task(struct xymodem_dev_t *dev, const uint8_t *data, size_t len)
{
    XY_LOCK(dev);
    drain_rx_locked(dev); /* preserve order if the ISR also posted bytes */
    xymodem_err_t rc = xymodem_core_on_receive(&dev->core, data, len, now_ms());
    XY_UNLOCK(dev);
    return core_rc_to_esp(rc);
}

esp_err_t xymodem_new(const xymodem_config_t *config, xymodem_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config && out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->emit, ESP_ERR_INVALID_ARG, TAG, "emit callback is required");
    /* The direction is the callbacks' business: no callback, no transfer. */
    ESP_RETURN_ON_FALSE(config->data_sink || config->data_source, ESP_ERR_INVALID_ARG, TAG,
                        "need data_sink and/or data_source callback");
    /* Protocol tuning (check mode, timeouts) is validated in exactly one
     * place: xymodem_core_init()/to_config(). Duplicating those rules here
     * would drift -- e.g. a stricter C-side limit that rejects names the
     * engine would accept. XYMODEM_ERR_INVALID_ARG maps to ESP_ERR_INVALID_ARG. */

    struct xymodem_dev_t *dev = calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "no memory for handle");
    dev->lock = xSemaphoreCreateMutex();
    if (dev->lock == NULL) {
        free(dev);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "no memory for handle lock");
    }
    dev->rx_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    dev->rx_cap = config->rx_buf_size ? config->rx_buf_size : XYMODEM_RX_BUF_DEFAULT;
    /* Internal RAM: the ring is written from a UART / UHCI ISR. */
    dev->rx = heap_caps_malloc(dev->rx_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dev->rx == NULL) {
        vSemaphoreDelete(dev->lock);
        free(dev);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "no memory for ingest ring");
    }
    dev->cfg = *config;
    dev->io.emit = dev_emit;
    dev->io.data_sink = dev_data_sink;
    dev->io.data_source = dev_data_source;
    dev->io.on_file = dev_on_file;
    dev->io.on_event = dev_event;
    dev->io.user = dev;
    dev->sinked = config->data_sink != NULL;
    dev->sourced = config->data_source != NULL;

    /* The core starts idle: no handshake byte, no timer. The direction and
     * the optional block 0 are the caller's per-session choices. */
    if (xymodem_core_init(&dev->core, &config->core, &dev->io) != XYMODEM_OK) {
        free(dev->rx);
        vSemaphoreDelete(dev->lock);
        free(dev);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_ARG, TAG, "core init rejected the config");
    }
    dev->magic = XY_MAGIC;
    *out_handle = dev;
    return ESP_OK;
}

esp_err_t xymodem_start_recv(xymodem_handle_t handle)
{
    ESP_RETURN_ON_FALSE(xy_live(handle), ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    struct xymodem_dev_t *dev = handle;
    ESP_RETURN_ON_FALSE(dev->sinked, ESP_ERR_INVALID_STATE, TAG, "no data_sink callback: cannot receive");
    XY_LOCK(dev);
    if (dev->magic != XY_MAGIC) {
        XY_UNLOCK(dev);
        return ESP_ERR_INVALID_ARG;
    }
    rx_flush(dev); /* drop leftover ISR bytes from the previous session */
    xymodem_err_t rc = xymodem_core_start_recv(&dev->core, now_ms());
    XY_UNLOCK(dev);
    return core_rc_to_esp(rc);
}

esp_err_t xymodem_start_send(xymodem_handle_t handle, const xymodem_file_info_t *file)
{
    ESP_RETURN_ON_FALSE(xy_live(handle), ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    struct xymodem_dev_t *dev = handle;
    ESP_RETURN_ON_FALSE(dev->sourced, ESP_ERR_INVALID_STATE, TAG, "no data_source callback: cannot send");
    ESP_RETURN_ON_FALSE(!file || file->name, ESP_ERR_INVALID_ARG, TAG, "invalid file info");
    /* NULL = classic, empty name = batch terminator, anything else = named
     * file. The engine infers the header from that data; there is no protocol
     * / kind argument. Name validity (NUL-free, name + decimal size <= 128)
     * belongs to the engine. */
    XY_LOCK(dev);
    if (dev->magic != XY_MAGIC) {
        XY_UNLOCK(dev);
        return ESP_ERR_INVALID_ARG;
    }
    rx_flush(dev);
    xymodem_core_file_info_t core_file;
    if (file) {
        core_file.name = file->name;
        core_file.size = file->size;
    }
    xymodem_err_t rc = xymodem_core_start_send(&dev->core, file ? &core_file : NULL, now_ms());
    XY_UNLOCK(dev);
    return core_rc_to_esp(rc);
}

esp_err_t IRAM_ATTR xymodem_on_receive(xymodem_handle_t handle, const uint8_t *data, size_t len)
{
    if (!xy_live(handle) || (len > 0 && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (xPortInIsrContext()) {
        return rx_ingest(handle, data, len);
    }
    return xymodem_on_receive_task(handle, data, len);
}

esp_err_t xymodem_poll(xymodem_handle_t handle)
{
    ESP_RETURN_ON_FALSE(xy_live(handle), ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    struct xymodem_dev_t *dev = handle;
    XY_LOCK(dev);
    if (dev->magic != XY_MAGIC) {
        XY_UNLOCK(dev);
        return ESP_ERR_INVALID_ARG;
    }
    drain_rx_locked(dev);
    xymodem_err_t rc = xymodem_core_poll(&dev->core, now_ms());
    XY_UNLOCK(dev);
    return core_rc_to_esp(rc);
}

xymodem_status_t xymodem_status(xymodem_handle_t handle)
{
    if (!xy_live(handle)) {
        return XYMODEM_ST_INVALID;
    }
    XY_LOCK(handle);
    if (handle->magic != XY_MAGIC) {
        XY_UNLOCK(handle);
        return XYMODEM_ST_INVALID;
    }
    xymodem_status_t status = xymodem_core_status(&handle->core);
    XY_UNLOCK(handle);
    return status;
}

xymodem_err_t xymodem_error(xymodem_handle_t handle)
{
    if (!xy_live(handle)) {
        return XYMODEM_ERR_INVALID_ARG;
    }
    XY_LOCK(handle);
    if (handle->magic != XY_MAGIC) {
        XY_UNLOCK(handle);
        return XYMODEM_ERR_INVALID_ARG;
    }
    xymodem_err_t err = xymodem_core_error(&handle->core);
    XY_UNLOCK(handle);
    return err;
}

xymodem_role_t xymodem_role(xymodem_handle_t handle)
{
    if (!xy_live(handle)) {
        return XYMODEM_ROLE_NONE;
    }
    XY_LOCK(handle);
    if (handle->magic != XY_MAGIC) {
        XY_UNLOCK(handle);
        return XYMODEM_ROLE_NONE;
    }
    xymodem_role_t role = xymodem_core_role(&handle->core);
    XY_UNLOCK(handle);
    return role;
}

esp_err_t xymodem_cancel(xymodem_handle_t handle)
{
    ESP_RETURN_ON_FALSE(xy_live(handle), ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    XY_LOCK(handle);
    if (handle->magic != XY_MAGIC) {
        XY_UNLOCK(handle);
        return ESP_ERR_INVALID_ARG;
    }
    (void)xymodem_core_cancel(&handle->core);
    XY_UNLOCK(handle);
    return ESP_OK;
}

esp_err_t xymodem_delete(xymodem_handle_t handle)
{
    ESP_RETURN_ON_FALSE(xy_live(handle), ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    /* Clear the magic first so a concurrent status/poll sees ST_INVALID /
     * INVALID_ARG instead of a freed mutex. Callers must still stop the
     * UHCI ISR and any other task that holds this handle before delete. */
    XY_LOCK(handle);
    handle->magic = 0;
    XY_UNLOCK(handle);
    free(handle->rx);
    vSemaphoreDelete(handle->lock);
    free(handle); // core has no Drop / heap allocations of its own
    return ESP_OK;
}
