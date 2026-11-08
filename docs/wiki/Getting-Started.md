# Getting started

From nothing to your own app on the screen, in C or Python.

## What you need

- A Tab5 running Tab5 OS. To flash one, see [installing and updating](../flashing.md).
- For **C apps**: ESP-IDF at the version in `firmware/idf_version.txt` (v6.1), and one firmware build, because apps compile against that build's headers and LVGL configuration.
- For **Python apps**: nothing but Python 3.11+ on the computer. You can also write them on the device, in the Editor app.

```sh
git submodule update --init third_party/micropython   # once, after cloning
. tools/env.sh                                        # puts the compiler and `t5` on PATH
t5 build                                              # the firmware build apps compile against
```

## Connect the computer

Two transports work: Wi-Fi after pairing, which is fast, and the USB-C console, which needs no setup.

```sh
# On the Tab5: join Wi-Fi, then Settings > Developer mode
t5 pair 192.168.1.50      # the Tab5 shows a six-digit code; type it here
t5 devices                # paired Tab5s
```

`t5 run` uses Wi-Fi if a paired Tab5 in developer mode is reachable, and otherwise falls back to USB-C, at about 20 KB/s. Add `--usb` to force USB. **`printf` from a C app only appears on the USB-C console**, so use `t5 run --usb` while you're debugging with `printf`. See [Debugging](Debugging-and-Testing.md#logs).

## Your first C app

```sh
t5 new "Hello" --id com.you.hello
cd hello
t5 run
```

That makes three files:

| File | Purpose |
| --- | --- |
| `main.c` | The app |
| `manifest.toml` | Identity and permissions ([reference](Manifest-and-Packaging.md)) |
| `CMakeLists.txt` | One line that matters: `t5_app(hello SRCS main.c MANIFEST manifest.toml)` |

The template, trimmed:

```c
#include <t5/app.h>
#include <t5/ui.h>

static void back(lv_event_t *e)
{
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    md_appbar_create(scr, "Hello", MD_ICON_ARROW_BACK, back, NULL);
    md_label_create(scr, "Hello, Tab5", MD_TYPE_HEADLINE5);
}

T5_APP(.on_create = on_create);
```

What's happening:

- `T5_APP` declares your callbacks. The loader calls `on_create` on the UI task.
- `t5_app_screen` is your root object. Anything under it is deleted for you at exit.
- `md_*` are Material Design widgets on top of LVGL 9.6. All of LVGL is available too.
- There's no `main`, no `free` you must remember, and no linking against the OS: every call is bound by name at load time.

## Your first Python app

```sh
t5 new "Hello" --python --id com.you.hello
cd hello
t5 run
```

```python
import t5

screen = t5.ui.screen("Hello")           # app bar with a back arrow
label = screen.label("Hello, Tab5", role="h5")
screen.button("Tap me", fn=lambda b: label.set_text("Tapped"))

while t5.app.running():
    t5.sleep_ms(250)                     # tap callbacks run in here
```

Python apps are slower and can't crash the OS. See [C or Python?](Platform-Overview.md#c-or-python) and [Python apps](Python-Apps.md).

## The edit-run loop

```sh
t5 run                       # build, push, launch, stream the log. Over Wi-Fi, about 3 s
t5 screenshot out.png        # what's on the panel, over Wi-Fi
t5 logs --grep com.you       # the OS log
t5 stop                      # close the foreground app
t5 rm com.you.hello          # uninstall (add --keep-data to keep its files)
```

Without a Tab5 nearby, `t5 sim` runs the same Material code on your computer, with `--shell` for the launcher. It runs the widget gallery and the OS shell rather than your app. See [the simulator](Debugging-and-Testing.md#the-simulator).

## Adding files

A multi-file C app lists its sources. Extra compiler options go on the `.elf` target:

```cmake
t5_app(airlog SRCS main.c sensors.c ui.c MANIFEST manifest.toml ASSETS assets)
target_compile_options(airlog.elf PRIVATE -Werror)
```

`ASSETS dir` copies a folder into the package, readable at run time under `t5_app_install_dir(app)`. A Python app's folder is packed whole, except `build`, dotted folders and caches.

## Next

Read [App lifecycle](App-Lifecycle.md) and [Threading](Threading.md) before writing anything that does I/O, then take a recipe from the [Cookbook](Cookbook.md). The [example apps](../../apps/examples) are real ones: Weather is the fullest (networking, caching, navigation, rotation), Logger shows background wakes, and UVC Monitor shows a fast image pipeline.
