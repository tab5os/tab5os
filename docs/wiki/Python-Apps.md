# Python apps

MicroPython 1.29, for apps and for on-device coding. [Python notes](../python.md) covers networking libraries and the containment in depth, and the [API reference](../api-reference.md#python-the-t5-module) lists every name.

## Shape of an app

```python
import t5

screen = t5.ui.screen("Air Log")
value = screen.label("--", role="h4")

while t5.app.running():              # false once back is pressed or the app closes
    value.set_text(str(read()))
    t5.sleep_ms(1000)                # tap callbacks run during this
```

The VM runs on its own task. A script's callbacks run between bytecodes, while it sleeps in `t5.sleep_ms` or `time.sleep`, or after the script's last line. A script that built a UI stays alive until back is pressed, even after its last line.

Until `t5.ui.screen()` is called, the app shows a console with everything it prints. An uncaught exception leaves the app on that console with the traceback. `print` also goes to the device log as `py <app id>: ...`, so `t5 logs` shows it over Wi-Fi.

Set the manifest's `min_heap_kb` if the default 2 MB GC heap doesn't suit. The range is 64 KB to 8 MB.

## UI

`t5.ui.screen(title=None, scroll=True)` returns a container. Each container can make:

| Method | Arguments |
| --- | --- |
| `label(text="", role="body1", color=, fn=)` | `role`: h4, h5, h6, subtitle1, body1, body2, button, caption, overline |
| `button(text, variant="contained", fn=None)` | |
| `switch(on=False, fn=None)` | |
| `slider(min=0, max=100, value=0, fn=None)` | |
| `checkbox(text, checked=False, fn=None)` | |
| `textfield(label="", text="", password=, fn=)` | |
| `item(text, secondary=None, icon=, fn=)` | A list row |
| `section(text)`, `divider()` | |
| `icon(name, size=24, color=, fn=)` | |
| `row()`, `column()`, `card()` | Containers |
| `chart(points=60, min=0, max=100, height=140)` | `.push(v)`, `.set([...])` |
| `progress()` | |
| `clear()` | Removes children |

Widgets have `set_text`, `text`, `set_secondary`, `value`, `set_value`, `on_click`, `on_change`, `set_hidden`, `enable`, `delete`, `size`, `grow`, `color`, `bg`, `focus` and `alive`. `fn=` receives the widget.

Module level: `t5.ui.snackbar`, `t5.ui.dialog(title, text="", ok="OK", cancel=None, fn=None)`, `t5.ui.dp`, `t5.ui.size`, `t5.ui.icon`, `t5.ui.toast`, `t5.ui.fullscreen`, `t5.ui.is_fullscreen`, `t5.ui.low_power`, `t5.ui.rotation`.

Only this MD2 subset exists: raw LVGL isn't reachable from Python.

## Hardware

```python
grove = t5.port.open("grove", mode="i2c", hz=400_000)
data = grove.read_reg(0x44, 0x00, 2)
grove.write_reg(0x44, 0x01, b"\x01")
raw = grove.xfer(0x44, b"\xFD", 6)      # write, then read
print(grove.scan())

rail = t5.port.rail("ext5v")            # power for modules
uart = t5.port.open("grove", mode="uart", baud=9600, rxbuf=4096)
```

`mode` is `i2c`, `uart`, `spi`, `gpio`, `pwm`, `adc` or `rs485`. Other options: `spi_mode`, `cs`, `pin`, `output`, `pull_up`, `pull_down`, `open_drain`, `risky`. Ports work with `with`.

| Module | Examples |
| --- | --- |
| `t5.app` | `running()`, `id()`, `data_dir()`, `state_save(bytes)`, `state_load()`, `restored()`, `on_suspend(fn)`, `on_resume(fn)`, `on_rotate(fn)`, `schedule_alarm(utc)`, `exit()`, `has_permission`, `request_permission` |
| `t5.power` | `battery()`, `lock("net")`, `state()`, `wake_reason()`, `battery_percent()` |
| `t5.time` | `local()`, `now()`, `valid()`, `ticks_ms()`, `sleep_ms()` |
| `t5.audio` | `out`, `tone`, `volume`, `set_volume`, `muted`, `headphones`, `play` |
| `t5.imu` | `accel()`, `status()` |
| `t5.net` | [Python notes](../python.md#networking) |

Errors raise `OSError`: `EACCES` for a missing permission, `EBUSY`, `ENODEV` for a NACK.

Also available: `sys`, `gc`, `io`, `os` (files only), `time`, `math`, `random`, `struct`, `array`, `collections`, `json`, `re`, `binascii`, `hashlib` (SHA-256), `heapq`, `errno`, `asyncio`, `socket`, `ssl`, `select`, `requests`, `umqtt`.

Not available, on purpose: `machine`, `uctypes`, threads, native and viper emitters, `.mpy` loading.

## Files

`open()` and `os` follow the same [file rules](Storage.md#which-files-an-app-can-use) as native apps: the data folder (read-write, up to 1 MB, and the working directory) and install folder (read-only) always, and the shared folders, `/sd` and `/usb` with `shared_read`, `shared_write` or `all_files`. Paths are normalised and any `..` is refused. For native code only, the rules guard against mistakes. For Python they're a real boundary.

## Performance

Python is much slower in tight loops. Bytecode only: no native or viper emitter.

- Sleep in every loop.
- Do arithmetic-heavy work in small batches, or move it to C.
- Avoid allocating in loops: `gc` pauses grow with heap use. Reuse `bytearray` buffers with `readinto`.
- Update widgets only when the value has changed.
- Keep the GC heap as small as your app allows. The whole of it is allocated up front.

## Background wakes

```python
def on_alarm():
    log_reading()
    t5.app.schedule_alarm(t5.time.now() + 3600)

if __name__ == "__main__":             # so importing for on_alarm doesn't build the UI
    main()
```

See [scheduled wakes](Power-and-Background.md#scheduled-wakes).

## One VM

There is one VM, shared by whichever of a Python app, the Editor's Run and the Terminal is in front. Opening a Python app while the Terminal runs says so. Leaving an app stops its VM with an uncatchable exception. A VM that doesn't stop within 1.5 s is killed. A tight loop that doesn't call `t5` runs until that happens.

## Trying things

```sh
t5 py -c "import t5; print(t5.power.battery())"
t5 py script.py
```

Or use the Terminal app, which has a REPL, and the Editor app. Scripts run through `t5 py` are headless.

## Porting from C

The table at the end of [Python notes](../python.md#porting-between-c-and-python) maps calls between the two.
