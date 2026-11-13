/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_eth_driver.h"
#include <stdint.h>

#include "ec_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind an installed ESP-IDF Ethernet driver to a CherryECAT netdev slot.
 * Must be called before ec_master_init(), because ec_master_init() creates
 * the netdevs. The Ethernet driver remains owned by the application: the
 * application is responsible for esp_eth_start()/esp_eth_stop().
 *
 * The binding runs through the L2 TAP interface (esp_vfs_l2tap): the
 * esp_eth sublayer takes over the driver's input path, an L2 TAP file
 * descriptor bound to the Ethernet driver receives only EtherCAT frames
 * (EtherType 0x88A4 filter applied in the driver receive path) and an
 * ETH_EVENT handler tracks the link state. Requires CONFIG_ETH_SUBLAYER_SUPPORT
 * and CONFIG_ESP_NETIF_L2_TAP (selected by the component Kconfig).
 *
 * @param netdev_index, Netdev slot to bind, in range [0, CONFIG_CHERRYECAT_MAX_NETDEVS).
 * @param eth_handle, Handle of an installed ESP-IDF Ethernet driver.
 * @return
 *      - ESP_OK: netdev bound successfully
 *      - ESP_ERR_INVALID_ARG: invalid arguments
 *      - ESP_ERR_INVALID_STATE: slot already bound or L2 TAP VFS not available
 *      - ESP_FAIL: failed to bind the descriptor to the driver
 */
esp_err_t esp_cherryecat_attach_netdev(uint8_t netdev_index, esp_eth_handle_t eth_handle);

/* Unbind the Ethernet driver from a CherryECAT netdev slot: closes the L2
 * TAP descriptor, deletes the esp_eth sublayer and releases pending RX
 * frames. Must be called after ec_master_stop() and before the Ethernet
 * driver is uninstalled.
 *
 * @param netdev_index, Netdev slot to unbind.
 * @return
 *      - ESP_OK: netdev unbound successfully
 *      - ESP_ERR_INVALID_ARG: invalid arguments
 *      - ESP_ERR_INVALID_STATE: slot not bound
 */
esp_err_t esp_cherryecat_detach_netdev(uint8_t netdev_index);

#ifdef __cplusplus
}
#endif
