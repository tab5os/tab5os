# Tab5 OS: implementation plan

Nov 5, 2026 · companion to [tab5os-design.md](tab5os-design.md)

## Summary

This plan turns the design into ten phases, each with concrete work items and an exit test. The ordering follows the design's main rule: build power management before the shell, so every driver is written for sleep from the start.

Three ideas shape the plan:

1. **Bench checks come first.** Seven of the twelve bench checks can change the architecture, so Phase 0 answers them before any code relies on the answers.
2. **Write logic once and test it on a PC.** Every service has a thin hardware layer underneath it. The broker, wake-lock arbitration, fuel gauge, manifest parser, orientation filter and the whole MD2 layer then compile for the host. That means unit tests and the SDL simulator run without a Tab5.
3. **The rendering test is a go/no-go gate.** The MD2 test screen (Phase 4) decides whether the shadow and corner strategy works before any real component is built on it.

Phases have exit criteria, not dates. Sizes (S under a week, M 1–3 weeks, L 3–6 weeks, for one person) are only for comparing items.

## Gaps in the design this plan fills

The design leaves a few things open that implementation can't skip. The plan uses these answers. Change them here if you disagree.

| Gap | Plan's answer |
| --- | --- |
| The roadmap diagram didn't come through as text | The phases below replace it |
| LVGL threading rules for apps | All LVGL calls run on the UI task. App callbacks run there too. App-created tasks must use `t5_ui_async()` or `t5_ui_lock()`/`t5_ui_unlock()` |
| How the OS tracks a native app's heap, timers and LVGL objects | The ELF symbol resolver binds `malloc`/`free`/`lv_timer_create`/`lv_anim_start`/`xTaskCreate` to tracking wrappers. All app UI sits under the app's root screen, so deleting that root frees every object |
| Which app is "current" inside a service call | A FreeRTOS thread-local pointer holds the app context, inherited by `t5_task_create`. While an app is in the foreground the UI task carries its context the whole time, so timers and event handlers are charged to it too. System UI code on the UI task brackets its own acquisitions with `t5_sys_enter()`/`t5_sys_leave()` |
| A native app that hangs the UI task | A UI-task watchdog (2 s) shows "App not responding": Wait, or Close app, which moves the UI task out of the app's code and closes the app (since 2026-11-08; a restart to the launcher only if the task can't be moved safely). The crash guard records it |
| How the system talks to itself | One `esp_event` loop for system events (power, battery, net, ports, USB, rotation), plus direct mutex-protected calls into service APIs |
| The camera's I2C address is missing from the reserved list | **Answered:** 0x36, once `CAM_RST` is released. Also reserved: 0x28, a second ST7123 address |
| Where `PWROFF_PLUSE` is wired | **Answered from the schematic:** expander 0x44, P4. Confirming the pulse on battery is still to do |
| How MicroPython stays contained | Build without `machine`, `uctypes`, `@native`/`@viper`/inline asm and raw `ffi`, so the only way out is the `t5` module |
| Which ESP32-P4 revision the unit has | Unknown. Early Tab5s report v1.0, and newer stock may have v3.x. ESP-IDF can't build one image for both, so the build supports both (see "Chip revision" below) |
| Theme | **Decided:** dark theme first. Light theme tokens exist from the start but aren't polished or tested until after 1.0 |
| Other open decisions | Defaults until you decide: Apache-2.0, landscape handheld first with a "mounted" profile setting, baseline palette as a placeholder |

## Repository layout

One monorepo. The firmware, the simulator and the SDK all build from the same component sources.

```
tab5os/
  firmware/                 ESP-IDF project: the OS image (ota_0 / ota_1)
    main/                   boot sequence, service start order
    components/
      t5_core/              errors, events, app context, logging, t5_task_create
      t5_bsp/               pin map, board variant probe, expander pins, rail switches
      t5_i2c/               bus service, device handles, reserved addresses, recovery
      t5_power/             states, wake locks, rails refcount, sleep and soft off, charge mode
      t5_battery/           INA226 sampling, fuel gauge, charge policy, history
      t5_ports/             named ports, claim broker, module profiles
      t5_display/ t5_input/ panel variants, backlight, rotation, GT911 and ST712x
      t5_imu/ t5_time/      BMI270 with retry, RX8130CE, NTP, alarms
      t5_storage/           LittleFS /data, SD mount and unmount, sync policy
      t5_net/               esp_hosted host, Wi-Fi, BLE, C6 power and update
      t5_audio/ t5_camera/  ES8388 and ES7210 via esp_codec_dev; esp_video, ISP, H.264, JPEG
      t5_usb/               USB-A host classes, USB-C mode switching
      t5_app/               lifecycle, resource tracker, ELF loader glue, ABI table, crash guard
      t5_pkg/               .t5app unzip, TOML manifest, permissions, installer
      t5_ota/               A/B updates, health check, rollback
      t5_mpy/               MicroPython port and t5 bindings
      md/                   md_tokens, md_theme, md_widgets, t5_md_* API
      t5_shell/             launcher, status bar, quick settings, snackbar, dialog, drawer
    sysapps/                Settings, Files, App Manager, Power, Port Inspector, Terminal, Camera
    partitions.csv
    sdkconfig.defaults
  recovery/                 small separate IDF project for the recovery partition
  c6/                       C6 firmware: esp_hosted slave plus wake signalling on IO2
  sdk/
    include/t5/             public headers, the only API apps see
    abi/api_v1.sym          exported symbol list per API level (checked by CI)
    cmake/                  toolchain file and t5_app() helper for position-independent ELFs
    templates/              C and Python app templates
  sim/                      host build: LVGL SDL driver plus mocked t5_* services
  tools/
    t5/                     Python CLI: new, build, run, sim, pack
    assets/                 font subsetter, shadow nine-slice baker, icon font builder
  apps/examples/            UVC monitor, RS-485 console, scheduled logger
  test/
    host/                   unit tests for pure logic (ctest + Unity)
    device/                 on-device Unity test app
    hil/                    bench scripts: power meter capture, sleep cycling, I2C stress
  docs/
```

