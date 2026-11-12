# Releasing Tab5 OS

Nov 12, 2026 · for maintainers

A release is two pieces of firmware that update one after the other: the OS on the ESP32-P4, and the radio firmware on the ESP32-C6. This page says how a release is cut, how a Tab5 picks both up, and the rules that keep that working.

## Versions

| File | What it versions | Raised |
| --- | --- | --- |
| `VERSION` | The OS, the SDK and the release (`v<VERSION>`) | Every release. A minor step when the app API grows, a patch step otherwise |
| `c6/VERSION` | The C6 radio firmware (`t5-c6-<version>` in its image) | Only when the C6 image changes: `c6/`, the esp_hosted release, or the IDF version |
| `sdk/abi/api_v1.sym`, `T5_API_LEVEL` | The app API | See [ABI stability](abi-stability.md) |

The C6 has its own version so that a release which doesn't touch it doesn't make every Tab5 download and reflash it.

## Cutting a release

1. **In a pull request:**
   - Raise `VERSION`.
   - Raise `c6/VERSION` if the C6 image changed (`t5 release-check` says if it did).
   - Rename `## Unreleased` in `CHANGELOG.md` to `## <version> (pre-release), <date>`, with the install notes and the downloads table.
   - If esp_hosted moved, do the update test below and add its line to `c6/hosted-compat.txt`.
2. **ci:** `t5 lint` runs the release rules on every push, so a broken rule shows on the pull request. Merge it with a merge commit.
3. **Publish:** wait for `ci` on main to pass, then Actions > release > Run workflow on `main` (or `gh workflow run release.yml --ref main`). The workflow:
   1. Checks the tag is free, the `CHANGELOG.md` section exists, `ci` passed on the commit, and the release rules hold.
   2. Builds everything with `tools/release.sh`, signed with the release key (secret `T5_OTA_SIGNING_KEY`).
   3. Checks that both update files are signed by `firmware/ota_keys/release.pem`.
   4. Tags `v<VERSION>` and publishes:
      - the zip, with `flash_all.sh`;
      - `tab5os-<VERSION>.t5os`;
      - `tab5os-c6-<c6/VERSION>.t5c6`;
      - the SDK, the example apps and `SHA256SUMS`.
   5. Moves `tab5os/app-template` to the new SDK.

Every release carries a `.t5c6`, even when `c6/VERSION` didn't change. A Tab5 only looks in the release of the OS it runs.

## What a Tab5 does with a release

1. **The OS update** (from Settings over Wi-Fi, from the SD card, or `t5 ota`) writes only the P4's OS, to the other system slot. The C6 isn't touched.
2. **The restart:** the new OS runs on trial for its first minute, still with the old C6 firmware. If it fails, the Tab5 goes back to the previous OS, which matches the C6 that's still there.
3. **Once the new OS is kept,** it asks the C6 which firmware it runs (`t5_ota_net/c6.c`).
   - If that's `t5-c6-<c6/VERSION it was built with>`, nothing happens. This is the usual case.
   - Otherwise it takes `tab5os-c6-<c6 version>.t5c6`:
     - from the SD card's top folder or `/update`, or else
     - from its own release on GitHub, over Wi-Fi.
   - It checks the signature like an OS update's, writes the firmware to the `c6fw` partition and updates the C6 through esp_hosted.
   - This takes about 20 seconds, and Wi-Fi and Bluetooth drop for a few seconds at the end.
   - It stops after three failed tries per version (`ota c6 retry` starts it again). It never runs while an app has Wi-Fi or Bluetooth to itself.

So for a minute, and longer on a Tab5 without Wi-Fi, **the new OS runs with the previous release's C6 firmware**. Two things follow:

- **The link between the chips must stay compatible across one release.** Our own messages (`c6/main/t5_wifimon_proto.h`) only gain fields, and the P4 accepts the older replies. Features that need new C6 firmware check for it and say so, as the Wi-Fi monitor does.
- **esp_hosted is the risky part.** The C6 update itself travels over esp_hosted. If a new P4 can't talk to the old C6, the Tab5 has no Wi-Fi and can't update the C6 either. The only ways out are a `.t5c6` on the SD card, `flash_all.sh` over USB-C, or putting the old OS back.

## The release rules

`tools/t5/release_rules.py` (`t5 release-check`, and part of `t5 lint`) compares the tree with the previous release tag. It runs in `ci`, in the release workflow and in `tools/release.sh`.

1. `c6/VERSION` is a dotted version.
2. If anything that goes into the C6 image changed since the last release, `c6/VERSION` was raised. That's `c6/` apart from its README and build output, the esp_hosted release, and `firmware/idf_version.txt`. It never goes down.
3. The P4 and the C6 use the same esp_hosted release, in `firmware/dependencies.lock` and `c6/dependencies.lock`.
4. If esp_hosted moved, `c6/hosted-compat.txt` has a line `<old> -> <new>: ...`.

### The update test for a new esp_hosted

Before adding the `c6/hosted-compat.txt` line:

1. Put the previous release on a Tab5 with `flash_all.sh`. Its C6 should report the previous C6 version (`net` on the console).
2. Build this tree signed, or use developer mode.
3. Install it as an update (`t5 ota`), not with `flash_all.sh`, because that writes the C6 image too.
4. Check, after the first minute:
   - `ota c6` says the C6 runs the new version;
   - `net` shows the same version and `state connected`;
   - Bluetooth still works.
5. Record the release you updated from, the device and the date.

## Development builds

A build that isn't published has no release to download from. Its OS leaves the C6 alone when the C6 already runs the version in `c6/VERSION`, and otherwise reports `couldn't get ...: HTTP 404` in `ota c6`. To put a development C6 image on, use either:
- `t5 c6-build`, `t5 c6-stage`, then `net update` on the console;
- or `ota c6 file /sd/<file>.t5c6 unsigned`, then `net update`, with the package made by `python3 tools/ota/t5ota.py pack-c6 c6/build/tab5os_c6.bin`.
