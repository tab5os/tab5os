# Tab5 OS: design for the M5Stack Tab5

Nov 5, 2026 · @Kanishka

## Summary

Build a single-foreground, service-backed OS from scratch on ESP-IDF and FreeRTOS, with a Material Design 2 interface on LVGL and apps loaded from storage without reflashing. Every port, sensor and power rail sits behind one OS service, so apps never fight over hardware. Treat the ESP32-C6 as an always-connected modem so the ESP32-P4 can sleep while Wi-Fi stays up.

This is a design, not a tested build. Details traced from the schematic need confirming on a real unit, and they are listed under Bench checks.

| Decision | Choice | Why |
| --- | --- | --- |
| Base (decided) | Our own OS on ESP-IDF and FreeRTOS | Best access to the P4 media blocks, the C6 and the board's power logic |
| Interface (decided) | Material Design 2 on LVGL | LVGL handles rendering and input, so the effort goes into the Material look and motion |
| Foreground model | One full-screen app, plus background services | Matches the memory bandwidth and avoids a window compositor |
| Hardware access | Named ports with a claim broker | Pins and the main I2C bus are shared, so ownership has to be explicit |
| Sleep | Light sleep for quick resume, soft off for long standby | Soft off cuts the main rails, which P4 deep sleep cannot do |
| Battery | Software fuel gauge on the INA226 | The board has no gauge chip or battery thermistor |
| User apps | Native ELF and MicroPython, with WebAssembly as a later option | ELF gives full speed, Python gives easy on-device coding, WebAssembly could add real isolation |
| Updates | A/B firmware slots plus a recovery image | A bad update or app must never need a USB reflash |

## Hardware facts that shape the design

The Tab5 is not a generic ESP32 board, and seven details decide most of this design. The Basis column says whether each comes from vendor docs, a schematic trace or another source.

| Hardware fact | What the OS must do | Basis |
| --- | --- | --- |
| A small PMS150G MCU drives `MPWR_EN` for the main rails. A standby rail keeps it, the RTC and the IMU alive. | Use soft off for long standby. Wake it from the button, RTC alarm or IMU. | Schematic, docs |
| Charging works only after the device has booted, and `CHG_EN` is on an I2C expander. | Run a charge mode when shutdown is requested with USB attached. | Docs |
| Wi-Fi, USB-A 5V, external 5V, the speaker amp and the backlight can be switched. The camera LDOs, audio LDO and SD supply have no switch. | Power down the codecs and camera by register, and accept some draw in light sleep. | Schematic |
| The INA226 `ALERT` pin looks unconnected and the charger's `NTC` pin sits on a fixed divider. | Poll in software, estimate charge in software, and use proxy temperatures. | Schematic |
| One I2C bus (G31/G32) serves nearly every internal chip and also reaches the M5-Bus, RS-485 connector and Stamp pads. | Broker all access, recover from stuck buses and block reserved addresses. | Docs, schematic |
| USB-A uses the high-speed pair. USB-C and the MX1.25 connector share a GPIO24/25 pair, which is full speed. | Put fast peripherals on USB-A. Use Wi-Fi for large transfers to a PC. | Schematic |
| The panel is natively 720×1280 portrait, with ILI9881C/GT911 and ST7123/ST7121 variants. | Probe the variant at boot and rotate efficiently for landscape. | Docs, NuttX |

Two chip-level limits matter as well. NuttX reports that early Tab5s ship with ESP32-P4 revision v1.0, which needs ESP-IDF's pre-v3 silicon option and runs at the board's 360 MHz. Newer units may have v3.x, which can't run the same image; the OS builds for v1.x only (`firmware/sdkconfig.board`). We build on ESP-IDF v6.1. The chip also has memory protection but no per-process virtual memory, so a native app shares one address space with the OS.

## Base platform decision

We are building our own OS from scratch on ESP-IDF. We write the services, app model, shell and UI, and keep FreeRTOS and Espressif's low-level drivers.

