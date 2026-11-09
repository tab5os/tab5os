# Tab5 OS app SDK (API level 1)

Nov 5, 2026, updated Nov 7 for Phase 8 · [implementation plan](tab5os-implementation-plan.md)

Native apps are position-independent RISC-V ELFs that the OS loads into PSRAM from the SD card or `/data`. They link against nothing; every function they call is bound at load time by name against the published symbol list, [`sdk/abi/api_v1.sym`](../sdk/abi/api_v1.sym).

Python apps: [python.md](python.md). Every function, with its permission: [api-reference.md](api-reference.md). Guides, recipes and an agent playbook: the [app developer wiki](wiki/Home.md).

## Build an app

```sh
. tools/env.sh                       # ESP-IDF v6.1: the compiler
t5 build                             # the firmware build supplies headers and LVGL config
t5 new "Hello"                       # sdk/templates/c (add --python for a Python app, see python.md)
cd hello && t5 run                   # build, push, launch, stream the log
```

`t5 run` uses Wi-Fi when a paired Tab5 in developer mode is reachable, and the USB-C console otherwise (or with `--usb`). On a Tab5 with a fresh firmware build, `t5 new` to the app on screen took 2.7 s over Wi-Fi (C) and the USB path moves about 20 KB/s.

To install by hand instead: copy the `.t5app` (from `t5 app-build`, in `build/`) to the SD card and tap it in Files, or run `app install /sd/hello.t5app` (or `app fetch <url>`) on the console. `t5 apps` builds the test apps in `test/apps/`, `t5 examples` the examples in `apps/examples/`.

An app compiles with the firmware's own flags for its ABI table (the generated `t5_abi/abi_table.c`) and against the same header files, so it sees exactly the headers and LVGL configuration the OS was built with. `sdk/cmake/sysroot.py` copies the headers the public `t5/*.h` headers reach (the OS's, ESP-IDF's libc, FreeRTOS and lwIP ports, LVGL, the generated `sdkconfig.h`: about 370 files, with their licences) into one tree, keeping each file's relative path so every include resolves as it did in the firmware, and writes the flags with paths relative to that tree. In the source tree `t5_app.cmake` stages it into the app's build directory from `firmware/build`. The SDK release ships it as `sdk/sysroot/`.

### From the SDK release, on any machine

The release's `tab5os-sdk-<version>.tar.gz` needs no firmware sources or build: only the ESP-IDF v6.1 RISC-V toolchain (`riscv32-esp-elf`, GCC 15.2.0; a full ESP-IDF install has it, or install the toolchain alone), CMake 3.22+, Ninja and Python 3.11+.

```sh
tar xzf tab5os-sdk-0.1.0.tar.gz && cd tab5os-sdk-0.1.0
. tools/env.sh                       # with ESP-IDF v6.1 installed; or put riscv32-esp-elf-gcc on PATH yourself
tools/t5/t5 new "Hello"              # an app from sdk/templates/c
tools/t5/t5 app-build hello          # hello/build/com.example.hello.t5app
tools/t5/t5 run hello                # build, push to a paired Tab5, launch, stream the log
cmake -S apps/examples -B ex -G Ninja && cmake --build ex    # the example apps, from source
```

The build stops if the compiler isn't GCC 15.2.0, since the flags use Espressif's RISC-V extensions and picolibc. `t5 sdk` packages the SDK and proves it: `test/sdk/check_tarball.sh` extracts it outside the repo and, with only the compiler, CMake, Ninja and Python on `PATH`, builds the C template, the Python template and every example (CI runs it). The apps it builds are byte-identical to the ones built in the source tree. The tools' device constants (permission names, limits, the API level) come from `sdk/device.json`, which the release writes from the firmware sources (`tools/t5/t5const.py snapshot`). The packaging refuses to ship a file that names the machine it ran on.

## Developer mode and the t5 CLI

Developer mode (Settings > Developer mode) is off by default; while it's off nothing listens on the network. When it's on, the Tab5 serves a small HTTP API on port 8350 to computers you pair:

