# App lifecycle

```c
T5_APP(.on_create = on_create, .on_resume = on_resume, .on_suspend = on_suspend,
       .on_destroy = on_destroy, .on_rotate = on_rotate, .on_alarm = on_alarm);
```

Every callback is optional. All but `on_alarm` run on the UI task, so LVGL is safe in them and blocking in them freezes the screen.

## Sequences

**Launch**

```text
ELF loaded -> root screen created -> on_create -> screen fades in -> on_resume
```

`on_resume` runs on every launch, right after `on_create`. So open hardware in `on_resume` and close it in `on_suspend`, and you don't need separate startup code.

**Sleep and wake**

```text
... -> Sleep: on_suspend (500 ms budget)
... -> wake: on_resume (queued to the UI task)
```

`on_suspend` runs on the way into **Sleep**, about 10 s after the screen goes off and only if no CPU or audio lock is held. It does *not* run at Dim or Screen off. The UI is already paused by then, so LVGL calls in it work but no timer or animation runs around it. `on_resume` runs once the screen is back, not on a wake that leaves the screen off.

**Leaving**

```text
Home, back, another app, or t5_app_exit():
    on_suspend -> on_destroy -> home screen shown
    -> your tasks get 150 ms to stop -> everything left is released -> ELF unloaded
```

`t5_app_running()` is already false when `on_suspend` runs at exit.

**Soft off**

The suspend hooks run, so `on_suspend` runs, unless the battery is critically low. The app is then gone. After the next boot the OS relaunches it, and `t5_app_restored()` is true.

**Crash or hang**

No `on_suspend` and no `on_destroy`. Your code is never called again and everything is reclaimed. **Memory pressure** is gentler: the app gets its exit callbacks.

## What each callback should do

| Callback | Do | Don't |
| --- | --- | --- |
| `on_create` | Allocate state, build the UI, load settings, start workers | Network, large file reads, anything over about 100 ms: it delays the app appearing |
| `on_resume` | Open the camera, restart polling, refresh stale data | Assume it's the first run or the fiftieth |
| `on_suspend` | Save state, close the camera, stop timers | Exceed 500 ms |
| `on_destroy` | Free what isn't shared with a task | **Free state a worker may still use**. See [exit](Threading.md#exit) |
| `on_rotate(app, degrees)` | Re-lay out anything flex doesn't handle | Rebuild the whole UI if you can avoid it |
| `on_alarm(app)` | Short headless job | Touch LVGL or `t5_app_screen` |

`on_destroy` is optional in most apps because the OS frees everything it tracks.

## State

```c
t5_app_set_user(app, state);        /* in on_create */
state_t *s = t5_app_get_user(app);  /* in later callbacks */
t5_app_t *app = t5_app_self();      /* the current app, from any app context */
```

A single heap-allocated struct passed as LVGL `user_data` is the usual shape. Statics work too, because each launch reloads the ELF, so they start fresh every time.

Callbacks can arrive before setup has finished if `on_create` bailed out early. The examples check `if (!s) return;` at the top of callbacks for that reason.

## Rotation

Only `orientation = "any"` apps rotate. The OS changes the screen size, flex and grid layouts reflow by themselves, and then `on_rotate` runs with 0, 90, 180 or 270. Your screen is 640 x 360 dp in landscape and 360 x 640 dp in portrait.

```c
static void on_rotate(t5_app_t *app, int degrees)
{
    state_t *s = t5_app_get_user(app);
    if (!s) return;
    bool portrait = degrees == 0 || degrees == 180;
    lv_obj_set_flex_flow(s->panels, portrait ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
}
```

## Session restore

After a soft off, the OS reopens the app that was in front, unless the wake was an alarm or the app crashed or hung.

```c
static void on_suspend(t5_app_t *app)
{
    saved_t st = current_position(app);
    T5_IGNORE(t5_app_state_save(app, &st, sizeof st));
}

/* in on_create */
if (t5_app_restored(app)) {
    saved_t st;
    size_t len;
    if (t5_app_state_load(app, &st, sizeof st, &len) == T5_OK && len == sizeof st) apply(&st);
}
```

The state is at most 8 KB (`T5_APP_STATE_MAX`) and is written atomically. Include a version field if the layout might change. Treat a load failure as a fresh start, since the state might be from an older version. There's a [full recipe](Cookbook.md#session-restore).

## Launch arguments

`t5_app_launch_arg(app)` returns the string the app was launched with, or `""`. Files and other apps use it to pass a path. Launching with `t5_apps_launch_arg` needs the `system` permission. If the app is already in front with a different argument, it's restarted.

## Going away by yourself

```c
t5_app_exit(t5_app_self());      /* after the current callback returns */
```

It's safe from any task, because the exit is queued to the UI task. `t5_app_running()` is false from the moment you call it.

## Auto-launch blocking

An app that crashes or hangs twice in a row is no longer auto-launched by session restore or alarms. The person can still open it, and a clean exit resets the count. `app crashes` lists the records.

## Events

```c
static void on_battery(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    const t5_ev_battery_t *b = data;      /* copy what you need; data doesn't outlive this call */
    ...
}
T5_IGNORE(t5_event_subscribe(T5_EV_BATTERY_UPDATE, on_battery, state));
```

Event handlers run on the **event task on core 0**, not the UI task. Don't touch LVGL in them: post with `t5_ui_async`. They must return quickly, because a handler that hasn't returned after 3 s is stopped and the app closed.

| Group | Events |
| --- | --- |
| Storage | `T5_EV_SD_INSERTED`, `T5_EV_SD_REMOVED`, `T5_EV_SD_MOUNT_FAILED` |
| Time | `T5_EV_TIME_SET` |
| Power | `T5_EV_POWER_STATE`, `T5_EV_POWER_SHUTDOWN` |
| Battery | `T5_EV_BATTERY_UPDATE`, `T5_EV_BATTERY_LOW`, `T5_EV_BATTERY_LOW_POWER`, `T5_EV_BATTERY_LOW_POWER_OFF`, `T5_EV_BATTERY_CRITICAL`, `T5_EV_CHARGE_STATE`, `T5_EV_USB_POWER` |
| Network | `T5_EV_NET_CONNECTED`, `T5_EV_NET_GOT_IP`, `T5_EV_NET_DISCONNECTED`, `T5_EV_NET_SCAN_DONE` |
| Bluetooth | `T5_EV_BLE_READY`, `T5_EV_BLE_SCAN_DONE`, `T5_EV_BLE_CONNECTED`, `T5_EV_BLE_DISCONNECTED` |
| UI | `T5_EV_UI_ROTATE` |
| Media and USB | `T5_EV_MIC_STATE`, `T5_EV_CAMERA_STATE`, `T5_EV_USB_ATTACHED`, `T5_EV_USB_DETACHED`, `T5_EV_USB_STORAGE` |

Subscriptions end with the app. Payload types are in `t5_core/event.h`.
