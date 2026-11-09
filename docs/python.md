# Python on Tab5 OS

Nov 7, 2026 · Phase 8 · [SDK guide](sdk.md) · [API reference](api-reference.md)

Tab5 OS runs MicroPython 1.29 (upstream, pinned as `third_party/micropython`) for apps, for the Editor and for the Terminal's REPL. A Python app is a folder with a `manifest.toml` and a `main.py`; nothing is compiled.

```sh
t5 new "Air Log" --python      # sdk/templates/python
cd air_log && t5 run           # pack, push over Wi-Fi (or USB-C), launch, stream the log
```

On the device, open **Editor** to write and run scripts in `/data/user/scripts` (or `/sd/scripts`), and **Terminal** for a REPL. A USB keyboard on the USB-A port works in both: in the Editor, arrows, Home/End, Tab and Shift-Tab indent, and Enter keeps the indent (one level more after a `:`); in the Terminal, Up and Down walk the history.

## An app

```python
import t5

screen = t5.ui.screen("Air Log")            # app bar with a back arrow
temp = screen.label("--", role="h4")
chart = screen.chart(points=60, min=15, max=35)

grove = t5.port.open("grove", mode="i2c", hz=400_000)
while t5.app.running():                     # false once back is pressed or the app closes
    raw = grove.read_reg(0x44, 0x00, 2)
    c = raw[0] * 175 / 255 - 45
    temp.set_text("%.1f °C" % c)
    chart.push(c)
    t5.sleep_ms(1000)                       # also runs tap callbacks
```

```toml
id = "com.you.airlog"
name = "Air Log"
version = "1.0.0"
api = 1
runtime = "micropython"
entry = "main.py"
min_heap_kb = 1024          # the GC heap, from PSRAM (default 2 MB, at most 8 MB)

[permissions]
ports = ["grove"]
net = true
```

Until the script calls `t5.ui.screen()`, the app shows its own name, a back arrow and a console with everything it prints. An uncaught exception leaves the app open on that console with the traceback. `print` output also goes to the device log as `py <app id>: ...`, which `t5 logs` and `t5 run` show.

Callbacks (`fn=` on widgets, `t5.app.on_suspend`, dialog answers) run on the Python task between bytecodes, while the script sleeps in `t5.sleep_ms` or `time.sleep`, or after the script ends. A script that built a UI keeps running after its last line until back is pressed; one that didn't finishes and stays on its console.