Each hardware component has two layers: `*_hw.c`, which calls ESP-IDF, and logic files that call only an internal `*_hw.h`. The simulator and host tests swap in `*_hw_mock.c`.

## Toolchain and baseline settings

| Item | Setting |
| --- | --- |
| ESP-IDF | **v6.1**, the latest stable release (27 Aug 2026). Record the tag in `firmware/idf_version.txt`, and have CI check it. Move to v6.1.x bugfix releases as they ship. Re-check component versions against IDF 6, since some (such as `esp_lvgl_adapter` and `elf_loader`) may need newer releases than the ones in the design |
| Silicon | Two build variants, one per revision family. See "Chip revision" below |
| PSRAM | 32 MB, 200 MHz if stable on this unit's revision, with `malloc` above a threshold going to PSRAM |
| LVGL | Pin one version per API level in `idf_component.yml`. Enable PPA draw unit, bezier paths, larger circle cache |
| Components | Pin versions of `esp_hosted`, `esp_video`, `esp_codec_dev`, `elf_loader` (2.1 or later), `esp_lvgl_adapter`, `usb_host_*`, `esp_tinyusb`, `littlefs` |
| Partitions | The flash layout in the design, with the partition table at 0x10000 so the bootloader has room to grow. Committed in Phase 0 (`firmware/partitions.csv`), so A/B and recovery offsets never move |
| CI | GitHub Actions on `espressif/idf:v6.1`: firmware and recovery builds for both revision variants, C6 build, host tests, simulator build, ABI symbol diff, image size budget |

### Build environment

Install ESP-IDF v6.1 natively on macOS with the standard installer, in its own directory. The v5.5.5 already in `~/esp` stays separate, and a `tools/env.sh` script sources the right one. If the native install fails, build in Docker:

```sh
docker run --rm -v "$PWD":/project -w /project/firmware espressif/idf:v6.1 \
  idf.py -B build build
```

The `espressif/idf:v6.1` image has a native arm64 build, so it runs at full speed on Apple Silicon. Docker on macOS can't reach USB serial ports, so flashing and the monitor always run on the Mac. Use host `esptool` (v5 is already installed), or `idf.py flash monitor` from a native install. `tools/t5` wraps both paths so the commands are the same either way.

### Chip revision

**Status:** the two variants below were the plan. Unit A turned out to be v1.3, and the rev3 variant was dropped: there is one build (`firmware/build`, `firmware/sdkconfig`) with the pre-v3 options in `firmware/sdkconfig.board`, for v1.x silicon only. The rest of this section is the original plan.

ESP-IDF v6.1 treats ESP32-P4 revisions below 3.0 and 3.0 or later as incompatible. `ESP32P4_SELECTS_REV_LESS_V3` picks one family, and the bootloader refuses to boot an image built for the other. NuttX reports v1.0 on early Tab5 units, but newer units may ship with v3.x, so the OS supports both:

| Variant | Config | Kconfig | Runs on |
| --- | --- | --- | --- |
| `rev1` | `sdkconfig.defaults.rev1` | `ESP32P4_SELECTS_REV_LESS_V3=y`, minimum Rev v1.0 | v1.x units, which the early Tab5s seem to be |
| `rev3` | `sdkconfig.defaults.rev3` | Default (no pre-v3 option), minimum Rev v3.0 rather than the v3.1 default | v3.x units |

- **Detect it first.** Run `esptool chip-id` with the Tab5 connected. It prints `Chip is ESP32-P4 (revision vX.Y)`. Build that variant from then on, and record the result in `bench-results.md`.
- **Name images by variant.** Use `tab5os-<version>-rev1.bin` and `-rev3.bin`. The OTA updater and recovery check the running chip's revision and refuse an image for the wrong family, before writing anything.
- **Keep revision-specific code in one place.** Keep `#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3` blocks inside `t5_bsp` and `sdkconfig`. Other code must not branch on the revision.
- **Apps don't depend on the revision.** Native apps are position-independent and link against the OS at load time, so one `.t5app` should run on both. Phase 5 tests this on both variants. If it fails, the manifest gets a `chip_rev` field.
- **Clock and memory differ.** The board's 360 MHz figure was reported for v1.0. v3.x silicon may allow a higher clock and has a different memory layout, so measure the frame rate and memory budget on whichever revision you have. Test the other variant when a unit is available.

## Phases at a glance

