# Tab5 OS recovery image (plan 9.2). A small separate app in the `recovery`
# partition (0x50000, 1.5 MB less the boot-control sector): display, touch,
# SD card and /data, and the update installer. The bootloader starts it after
# three failed boots, when the OS asks (Settings > Updates, or a RECOVERY
# file on the SD card), and it can reinstall the OS, clear settings and save
# logs. Build with `t5 recovery-build`, write with `t5 recovery-flash`.
cmake_minimum_required(VERSION 3.22)
set(T5_FW "${CMAKE_CURRENT_LIST_DIR}/../firmware")
set(EXTRA_COMPONENT_DIRS
    "${T5_FW}/components/t5_core" "${T5_FW}/components/t5_pins" "${T5_FW}/components/t5_i2c"
    "${T5_FW}/components/t5_bsp" "${T5_FW}/components/t5_display" "${T5_FW}/components/t5_input"
    "${T5_FW}/components/t5_storage" "${T5_FW}/components/t5_bootfmt" "${T5_FW}/components/t5_ota")
set(SDKCONFIG_DEFAULTS "${T5_FW}/sdkconfig.board;sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" T5_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${T5_VERSION}")
project(tab5recovery)

# Coding standard (docs/coding-standard.md): recovery is one of the parts
# that must never fail, so its own code gets the strict profile.
include(${T5_FW}/cmake/t5_standard.cmake)
t5_apply_standard(
    COMPONENTS t5_core t5_pins t5_bsp t5_display t5_input t5_storage
    STRICT     main t5_i2c t5_ota)