More modules: `sys`, `gc`, `io`, `os` (files only, see below), `time`, `math`, `random`, `struct`, `array`, `collections`, `json`, `re`, `binascii`, `hashlib` (sha256), `heapq`, `errno`, `platform`, `micropython`, `deflate`, and for networking (below) `socket`, `ssl`, `select`, `asyncio`, `network`, `requests`/`urequests`, `umqtt.simple`/`umqtt.robust` and `aiohttp`. The full `t5` list is in the [API reference](api-reference.md#python-the-t5-module); `help()` on the device prints a summary.

## Containment

Native apps run with full access to the device; Python apps can't, by construction (design, "User apps"):

- The firmware is built without `machine`, `uctypes`, MicroPython's lwIP sockets, threads, the native and viper emitters, the inline assembler and `.mpy` loading, so there is no way to name an address or run machine code. The switches are set explicitly in `firmware/components/t5_py/mpconfigport.h`, and `tools/py/check_modules.py` fails the build if MicroPython registers any module that isn't in `modules_allowed.txt`.
- Everything else goes through `t5`, which checks the same manifest permissions as the native ABI (`perm=` in `sdk/abi/api_v1.sym`) and opens ports through the same broker: internal I2C addresses are refused, ports must be listed in the manifest, and everything an app opens is closed when it exits.
- `open()` and `os` follow the same file rules as native apps (`t5_app/fspolicy.h`): the app's data folder (read-write, also the working directory), its install folder (read-only), the shared folders with `shared_read` or `shared_write`, and the rest of `/sd` and `/usb` with `all_files`. Paths are normalised, and `..` or a name FAT would read as another (an 8.3 alias, a trailing dot) is refused.
- `py test` on the USB-C console runs the containment checks: every module above fails to import, the emitter decorators are syntax errors, files outside the roots and `..` paths are refused, internal I2C addresses and internal rails are refused.

Not contained: CPU time (a tight loop with no `t5` calls still runs until the app closes) and memory beyond the app's GC heap, which is capped. An error in a Python app raises an exception in Python; it isn't meant to be able to crash the device.

## Networking

`t5.net` needs `net = true` in the manifest. It's the same stack native apps use (`t5/net.h`): every object it returns is closed when the app exits.

```python
import t5

r = t5.net.get("https://api.example.com/v1/items", headers={"Authorization": "Bearer xyz"})
r.status, r.ok, r.headers["content-type"], r.url      # headers: lower-case names
items = r.json()                                      # also r.text(), r.content (bytes)
r = t5.net.post(url, json={"temp": 21.5})             # or data=b"..." / "..."; put, patch, delete, head
r = t5.net.request("PROPFIND", url, headers=[("Depth", "1")])
r = t5.net.get(url, stream=True)                      # read the body as it comes
while chunk := r.read(4096):
    save(chunk)

ws = t5.net.websocket("wss://stream.example.com/feed", protocols=["v1"])
ws.send("hello")                                      # str goes as text, bytes as binary
msg = ws.recv(5000)                                   # str or bytes; None on timeout
if ws.closed:
    print(ws.close_code, ws.close_reason)
ws.close()

s = t5.net.tcp("example.com", 443, tls=True)          # verify=False, ca=PEM, server_hostname=...
s.send(b"..."); data = s.recv(4096, 2000)             # b"" when the peer closed, None on timeout
srv = t5.net.listen(8080)
client, (ip, port) = srv.accept()
u = t5.net.udp(5000, broadcast=True)
u.sendto(b"hi", ("255.255.255.255", 5000))
data, addr = u.recvfrom(1500, 1000)                   # or None on timeout
u.join("239.1.2.3")                                   # multicast

for obj in t5.net.poll([ws, u, srv], 1000):           # the ones ready to read
    ...
t5.net.resolve("example.com")                         # "93.184.215.14"
```

Timeouts are milliseconds. Receives wait until something arrives unless they're given a timeout (or `settimeout()` on the object). Connects and requests default to 10 s. While a call waits, tap callbacks still run, and Ctrl-C and Stop still get through. Errors raise `OSError` (`ETIMEDOUT`, `EIO`, `ENOENT` for an unknown host, `EBUSY` past the 12 open handles an app may hold). `t5.net.http_get(url)` and `t5.net.http_post(url, body, content_type)` still return `(status, body)`.

### Standard libraries

Ordinary MicroPython and CPython-style networking code runs unchanged. All of it sits on the same tracked `t5.net` handles, so it needs `net = true`, it counts against the 12 open handles, and it's closed when the app exits.

| Module | What it is |
| --- | --- |
| `socket` | The standard API (`socket()`, `connect`, `bind`, `listen`, `accept`, `send`/`recv`, `sendto`/`recvfrom`, `setsockopt`, `getaddrinfo`, `settimeout`/`setblocking`, and `read`/`readline`/`write` as a stream). TCP and UDP; no raw sockets. Timeouts are in seconds here, as in CPython. `connect()` finishes before returning even on a non-blocking socket |
| `ssl` | `SSLContext`, `wrap_socket` (client and server), `load_verify_locations`, `load_cert_chain`, ALPN. Servers are checked against the built-in CA bundle unless you pass `cert_reqs=ssl.CERT_NONE` (or set `verify_mode`). Certificates may be PEM, DER or file paths |
| `select` | `poll()` and `select()` over sockets (and any stream) |
| `asyncio` | MicroPython's asyncio, with `open_connection` and `start_server` |
| `requests`, `urequests` | `get`/`post`/...: `params=`, `json=`, `data=`, `headers=`, `auth=`, `timeout=` (seconds), `stream=` with `iter_content`, `raise_for_status` |
| `aiohttp` | micropython-lib's asyncio HTTP and WebSocket client |
| `umqtt.simple`, `umqtt.robust` | micropython-lib's MQTT clients |
| `network` | `WLAN(STA_IF)`: `isconnected()`, `ifconfig()`, `status()`, `config("ssid")`. Wi-Fi is joined in Settings: `connect()` does nothing when already connected and raises otherwise |
| `deflate` | gzip and zlib streams |

`t5.net` adds the rest:

```python
m = t5.net.mqtt("mqtts://broker.example.com", user="me", password="pw", will=("t5/status", "gone", 1, True))
m.subscribe("home/+/temp", qos=1)
m.publish("t5/status", "up", qos=1, retain=True)
e = m.recv(5000)                     # None on timeout; e.type, e.topic, e.payload, e.qos, e.retain, e.msg_id
if e and e.type == "message": ...

srv = t5.net.serve(8080)             # cert=..., key=... for HTTPS
while True:
    req = srv.next()                 # req.method, .path, .query, .headers, .body, .peer
    if req.path == "/ws":
        ws = req.websocket()         # the same WebSocket object as a client's
    elif req.path == "/api":
        req.json({"ok": True})
    else:
        req.respond(404, "not here")  # or req.begin(); req.write(...); req.end()
```

Open connections keep Wi-Fi up: while a socket, request, WebSocket, MQTT client or server is open, the OS holds a net wake lock for it, so a download or a long-lived connection carries on when the screen goes off and the Tab5 enters Sleep. With nothing open, Sleep (2 minutes idle) powers the radio down; `lock = t5.power.lock("net")` keeps it connected anyway, in power save.

The library code in `umqtt/` and `aiohttp/` is micropython-lib's, unmodified (MIT, `firmware/components/t5_py/frozen/LICENSE-micropython-lib.txt`). It is frozen into the firmware as source with MicroPython's own `asyncio` package, so `.mpy` loading stays off. A copy in your app's folder or `lib/` takes precedence.

## One VM at a time

There is one MicroPython VM, used by whichever of a Python app, the Editor's Run and the Terminal is in front. Opening a Python app while the Terminal is open says so; close the Terminal first. Leaving an app stops its VM: code still running gets an exception it can't catch, and a VM that doesn't stop within 1.5 s is killed.

## Background wakes

With `background_wake = true` and a `[background]` table, `t5.app.schedule_alarm(utc)` wakes the device from soft off later. The OS then imports the entry module and calls its `on_alarm()` (or the `[background] entry` name) with no UI, for at most `max_seconds`. Put the UI under `if __name__ == "__main__":` so the import doesn't build it.

## Porting between C and Python

| C (native) | Python |
| --- | --- |
| `t5_port_open("grove", T5_MODE_I2C, &(t5_port_i2c_cfg_t){ .hz = 400000 }, &p)` | `p = t5.port.open("grove", mode="i2c", hz=400_000)` |
| `t5_port_i2c_read_reg(p, 0x44, 0x00, buf, 2)` | `buf = p.read_reg(0x44, 0x00, 2)` |
| `t5_port_close(p)` | `p.close()`, or `with t5.port.open(...) as p:` |
| `t5_err_t` return codes | `OSError` (`e.errno`: `EACCES` for a missing permission, `EBUSY`, `ENODEV` for a NACK...) |
| `md_label_create(parent, "x", MD_TYPE_HEADLINE5)` | `parent.label("x", role="h5")` |
| `lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud)` | `parent.button("Go", fn=cb)`, `w.on_click(cb)` |
| `lv_timer_create` | a loop with `t5.sleep_ms`, or work in callbacks |
| `t5_app_state_save(app, &s, sizeof s)` | `t5.app.state_save(bytes)`, `t5.app.state_load()` |
| `on_suspend` / `on_resume` / `on_rotate` callbacks | `t5.app.on_suspend(fn)` and so on |
| Full LVGL 9.6 | The MD2 widget set in `t5.ui` (labels, buttons, switches, sliders, checkboxes, text fields, list items, cards, rows, columns, charts, progress bars, dialogs, snackbars) |
| Full speed | Much slower for tight loops (bytecode, no native emitter); I/O-bound code is fine |

Choose C for drivers, games, the camera and anything time-critical; Python for sensor scripts, dashboards and anything you want to change on the device itself.