| Phase | Goal | Depends on | Size |
| --- | --- | --- | --- |
| 0. Bring-up and bench checks | A booting image, a pin map you trust, and answers to the bench checks that affect architecture | Hardware | M |
| 1. Board services | I2C broker, expanders, display, touch, RTC, IMU, SD, all behind services | 0 | L |
| 2. Power and battery | Wake locks, light sleep, soft off, charge mode, fuel gauge | 1 | L |
| 3. C6 modem | Wi-Fi and BLE through `esp_hosted`, Wi-Fi kept up while the P4 sleeps | 1, 2 | M |
| 4. Graphics and MD2 base | Rotation pipeline, tokens, theme, fonts, the rendering gate, the simulator | 1 | L |
| 5. App platform | Ports broker, app lifecycle, ELF loader, ABI, packages, crash guard | 2, 4 | L |
| 6. Shell and system apps | Launcher, quick settings, Settings, Files, App Manager, Power, Port Inspector | 3, 5 | L |
| 7. Media and USB | Audio, camera, codecs, USB host classes, USB-C modes | 5 | L |
| 8. MicroPython and developer tools | Contained Python runtime, the `t5` CLI, on-device editor and REPL | 5, 6 | M |
| 9. Updates, recovery, release | A/B OTA, recovery image, safe mode, security defaults, example apps, 1.0 | all | M |

Phases 3 and 4 can run in parallel with Phase 2 once Phase 1 is done. The simulator part of Phase 4 can start on day one, since it needs no hardware.

## Phase 0: bring-up and bench checks

**Goal:** a known-good build setup, a verified pin map, and the measurements that decide the power architecture.

| # | Work item | Detail |
| --- | --- | --- |
| 0.0 | Read the chip revision | `esptool chip-id` with the Tab5 connected. This picks the `rev1` or `rev3` build variant for everything that follows [now one build, for v1.x: see "Chip revision"] |
| 0.1 | Repo, CI, toolchain | Layout above. ESP-IDF v6.1 natively, or in Docker if that fails. CI builds an empty `firmware/` for both variants and runs one host test |
| 0.2 | Minimal boot | Console on USB-C (USB Serial/JTAG), PSRAM test, chip revision and reset reason printed, partition table flashed, and the highest stable CPU and PSRAM clocks for this revision |
| 0.3 | I2C scan tool | A console command that scans G31/G32 and decodes known addresses. Answers bench check 1 (panel variant) and finds the camera address |
| 0.4 | Expander map | Read and toggle every expander pin, starting with `LCD_RST`, `TP_RST`, `SPK_EN`, `CHG_EN`, `nCHG_QC_EN`, `CHG_STAT`, `USB5V_EN`, `EXT5V_EN`, `WLAN_PWR_EN` and the antenna switch. Write it down in `t5_bsp/pins.h` |
| 0.5 | Find `PWROFF_PLUSE` | From the schematic, then confirm by pulsing it on a unit running from battery |
| 0.6 | Power button study | Bench check 3: G35 visibility of short presses, and whether a double press gives any warning. Use a logic analyser on G35 and `MPWR_EN` |
| 0.7 | Interrupt routing | Bench check 2: do RTC and IMU interrupts reach the P4? Bench check 9: SD detect and IMU wake line |
| 0.8 | Light sleep spike | Bench check 4: enter light sleep with PSRAM full of a pattern and the DSI panel on, wake on G23, G35 and G14, check the pattern and the panel |
| 0.9 | Baseline power draw | Bench check 5 for the states you can reach yet. Use a logging supply or a Joulescope-class meter that handles 8.4 V, in series with the pack |
| 0.10 | ELF from PSRAM spike | Bench check 11: load a tiny ELF from SD into PSRAM, run a benchmark loop, compare with the same code built in |
| 0.11 | Connector pins | Bench check 10: meter the `ext` and `stamp` pins and the M5-Bus BAT pin |

**Exit criteria:** the bench checks that can change the architecture (1, 2, 3, 4, 9, 11) have written answers in `docs/bench-results.md`. `pins.h` matches a real unit. The design doc is updated where an answer changed it.

**Decision points at the end of Phase 0:**

- If light sleep can't resume DSI or PSRAM cleanly, Sleep becomes "screen off plus a low CPU clock", and soft off carries more of the standby work.
- If ELF code in PSRAM is unreliable on this revision, native apps run from a reserved internal RAM region with a size cap. The design should then make MicroPython the default runtime.
- If the double press cuts power without warning, cut the autosave interval to 10 s and make all settings writes atomic straight away.

## Phase 1: board services

