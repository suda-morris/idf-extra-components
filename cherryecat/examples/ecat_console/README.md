# CherryECAT console example

Runs the CherryECAT EtherCAT master over the ESP-IDF Ethernet driver (internal EMAC + external IP101 PHY on the ESP32-P4/ESP32 RMII pins) and exposes the upstream IgH style `ethercat` command line tool — the same tool CherryECAT ships for RT-Thread's finsh/msh — through an esp_console REPL on the default UART console.

## Build and flash

```bash
idf.py set-target esp32p4   # or esp32
idf.py build flash monitor
```

The master comes up in the idle phase and scans the bus automatically every 100 ms once the Ethernet link is up. Topology changes (hot-plug) are detected and slaves are updated automatically.

## Command session

```
ecat> ethercat help
ecat> ethercat master                    # master/netdev state, phase, statistics
ecat> ethercat slaves                    # list scanned slaves (SII identity, name)
ecat> ethercat slaves -p 0 -v            # verbose details of slave 0
ecat> ethercat pdos                      # PDO mapping of all slaves
ecat> ethercat states                    # current AL states
ecat> ethercat states -p 0 8             # request slave 0 to OPERATIONAL
ecat> ethercat start 1000                # enter cyclic operation, 1000 us cycle
ecat> ethercat wc                        # process data working counters
ecat> ethercat pdo_read -p 0             # dump process data of slave 0
ecat> ethercat perf -s                   # start cycle timing statistics
ecat> ethercat perf -v                   # print min/max/avg cycle, send, recv times
ecat> ethercat perf -d                   # stop statistics
ecat> ethercat coe_read -p 0 0x1018 0    # CoE SDO upload
ecat> ethercat coe_write -p 0 0x6040 0 0x0f 2
ecat> ethercat sii_read -p 0 0x0 4       # read SII words
ecat> ethercat rescan                    # force a bus re-scan
ecat> ethercat stop                      # leave the operation phase
```

`ethercat start <cycle_us>` configures every slave from its internal sync info table (`ec_master_find_slave_sync_info`, CIA402 style slaves are matched by vendor/product/revision), assigns DC SYNC0 with the given cycle and starts the periodic process. The process data callback in the example echoes the first TxPDO byte into the first RxPDO byte; replace `ec_pdo_callback()` for your hardware.

The `foe_write`/`foe_read`/`eoe_*` subcommands exist in the vendored tool but require the FOE/EOE features, which are stubs in the public upstream repository and therefore non-functional here.

## Hardware

Configure the PHY with `phy_config.phy_addr` / `reset_gpio_num` in the example main for your board. The EtherCAT frame path runs on the L2 TAP interface (see the component README); the experimental esp_eth sublayer API is enabled in sdkconfig.defaults.
