# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS "xymodem_ota_main.c"
                       PRIV_REQUIRES esp_driver_uart app_update esp_partition mbedtls
                       INCLUDE_DIRS ".")
