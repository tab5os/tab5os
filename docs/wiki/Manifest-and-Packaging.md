# Manifest and packaging

A `.t5app` is a zip with `manifest.toml` at its root, one entry point and any assets. Unknown keys are errors, so a typo in a permission fails at pack time rather than silently doing nothing.

## Example

```toml
id = "com.you.airlog"
name = "Air Log"
version = "1.2.0"
api = 1
runtime = "native"
entry = "app.elf"
orientation = "any"
description = "Logs air quality"
author = "You"
homepage = "https://example.com"
license = "MIT"

[permissions]
ports = ["grove"]
net = true
background_wake = true

[background]
entry = "on_alarm"
max_seconds = 30
```

## Top-level keys

| Key | Required | Rules |
| --- | --- | --- |
| `id` | Yes | Reverse DNS: `[a-z0-9_]+(\.[a-z0-9_]+)+`, at most 63 bytes |
| `name` | Yes | At most 47 bytes, shown in the launcher |
| `version` | Yes | Semver `MAJOR.MINOR.PATCH`, optional `-pre` or `+build`, at most 15 bytes |
| `api` | Yes | API level the app needs. Currently `1` |
| `runtime` | Yes | `"native"` or `"micropython"` |
| `entry` | Yes | File in the package: `app.elf`, or `main.py` |
| `orientation` | No | `"landscape"` (default), `"portrait"` or `"any"`. Only `"any"` gets `on_rotate` |
| `min_heap_kb` or `min_heap_mb` | No, and not both | For Python: the GC heap. See below |
| `icon` | No | A file that must exist in the package. The launcher currently draws a generic icon for installed apps |
| `description`, `author`, `homepage`, `license` | No | Strings |

`min_heap_kb` sets a Python app's GC heap. The default is 2 MB, the range is clamped to 64 KB through 8 MB, and it's taken from PSRAM when the app starts. Native apps grow their heap on demand, so they don't need it.

The TOML is a subset. Not supported, and rejected: floats, dates, multi-line strings, quoted keys, inline tables, arrays of tables, nested arrays.

## Permissions

Declare only what you use. People see the list at install.

