cmake_minimum_required(VERSION 3.22)

# System apps (plan 6.5-6.9) are normal apps against the public SDK, built in.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/sysapps")
set(SDKCONFIG_DEFAULTS "sdkconfig.board;sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" T5_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${T5_VERSION}")
project(tab5os)

# Updates are refused unless the image's project name is T5_OTA_PROJECT.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/components/t5_ota/include/t5_ota/ota.h" _t5_ota_project
     REGEX "^#define T5_OTA_PROJECT ")
if(NOT _t5_ota_project MATCHES "\"${CMAKE_PROJECT_NAME}\"")
    message(FATAL_ERROR "T5_OTA_PROJECT (t5_ota/ota.h) is not \"${CMAKE_PROJECT_NAME}\"")
endif()

# Hot paths keep -O2 under the size-first default (sdkconfig.board):
# LVGL's renderer, the MD2 layer, the display pipeline and the audio mixer.
foreach(hot lvgl__lvgl md t5_ui t5_audio)
    idf_component_get_property(hot_lib ${hot} COMPONENT_LIB)
    target_compile_options(${hot_lib} PRIVATE -O2)
endforeach()

# Coding standard (docs/coding-standard.md). esp_driver_ppa is a vendored
# copy of the IDF driver and keeps IDF's own warning set.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/t5_standard.cmake)
t5_apply_standard(
    COMPONENTS main sysapps md t5_abi t5_audio t5_bsp t5_camera t5_core t5_diag t5_display t5_imu
               t5_input t5_net t5_pins t5_py t5_shell t5_storage t5_time t5_ui t5_usb
    STRICT     t5_power t5_battery t5_i2c t5_app t5_ota t5_ota_net t5_pkg t5_dev t5_ports)
