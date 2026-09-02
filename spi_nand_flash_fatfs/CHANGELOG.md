## [1.1.1]
- fix: call `esp_vfs_fat_register` with `esp_vfs_fat_conf_t` on ESP-IDF v6.0+ (the `_cfg` name is deprecated there). Keep `esp_vfs_fat_register_cfg` for v5.3–v5.x and the 4-argument register API for older IDF.

## [1.1.0]

### Added
- Example `examples/nand_flash_bdl`: FatFS on SPI NAND via ESP-IDF BDL (`spi_nand_flash_init_with_layers` + `esp_vfs_fat_bdl_mount`), ESP-IDF 6.1+ with `CONFIG_NAND_FLASH_ENABLE_BDL=y`.
- `esp_vfs_fat_nand_bdl_format()`: mandatory pre-mount FAT format on the WL BDL (same `f_mkfs` layout as `esp_vfs_fat_bdl_mount()`), ESP-IDF 6.1+ with `CONFIG_NAND_FLASH_ENABLE_BDL=y`.
- Documentation updates for BDL vs legacy FatFS paths in the component README and `spi_nand_flash/layered_architecture.md`.

## [1.0.0]

### Breaking Changes
- FATFS integration for SPI NAND Flash now lives in this component. Projects that previously relied on FATFS support bundled inside `spi_nand_flash` must add `spi_nand_flash_fatfs` as a dependency and include its headers.

**Migration:** See **Migration Guide (0.x → 1.0.0)** in [`spi_nand_flash/layered_architecture.md`](../spi_nand_flash/layered_architecture.md) (FATFS split, legacy init with BDL disabled, and related driver changes).
