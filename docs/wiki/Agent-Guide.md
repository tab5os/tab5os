# Agent guide

For coding agents writing Tab5 OS apps. Everything here is a directive. The human-oriented reasoning is in the linked pages. Read this page in full before writing code, then read only the pages your task needs.

## Your training data is wrong here

- This is not generic ESP-IDF. No `app_main`, no `idf.py`, no components. An app is a `.t5app` built with `t5`.
- This is LVGL **9.6**. Older examples use functions that don't exist: `lv_obj_add_flag`, `lv_obj_remove_flag`. Use `lv_obj_set_hidden`, `lv_obj_set_clickable`, `lv_obj_set_scrollable`.
- A successful build proves little. Undefined symbols link fine and fail at install.
- Only names in `sdk/abi/api_v1.sym` exist. Headers declare more. **Grep that file before using any name you're not sure of.**

## Workflow

1. Pick a runtime: [C or Python](Platform-Overview.md#c-or-python). Default to Python for tools that mostly wait, and C for anything drawing or computing continuously.
2. Start from the nearest [Cookbook](Cookbook.md) recipe. Do not start from a blank file.
3. `t5 new "Name" --id com.you.name [--python]`, and fill in `manifest.toml` permissions.
4. Write the code, following the rules below.
5. Run the verification steps. Report what you actually ran.

## Hard rules

| # | Rule |
| --- | --- |
| 1 | No blocking on the UI task: every callback, LVGL event and `lv_timer`. No network, delay, large I/O, or wait on a slow mutex |
| 2 | LVGL only on the UI task, or inside `t5_ui_lock()`/`t5_ui_unlock()`. Event handlers, USB callbacks and `on_alarm` are **not** on the UI task |
| 3 | From a task, update UI with `t5_ui_async`. Coalesce high-rate updates with a "queued" flag |
| 4 | Every task loop: `while (t5_app_running(app))`, with waits of 100 ms or less |
| 5 | Never free state in `on_destroy` that a task may touch |
| 6 | Late results must not touch deleted widgets: use a generation counter |
| 7 | Check every `malloc` and every `t5_err_t`. Use `T5_IGNORE(expr)` with a reason comment when it doesn't matter. `(void)` doesn't work |
| 8 | Sizes in `T5_DP(n)`. Layouts in flex or grid. No hardcoded screen dimensions |
| 9 | Use `md_*` widgets first. `md_list` for lists over about 30 rows |
| 10 | Under `t5_app_screen()` only. Don't create LVGL screens |
| 11 | Stacks: 4 KB simple, 8 KB with files or `printf`, 12 KB with TLS or HTTP |
| 12 | Priorities at most 2. Yield in heavy loops |
| 13 | Persist with `t5_storage_write_atomic`, on change, debounced. Never rely on `on_suspend` alone |
| 19 | Own data under `t5_app_data_dir` (1 MB at most). Anything else: `t5_file_pick`, or a declared `shared_read`, `shared_write` or `all_files` |
| 14 | Open the camera, and such, in `on_resume`; close it in `on_suspend` |
| 15 | Don't set a widget to the value it already has. Compare first |
| 16 | Declare only the permissions you use. Camera and mic also need `t5_app_request_permission` |
| 17 | Use `float` and `sinf`, not `double` and `sin`, in hot code |
| 18 | `snprintf`, not `sprintf`; bounds-check all external input |

## Standard skeleton (C)

```c
#include <stdlib.h>
#include <t5/app.h>
#include <t5/ui.h>

typedef struct {
    t5_app_t *app;
    lv_obj_t *label;
} state_t;

static void back(lv_event_t *e)
{
    (void)e;
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);
    if (!s) return;
    s->app = app;
    t5_app_set_user(app, s);
    lv_obj_t *page = md_page_create(t5_app_screen(app), "Title", back, s, NULL);
    lv_obj_t *body = md_page_body(page);
    lv_obj_set_style_pad_all(body, T5_DP(16), 0);
    s->label = md_label_create(body, "Hello", MD_TYPE_BODY1);
}

T5_APP(.on_create = on_create);
```

## Decision tables

**Moving data from a task to the UI**

| Situation | Use |
| --- | --- |
| A one-off result | `t5_ui_async` with a heap-allocated struct |
| Rapid updates | Latest value under a mutex, `t5_ui_async` with a queued flag |
| Big results, simple code | A 200 ms `lv_timer` polling a ready flag |
| Need a return value | `t5_ui_sync` with a timeout |

**Keeping a list**

| Rows | Use |
| --- | --- |
| Up to about 30, static | `md_row_create` |
| More, or growing | `md_list_create` |

**Storing data**

| Data | Where and how |
| --- | --- |
| Settings | `t5_app_data_dir`, `t5_storage_write_atomic` |
| Resume position | `t5_app_state_save` in `on_suspend` |
| Large or media | A shared folder such as `/sd/Downloads` with `shared_write`, after `t5_storage_sd_state() == T5_SD_MOUNTED` |
| A file the person has | `t5_file_pick` |
| Static assets | The package, `t5_app_install_dir` |

**Networking**

| Need | Use |
| --- | --- |
| Small GET or POST | `t5_http_fetch` |
| Big body | `t5_http_open` and `t5_http_read` |
| Stream | `t5_ws_*`, or `t5_mqtt_*` |

## What doesn't exist

Don't generate code using these. Substitutes are in [the ABI table](Manifest-and-Packaging.md#the-abi).

C++ · `exit` · `unlink`, `rmdir`, `access`, `fsync`, `open`, `read`, `write` · `fputc` · `long double` · `pthread_*` · `heap_caps_*`, `memalign` · `esp_timer_*`, `esp_log_*`, `gpio_*`, `i2c_*`, other driver APIs · `lv_obj_add_flag`, `lv_obj_remove_flag`, `lv_obj_is_valid` · `t5_ui_get_stats`, `t5_power_low_power`, `t5_storage_blob_*` · LVGL file paths and PNG decoding · any glyph outside Latin-1 · any icon not in `md/md_icons.h`.

## Verification

Run what you can, and say exactly what you ran.

```sh
. tools/env.sh
cd <app folder>
t5 app-build                                                   # 1. builds
riscv32-esp-elf-nm -D --undefined-only build/<target>.elf | awk '{print $NF}' | sort -u > /tmp/imports
awk -F' *[|] *' '!/^#/ && NF>1 {print $1}' "$T5_ROOT/sdk/abi/api_v1.sym" | sort -u > /tmp/abi
comm -23 /tmp/imports /tmp/abi                                 # 2. must print nothing
python3 "$T5_ROOT/tools/t5/t5pack.py" --inspect build/<id>.t5app   # 3. the package is valid
```

With a device:

```sh
t5 run --usb                                                   # 4. launches; watch for crash lines
t5 screenshot out.png                                          # 5. look at the result
t5 console "app usage" "ui" "stacks"                           # 6. memory, longest UI loop, stacks
t5 console "app stress com.you.name 20"                        # 7. returns to baseline
```

Keep any single device test under five minutes. Don't arm RTC or IMU interrupts on a device nobody is standing next to: `rtc timer`, `alarm` and `power off --force` can leave the board off until someone presses the button.

Without a device, say that you only built it. Don't claim it works.

Check before reporting completion, using the [hardening checklist](Stability.md#hardening-checklist).

## Common mistakes to avoid

| Don't | Do |
| --- | --- |
| `vTaskDelay` in an LVGL callback | A timer or a task |
| `lv_label_set_text` from a task | `t5_ui_async` |
| `t5_http_fetch` in a click handler | Start a task |
| A worker `for (;;)` with `T5_NET_FOREVER` | A running-checked loop with 500 ms timeouts |
| A 4 KB stack with TLS | 12 KB |
| `lv_obj_set_size(o, 200, 100)` | `lv_obj_set_size(o, T5_DP(100), T5_DP(50))` |
| `lv_image_set_src(img, "S:/a.png")` | Raw RGB565 or `t5_jpeg_decode` |
| 500 `md_row_create` calls | `md_list_create` |
| Setting a label 60 times a second | 2 to 5 Hz, only on change |
| Saving only in `on_suspend` | Saving on change |
| `fopen(path, "w")` for settings | `t5_storage_write_atomic` |
| `fopen("/sd/x.txt", ...)` with no permission | Declare one, or use `t5_file_pick` |
| Opening the camera in `on_create` | `on_resume`, after permission |
| Assuming Wi-Fi | Checking `t5_net_status`, with a visible offline state |
| Freeing shared state in `on_destroy` | Leaving it for the OS |

## When something is unclear

Read the source, in this order:

1. `sdk/abi/api_v1.sym`: does it exist, what permission does it need
2. `firmware/components/<name>/include/<name>/*.h`: what it does
3. `apps/examples/`: how real apps use it
4. `firmware/components/t5_app/lifecycle.c`, `wrappers.c`, `isolate.h`: what the OS does around you
5. `docs/bench-results.md`: what was actually measured

Don't guess at an API, and don't invent a permission. If the platform can't do something, say so, and offer the closest alternative.
