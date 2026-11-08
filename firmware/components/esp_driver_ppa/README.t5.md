# esp_driver_ppa (Tab5 OS copy)

ESP-IDF v6.1's PPA driver with one change, in `src/ppa_srm.c`: scale-rotate-mirror
always bypasses the macro-block order on the P4. IDF's size-based DIG-734 workaround
still lets a 90/270 rotation hang the PPA on rev1 silicon (Unit A) when a fill runs
beside it. The fix is Espressif's, from esp_lvgl_adapter:
`0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch`
(espressif/esp-iot-solution, components/display/tools/esp_lvgl_adapter).

Being in `components/`, this copy overrides the IDF component. Drop it once IDF ships
the fix. Measured 2026-11-06: scroll 30 to 37 fps, no hangs in stress runs.
