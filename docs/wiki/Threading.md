# Threading

Tab5 OS runs your code on several tasks. Almost all concurrency bugs on it come from three things: blocking the UI task, touching LVGL from elsewhere, or a task outliving what it uses.

## Who runs what

| Code | Task | LVGL allowed? | Stack |
| --- | --- | --- | --- |
| `on_create`, `on_resume`, `on_suspend`, `on_destroy`, `on_rotate` | UI task | Yes | 16 KB, shared with LVGL |
| LVGL events, `lv_timer`, `lv_anim`, `md_list` bind and click | UI task | Yes | Same |
| `t5_ui_async`, `t5_ui_sync` functions | UI task | Yes | Same |
| `t5_app_request_permission` answer | UI task | Yes | Same |
| Your tasks | Their own | Only with the lock | What you asked for, in PSRAM |
| `t5_event_subscribe` handlers | Event task, core 0 | **No** | The OS's |
| UVC frame and MIDI callbacks | USB driver task | **No** | The OS's |
| `on_alarm` | Alarm task, priority 4, core 0 | **No UI exists** | 8 KB |

## The UI task budget

The panel refreshes every 16 ms, and your callback shares that time with rendering. Aim for:

| Time in a callback | Effect |
| --- | --- |
| Under 2 ms | Invisible |
| 2 to 16 ms | Can drop a frame during scrolling or an animation |
| Over 16 ms | Visible jank |
| Over 2 s | "App not responding" dialog |

**Never on the UI task:** network calls, `vTaskDelay` or `t5_delay_ms`, waits on queues or mutexes held by a slow task, large file reads or any file write to a slow card, `t5_camera_photo`, `t5_camera_record_start` and `t5_camera_record_stop` (the record calls return `T5_ERR_STATE` there), heavy maths, JPEG decodes, `t5_storage_sd_format`.

Short reads from `/data` and small `t5_storage_write_atomic` calls are acceptable on the UI task. The examples do this.

The UI task's stack is 16 KB and LVGL uses part of it. Keep local arrays small, about 2 KB at most, and don't recurse. An overflow closes the app.

## Creating tasks

```c
t5_task_t handle;
t5_err_t e = t5_task_create("worker", fn, arg, 8192, 2, T5_CORE_ANY, &handle);
```

| Argument | Advice |
| --- | --- |
| Stack, bytes | 4 KB for simple loops. 8 KB if it uses `printf` or files. **12 KB if it uses TLS or HTTP.** The bottom 512 bytes are a guard |
| Priority | 0 to 2. **Anything higher is silently capped to 2**, so the OS always runs first |
| Core | `T5_CORE_ANY` by default. Pin heavy maths to core 0 to keep it away from the UI task |

`xTaskCreatePinnedToCore` is wrapped the same way. An app can have at most 16 tasks, and creation fails past that. Stacks and kernel objects are in PSRAM.

A task function may simply return when it's done. Don't call `vTaskDelete(NULL)` needlessly.

## Getting back to the UI

Three ways, best first.

### 1. `t5_ui_async`: fire and forget

```c
static void apply(void *arg)            /* runs on the UI task */
{
    update_t *u = arg;
    lv_label_set_text(u->state->label, u->text);
    free(u);
}

/* on your task: */
update_t *u = malloc(sizeof *u);
if (u) {
    u->state = s;
    snprintf(u->text, sizeof u->text, "%d", value);
    if (t5_ui_async(apply, u) != T5_OK) free(u);
}
```

The queue holds 512 calls, and past that it returns `T5_ERR_BUSY`. A fast producer can fill it, so coalesce:

```c
if (!s->ui_queued) {                    /* volatile bool, cleared first thing in the callback */
    s->ui_queued = true;
    if (t5_ui_async(show_latest, s) != T5_OK) s->ui_queued = false;
}
```

Store the newest value under a mutex, and let `show_latest` read it. That keeps the UI current however fast the data arrives. Calls still queued when the app exits are dropped by the OS and never run.

### 2. A UI timer that polls

```c
s->tick = lv_timer_create(tick, 200, s);
```

`tick` checks a mutex-protected "result ready" flag. [Weather](https://github.com/tab5os/weather) uses this. It's the simplest design when the result is big, and the 200 ms poll is cheap. Note that LVGL timers stop while the screen is off.

### 3. `t5_ui_lock`: when you must

```c
if (t5_ui_lock_timeout(100)) {
    lv_label_set_text(label, text);
    t5_ui_unlock();
}
```

Hold it briefly. The UI task can't render while you hold it. Prefer the timeout form, and never do I/O inside it.

`t5_ui_sync(fn, arg, timeout_ms)` runs a function on the UI task and waits. It's useful when you need a value back. It runs directly if you're already on the UI task. If it times out and the call hadn't started, the call is cancelled.

## Results that arrive too late

A worker finishes after the person left that page. If the callback touches a deleted widget, the app crashes. `lv_obj_is_valid` isn't exported, so use a generation counter:

```c
typedef struct { state_t *s; uint32_t generation; ... } job_t;

/* starting a job, UI task */
j->generation = ++s->generation;

/* closing the page, UI task */
s->generation++;
s->page_label = NULL;

/* the result, UI task */
if (j->generation == s->generation) lv_label_set_text(s->page_label, j->text);
free(j);
```

The [worker recipe](Cookbook.md#background-work-with-a-ui-result) shows it in full.

## Exit

At exit, in order:

1. `t5_app_running()` becomes false.
2. `on_suspend`, then `on_destroy`, run on the UI task.
3. The home screen is shown.
4. Your tasks get **150 ms** to finish by themselves.
5. Any left are deleted where they stand, then timers, animations, UI, ports, rails, wake locks, other handles and finally the heap are released.

Rules that follow:

```c
while (t5_app_running(app)) {           /* every loop */
    if (xQueueReceive(q, &msg, pdMS_TO_TICKS(100))) handle(&msg);   /* short waits */
}
```

- **Wait in slices of about 100 ms,** not forever, so you notice the exit.
- **Don't `free()` shared state in `on_destroy`** while a worker might still be running. Leave it: the OS frees the heap after the task is gone. [Weather](https://github.com/tab5os/weather) has a comment saying exactly this.
- **Don't wait for a worker from the UI task.** A worker blocked in `t5_ui_lock` or `t5_ui_sync` while the UI task waits for it is a deadlock. The camera service had this bug.
- A blocking `t5_http_*` or `t5_ws_*` call ends within about 50 ms if another task closes its handle.
- `t5_port_close` isn't safe while another task is using that handle. Close it from the task that uses it.

## Shared data

| Tool | Use |
| --- | --- |
| `t5_mutex_create` | Recursive mutex, tracked |
| `xQueueCreate` | Messages between tasks |
| `xSemaphoreCreateBinary` | "Something happened" |
| `volatile bool` / `int` | A single flag or counter, one writer |

`t5_mutex_t` needs no cleanup if you forget. Don't hold one across I/O or across a UI call, and don't take the UI lock while holding a mutex a UI callback also takes: that's a lock-order deadlock.

## CPU hogs

The OS samples your tasks once a second. A task that uses over 90% of a core for three checks in a row is moved to priority 1, and is taken off the CPU for about a tenth of the time.

So a legitimately heavy computation should yield: do a chunk, then `t5_delay_ms(1)` or more, so it stays below 90% in any given second. Sleep or block in every loop. A hogging task on core 0 competes with services at the same priority.

## Python

A Python app has one thread of execution: the VM task. Callbacks run between bytecodes and during `t5.sleep_ms`. None of the above applies, other than keeping loops responsive. See [Python apps](Python-Apps.md).
