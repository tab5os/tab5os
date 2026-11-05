# Host unit tests for service logic (ctest + Unity). Run: tools/t5/t5 host-test
#
# Every service's logic file builds here against mocks of its thin hardware
# layer (*_hw.c on the device, mocks/ here). That is the Phase 1 rule that
# keeps the simulator and these tests possible.
cmake_minimum_required(VERSION 3.22)
project(tab5os_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE)

include(FetchContent)
FetchContent_Declare(unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG v2.6.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unity)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../../firmware/components)

# All service logic plus mocks, as one library.
add_library(t5_host STATIC
    ${FW}/t5_core/t5_err.c
    ${FW}/t5_core/t5_event.c
    ${FW}/t5_core/t5_event_names.c
    ${FW}/t5_pins/t5_pins_names.c
    ${FW}/t5_i2c/t5_i2c.c
    ${FW}/t5_bsp/xpin.c
    ${FW}/t5_time/rtc.c
    ${FW}/t5_time/t5_time.c
    ${FW}/t5_imu/bmi270.c
    ${FW}/t5_imu/bmi270_config.c
    ${FW}/t5_storage/storage.c
    ${FW}/t5_display/display.c
    ${FW}/t5_input/touch.c
    ${FW}/t5_core/t5_crc.c
    ${FW}/t5_power/arbiter.c
    ${FW}/t5_power/wake_reason.c
    ${FW}/t5_power/rail.c
    ${FW}/t5_power/wakelock.c
    ${FW}/t5_power/suspend.c
    ${FW}/t5_power/autosave.c
    ${FW}/t5_power/power.c
    ${FW}/t5_battery/ina226.c
    ${FW}/t5_battery/gauge.c
    ${FW}/t5_battery/policy.c
    ${FW}/t5_battery/battery.c
    ${FW}/t5_net/netcfg.c
    ${FW}/t5_net/c6img.c
    ${FW}/t5_net/ble_util.c
    mocks/power_hw_mock.c
    mocks/chips_mock.c
    mocks/t5_os_mock.c
    mocks/t5_event_mock.c
    mocks/t5_i2c_hw_mock.c
    mocks/hw_stubs_mock.c)
target_include_directories(t5_host PUBLIC
    mocks
    ${FW}/t5_core/include ${FW}/t5_core
    ${FW}/t5_pins/include
    ${FW}/t5_i2c/include ${FW}/t5_i2c
    ${FW}/t5_bsp/include
    ${FW}/t5_time/include ${FW}/t5_time
    ${FW}/t5_imu/include
    ${FW}/t5_storage/include ${FW}/t5_storage
    ${FW}/t5_display/include ${FW}/t5_display
    ${FW}/t5_input/include ${FW}/t5_input
    ${FW}/t5_power/include ${FW}/t5_power
    ${FW}/t5_battery/include ${FW}/t5_battery
    ${FW}/t5_net/include ${FW}/t5_net)
target_link_libraries(t5_host PUBLIC unity)

enable_testing()
foreach(t test_bsp_names test_core test_i2c test_xpin test_time test_imu test_storage test_display_touch test_power test_battery test_net)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE t5_host)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
