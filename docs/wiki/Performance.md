# Performance

The numbers here were measured on Unit A (ESP32-P4 v1.3 at 360 MHz, `rev1`), and the record is in [bench results](../bench-results.md).

## The mental model

LVGL redraws only what's invalidated, into 20-line bands in fast internal RAM. Each band is rotated and copied by the PPA into one of three framebuffers in PSRAM. The panel flips buffers at vsync, so there's no tearing and rendering never waits for the panel.

Two limits matter, and they're independent:

1. **CPU time on the UI task.** 16 ms per frame is shared by your callbacks, LVGL's tree walk and software drawing.
2. **PSRAM bandwidth.** About 40 to 60 MB/s while the panel scans out. A full-screen frame is 1.84 MB, and each frame reads and writes it.

The second is a hard wall. **An animation that changes the whole screen runs at about 27 fps**, however optimised your code is. Reduce how many pixels change.

| What's on screen | Measured |
| --- | --- |
| Scrolling a 100-row list in landscape | 53 to 58 fps average, render 12 ms a frame |
| Dialog appearing | about 33 to 40 fps while animating |
| App launch fade | about 31 fps |
| Full-screen slide | 27 fps, at the PSRAM floor |
| Camera preview | 11 to 13 fps through LVGL |

## What a pixel costs

| Cheap | Expensive (CPU, per pixel) |
| --- | --- |
| Unscaled images, opaque or with alpha, and translucent square fills on the PPA | Rounded corners |
| Opaque square backgrounds, drawn into fast internal RAM | Gradients |
| Not drawing at all | Live shadows (`lv_obj_set_style_shadow_*`; the shadow cache is off) |
| | Scaled or rotated images |
| | Anti-aliased text (about 8% of a scroll frame) |
| | Anything rendered through an intermediate layer: `lv_obj_set_style_opa_layered`, transforms |

Practical choices:

- Prefer square, opaque surfaces for anything large. A full-width bar is nearly free; a big rounded card over a textured background is not.
- Use `md_set_elevation`, which uses pre-baked shadows, instead of LVGL shadows.
- Give containers `lv_obj_remove_style_all` so they draw nothing.
- Pre-scale images to display size. Don't use `lv_image_set_scale` on anything big or animated.
- Pre-render expensive static things with `lv_snapshot_take` and show the result as an image. The buffer is tracked, and it counts against your heap.

## Object count

LVGL's objects and styles live in an 88 KB pool of internal RAM. Walking them is what limits frame rate on a busy screen, and once the pool overflows new objects go to PSRAM, where the walk runs 2 to 4 times slower.

