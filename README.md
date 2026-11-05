# Tab5 OS

An operating system for the M5Stack Tab5 (ESP32-P4 + ESP32-C6), built on ESP-IDF.
See [docs/tab5os-design.md](docs/tab5os-design.md) and [docs/tab5os-implementation-plan.md](docs/tab5os-implementation-plan.md).

**Status:** Phases 2 (power and battery) and 3 (C6 modem). On top of the Phase 1 board services: power states and wake locks, rails, soft off, charge mode, a fuel gauge, and Wi-Fi and BLE through the ESP32-C6. Bench results: [docs/bench-results.md](docs/bench-results.md).

## Build and flash

```sh
. tools/env.sh                 # ESP-IDF at the version in firmware/idf_version.txt
t5 chip                        # which ESP32-P4 revision? v1.x -> rev1, v3.x -> rev3
t5 build                       # default rev1; or: t5 --rev rev3 build
t5 flash                       # bootloader, partition table, OS to ota_0
t5 monitor                     # console (Ctrl-] to quit)
t5 host-test                   # host unit tests (service logic against mocks)
t5 device-test                 # on-device Unity tests (replaces the OS; `t5 flash` after)
t5 console "sys" "i2c scan"    # run console commands from a script
t5 c6-build / t5 c6-stage      # C6 radio firmware, staged in the c6fw partition
```

Without a native ESP-IDF install, `t5 --docker build` builds in `espressif/idf`. Flashing always runs on the host.

**First flash over the factory firmware:** back up first (`esptool read-flash 0 0x1000000 backup.bin`), then after `t5 flash` erase stale NVS and core dump data once:

```sh
esptool erase-region 0x11000 0x2F000 && esptool erase-region 0xB50000 0x40000
```

## Bring-up console

| Command | Purpose |
| --- | --- |
| `sys` | Chip revision, clocks, PSRAM, heap, partition, reset reason |
| `psram [MB]` | Pattern test and bandwidth |
| `tasks` | FreeRTOS tasks and CPU use |
| `i2c scan` / `i2c r <addr> <reg> [n]` / `i2c w <addr> <reg> <bytes>` | System I2C bus (G31/G32) |
| `i2c stats` / `trace` / `recover` / `stress <s>` / `r16` | Bus counters, traffic log, forced recovery, multi-task stress, 16-bit registers |
| `exp` / `exp out\|in\|pulse <exp> <pin> ...` | Expanders 0x43/0x44 through the xpin service (`PWROFF_PLUSE` needs `--force`) |
| `rtc` / `rtc set <epoch>` / `alarm` / `timer` / `clear` | RTC and wall time. **An RTC interrupt while running cuts power** |
| `imu` / `imu watch <s>` / `imu motion <mg>\|off` | BMI270 |
| `fs df\|ls\|cat\|rm\|stress\|verify\|eject\|rescan\|sdraw\|sdbench` | `/data` and `/sd` |
| `disp` / `disp bl\|pattern\|fill\|sleep\|wake\|ids` | Display |
| `touch` / `touch watch <s>` / `touch dots on\|off` | Touch |
| `gpio watch <ms> [pins] [log]` / `gpio get` | Edge watcher for bench checks |
| `sleep light <ms> [MB] [g<pin>...]` / `sleep result` | Light-sleep test |
| `elf bench [rounds]` | ELF-in-PSRAM benchmark |
| `dmesg` | Log since boot |
| `power` / `power locks\|hooks\|sleep\|wake\|screenoff` | Power state, wake locks, suspend hooks |
| `power lock <type> [ms]` / `unlock` / `profile` / `timers` / `alarm <s>` / `off --force` | Locks, idle timers, scheduled wake, soft off |
| `rail` / `rail on\|off <rail>` | Switched rails and who holds them |
| `bat` / `bat log start\|stop` / `bat limit\|fast on\|off` | Fuel gauge, charging, trace log |
| `net` / `net scan\|connect <ssid> [pass]\|forget\|reset\|update\|ant\|echo` | Wi-Fi via the C6 |
| `ble on\|scan\|connect <addr>\|gatt\|read\|sub` | BLE central via the C6 |

Bench scripts in `test/hil/` use `tools/t5/t5link.py` to drive the console; run them with the IDF Python env. Steps that need a person are in [docs/bench-procedures.md](docs/bench-procedures.md).

## Layout

`firmware/` OS image · `recovery/` recovery image · `c6/` radio firmware · `sdk/` app SDK · `sim/` SDL simulator · `tools/` CLI and asset tools · `test/` host, device and HIL tests · `docs/`

Licence: Apache-2.0.
