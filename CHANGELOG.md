# Changelog

## 0.2.0 (pre-release), Nov 12, 2026

Apps can act as a USB or Bluetooth keyboard, mouse or MIDI device, reach the USB-A bus, the Wi-Fi radio and the Bluetooth controller below the OS, and draw their UI with t5ui, a UI API that doesn't expose LVGL. Releases are now signed and the Tab5 finds them over Wi-Fi.

**Before you install**

- It runs only on Tab5 units with ESP32-P4 silicon v1.x. `t5 chip`, or `esptool chip-id`, tells you which you have. Newer v3.x silicon isn't supported.
- **App API level 2.** Level 1 is unchanged, so apps built with an earlier SDK keep running without a rebuild. Apps that use the new calls declare `api = 2` and need this release: 0.1.3 and earlier refuse to install them. Build them with the 0.2.0 SDK. Level 2 is a candidate for now: its calls may still change before it's frozen, as level 1's did between 0.1.0 and 0.1.1.
- This is the first signed release. 0.1.3 and earlier trust no key, so install this one once in developer mode (Settings, then `tab5os-0.2.0.t5os` from the SD card in Settings > Updates, or `t5 ota`), or flash over USB-C with `firmware/flash_all.sh`. Later updates install with developer mode off. Settings and files in `/data` are kept either way. See [installing and updating](docs/flashing.md).
- The first minute after the update, the Tab5 also updates its Wi-Fi chip (the ESP32-C6) to the firmware that comes with this release, C6 firmware 0.2.0, downloading it from the release. It takes about 20 seconds, and Wi-Fi and Bluetooth drop for a few seconds when the C6 restarts. Without Wi-Fi, copy `tab5os-c6-0.2.0.t5c6` to the SD card's top folder first. See [the Wi-Fi chip's firmware](docs/flashing.md#the-wi-fi-chips-firmware).

**What changed**

- **USB-C device mode for apps.** An app can make the Tab5 a USB keyboard, mouse, gamepad, media remote or MIDI device for a computer (`t5/usbdev.h`), with HID reports built by the shared `t5/hid.h` helpers and keyboard layouts. The console and sound are off while it does, as with USB storage. See [the design](docs/io-access-design.md).
- **Bluetooth HID.** The Tab5 pairs with a computer, phone or tablet as a keyboard, trackpad or gamepad (HID over GATT), with pairing codes and remembered bonds per app.
- **Raw USB-A access**: open a device, read its descriptors, claim interfaces and do control, bulk and interrupt transfers (`t5/usb.h`).
- **Wi-Fi monitor mode and raw frames** (`t5/wifi.h`), through new C6 firmware: frames from the air with channel and signal strength, channel hopping, raw transmit.
- **Exclusive control** (`t5/excl.h`): an app can take the whole USB-C port (a USB device of its own design), the USB-A bus, the Wi-Fi radio or the Bluetooth controller (raw HCI) until it closes.
- These ask once on first use, like the camera, and the status bar and Quick Settings show which app holds a port or radio, with a Stop button.
- **Python**: `t5.hid`, `t5.usbdev`, `t5.ble.hid`, `t5.wifi.monitor`, raw `t5.usb` and `t5.exclusive`.
- **A new example, Keyboard & Trackpad**: the Tab5 as a computer's keyboard, trackpad and media remote, over USB-C or Bluetooth. It's the first app built only on t5ui.
- **t5ui, an app UI API that doesn't expose LVGL** (`t5/t5ui.h`): objects, typed properties, styles, animations, events and commands with fixed numbers, Material widgets, and drawing. LVGL 9 is the backend today, so a future LVGL won't break apps written with t5ui. See [t5ui](docs/t5ui.md).
- **The OS draws its own UI with t5ui**: the shell, every system app, the file picker, the permission and app-not-responding dialogs, and Python's `t5.ui`. It looks the same as before.
- **Updates now keep the Wi-Fi chip's firmware current.** The C6 firmware has its own version (`c6/VERSION`), raised only when it changes. Each release publishes the C6 firmware it was built with, signed (`tab5os-c6-<C6 version>.t5c6`), and `flash_all.sh` writes it too. After an update has passed its first minute, the OS moves the C6 onto that version if it runs another one: from the SD card if the file is there, else downloaded from the release. The release checks (`t5 release-check`, in `ci` and the release workflow) refuse a release whose C6 image changed without a new C6 version, or whose esp_hosted moved without a recorded update test. See [Releasing](docs/releasing.md). Our C6 firmware now tells the OS which build it is (esp_hosted only reports its own version), which also makes `net update`'s check after an update right. Console: `ota c6`, `ota c6 retry`, `ota c6 file|url`.
- **Sleep stays asleep with Wi-Fi off.** A stray wake signal from the C6, caught while Sleep powered it off, could wake the Tab5 again every few seconds.
- **Signed updates over Wi-Fi.** Releases are now signed with the Tab5 OS release key, and the OS trusts it (`firmware/ota_keys/release.pem`). The Tab5 checks the GitHub releases about once a day while Wi-Fi is connected, says when a newer one is out, and Settings > Updates has a new Over Wi-Fi section with Install and Check now. A signed update installs with developer mode off, from Wi-Fi or the SD card. Nothing installs without a tap. From Settings without developer mode, an update older than the running system is refused. Coming from 0.1.3 or earlier, which trust no key, install this release once in developer mode or with `flash_all.sh`. See [installing and updating](docs/flashing.md#updating).
- **A built-in video player.** Files, the Camera's gallery and a new Videos app play Motion JPEG in AVI, with PCM sound, full screen, with play, pause and a seek bar. The P4 has no H.264 decoder, so H.264 isn't played. To convert a video: `ffmpeg -i in.mp4 -vf scale=1280:-2 -c:v mjpeg -q:v 4 -c:a pcm_s16le out.avi`. See [the video player](docs/wiki/Media-and-USB.md#video-player).
- **The Camera records video the player can play**: Motion JPEG in AVI (`.avi`), 720p at 30 fps. The quality drops a little if the card can't keep up. Apps still choose H.264 by recording to any other file name.
- **A smoother camera preview**, 15 fps where it was 11. The preview, videos and photos are now drawn straight into the screen by the PPA in one pass, without going through the UI renderer (video planes).
- **Zoom in the photo viewer**: pinch, or double-tap to zoom in on a spot and back out; drag to move around while zoomed in.
- **A smaller OS image.** The Phase 0 bench commands (`elf bench`, `gpio watch|get`, `sleep light|result`) are left out of normal builds, and NimBLE logs only warnings and errors. The image is 132 KB smaller, about 3.5% of an update slot. Apps, the app API and the SDK are unchanged. To get the bench commands back, build with `CONFIG_T5_DIAG_BENCH=y` (see [bench procedures](docs/bench-procedures.md)).

**Downloads**

| File | What it is |
| --- | --- |
| `tab5os-0.2.0.zip` | Everything below, plus `flash_all.sh` and the install guide |
| `tab5os-0.2.0.t5os` | The OS as an update package, signed |
| `tab5os-c6-0.2.0.t5c6` | The Wi-Fi chip's firmware for this release, signed; the OS fetches it by itself |
| `tab5os-sdk-0.2.0.tar.gz` | The app SDK |
| `*.t5app` | The example apps, ready to install: BLE Explorer, Calculator, Keyboard & Trackpad, Logger, RS-485 Console, UI Gallery and UVC Monitor |
| `SHA256SUMS` | Checksums |

## 0.1.3 (pre-release), Nov 10, 2026

The standby screen shows every app card in a stack you can scroll, beside a clock that fills half the screen.

**Before you install**

- It runs only on Tab5 units with ESP32-P4 silicon v1.x. `t5 chip`, or `esptool chip-id`, tells you which you have. Newer v3.x silicon isn't supported.
- App API level 1 and the SDK are unchanged, so apps built with the 0.1.2 SDK keep running without a rebuild.
- This build still trusts no update signing key, so updates install only with developer mode on. Turn it on in Settings, then install `tab5os-0.1.3.t5os` from the SD card (Settings > Updates) or with `t5 ota`. Or flash over USB-C with `firmware/flash_all.sh`. Settings and files in `/data` are kept either way. See [installing and updating](docs/flashing.md).

**What changed**

- **Standby shows every app card.** The standby screen is now two halves. One holds a much larger clock and the date (left in landscape, top in portrait). The other holds all the app cards (up to 8) as a stack you scroll a card at a time, with a dot per card, and the music bar pinned under it. Cards keep a fixed order by app name. Standby opens on the newest one and turns to a card when it changes, unless you scrolled in the last two minutes. With no cards and no music, the clock takes the whole screen. A tap still leaves standby. See [Standby](docs/sdk.md#standby).

**Downloads**

| File | What it is |
| --- | --- |
| `tab5os-0.1.3.zip` | Everything below, plus `flash_all.sh` and the install guide |
| `tab5os-0.1.3.t5os` | The OS as an update package |
| `tab5os-sdk-0.1.3.tar.gz` | The app SDK |
| `*.t5app` | The example apps, ready to install: BLE Explorer, Calculator, Logger, RS-485 Console, UI Gallery and UVC Monitor |
| `SHA256SUMS` | Checksums |

## 0.1.2 (pre-release), Nov 10, 2026

Apps can keep their standby card current while another app is open, and the next Weather release uses it.

**Before you install**

- It runs only on Tab5 units with ESP32-P4 silicon v1.x. `t5 chip`, or `esptool chip-id`, tells you which you have. Newer v3.x silicon isn't supported.
- App API level 1 is unchanged, so apps built with the 0.1.1 SDK keep running. A manifest with `standby_refresh_minutes` needs this release: 0.1.1 and earlier refuse to install it. Build such apps with the 0.1.2 SDK.
- This build still trusts no update signing key, so updates install only with developer mode on. Turn it on in Settings, then install `tab5os-0.1.2.t5os` from the SD card (Settings > Updates) or with `t5 ota`. Or flash over USB-C with `firmware/flash_all.sh`. Settings and files in `/data` are kept either way. See [installing and updating](docs/flashing.md).

**What changed**

- **Standby cards stay current with another app open.** An app can add `standby_refresh_minutes` (15 to 1440, with `ambient = true`) to `[background]`. While the standby screen shows, the OS then runs its `on_alarm` headless about that often, so a weather card updates every hour even with Files open underneath. A tap still returns to Files. A run is skipped when the app is the one open, or when it set its card recently itself. Making this safe with an app open fixed three things that assumed one app at a time: ELF loading is now serialised, the crash guard goes back to the open app after a headless run, and a Python background run no longer overwrites an open Python app's state. `app alarm <id>` now refuses to run a second copy of the open app, or two runs at once. See [Standby refreshes](docs/wiki/Power-and-Background.md#standby-refreshes).

**Downloads**

| File | What it is |
| --- | --- |
| `tab5os-0.1.2.zip` | Everything below, plus `flash_all.sh` and the install guide |
| `tab5os-0.1.2.t5os` | The OS as an update package |
| `tab5os-sdk-0.1.2.tar.gz` | The app SDK |
| `*.t5app` | The example apps, ready to install: BLE Explorer, Calculator, Logger, RS-485 Console, UI Gallery and UVC Monitor |
| `SHA256SUMS` | Checksums |

## 0.1.1 (pre-release), Nov 10, 2026

App API level 1 is frozen in this release, and it fixes a reboot, a handle leak and stale USB devices. Weather and Music now live in their own repos.

**Before you install**

- It runs only on Tab5 units with ESP32-P4 silicon v1.x. `t5 chip`, or `esptool chip-id`, tells you which you have. Newer v3.x silicon isn't supported.
- Apps built against API level 1 from now on keep running on later releases. Apps built with the 0.1.0 SDK need one rebuild with this SDK, because the app bar functions were removed.
- This build still trusts no update signing key, so updates from 0.1.0 install only with developer mode on. Turn it on in Settings, then install `tab5os-0.1.1.t5os` from the SD card (Settings > Updates) or with `t5 ota`. Or flash over USB-C with `firmware/flash_all.sh`. Settings and files in `/data` are kept either way. See [installing and updating](docs/flashing.md).

**What changed**

- **Finished animations no longer use up an app's handles.** The OS recognised an app's animations by their target object and callback. Starting a new animation on the same object made every finished one on it look like it was still running, so none were cleared. A game that animates its tiles reached the 256-handle limit after about 100 moves, and files it opened after that went untracked. Each animation is now tracked by LVGL's own record of it and cleared once it ends. On exit the OS deletes exactly the app's running animations, where before an animation with no target object could delete every animation in the system. `app usage` counts only the animations still running. `test/apps/anims` in `t5 hil app_checks` covers it.
- **Closing an app during a network request no longer reboots the Tab5.** Exiting Weather right after launch, while its worker was inside `t5_http_fetch`, deleted that task inside lwIP's `select()`. lwIP kept pointing at the task's freed semaphore, and the next packet on the socket made the network task panic. Now every lwIP socket call made on an app's task is counted, whether the app made it or the OS made it for the app. An exiting app's tasks get `ECANCELED` for new network waits, so OS requests end within 50 ms. The app's own sockets are closed first, which wakes `recv()`, `accept()` or `select()` calls blocked on them. A task inside lwIP isn't stopped there: it's stopped once it's out, or, if it is still inside when the exit can't wait any longer, it ends itself when the call returns. The app ABI is unchanged. `test/apps/netexit` and `t5 hil net_exit` cover it.
- **Unplugged USB devices leave the device list.** Drives, keyboards and other devices stayed in the status bar and Settings after they were removed, because the OS only listened for removal of devices it still held open. It now drops a device on either removal event.
- **Weather and Music have their own repos:** [tab5os/weather](https://github.com/tab5os/weather) and [tab5os/music](https://github.com/tab5os/music), with their history. They build against the SDK like any other app (`t5 app-build .`), and Music's host test went with it. They're no longer built by `t5 examples` or shipped in the release's `apps/` folder.
- **An app template:** [tab5os/app-template](https://github.com/tab5os/app-template) is a GitHub template for an app in its own repo. `./build.sh` fetches the SDK and builds the `.t5app`, and its CI builds every push and publishes a release for each version tag. Weather and Music use it.
- **App API level 1 is frozen.** Apps built against it keep running on every later release without a rebuild ([ABI stability](docs/abi-stability.md)). Apps built with the 0.1.0 SDK need one rebuild, because the app bar functions were removed after 0.1.0. From now on, `layout.py gen` only adds to the layout lock and refuses to rewrite a recorded struct, enum or macro (`--force` overrides). The renderer's backend settings (`CONFIG_LV_USE_PPA*`, `CONFIG_LV_USE_DRAW_SW_ASM`) are left out of the lock, so rendering can still be tuned. Notifications are planned for level 2.
- **Apps can have their own icon.** A manifest's `icon` names a PNG in the package. The launcher draws it as a circle in the app drawer, the recent apps and the dock, and App Manager shows it too, instead of the puzzle piece. The OS decodes PNGs itself with the package inflater, so the firmware stays small. Icons are resized once per size and cached until the next install or removal. `t5 pack` checks the icon and warns if it's not square or is under 112 px. Every example app now has an icon, drawn by `tools/assets/app_icons.py`. See [App icon](docs/wiki/Manifest-and-Packaging.md#app-icon).
- **A polish pass over every screen.** Labels, buttons, tabs and section headers are in sentence case instead of capitals. Detail lines read as plain sentences with no " · " runs: Wi-Fi shows "Saved, strong signal" instead of dBm, Port Inspector says "Grove" and "M5-Bus UART" instead of `grove` and `mbus.uart`, and App Manager stops printing "Built in" on every row. Screens with nothing to show say what to do next (Logger with no sensor, an empty Gallery, Power before its first readings). List rows grow when their second line wraps, instead of clipping it. Wi-Fi lists each network once. Editor makes new scripts from a + button, and Logger picks its interval from a menu. The home screen and app drawer follow the accent colour, including right after boot, and the app icons are toned to sit together.
- **Music is a real player now.** It plays MP3, FLAC (up to 192 kHz) and WAV from the SD card's `music` folder, with the decoders in the app. The library is browsable by song, album and artist from the files' tags (ID3v1 and v2, Vorbis comments, WAV INFO), with album art from the files or a `cover.jpg` in the album folder. It reads M3U playlists and makes and edits its own in `music/playlists`. Playback has a queue with play next, shuffle and repeat, a seekable progress bar, no gap between tracks of the same format, and it resumes where it left off. The standby screen's music card controls it.
- **Calculator example:** a basic and a scientific calculator (trigonometry in degrees or radians, inverse trig, logarithms, powers, roots, factorial, π, e, Ans, brackets) with the result shown live as you type. Its engine is plain C with host tests (`test/host/test_calc.c`), and it brings its own font for the maths symbols the OS fonts lack.
- **The shell no longer draws an app bar.** Apps get the whole screen under the status bar. An app that wants a title, a back arrow or actions draws its own with `md_appbar_create()` or `md_page_create()`. `t5_shell_set_app_bar()`, `t5_shell_set_title()` and `t5_shell_app_bar()` are gone from API level 1 (a candidate then), and so are `t5.ui.app_bar()` and `t5.ui.title()` in Python. `t5 pack` refuses `[shell] app_bar` and `title`; packages built with them still install, without the bar. Music, BLE Explorer and UI Gallery no longer use it.

**Downloads**

| File | What it is |
| --- | --- |
| `tab5os-0.1.1.zip` | Everything below, plus `flash_all.sh` and the install guide |
| `tab5os-0.1.1.t5os` | The OS as an update package |
| `tab5os-sdk-0.1.1.tar.gz` | The app SDK |
| `*.t5app` | The example apps, ready to install: BLE Explorer, Calculator, Logger, RS-485 Console, UI Gallery and UVC Monitor |
| `SHA256SUMS` | Checksums |

Weather and Music are released from [tab5os/weather](https://github.com/tab5os/weather/releases) and [tab5os/music](https://github.com/tab5os/music/releases).

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
