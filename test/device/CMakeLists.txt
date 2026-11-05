# On-device Unity tests for the services (plan items 1.9, Phase 2 and 3).
# Build and run: tools/t5/t5 device-test   (flashes ota_0; run `t5 flash` after to restore the OS)
cmake_minimum_required(VERSION 3.22)

set(T5_FW ${CMAKE_CURRENT_LIST_DIR}/../../firmware)
if(NOT DEFINED T5_REV)
    set(T5_REV "$ENV{T5_REV}")
endif()
if(NOT T5_REV)
    set(T5_REV rev1)
endif()
set(SDKCONFIG_DEFAULTS "${T5_FW}/sdkconfig.defaults;${T5_FW}/sdkconfig.defaults.${T5_REV};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${T5_REV}")
# Services only: not t5_diag (the console), so its elf_loader dependency stays out.
set(EXTRA_COMPONENT_DIRS "")
foreach(c t5_core t5_pins t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input t5_power t5_battery t5_net)
    list(APPEND EXTRA_COMPONENT_DIRS ${T5_FW}/components/${c})
endforeach()
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5os_device_tests)
