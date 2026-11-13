# CherryECAT for ESP-IDF

This component ports [CherryECAT](https://github.com/cherry-embedded/CherryECAT), a tiny and highly real-time EtherCAT master stack designed for MCUs running with an RTOS, to ESP-IDF.

The component is based on CherryECAT v3.0.0 (upstream commit b1555ff) and uses the ESP-IDF Ethernet driver to transmit and receive raw EtherCAT frames. The application owns the Ethernet driver and must not run an IP stack on the same port.

Requires ESP-IDF 6.0 or later.

For CherryECAT protocol details, use the upstream [README](https://github.com/cherry-embedded/CherryECAT) and headers.

## Features

- Asynchronous queue-based transfer, one frame can carry multiple datagrams
- Hot-plug support: automatic bus scan and automatic slave info update on topology changes
- Automatic slave status monitoring
- Distributed clocks (DC), CANopen over EtherCAT (CoE), slave SII and register access
- Single domain (all slaves share one LRW datagram) or multi domain PDO transfer
- Multi master support (one master per Ethernet driver, `CONFIG_CHERRYECAT_MAX_NETDEVS` slots)
- IgH style `ethercat` command line tool on esp_console

## Add the component

```bash
idf.py add-dependency "espressif/cherryecat^0.1.0"
```

Include the public headers:

```c
#include "esp_cherryecat.h" /* ESP-IDF glue: Ethernet driver binding, console */
#include "ec_master.h"      /* upstream CherryECAT API */
```

## Basic usage

The application is responsible for installing and starting the Ethernet driver (internal EMAC + external PHY, or a SPI Ethernet module):

```c
/* Install the esp_eth driver with the MAC/PHY fitting the board, then bind
   it to the master before ec_master_init(). */
ESP_ERROR_CHECK(esp_cherryecat_attach_netdev(0, eth_handle));
ESP_ERROR_CHECK(esp_eth_start(eth_handle));

ec_master_t master;
ec_master_init(&master, 0);
```

The master scans the bus periodically. Once `master.scan_done` is set and the slaves are online, attach the per-slave configuration (sync managers, PDO callback, DC settings) and start the operation phase:

```c
master.slaves[0].config = &slave_config;
ec_master_config_t config = {
    .cycle_time = 10 * 1000 * 1000, /* 10 ms [ns] */
    .shift_time = 0,
    .dc_sync_with_dc_ref_enable = false,
};
ec_master_start(&master, &config);
```

The `pdo_callback` in `ec_slave_config_t` runs in the PDO cycle task context, keep it short and non-blocking. See [examples/ecat_io](examples/ecat_io) for a complete example.

To use the IgH style `ethercat` command, call `ec_master_cmd_init(&master)` first. The command entry point is the vendored `ethercat(argc, argv)` function (src/ec_cmd.c); to expose it on esp_console, add the `espressif/esp_console` managed component to the application and register a thin adapter:

```c
static int ethercat_console(int argc, char **argv)
{
    return ethercat(argc, (const char **)argv);
}

esp_console_cmd_t command = {
    .command = "ethercat",
    .help = "CherryECAT command line tool (IgH style)",
    .func = &ethercat_console,
};
ESP_ERROR_CHECK(esp_console_cmd_register(&command));
```

## Frame path

The netdev port receives EtherCAT frames through the **L2 TAP interface** (`esp_vfs_l2tap`, the ESP32 analog of a Linux TAP device on the VFS layer):

- `esp_cherryecat_attach_netdev()` creates an esp_eth **sublayer** which takes over the driver's input path, opens `/dev/net/tap` (non-blocking), binds it to the Ethernet driver handle and applies an **EtherType 0x88A4 filter**: filtering happens in the Ethernet driver receive path, so the descriptor only ever delivers EtherCAT frames.
- The stack pulls frames with `read()` (returns `EAGAIN` when nothing is queued); the L2 TAP receive queue (`CONFIG_ESP_NETIF_L2_TAP_RX_QUEUE_SIZE`, default 20, tail drop) absorbs frame bursts between cycles.
- Transmitted frames go out through `esp_eth_transmit()`; the L2 TAP `write()` path is not used.
- The L2 TAP descriptor also gives applications standard VFS semantics (`poll()`/`select()`/blocking reads) if they want to co-observe EtherCAT traffic on their own descriptors.

This requires the **experimental** esp_eth sublayer API. Enable in menuconfig: `Component config → Experimental` (`CONFIG_IDF_EXPERIMENTAL_FEATURES`) and `Component config → Ethernet → Support Ethernet netif sublayer` (`CONFIG_ETH_SUBLAYER_SUPPORT`). The component Kconfig selects `CONFIG_ESP_NETIF_L2_TAP` automatically.

## Configuration

Open **CherryECAT Configuration** in `idf.py menuconfig` to set resource limits, thread priorities and debug levels. The upstream `CONFIG_EC_*` macros from `cherryecat_config_template.h` are mapped to Kconfig options in `port/include/ec_config.h`.

Timing-related notes:

- `CONFIG_FREERTOS_HZ=1000` is recommended, matching the millisecond granularity assumed by the upstream stack.
- The PDO cycle is driven by a hardware gptimer alarm (1 MHz tick) that wakes a dedicated cycle task (`CONFIG_CHERRYECAT_CYCLE_TASK_*`). The next alarm is anchored to the previous alarm, so the long term cycle rate stays exact and wake-up jitter only shifts individual cycles.
- Stack timestamps use `esp_timer` (64-bit, microsecond resolution). For tight DC synchronization, keep the CPU frequency fixed (no power management scaling).
- Thread priorities for the internally created threads (`CONFIG_CHERRYECAT_NONPERIOD_PRIO`, `CONFIG_CHERRYECAT_SCAN_PRIO`) follow the upstream convention: smaller means higher priority, mapped onto FreeRTOS as `configMAX_PRIORITIES - 1 - prio`.

## Scope and limitations

What is compiled into the component is not automatically what has been validated on ESP-IDF.

### Validated

- Build of the full stack for ESP32-P4 (internal EMAC)
- [ecat_io](examples/ecat_io) example structure (bind driver, scan, configure, cyclic operation)

### Compiled but not validated on hardware

- Hardware bus operation with real EtherCAT slaves (DC synchronization, CoE, hot-plug)
- SPI Ethernet modules (W5500, DM9051, KSZ8851): the upstream stack calls the low-level send from a critical section; the internal EMAC transmit path is non-blocking and safe there, but SPI Ethernet drivers lock their bus and must not be called in that context

### Differences from upstream

- **PHY handling**: upstream drives PHYs itself through `chry_phy` (MDIO + per-PHY drivers). The ESP port always builds with `CONFIG_EC_PHY_CUSTOM`, the esp_eth driver owns the PHY (autonegotiation, MDIO, link events) and the netdev only mirrors the link state.
- **Frame RX**: upstream ports push frames from their ENET driver into the stack through platform callbacks. The ESP port pulls frames from an L2 TAP descriptor (EtherType-filtered, see the frame path section above), receiving one frame copy per `read()`.
- **Timestamps**: upstream reads ARM DWT or RISC-V MCYCLE cycle counters. ESP chips have neither, so the port builds with `CONFIG_EC_TIMESTAMP_CUSTOM` and provides an esp_timer based backend.
- **High precision timer**: upstream wires `ec_htimer_*` to a bare hardware timer ISR that runs the periodic process in interrupt context. The ESP port runs the periodic process in a dedicated cycle task woken by the gptimer alarm instead, because the ESP-IDF Ethernet driver must not be called from an ISR.
- **FOE/EOE**: the public upstream repository ships empty `ec_foe.c`/`ec_eoe.c` stubs, so those features are not functional in this port either.

## Vendor directory

`cherryecat/` contains the pristine upstream sources (`include/`, `src/`, `osal/`, `port/` from other SoC vendors kept for reference). ESP-IDF specific code lives in `port/`. Update the vendored copy by replacing the directory with a new upstream release and re-checking the port against the `ec_port.h`/`ec_osal.h` interfaces.
