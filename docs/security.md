# Tab5 OS security defaults

Nov 7, 2026 · plan 9.4 · [design, "Updates, recovery and security"](tab5os-design.md#updates-recovery-and-security)

Tab5 OS is open for tinkering by default. The stricter options exist but are opt-in, because the hardware ones can't be undone.

## What is on by default

| Area | Default |
| --- | --- |
| Network exposure | Nothing listens on the network. Developer mode (Settings > Developer mode, off by default) starts the only server, on port 8350, and every request except pairing needs a token from a computer paired on the device with a code shown on its screen. The tokens are random 256-bit values; the Tab5 stores only their SHA-256. |
| Saved credentials | Wi-Fi passwords are sealed with AES-256-GCM before they go into NVS, under a key derived from this chip's factory MAC and its 128-bit unique ID (`t5_storage/secret.h`). A flash dump read on another device or a PC doesn't show them. Someone holding the Tab5 can read both IDs over USB, so this doesn't protect against them: see the next section. |
| OS updates | Installed only if signed by a key built into the running OS (`firmware/ota_keys/`); otherwise only in developer mode (Settings, or pushed from a paired computer), with `ota ... unsigned` on the USB-C console, or, unsigned and after asking, from the recovery image (`t5_ota_source_t` in `t5_ota/ota.h`). Every update is checked against its SHA-256 and for the chip revision before it can boot, and runs on trial until it has been up for a minute: a crash or reset during the trial boots the previous system again. |
| Apps | Python apps reach hardware only through `t5`, with permission checks ([python.md](python.md#containment)). Native apps have full access; the installer says so. Unsigned apps install with that warning; signed ones name their publisher's key. |
| USB | The USB-C console is always on (it is how a broken device is fixed). USB mass storage hands over only the SD card, never `/data`, and only when asked. |
| Telemetry | None. Crash data stays on the device unless you save it (App Manager, or recovery's "Save logs to the SD card"). |

## What is off, and why

ESP32-P4 secure boot and flash encryption are configured by burning eFuses. Burning them is permanent: there is no way back to an open device, a lost signing key bricks the update path for good, and a mistake in the encryption setup can leave the device unable to boot at all. Tab5 OS leaves both off and its build never burns an eFuse.

Without them, anyone with the device and a USB cable can read the flash (including NVS) and write any firmware. If you need protection against that:

1. **Secure boot v2** (`CONFIG_SECURE_BOOT`): the ROM then boots only a bootloader signed with your key, and the bootloader only signed apps. Keep the key offline and backed up; losing it means no more updates.
2. **Flash encryption** (`CONFIG_SECURE_FLASH_ENC_ENABLED`, release mode): flash contents are encrypted with a key in eFuse the CPU can't read out. Then NVS encryption can use the flash-encryption scheme instead of the chip-ID key above.
3. Disable the ROM download mode or set it to secure mode.

Do this on a spare unit first, read Espressif's [security guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/security/security.html) end to end, and expect that the boot-control record, recovery image and A/B layout used here need signing too. Tab5 OS hasn't been tested with either option on.

## Reporting

Security problems: open a private advisory on the repository rather than a public issue.