```sh
t5 pair 192.168.1.50                 # the Tab5 shows a six-digit code; type it here
t5 devices                           # paired Tab5s (tokens live in ~/.config/t5/devices.json)
t5 run [DIR] | t5 push FILE.t5app | t5 launch ID | t5 stop | t5 rm ID | t5 ls
t5 logs [--grep TEXT]                # the device log, live
t5 py script.py | t5 py -c "import t5; print(t5.power.battery())"
t5 screenshot out.png
```

Pairing gives the computer a random 256-bit token; the Tab5 keeps only its SHA-256 and refuses any other request without it. Three wrong codes or two minutes end a pairing. Settings lists the paired computers and forgets them. The endpoints are documented in `firmware/components/t5_dev/include/t5_dev/dev.h`.

### Signed packages

`t5 keygen me.pem` makes an ECDSA P-256 key; `t5 pack --sign me.pem` (or `t5 run --sign me.pem`) adds `signature.t5sig`, a signature over every other file's SHA-256. `python3 tools/t5/t5pack.py --inspect app.t5app` checks it. The device doesn't verify package signatures yet: every app, signed or not, installs with the usual warning (design, "App trust").

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
| `t5/ui.h` | LVGL 9.6, the MD2 layer (`md_*`: theme, type, elevation, ripple, app bar, buttons, FAB, card, recycled list, dialog; and from Phase 6 text field, switch, slider, checkbox, chips, tabs, menu, static rows and pages, snackbar, navigation drawer), `T5_DP()`, `t5_ui_lock`/`t5_ui_async` |
| `t5/shell.h` | The status and gesture bars, full screen and the insets they take; snackbars from any task, quick settings, home, the on-screen keyboard |
| `t5/port.h` | Named ports (`grove`, `mbus.*`, `ext`, `rs485`, `stamp`), Modbus RTU, rails |
| `t5/power.h` | Wake locks, power state, battery (read-only) |
| `t5/sys.h` | Time (and local time with the zone from Settings), IMU, network status, FreeRTOS |
| `t5/audio.h` | Up to four mixed output streams (any rate 8-48 kHz, mono or stereo) with ducking, sound files (a WAV decoder and a player), system sounds, volume and route events; microphone input (`mic`) with optional echo cancellation; resampling and level helpers |
| `t5/camera.h` | The built-in camera (`camera`): preview in an LVGL object, JPEG photos, H.264 video; hardware JPEG decoding (`t5_jpeg_decode`) |
| `t5/usb.h` | USB-A devices: list, CDC serial, MIDI, UVC cameras (`usb`); a USB drive is mounted at `/usb` |
| `t5/storage.h` | SD and `/data` state, atomic writes, safe eject (`storage`) |
| `t5/system.h` | App list, launching, app info, per-app power, charge history (`system`) |
| `t5/ble.h` | Bluetooth LE (`ble`): scan, connect, a GATT client with notifications, advertising and a GATT server ([guide](wiki/Bluetooth.md)) |
| `t5/net.h` | Networking (`net`): DNS, TCP and TLS clients, TCP servers, UDP with broadcast and multicast, poll, an HTTP/1.1 client and a WebSocket client |

Plus the C library functions listed in `api_v1.sym` (`malloc`, `snprintf`, `fopen`, `sinf`…) and BSD sockets (`<sys/socket.h>`, `select`, `fcntl`, `getaddrinfo`...: lwIP) for apps with the `net` permission. Sockets an app leaves open are closed when it exits, like everything else. `errno` works as usual: apps build with `-D__PICOLIBC_ERRNO_FUNCTION=t5_errno`, because picolibc keeps it thread-local and the app loader can't relocate that. lwIP reports a refused connect as `ECONNRESET`.

## Networking

Everything in `t5/net.h` needs the `net` permission. Every handle (socket, HTTP response, WebSocket, MQTT client, HTTP server and request) is closed when the app exits or crashes, and an app may hold at most 12 at once. Timeouts are in milliseconds: 0 doesn't wait, `T5_NET_FOREVER` waits until something happens. Calls that move bytes return a count, or a negative `t5_err_t` (`T5_ERR_TIMEOUT`, `T5_ERR_IO`, `T5_ERR_STATE` once closed). A receive that returns 0 means the peer closed. Closing a handle from another task ends a wait on it within about 50 ms.

