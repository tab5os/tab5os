# Troubleshooting

## Build and install

| Symptom | Cause | Fix |
| --- | --- | --- |
| `no firmware build at ...` | Apps compile against a firmware build | `t5 build` in the repo |
| `RISC-V compiler isn't on PATH` | Environment not set up | `. tools/env.sh` |
| Install refused, naming a symbol | You called something not exported. It linked anyway | Check [the ABI](Manifest-and-Packaging.md#the-abi). `riscv32-esp-elf-nm -D --undefined-only` |
| Install refused: C++ symbols | No C++ runtime in the ABI | Write C |
| Pack error about the manifest | Unknown key, or TOML the device can't read | [Manifest reference](Manifest-and-Packaging.md) |
| `implicit declaration of lv_obj_add_flag` | That's not in LVGL 9.6 | `lv_obj_set_hidden`, `lv_obj_set_clickable`... |
| An older version is refused | Downgrades are blocked | Raise `version`, or `t5 rm` first |
| `the Tab5 didn't answer` | Console busy or old firmware | Close other monitors; update the OS |

## Runtime: crashes and hangs

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| "stopped: it crashed" toast | A bad pointer, often a `NULL` from `malloc` or a deleted widget | [Map the offset](Debugging-and-Testing.md#crashes) |
| Crash right after leaving a page | A late result touched a deleted widget | [Generation counter](Threading.md#results-that-arrive-too-late) |
| Crash when exiting | A worker used state freed in `on_destroy` | Don't free shared state there |
| "App not responding" | Blocking the UI task | Move the work to a [task](Threading.md) |
| Random crashes under load | Touching LVGL from a task or an event handler | `t5_ui_async` |
| `ran out of stack` | Stack too small | 12 KB for TLS. On the UI task, fewer locals |
| App won't relaunch by itself | It crashed twice in a row | Fix it; a clean exit resets the count |
| "closed: the device is low on memory" | PSRAM or internal RAM critically low | Smaller buffers, close what you aren't using |

## Runtime: nothing happens

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| A call returns `T5_ERR_DENIED`, or a stub result | Permission not in the manifest | Add it. `app usage` shows denied imports |
| A port opens, but the module is dead | The 5 V rail isn't on | `t5_rail_acquire(T5_RAIL_EXT5V, ...)` |
| `T5_ERR_BUSY` on a port | Pin held by another claim | `t5_port_open_ex` gives the reason |
| `printf` output is missing | It only reaches USB-C | `t5 run --usb` |
| UI stops updating with the screen off | The UI pauses in Screen off and Sleep | Expected. Resume in `on_resume` or on wake |
| A task seems slower after a while | Demoted as a CPU hog | [Yield](Threading.md#cpu-hogs) |
| `t5_task_create` priority has no effect | Capped at 2 | Expected |
| Camera dead after sleep | Not reopened | Open it in `on_resume`, close it in `on_suspend` |
| USB device gone after sleep | USB power dropped; it re-enumerates | Reopen after `T5_EV_USB_ATTACHED` |
| Alarm job never runs | The device wasn't in soft off when it came due, or another app rescheduled the single alarm | [Rules](Power-and-Background.md#scheduled-wakes) |
| Image is blank | PNG or a path passed to `lv_image_set_src` | [Images](UI-Guide.md#images) |
| Image shows garbage | The buffer was freed or reused | Keep it alive while shown |
| Characters are blank | Outside the Latin-1 fonts | [Fonts](UI-Guide.md#text-and-fonts) |
| App not in the launcher after the card was removed | Installed on SD | Install to `/data`, or re-insert the card |

## Network

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Crash or stack overflow in TLS | The task's stack is too small | 12 KB |
| Handshake fails | Clock wrong, or a private CA | `t5_time_valid()`; use `ca_pem` |
| Connection drops in Sleep | Plain BSD sockets hold no lock | Take `T5_LOCK_NET`, or use `t5/net.h` |
| `T5_ERR_BUSY` opening a handle | Past 12 handles | Close them. `net handles` |
| Slow after hours of uptime | The radio link degraded | `net reset`; handle it in code with reconnects |
| `T5_ERR_NO_MEM` from `t5_http_fetch` | The body is bigger than your buffer | Stream it |

## Performance

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Janky scrolling | Long list of real objects, or heavy `bind` | `md_list`, cheaper `bind` |
| Full-screen animation under 30 fps | PSRAM bandwidth | Change fewer pixels |
| Everything sluggish on a busy page | LVGL pool overflow, or too many objects | `ui mem`; fewer objects |
| The screen redraws when nothing changes | Setting values to what they already are | Compare first; `ui frames` |
| High idle power | Fast timers, wake locks, open radio handles | Longer periods, release locks |
| UI degrades when something else runs | Internal RAM short | Close what you aren't using; check the `heap dma` line in `sys` |

## Data

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Settings lost after a power cut | Written with `fopen` and `fclose`, or only in `on_suspend` | `t5_storage_write_atomic`, on change |
| Old settings break the new version | Struct layout changed | Version field and migration |
| `rename` fails on `/sd` | FAT won't replace an existing file | `remove` first |
| `fopen` fails with `EACCES` | Outside the app's [file scope](Storage.md#which-files-an-app-can-use) | Use the picker, or declare `shared_read`, `shared_write` or `all_files` |
| Denied on first use, then fine | A file permission was checked on the UI task, which can't wait for the prompt | Call `t5_app_request_permission` first, or do the file work on a task |
| Write fails with `ENOSPC` in the data folder | Over the 1 MB quota, or `/data` nearly full | Move large data to the SD card |
