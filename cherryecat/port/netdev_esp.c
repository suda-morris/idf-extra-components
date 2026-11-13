/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT ESP-IDF netdev low-level port.
 *
 * Bridges the stack's pull-model netdev interface (ec_port.h) onto the
 * ESP-IDF Ethernet driver through the L2 TAP interface (esp_vfs_l2tap):
 *  - The esp_eth sublayer (esp_eth_sublayer.h) takes over the driver's
 *    input path and feeds every received frame into the L2 TAP filter.
 *  - One L2 TAP file descriptor per netdev slot, bound to the Ethernet
 *    driver handle and filtered on EtherType 0x88A4: the filtering happens
 *    in the Ethernet driver receive path, the descriptor only ever sees
 *    EtherCAT frames (frames of any other type never enter the queue).
 *  - TX: statically staged frame buffers with prefilled Ethernet headers,
 *    handed to esp_eth_transmit() (non-blocking on the internal EMAC).
 *  - RX pull model: get_rxbuf() reads one frame from the L2 TAP descriptor
 *    (opened O_NONBLOCK: EAGAIN when the queue is empty), release_rxbuf()
 *    frees the frame buffer. The L2 TAP receive queue is managed by
 *    esp_netif (ESP_NETIF_L2_TAP_RX_QUEUE_SIZE, tail drop).
 *  - PHY: esp_eth owns the PHY (CONFIG_EC_PHY_CUSTOM), so the port only
 *    mirrors the link state reported through ETH_EVENT into the netdev.
 *
 * Requires the experimental Ethernet sublayer API: menuconfig
 * "Support Ethernet netif sublayer" (CONFIG_ETH_SUBLAYER_SUPPORT), which
 * needs CONFIG_IDF_EXPERIMENTAL_FEATURES. The component Kconfig selects
 * CONFIG_ESP_NETIF_L2_TAP, which in turn enables the sublayer IO driver
 * provider needed for the descriptor binding. */

#include "ec_master.h"
#include "esp_cherryecat.h"

#include "esp_eth_sublayer.h"
#include "esp_vfs_l2tap.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if !CONFIG_ETH_SUBLAYER_SUPPORT
#error "The CherryECAT netdev port requires the experimental esp_eth sublayer API. Enable CONFIG_IDF_EXPERIMENTAL_FEATURES and CONFIG_ETH_SUBLAYER_SUPPORT (menuconfig: Component config → Ethernet → Support Ethernet netif sublayer)."
#endif

#if !CONFIG_ESP_NETIF_L2_TAP
#error "The CherryECAT netdev port requires the L2 TAP interface (CONFIG_ESP_NETIF_L2_TAP), normally selected by the component Kconfig."
#endif

#define EC_NETDEV_ETHERTYPE_ECAT 0x88A4
#define EC_NETDEV_TXBUF_SIZE     1536
#define EC_NETDEV_RXBUF_SIZE     2048

typedef struct {
    esp_eth_handle_t eth_handle;
    esp_eth_sublayer_handle_t sublayer;
    int l2tap_fd;
    bool bound;
    bool link_up;
    uint16_t speed_mbps;
    bool full_duplex;
} ec_netdev_esp_state_t;

typedef struct {
    uint8_t *buffer;
    uint32_t length;
} ec_netdev_esp_rx_item_t;

static ec_netdev_t s_netdevs[CONFIG_EC_MAX_NETDEVS];
static ec_netdev_esp_state_t s_states[CONFIG_EC_MAX_NETDEVS];

static ec_netdev_esp_rx_item_t s_rx_inflight[CONFIG_EC_MAX_NETDEVS];

static uint8_t s_txbuf[CONFIG_EC_MAX_NETDEVS][CONFIG_CHERRYECAT_TXBUF_COUNT][EC_NETDEV_TXBUF_SIZE];

static bool s_l2tap_vfs_registered;
static bool s_event_handler_registered;

