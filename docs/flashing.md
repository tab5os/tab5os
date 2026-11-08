# Installing and updating Tab5 OS

Nov 7, 2026 · plan 9.7 · for the M5Stack Tab5 (ESP32-P4)

## A release, from scratch

You need a USB-C cable and [esptool](https://docs.espressif.com/projects/esptool/) (`pip install esptool`).

1. Unpack the release and open `firmware/`.
2. Connect the Tab5 over USB-C. If it doesn't show up as a serial port, hold the reset button for a moment.
3. Run `./flash_all.sh` (or `PORT=/dev/ttyACM0 ./flash_all.sh`). It writes the bootloader, the partition table, Tab5 OS into the first system slot and the recovery image. Settings and files in `/data` are kept; the SD card isn't touched.
4. The Tab5 restarts into the launcher.

To back up the factory firmware first: `esptool --port PORT read-flash 0 0x1000000 tab5-factory.bin` (16 MB, a few minutes).

## Updating

Updates never need a cable once Tab5 OS is installed, and a failed one never leaves the Tab5 unable to start:

- **From the SD card:** copy `tab5os-<version>.t5os` to the card's top folder (or `/update`), then Settings > Updates and tap it.
- **From a computer in developer mode:** `t5 ota tab5os-<version>.t5os`.

The update is written to the other system slot and checked (signature, contents, chip revision) before the Tab5 will start it. After the restart it runs on trial: if it crashes or is reset in its first minute, the Tab5 goes back to the previous version by itself. Updates signed by a key the installed system doesn't trust, and unsigned ones, install only in developer mode.

## When it doesn't start

- **Safe mode** starts by itself after two failed starts in a row (or from Settings > Updates): the system without installed apps, with the last crash on the home screen. Remove or update the app in App Manager and restart.
- **Recovery** starts by itself after three failed starts, from Settings > Updates, or when the SD card holds a file named `RECOVERY` in its top folder. It can restart the system, start it in safe mode, reinstall it from a `.t5os` on the SD card, save crash logs to the card, clear settings, or erase `/data`.
- If even recovery doesn't start, `./flash_all.sh` over USB-C always works: the chip's own download mode can't be switched off by software.

## From source

```sh
git clone https://github.com/<you>/tab5os && cd tab5os
git submodule update --init third_party/micropython      # not --recursive: its own submodules aren't needed
. tools/env.sh                     # ESP-IDF v6.1
tools/t5/t5 build && tools/t5/t5 flash          # bootloader, partition table, OS
tools/t5/t5 recovery-build && tools/t5/t5 recovery-flash
```

`tools/release.sh [--sign key.pem]` builds everything above into `dist/`.