```c
/* HTTP: any method, headers, a body; redirects followed; the body streamed. */
t5_http_header_t hdr[] = { { "Authorization", "Bearer xyz" }, { "Content-Type", "application/json" } };
t5_http_req_t req = { .method = "POST", .url = "https://api.example.com/v1/log",
                      .headers = hdr, .n_headers = 2, .body = json, .body_len = strlen(json) };
t5_http_t *h;
if (t5_http_open(&req, &h) == T5_OK) {
    printf("%d %s\n", t5_http_status(h), t5_http_header(h, "Content-Type"));
    char buf[512];
    for (int n; (n = t5_http_read(h, buf, sizeof buf, 10000)) > 0;) consume(buf, n);
    T5_IGNORE(t5_http_close(h));
}
/* Or the whole body into a buffer: */
int status, len = t5_http_fetch(&(t5_http_req_t){ .url = "http://10.0.0.5/state" }, buf, sizeof buf, &status);

/* WebSocket: whole messages; pings are answered for you. */
t5_ws_t *ws;
t5_ws_connect("wss://stream.example.com/feed", &(t5_ws_cfg_t){ .subprotocols = "v1" }, &ws);
t5_ws_send(ws, T5_WS_TEXT, "hello", 5, 5000);
t5_ws_op_t op;
int n = t5_ws_recv(ws, buf, sizeof buf, &op, 5000);   /* T5_WS_TEXT, T5_WS_BINARY, or T5_WS_CLOSE once */
T5_IGNORE(t5_ws_close(ws, 1000));

/* TCP (TLS with T5_TCP_TLS, checked against the built-in CA bundle), servers, UDP: */
t5_sock_t *s, *srv, *u;
t5_tcp_connect("example.com", 443, &(t5_tcp_cfg_t){ .flags = T5_TCP_TLS }, &s);
t5_tcp_listen(8080, 4, &srv);                        /* t5_tcp_accept(srv, timeout, &client) */
t5_udp_open(5000, T5_UDP_BROADCAST, &u);             /* t5_udp_sendto / t5_udp_recvfrom, t5_udp_join("239.1.2.3") */

/* Wait on several at once: */
t5_poll_t p[] = { { .handle = ws, .events = T5_POLL_IN }, { .handle = u, .events = T5_POLL_IN } };
if (t5_net_poll(p, 2, 1000) > 0 && (p[1].revents & T5_POLL_IN)) { /* a datagram is waiting */ }
```

```c
/* TLS on a connected socket: STARTTLS, or the server side of an accepted one. */
t5_sock_start_tls(s, &(t5_tls_cfg_t){ .sni = "mail.example.com" });
t5_sock_start_tls(client, &(t5_tls_cfg_t){ .flags = T5_TLS_SERVER, .cert_pem = cert, .key_pem = key });
t5_sock_set_option(s, T5_SOCKOPT_KEEPALIVE, 30);    /* NODELAY, BROADCAST, REUSEADDR, RCVBUF, TTL, TOS */

/* MQTT 3.1.1/5 (Espressif's esp-mqtt): mqtt://, mqtts://, ws://, wss://, QoS 0-2, last will, auto-reconnect.
 * Events are taken on your task; no callbacks run behind your back. */
t5_mqtt_t *m;
t5_mqtt_connect(&(t5_mqtt_cfg_t){ .uri = "mqtts://broker.example.com", .username = "me", .password = "pw" }, &m);
t5_mqtt_subscribe(m, "home/+/temp", 1);
t5_mqtt_publish(m, "home/tab5/online", "1", 1, 1, true);
t5_mqtt_event_t ev;
if (t5_mqtt_next(m, &ev, buf, sizeof buf, 5000) == T5_OK && ev.type == T5_MQTT_EV_MESSAGE) use(ev.topic, ev.data, ev.len);

/* HTTP(S) server: complete requests one at a time; respond, stream (chunked), or upgrade to a WebSocket. */
t5_httpd_t *srv;
t5_httpd_start(8080, NULL, &srv);                    /* .cert_pem/.key_pem for HTTPS */
t5_httpd_req_t *req;
if (t5_httpd_next(srv, 1000, &req) == T5_OK) {
    if (!strcmp(t5_httpd_path(req), "/ws")) t5_httpd_websocket(req, NULL, 0, &ws);
    else t5_httpd_respond(req, 200, "application/json", "{}", 2, NULL, 0);
}
```

