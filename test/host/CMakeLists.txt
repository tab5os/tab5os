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
# All service logic plus mocks, as one library: every component (and main)
# with a portable.cmake lists the files that build here in it
# (T5_<NAME>_PORTABLE_SRCS). Their include/ directories are public; their
# private directories are visible to the library's sources only (not to the
# tests), so the device build is what keeps components out of each other's. t5_bootfmt and t5_ota
# are headers only here (bootctl, espimg, the slot and source rules).
file(GLOB T5_HOST_PORTABLE ${FW}/*/portable.cmake ${FW}/../main/portable.cmake)
set(T5_HOST_SRCS)
set(T5_HOST_INCS mocks ${FW}/t5_bootfmt/include ${FW}/t5_ota/include)
set(T5_HOST_PRIV)
foreach(pc ${T5_HOST_PORTABLE})
    get_filename_component(dir ${pc} DIRECTORY)
    get_filename_component(name ${dir} NAME)
    string(TOUPPER ${name} var)
    if(NOT var MATCHES "^T5_")
        set(var T5_${var})
    endif()
    include(${pc})
    list(APPEND T5_HOST_SRCS ${${var}_PORTABLE_SRCS})
    if(EXISTS ${dir}/include)
        list(APPEND T5_HOST_INCS ${dir}/include)
    endif()
    list(APPEND T5_HOST_PRIV ${dir})
endforeach()
file(GLOB T5_HOST_MOCKS ${CMAKE_CURRENT_LIST_DIR}/mocks/*.c)
add_library(t5_host STATIC ${T5_HOST_SRCS} ${T5_HOST_MOCKS})
target_include_directories(t5_host PUBLIC ${T5_HOST_INCS} PRIVATE ${T5_HOST_PRIV})
target_link_libraries(t5_host PUBLIC unity m)

enable_testing()
foreach(t test_pins test_core test_i2c test_xpin test_time test_imu test_storage test_display_touch test_power test_battery test_net test_ota test_ports test_pkg test_ui test_app test_media test_hid)
    add_executable(${t} ${t}.c)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(${t} PRIVATE t5_host)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
# The Calculator example's engine is plain C: tested here as it is.
set(CALC ${CMAKE_CURRENT_LIST_DIR}/../../apps/examples/calculator)
add_executable(test_calc test_calc.c ${CALC}/calc.c)
target_include_directories(test_calc PRIVATE ${CALC})
target_link_libraries(test_calc PRIVATE unity m)
add_test(NAME test_calc COMMAND test_calc)

# Tests of a component's internals see its private directory.
target_include_directories(test_app PRIVATE ${FW}/t5_app)
target_include_directories(test_display_touch PRIVATE ${FW}/t5_display)
target_include_directories(test_media PRIVATE ${FW}/t5_usb ${FW}/t5_camera)
target_include_directories(test_net PRIVATE ${FW}/t5_net)
target_include_directories(test_ota PRIVATE ${FW}/t5_ota)
target_include_directories(test_pkg PRIVATE ${FW}/t5_pkg)
target_include_directories(test_ports PRIVATE ${FW}/t5_ports)
target_include_directories(test_power PRIVATE ${FW}/t5_power ${FW}/../main)
target_include_directories(test_ui PRIVATE ${FW}/t5_shell)
# test_ota runs tools/ota/t5ota.py (the .t5os header both sides agree on),
# test_pkg tools/t5/t5pack.py (the manifest rules).
target_compile_definitions(test_ota PRIVATE T5_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
target_compile_definitions(test_pkg PRIVATE T5_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/../..")