**Goal:** every internal chip runs behind a service, with no raw driver calls outside `components/`.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 1.1 | `t5_core` | Error codes (`T5_OK`, `T5_ERR_BUSY`, `T5_ERR_DENIED`…), event loop, logging tags, app context in thread-local storage, `t5_task_create` with tracking hooks | S |
| 1.2 | `t5_i2c` bus service | One mutex-owned bus with per-device handles, a reserved address table, timeout handling, recovery (9 clock pulses, STOP, controller reset), per-device error counters, and an optional traffic ring buffer for Port Inspector | M |
| 1.3 | `t5_bsp` expanders | Typed pin API on top of `t5_i2c`, a shadow copy of output state, and re-apply after a bus reset | S |
| 1.4 | Display | Variant probe (ILI9881C+GT911 or ST7123/ST7121), the ST712x reset sequence, DSI init, backlight PWM on G22, panel sleep and wake commands | M |
| 1.5 | Touch | GT911 and ST712x drivers behind one interface, interrupt on G23, coordinate scaling, LVGL input device | S |
| 1.6 | RTC | RX8130CE read, write and alarms, plus a monotonic "wall time valid" flag | S |
| 1.7 | IMU | BMI270 init with reset and retry, a non-blocking failure report, low-rate accelerometer stream, motion interrupt setup | S |
| 1.8 | Storage | LittleFS `/data` with power-loss tests, 4-bit SD on `/sd`, card-change polling (or detect, per bench check 9), safe unmount, a sync API used by Power | M |
| 1.9 | Device test app | Unity tests for each service on real hardware, run by `test/device` | S |
| 1.10 | I2C stress test | HIL script: hammer the bus from several tasks, short SDA briefly, check that recovery works and the right device is reported | S |

**Exit criteria:** cold boot shows a test pattern on whichever panel variant is fitted, touch dots follow the finger, and RTC time survives a reboot. The I2C stress test runs for an hour with no lockups. Every service compiles for the host with mocks.

## Phase 2: power and battery

**Goal:** the device sleeps, wakes, turns itself off, charges and knows its battery level. Every later driver plugs into this.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 2.1 | Rails | Reference-counted `t5_rail_acquire/release` for USB 5V, external 5V, Wi-Fi, speaker amp and backlight, with an owner list per rail for debugging | S |
| 2.2 | Wake locks | Lock types (screen, CPU, audio, net, rail), owner names, timeouts, and a pure-logic arbiter that picks the deepest allowed state. Host-tested | S |
| 2.3 | Suspend protocol | Ordered suspend and resume hooks for services (priority list), each with a time budget. App `on_suspend` gets 500 ms. Log any overrun | M |
| 2.4 | State machine | Active → Dim → Screen off → Sleep, with idle timers set by profile (handheld or mounted) | M |
| 2.5 | Light sleep | The 7-step sleep sequence from the design, wake source arming, resume in reverse, first frame within 300 ms measured with a GPIO toggle and scope | L |
| 2.6 | Soft off | Session save, RTC alarm program, IMU wake config if bench check 2 allows it, `PWROFF_PLUSE` pulse. Read the wake reason on boot | M |
| 2.7 | Charge mode | Entered on shutdown with USB attached: minimal UI (battery level), `CHG_EN` on, P4 deep sleep with a timer wake to check unplug and temperature. Depends on bench checks 7 and 8 | M |
| 2.8 | Fuel gauge | INA226 setup (5 mΩ shunt, averaging), 1 Hz sampling, coulomb counting, rest-voltage correction, a soft-off gap estimate from RTC time × standby draw, pack-swap detection, learned capacity in NVS. All logic host-tested against recorded traces | L |
| 2.9 | Discharge policy | The 15/10/5% and 6.5/6.3 V rules, low-power mode flags (frame cap, ripple off), and an emergency soft off that skips app callbacks | S |
| 2.10 | Charge policy | `CHG_EN` rules, fast-charge setting, cross-check of `CHG_STAT` against current direction, 80% limit with a 5% restart gap, 40 °C proxy-temperature pause | S |
| 2.11 | Power button | Short press, double press and long hold handling, matching what Phase 0 found. Download mode is never touched | S |
| 2.12 | Autosave | Settings and session written atomically (write to temp, fsync, rename) at least every 30 s and on every state change | S |
| 2.13 | Power HIL | Script that cycles sleep and wake 1,000 times, and records draw in each state with the external meter | M |

**Trace collection:** start logging INA226 data on every dev unit from the first day of Phase 2. Discharge curves from full to 6.5 V at a few loads are what tune the gauge. One full curve takes about 6 hours.

**Status (2026-11-05):** all items implemented (`t5_power`, `t5_battery`). On Unit A: the state machine, rails, wake reason, autosave and 179 sleep/wake cycles with no failed resume. Not yet run: anything on battery alone (soft off, `PWROFF_PLUSE`, charge mode, gauge against a real discharge, draw per state). Sleep keeps the CPU at full clock (see bench-results).

**Exit criteria:** 1,000 sleep and wake cycles with no failed resume. Soft off wakes from the button and from an RTC alarm, and restores the last app (a stub for now). Gauge error under 5% against a measured full discharge. Every power state's draw is recorded in `bench-results.md`.

## Phase 3: C6 modem

**Goal:** Wi-Fi and BLE through the C6, with the link staying up while the P4 sleeps.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 3.1 | C6 firmware build | `c6/` builds the `esp_hosted` slave with our additions: power-save policy and a wake line to the P4 on IO2/G14 when traffic arrives | M |
| 3.2 | Host link | `esp_hosted` on the P4, link health check, reset and re-sync after the P4 wakes | M |
| 3.3 | `t5_net` | Scan, connect, saved networks in encrypted NVS, events, DHCP, DNS, SNTP into the RTC | M |
| 3.4 | Sleep integration | Wi-Fi power save on Sleep, `WLAN_PWR_EN` off if no net lock is held, wake on C6 signal (bench check 12) | M |
| 3.5 | C6 updates | C6 image kept in its partition, version compared at boot, update over the existing link with rollback if the C6 doesn't come back | M |
| 3.6 | BLE | `t5_ble_*` basics: scan, connect, GATT client. HID keyboards over BLE can come later | M |
| 3.7 | Antenna | `t5_radio_antenna` switching internal or MMCX through E1.P0 | S |