The CA bundle is Mozilla's full list, with cross-signed chains supported. `ca_pem` (in each config) trusts one PEM CA instead of the bundle, for a server with a private certificate. `T5_TCP_TLS_NO_VERIFY` / `T5_HTTP_TLS_NO_VERIFY` / `T5_MQTT_TLS_NO_VERIFY` skip the certificate check: use them for testing only. `cert_pem`/`key_pem` give a client certificate for mutual TLS. While any handle is open the OS holds a `T5_LOCK_NET` wake lock ("net.io"), so Sleep keeps Wi-Fi connected and transfers continue with the screen off; with none open, Sleep powers the radio down unless the app holds its own `T5_LOCK_NET` lock (`t5/power.h`). Plain BSD sockets don't count: take the lock yourself if you use them across Sleep. The `net handles` console command lists every open handle and its owner. `test/hil/net_stack.py` and `test/hil/net_compat.py` check the whole stack against servers on your computer (`--external` adds public ones), and `test/apps/netleak` checks that an app's handles are freed when it exits.

## Bluetooth LE

`t5/ble.h` needs the `ble` permission and follows the networking conventions above: handles (scanner, link, advertiser, server) close when the app exits, an app holds at most 6, timeouts are in milliseconds, events are pulled with `*_next()` calls. The radio is the ESP32-C6, which is Bluetooth LE only, and Bluetooth works whenever it is on in Settings, with Wi-Fi on or off (`t5_ble_ready()`). Up to 3 links at once in either role. See the [Bluetooth guide](wiki/Bluetooth.md) for the hardware's limits and examples of each role.

```c
t5_ble_conn_t *c;
t5_ble_conn_open(addr, addr_type, 10000, &c);
t5_ble_gatt_discover(c, 10000);
t5_ble_chr_t ch;
t5_ble_gatt_find(c, "0x180f", "0x2a19", &ch);
t5_ble_gatt_subscribe(c, ch.val_handle, true, 5000);
int n = t5_ble_gatt_notification(c, &handle, buf, sizeof buf, 30000);
```

## Session restore and launch arguments

