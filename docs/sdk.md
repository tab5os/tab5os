# Tab5 OS app SDK (API level 1)

Nov 5, 2026 · Phase 5 of the [implementation plan](tab5os-implementation-plan.md)

Native apps are position-independent RISC-V ELFs that the OS loads into PSRAM from the SD card or `/data`. They link against nothing; every function they call is bound at load time by name against the published symbol list, [`sdk/abi/api_v1.sym`](../sdk/abi/api_v1.sym).

## Build an app

```sh
. tools/env.sh                       # ESP-IDF v6.1: the compiler
t5 build                             # the firmware build supplies headers and LVGL config
cp -r sdk/templates/c myapp && cd myapp
cmake -B build -G Ninja -DT5_SDK=$PWD/../sdk && cmake --build build
# -> build/hello.elf, build/hello.t5app
```

Copy the `.t5app` to the SD card and run `app install /sd/hello.t5app` on the console (the Files app does this in Phase 6). `t5 apps` builds the test apps in `test/apps/`.

The build uses the firmware's own compile flags for its ABI table (`sdk/cmake/app_flags.py` reads them from `firmware/build-<rev>/compile_commands.json`), so an app sees exactly the headers and LVGL configuration the OS was built with. One `.t5app` runs on both chip revisions: nothing in it depends on `rev1` or `rev3`.

## What an app looks like

```c
#include <t5/app.h>
#include <t5/ui.h>

static void on_create(t5_app_t *app) {
    lv_obj_t *label = md_label_create(t5_app_screen(app), "Hello, Tab5", MD_TYPE_HEADLINE5);
    lv_obj_center(label);
}

T5_APP(.on_create = on_create);
```

| Header | What it gives you |
| --- | --- |
| `t5/app.h` | Lifecycle (`T5_APP`, `on_create`, `on_resume`, `on_suspend`, `on_destroy`, `on_rotate`, `on_alarm`), the app's screen and data directory, permissions, scheduled alarms, tasks and mutexes, events |
| `t5/ui.h` | LVGL 9.6, the MD2 layer (`md_*`: theme, type, elevation, ripple, app bar, buttons, FAB, card, recycled list, dialog), `T5_DP()`, `t5_ui_lock`/`t5_ui_async` |
| `t5/port.h` | Named ports (`grove`, `mbus.*`, `ext`, `rs485`, `stamp`), Modbus RTU, rails |
| `t5/power.h` | Wake locks, power state, battery (read-only) |
| `t5/sys.h` | Time, IMU, network status, FreeRTOS |

Plus the C library functions listed in `api_v1.sym` (`malloc`, `snprintf`, `fopen`, `sinf`…) and lwIP sockets for apps with the `net` permission.

## Rules

- **Threads.** Every callback runs on the UI task, so LVGL calls are safe there. Tasks you create (`t5_task_create`, or `xTaskCreate`) must use `t5_ui_async()` or `t5_ui_lock()`/`t5_ui_unlock()` for LVGL.
- **Cleanup.** The OS tracks your heap, tasks, LVGL timers and animations, files, queues and mutexes, event subscriptions, port claims, rails and wake locks, and frees what's left when the app exits or crashes. Release order: tasks, timers, animations, UI, ports, rails, wake locks, other handles, heap. Loops in your tasks should check `t5_app_running()` so they stop before they are deleted.
- **UI.** Build under `t5_app_screen()`. Objects on `lv_layer_top()` (dialogs) are tracked too. Other screens you create are not.
- **LVGL timers** don't delete themselves when their repeat count runs out: they pause, and the OS deletes them on exit.
- **Sizes** are in dp: `T5_DP(16)` is 32 px. The screen is 640 × 360 dp in landscape.
- **Memory from `malloc` is yours.** Don't pass it to an OS function that frees it, and free memory from libc helpers (`strdup`) with `free()` as usual.

## Manifest

```toml
id = "com.example.airlog"
name = "Air Log"
version = "1.2.0"
api = 1
runtime = "native"
entry = "app.elf"
orientation = "landscape"     # or "portrait", "any" (then on_rotate is called)

[permissions]
ports = ["grove"]             # only these ports can be opened
net = true
background_wake = true

[background]
entry = "on_alarm"
max_seconds = 30
```

Permission groups: `camera`, `mic`, `net`, `ble`, `ports`, `usb`, `storage`, `background_wake`, `system`. Calling a function whose group the manifest doesn't declare gets a stub that returns `T5_ERR_DENIED` (or `NULL`, `false`, `0`). Camera and mic also ask the person on first use (`t5_app_request_permission()`). **These are consent and correctness checks, not a security boundary:** a native app shares the OS address space and can reach the hardware directly. The installer says so.

## Stability

- An app built for API level N runs on any OS with level ≥ N.
- `api_v1.sym` stores each symbol's full prototype. The firmware build redeclares all of them, so a header that drifts from the published ABI fails the build. CI (`tools/abi/abi.py check --base <ref>`) fails if a symbol of a published level is removed or its prototype changes.
- LVGL is pinned to 9.6.0 for level 1 (all 1,903 public LVGL functions in the OS build are exported).
- At install the OS reads the ELF's imports and refuses the package if any isn't exported, naming the missing ones (`app abi <file.elf>` on the console does the same check).

## Crashes and hangs

A crashing native app reboots the device. The crash guard records which app was running (RTC memory survives the reset), returns to the launcher, saves the core dump to `/data/sys/dumps/<id>.elf`, and stops auto-launching the app (session restore, alarms) after two crashes in a row. If an app blocks the UI task for 2 s, the OS draws "App not responding" itself, outside LVGL: Wait, or Close app (a restart to the launcher, recorded as a hang).

## Tools

| Tool | Does |
| --- | --- |
| `sdk/cmake/t5_app.cmake` | `t5_app(name SRCS … MANIFEST …)`: ELF and `.t5app` |
| `tools/t5/t5pack.py` | Pack or inspect a `.t5app` |
| `tools/abi/abi.py` | Generate the export table, update prototypes, add LVGL exports, check stability |
| `t5 sim` | The SDL simulator with the same MD2 code |