**Status (2026-11-05):** all items implemented (`t5_net`, `t5_ble`, `c6/`). On Unit A, with the factory C6 firmware (esp_hosted 1.4.1): packets answered in Sleep within 450 ms, C6 reset to connected in about 4.8 s (two runs over 5 s), BLE scan, connect and GATT discovery. Not yet run: the C6 update round trip, which installs our C6 build for the first time.

**Exit criteria:** the P4 sleeps with a TCP connection open, and wakes on an incoming packet within 1 s. Reconnect after a C6 reset takes under 5 s. A C6 update round trip works, including a forced failure that rolls back.

## Phase 4: graphics and MD2 base

**Goal:** a rendering pipeline that meets the frame rate targets, and an MD2 layer the shell and apps can build on.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 4.1 | Simulator | `sim/` builds LVGL with SDL at 1280×720 and 720×1280, plus mocked services (battery slider, fake Wi-Fi, fake ports). Starts on day one | M |
| 4.2 | Display pipeline | RGB565, partial updates, three framebuffers, PPA rotation for landscape, tear-free swap on DSI vsync. Frame time and PSRAM bandwidth counters on screen | L |
| 4.3 | Density | `T5_DP(n)` = 2 px, layout helpers, a lint rule in CI that flags raw pixel numbers in `md/` and `t5_shell/` | S |
| 4.4 | Asset tools | Font subsetter for Roboto Regular and Medium at the 9 type sizes; Material Icons subset as a font; shadow nine-slice baker for elevations 1, 2, 4, 6, 8, 16 and 24, with dithering for RGB565 | M |
| 4.5 | `md_tokens` | Colour, type, shape, motion and elevation as plain C data shared with the simulator. Dark set first: `#121212` surface, white elevation overlays (5/9/12/16%, to check against the spec), on-dark text opacities, accent colours desaturated for dark surfaces. The light set is stubbed out with the same token names | S |
| 4.6 | `md_theme` | An LVGL theme using `lv_theme_set_parent` and `lv_theme_set_apply_cb`, dark by default, with runtime accent switching. The light/dark switch is built in but hidden until the light theme is finished | M |
| 4.7 | Ripple | Ink circle clipped to the widget, with the MD2 timings, and a fallback to a plain fade on frame overrun or in low-power mode | M |
| 4.8 | **Rendering gate** | The test screen from the design: app bar, 100-row list, card grid, FAB, open dialog. Measured on the device | M |
| 4.9 | Auto-rotate | Orientation filter (flat detection, 500 ms hold, hysteresis) as host-tested logic, lock in quick settings that survives reboot, touch remapping, `on_rotate` events | M |

**Status (2026-11-05):** all items implemented (`t5_ui`, `md`, `sim/`, `tools/assets/`, `tools/lint_px.py`). The simulator renders the gate in both orientations, and CI compares its screenshots. On Unit A the pipeline runs, but **the gate fails: 29–35 fps scrolling** against 57 (bench-results, "Phase 4"). Not yet checked: rotation in all four directions, the IMU axis mapping and the touch-to-ripple time. The ripple clips to the widget's box, not its 4 dp corners; round widgets ripple from their centre instead.

**Dark first.** Everything in Phases 4 and 6 is designed and tuned in the dark theme. In dark, elevation is mostly shown with lighter surface overlays, which are plain fills on the PPA path, so baked shadows matter less and the gate is easier to pass. Check the dark overlays for RGB565 banding early.

**The gate (4.8):** 60 fps while scrolling lists, 30 fps during full-screen transitions, touch to ripple within one frame, in both landscape and portrait. If it fails, try in this order: more nine-slices and fewer live shadows; square corners on large surfaces; a smaller list-row cache; a lower panel refresh rate with steady frame pacing. Don't start Phase 6 until this passes.

**Exit criteria:** the gate passes on hardware and runs the same in the simulator. Rotation works in all four directions with no tearing.

## Phase 5: app platform

**Goal:** install and run native apps from storage, with clean exit, permissions and crash recovery.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 5.1 | Ports broker | Named port table from `pins.h`, exclusive claims by mode, conflict errors naming the owner, shared-pin groups (`stamp` and `mbus.spi`), risky-pin flag for G35, G37 and G38, external I2C/SPI/UART/GPIO/PWM/ADC handles. Host-tested claim logic | L |
| 5.2 | Modbus helper | RTU master over `rs485` with direction control on G34 | S |
| 5.3 | Module profiles | JSON profile schema, loader, manual Scan on external buses only, suggestions | S |
| 5.4 | App context and tracker | Per-app records of heap, tasks, ports, rails, locks, timers and LVGL root, released in a fixed order on exit or crash | M |
| 5.5 | ELF loading | `elf_loader` from SD or `/data` into PSRAM, the resolver built from `sdk/abi/api_v1.sym`, tracking wrappers for allocation and tasks, a `T5_ERR_DENIED` stub for APIs the manifest didn't declare | L |
| 5.6 | Stable ABI | Generated export table, CI check that no symbol in a published API level was removed or changed, install-time check of an app's undefined symbols | M |
| 5.7 | SDK | Public headers, CMake toolchain for position-independent ELFs, `t5_app()` helper, C template, `T5_APP(...)` macro | M |
| 5.8 | Packages | `.t5app` reader (zip with manifest, icon, assets, entry), TOML manifest validation, install to `/sd/apps` or `/data/apps`, upgrade and removal keeping app data | M |
| 5.9 | Permissions | Install-time list, first-use prompts for camera and mic, stored grants, a clear note that native apps have full device access | S |
| 5.10 | Lifecycle | `on_create`, `on_resume`, `on_suspend`, `on_destroy`, `on_rotate`, plus headless `on_alarm` with `max_seconds` enforced | M |
| 5.11 | Crash guard | Running app ID kept in RTC no-init memory, checked against the reset reason at boot, back to the launcher, auto-launch blocked after two crashes, core dump kept and linked to the app | M |
| 5.12 | UI watchdog | 2 s UI-task watchdog with the "App not responding" dialog | S |

