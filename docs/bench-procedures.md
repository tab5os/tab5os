# Bench procedures that need a person

These Phase 0 and 1 checks need hands on the hardware. Record the results in [bench-results.md](bench-results.md).

Open the console with `. tools/env.sh && t5 monitor` (Ctrl-] quits), or run single commands with `t5 console "cmd" "cmd"`.

**Before you start, know how to recover the board.** If it goes dark and USB disappears, press the power button once. If it's stuck in a boot loop, run `esptool chip-id`. That resets it through the ROM bootloader, which reloads the eFuses.

## Visual checks for Phase 1

1. Reset the board. The screen stays dark until the launcher's first frame is drawn, then the backlight comes on. (The Phase 1 colour-bar test pattern was removed in Phase 5; `disp fill <r> <g> <b>` still paints the panel for checks, when the UI isn't running.)
2. Run `touch dots on` and draw on the screen. Coloured dots should follow your finger, in a different colour for each finger. Turn it off with `touch dots off`.
3. Run `disp sleep` then `disp wake`. The screen goes dark and comes back with the same image.
4. Run `disp bl 10`, `disp bl 100`, then `disp bl 80`. Check that the brightness steps and doesn't flicker or whine.

## 0.5 Confirm `PWROFF_PLUSE` (0x44 P4)

On battery only, with USB unplugged and the console reachable over Wi-Fi or by watching the screen:

1. Run `exp pulse 0x44 4 50 --force`, which drives P4 high for 50 ms.
2. Expected: the board powers off. Note whether the screen dies at once.
3. Repeat with 10 ms and 200 ms pulses to find the minimum width.

## 0.6 Power button

Put a logic analyser on G35 (`BOOT_GPIO35`, the M5-Bus pin) and `MPWR_EN` (PMS150G pin 6, from the schematic).

1. While the board runs `gpio watch 30000 35`, give one short press, then wait 5 s. Does G35 show the press, and for how long?
2. Double press. Does `MPWR_EN` drop at once, or after a delay? Is there any warning on G35 first?
3. Long hold (10 s). Note when download mode starts. The OS must never take this over.

## 0.7 IMU interrupt routing (have someone ready to press the power button)

1. Run `imu motion 50 high`. This arms a latched any-motion interrupt on INT1 and INT2.
2. Run `gpio watch 20000 log`, then tap the board.
3. Expected: either a GPIO edge shows up (the IMU reaches the P4), or the board powers off, as it does for the RTC. After a power-off, press the button and run `fs cat /data/edges.log`.
4. Run `imu motion off` afterwards. Don't try a non-latched or periodic IMU interrupt. If it toggles power, it could cycle the board on and off.

## 0.8 Real wake events from light sleep

1. Run `sleep light 30000 8 g23 g35 g14` and touch the screen within 30 s. Then run `sleep result`. It should report `wake=gpio` with bit 23 set in the mask.
2. Repeat, pressing the power button once instead. Check whether a short press wakes the board without the PMS150G turning it off.
3. Note what the screen shows during sleep (frozen image, black, or noise).

## 0.9 Power per state

Put a logging meter rated for 8.4 V in series with the pack, and unplug USB.

| State | How to get there |
| --- | --- |
| Active, idle console | Boot and wait 30 s |
| Backlight off | `disp bl 0` |
| Panel asleep | `disp sleep` |
| Light sleep | `sleep light 20000 0` |
| Soft off | Pulse `PWROFF_PLUSE` (0.5) |

## 0.11 Connector pins

Meter each `ext` and `stamp` pin against the table in the design doc. Toggle them with `gpio watch` or with a test build. Also measure the M5-Bus BAT pin, both on battery and on USB.

## 1.8 Real power-cut test

`test/hil/fs_powerloss.py --mode powercut --cycles 10` cuts main power through the RTC interrupt mid-write. The board doesn't always come back by itself. When the script waits on "Tab5 not reachable", press the power button and run it again. The chip-reset variant runs unattended: `--mode reset --cycles 30`.

## 1.10 I2C stress with a stuck bus

1. Run `test/hil/i2c_stress.py --seconds 3600`. It runs for 1 hour unattended.
2. For the physical part, run it with `--manual-short` and short the system SDA line (G31, brought out as `SYS_SDA` on the M5-Bus connector) to GND with a probe for about 100 ms, a few times. Pass means `I2C STRESS PASS`: no bad data, recoveries counted, and every device answering afterwards.

## Phase 2 on battery

Unplug USB. Keep a hand near the power button: one press brings the board back.

