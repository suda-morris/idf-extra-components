/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT EtherCAT digital IO example.
 *
 * Runs a CherryECAT EtherCAT master over the ESP-IDF Ethernet driver
 * (internal EMAC + external PHY). The process data of the slave is
 * exchanged cyclically: the input byte (switches) is echoed to the output
 * byte (LEDs) by the PDO callback.
 *
 * Slave identification and process-data layout follow a LAN9252 style
 * digital IO slave; adjust the sync managers and PDO entries for the
 * actual slave hardware. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_cherryecat.h"
#include "ec_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "ecat_io";

/* EtherCAT cyclic IO runtime parameters. */
#define IO_CYCLE_PERIOD_NS (10 * 1000 * 1000) /* 10 ms cycle time. */
#define IO_SCAN_TIMEOUT_MS 30000              /* Bus scan timeout in milliseconds. */

static ec_master_t g_ec_master;
static ec_slave_config_t s_slave_config;

/* Process-data mapping for a LAN9252 style digital IO slave:
   one output byte (LEDs) and one input byte (switches). */
static const ec_pdo_entry_info_t s_rxpdo_entries[] = {
    { 0x7000, 0x01, 8 }, /* LED output. */
};
static const ec_pdo_entry_info_t s_txpdo_entries[] = {
    { 0x6000, 0x01, 8 }, /* Switch input. */
};
static const ec_pdo_info_t s_rxpdos[] = {
    { 0x1600, sizeof(s_rxpdo_entries) / sizeof(s_rxpdo_entries[0]), s_rxpdo_entries },
};
static const ec_pdo_info_t s_txpdos[] = {
    { 0x1a00, sizeof(s_txpdo_entries) / sizeof(s_txpdo_entries[0]), s_txpdo_entries },
};
static const ec_sync_info_t s_slave_syncs[] = {
    { 0, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE }, /* Mailbox write. */
    { 1, EC_DIR_INPUT, 0, NULL, EC_WD_DISABLE },  /* Mailbox read. */
    { 2, EC_DIR_OUTPUT, sizeof(s_rxpdos) / sizeof(s_rxpdos[0]), s_rxpdos, EC_WD_ENABLE },
    { 3, EC_DIR_INPUT, sizeof(s_txpdos) / sizeof(s_txpdos[0]), s_txpdos, EC_WD_ENABLE },
    { 0xff, EC_DIR_OUTPUT, 0, NULL, EC_WD_DISABLE }, /* Terminator. */
};

/* PDO process data callback. Runs in the PDO cycle task context: keep it
   short and non-blocking. */
static void ecat_io_pdo_callback(ec_slave_t *slave, uint8_t *output, uint8_t *input)
{
    (void)slave;
    /* Echo the input (switches) to the output (LEDs). */
    *output = *input;
}

/* Initialize the internal EMAC with an external IP101 PHY. */
static void ecat_io_eth_init(esp_eth_handle_t *eth_handle_out)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* Use the ESP32-P4 default RMII pins and the PHY settings required by
       the example hardware. */
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_ERROR_CHECK(mac != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* IP101 is supported by the generic IEEE 802.3 PHY driver. */
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    ESP_ERROR_CHECK(phy != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, eth_handle_out));
}

void app_main(void)
{
    esp_eth_handle_t eth_handle = NULL;

    /* stage1: Initialize Ethernet and start the esp_eth driver. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ecat_io_eth_init(&eth_handle);

    /* stage2: Bind the Ethernet driver to the CherryECAT master and start
       the master. The master scans the bus automatically once the link is
       up, and re-scans when the topology changes. */
    ESP_ERROR_CHECK(esp_cherryecat_attach_netdev(0, eth_handle));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

#if CONFIG_CHERRYECAT_CMD_ENABLE
    /* Make the master known to the IgH style "ethercat" console command. */
    ec_master_cmd_init(&g_ec_master);
#endif
    ESP_ERROR_CHECK(ec_master_init(&g_ec_master, 0) == 0 ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "CherryECAT master initialized, waiting for the bus scan");

    /* stage3: Wait for the scan to find slaves, then attach the process-data
       configuration and enter the operation phase. */
    int64_t deadline_ms = esp_timer_get_time() / 1000 + IO_SCAN_TIMEOUT_MS;
    bool started = false;
    while (!started && esp_timer_get_time() / 1000 < deadline_ms) {
        if (g_ec_master.scan_done && (g_ec_master.slave_count > 0)) {
            bool all_online = true;
            for (uint32_t i = 0; i < g_ec_master.slave_count; i++) {
                if (!g_ec_master.slaves[i].online) {
                    all_online = false;
                    break;
                }
            }

            if (all_online) {
                for (uint32_t i = 0; i < g_ec_master.slave_count; i++) {
                    memset(&s_slave_config, 0, sizeof(s_slave_config));
                    s_slave_config.sync = (ec_sync_info_t *)s_slave_syncs;
                    s_slave_config.sync_count = (sizeof(s_slave_syncs) / sizeof(s_slave_syncs[0])) - 1;
                    s_slave_config.pdo_callback = ecat_io_pdo_callback;
                    s_slave_config.dc_assign_activate = 0x0000; /* No DC. */
                    g_ec_master.slaves[i].config = &s_slave_config;
                }

                ec_master_config_t master_config = {
                    .cycle_time = IO_CYCLE_PERIOD_NS,
                    .shift_time = 0,
                    .dc_sync_with_dc_ref_enable = false,
                };
                int ret = ec_master_start(&g_ec_master, &master_config);
                if (ret == 0) {
                    started = true;
                } else {
                    ESP_LOGE(TAG, "ec_master_start failed: %d", ret);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!started) {
        ESP_LOGE(TAG, "No EtherCAT slave found within %d ms", IO_SCAN_TIMEOUT_MS);
        return;
    }

    ESP_LOGI(TAG, "EtherCAT operation started, %" PRIu32 " slave(s)", g_ec_master.slave_count);

    /* stage4: Monitor loop, report state and working counters. The
       application can also use the "ethercat" console command to inspect
       the bus. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        ESP_LOGI(TAG, "phase %d, slave %" PRIu32 " state %d, WKC %" PRIu32 "/%" PRIu32 ", rx errors %" PRIu32,
                 (int)g_ec_master.phase,
                 g_ec_master.slave_count,
                 (int)g_ec_master.slaves[0].current_state,
                 g_ec_master.actual_working_counter,
                 g_ec_master.expected_working_counter,
                 g_ec_master.netdev[0]->stats.tx_errors);
    }
}
