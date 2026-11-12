# Tab5 OS radio firmware for the ESP32-C6: the esp_hosted coprocessor with
# Tab5 wiring (SDIO to the P4, host wake on IO2 -> P4 G14) and our additions.
# Build: idf.py set-target esp32c6 (once), then idf.py build. See README.md.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The C6 firmware has its own version (c6/VERSION), raised only when the
# image changes, so an OS release moves the C6 only then (docs/releasing.md).
# The OS expects the same version (t5_ota_net/c6.c).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" T5_C6_VERSION LIMIT_COUNT 1)
# Re-run CMake when VERSION changes, so the image never keeps a stale version.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")
set(PROJECT_VER "t5-c6-${T5_C6_VERSION}")
project(tab5os_c6)