static void ec_netdev_esp_event_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    for (uint8_t idx = 0; idx < CONFIG_EC_MAX_NETDEVS; idx++) {
        if (s_states[idx].eth_handle != eth_handle) {
            continue;
        }

        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            eth_speed_t speed = ETH_SPEED_10M;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;

            (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
            (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);

            s_states[idx].link_up = true;
            s_states[idx].speed_mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
            s_states[idx].full_duplex = (duplex == ETH_DUPLEX_FULL);
            EC_LOG_INFO("ec-netdev%u: link up (%u Mbps, %s duplex)\n",
                        idx, s_states[idx].speed_mbps,
                        s_states[idx].full_duplex ? "full" : "half");
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            s_states[idx].link_up = false;
            EC_LOG_INFO("ec-netdev%u: link down\n", idx);
            break;
        default:
            break;
        }
        break;
    }
}

ec_netdev_t *ec_netdev_low_level_init(uint8_t netdev_index)
{
    ec_netdev_esp_state_t *state;
    ec_netdev_t *netdev;

    if (netdev_index >= CONFIG_EC_MAX_NETDEVS) {
        return NULL;
    }

    state = &s_states[netdev_index];
    if (!state->bound) {
        EC_LOG_ERR("ec-netdev%u: no Ethernet driver attached, call esp_cherryecat_attach_netdev() before ec_master_init()\n",
                   netdev_index);
        return NULL;
    }

    netdev = &s_netdevs[netdev_index];

    (void)esp_eth_ioctl(state->eth_handle, ETH_CMD_G_MAC_ADDR, netdev->mac_addr);

    /* Prefill the invariant Ethernet header fields of every TX slot:
       broadcast destination, our source MAC and EtherType 0x88A4. */
    for (uint32_t i = 0; i < CONFIG_CHERRYECAT_TXBUF_COUNT; i++) {
        memset(s_txbuf[netdev_index][i], 0xFF, 6);
        memcpy(&s_txbuf[netdev_index][i][6], netdev->mac_addr, 6);
        s_txbuf[netdev_index][i][12] = (EC_NETDEV_ETHERTYPE_ECAT >> 8) & 0xFF;
        s_txbuf[netdev_index][i][13] = EC_NETDEV_ETHERTYPE_ECAT & 0xFF;
    }

    netdev->link_state = state->link_up;
    return netdev;
}

void ec_netdev_low_level_poll_link_state(ec_netdev_t *netdev)
{
    ec_netdev_esp_state_t *state = &s_states[netdev->index];

    /* esp_eth owns the PHY: the link state tracked through ETH_EVENT is
       authoritative, just mirror it into the netdev. */
    netdev->link_state = state->link_up;
    netdev->phy_status.link = state->link_up;
    netdev->phy_status.speed = state->speed_mbps;
    netdev->phy_status.duplex = state->full_duplex;
}

EC_FAST_CODE_SECTION uint8_t *ec_netdev_low_level_get_txbuf(ec_netdev_t *netdev)
{
    return s_txbuf[netdev->index][netdev->tx_frame_index];
}

EC_FAST_CODE_SECTION int ec_netdev_low_level_output(ec_netdev_t *netdev, uint32_t size)
{
    esp_eth_handle_t eth = s_states[netdev->index].eth_handle;

    if (size > EC_NETDEV_TXBUF_SIZE) {
        return -1;
    }

    /* esp_eth_transmit() copies the frame into the DMA buffers and never
       blocks; when all TX descriptors are still owned by the DMA it fails
       fast, which the stack accounts as a transmit error. */
    if (esp_eth_transmit(eth, s_txbuf[netdev->index][netdev->tx_frame_index], size) != ESP_OK) {
        return -1;
    }

    netdev->tx_frame_index++;
    netdev->tx_frame_index %= CONFIG_CHERRYECAT_TXBUF_COUNT;
    return 0;
}

EC_FAST_CODE_SECTION int ec_netdev_low_level_get_rxbuf(ec_netdev_t *netdev, uint8_t **buffer, uint32_t *size)
{
    ec_netdev_esp_state_t *state = &s_states[netdev->index];
    ec_netdev_esp_rx_item_t *inflight = &s_rx_inflight[netdev->index];
    ssize_t frame_len;

    inflight->buffer = malloc(EC_NETDEV_RXBUF_SIZE);
    if (inflight->buffer == NULL) {
        return -1;
    }

    /* The descriptor was opened O_NONBLOCK: read() returns -1/EAGAIN when
       no (filtered) frame is queued. */
    frame_len = read(state->l2tap_fd, inflight->buffer, EC_NETDEV_RXBUF_SIZE);
    if (frame_len <= 0) {
        free(inflight->buffer);
        inflight->buffer = NULL;
        return -1;
    }

    inflight->length = (uint32_t)frame_len;
    *buffer = inflight->buffer;
    *size = inflight->length;
    return 0;
}

