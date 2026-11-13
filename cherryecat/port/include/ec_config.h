/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT runtime configuration for ESP-IDF.
 *
 * Maps the component Kconfig options (cherryecat/Kconfig) onto the
 * CONFIG_EC_* macros the upstream stack expects, in the spirit of
 * cherryecat_config_template.h. The ESP port always uses the esp_eth
 * managed PHY (CONFIG_EC_PHY_CUSTOM) and its own timestamp backend
 * (CONFIG_EC_TIMESTAMP_CUSTOM). */
#ifndef EC_CONFIG_H
#define EC_CONFIG_H

#include "sdkconfig.h"
#include <stdio.h>

#define CONFIG_EC_PRINTF(...) printf(__VA_ARGS__)

#define CONFIG_EC_DBG_LEVEL CONFIG_CHERRYECAT_DBG_LEVEL
#define CONFIG_EC_SLAVE_DBG_LEVEL CONFIG_CHERRYECAT_SLAVE_DBG_LEVEL

#ifdef CONFIG_CHERRYECAT_PRINTF_COLOR_ENABLE
#define CONFIG_EC_PRINTF_COLOR_ENABLE
#endif

#define EC_FAST_CODE_SECTION

/* The esp_eth driver owns the PHY (autonegotiation, MDIO, link events). */
#define CONFIG_EC_PHY_CUSTOM

/* Xtensa and RISC-V ESP chips have neither ARM DWT nor RISC-V MCYCLE
   support upstream; the port provides an esp_timer based backend. */
#define CONFIG_EC_TIMESTAMP_CUSTOM

#ifdef CONFIG_CHERRYECAT_CMD_ENABLE
#define CONFIG_EC_CMD_ENABLE
#endif

#ifdef CONFIG_CHERRYECAT_ASSERT_DISABLE
#define CONFIG_EC_ASSERT_DISABLE
#endif

#ifdef CONFIG_CHERRYECAT_PDO_MULTI_DOMAIN
#define CONFIG_EC_PDO_MULTI_DOMAIN
#endif

#define CONFIG_EC_MAX_NETDEVS CONFIG_CHERRYECAT_MAX_NETDEVS

#define CONFIG_EC_NONPERIOD_PRIO CONFIG_CHERRYECAT_NONPERIOD_PRIO
#define CONFIG_EC_NONPERIOD_STACKSIZE CONFIG_CHERRYECAT_NONPERIOD_STACKSIZE
#define CONFIG_EC_NONPERIOD_WAITERS CONFIG_CHERRYECAT_NONPERIOD_WAITERS
#define CONFIG_EC_SCAN_PRIO CONFIG_CHERRYECAT_SCAN_PRIO
#define CONFIG_EC_SCAN_STACKSIZE CONFIG_CHERRYECAT_SCAN_STACKSIZE
#define CONFIG_EC_SCAN_INTERVAL_MS CONFIG_CHERRYECAT_SCAN_INTERVAL_MS

#define CONFIG_EC_MAX_PDO_BUFSIZE CONFIG_CHERRYECAT_MAX_PDO_BUFSIZE
#define CONFIG_EC_PER_SM_MAX_PDOS CONFIG_CHERRYECAT_PER_SM_MAX_PDOS
#define CONFIG_EC_PER_PDO_MAX_PDO_ENTRIES CONFIG_CHERRYECAT_PER_PDO_MAX_PDO_ENTRIES

/* GCC does not predefine __WEAK; the upstream stack uses it for
   user-overridable hooks (e.g. cherryecat_eepromdata in ec_cmd.c). */
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#endif /* EC_CONFIG_H */
