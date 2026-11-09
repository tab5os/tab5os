# Changelog

## Unreleased

- **Calculator example:** a basic and a scientific calculator (trigonometry in degrees or radians, inverse trig, logarithms, powers, roots, factorial, π, e, Ans, brackets) with the result shown live as you type. Its engine is plain C with host tests (`test/host/test_calc.c`), and it brings its own font for the maths symbols the OS fonts lack.
- **The shell no longer draws an app bar.** Apps get the whole screen under the status bar. An app that wants a title, a back arrow or actions draws its own with `md_appbar_create()` or `md_page_create()`. `t5_shell_set_app_bar()`, `t5_shell_set_title()` and `t5_shell_app_bar()` are gone from API level 1 (still a candidate), and so are `t5.ui.app_bar()` and `t5.ui.title()` in Python. `t5 pack` refuses `[shell] app_bar` and `title`; packages built with them still install, without the bar. Music, BLE Explorer and UI Gallery no longer use it.

## 0.1.0 (pre-release), Nov 9, 2026

The first public release of Tab5 OS, an operating system for the M5Stack Tab5 built on ESP-IDF v6.1.

**Before you install**

- It runs only on Tab5 units with ESP32-P4 silicon v1.x. `t5 chip`, or `esptool chip-id`, tells you which you have. Newer v3.x silicon isn't supported.
- Back up the factory firmware first: see [installing and updating](docs/flashing.md).
- App API level 1 is a **candidate**. Apps built against 0.1.0 may need rebuilding for a later release, until level 1 is frozen.
- This build trusts no update signing key. Updates install only in developer mode or from the USB-C console, and a later release with a key needs one reflash over USB-C.

**What's in it**

- **System:** a launcher, quick settings, a status bar, an app bar and a gesture bar, and system apps: Settings, Files, App Manager, Camera, Photos, Editor, Terminal, Ports and Power.
- **Standby:** on USB power, when the screen dims, a standby screen shows the clock, the playing music with controls, and a card from an app (weather, for example).
- **Apps:** native apps in C (a RISC-V ELF with a locked ABI) or MicroPython scripts, packaged as `.t5app` and installed without reflashing. Permissions are declared in a manifest, and the person is asked at run time for the camera, the microphone and shared files.
- **UI:** LVGL 9.6 with Material Design 2 components, rotation, and an SDL simulator.
- **Hardware services:** power, battery and charging, sleep and wake locks, Wi-Fi, and Bluetooth LE (central and peripheral, through the ESP32-C6, with or without Wi-Fi). Also audio: mixed streams with ducking, WAV playback, headphone detection and media sessions. And the camera, USB host (serial, MIDI, UVC), the SD card, the RTC, the IMU, and the external ports with a ports broker (I2C, GPIO, PWM, ADC, UART, RS-485 and Modbus RTU).
- **Updates and recovery:** A/B updates with a trial boot and automatic rollback, safe mode, and a separate recovery image.
- **Developer tools:** developer mode with pairing, and the `t5` CLI: create, build, push, run, logs, Python, screenshots and OTA.
- **SDK:** `tab5os-sdk-0.1.0.tar.gz` builds apps on any machine with the ESP-IDF v6.1 RISC-V toolchain, CMake, Ninja and Python. No firmware sources are needed.
- **Examples:** BLE Explorer, Logger, Music, RS-485 Console, UI Gallery, UVC Monitor and Weather.

**Known limitations**

- No MP3 decoder in the OS image: it doesn't fit the size budget. WAV is supported.
- Bluetooth LE traffic slows Wi-Fi while both are active.
- No Classic Bluetooth, so no Bluetooth audio: the ESP32-C6 has LE only.
- The ESP32-C6 runs its factory radio firmware (1.4.1).

**Downloads**

| File | What it is |
| --- | --- |
| `tab5os-0.1.0.zip` | Everything below, plus `flash_all.sh` and the install guide |
| `tab5os-0.1.0.t5os` | The OS as an update package |
| `tab5os-sdk-0.1.0.tar.gz` | The app SDK |
| `*.t5app` | The example apps, ready to install |
| `SHA256SUMS` | Checksums |
