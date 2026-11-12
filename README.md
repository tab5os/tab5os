# Tab5 OS

An operating system for the M5Stack Tab5 (ESP32-P4 + ESP32-C6), built on ESP-IDF.

**0.1.3 is a pre-release.** Download it from [Releases](https://github.com/tab5os/tab5os/releases) and follow [installing and updating](docs/flashing.md). It runs only on Tab5 units with ESP32-P4 silicon v1.x (`t5 chip` tells you). App API level 1 is frozen: apps built against it keep running on later releases. Apps built with the 0.1.0 SDK need one rebuild. What changed is in [CHANGELOG.md](CHANGELOG.md).
See [docs/tab5os-design.md](docs/tab5os-design.md) and [docs/tab5os-implementation-plan.md](docs/tab5os-implementation-plan.md).

**Status:** Phases 8 (MicroPython and developer tools) and 9 (updates, recovery and release tooling), on top of Phases 0–7: see [bench-results](docs/bench-results.md#phases-8-and-9--2026-11-07) for what is verified on Unit A and what still needs unattended board time or a person. Phase 8 adds MicroPython 1.29 for apps (contained: no `machine`, no native code, files only in the app's folders; everything else through the `t5` module), an on-device Editor and a Terminal with a REPL, developer mode (pairing, a token-authenticated HTTP API) and the `t5` CLI (`new`, `run`, `pair`, `push`, `logs`, `py`, `ota`), with Wi-Fi first and a USB-C fallback. Phase 9 adds A/B updates with signatures, a trial boot and automatic rollback, a boot counter in a custom bootloader, safe mode, a recovery image, credentials sealed with a chip-bound key, the RS-485 Console and Logger examples, long-run test scripts and `tools/release.sh`. Phases 6 and 7: launcher, quick settings, system apps, audio, camera, USB. Phases 1–5: board services, power and battery, Wi-Fi and BLE through the ESP32-C6, LVGL 9.6 with a PPA-rotated pipeline, Material Design 2, the simulator, the ports broker and native apps with a stable ABI. Guides: [app developer wiki](docs/wiki/Home.md), [SDK](docs/sdk.md), [Python](docs/python.md), [API reference](docs/api-reference.md), [installing and updating](docs/flashing.md), [security](docs/security.md), [coding standard](docs/coding-standard.md).

## Build and flash

```sh
. tools/env.sh                 # ESP-IDF at the version in firmware/idf_version.txt
t5 chip                        # chip revision (the build targets ESP32-P4 v1.x)
t5 build                       # firmware/build/
t5 size-check                  # the OS image within its budget (90% of an OTA slot)
t5 flash                       # bootloader, partition table, OS to ota_0
t5 monitor                     # console (Ctrl-] to quit)
t5 host-test                   # host unit tests (service logic against mocks)
t5 device-test                 # on-device Unity tests (replaces the OS; `t5 flash` after)
t5 console "sys" "i2c scan"    # run console commands from a script
t5 c6-build / t5 c6-stage      # C6 radio firmware, staged in the c6fw partition
t5 sim                         # SDL simulator (needs SDL2); t5 sim-shots checks its screenshots
t5 apps                        # build the SDK test apps (test/apps/build/*.t5app)
t5 examples                    # build the example apps (apps/examples/build/*.t5app)
t5 shot out.png                # screenshot over the console (--cam: the camera preview)
t5 sim --shell                 # the launcher, status bar and quick settings in the simulator
t5 lint                        # UI density, coding standard, ABI symbol file, docs/api-reference.md
t5 analyze                     # GCC's static analyzer on the strict components (firmware and recovery)
t5 hil NAME [ARGS]             # a bench script from test/hil/ (needs the board)
t5 recovery-build / recovery-flash   # the recovery image (recovery/), written to its own partition
t5 new NAME [--python] && t5 run     # an app: build, push (Wi-Fi, else USB-C), launch, stream the log
t5 pair <tab5-ip>              # developer mode (Settings > Developer mode): pair this computer
t5 ota-pack [--sign key.pem] && t5 ota   # an OS update, pushed to the paired Tab5
tools/release.sh [--sign key.pem]   # firmware, recovery, update package, SDK and examples into dist/
```

CI (`.github/workflows/ci.yml`) runs these same `t5` commands.

MicroPython is a git submodule: `git submodule update --init third_party/micropython` after cloning (not `--recursive`).

Without a native ESP-IDF install, `t5 --docker build` builds in `espressif/idf`. Flashing always runs on the host.

**First flash over the factory firmware:** back up first (`esptool read-flash 0 0x1000000 backup.bin`), then after `t5 flash` erase stale NVS and core dump data once:

```sh
esptool erase-region 0x11000 0x2F000 && esptool erase-region 0xB50000 0x40000
```

## Bring-up console

Commands marked "bench builds only" are left out unless the firmware is built with `CONFIG_T5_DIAG_BENCH=y` (see [bench procedures](docs/bench-procedures.md)).

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
| `disp` / `disp bl\|fill\|sleep\|wake\|ids` | Display |
| `touch` / `touch watch <s>` / `touch dots on\|off` | Touch |
| `gpio watch <ms> [pins] [log]` / `gpio get` | Edge watcher for bench checks (bench builds only) |
| `sleep light <ms> [MB] [g<pin>...]` / `sleep result` | Light-sleep test (bench builds only) |
| `elf bench [rounds]` | ELF-in-PSRAM benchmark (bench builds only) |
| `dmesg` | Log since boot |
| `power` / `power locks\|hooks\|sleep\|wake\|screenoff` | Power state, wake locks, suspend hooks |
| `power lock <type> [ms]` / `unlock` / `profile` / `timers` / `alarm <s>` / `off --force` | Locks, idle timers, scheduled wake, soft off |
| `rail` / `rail on\|off <rail>` | Switched rails and who holds them |
| `bat` / `bat log start\|stop` / `bat limit\|fast on\|off` | Fuel gauge, charging, trace log |
| `net` / `net scan\|connect <ssid> [pass]\|forget\|reset\|update\|ant\|echo` | Wi-Fi via the C6 |
| `ble on\|scan\|connect <addr>\|gatt\|read\|sub` | BLE central via the C6 |
| `ui` / `ui rot\|lock\|auto\|imu\|stats\|accent` | Display pipeline, rotation, frame counters, MD2 accent |
| `ui gate [all\|scroll\|transition\|dialog] [s]` | The rendering gate (plan 4.8), PASS/FAIL against the frame-rate targets |
| `ui frames` / `ui dialog open\|close` | The last 32 frames (render, PPA and copy time, area redrawn); a test dialog to step through |
| `app` / `app run\|exit\|install\|rm\|usage\|crashes\|dump <id>\|abi\|alarm` | App runtime; `dump` prints an app's saved core dump as base64 |
| `app fetch <url> [sd\|data]` | Download a `.t5app` over HTTP or HTTPS (outgoing only) and install it |
| `app stress <id> <n>` | Open and close an app n times, checking heap, claims, locks and rails return to baseline |
| `port` / `port open\|close\|i2c\|gpio\|pwm\|adc\|uart\|scan\|profiles\|modbus ...` | External ports: owners and pin levels, claims, raw I/O, module scan, Modbus RTU |
| `shot [div]` | The screen as base64 RGB565 (`t5 shot` turns it into a PNG) |
| `shell home\|qs\|toast <text>\|launch <id> [arg]` | Shell: go home, quick settings, a snackbar, open an app with an argument |
| `audio` / `audio vol\|mute\|sounds\|sound\|tone ...` | Audio state, volume, system sounds, a test tone |
| `audio play <s>` / `rec <s> [aec]` / `echo <s>` | Playback (underruns), recording (levels), echo cancellation with and without |
| `cam open\|close\|stats\|photo <path>\|rec <path> <s>\|snap` | Camera: stats, a JPEG, an H.264 recording, the preview as base64 |
| `usb` / `usb host on\|off\|eject\|midi <s>\|serial <baud> <text>` | USB-A devices and classes |
| `usbc console\|storage [s]\|trace` | USB-C role: hand the SD card to a computer until it ejects it (or for s seconds); the console is gone meanwhile |
| `py exec <code>\|run <file>\|test` | MicroPython: run code (statements separated by `;`), a script, or the containment checks |
| `devmode [on\|off\|forget]` | Developer mode: the HTTP API on port 8350, paired computers |
| `ota [file P\|url U [unsigned]\|selftest\|recovery\|safemode]` | Updates, the trial and boot counter, restart into recovery or safe mode |
| `ui tap X Y [ms]` / `ui type TEXT` / `ui key enter\|up\|...` | Scripted touch and keys (tests) |
| `app put PATH SIZE` | Receive a file as base64 lines (the CLI's USB fallback) |

Bench scripts in `test/hil/` drive the console through `tools/t5/t5link.py`. Run them with `t5 hil NAME [ARGS]` (`t5 hil` lists them), which uses the IDF Python and finds `tools/t5`; `t5 --port PORT hil ...` picks the board. `t5 hil app_checks` runs the self-checking test apps from `t5 apps`. Steps that need a person are in [docs/bench-procedures.md](docs/bench-procedures.md).

## Layout

`firmware/` OS image · `recovery/` recovery image · `c6/` radio firmware · `sdk/` app SDK · `sim/` SDL simulator · `tools/` CLI and asset tools · `test/` host, device and HIL tests · `docs/`

Licence: Apache-2.0.
