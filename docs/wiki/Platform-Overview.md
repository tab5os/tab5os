# Platform overview

What the machine is, and how that should shape your design.

## Hardware

| | |
| --- | --- |
| Main chip | ESP32-P4: two RISC-V cores at 360 MHz (`rev1` build), single-precision FPU |
| Memory | 32 MB PSRAM at 200 MHz; 768 KB of internal L2 RAM, of which the OS keeps almost all; 128 KB L2 cache in front of PSRAM |
| Display | 720 x 1280 panel, used at 1280 x 720 in landscape, 64 Hz scan-out, RGB565 |
| Touch | Capacitive, on the display controller. USB keyboards and mice work too |
| Radio | ESP32-C6 as a modem: Wi-Fi and BLE, over SDIO |
| Storage | 16 MB flash, with 4.35 MB for `/data` and 1 MB of that per app; microSD at `/sd`; USB drives at `/usb` |
| Audio | Speaker, headphone jack, two microphones |
| Camera | SC202CS, 1280 x 720 at 30 fps, with hardware JPEG and H.264 encoders |
| Accelerators | PPA (blits, blends, rotation), JPEG, H.264 |
| Ports | Grove, M5-Bus, 10-pin header, RS-485, Stamp pads, USB-A host |
| Sensors | BMI270 accelerometer, RTC, battery gauge |
| Battery | 2S pack, read-only to apps |

## The OS model

```text
Your app (native ELF or MicroPython)
    |  t5_* calls, md_* widgets, LVGL
Services (UI, power, ports, audio, camera, net, storage...)
    |
Drivers, FreeRTOS, ESP-IDF
```

Four decisions shape everything:

1. **One foreground app.** No windows and no background apps. When someone goes home or opens another app, yours exits. Only a scheduled alarm runs an app that isn't in front.
2. **Services own hardware.** You ask for a port, a camera, a wake lock. That's how the OS arbitrates and how it cleans up after you.
3. **One address space.** A native app shares memory with the OS. The OS can contain most faults, but not a stray write that lands in its own memory.
4. **Named ports and dp.** No pin numbers and no pixel sizes, so your code doesn't encode a board revision or a density.

## Where work runs

| Core | Runs | Consequence |
| --- | --- | --- |
| 0 | System services, event dispatch, Wi-Fi link, storage | Your event handlers run here, not on the UI task |
| 1 | The UI task (priority 5): LVGL, and all your callbacks | Time in a callback is time not rendering |
| Either | Your tasks, at priority 2 or less | They never preempt the UI task or a system service that does work |

## The memory hierarchy

Bandwidth is the scarce resource. Bench figures on Unit A:

| Memory | Speed | Who gets it |
| --- | --- | --- |
| Internal L2 RAM | about 1.1 GB/s CPU writes | The OS: LVGL's objects and draw bands, DMA, Wi-Fi. Never your heap |
| L2 cache (128 KB) | near internal speed on a hit | Your hot code and data, if they fit |
| PSRAM, CPU | 88 MB/s fill, 57 MB/s copy idle | Your heap, stacks, code, images |
| PSRAM with the panel scanning out | 40 to 60 MB/s total | Everything above, while the panel also reads about 118 MB/s |

Three consequences:

- **Moving a full screen costs about 35 ms** no matter who does it. A full-screen frame is 1.8 MB, and it's read and written.
- **Your code runs from PSRAM through the cache.** Small loops run at full speed: a matrix kernel measured the same as built-in code. Large code that misses the cache is slower.
- **Your buffers are always PSRAM.** A loop over a few KB is fast. Walking 8 MB is limited by PSRAM.

Details and tactics are in [Performance](Performance.md).

## Power states

| State | Screen | Your code | Enter after (handheld profile) |
| --- | --- | --- | --- |
| Active | On | Runs normally | Input |
| Dim | Dimmed | Runs normally | 30 s idle |
| Screen off | Off | Tasks run; **LVGL timers and animations stop** | 2 min idle |
| Sleep | Off | Tasks still run, at full clock in this build | 10 s after screen off |
| Soft off | Power cut | Nothing, unless an alarm boots the device | 24 h asleep |

Wake locks hold the device in a shallower state. See [Power and background](Power-and-Background.md).

## C or Python?

| | Native C | MicroPython |
| --- | --- | --- |
| Speed | Full | Much slower in tight loops; fine for I/O-bound work |
| UI | All of LVGL plus MD2 | The MD2 subset in `t5.ui` |
| Failure | Usually closes the app; a stray write can corrupt the OS | An exception. Designed not to be able to crash the device |
| Access | Everything in the ABI, and in fact the whole address space | Only the `t5` module and contained file paths |
| Tooling | Compiler and a firmware build | None; can be written on the device |
| Concurrent apps | One foreground app | One VM, shared with the Editor and Terminal |
| Good for | Games, drivers, camera, custom rendering, anything timed | Sensor scripts, dashboards, automation, quick tools |

Start in Python if your app mostly waits for things. Choose C if it has to draw or compute continuously.

## What isn't available

Knowing this early saves a redesign.

- **C only.** There's no C++ runtime in the ABI (`new`, exceptions and static constructors can't be bound).
- **Files are scoped.** An app gets its own folders and whatever the person grants or picks. See [which files an app can use](Storage.md#which-files-an-app-can-use).
- **No `fsync` in C.** The only durable write is `t5_storage_write_atomic`.
- **No PNG decoder, and no LVGL file paths.** Images are raw RGB565 in memory, or JPEG through the hardware decoder. See [images](UI-Guide.md#images).
- **Text is Latin-1.** The bundled fonts have no CJK, Cyrillic, Arabic or emoji. See [text](UI-Guide.md#text-and-fonts).
- **160 icons,** not all of Material Icons. They're listed in `md/md_icons.h`.
- **No app-to-app messaging** and no background services. An app can launch another with an argument, with the `system` permission.
- **Internal RAM isn't yours.** There's no API to allocate it.
- **No DMA or ISR access.** Apps can't register interrupts through the ABI.