1. **Soft off (also 0.5).** `power off --force`. Expected: the screen goes dark within a second and the board stays off. Press the button: `power` should then show `wake reason button, last off user`.
2. **RTC alarm wake.** `power alarm 120`, then `power off --force`. Expected: the board comes back on by itself about 2 minutes later, with `wake reason rtc-alarm`.
3. **USB detection.** `bat` should show `usb no` and a current well above 50 mA. Note the `CHG_STAT` level.
4. **Discharge trace.** `bat log start` (writes `/sd/battery-<epoch>.csv`), then leave it running to empty. The device turns itself off at 5% or 6.5 V. Copy the file off the card.
5. **Draw per state (0.9).** Meter in series with the pack, then `test/hil/power_cycle.py --cycles 1 --meter-pause`.

## Charge mode (bench checks 7 and 8)

With USB attached, `power off --force` enters charge mode: a battery screen for 5 s, then the P4 deep-sleeps and checks every 60 s. Check that the pack keeps charging (`CHG_EN` holds in deep sleep, bench check 8), that unplugging USB turns the board off within a minute, and that the button boots normally.

## C6 firmware update (plan 3.5)

This replaces the factory C6 firmware (esp_hosted 1.4.1) with ours (3.1.0). It's done over the link, but rollback depends on the C6's factory bootloader, which hasn't been checked. If the C6 doesn't come back, recovering it needs its UART.

1. `t5 c6-build`, then `t5 c6-stage` (writes the `c6fw` partition).
2. `t5 console "net"` should show the staged version, then run `net update`. Watch the log for `C6 updated to t5-c6-...`.

## Phase 4: display pipeline and the rendering gate

Flash the current firmware (`t5 build && t5 flash`). After boot the screen stays dark until the launcher (a list titled "Tab5 OS") is drawn.

1. **Orientation and touch.** Run `ui rot 90`, `ui rot 270`, `ui rot 0`, `ui rot 180` in turn. Each time, the app bar must be along the top edge as you look at it, and tapping a list row must ripple under your finger (not mirrored). If landscape comes out upside down or mirrored, the PPA angle mapping in `t5_ui/ui_hw.c` (`angle()`) is the thing to change.
2. **Auto-rotate axes.** Run `ui imu 10` and hold the board upright in portrait, then turn it to each edge. The angle should read about 0, 90, 180 and 270, and the screen should follow after half a second. If the angle is off by 90 or 180 degrees, or runs backwards, fix the IMU axis map in `t5_ui/ui.c` (`s_axes`, marked unverified). Lay it flat: the screen must not rotate.
3. **Rotation lock.** `ui lock on`, reboot, and check the orientation and the lock survived.
4. **The gate (plan 4.8).** Run `ui stats on` to see counters on screen, then `ui gate all 10` in landscape (`ui rot 90`) and again in portrait (`ui rot 0`). Targets: scroll ≥ 57 fps sustained (60 on the 64.4 Hz panel, one dropped frame a second allowed), transitions and the dialog ≥ 30 fps. The command prints PASS or FAIL per mode with render and PPA copy times. Record both orientations in bench-results. If it fails, try the fallbacks in the plan's order.
5. **Touch to ripple.** With `ui stats on`, tap a list row: the `touch` figure is press to the end of the refresh that drew the first ripple frame. It should be under 16.7 ms.
6. **Tearing.** During `ui gate transition`, look for a horizontal tear line. There should be none (three buffers, swapped at the end of a frame).
7. **Banding.** Look at the dialog's shadow and the app bar overlay on the dark background, close up. Note any visible steps.

## Phase 5: apps

Build the test apps with `t5 apps`. Either copy `test/apps/build/*.t5app` to the SD card and run `app install /sd/<name>.t5app`, or serve the build directory from the Mac (`python3 -m http.server 8765 --bind <mac-ip>` in `test/apps/build`) and run `app fetch http://<mac-ip>:8765/<name>.t5app` on the device. Reinstalling an app clears its crash record.

1. **Lifecycle and cleanup (the exit test).** `app stress com.t5.test.leaky 500`. Every line must say heap, pins, locks and rails are back at baseline, and the summary must say PASS. The run takes about 4 minutes and holds a screen wake lock so the idle timers don't turn the UI off. Also open Leaky by hand, check that grove shows as claimed (`port`), the wake lock (`power locks`) and the dialog, then `app exit` and check they're gone.
2. **Crash guard.** `app run com.t5.test.crasher`. The screen goes dark at once (the backlight is switched off in the panic handler), stays dark through the reboot, and comes back on with the launcher: no blue, cyan or test-pattern screens. `app crashes` should show one crash with a dump path. Do it again: two in a row, and `app` shows "auto-launch blocked". Open Hello and exit it; Crasher stays blocked until it exits cleanly.
3. **UI watchdog.** `app run com.t5.test.hang`. After about 2.5 s "App not responding" appears. Tap Wait: it goes away and comes back about 5 s later. Tap Close app: the board restarts to the launcher, and `app crashes` lists Hang as "not responding".
4. **ABI stability.** Rebuild the firmware after any unrelated change, flash it, and check the `.t5app` files built earlier still run without reinstalling.
5. **Revisions.** Only `rev1` (v1.x silicon) is targeted; `rev3` is not tested.
