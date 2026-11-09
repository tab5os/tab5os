# On-device Unity tests for the services (plan items 1.9, Phase 2 and 3).
# Build and run: tools/t5/t5 device-test   (flashes ota_0; run `t5 flash` after to restore the OS)
cmake_minimum_required(VERSION 3.22)

set(T5_FW ${CMAKE_CURRENT_LIST_DIR}/../../firmware)
set(SDKCONFIG_DEFAULTS "${T5_FW}/sdkconfig.board;${T5_FW}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
# Services only: not t5_diag (the console), so its elf_loader dependency stays out.
include(${CMAKE_CURRENT_LIST_DIR}/components.cmake)
set(EXTRA_COMPONENT_DIRS "")
foreach(c ${T5_DEVICE_TEST_COMPONENTS})
    list(APPEND EXTRA_COMPONENT_DIRS ${T5_FW}/components/${c})
endforeach()
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5os_device_tests)
