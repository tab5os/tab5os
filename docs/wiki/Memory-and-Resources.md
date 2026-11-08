# Memory and resources

## Where things live

| Thing | Memory | Limit |
| --- | --- | --- |
| Your code and static data | PSRAM | None specific |
| `malloc`, `lv_malloc`, `strdup` | Your own PSRAM heap | Refused rather than taking the last 2 MB |
| Draw buffers, snapshots | PSRAM, freed at exit | Free PSRAM |
| Task stacks, queues, mutexes | PSRAM | 16 tasks, 256 handles |
| LVGL objects and styles | The OS's 88 KB internal pool | Shared with the shell; see [object count](Performance.md#object-count) |
| Python GC heap | PSRAM | 8 MB at most |

With audio and the camera idle, about 21 MB of PSRAM is free for your app. With both active, about 14 MB. You can't allocate internal RAM, by design: Wi-Fi, DMA and the UI need it.

## The app heap

Your heap is a set of private PSRAM chunks: first 128 KB, then doubling to 2 MB. Consequences:

- An overrun damages only your own memory.
- Exit returns it all at once, so a leak ends with the app.
- A chunk that empties is returned, unless it's the only one.
- `free` of a bad pointer, or of something already freed, is logged and ignored.
- Allocation is locked, so it isn't free. Avoid it in tight loops.

Fragmentation matters for long-running apps that allocate variable-sized blocks. Prefer fixed buffers, pools, and one big allocation at startup.

**Always check `malloc`.** It returns `NULL` under memory pressure, and a `NULL` store closes your app.

```c
uint16_t *px = malloc(w * h * 2);
if (!px) {
    md_snackbar_show("Not enough memory", NULL, NULL, NULL);
    return;
}
```

## Memory pressure

| Condition | What happens |
| --- | --- |
| Heap growth would leave under 2 MB free PSRAM | `malloc` returns `NULL` |
| Free PSRAM under 1 MB for 1.5 s | The foreground app is closed, with its exit callbacks |
| Internal RAM under 16 KB for 1.5 s | Same |

Large consumers, such as the camera at about 6.5 MB, should be sized against this. Check `malloc` for your frame buffers even when they fit today, because what's free depends on what's open.

## What the OS tracks

When your app exits or crashes, it releases all of this:

| Resource | Created by |
| --- | --- |
| Heap blocks | `malloc`, `calloc`, `realloc`, `strdup`, `lv_malloc`... |
| Tasks | `t5_task_create`, `xTaskCreatePinnedToCore` |
| Queues, semaphores, mutexes | `xQueueCreate`, `xSemaphoreCreate*`, `t5_mutex_create` |
| Timers | `lv_timer_create` |
| Animations | `lv_anim_start` |
| UI objects | Anything under `t5_app_screen()` or on `lv_layer_top()` |
| Draw buffers | `lv_draw_buf_create`, `lv_snapshot_take` |
| Files | `fopen` |
| Sockets | BSD sockets and every `t5_net` handle |
| Event subscriptions | `t5_event_subscribe` |
| Ports, rails, wake locks | `t5_port_open`, `t5_rail_acquire`, `t5_wakelock_acquire` |
| Camera, audio streams | Their open calls |

The release order is tasks, timers, animations, UI, ports, rails, wake locks, other handles, heap.

**Not tracked:** other LVGL screens you create with `lv_obj_create(NULL)`, and memory obtained from elsewhere.

`test/apps/leaky` deliberately leaks all of it, and 500 open-and-close cycles returned to baseline.

Cleanup at exit is a safety net. **While the app runs, leaks still accumulate**, so release what you're done with: a long-running app that opens a file per request will reach the 256-handle cap.

## Handle limits

| Limit | Value | Past it |
| --- | --- | --- |
| Tasks | 16 | Creation fails |
| Tracked handles of all kinds | 256 | Creation fails |
| `t5_net` handles | 12 | `T5_ERR_BUSY` |
| BSD sockets | 8 | `ENFILE` |
| `t5_ui_async` queue | 512 | `T5_ERR_BUSY` |
| Wake locks, system-wide | 32 | `NULL` |
| Open ports | One claim per pin | `T5_ERR_BUSY` |

## LVGL timers

Timers you create are set not to auto-delete. A timer that exhausts its repeat count pauses, and the OS deletes it at exit. That's different from stock LVGL, so a one-shot in a long-lived app should be deleted when done, or reused with `lv_timer_reset` and `lv_timer_resume`.

## Who frees what

Memory from `malloc` is yours. Don't pass it to an OS function that frees it unless the docs say so. LVGL may free user data if you registered `lv_event_free_user_data_cb`, and the OS handles that case. Text passed to `lv_label_set_text` is copied. **Data passed to `lv_image_set_src` isn't**: it must stay alive as long as the image shows it.

## Stack sizes

| Task | Stack |
| --- | --- |
| UI task | 16 KB, shared with LVGL. Keep frames small |
| `on_alarm` | 8 KB |
| Worker, simple | 4 KB |
| Worker, file or `printf` | 8 KB |
| Worker, TLS or HTTP | 12 KB |

`stacks` on the console shows high-water marks for every task. If yours has under about 1 KB unused, increase it. An overflow closes the app, and the fault line says `ran out of stack`.

## Internal RAM

Apps can't take it directly, and their tasks, heaps and TLS buffers are all in PSRAM. It's still shared with what you open: the camera, for instance, took free internal RAM down to about 74 KB in one bench run. Below 16 KB for 1.5 s, the foreground app is closed. `sys` shows it: read the `heap dma` line, because `heap int` also counts slow memory the UI and DMA can't use.
