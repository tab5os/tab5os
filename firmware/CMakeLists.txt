cmake_minimum_required(VERSION 3.22)

# Build variant: rev1 (ESP32-P4 v1.x) or rev3 (v3.x). Default rev1.
if(NOT DEFINED T5_REV)
    set(T5_REV "$ENV{T5_REV}")
endif()
if(NOT T5_REV)
    set(T5_REV rev1)
endif()
if(NOT T5_REV MATCHES "^rev(1|3)$")
    message(FATAL_ERROR "T5_REV must be rev1 or rev3, got '${T5_REV}'")
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${T5_REV}")
endif()
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${T5_REV}")
endif()

# System apps (plan 6.5-6.9) are normal apps against the public SDK, built in.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/sysapps")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" T5_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${T5_VERSION}")
project(tab5os)

# Hot paths keep -O2 under the size-first default (sdkconfig.defaults):
# LVGL's renderer, the MD2 layer, the display pipeline and the audio mixer.
foreach(hot lvgl__lvgl md t5_ui t5_audio)
    idf_component_get_property(hot_lib ${hot} COMPONENT_LIB)
    target_compile_options(${hot_lib} PRIVATE -O2)
endforeach()

# Coding standard (docs/coding-standard.md). esp_driver_ppa is a vendored
# copy of the IDF driver and keeps IDF's own warning set.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/t5_standard.cmake)
t5_apply_standard(
    COMPONENTS main sysapps md t5_audio t5_bsp t5_camera t5_core t5_diag t5_display t5_imu
               t5_input t5_net t5_pins t5_py t5_shell t5_storage t5_time t5_ui t5_usb
    STRICT     t5_power t5_battery t5_i2c t5_app t5_ota t5_pkg t5_dev t5_ports)
