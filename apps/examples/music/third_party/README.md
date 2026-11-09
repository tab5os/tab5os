# Third-party code in the Music example

| Library | Version | License | Use |
| --- | --- | --- | --- |
| [minimp3](https://github.com/lieff/minimp3) | commit ea99364f61c14656440e8d77e9c233ccf3124633 | CC0 1.0 (`minimp3/LICENSE`) | MP3 decoding |
| [dr_flac](https://github.com/mackron/dr_libs) | commit e676472be4328a88921a686085695adae3d3af31 | Public domain or MIT No Attribution (in the header) | FLAC decoding |
| TJpgDec R0.03 | as bundled with LVGL 9.6 | ChaN's free license (in the source) | Album art JPEG decoding |
| [Material Icons](https://github.com/google/material-design-icons) | the commit pinned in `tools/assets/fonts.py` | Apache 2.0 | `../music_icons_*.c`: the player's icons the OS font leaves out, made by `../gen_fonts.py` |

The OS image has no MP3, FLAC or JPEG decoder (it doesn't fit its size budget), so the app carries its own, as `docs/sdk.md` suggests. `tjpgd.c` and `tjpgd.h` are LVGL's copy with its `LV_USE_TJPGD` guard turned on and its LVGL includes replaced by `<stdint.h>`; `tjpgdcnf.h` is the app's own configuration.
