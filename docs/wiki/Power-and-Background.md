# Power and background

The Tab5 draws real power, and apps choose how much. Apps don't pick a power state: they hold wake locks, and the power service picks the deepest state those allow.

## States and timers

Timers are measured from the last input.

| State | Handheld (default) | Mounted profile | What runs |
| --- | --- | --- | --- |
| Active | Input | Input | Everything |
| Dim | 30 s | 5 min | Everything, backlight capped |
| Screen off | 2 min | 30 min | Tasks run. **The UI is paused**: LVGL timers, animations and `t5_ui_async` callbacks wait. The panel stays fed so touch can wake it |
| Sleep | 10 s after screen off | 10 s after screen off | Tasks still run (the CPU stays at 360 MHz in this build), unkept rails off, Wi-Fi off unless locked |
| Soft off | 24 h after sleep starts | Never | Power cut, except the RTC and standby |

In low-power mode, at 10% battery or by choice, Dim is 15 s, Screen off is 60 s, and brightness and frame rate are capped.

The person picks the profile, and on USB power idle soft off doesn't happen.

## Wake locks

```c
t5_wakelock_t *lock = t5_wakelock_acquire(T5_LOCK_SCREEN, "myapp");
...
t5_wakelock_release(lock);
```

| Lock | Effect | Use for |
| --- | --- | --- |
| `T5_LOCK_SCREEN` | Stays Active: no dim, no screen off | Dashboards, navigation, video |
| `T5_LOCK_CPU` | Allows Screen off, blocks Sleep | Finishing work with the screen off |
| `T5_LOCK_AUDIO` | As CPU; media keeps playing | Taken for you by an open output stream |
| `T5_LOCK_NET` | Allows Sleep, keeps Wi-Fi connected in power save | MQTT, a server, raw sockets |
| `T5_LOCK_RAIL` | Allows Sleep, keeps held rails on | A sensor that must stay powered |

`t5_wakelock_acquire_timeout(type, owner, ms)` releases by itself, and it's the safer choice for a job of bounded length. `NULL` means the table of 32 is full.

Hold locks for as short a time as possible. `power locks` on the console shows who holds what. Locks end with the app.

Most apps never need one. **Taps keep the screen on.** A screen lock is for when someone is watching but not touching.

### Locks you get for free

| Activity | Lock |
| --- | --- |
| Any open `t5/net.h` handle | `T5_LOCK_NET` as "net.io". **Plain BSD sockets don't count** |
| An open audio output stream | Audio. `t5_audio_out_pause` releases it |

So a transfer through `t5_http_*` carries on with the screen off. With nothing open, Sleep powers the radio down.

## Being a good citizen

- Use the longest timer that works.
- Pause polling while the screen is off. `T5_EV_POWER_STATE` tells you, or check `t5_power_state()`.
- Reduce work in low power. `t5_ui_low_power()`, or listen for `T5_EV_BATTERY_LOW_POWER`.
- Release rails you don't use. Grove 5 V isn't on unless someone holds it.
- Close the camera and microphone as soon as you can.
- The Power system app shows the average draw for each app. Check yours there.

```c
static void on_power(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    const t5_ev_power_t *p = data;
    state_t *s = arg;
    s->screen_on = p->to == T5_PWR_ACTIVE || p->to == T5_PWR_DIM;   /* read by the polling task */
}
```

## Reading the battery

```c
t5_battery_status_t b;
t5_battery_status(&b);       /* b.percent (-1 unknown), b.mv, b.mw, b.ma10, b.usb, b.minutes_left */
int pct = t5_battery_percent();
```

Battery policy is the OS's job. At 15% it warns once, at 10% it enters low-power mode, at 5% it soft-offs, and at 6.3 V it powers off immediately, without app callbacks.

## Scheduled wakes

The device can boot from soft off to run your job, then go back off.

```c
T5_IGNORE(t5_app_schedule_alarm(app, time(NULL) + 3600));
```

Manifest: `background_wake = true` and a `[background]` table. There's a [full recipe](Cookbook.md#scheduled-background-job). Logger is a real example.

Know the rules:

| Rule | Detail |
| --- | --- |
| **Runs only from soft off** | The job runs at the boot the RTC alarm causes. If the device is on or asleep when it comes due, your job doesn't run |
| **One alarm** | The OS keeps a single scheduled wake, so a later call replaces it, including another app's |
| Resolution | One minute, rounded up |
| Time limit | `max_seconds`, default 30, at most 300. Overruns are stopped and recorded as a crash |
| Environment | Own task, 8 KB stack, core 0. **No UI** |
| Reschedule | Each wake is one-shot. Schedule the next **first**, so a failure later doesn't end the series |
| After | The OS powers off again. The session isn't restored |
| Network | Wi-Fi connects by itself. Wait for `T5_NET_CONNECTED`, with a bound |

Because of the first rule, reschedule in `on_create` too, so opening the app recovers a series that lapsed.

Test without waiting: `app alarm <id>` on the console runs the job now.

**Hardware caution:** arming the RTC interrupt toggles main power on this board. The OS only arms it as the last step before soft off, and that's handled for you. Don't look for a way around it.

In Python, put the UI under `if __name__ == "__main__":`, so importing the module for `on_alarm()` doesn't build it.

## Keeping time

The RTC holds UTC. `t5_time_valid()` is false until the clock has been set by the RTC, NTP or a person. `t5_time_local(&tm)` gives local time in the zone from Settings. Use `t5_time_us()` for intervals, because it never goes backwards.