**Status (2026-11-05):** all items implemented (`t5_app`, `t5_pkg`, `t5_ports`, `sdk/`, `tools/abi/`, test apps in `test/apps/`, guide in [sdk.md](sdk.md)). Host tests cover the tracker, crash guard, ELF import scanner, ABI resolution, packages, manifests, the ports broker and Modbus framing. API level 1 exports 2,142 symbols (1,903 of them LVGL). **Complete: exit criteria met on Unit A** (bench-results, "Phase 5"): 500 open/close cycles back to baseline, the crash guard, and apps surviving firmware rebuilds. Only v1.x silicon is targeted (the rev3 variant has since been dropped). Not yet run: real Grove, RS-485 and profile hardware for the ports broker.

**Update (2026-11-08): app isolation.** Misbehaving installed apps no longer take the OS down (`t5_app/isolate.h`, `t5_core/fault.h`): CPU faults blamed on an app close it instead of rebooting, Close app on the ANR dialog no longer restarts, app heaps keep out of the OS's internal-RAM and PSRAM reserves, a PSRAM watchdog closes the app in front, and app tasks that hog a core are demoted. Verified on Unit A with `test/apps/crasher` (every mode) and `test/apps/hang` (both modes): no reboot, records written, launcher back; leaky stress still passes. See bench-results, "App isolation".

**Exit criteria:** a test app that leaks memory, claims `grove`, holds a wake lock and creates tasks and timers is opened and closed 500 times, and heap, claims and locks return to baseline every time. A test app that crashes on purpose triggers the crash guard. An app built against API 1 still loads after unrelated firmware changes.

## Phase 6: shell and system apps

**Goal:** a usable device for someone who never touches code.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 6.1 | Widgets | `t5_md_*`: app bar, buttons, FAB, card, list row with recycling, text field with floating label, switch, slider, checkbox, chip, tabs, menu | L |
| 6.2 | Shell services | Snackbar, dialog, navigation drawer, on-screen keyboard (USB-A keyboard too), permission prompts | M |
| 6.3 | Launcher | Landscape-first app grid, status bar (time, battery, Wi-Fi, BLE, mic and camera indicators), app switching with transitions | M |
| 6.4 | Quick settings | Pull-down panel: Wi-Fi, BLE, brightness, rotation lock, low-power mode, volume | M |
| 6.5 | Settings | Wi-Fi, BLE, display, sound, power, time, updates, developer mode, about with licences (Roboto OFL, Material Icons Apache-2.0) | L |
| 6.6 | Files | SD and `/data` browsing, `.t5app` install, safe eject (later also: a USB drive at `/usb`, delete, format) | M |
| 6.7 | App Manager | Installed apps, permissions, storage use, crash reports and core dumps, removal | M |
| 6.8 | Power app | Live voltage, current and watts, time remaining, charge history graph, per-app average draw, charge limit | M |
| 6.9 | Port Inspector | Pin owner, mode and level; rail state and owners; I2C traffic; module scan | M |
| 6.10 | Session restore | Last app and its saved state brought back after soft off, unless the wake came from an alarm | S |

All system apps are built as normal apps against the public SDK, compiled into the image. That tests the API with real use.

**Status (2026-11-06):** all items implemented (`md/md_controls.c`, `t5_shell`, `firmware/sysapps/`, per-app energy and session state in `t5_app`, charge history in `t5_battery`, time zones in `t5_time`), started against the plan's rule while the Phase 4 gate still fails. On Unit A every system app opens and draws (screenshots over the console with `t5 shot`), the launcher, status bar, quick settings, drawer and widget set also run in the simulator, whose screenshots CI now checks (`shell_*` goldens), and the Phase 5 stress test still passes with the shell in place. Not yet checked by a person: touch gestures, the keyboard on glass, joining a new network and setting the time by hand on the device (the console drove everything here), and per-app power (it is measured on battery only). See bench-results, "Phases 6 and 7".

**Update (2026-11-07):** a person ran the no-PC path on Unit A: install from SD, permission prompt, run, gestures, App Manager, per-app power on battery (camera ~1.8 W). It found the on-screen keyboard never appearing and the Camera app's recording crashing; both are fixed, Wi-Fi was then rejoined by touch and the time zone set. Still to do by touch: setting the time by hand after the keyboard fix, and the "Sync with the network now" row. Frame rates: scrolling passes; full-screen transitions 26.7 fps while animating (target 30, limited by PSRAM bandwidth); dialogs ~6.6 fps. See bench-results, "Phases 6 and 7 follow-up".

