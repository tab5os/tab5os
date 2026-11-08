# Tab5 OS app developer wiki

For Tab5 OS 0.0.1, API level 1, as of Nov 8, 2026. Written for people and for coding agents. Agents should start with the [Agent guide](Agent-Guide.md).

Tab5 OS runs on the M5Stack Tab5: an ESP32-P4 with a 1280 x 720 touch screen, 32 MB of PSRAM, and an ESP32-C6 for Wi-Fi and Bluetooth. One app is in front at a time, and services own the hardware. An app is either a native RISC-V ELF written in C or a MicroPython script. Both are packaged as a `.t5app` file and installed without reflashing.

## Start here

| If you want to | Read |
| --- | --- |
| Get an app on the screen in five minutes | [Getting started](Getting-Started.md) |
| Copy something that works | [Cookbook](Cookbook.md) |
| Understand what the machine can and can't do | [Platform overview](Platform-Overview.md) |
| Hand this to a coding agent | [Agent guide](Agent-Guide.md) |
| Find out why something broke | [Troubleshooting](Troubleshooting.md) |
| Look up a number | [Limits](Limits.md) |

## Guides

**Building an app**

- [Getting started](Getting-Started.md): toolchain, first C and Python app, the edit-run loop
- [Manifest and packaging](Manifest-and-Packaging.md): every manifest key, permissions, assets, signing
- [App lifecycle](App-Lifecycle.md): which callback runs when, and what it must do
- [UI guide](UI-Guide.md): Material widgets, layout, rotation, text, icons, images

**Making it good**

- [Threading](Threading.md): the UI task, workers, safe hand-offs, avoiding deadlocks
- [Performance](Performance.md): the frame budget, what's cheap, what's expensive, how to measure
- [Memory and resources](Memory-and-Resources.md): the app heap, internal RAM, what the OS cleans up
- [Stability](Stability.md): the failure model, error handling, hardening checklist
- [Power and background](Power-and-Background.md): wake locks, sleep, scheduled wakes, battery

**Using the hardware**

- [Storage](Storage.md): `/data`, `/sd`, `/usb`, durable writes, what's missing from the C library
- [Networking](Networking.md): HTTP, WebSocket, MQTT, sockets, TLS, servers
- [Ports and sensors](Ports-and-Sensors.md): Grove, M5-Bus, RS-485, Modbus, rails, IMU, time
- [Media and USB](Media-and-USB.md): audio, microphone, camera, JPEG, USB devices

**Tools and reference**

- [Python apps](Python-Apps.md): MicroPython on the device
- [Debugging and testing](Debugging-and-Testing.md): logs, crash offsets, profiling, stress tests, the simulator
- [Troubleshooting](Troubleshooting.md): symptom, cause, fix
- [Limits](Limits.md): every hard number in one table
- [API reference](../api-reference.md): every exported function and its permission
- [SDK notes](../sdk.md), [Python notes](../python.md), [ABI stability](../abi-stability.md), [Security defaults](../security.md)

## The ten rules

Nearly every problem in an app comes from breaking one of these. Each links to the reasoning.

1. **Never block the UI task.** Callbacks run on it. No network, no sleeping, no long file I/O. After 2 s the OS shows "App not responding". [Threading](Threading.md)
2. **Touch LVGL only from the UI task,** or between `t5_ui_lock()` and `t5_ui_unlock()`. Prefer `t5_ui_async()`. [Threading](Threading.md)
3. **Only what's in `api_v1.sym` exists.** A function that compiles and links can still be refused at install. [Manifest and packaging](Manifest-and-Packaging.md#the-abi)
4. **Keep changed pixels few.** A full-screen update is limited to about 27 fps by memory bandwidth, however fast your code is. [Performance](Performance.md)
5. **Use `md_list` for long lists.** LVGL objects live in an 88 KB fast pool. [Performance](Performance.md#object-count)
6. **Sizes are in dp.** `T5_DP(16)`, never raw pixels. Build layouts with flex so rotation works. [UI guide](UI-Guide.md)
7. **Leaving the app ends it.** There's no background state: going home runs `on_suspend`, `on_destroy`, then everything is freed. [App lifecycle](App-Lifecycle.md)
8. **Loops in your tasks check `t5_app_running()` and sleep.** Tasks get 150 ms to stop at exit. A task that spins is demoted. [Threading](Threading.md#exit)
9. **Save early with `t5_storage_write_atomic`.** The power button can cut power with no warning. [Storage](Storage.md#durable-writes)
10. **Check every result.** Hardware goes away, Wi-Fi drops, cards fill up. [Stability](Stability.md#handling-errors)

## How these pages stay true

Claims here come from the OS source and from [measurements on the bench](../bench-results.md). Figures are from one device, Unit A (ESP32-P4 v1.3 at 360 MHz, `rev1` build). When a page and the code disagree, the code wins: `sdk/abi/api_v1.sym` for what's callable, and the headers under `firmware/components/*/include` for what it does.