EC_FAST_CODE_SECTION void ec_netdev_low_level_release_rxbuf(ec_netdev_t *netdev)
{
    ec_netdev_esp_rx_item_t *inflight = &s_rx_inflight[netdev->index];

    /* The L2 TAP read path already released the Ethernet driver buffer; the
       copy handed out here is owned by the port. */
    free(inflight->buffer);
    inflight->buffer = NULL;
    inflight->length = 0;
}

esp_err_t esp_cherryecat_attach_netdev(uint8_t netdev_index, esp_eth_handle_t eth_handle)
{
    esp_err_t ret;

    if ((netdev_index >= CONFIG_EC_MAX_NETDEVS) || (eth_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    ec_netdev_esp_state_t *state = &s_states[netdev_index];
    if (state->bound) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_l2tap_vfs_registered) {
        /* Registering twice returns ESP_ERR_INVALID_STATE, tolerate it for
           applications that registered the L2 TAP VFS themselves. */
        ret = esp_vfs_l2tap_intf_register(NULL);
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
            return ret;
        }
        s_l2tap_vfs_registered = true;
    }

    if (!s_event_handler_registered) {
        ret = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED,
                                         ec_netdev_esp_event_handler, NULL);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                         ec_netdev_esp_event_handler, NULL);
        if (ret != ESP_OK) {
            (void)esp_event_handler_unregister(ETH_EVENT, ETHERNET_EVENT_CONNECTED,
                                               ec_netdev_esp_event_handler);
            return ret;
        }
        s_event_handler_registered = true;
    }

    /* The sublayer takes over the driver input path and forwards received
       frames to the L2 TAP filter. */
    esp_eth_sublayer_config_t sub_config = {
        .eth_handle = eth_handle,
    };
    ret = esp_eth_sublayer_new(&sub_config, &state->sublayer);
    if (ret != ESP_OK) {
        return ret;
    }

    state->l2tap_fd = open(L2TAP_VFS_DEFAULT_PATH, O_NONBLOCK);
    if (state->l2tap_fd < 0) {
        (void)esp_eth_sublayer_del(state->sublayer);
        state->sublayer = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    /* VFS ioctl returns 0 on success and -1 on failure. */
    if (ioctl(state->l2tap_fd, L2TAP_S_DEVICE_DRV_HNDL, eth_handle) != 0) {
        goto fail;
    }

    uint16_t ethertype = EC_NETDEV_ETHERTYPE_ECAT;
    if (ioctl(state->l2tap_fd, L2TAP_S_RCV_FILTER, &ethertype) != 0) {
        goto fail;
    }

    state->eth_handle = eth_handle;
    state->bound = true;
    state->link_up = false;
    state->speed_mbps = 0;
    state->full_duplex = false;

    return ESP_OK;

fail:
    (void)close(state->l2tap_fd);
    state->l2tap_fd = -1;
    (void)esp_eth_sublayer_del(state->sublayer);
    state->sublayer = NULL;
    return ESP_FAIL;
}

esp_err_t esp_cherryecat_detach_netdev(uint8_t netdev_index)
{
    if (netdev_index >= CONFIG_EC_MAX_NETDEVS) {
        return ESP_ERR_INVALID_ARG;
    }

    ec_netdev_esp_state_t *state = &s_states[netdev_index];
    if (!state->bound) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)close(state->l2tap_fd);
    state->l2tap_fd = -1;
    (void)esp_eth_sublayer_del(state->sublayer);
    state->sublayer = NULL;

    free(s_rx_inflight[netdev_index].buffer);
    s_rx_inflight[netdev_index].buffer = NULL;
    s_rx_inflight[netdev_index].length = 0;

    state->eth_handle = NULL;
    state->bound = false;
    state->link_up = false;
    state->speed_mbps = 0;
    state->full_duplex = false;

    return ESP_OK;
}