**Exit criteria:** a fresh device can join Wi-Fi, set the time, install an app from SD, grant permissions, run it, see its power cost and remove it, all without a PC. The shell meets the Phase 4 frame rate targets.

## Phase 7: media and USB

**Goal:** working audio, camera, codecs and USB, with reference apps.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 7.1 | Audio out | ES8388 via `esp_codec_dev`, speaker amp on `SPK_EN`, headphone detect routing, volume, power down by register on suspend, a mixer for system sounds and one app stream | M |
| 7.2 | Audio in | ES7210 dual mics, echo cancellation (`esp-sr` AFE or similar), the privacy indicator in the status bar | M |
| 7.3 | Audio priority | Audio task at top priority, underrun counter, audio wake lock that keeps playback going with the screen off | S |
| 7.4 | Camera | SC2356 via `esp_video`, CSI → ISP → PPA → display preview outside LVGL, kept upright when rotated, released on suspend | L |
| 7.5 | Codecs | Hardware H.264 recording to SD, JPEG stills, frame buffers taken from the camera budget | M |
| 7.6 | Camera app | Preview, photo, video, gallery | M |
| 7.7 | USB-A host | Hub support, HID (keyboard, mouse), mass storage to `/usb`, CDC serial, MIDI, UVC; `USB5V_EN` tied to the rail refcount | L |
| 7.8 | USB-C modes | Console by default, with mass storage or other device modes on request (bench check 6). Mass storage exposes SD only, never `/data` | M |
| 7.9 | UVC monitor | Example app showing a USB capture dongle on screen | M |

**Status (2026-11-06):** all items implemented (`t5_audio`, `t5_camera`, `t5_usb`, the Camera app, `apps/examples/uvc_monitor`). On Unit A: audio out and in with both codecs, echo cancellation, playback with the screen off, the camera with preview, JPEG and H.264 to SD. USB-A host runs (rail, hub support, all class drivers), but no USB device has been plugged in yet, and USB-C mass storage works (the SD card mounts on a Mac; the console comes back after the timeout). A USB drive on USB-A works too. Recording is 1280x720: the SC202CS has no 1080p mode (1600x1200 and 1600x900 are its 2 MP modes). See bench-results, "Phases 6 and 7".

**Update (2026-11-07):** media through 100 sleep cycles passes (250 cycles, 0 failures, 0 underruns). Apps run again (an XIP/memory-protection change had broken every native app) and Wi-Fi no longer stalls (esp_hosted vs the factory C6 firmware). 10 minutes of 720p recording on Unit A's card: 14 frames dropped with adaptive bitrate (2,053 without); the card (FAT32, 512-byte clusters) stalls for seconds, so a reformat with 32 KB clusters is the remaining step. The exit criterion says 1080p; the sensor's top mode at 30 fps is 720p. 1 h playback with the screen off: see bench-results. Keyboard, flash drive and UVC dongle through a hub: not tried (no hub).

**Exit criteria:** 1 hour of audio playback with the screen off and no underruns. 10 minutes of 1080p H.264 recording to SD with no dropped frames reported. Keyboard, flash drive and UVC dongle work through a hub. Media services suspend and resume through 100 sleep cycles.

## Phase 8: MicroPython and developer tools

**Goal:** on-device coding, and a fast loop from a PC.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 8.1 | MicroPython port | Embedded in the firmware, one VM per Python app with a heap from the app's PSRAM budget, runs as an app under the same tracker | L |
| 8.2 | Containment | Built without `machine`, `uctypes`, native emitters and inline asm. Tests show that no frozen module reaches raw hardware | S |
| 8.3 | `t5` bindings | Generated from the SDK headers where possible: `t5.port`, `t5.ui` (a light wrapper over `t5_md_*`), `t5.net`, `t5.power`, `t5.time`, `t5.audio`, `t5.app` | L |
| 8.4 | Editor and REPL | On-device editor with syntax colouring, REPL in Terminal, works with a USB-A keyboard | M |
| 8.5 | Developer mode | Opt-in, pairing with a code shown on screen, token auth, a small authenticated HTTP or WebSocket endpoint for push, launch and logs | M |
| 8.6 | `t5` CLI | `new`, `build`, `run` (Wi-Fi first, USB fallback), `sim`, `pack` (signed or unsigned) | M |
| 8.7 | Docs | SDK guide, API reference generated from headers, porting notes for Python versus C | M |

**Exit criteria:** `t5 new` → `t5 run` shows a C app on the device in under 30 s with logs streaming. A Python app using `grove` I2C runs from the on-device editor. A Python app can't reach any hardware except through `t5`.