- Keep live objects to a few hundred. Note that each `md_row` or button is several objects.
- Use [`md_list`](UI-Guide.md#lists) for long lists: 10,000 rows costs about 20 rows of objects.
- Delete pages you've left, rather than hiding them.
- Prefer one label with multiple lines to many labels.
- Share styles with `lv_style_t` when many objects are identical, rather than calling `lv_obj_set_style_*` on each, since each local style allocates.

`ui mem` on the console shows where LVGL's allocations landed. If `overflow` climbs while your app runs, you've run the pool out.

## Updating widgets

**Every set invalidates, even if the value is the same.** One real bug in the OS came from re-setting a scrim to its current colour, which redrew the entire screen. So:

```c
if (s->last_mv != b.mv) {
    s->last_mv = b.mv;
    lv_label_set_text_fmt(s->label, "%d mV", b.mv);
}
```

- Update at the speed a person can read: 2 to 5 Hz for numbers.
- Coalesce updates from fast sources ([how](Threading.md#1-t5_ui_async-fire-and-forget)).
- `lv_chart`: add points at a sensible rate, and use `LV_CHART_UPDATE_MODE_SHIFT`.
- Don't rebuild a list to change a row. Change the row, or call `md_list_refresh`.
- A blinking cursor or a tiny spinner is cheap. A full-width marquee is not.

Find what a frame redraws with `ui frames`, which reports each of the last 32 frames' area, render time and PPA time.

## Timers

Use `lv_timer_create` for periodic UI work, and pick the longest period that looks right. A 16 ms timer that does nothing still costs wakes and keeps the CPU from idling. Pause timers you don't need with `lv_timer_pause`.

## Animation

| Good | Costly |
| --- | --- |
| Small objects: a ripple, a switch thumb, a progress bar | Anything full-screen |
| `translate` on one object | Fading a large area, every frame |
| 150 to 300 ms | Long-running decorative loops |

`lv_screen_load_anim` is handled by the OS: both screens are rendered once to snapshots and slid or faded as images. Budget about 190 ms of set-up before the first frame.

In low-power mode the refresh is 33 ms. Check `t5_ui_low_power()` and drop decoration.

## Drawing your own pixels

For a game or visualisation:

1. Use `lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO)` with `lv_canvas`, or an `lv_image` over your own buffer.
2. Write pixels directly.
3. **Invalidate only the dirty rectangle** with `lv_obj_invalidate_area`, in screen coordinates.

The [canvas recipe](Cookbook.md#fast-pixel-drawing) does this. Budget roughly: a 320 x 320 px area updated every frame is easy; the whole screen caps near 25 fps.

For a full-screen surface, decide the resolution deliberately. Rendering at half resolution and scaling costs CPU, because LVGL scales in software, and the OS's own experiment with PPA scaling at this size was slower than copying. A smaller centred play area often beats scaling.

For streams, double-buffer: fill the hidden buffer on a worker, then swap the image source on the UI task. Drop a frame rather than queue it (`uvc_monitor` keeps a `swap_pending` flag).

## CPU

- **Prefer `float`.** The FPU is single precision: `sinf`, `sqrtf`, `float` literals with `f`. `double` runs in software, through `__adddf3` and friends.
- Hot code and data should fit in the cache: L2 is 128 KB, shared with everything else.
- Your code is built with `-O2`. You can add `-O3` or `-ffast-math` to one file, with `set_source_files_properties` or `target_compile_options`, for numeric code.
- Compute on a task, pinned to core 0, and [yield regularly](Threading.md#cpu-hogs).
- `memcpy` into PSRAM runs at about 57 MB/s. Avoid large copies per frame: swap pointers.
- `malloc` from your heap takes a lock. Don't allocate per frame or per scanline: use a pool or a fixed buffer.

## Startup

The time to first frame is your `on_create` plus ELF loading.

- Build the first screen only. Load the rest lazily.
- Show cached data at once, then refresh on a worker. Weather loads its cache in `on_create` and fetches in the background.
- Keep the ELF small: fewer relocations, faster load. The build already uses `--gc-sections`.
- Very large `.rodata` costs PSRAM and load time. Move big assets to files in the package and read them when needed.

## Network and storage throughput

| Path | Figure | Note |
| --- | --- | --- |
| SD card, the card in Unit A | about 210 KB/s FAT32 sequential write at 512-byte clusters | Card and format dominate. Reformat with 32 KB clusters |
| H.264 encoder output | up to about 535 KB/s | Bitrate is lowered if the card falls behind |
| Wi-Fi app push | about 3 s end to end for a small app | |
| USB-C console transfer | about 20 KB/s | |

Do file and network I/O in chunks of 4 KB or more, off the UI task.

## Measuring

Over the USB-C console:

| Command | Tells you |
| --- | --- |
| `ui stats on` | Live overlay: fps, render time, copy time |
| `ui` | Current frame stats |
| `ui frames` | Each of the last 32 frames, including what it redrew |
| `ui mem` | LVGL pool use and overflow |
| `ui slowmo 8` | Run animations 8x slower, to see artefacts. `ui slowmo 1` to restore |
| `tasks` | CPU per task |
| `stacks` | Stack high-water marks |
| `sys` | Heaps: read **`heap dma`**, not `heap int`, for internal RAM |
| `app usage` | Your heap, handles, tasks |
| `prof start 3000 1000`, then `prof dump 800` | Sampling profile of core 1 |
| `tools/t5/t5prof.py firmware/build-rev1/tab5os.elf dump.log` | Map samples to OS functions |

A typical investigation:

1. Reproduce the jank with `ui stats on` showing.
2. Render time high? `ui frames` for what's being redrawn. Reduce the area first and the cost per pixel second.
3. Render time low but it still feels slow? Look for long callbacks: `ui` reports the longest UI loop.
4. Run `prof` to find the hot function.

Measure on the device. The simulator's frame rates are desktop numbers.
