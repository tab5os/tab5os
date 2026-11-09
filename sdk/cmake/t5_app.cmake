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
# only) and <name>.t5app (the installable package).
#
# Headers and flags: the SDK release ships them in sdk/sysroot/ (the headers
# the public SDK headers reach, and the firmware's compile flags with
# relative paths; sdk/cmake/sysroot.py). Then nothing but the RISC-V
# compiler from ESP-IDF v6.1 is needed. In the source tree there is no
# sdk/sysroot/: it is staged into the build directory from the firmware
# build, T5_FIRMWARE_BUILD (default: the repo's firmware/build).
get_filename_component(T5_SDK "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(T5_ROOT "${T5_SDK}/.." ABSOLUTE)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

if(EXISTS "${T5_SDK}/sysroot/flags.cmake")
    set(T5_SYSROOT "${T5_SDK}/sysroot")
else()
    if(NOT T5_FIRMWARE_BUILD)
        set(T5_FIRMWARE_BUILD "${T5_ROOT}/firmware/build")
    endif()
    set(T5_SYSROOT "${CMAKE_BINARY_DIR}/t5_sysroot")
    set(_db "${T5_FIRMWARE_BUILD}/compile_commands.json")
    if(NOT EXISTS "${_db}")
        message(FATAL_ERROR "t5_app: no sdk/sysroot and no firmware build at ${T5_FIRMWARE_BUILD} "
                            "(use an SDK release, or run `t5 build` in the repo)")
    endif()
    if(NOT EXISTS "${T5_SYSROOT}/flags.cmake" OR "${_db}" IS_NEWER_THAN "${T5_SYSROOT}/flags.cmake")
        execute_process(COMMAND ${Python3_EXECUTABLE} ${T5_SDK}/cmake/sysroot.py stage ${T5_FIRMWARE_BUILD} ${T5_SYSROOT}
                        RESULT_VARIABLE rc)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "t5_app: can't stage the SDK headers from ${T5_FIRMWARE_BUILD}")
        endif()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_db}")
endif()
include("${T5_SYSROOT}/flags.cmake")

# The flags (-march with Espressif's extensions, picolibc) need the compiler
# the OS was built with.
execute_process(COMMAND ${CMAKE_C_COMPILER} -dumpversion OUTPUT_VARIABLE _gcc OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _want "${T5_TOOLCHAIN_VERSION}")
if(NOT _gcc VERSION_EQUAL _want)
    message(FATAL_ERROR "t5_app: ${CMAKE_C_COMPILER} is GCC ${_gcc}; Tab5 OS apps need the ESP-IDF v6.1 toolchain "
                        "(${T5_TOOLCHAIN_VERSION}, GCC ${_want}). Source tools/env.sh or put that riscv32-esp-elf on PATH.")
endif()

function(t5_app name)
    cmake_parse_arguments(A "" "MANIFEST;ICON;ASSETS" "SRCS" ${ARGN})
    if(NOT A_MANIFEST)
        set(A_MANIFEST manifest.toml)
    endif()

    add_executable(${name}.elf ${A_SRCS})
    target_include_directories(${name}.elf PRIVATE ${T5_SDK}/include)
    target_compile_options(${name}.elf PRIVATE ${T5_APP_FLAGS}
        -fPIC -O2 -g0 -ffunction-sections -fdata-sections -fvisibility=hidden
        -fno-common -fno-jump-tables -fno-tree-switch-conversion
        -D__PICOLIBC_ERRNO_FUNCTION=t5_errno   # errno is thread-local: see t5_errno()
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