**Status (Nov 7):** built and run on Unit A. `t5 new` → `t5 run` in 2.7 s; the containment checks pass (`py test`, and a build-time module allowlist); the editor runs Grove I2C code (no sensor was attached). Deviation: `t5.ui` wraps the MD2 widget set by hand rather than generating it; the scalar API (power, time, audio, IMU, USB, app) is generated from `api_v1.sym`. See [bench-results](bench-results.md#phases-8-and-9--2026-11-07).

## Phase 9: updates, recovery and release

**Goal:** nothing a user does needs a USB reflash to fix.

| # | Work item | Detail | Size |
| --- | --- | --- | --- |
| 9.1 | A/B OTA | Download over Wi-Fi or install from SD, signature check, write to the inactive slot, check that the image's revision variant matches the chip, health check on first boot (services up, shell drawn, 60 s stable) before marking it valid, otherwise roll back | M |
| 9.2 | Recovery image | Small separate image with display, touch, SD and LittleFS. Reinstall the OS from SD, clear settings, export logs and core dumps. Entered after three failed boots or from a flag file on SD | M |
| 9.3 | Safe mode | Boot without user apps or auto-launch, showing the last crash | S |
| 9.4 | Security defaults | NVS encryption with a device-bound key, no listening services unless developer mode is on, the eFuse options documented as one-way and left off | S |
| 9.5 | Example apps | RS-485 console, scheduled logger (hourly wake, Grove read, upload, back to soft off), UVC monitor from Phase 7 | M |
| 9.6 | Long-run tests | 7-day soak with mixed use, 1,000 OTA cycles with forced failures, power pulled during OTA and during `/data` writes | M |
| 9.7 | Release | Tagged firmware, SDK and CLI packages, a flashing guide, licence file | S |

**Exit criteria:** power pulled at random points during an update always leaves a bootable device. The scheduled logger runs for a week on one charge, with its draw measured. 1.0 is tagged with API level 1 frozen.

**Status (Nov 7):** 9.1–9.5 built and run on Unit A: signed A/B updates with a trial boot and rollback, the boot counter in a custom bootloader, safe mode, the recovery image, chip-bound credential sealing (NVS encryption proper would burn an eFuse key, so credentials are sealed before they reach NVS instead; [security.md](security.md)), and the RS-485 Console and Logger examples. 9.6 and 9.7 are tooling (`test/hil/ota_cycles.py`, `test/hil/soak.py`, `tools/release.sh`, [flashing.md](flashing.md)) with short runs only; the 7-day soak, 1,000 OTA cycles, real power cuts, the logger week and the 1.0 tag are still to do.

## Testing strategy

| Layer | What it covers | Where it runs |
| --- | --- | --- |
| Host unit tests | Broker claims, wake-lock arbiter, fuel gauge (against recorded traces), orientation filter, manifest parser, ABI diff, discharge and charge policies | CI on every push |
| Simulator | Shell and MD2 widgets, screenshot comparisons for the dark theme in each orientation (light added after 1.0) | CI on every push |
| Device tests | One Unity test app per service, run by a person or a HIL runner | Before each merge to main that touches `components/` |
| HIL bench | Sleep cycling, power draw per state, I2C stress, OTA power-pull, audio and camera endurance | Nightly on one wired-up unit, once one exists |
| Performance | Frame time, first frame after wake, boot time, image size, free heap after boot | Tracked per build, with a CI warning on regressions |

Suggested bench kit: a second Tab5 for HIL, a logging bench supply or Joulescope-class meter rated for 8.4 V, a logic analyser, a USB hub, a UVC capture dongle, a Grove I2C sensor and a USB-to-RS-485 adapter.

## Risks

| Risk | Effect | Early warning | Fallback |
| --- | --- | --- | --- |
| Light sleep breaks DSI or PSRAM on this revision | Sleep is not really sleep | Phase 0.8 | Screen off at low clock, more use of soft off |
| ELF in PSRAM unreliable on this revision | Native apps limited | Phase 0.10 | Internal RAM region with a size cap, MicroPython first |
| MD2 rendering misses the frame budget | UI feels slow | Phase 4.8 gate | The fallbacks listed under the gate |
| Double press cuts power with no warning | Data loss | Phase 0.6 | Atomic writes and short autosave interval |
| C6 can't hold Wi-Fi with the P4 asleep | No background sync in Sleep | Phase 3.4 | Scheduled wake from soft off for background work |
| LVGL upgrade regressions | Broken apps | ABI diff and screenshot tests | Pin per API level, as planned |
| Image built for the wrong chip revision | Device won't boot after an update | Phase 0.0, OTA check | Updater and recovery refuse mismatched images; recovery is built per variant too |
| Fuel gauge drift on third-party packs | Bad battery figures, early shutdown | Phase 2 trace logging | Conservative voltage floor, capacity learning over cycles |
| One shared I2C bus | One stuck device takes down the board | Phase 1.10 stress test | Recovery, per-device error counts, disable a failing device |

## First two weeks

1. Answer the open decisions, or accept the defaults in the gaps table.
2. Read the chip revision (0.0). Install ESP-IDF v6.1 natively (Docker `espressif/idf:v6.1` if that fails), create the repo layout with both `rev1`/`rev3` configs [now one build, for v1.x], pin the components, commit `partitions.csv`, set up CI (0.1).
3. Start the simulator and `md_tokens` (4.1, 4.5), which need no hardware.
4. On the Tab5: minimal boot, I2C scan, expander map (0.2–0.4).
5. Start the bench checks that can change the architecture: power button, interrupt routing, light sleep, ELF in PSRAM (0.6–0.8, 0.10).
6. Put an INA226 logger on the unit so discharge traces build up while other work goes on.
