# SPDX-License-Identifier: Apache-2.0
# Build a Tab5 OS native app (plan 5.7).
#
#   cmake_minimum_required(VERSION 3.22)
#   set(CMAKE_TOOLCHAIN_FILE ${T5_SDK}/cmake/toolchain-t5.cmake)
#   project(hello C)
#   include(${T5_SDK}/cmake/t5_app.cmake)
#   t5_app(hello SRCS main.c MANIFEST manifest.toml [ICON icon.png] [ASSETS dir])
#
# Produces <name>.elf (position-independent, entry t5_app_entry, imports
# only) and <name>.t5app (the installable package). T5_FIRMWARE_BUILD points
# at the firmware build the app is compiled against (default: the repo's
# firmware/build-rev1); it supplies the exact headers and LVGL config.
get_filename_component(T5_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(T5_ROOT "${T5_SDK}/.." ABSOLUTE)
if(NOT T5_FIRMWARE_BUILD)
    set(T5_FIRMWARE_BUILD "${T5_ROOT}/firmware/build-rev1")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(t5_app name)
    cmake_parse_arguments(A "" "MANIFEST;ICON;ASSETS" "SRCS" ${ARGN})
    if(NOT A_MANIFEST)
        set(A_MANIFEST manifest.toml)
    endif()
    execute_process(COMMAND ${Python3_EXECUTABLE} ${T5_SDK}/cmake/app_flags.py ${T5_FIRMWARE_BUILD}
                    OUTPUT_VARIABLE flags RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "t5_app: can't read the firmware's compile flags (${T5_FIRMWARE_BUILD})")
    endif()
    string(REPLACE "\n" ";" flags "${flags}")

    add_executable(${name}.elf ${A_SRCS})
    target_include_directories(${name}.elf PRIVATE ${T5_SDK}/include)
    target_compile_options(${name}.elf PRIVATE ${flags}
        -fPIC -O2 -g0 -ffunction-sections -fdata-sections -fvisibility=hidden
        -fno-common -fno-jump-tables -fno-tree-switch-conversion
        -Wall -Wextra -Wno-unused-parameter)
    target_link_options(${name}.elf PRIVATE
        -nostdlib -nostartfiles -shared -fPIC -e t5_app_entry
        -Wl,--gc-sections -Wl,--strip-all -Wl,-z,noexecstack)
    set_target_properties(${name}.elf PROPERTIES SUFFIX "" OUTPUT_NAME ${name}.elf)

    # Package: manifest, the ELF as its entry, optional icon and assets.
    set(stage ${CMAKE_CURRENT_BINARY_DIR}/${name}.pkg)
    set(cmds COMMAND ${CMAKE_COMMAND} -E rm -rf ${stage}
             COMMAND ${CMAKE_COMMAND} -E make_directory ${stage}
             COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/${A_MANIFEST} ${stage}/manifest.toml
             COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${name}.elf> ${stage}/app.elf)
    if(A_ICON)
        list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/${A_ICON} ${stage}/)
    endif()
    if(A_ASSETS)
        list(APPEND cmds COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/${A_ASSETS} ${stage}/${A_ASSETS})
    endif()
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${name}.t5app
        ${cmds}
        COMMAND ${Python3_EXECUTABLE} ${T5_ROOT}/tools/t5/t5pack.py ${stage} -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.t5app
        DEPENDS ${name}.elf ${CMAKE_CURRENT_SOURCE_DIR}/${A_MANIFEST}
        COMMENT "Packing ${name}.t5app" VERBATIM)
    add_custom_target(${name}_pkg ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${name}.t5app)
endfunction()