| Key | Type | Allows | Asked at run time? |
| --- | --- | --- | --- |
| `net` | bool | `t5/net.h`, BSD sockets | No |
| `ports` | string array, at most 8 | Opening those ports, acquiring rails | No |
| `camera` | bool | The camera | **Yes** |
| `mic` | bool | The microphone | **Yes** |
| `ble` | bool | Bluetooth | No |
| `usb` | bool | USB serial, MIDI, UVC | No |
| `storage` | bool | SD and USB eject, format and rescan | No |
| `shared_read` | bool | Reading the [shared folders](Storage.md#which-files-an-app-can-use) | **Yes** |
| `shared_write` | bool | Reading and changing them | **Yes** |
| `all_files` | bool | All of `/sd` and `/usb`, and `/data/user`. Never OS files or other apps' files | **Yes** |
| `background_wake` | bool | `t5_app_schedule_alarm`. Also needs `[background]` | No |
| `system` | bool | App list, launching others, per-app power, charge history | No |
| `ambient` | bool | A card on the standby screen (`t5_ambient_set`) | No |

Port names: `grove`, `mbus.spi`, `mbus.uart`, `mbus.debug`, `mbus.i2c`, `mbus.g2` and the other M5-Bus single pins, `ext`, `rs485`, `stamp`.

### What a missing permission looks like

A call in an undeclared group is bound to a stub that returns `T5_ERR_DENIED` (or `NULL`, `false`, `0`). The app installs and runs; the feature just does nothing, or crashes on a `NULL`. Check for `T5_ERR_DENIED` while developing. `app usage` on the console reports `denied imports`.

### Permissions the person is asked for

For `camera`, `mic`, `shared_read`, `shared_write` and `all_files`, declaring isn't enough. The person is asked the first time:

```c
if (t5_app_has_permission(app, T5_PERM_MIC)) start_mic();
else t5_app_request_permission(app, T5_PERM_MIC, on_answer, state);   /* doesn't block */
```

`t5_app_request_permission` must be called on the UI task. Open the device or file in the callback, and handle a "no" with a clear message: the person can only change it in App Manager afterwards. See the [camera recipe](Cookbook.md#camera-with-permission).

Permissions are consent and correctness checks, not a security boundary, because a native app shares the OS's address space. Python apps are genuinely contained. See [security defaults](../security.md).

## Background section

```toml
[background]
entry = "on_alarm"      # the function name, for Python; native apps use the T5_APP callback
max_seconds = 30        # 1..300, default 30
```

The section needs `background_wake = true`. See [Power and background](Power-and-Background.md#scheduled-wakes).

## Shell section

```toml
[shell]
fullscreen = false      # start in full screen (default false)
```

The shell doesn't draw a title bar. `app_bar` and `title`, from the first 0.1.0 SDK, are refused by `t5 pack`; packages already built with them still install, without the bar.

All optional; unknown keys are errors. The app can change each at run time ([UI guide](UI-Guide.md#shell-bars)).

## Package contents

```text
manifest.toml
app.elf or main.py
your assets...
signature.t5sig        (only if signed)
```

Installing from a CMake build packs what `t5_app()` lists. A Python app packs the whole folder, except `build`, `__pycache__`, `.git`, `.DS_Store`, `Thumbs.db`, dotted directories and `.t5app` files.

At run time:

| Function | Path | Access |
| --- | --- | --- |
| `t5_app_install_dir(app)` | `/sd/apps/<id>` or `/data/apps/<id>` | Read-only: your assets. Replaced on upgrade |
| `t5_app_data_dir(app)` | `/data/appdata/<id>` | Read-write, up to 1 MB. Kept across upgrades, on internal flash |

## Installing

| How | Command |
| --- | --- |
| From a computer | `t5 run`, or `t5 push build/com.you.app.t5app` |
| From the console | `app install /sd/x.t5app [sd\|data]`, or `app fetch http://host/x.t5app` |
| On the device | Copy to the SD card and tap it in Files |

The default destination is the app's existing location, else the SD card if there is one, else `/data`. Apps on an SD card disappear with the card. `/data` totals 4.35 MB and is shared, so keep large apps on SD.

Reinstalling the same version is allowed. An older version is refused unless downgrades are explicitly allowed. A power cut mid-install leaves the old or the new version, never a mix. Installing over a running app stops it first, and the install is refused if it doesn't stop within 3 s.

## The ABI

Native apps link against nothing: imports stay undefined in the ELF and the OS binds them by name. Only names in [`sdk/abi/api_v1.sym`](../../sdk/abi/api_v1.sym) exist. That's about 2,400 names: around 300 Tab5 OS functions, 1,900 from LVGL and 170 from the C library and lwIP.

This has an unusual failure mode. **The compiler and linker accept a call to anything declared in a header.** Headers under `firmware/components` declare many functions that aren't exported, such as `t5_ui_get_stats` and `heap_caps_malloc`. If you call one, the build succeeds and the **install is refused**, naming the missing symbol.

Check before pushing:

```sh
riscv32-esp-elf-nm -D --undefined-only build/myapp.elf
```

Everything printed must appear in `api_v1.sym`. On the device, `app abi <file.elf>` does the same check. [API reference](../api-reference.md) is the readable form.

Common surprises, with what to use instead:

| Missing | Use |
| --- | --- |
| `exit` | `t5_app_exit(t5_app_self())` |
| `unlink`, `rmdir`, `access`, `fsync` | See [Storage](Storage.md#what-the-c-library-is-missing) |
| `pthread_*` | `t5_task_create`, `t5_mutex_create` |
| `heap_caps_malloc`, `memalign` | `malloc`, or `lv_draw_buf_create` for aligned pixels |
| `esp_timer_*` | `t5_time_us` to read, `lv_timer_create` or a task to schedule |
| `esp_log_*` | `printf` |
| `lv_obj_add_flag`, `lv_obj_remove_flag` | The 9.6 setters: `lv_obj_set_hidden`, `lv_obj_set_clickable`, `lv_obj_set_scrollable`... |
| `lv_obj_is_valid` | Track lifetimes yourself: [stale results](Threading.md#results-that-arrive-too-late) |
| `fputc`, `long double` | `fputs` or `fwrite`; `double` |

LVGL examples written for older versions may need the same flag translation.

`assert` and `abort` close your app and record a crash, rather than rebooting the device.

### Stability promise

An app built for API level N runs on any OS at level N or higher. Lock files record every symbol, struct layout and constant apps depend on, and CI fails if one changes. LVGL is pinned to 9.6.0 for level 1. The rules are in [ABI stability](../abi-stability.md).

## Signing

```sh
t5 keygen me.pem
t5 pack --sign me.pem          # or t5 run --sign me.pem
python3 tools/t5/t5pack.py --inspect build/com.you.app.t5app
```

The signature is ECDSA P-256 over the SHA-256 of every file. Unsigned apps install with a warning. A signature names the publisher by key fingerprint and doesn't grant any additional access.
