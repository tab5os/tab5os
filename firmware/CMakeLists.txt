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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" T5_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${T5_VERSION}")
project(tab5os)
