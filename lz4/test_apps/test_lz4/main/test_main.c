/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_newlib.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "unity_test_utils_memory.h"

void setUp(void)
{
    unity_utils_record_free_mem();
}

void tearDown(void)
{
    printf("\r\n");
    esp_reent_cleanup();
    unity_utils_evaluate_leaks_direct(0);
}

void app_main(void)
{
    const esp_vfs_littlefs_conf_t littlefs_config = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&littlefs_config));

    // Touch the VFS once before Unity starts. LittleFS and newlib may lazily
    // allocate file-system and stdio bookkeeping on the first file operation.
    FILE *warmup_file = fopen("/littlefs/.lz4_warmup", "wb");
    ESP_ERROR_CHECK(warmup_file != NULL ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(fclose(warmup_file) == 0 ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(remove("/littlefs/.lz4_warmup") == 0 ? ESP_OK : ESP_FAIL);

    printf("Running lz4 component tests\n");
    unity_run_menu();
}
