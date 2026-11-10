# Limits

Every hard number an app can run into, with where it's defined. API level 1.

## Time

| Limit | Value | Source |
| --- | --- | --- |
| Frame period | 16 ms (33 ms in low-power mode) | `LV_DEF_REFR_PERIOD` |
| Panel scan-out | about 64 Hz | bench |
| UI block before "not responding" | 2 s | `t5_app/anr.c` |
| "Wait" extends by | 5 s | `t5_app/anr.c` |
| Event handler limit | 3 s | `t5_app/wrappers.c` |
| `on_suspend` budget | 500 ms | `t5_app/lifecycle.c` |
| Task stop grace at exit | 150 ms | `t5_app/lifecycle.c` |
| CPU hog | over 90% of a core, 3 checks a second apart | `t5_app/isolate.h` |
| `on_alarm` default and maximum | 30 s, 300 s | `t5_pkg/manifest.h` |
| Alarm resolution | 1 minute, rounded up | `t5_power/power.h` |
| Full-screen animation ceiling | about 27 fps | bench |
| Connect default | 10 s | `t5_net/sock.h` |

## Tasks and handles

| Limit | Value | Past it |
| --- | --- | --- |
| Tasks per app | 16 | Creation fails |
| Task priority, apps | 2 at most | Silently capped |
| Tracked handles per app | 256 | Creation fails |
| `t5/net.h` handles per app | 12 | `T5_ERR_BUSY` |
| BSD sockets per app | 8 | `ENFILE` |
| `t5_net_poll` items | 16 | Error |
| `t5_ui_async` queue | 512 | `T5_ERR_BUSY` |
| `lv_async_call` queue per system | 256 | `LV_RESULT_INVALID` |
| Wake locks, system-wide | 32 | `NULL` |
| Rail holds, system-wide | 32 | Error |
| Minimum task stack | 1 KB (512 bytes guard) | `T5_ERR_INVALID_ARG` |

## Memory

| Limit | Value |
| --- | --- |
| PSRAM total | 32 MB |
| Free for apps, media idle | about 21 MB |
| Free for apps, media active | about 14 MB |
| PSRAM an app's heap can't take | Last 2 MB |
| App closed when free PSRAM is under | 1 MB for 1.5 s |
| App closed when internal RAM is under | 16 KB for 1.5 s |
| Heap chunk growth | 128 KB, doubling to 2 MB |
| LVGL object pool | 88 KB (requests up to 4 KB) |
| UI task stack | 16 KB |
| `on_alarm` stack | 8 KB |
| Python GC heap | 64 KB to 8 MB, default 2 MB |
| L2 cache | 128 KB, 64-byte lines |
| Session state | 8 KB (`T5_APP_STATE_MAX`) |
| Draw buffer alignment | 64 bytes |

## Storage

| Limit | Value |
| --- | --- |
| `/data` total | 4.35 MB, shared |
| App data folder quota | 1 MB (`T5_FS_QUOTA_BYTES`) |
| `/data` kept free for the OS | 128 KB |
| Files open for writing in the data folder | 12 |
| Files opened through the picker, per run | 8 |
| Paths apps can use | 191 bytes, absolute |
| Package path buffer | 192 bytes |
| Zip entry name | 127 bytes |
| Launch argument | 127 bytes |

## Manifest

| Field | Limit |
| --- | --- |
| `id` | 63 bytes, reverse DNS, at least two parts |
| `name` | 47 bytes |
| `version` | 15 bytes, semver |
| `entry`, `icon` | 63 bytes |
| Icon file | PNG, at most 512 x 512 px and 512 KB, not interlaced. 112 x 112 px is drawn pixel for pixel |
| `ports` | 8 entries |
| Port name | 15 bytes |
| `background.entry` | 31 bytes |

## Display and text

| Limit | Value |
| --- | --- |
| Landscape | 640 x 360 dp, 1280 x 720 px |
| Portrait | 360 x 640 dp, 720 x 1280 px |
| px per dp | 2 |
| Status bar | 24 dp |
| App bar (when on) | 48 dp |
| Gesture bar | 16 dp (none in full screen) |
| Icons | 160 glyphs |
| Text fonts | ASCII, Latin-1, some punctuation |
| Icon sizes | 18, 24, 40 dp |

## Media

| Limit | Value |
| --- | --- |
| Audio output streams per app | 1 |
| Audio rates | 8 to 48 kHz |
| Echo cancellation | 16 kHz or lower, mono |
| Camera | 1280 x 720, 30 fps; one owner |
| Default H.264 bitrate | 4000 kbps |
| Media sessions | 1 per app, 4 in all; 8 queued commands each |
| Standby cards | 1 per app, 8 kept and shown in the stack; title 31, value 23, detail 63 bytes |
| USB devices listed | 8 |
| Python `t5.net.http_get` / `http_post` body | 512 KB |

## Crash handling

| Limit | Value |
| --- | --- |
| Crashes in a row before auto-launch is blocked | 2 |
| Window for a second unprovable fault to reboot | 30 s |
