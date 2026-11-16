/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT console example.
 *
 * Runs the CherryECAT EtherCAT master over the ESP-IDF Ethernet driver and
 * exposes the upstream IgH style "ethercat" command line tool (the tool
 * CherryECAT ships for RT-Thread's finsh/msh) through an esp_console REPL
 * on the default UART console.
 *
 * Typical session:
 *   ecat> ethercat master          # show master and netdev state
 *   ecat> ethercat slaves          # list scanned slaves
 *   ecat> ethercat slaves -p 0 -v  # verbose slave details
 *   ecat> ethercat start 1000      # start cyclic operation, 1000 us cycle
 *   ecat> ethercat states          # read slave AL states
 *   ecat> ethercat wc              # working counters
 *   ecat> ethercat perf -s         # start cycle/send/recv statistics
 *   ecat> ethercat perf -v         # print statistics
 *   ecat> ethercat perf -d         # stop statistics
 *   ecat> ethercat stop            # leave the operation phase
 *
 * The process data callback below echoes the first TxPDO byte to the first
 * RxPDO byte of every slave in OP state (switches to LEDs on typical IO
 * test slaves); adapt it for the target hardware. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_cherryecat.h"
#include "ec_master.h"
#include "esp_console.h"
#include "esp_stdio_cli_config.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "ecat_console";

static ec_master_t g_ec_master;
static esp_console_repl_t *s_repl = NULL;

/* PDO process data callback. Runs in the PDO cycle task context once the
   master reached the operation phase: keep it short and non-blocking.
   This is the same hook the upstream RT-Thread demos override. */
void ec_pdo_callback(ec_slave_t *slave, uint8_t *output, uint8_t *input)
{
    if (slave->current_state != EC_SLAVE_STATE_OP) {
        return;
    }

    /* Echo process data: first output byte follows the first input byte. */
    if (slave->odata_size > 0 && slave->idata_size > 0) {
        output[0] = input[0];
    }
}

/* Adapter from esp_console to the vendored IgH style ethercat tool
 * (src/ec_cmd.c). The tool requires ec_master_cmd_init() to have been
 * called with the master it should operate on.
 *
 * The upstream dispatch reads argv[2] unguarded for some subcommands
 * (e.g. a bare "ethercat perf" would dereference a NULL argument), so the
 * adapter rejects argument shapes the tool cannot handle. */
static int ethercat_console_adapter(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: ethercat <subcommand> [args], try 'ethercat help'\n");
        return -1;
    }
    if (strcmp(argv[1], "perf") == 0 && argc < 3) {
        printf("Usage: ethercat perf <-s|-d|-v>\n");
        return -1;
    }
    return ethercat(argc, (const char **)argv);
}

static void ethercat_console_register(void)
{
    esp_console_cmd_t command = {
        .command = "ethercat",
        .help = "CherryECAT EtherCAT master tool (IgH style)",
        .hint = "<subcommand> [args]",
        .func = &ethercat_console_adapter,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

/* Initialize the internal EMAC with an external IP101 PHY. */
static void ecat_console_eth_init(esp_eth_handle_t *eth_handle_out)
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

    /* stage1: console REPL first, so ethercat works even before the bus is
       up (the tool reports scan/link state on its own). */
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ecat";
    repl_config.max_cmdline_length = 512;
    /* The ethercat subcommands run inside the REPL task; give them room. */
    repl_config.task_stack_size = 8192;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &s_repl));
    ethercat_console_register();
    ESP_ERROR_CHECK(esp_console_start_repl(s_repl));

    /* stage2: bring up Ethernet and the master. The master scans the bus
       automatically once the link is up and detects topology changes. */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ecat_console_eth_init(&eth_handle);

    ESP_ERROR_CHECK(esp_cherryecat_attach_netdev(0, eth_handle));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    /* Make the vendored ethercat tool operate on this master. */
    ec_master_cmd_init(&g_ec_master);
    ESP_ERROR_CHECK(ec_master_init(&g_ec_master, 0) == 0 ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "CherryECAT master initialized, waiting for the bus scan");
    ESP_LOGI(TAG, "Type 'ethercat help' for the command list, 'ethercat master' for status");
}
