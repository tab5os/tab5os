# Bench procedures that need a person

These Phase 0 and 1 checks need hands on the hardware. Record the results in [bench-results.md](bench-results.md).

Open the console with `. tools/env.sh && t5 monitor` (Ctrl-] quits), or run single commands with `t5 console "cmd" "cmd"`.

**Before you start, know how to recover the board.** If it goes dark and USB disappears, press the power button once. If it's stuck in a boot loop, run `esptool chip-id`. That resets it through the ROM bootloader, which reloads the eFuses.

## Visual checks for Phase 1

1. Reset the board. The screen should show eight colour bars on the top half, a grey ramp, then a dark grid with a 4 px white border. There's a red square at the top-left of the panel's native portrait orientation.
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
