# Tab5 OS radio firmware for the ESP32-C6: the esp_hosted coprocessor with
# Tab5 wiring (SDIO to the P4, host wake on IO2 -> P4 G14) and our additions.
# Build: idf.py set-target esp32c6 (once), then idf.py build. See README.md.
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" T5_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "t5-c6-${T5_VERSION}")
project(tab5os_c6)