| Option | Tab5 status | Strength | Limit | Verdict |
| --- | --- | --- | --- | --- |
| ESP-IDF plus custom OS layer | Espressif and espp both publish Tab5 board packages | Full control of power, media blocks and the C6 | You build the shell, services and SDK | **Chosen** |
| [Tactility](https://github.com/TactilityProject/Tactility/releases/tag/v0.7.0) fork | Tab5 added in v0.7.0, with USB host and mass storage | Launcher, apps, SDK and simulator already exist | Multi-device design, GPL core, Tab5 power work still needed | Not chosen; useful as a reference |
| [NuttX](https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32p4/boards/esp32p4-tab5/index.html) | Display, touch, expanders and PSRAM work | POSIX-style OS and shell | Audio, camera, INA226, RTC and C6 Wi-Fi are not implemented | Revisit later |
| Zephyr | I found other P4 boards but no Tab5 board definition | Strong driver model | Board bring-up comes first | Not now |

The custom route is worth it because the board-specific parts are the product: soft off, charge mode, the C6 modem split and the port broker. Generic device support would add little here. Reuse Espressif's components (`esp_hosted`, `esp_video`, `esp_codec_dev`, `elf_loader`, the LVGL adapter) rather than rewriting drivers.

## Architecture

The OS is a layered system with one rule: apps call services, and only services touch drivers. That rule makes power control, port sharing and app cleanup possible.

&#91;embedded content: OS layers, from user apps down to hardware\]

The C6 runs its own firmware, so the P4 can sleep while the C6 keeps the Wi-Fi link. The standby domain outlives everything else and can restart the device.

### Core and task layout

| Where | Runs | Rule |
| --- | --- | --- |
| HP core 0 | System services, C6 link, USB, storage | Keeps slow I/O off the UI core |
| HP core 1 | LVGL rendering, foreground app | Apps get a time slice but cannot starve touch input |
| Either HP core | Audio, at the highest priority | Audio underruns are audible, so it outranks UI |
| LP core | Optional pin watching during deep sleep | It likely cannot reach the G31/G32 bus, so no battery polling |

### Graphics pipeline

Render in RGB565 with partial updates, and use the PPA for rotation, scaling and blits. A full landscape frame is 1280 × 720 × 2 = 1.84 MB, or about 111 MB/s at 60 fps. Memory bandwidth is the reported bottleneck, so aim for 60 fps on partial updates and at least 30 fps on full-screen redraws.

Camera preview should run CSI → ISP → PPA → display without passing through LVGL. Video recording should use the hardware H.264 encoder, and photos the JPEG codec.

### Memory budget (32 MB PSRAM)

These are starting targets to tune. Internal L2 memory (768 KB, part of it used as cache) is reserved for interrupt code, DMA descriptors and timing-critical tasks.

| Use | Size (MiB) | When |
| --- | --- | --- |
| Three framebuffers, needed for rotated tear-free output | 5.3 | Always |
| LVGL draw buffers and image cache | 3.0 | Always |
| Services, TLS and network buffers | 2.5 | Always |
| Audio buffers | 0.5 | While audio runs |
| Camera frames and encoder output | 6.5 | While the camera runs |
| App heap, with audio and camera idle | 21.2 | Remainder |
| App heap, with audio and camera active | 14.2 | Remainder |

### Flash layout (16 MB)

| Partition | Size (MiB) | Purpose |
| --- | --- | --- |
| Bootloader and partition table | 0.07 | Boot chain. The table sits at 0x10000, not 0x8000, so the bootloader has room for the recovery hook and secure boot |
| NVS and OTA data | 0.26 | Settings, credentials, boot slot |
| Recovery | 1.50 | Repairs the device without a PC |
| `ota_0` and `ota_1` | 4.00 each | A/B OS images |
| C6 firmware | 1.50 | Image used to update the radio chip |
| Core dump | 0.25 | Crash evidence that survives reboot |
| `/data` (LittleFS) | 4.35 | Power-safe settings, system apps and app data |

User apps and media default to the SD card, with `/data` used for small apps that must work without a card.

## Access to every port, connector and sensor

Every hardware feature gets a service and an API, and apps address ports by name instead of raw pin numbers. Only the Ports service knows the pin map, so a board revision changes one table.

### Built-in hardware

| Hardware | App API | What the OS handles |
| --- | --- | --- |
| Display and backlight (G22) | LVGL, `t5_display_*` | Variant probe, orientation, brightness, the ST712x reset sequence |
| Touch (G23) | LVGL input, gestures | GT911 or ST712x driver, scaling |
| Camera, SC2356 2 MP | `t5_camera_*` | CSI, ISP, preview path, H.264 and JPEG |
| Speaker and headphone jack | `t5_audio_out_*` | ES8388, amp enable, routing from headphone detect |
| Dual microphones | `t5_audio_in_*` | ES7210, echo cancellation, a privacy indicator |
| IMU, BMI270 | `t5_imu_*` | Auto-rotate, motion wake, init retry after power loss |
| RTC, RX8130CE | `t5_time_*`, `t5_power_schedule_wake` | Network time sync, alarms, wake scheduling |
| INA226 and IP2326 | `t5_battery_*` (read-only) | Fuel gauge, charge control, safety cutoffs |
| microSD | Normal file paths under `/sd` | 4-bit SDIO, polling for card changes, safe unmount |
| Wi-Fi 6 and BLE | `t5_net_*`, `t5_ble_*` | `esp_hosted`, credentials, power save, radio power |
| Thread and Zigbee | `t5_radio_profile_*` | A separate C6 firmware profile, so this is later work |
| Antenna switch | `t5_radio_antenna` | Internal or MMCX, using expander pin E1.P0 |
| USB-A host | `t5_usb_host_*` | HID, storage, UVC, serial, MIDI and hubs, plus a 5V rail switch |
| USB-C and MX1.25 | `t5_usb_mode` | Dev console by default, with storage or other device modes on request |
| Power button | System events | Sleep, wake and shutdown, with download mode left untouched |

### External ports

| Port | Signals | App modes | Power |
| --- | --- | --- | --- |
| `grove` (HY2.0-4P) | G53, G54 | I2C, UART, GPIO, PWM | Switched 5V |
| `mbus.spi` | G18 MOSI, G19 MISO, G5 SCK | SPI | 3V3, 5V, HVIN, BAT pins |
| `mbus.uart` | G38 RX, G37 TX | UART | As above |
| `mbus.debug` | G7 RX, G6 TX | UART, or a fallback console | As above |
| `mbus.gpio` | G2, G3, G4, G16, G17, G35, G45, G47, G48, G51, G52 | GPIO, PWM, ADC where the pin allows | As above |
| `ext` (2.54 10-pin) | G0, G1, G49, G50 | GPIO, UART, I2C, PWM | HVIN, 5V, 3V3 |
| `rs485` (HC1.25-6P) | G20 TX, G21 RX, G34 DIR | RS-485, with a Modbus helper | 6–24V input |
| `stamp` (pads) | G33 CS, G5, G18, G19, G6, G46 | SPI, UART, GPIO for Cat-M, NB-IoT or LoRaWAN | 5V, 3V3 |

The `ext` and `stamp` pin lists come from the schematic netlist rather than the docs' pin tables, so confirm them with a meter.

### Port broker rules

1. **Claims are exclusive.** An app claims a port or pin set in a mode, and a conflict returns an error naming the current owner.
2. **Exit releases everything.** Closing or crashing an app frees its claims, tasks and rails.
3. **Internal I2C is never raw.** Apps get a device handle, and the broker refuses reserved addresses 0x10, 0x14, 0x28, 0x32, 0x36, 0x40, 0x41, 0x43, 0x44, 0x55, 0x5D and 0x68. 0x28 is a second address of the ST7123 display and touch chip, and 0x36 is the SC2356 camera (both confirmed on Unit A).
4. **The bus recovers itself.** A stuck bus gets clock pulses and a controller reset, then the service reports which device failed.
5. **Rails are reference counted.** The external 5V rail feeds Grove, M5-Bus and `ext` together, so it stays on until the last user releases it.
6. **Shared pins conflict by design.** Claiming `stamp` SPI also blocks `mbus.spi`, because they use the same pins.
7. **Risky pins need a flag.** G35 is the boot and power line, and G37 and G38 are strapping pins. Modules that pull them at boot can change the boot mode.

The RS-485 terminator is a physical DIP switch, so the OS can only explain it, not set it.

```c
t5_port_t *grove;
t5_i2c_cfg_t cfg = { .hz = 400000 };

if (t5_port_open("grove", T5_MODE_I2C, &cfg, &grove) == T5_OK) {
    t5_rail_acquire(T5_RAIL_EXT5V);
    uint8_t buf[6];
    t5_i2c_read_reg(grove, 0x44, 0x00, buf, sizeof buf);
}
// Port and rail are released on t5_port_close() or app exit.
```

### Module support

Store each M5-Bus module or Grove unit as a small JSON profile: name, port, pins, power needs and driver. When a person taps Scan, the OS scans only external buses and suggests matching profiles. Scanning stays manual because probing can disturb some devices.

The system Port Inspector app shows each pin's owner, level and mode, plus rail state and live I2C traffic. It doubles as a debugging tool for people writing apps.

## Sleep and wake

Use five power states plus a charge mode, and make light sleep and soft off the two that matter. P4 deep sleep alone is a poor standby: it loses RAM like a reboot does, yet the main rails stay powered.

&#91;embedded content: Power states and how each returns to Active\]

Apps do not choose a state. They hold wake locks, and the Power service picks the deepest state those locks allow.

| State | Enter when | Stays on | Wakes on | App state |
| --- | --- | --- | --- | --- |
| Active | Input or a foreground wake lock | Whatever is claimed | n/a | Running |
| Dim | 30 s idle by default | As Active, with lower backlight and clock | Touch, button | Running |
| Screen off | 2 min idle, or the power menu | Backlight off; panel and video stay on so touch works; audio, radio and CPU only if locked | Touch, timer | Foreground paused, services run |
| Sleep | Screen off with no CPU or audio lock, 10 s on | Backlight off; the video stream to the panel stays alive (touch needs it); CPU idles in WFI at full clock (IDF's DSI driver pins it, measured); C6 off, or in modem power save if a net lock is held; codecs, camera and switched rails off | Touch, timer, C6 traffic, claimed M5-Bus pins | Suspended by hooks, resumes in place |
| Soft off | Shutdown, critical battery or 24 h asleep | Standby rail: PMS150G, RTC, IMU | Button, RTC alarm, IMU | Saved, then restored after boot |
| Charge mode | Shutdown with USB attached | Charger enabled, P4 in deep sleep | Button, unplug check | None |

The RTC interrupt goes to the PMS150G power controller, not to the P4 (bench check 2, measured on Unit A). Each `/IRQ` pulse toggles main power: it switches a soft-off board on, **and it switches a running board off**. So the OS keeps RTC alarm and timer interrupts disabled whenever it runs, and arms them only as the last step before soft off. In Sleep, use the P4's own timer. The IMU interrupt, when set active-low, drives the same trigger (measured), so the same rule applies: motion wake only from soft off, with INT active-low, and IMU interrupts off or active-high while the OS runs.

Full light sleep isn't usable as Sleep. The ST7123 scans touch only while the P4 sends it video, and light sleep stops the video stream, so touch can't wake it, and the button power-cycles the board (bench 0.8). Sleep therefore keeps the video stream running with the backlight off and the CPU idle at low clock. Soft off does long standby.

Vendor figures imply about 2.5 W in typical use: 14.8 Wh over about 6 hours. For standby planning, a steady 100 mW would last about 6 days and 10 mW about 2 months. Actual sleep and soft-off draw is unmeasured.

### Sleep sequence

1. Send `on_suspend` to apps, with a 500 ms budget.
2. Sync `/data` and the SD card, and stop LVGL rendering.
3. Stop the camera, power down the ES8388 and ES7210 by register, and set `SPK_EN` low.
4. Turn off the backlight, send the panel its sleep command and stop DSI.
5. Put the C6 in power save, or drop `WLAN_PWR_EN` if no app needs the network.
6. Drop `USB5V_EN` and `EXT5V_EN`, unless a lock holds them.
7. Arm the wake sources and enter light sleep.

Resume runs this in reverse. Aim for a first frame within 300 ms, measured from a real wake.

### Soft-off sequence

Save the session and battery estimate, program the RTC alarm and IMU wake, record the reason in the RX8130's four bytes of RAM, then toggle `PWROFF_PLUSE` (expander 0x44, P4, per the schematic) ten times at 50 ms, as M5Unified does. If power is still on 1.5 s later (USB holding the rails up), soft off undoes itself and reports failure. On the next boot, read the wake reason and restore the last app unless the wake came from a background alarm.

The wake reason comes from the RTC flags, which `t5_rtc_init()` records before clearing them. A power-up caused by the PMS150G reports reset reason `ESP_RST_WDT` on the P4, not power-on. The RX8130 countdown timer reloads itself, so boot must stop it early, or its next pulse switches the board off again.

### Power button

| Action | Result |
| --- | --- |
| Press while off | Power on, handled by the PMS150G |
| Short press while on | **Power-cycles the board** (measured, bench 0.6). The OS gets no warning, so it can't use the press. After the reboot, restore the session |
| Double press while on | Power off, handled by the PMS150G with no warning to the OS |
| Long hold | Download mode, which the OS must never reassign |

The OS can't turn the button into a screen-off or sleep key on this hardware. Screen off comes from idle timers and the on-screen power menu.

If the PMS150G cuts power on a double press without warning, the OS cannot run a clean shutdown. Autosave settings and session state regularly so that case loses little.

G35 is wired to the PMS150G as well as the P4. If its pad changes state in sleep (ESP-IDF isolates pads by default, and arming it as a wake source changes its sleep config), the PMS150G reads a key press and resets the board. `t5_bsp_bringup()` therefore keeps G35's active config through sleep (`gpio_sleep_sel_dis`). With that rule, arming G35 as a light-sleep wake source is safe (bench check 4).

### Power API

```c
t5_wakelock_t *lock = t5_wakelock_acquire(T5_LOCK_AUDIO, "podcast");
t5_wakelock_release(lock);

// Wakes the device from Soft off, runs the app's background entry, then returns to sleep.
t5_power_schedule_wake(next_hour, T5_WAKE_BACKGROUND);

t5_wake_reason_t why = t5_power_wake_reason();
```

Scheduled wake is a strong feature for this board. A sensor logger can wake hourly, read a Grove sensor, upload over Wi-Fi and return to soft off without keeping the system powered between readings.

## Battery management

The OS must act as the fuel gauge and protect the pack from over-discharge. Below 6 V the pack enters protection, and vendor recovery means removing it, resetting the device, refitting it and trickle charging.

### Charge estimate

The INA226 sits on the battery rail with a 5 mΩ shunt, where positive current means discharging. Combine three methods:

- **Awake:** sample once per second and integrate current over time.
- **At rest:** correct the estimate from a voltage curve once current has stayed low for several minutes.
- **After sleep or soft off:** the INA226 is unpowered in soft off, so subtract elapsed RTC time × measured standby draw, then correct from voltage.

Learn real capacity over full charge cycles, because third-party NP-F550 packs vary. If voltage jumps at boot, assume a swapped pack and reset the estimator from voltage.

### Discharge policy

Vendor figures give 8.23 V full and a 6.0 V shutdown threshold, while the block diagram lists a 6.5 V minimum. These defaults stay clear of protection, and need tuning against a measured curve.

| Trigger | Action |
| --- | --- |
| 15% | Warn once |
| 10% | Low-power mode: dim the screen, cap frame rate, pause background apps |
| 5%, or 6.5 V under light load | Save the session and enter soft off |
| 6.3 V at any time | Immediate soft off, skipping app callbacks |

Soft off still draws from the pack, so a device stored for months could reach protection. Show this once in the shutdown help, and advise removing the pack for long storage.

### Charging

| Control | Signal | OS behaviour |
| --- | --- | --- |
| Charge enable | `CHG_EN`, expander pin E2.P7 | On when USB is present, unless a limit or temperature rule stops it |
| Fast charge | `nCHG_QC_EN`, E2.P5 | Off by default, enabled in settings |
| Charge status | `CHG_STAT`, E2.P6 | Cross-checked against INA226 current direction |
| Charge limit | Software | Optional 80% stop with a 5% restart gap, for permanently mounted screens |

The board has no battery temperature sensor. The design asked for a 40 °C pause on the BMI270 or P4 sensor, but on Unit A the BMI270 idles at 42–43 °C on USB at room temperature, so that rule would never charge. Charging pauses at 50 °C on the BMI270 and resumes at 47 °C, roughly 40 °C ambient. A better proxy (rise above a cold-boot baseline) is still open.

### Pack swaps and power loss

The vendor says to shut down before removing power, and to wait 5 seconds after any abrupt loss or the IMU may fail to initialise. The IMU driver should reset and retry at boot, and report a failure rather than block startup. Autosave limits what a yanked pack can lose.

### Power settings screen

Show voltage, current, watts, an estimated time remaining and a charge history graph. Also record average draw while each app is in the foreground. People writing apps then see their power cost directly, without external meters.

## User apps

Ship two runtimes on one API: native ELF for performance and MicroPython for easy coding on the device. Neither requires reflashing the OS. Be honest in the UI about the difference, because only one of them can be contained.

| Runtime | Best for | Containment | Trade-off |
| --- | --- | --- | --- |
| Native ELF (C, C++, Rust later) | Camera, games, emulators, drivers | None: it shares the OS address space | A bad pointer usually just closes the app; a stray write can corrupt the OS |
| MicroPython | Sensor scripts, dashboards, learning, on-device edits | Enforced, if the OS exposes only `t5` bindings and no raw `machine` access | Slower, with more memory per app |
| WebAssembly (later spike) | Untrusted third-party apps | Strong, in principle | Needs testing on this chip for speed and API overhead |

Espressif's `elf_loader` is the base for native apps. Recent releases add ELF validation, PSRAM execution fixes for the P4 and streaming relocation from SD. Pin version 2.1 or later, then test on revision v1.0 silicon, since much recent P4 work targets v3.

### Package format

A `.t5app` file is a zip containing a manifest, an icon, assets and one entry point. Install by copying it to the SD card, downloading it in the App Manager or pushing it from a PC.

```toml
id = "com.kanishka.airlog"
name = "Air Log"
version = "1.2.0"
api = 1
runtime = "native"          # or "micropython"
entry = "app.elf"
min_heap_mb = 2

[permissions]
ports = ["grove"]
net = true
background_wake = true

[background]
entry = "on_alarm"
max_seconds = 30
```

### Permissions

The OS shows requested permissions at install and asks again the first time an app uses the camera or microphone. The groups are `camera`, `mic`, `net`, `ble`, `ports`, `usb`, `storage`, `background_wake` and `system`.

For native apps, these are consent and correctness checks, not a security boundary. The loader's symbol resolver can bind undeclared APIs to a stub that returns `T5_ERR_DENIED`, but native code could still access hardware registers directly. The installer should say plainly that a native app has full device access.

### Stable ABI

Export only versioned `t5_*` symbols and the OS's LVGL build. An app needing an unknown symbol is rejected at install, not at runtime. Pin the LVGL version per API level, because upgrades can regress: Tactility rolled back from LVGL 9.4 to 9.3 for that reason.

### App lifecycle

| Callback | Called when | Expected work |
| --- | --- | --- |
| `on_create` | App is launched | Allocate resources, build UI |
| `on_resume` | App comes to the foreground or the device wakes | Reopen hardware, restart timers |
| `on_suspend` | App leaves the foreground or the device sleeps | Save state, stop timers, release the camera |
| `on_alarm` | Scheduled wake fires | Short headless work |
| `on_destroy` | App exits | Final cleanup; the OS reclaims anything left |

The OS tracks each app's heap, tasks, ports, rails, locks and LVGL objects, so exit always cleans up. Apps must create tasks through `t5_task_create` for that tracking to work.

There is no process isolation, but a native app's crash doesn't take the OS down: a CPU fault blamed on the app (its code is on the faulting call chain, or the task is one the app created) moves the faulting task to a recovery point instead of panicking, and the app is closed. Hangs on the UI task are stopped the same way, app heaps stay out of the OS's memory reserves, and a task that hogs a core is moved to the lowest priority (see sdk.md, "Crashes and hangs"). What can't be pinned on the app still reboots; the crash guard then records the app that was running, returns to the launcher and blocks auto-launch after two crashes. The core dump stays available in the App Manager.

### What an app looks like

```c
#include <t5/app.h>
#include <t5/ui.h>

static void on_create(t5_app_t *app) {
    lv_obj_t *label = lv_label_create(t5_app_screen(app));
    lv_label_set_text(label, "Hello, Tab5");
    lv_obj_center(label);
}

T5_APP(.on_create = on_create);
```

```python
import t5

port = t5.port.open("grove", mode="i2c", hz=400_000)
screen = t5.ui.screen()
label = screen.label("Reading sensor")

while t5.app.running():
    temp = port.read_reg(0x44, 0x00, 2)
    label.set_text(f"Raw: {temp.hex()}")
    t5.sleep_ms(1000)
```

### Developer workflow

Provide one CLI, with deployment over Wi-Fi as the main path because USB-C is the slower link.

| Command | Action |
| --- | --- |
| `t5 new` | Create an app from a C or Python template |
| `t5 build` | Build a position-independent ELF against the SDK |
| `t5 run` | Push, launch and stream logs over Wi-Fi or USB |
| `t5 sim` | Run the UI on a PC using LVGL's SDL driver with mocked hardware |
| `t5 pack` | Produce a signed or unsigned `.t5app` |

Developer mode is opt-in, requires pairing and is off by default. On the device itself, ship a Python editor and REPL that work with a USB keyboard on the USB-A port.

## Interface: Material Design 2

Build Material Design 2 as a token layer, an LVGL theme and a small widget set, so apps get the look by default. The main technical risk is rendering: rounded corners and soft shadows cannot use the P4's PPA in LVGL today, so they must be budgeted carefully.

This follows the MD2 spec without being a Google product, so the OS should describe itself as "Material-inspired". Numbers below come from Material Components Web 14, Google's own MD2 code, because the m2.material.io pages would not load here.

&#91;embedded content: Launcher mock-up, drawn to Material 2 sizes\]

### Density

Use a fixed 2 pixels per dp. True density would be 294 PPI ÷ 160 = 1.84, but a whole number keeps 1 dp hairlines, icons and text sharp. All apps then see 640 × 360 dp in landscape and 360 × 640 dp in portrait.

The screen is short, so landscape defaults to the 48 dp dense app bar. Apps use dp through `T5_DP(n)`, never raw pixels.

### Auto-rotate

The OS rotates the screen in all four orientations using the BMI270 accelerometer, and a quick settings toggle locks it. Portrait is the panel's native direction, so landscape is the rotated case and pays for it with the third framebuffer in the memory budget.

- **Detection:** read gravity at low rate only while the screen is on, and ignore readings while the device lies flat.
- **Stability:** change only after the new orientation holds for about 500 ms, with hysteresis so that 45° tilts do not flicker. These are starting values to tune.
- **Apps:** a manifest entry sets `orientation = "any"`, `"landscape"` or `"portrait"`. Apps set to `"any"` receive an `on_rotate` callback, and LVGL flex and grid layouts reflow on their own.
- **Touch:** input coordinates rotate with the display, and the camera preview keeps its image upright.
- **Mounted use:** a lock survives reboot, so a tripod or wall-mounted panel never turns by accident.

### Tokens

| Token group | Values | Notes |
| --- | --- | --- |
| Grid | 8 dp baseline, 4 dp for small details | 16 dp screen margin, 8 dp gutter (our choice) |
| Light colours | Primary `#6200EE`, secondary `#018786`, surface `#FFFFFF`, error `#B00020` | Baseline placeholders until we choose a palette |
| Text on light | 87% primary, 54% secondary, 38% disabled | As set in MDC |
| Text on dark | 100% primary, 70% secondary, 50% disabled | As set in MDC |
| Shape | Small and medium 4 dp, large 0 dp | FAB is fully round |
| Icons | 24 dp, or 48 px | Material Icons, compiled to a font subset |

Dark surfaces use `#121212` plus a white overlay that grows with elevation: about 5% at 1 dp, 9% at 4 dp, 12% at 8 dp and 16% at 24 dp. These dark values are from my own knowledge, not a page I could open, so check them against the spec.

### Type

| Role | Size / line (dp) | Weight | Size on panel (px) | Use |
| --- | --- | --- | --- | --- |
| Headline 4 | 34 / 40 | Regular | 68 | Clock, large numbers |
| Headline 5 | 24 / 32 | Regular | 48 | Page titles |
| Headline 6 | 20 / 32 | Medium | 40 | App bar, dialog titles |
| Subtitle 1 | 16 / 28 | Regular | 32 | List items |
| Body 1 | 16 / 24 | Regular | 32 | Main text |
| Body 2 | 14 / 20 | Regular | 28 | Secondary text |
| Button | 14 / 36 | Medium, sentence case | 28 | Buttons, snackbar actions |
| Caption | 12 / 20 | Regular | 24 | Hints, status bar |
| Overline | 12 / 32 | Medium, sentence case | 24 | Section labels |

Ship Roboto Regular and Medium as precompiled LVGL bitmap fonts at these sizes, so UI text needs no font engine at runtime. The larger Headline 1 to 3 sizes (96, 60 and 48 dp) stay out of the system image. Add FreeType later for other scripts or free scaling.

### Components

| Component | Size (dp) | Elevation (dp) | Build on |
| --- | --- | --- | --- |
| Top app bar | 48 dense, 56, 64 | 4 | Custom container |
| Button | 36 high, 64 min wide, 48 touch | 0 by default | `lv_button` plus ripple |
| Floating action button | 56, mini 40 | 6 | `lv_button`, round |
| Card | Content-sized | 1 | `lv_obj` |
| List row | 48, 64, 72 | 0 | Custom row, recycled while scrolling |
| Text field | 56 high | 0 | `lv_textarea` plus floating label |
| Switch | 36 × 14 track, 20 handle, 48 touch | 1 on handle | `lv_switch` |
| Slider | 4–6 track, 20 thumb, 48 touch | 1 on thumb | `lv_slider` |
| Checkbox | 18 box, 48 touch | 0 | `lv_checkbox` |
| Chip | 32 high | 0 | Custom |
| Tabs | 48, or 64 with icon | 0 | `lv_tabview` |
| Menu | Content-sized | 8 | Custom surface |
| Snackbar | 344–672 wide | 6 | Shell service |
| Navigation drawer | 256 wide | 16 | Shell service |
| Dialog | 280–560 wide | 24 | Shell service |

Card and dialog elevations are from my own knowledge, and the rest are from MDC. Expose these as `t5_md_*` constructors, with the raw LVGL objects still available when an app needs them.

### Motion

LVGL can use the exact Material curves through its custom cubic-bezier animation path.

| Curve | Cubic-bezier | Use |
| --- | --- | --- |
| Standard | 0.4, 0, 0.2, 1 | Elements moving on screen, elevation |
| Deceleration | 0, 0, 0.2, 1 | Elements entering |
| Acceleration | 0.4, 0, 1, 1 | Elements leaving for good |
| Sharp | 0.4, 0, 0.6, 1 | Elements leaving temporarily |

| Transition | Duration (ms) |
| --- | --- |
| Ripple | 75 fade in, 225 expand, 150 fade out |
| Elevation change | 280 |
| Dialog | 150 in, 75 out |
| Drawer | 250 in, 200 out |
| Menu | 120 scale, 30 fade in, 75 fade out |

The ripple is a circle growing from the touch point, clipped to the widget, at 12% dark or 24% light ink. Large or rounded widgets clip in software, so fall back to a plain 12% or 24% fade if a frame budget is missed, and always in low-power mode.

### Rendering budget

In current LVGL source, the PPA draw unit takes only square, opaque, non-gradient fills, plus unscaled opaque images when enabled. Everything distinctive in MD2 therefore lands on the CPU: 4 dp corners, shadows, transparent state layers and anti-aliased text.

1. **Bake shadows.** MD2 shadows stack three layers, and an LVGL object draws one. Pre-render each used elevation into an ARGB nine-slice image with the true three-layer look, then draw that image.
2. **Limit live shadows.** Keep buttons flat by default. Avoid animating shadowed surfaces, and fade a dialog's scrim rather than resizing its shadow.
3. **Use square surfaces when you can.** Full-width bars, list rows, drawers and backgrounds have no rounded corners, so they stay on the PPA path.
4. **Cache shapes.** Raise LVGL's circle cache for the corner radii in use. Its shadow cache costs its size squared in RAM, so nine-slices are the better default.
5. **Check for banding.** Faint shadows and dark overlays can band in RGB565. If they do, add dithering to the baked assets before considering a larger framebuffer format.
6. **Pin LVGL.** PPA support and its Kconfig differ between versions, so the OS pins one version per API level and re-tests after upgrades.

Build a test screen before any real component: an app bar, a scrolling list of 100 rows, a card grid, a FAB and an open dialog. It must hold 60 fps while scrolling lists and 30 fps during full-screen transitions on the Tab5, with touch-to-ripple under one frame at the target rate. These targets are ours, and the test decides whether the plan above is enough.

### Architecture

| Layer | Contents |
| --- | --- |
| `md_tokens` | Colours, type, shape, motion and elevation, as data shared with the PC simulator |
| `md_theme` | An LVGL theme applying tokens to standard widgets, using `lv_theme_set_parent` and `lv_theme_set_apply_cb` |
| `md_widgets` | Ripple, app bar, FAB, text field, menu, snackbar, drawer, dialog |
| `t5_md_*` | The stable app API, with runtime light, dark and accent switching |

Roboto is on Google's open licences, and npm currently lists packaged Roboto under OFL-1.1 and Material Icons under Apache-2.0. Confirm the exact font files and keep their licence texts in the OS image.

```c
lv_obj_t *fab = t5_md_fab_create(screen, T5_ICON_ADD);
t5_md_snackbar_show("Grove sensor detected", "OPEN", open_sensor_cb);
t5_md_button_create(screen, "Install", T5_MD_BUTTON_CONTAINED);
```

## Shell and system apps

The shell is a landscape-first launcher with a status bar, a pull-down quick settings panel and one foreground app at a time. It follows the Material Design 2 spec in the section above.

| System app | Purpose |
| --- | --- |
| Settings | Wi-Fi, BLE, display, sound, power, time, updates, developer mode |
| Port Inspector | Pin owners, rails, I2C traffic, module profiles |
| Power | Live draw, charge history, per-app use, charge limit |
| Files | SD and `/data` browsing, app install, USB storage mode |
| App Manager | Install, permissions, crash reports, removal |
| Terminal | Logs, service state, a Python REPL |
| Camera and Recorder | Prove the media path and give apps working reference code |

Three example apps would show off what makes this device unusual, and test the APIs under real load:

- **UVC monitor:** show a USB capture dongle on screen. A [review](https://taoofmac.com/space/reviews/2026/07/18/1920) reports this already works as standalone firmware.
- **RS-485 console:** read and write Modbus devices on battery, with a tripod-mounted screen.
- **Scheduled logger:** the Grove sensor and soft-off wake example from the power section.

## Updates, recovery and security

A failed update or a bad app must always be recoverable on the device itself. Keep the device open for tinkering by default, and make the stricter options opt-in.

| Area | Design |
| --- | --- |
| OS updates | A/B slots, with automatic rollback if a new image fails its boot health check |
| Radio updates | Store the C6 image in flash, compare versions at boot and update over the existing link |
| Recovery | Enter after three failed boots or from a flag file on SD. It can reinstall the OS from SD, clear settings and export logs |
| Safe mode | Boots with no user apps or auto-launch, and shows the last crash |
| Filesystems | LittleFS for `/data`, since it tolerates power loss. FAT on SD for PC compatibility, with frequent syncs |
| Credentials | Encrypted in NVS, using a device-bound key |
| App trust | Unsigned apps allowed with a clear warning; a signed publisher badge can come later |
| Secure boot and flash encryption | Off by default and documented as a one-way choice, since eFuses cannot be undone |
| Network exposure | No listening services by default. Developer mode requires pairing and a token |
| USB storage | Off until the person turns it on, and never exposes `/data` credentials |
| Telemetry | None. Crash data stays on the device unless exported |

## Roadmap

Build power management before the shell, because it changes how every driver and app must behave. Adding sleep support to finished drivers is much harder than designing for it from the start.

&#91;embedded content: Build phases and exit criteria, not to scale\]

Phases have exit criteria rather than dates, since the pace depends on your time and on what the bench checks find.

## Bench checks

These twelve questions need a real Tab5, and several could change the design. The schematic readings came from text extracted out of the PDF netlist, so treat them as strong leads rather than proof.

1. Which panel does the unit have? Probe I2C 0x14 and 0x55, and check how to separate ST7123 from ST7121.
2. Do RTC and IMU interrupts reach the P4 while it is running or in light sleep, perhaps through G35? If not, motion wake exists only from soft off.
3. Does the PMS150G cut power at once on a double press, or does the OS get time to save? Can the OS see short presses on G35?
4. Does light sleep wake reliably from G23, G35 and G14, and do PSRAM contents and the DSI panel resume cleanly?
5. What does each power state draw? Use an external meter for soft off, since the INA226 is unpowered there.
6. Is USB-C really limited to full speed? Can the console and device modes switch at runtime?
7. Does plugging in USB during soft off power the device on, and does any trickle charge occur?
8. Do the expander outputs, especially `CHG_EN`, hold their state while the P4 is in deep sleep?
9. Does the SD detect switch share a net with the IMU wake line? The netlist suggests it might, and no card-detect GPIO is apparent.
10. Do the `ext` and `stamp` pins match this document, and what voltage is on the M5-Bus BAT pin? The schematic suggests a regulated rail rather than raw battery.
11. Does `elf_loader` 2.1+ run PSRAM code reliably on revision v1.0 silicon, and how fast compared with built-in code?
12. Can the C6 hold Wi-Fi while the P4 sleeps, and signal it on IO2 (G14)? How well do USB hubs and composite devices work on USB-A? A reviewer needed SDK patches for a complex composite device.

A few chip details here come from my own knowledge rather than opened pages, so check them in the datasheet: LP-core I2C pin limits, P4 pin capabilities and WebAssembly runtime support.

## Open decisions

- [ ] **What license?** MIT or Apache suits wide adoption, and either works with our own code.
- [ ] **Handheld or mounted panel first?** This sets idle timers, the default for charge limit and how much soft off matters.
- [x] **Which Tab5 do you have?** Unit A: ESP32-P4 v1.3 (`rev1` build), ST7123 panel with touch firmware 3, 16 MB flash, 32 MB PSRAM, a 2S battery pack (8.38 V when full).
- [ ] **Which brand colours?** The Material baseline purple and teal are placeholders until we pick a palette.
- [x] **Light theme, dark theme or both at launch?** Dark first. It's cheaper to polish, because elevation shows as a lighter surface rather than a soft shadow. Light comes after 1.0.

## Sources

Pages opened in full:

- [M5Stack Tab5 documentation](https://docs.m5stack.com/en/core/Tab5): specs, pin maps, power notes, revisions
- [Tab5 schematic](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf) and [block diagram](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Overall_Design_Block_Diagram.pdf): power tree, USB, connectors
- [NuttX Tab5 board page](https://nuttx.apache.org/docs/latest/platforms/risc-v/esp32p4/boards/esp32p4-tab5/index.html): silicon revision, INA226 notes, driver status
- [Tao of Mac review](https://taoofmac.com/space/reviews/2026/07/18/1920): performance, USB host experience, UVC monitor

Read only as search excerpts, so check the full pages before relying on them:

- [Espressif `elf_loader` changelog](https://components.espressif.com/components/espressif/elf_loader/versions/2.1.0/changelog?language=en)
- [Tactility v0.7.0 release](https://github.com/TactilityProject/Tactility/releases/tag/v0.7.0) and [SDK licensing note](https://forum.lvgl.io/t/an-operating-system-for-esp32/19585)
- [ESP-IDF ESP32-P4 sleep modes](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32p4/api-reference/system/sleep_modes.html)
- [Espressif LVGL adapter](https://components.espressif.com/components/espressif/esp_lvgl_adapter/versions/0.7.1/readme) and [Tab5 board package](https://components.espressif.com/components/espressif/m5stack_tab5_noglib/versions/1.3.1/readme)
- [espp Tab5 board package](https://components.espressif.com/components/espp/m5stack-tab5)
- [ESP32-P4 datasheet](https://documentation.espressif.com/esp32-p4_datasheet_cn.pdf)

Read as downloaded source code:

- Material Components Web 14.0.0 packages, including [`@material/typography`](https://www.npmjs.com/package/@material/typography), [`@material/elevation`](https://www.npmjs.com/package/@material/elevation), [`@material/animation`](https://www.npmjs.com/package/@material/animation) and [`@material/ripple`](https://www.npmjs.com/package/@material/ripple): MD2 tokens, component sizes, timings
- [LVGL](https://github.com/lvgl/lvgl), commit 1918a99 of 2 November 2026: PPA draw unit limits, theme API, shadow cache, bezier animation

The [Material Design 2 site](https://m2.material.io/go/design-elevation) could not be read here, because it needs JavaScript. Check the dark-theme values against it.
