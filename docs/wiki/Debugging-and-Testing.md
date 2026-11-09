# Debugging and testing

## Logs

There are two log streams, and which one you see matters.

| Output | USB-C console | `t5 logs`, `t5 run` over Wi-Fi |
| --- | --- | --- |
| C `printf` | Yes | **No** |
| Python `print` | Yes | Yes, as `py <id>: ...` |
| OS log lines (app start, crash lines, warnings) | Yes | Yes |

So for C apps, debug with `t5 run --usb`, or `t5 monitor` in a second terminal. There's no logging API in the C ABI. The ABI has `printf`, and `esp_log_*` isn't exported.

To see a C app's state over Wi-Fi, show it in the UI.

`dmesg` on the console prints the OS log since boot, from a 16 KB ring.

## The console

`t5 monitor` is interactive. `t5 console "cmd" "cmd"` runs commands from a script and prints the replies. Press `Ctrl-]` to quit the monitor.

| Command | Use |
| --- | --- |
| `app` | Installed apps, foreground |
| `app run <id> [arg]`, `app exit` | Launch and close |
| `app usage` | Heap, handles, tasks, denied imports of the foreground app |
| `app crashes` | Crash and hang records |
| `app abi <file.elf>` | Check imports against the OS |
| `app stress <id> <n>` | Open and close n times; verifies memory and claims return to baseline |
| `app alarm <id>` | Run `on_alarm` now |
| `app dump <id>` | The saved core dump, as base64 |
| `sys` | Memory, reset reason |
| `tasks`, `stacks` | CPU and stack use |
| `net`, `net handles` | Wi-Fi and open handles |
| `port`, `rail` | Pins and rails, with owners |
| `power`, `power locks` | State, wake locks |
| `audio`, `cam`, `usb` | Media state |
| `fs ls /data/appdata/<id>`, `fs cat <path>` | Look at your files |

## UI automation

These are for tests and agents. Coordinates are panel-logical pixels: multiply screenshot coordinates by 2.

```text
ui tap 640 360
ui swipe 640 600 640 100 300
ui type hello
ui key enter
```

```sh
t5 screenshot out.png                  # Wi-Fi, developer mode, 640 x 360
t5 shot out.png                        # USB console; retry if the dump is incomplete
```

A blank-looking tap can launch something on the home screen. To wake the device, use `power wake`. The UI pauses when the screen goes off after 2 minutes idle, so longer scripted tests should run `power lock screen` first, and `power unlock screen` afterwards. `ui rot` and `ui lock` persist, so restore them after testing.

## Crashes

A crash writes a line like:

```text
com.you.app crash: pc app+0x730 ra app+0x716 addr 0x00000000 cause 7 (3 survived since boot)
```

`app+0x730` is an offset in your ELF's load image. `os` means the fault was inside the OS, usually because your code passed it a bad pointer; look at `ra`.

Apps are built stripped and without debug info, so `addr2line` on the ELF has nothing to work with. Ask the linker for a map:

```cmake
target_link_options(myapp.elf PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/myapp.map)
```

The map lists each function's address, because of `-ffunction-sections`. Find the last entry at or below your offset:

```text
 .text.on_create  0x000005cc   0x88  CMakeFiles/myapp.elf.dir/main.c.obj
 .text.tapped     0x00000654   0x1e  CMakeFiles/myapp.elf.dir/main.c.obj
```

An offset of `0x660` is in `tapped`, 12 bytes in. For the exact instruction, `riscv32-esp-elf-objdump -d build/myapp.elf`.

Common `cause` values: 2 is an illegal instruction, 5 a load fault, and 7 a store fault. `addr` is the bad address: near zero means a `NULL` pointer plus an offset.

A task that ran out of stack logs `ran out of stack`. Raise its size. For the UI task, reduce local arrays or recursion.

If the device rebooted rather than closing the app, `app crashes` still names it, and `/data/sys/dumps/<id>.elf` holds the core dump.

## Reproducing misbehaviour

`test/apps/crasher` demonstrates the fault paths, with the mode as launch argument: `app run com.t5.test.crasher task`. `test/apps/hang` and `test/apps/leaky` do what their names say.

`l2eat <KB>` holds internal RAM for low-memory testing, and `l2eat 0` frees it.

## Performance

See [measuring](Performance.md#measuring).

## Stress test

```text
app stress com.you.app 50
```

It opens and closes your app n times, and reports any cycle whose heap, claims, locks or rails didn't return to baseline. Worth running at every significant change. It holds a screen lock so the UI stays up.

## Manual test matrix

| Scenario | How |
| --- | --- |
| No Wi-Fi | Turn it off in quick settings |
| Wi-Fi drops mid-transfer | `net reset` |
| No SD | Pull it, or `fs eject` |
| SD pulled while writing | Pull it |
| No sensor | Unplug the module |
| Permission denied | Answer no, or change it in App Manager |
| Screen off and sleep | Wait, or `power screenoff` and `power sleep`; then `power wake` |
| Rotation | Turn the device, or `ui rot 0`, `ui rot 90`... |
| Low battery behaviour | Check `t5_ui_low_power()` paths |
| Exit during work | Press back while a job runs |
| Launch twice quickly | Tap the icon, go home, tap again |
| Session restore | Soft off with your app in front, then power on |

## The simulator

`t5 sim` is an SDL window running the OS's own UI code on your computer. `t5 sim --shell` runs the launcher, status bar and quick settings.

It's useful for checking how Material widgets look and for working on the shell. It has limits to be aware of:

- It doesn't load `.t5app` packages. Apps run on the device.
- Frame rates are desktop numbers. The only meaningful performance run is on the device (`ui gate`).

`t5 sim-shots [DIR]` renders screens into DIR (default `sim/build/shots`) and compares them against `sim/golden`.

## Static checks

```sh
riscv32-esp-elf-nm -D --undefined-only build/myapp.elf     # every name must be in api_v1.sym
python3 tools/t5/t5pack.py --inspect build/com.you.app.t5app
python3 tools/lint_px.py path/to/your/src                  # raw pixel numbers in layout calls
```

Add `-Werror` to your build, and consider `-fanalyzer` when the code is settled.

## Recovering the device

| Symptom | Fix |
| --- | --- |
| App crashes at launch and session restore relaunches it | It stops after two crashes. Remove it with `t5 rm`, or in App Manager |
| Boot problems | Safe mode boots without installed apps: `ota safemode` on the console |
| OS damaged | The recovery image: `ota recovery`. [Installing and updating](../flashing.md) has more |