After a soft off the OS reopens the app that was in front (not after an alarm wake, a crash or a hang). Save what the person was doing in `on_suspend` with `t5_app_state_save()`; in `on_create`, `t5_app_restored()` says whether this launch is a restore, and `t5_app_state_load()` gets the state back (up to 8 KB, written atomically to the app's data directory). `t5_app_launch_arg()` is the argument another app or the shell launched it with (`t5_apps_launch_arg()`), for example a file path.

## Media

```c
/* Play 16 kHz mono PCM: any rate and channel count is converted to 48 kHz stereo. */
t5_audio_out_cfg_t cfg = { .rate = 16000, .channels = 1 };
t5_audio_out_t *out;
t5_audio_out_open(&cfg, &out);            /* holds an audio wake lock: plays with the screen off */
t5_audio_out_write(out, pcm, bytes, 100);
t5_audio_out_drain(out, 1000);
t5_audio_out_close(out);
```

Up to `T5_AUDIO_OUT_MAX` (4) output streams can be open at once, from any apps, and they are mixed. Each has a usage: `t5_audio_out_open()` opens a MEDIA stream; `t5_audio_out_open_usage(&cfg, T5_AUDIO_USAGE_ALERT, &out)` (or `SPEECH`) opens one that **ducks** the media streams to a quarter of their level (-12 dB) while it has sound queued, and so does every system sound (clicks, notifications). The level ramps down over 100 ms and back up over 300 ms, so there is no click. `t5_audio_out_set_volume(o, pct)` sets a stream's own level under the system volume.

```c
/* A sound file: the player decodes and plays it on its own task. */
t5_audio_player_t *p;
if (t5_audio_play_file("/sd/Music/chime.wav", T5_AUDIO_USAGE_ALERT, &p) == T5_OK) {
    t5_audio_player_wait(p, 10000);          /* or poll t5_audio_player_state(p, &pos_ms) */
    t5_audio_player_close(p);
}
/* Or decode it yourself: 16-bit PCM at the file's rate, 1 or 2 channels. */
t5_audio_dec_t *d;
t5_audio_dec_open(path, &d);                  /* t5_audio_dec_open_mem() for an asset in memory */
t5_audio_info_t info;
t5_audio_dec_info(d, &info);                  /* rate, channels, duration_ms */
int16_t pcm[480 * 2];
for (int n; (n = t5_audio_dec_read(d, pcm, 480)) > 0;) process(pcm, n);
t5_audio_dec_close(d);
```

The decoder takes WAV: PCM 8, 16, 24 and 32-bit, 32 and 64-bit float, `WAVE_FORMAT_EXTENSIBLE`, any rate, up to 8 channels (it gives the first two). The player needs a rate the output takes, 8 to 48 kHz. MP3 is in the API (`T5_AUDIO_FMT_MP3`) but not in this OS build: `t5_audio_format_supported()` says false and the open calls return `T5_ERR_NOT_SUPPORTED`, because Espressif's decoder (`esp_audio_codec`, about 56 KB) doesn't fit the image's size budget. An app that needs MP3 can compile a decoder (minimp3, for example) into itself and play what it decodes through a stream. Files follow the app's file rules; decoders and players are closed when the app exits, at most 4 per app.

`T5_EV_AUDIO_ROUTE` is posted when headphones are plugged in or out, and `T5_EV_AUDIO_VOLUME` when the system volume or mute changes; both carry `t5_ev_audio_t` (volume, muted, headphones). Bluetooth audio isn't possible: the radio is an ESP32-C6, which has Bluetooth LE only (no Classic Bluetooth, so no A2DP or HFP), and LE Audio (isochronous channels, LC3) isn't available on the C6 with ESP-IDF v6.1's controller and NimBLE host.

Microphone input (`t5_audio_in_open`) and the camera (`t5_camera_open`) need the permission declared in the manifest and asked for at run time: call `t5_app_request_permission(app, T5_PERM_MIC, cb, user)` first and open in the callback. While either is open the status bar shows an indicator. Close the camera in `on_suspend` (the OS closes it for you at sleep, and on exit). The camera and the hardware JPEG decoder run their interrupts on core 0, so call them from any task; the photo and record calls block and must not run on the UI task.

### Media sessions

An app that plays music or other long audio opens a **media session**. That gives the person controls for it outside the app: the music card on the [standby screen](#standby) shows the title, artist and progress, and its previous, play/pause and next buttons send commands to your app. They don't touch your player directly, so your app stays in charge of what plays.

```c
t5_media_session_t *ms;
t5_media_session_open(&ms);                       /* one per app; closed when the app exits */
t5_media_info_t info = { .duration_ms = 215000 };
snprintf(info.title, sizeof info.title, "%s", "Clair de lune");
snprintf(info.artist, sizeof info.artist, "%s", "Claude Debussy");
t5_media_session_set_info(ms, &info);
t5_media_session_set_state(ms, T5_MEDIA_PLAYING, 0);   /* on changes only: the OS moves the position on */

/* On a task of your own: */
t5_media_cmd_t c;
while (t5_app_running(app)) {
    if (t5_media_session_next(ms, &c, 250) != T5_OK) continue;
    switch (c.type) {
    case T5_MEDIA_CMD_TOGGLE: /* play or pause, then set_state */ break;
    case T5_MEDIA_CMD_NEXT:   /* next track: set_info, set_state(PLAYING, 0) */ break;
    default: break;                               /* PLAY, PAUSE, PREVIOUS, STOP, SEEK (c.position_ms) */
    }
}
```

The active session is the one most recently opened or set to `T5_MEDIA_PLAYING`; commands go to it. Up to 4 sessions are open at once, and 8 commands are queued per session. No permission is needed. Audio keeps playing under standby and with the screen off, because an open output stream holds an audio wake lock. `apps/examples/music` is a complete player.

## Rules

- **Threads.** Every callback runs on the UI task, so LVGL calls are safe there. Tasks you create (`t5_task_create`, or `xTaskCreate`) must use `t5_ui_async()` or `t5_ui_lock()`/`t5_ui_unlock()` for LVGL.
- **Cleanup.** The OS tracks your heap, tasks, LVGL timers and animations, files, queues and mutexes, event subscriptions, port claims, rails and wake locks, and frees what's left when the app exits or crashes. Release order: tasks, timers, animations, UI, ports, rails, wake locks, other handles, heap. Loops in your tasks should check `t5_app_running()` so they stop before they are deleted.
- **UI.** Build under `t5_app_screen()`. Objects on `lv_layer_top()` (dialogs) are tracked too. Other screens you create are not.
- **LVGL timers** don't delete themselves when their repeat count runs out: they pause, and the OS deletes them on exit.
- **Sizes** are in dp: `T5_DP(16)` is 32 px. The screen is 640 × 360 dp in landscape.
- **Text input.** Tapping a text area shows the on-screen keyboard; with a USB keyboard attached, keys go to the focused text area instead. The keyboard pans the text area's container up if it would be covered.
- **Bars and full screen.** See [Shell bars](#shell-bars) below. Swiping up from the bottom edge goes home; swiping down from the top edge opens quick settings.
- **Memory from `malloc` is yours.** Don't pass it to an OS function that frees it, and free memory from libc helpers (`strdup`) with `free()` as usual.

## Shell bars

The shell draws two bars around an app's screen, and pads `t5_app_screen()` clear of them, so anything built under it is never covered. There's no title bar: the app's whole screen is its own. An app that wants a title, a back arrow or actions draws them itself, with `md_appbar_create()` or `md_page_create()`.

| Bar | Size | When |
| --- | --- | --- |
| Status bar | 24 dp, top | Always, except in full screen |
| Gesture bar | 16 dp, bottom | Always on app screens, except in full screen: a handle that shows where to swipe up to go home |

```c
static void on_create(t5_app_t *app) {
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *bar = md_appbar_create(scr, "Air Log", NULL, NULL, NULL);   /* only if you want one */
    md_appbar_add_action(bar, MD_ICON_REFRESH, refresh_cb, app);
    t5_shell_insets_t in;
    t5_shell_get_insets(&in);    /* {top 24, bottom 16} dp: already taken off the screen's content area */
}
```

`t5_shell_get_insets()` reports what the bars take, in dp, and `T5_EV_UI_INSETS` is posted when that changes (full screen on or off). Objects you place yourself on `lv_layer_top()` aren't padded: use the insets for them.

**Full screen** (`t5_shell_set_fullscreen(true)`, or `[shell] fullscreen = true` to start that way) hides both bars and the padding goes to 0. The gestures still work. So people can find the way out, the gesture bar's handle shows over the app for 2.5 s when full screen starts, and again whenever a finger touches the bottom 20 dp; a swipe up from there still goes home. `t5_shell_fullscreen()` says whether it's on. Everything resets when the app leaves the foreground. Python apps have `t5.ui.fullscreen()` and `t5.ui.is_fullscreen()`.

## Status bar and activities

Besides the time, Wi-Fi, Bluetooth (on, or connected), USB, a keyboard and the battery, the status bar shows what is going on in the background, so nothing runs unseen:

| Icon | When |
| --- | --- |
| The app's own icon | An app posted an activity (below). Up to three apps' icons (one in portrait), then "+N" |
| Bolt | An app other than the one in front holds a wake lock, or runs its alarm headless |
| Alarm | An app has a background wake scheduled (`t5_app_schedule_alarm`) |
| Download, update | The OS is downloading something or installing an update |
| Arrows | Network handles are open (any app's); left out in portrait |
| Note, headphones, speaker off | Sound is playing (to the speaker or headphones), or the sound is muted |
| Mic, camera (in colour) | The microphone or camera is in use |

Quick settings lists the same activities with the app's name, a line of text and a progress bar if there is one. An app posts one for something people should be able to see while it lasts, such as a sync or a recording:

```c
t5_shell_activity_t *a;
t5_shell_activity_start(MD_ICON_DOWNLOAD, "Syncing photos", &a);      /* any task */
t5_shell_activity_update(a, "Syncing photos: 12 of 40", 30);          /* text (NULL keeps it), percent or -1 */
t5_shell_activity_end(a);                                            /* or the app exits */
```

At most `T5_SHELL_ACTIVITY_MAX` (4) per app; the icon is one `MD_ICON_*` glyph.

## Standby

When the device is on USB power and dims, the shell shows the **standby screen** in front of whatever is open: a large clock and the date, the music card for the active [media session](#media-sessions), and the newest app card. The app under it keeps running. The backlight is capped at 20%, and from 22:00 to 07:00 at 5% with everything in dim red. A tap leaves it, and so does unplugging. It also gives way to a dialog, menu or sheet that opens over it. A toast shows on top without leaving. People can turn it off under Settings > Display > Standby.

An app with `ambient = true` in `[permissions]` can post one card there, for weather, a calendar entry, an alarm and so on:

```c
t5_ambient_card_t card = { .expires_s = 3 * 3600 };       /* 0: until cleared */
snprintf(card.icon, sizeof card.icon, "%s", MD_ICON_CLOUD);   /* one MD_ICON_* glyph */
snprintf(card.title, sizeof card.title, "%s", "Bengaluru");  /* shown as "Bengaluru · <app name>" */
snprintf(card.value, sizeof card.value, "%s", "29°");        /* large */
snprintf(card.detail, sizeof card.detail, "%s", "Overcast, high 29°, low 20°");
t5_ambient_set(&card);       /* any task; replaces this app's card */
t5_ambient_clear();          /* takes it down */
```

The card is copied and kept after your app exits, and across a restart, until you clear it or it expires. It isn't shown once the app is removed. Set it when what it shows changes; there's no need to keep the app running. The OS keeps 8 cards (the least recently set goes when a ninth arrives), and standby shows the newest. Without the permission, `t5_ambient_set` returns `T5_ERR_DENIED`.

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

[shell]                       # optional
fullscreen = false            # start in full screen
```

Permission groups: `camera`, `mic`, `net`, `ble`, `ports`, `usb`, `storage`, `shared_read`, `shared_write`, `all_files`, `background_wake`, `system`, `ambient`. Calling a function whose group the manifest doesn't declare gets a stub that returns `T5_ERR_DENIED` (or `NULL`, `false`, `0`). Camera and mic also ask the person on first use (`t5_app_request_permission()`). **These are consent and correctness checks, not a security boundary:** a native app shares the OS address space and can reach the hardware directly. The installer says so.

## Stability

- An app built for API level N runs on any OS with level ≥ N.
- `api_v1.sym` stores each symbol's full prototype. The firmware build redeclares all of them, so a header that drifts from the published ABI fails the build. Lock files record every symbol, struct layout and constant apps compile in, and CI fails if one changes: see [ABI stability](abi-stability.md).
- LVGL is pinned to 9.6.0 for level 1 (all 1,903 public LVGL functions in the OS build are exported, plus `lv_image_cache_drop` for apps that redraw image buffers in place).
- Phases 6 and 7 added 114 symbols to level 1 (the MD2 widgets, shell, audio, camera, USB, storage and system calls) without changing any published one.
- At install the OS reads the ELF's imports and refuses the package if any isn't exported, naming the missing ones (`app abi <file.elf>` on the console does the same check).

## Files

Every app is isolated. It can read and write its own data folder (`t5_app_data_dir`, up to 1 MB) and read its install folder (`t5_app_install_dir`). Everything else is refused with an ordinary file error (`fopen` returns NULL, `errno` is `EACCES`), so check for it. Going past the quota gives `ENOSPC`. Paths must be absolute; `..` is refused.

| To reach | Do this |
| --- | --- |
| One file the person chooses | `t5_file_pick()`. No permission needed. Open until the app exits; add `T5_FILE_PICK_WRITE` to change it too |
| Shared folders: `/data/user` and `DCIM`, `Photos`, `Pictures`, `Music`, `Videos`, `Documents`, `Downloads` on the SD card | `shared_read = true` or `shared_write = true` in `[permissions]`; asked on first use |
| All of the SD card and USB drives | `all_files = true`; asked on first use. For file managers |
| OS files, another app's data | Never |

Call `t5_app_request_permission()` before touching a shared folder. Called from the UI task without it, the file call shows the prompt and fails that once. People can change the answer in App Manager. The `storage` permission now only covers ejecting and formatting the SD card.

`t5_file_pick()` shows a large modal over your app, on the UI task, one at a time:

    static void picked(t5_app_t *app, const char *path, void *user)
    {
        if (!path) return;       /* backed out; your app carries on */
        FILE *f = fopen(path, "r");
    }
    t5_file_pick(app, "Choose a photo", "jpg,png", 0, picked, NULL);

`cb` gets `NULL` for Cancel, the close button, Back at the top level, a tap outside the box, or the swipe up from the bottom edge. Those close only the picker. If your app exits or crashes first, the picker closes and `cb` isn't called. The picker hides OS and app folders. Python apps follow the same folder rules but have no picker yet. These rules stop mistakes; they can't stop a native app written to get around them.

## Crashes and hangs

A misbehaving installed app is closed; the OS keeps running (`t5_app/isolate.h`, `t5_core/fault.h`):

- **Crash.** A bad load or store, illegal instruction or jump to nowhere in your code, in an OS function your code called (`strlen(NULL)`, an LVGL call with a bad object), in a draw callback, or anywhere in a task your app created closes the app without a reboot. So does a stack overflow, in your own task or from recursion on the UI task. The app gets no `on_suspend` or `on_destroy`; the OS frees everything it tracks, shows a toast and records the crash. The log line gives the faulting address as `app+0x…`, an offset into your ELF. Apps are stripped, so map it with a linker map file ([how](wiki/Debugging-and-Testing.md#crashes)).
- **Hang.** If your code blocks the UI task for 2 s, the OS draws "App not responding" itself, outside LVGL. Wait gives it 5 s more; Close app stops the UI task where it is and closes the app, also without a reboot (recorded as a hang). An event handler that doesn't return within 3 s is stopped and the app closed.
- **Memory.** Your heap is PSRAM chunks of its own: an overrun damages only your memory, and exit gives all of it back at once. `lv_malloc` and draw buffers you create (`lv_draw_buf_create`, `lv_snapshot_take`...) are yours too and end with the app. `malloc` returns NULL rather than take the last 2 MB of PSRAM. Your tasks, queues, semaphores and mutexes are in PSRAM as well, so an app can't use up internal RAM directly. If PSRAM drops below 1 MB, or internal RAM below 16 KB, for 1.5 s, the app in front is closed; it does get its exit callbacks. Freeing a block twice, or a pointer that isn't a heap address, is logged and ignored.
- **Limits.** 16 tasks per app at priority 2 or lower (a higher request is lowered), 8 raw sockets, and 256 tracked handles (open files, sockets, timers, queues...); over that, creation fails like running out of memory. Queued `t5_ui_async` calls are capped at 512 (`T5_ERR_BUSY`).
- **CPU.** A task of yours that keeps a core busy for 3 s is moved to priority 1 and paused for 100 ms in every second, so OS services keep running. Sleep or block in loops.
- **UI lock.** A task that takes `t5_ui_lock()` and never releases it stalls the UI like a hang: Close app ends that task.
- **Queued calls.** `t5_ui_async`, `t5_ui_sync` and `lv_async_call` calls still waiting when your app exits are dropped.
- **OS files.** Apps can't write, rename, delete or create anything under `/data/sys`, or use `..` in those calls.

A crash still reboots the device when it can't be pinned on the app, or when leaving the code safely isn't possible: inside a critical section or an interrupt handler, while a finished frame is being handed to the panel, or a second fault with no app frame on the stack within 30 s. The crash guard then records which app was running (RTC memory survives the reset), returns to the launcher and saves the core dump to `/data/sys/dumps/<id>.elf`. Either way, an app that crashes or hangs twice in a row is no longer auto-launched (session restore, alarms). Native code shares the OS address space, so a stray write that corrupts OS memory without faulting can't be caught. Built-in apps and MicroPython apps are OS code and aren't covered.

## Tools

| Tool | Does |
| --- | --- |
| `sdk/cmake/t5_app.cmake` | `t5_app(name SRCS … MANIFEST …)`: ELF and `.t5app` |
| `tools/t5/t5pack.py` | Pack or inspect a `.t5app` |
| `tools/abi/abi.py` | Generate the export table, update prototypes, add LVGL exports, check stability |
| `t5 sim` | The SDL simulator with the same MD2 code; `t5 sim --shell` runs the launcher, status bar and quick settings |
| `t5 shot` | A screenshot of the device over the console (`tools/t5/t5shot.py`, `--cam` for the camera preview) |
