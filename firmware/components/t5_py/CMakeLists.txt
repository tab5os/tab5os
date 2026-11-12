# MicroPython runtime (plan 8.1-8.3). The interpreter is the upstream
# MicroPython tree pinned as a submodule (third_party/micropython); this
# component is its port: the configuration (mpconfigport.h, where the
# containment switches live), the HAL, the VM task, sandboxed files and the
# `t5` module. MicroPython's own CMake rules generate the qstr, module and
# root-pointer headers into the build dir.
set(T5_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(MICROPY_DIR "${T5_ROOT}/third_party/micropython" ABSOLUTE)
set(MICROPY_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT EXISTS "${MICROPY_DIR}/py/py.cmake")
    message(FATAL_ERROR "MicroPython is missing: run 'git submodule update --init third_party/micropython'")
endif()

set(MICROPY_ROM_TEXT_COMPRESSION ON)
include(${MICROPY_DIR}/py/py.cmake)
# String-only frozen modules: frozen.c replaces upstream's lookup (see there).
list(REMOVE_ITEM MICROPY_SOURCE_PY ${MICROPY_DIR}/py/frozenmod.c)

# Only the extmod modules on the allowlist are compiled at all.
set(T5_PY_EXTMOD
    ${MICROPY_DIR}/extmod/modbinascii.c
    ${MICROPY_DIR}/extmod/modhashlib.c
    ${MICROPY_DIR}/extmod/modheapq.c
    ${MICROPY_DIR}/extmod/modjson.c
    ${MICROPY_DIR}/extmod/modplatform.c
    ${MICROPY_DIR}/extmod/modrandom.c
    ${MICROPY_DIR}/extmod/modre.c
    ${MICROPY_DIR}/extmod/modtime.c
    ${MICROPY_DIR}/extmod/modselect.c       # polls stream objects only (t5 sockets)
    ${MICROPY_DIR}/extmod/modasyncio.c
    ${MICROPY_DIR}/extmod/moddeflate.c      # gzip/zlib: HTTP bodies
)
set(T5_PY_SHARED
    ${MICROPY_DIR}/shared/runtime/pyexec.c
    ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/runtime/gchelper_native.c
    ${MICROPY_DIR}/shared/readline/readline.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
)
set(T5_PY_PORT vm.c fs.c console.c runtime.c mod_t5.c mod_port.c mod_io.c mod_ui.c pynet.c mod_net.c mod_socket.c frozen.c)
set(T5_PY_PORT_ABS)
foreach(f ${T5_PY_PORT})
    list(APPEND T5_PY_PORT_ABS "${CMAKE_CURRENT_LIST_DIR}/${f}")
endforeach()

set(MICROPY_SOURCE_QSTR ${MICROPY_SOURCE_PY} ${T5_PY_EXTMOD} ${T5_PY_SHARED} ${T5_PY_PORT_ABS})
set(MICROPY_QSTRDEFS_PORT "${CMAKE_CURRENT_LIST_DIR}/qstrdefsport.h")

# Bindings generated from the published ABI (plan 8.3).
set(BIND_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    file(GLOB T5_SDK_HEADERS "${T5_ROOT}/firmware/components/*/include/*/*.h")
    file(GLOB T5_INC_DIRS LIST_DIRECTORIES true "${T5_ROOT}/firmware/components/*/include")
    set(GENBIND "${T5_ROOT}/tools/py/genbind.py")
    execute_process(
        COMMAND ${python} ${GENBIND} "${T5_ROOT}/sdk/abi/api_v1.sym" "${CMAKE_CURRENT_LIST_DIR}/bindings.txt"
                "${BIND_DIR}/bind_gen.h" ${T5_INC_DIRS}
        RESULT_VARIABLE genbind_rc)
    if(NOT genbind_rc EQUAL 0)
        message(FATAL_ERROR "tools/py/genbind.py failed")
    endif()
    # Standard-library Python frozen in as source: MicroPython's asyncio and
    # ours (ssl, network, requests) and micropython-lib's (umqtt, aiohttp).
    set(FROZEN_DIR "${CMAKE_CURRENT_LIST_DIR}/frozen")
    execute_process(
        COMMAND ${python} "${T5_ROOT}/tools/py/freeze_str.py" "${BIND_DIR}/frozen_content.c"
                "${MICROPY_DIR}/extmod/asyncio=asyncio" "${FROZEN_DIR}"
        RESULT_VARIABLE freeze_rc)
    if(NOT freeze_rc EQUAL 0)
        message(FATAL_ERROR "tools/py/freeze_str.py failed")
    endif()
    file(GLOB_RECURSE T5_FROZEN_PY "${FROZEN_DIR}/*.py" "${MICROPY_DIR}/extmod/asyncio/*.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${T5_ROOT}/tools/py/freeze_str.py" ${T5_FROZEN_PY})
    # genbind.py reads the permission table through abi.py and t5const.py.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GENBIND} "${T5_ROOT}/tools/abi/abi.py"
                 "${T5_ROOT}/tools/t5/t5const.py" "${T5_ROOT}/firmware/components/t5_pkg/manifest.c"
                 "${T5_ROOT}/sdk/abi/api_v1.sym" "${CMAKE_CURRENT_LIST_DIR}/bindings.txt" ${T5_SDK_HEADERS})
endif()

idf_component_register(
    SRCS ${MICROPY_SOURCE_PY} ${T5_PY_EXTMOD} ${T5_PY_SHARED} ${MICROPY_DIR}/shared/runtime/gchelper_rv32i.s ${T5_PY_PORT}
         ${BIND_DIR}/frozen_content.c
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${MICROPY_DIR}" "${BIND_DIR}" "${CMAKE_BINARY_DIR}"
    REQUIRES t5_core lvgl__lvgl
    PRIV_REQUIRES t5_app t5_pkg t5_ui md t5_ports t5_power t5_battery t5_time t5_audio t5_imu t5_usb t5_hid t5_net t5_shell
                  t5_storage esp_timer esp_hw_support heap esp_app_format esp_rom freertos log
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(MICROPY_TARGET ${COMPONENT_LIB})
    # The qstr scan preprocesses the sources with everything they include,
    # IDF and t5 headers too: take the include paths and definitions from
    # the target, transitive usage requirements included.
    set(_inc "$<TARGET_PROPERTY:${COMPONENT_LIB},INCLUDE_DIRECTORIES>")
    set(_def "$<TARGET_PROPERTY:${COMPONENT_LIB},COMPILE_DEFINITIONS>")
    set(MICROPY_CPP_FLAGS
        "$<$<BOOL:${_inc}>:-I$<JOIN:${_inc},$<SEMICOLON>-I>>"
        "$<$<BOOL:${_def}>:-D$<JOIN:${_def},$<SEMICOLON>-D>>")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE T5_PY_BUILD=1)
    # Upstream code under IDF's warning set.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-missing-field-initializers
                           -Wno-unused-variable -Wno-sign-compare -Wno-maybe-uninitialized -Wno-clobbered
                           -Wno-error=format -Wno-format-truncation -O2)
    include(${MICROPY_DIR}/py/mkrules.cmake)

    # Containment check (plan 8.2): fail the build if a module outside the
    # allowlist is registered.
    set(MODCHECK "${CMAKE_CURRENT_BINARY_DIR}/modules.txt")
    add_custom_command(
        OUTPUT ${MODCHECK}
        COMMAND ${python} "${T5_ROOT}/tools/py/check_modules.py" ${MICROPY_MODULEDEFS}
                "${CMAKE_CURRENT_LIST_DIR}/modules_allowed.txt" "${CMAKE_CURRENT_LIST_DIR}/include/t5_py/containment.h"
                ${MODCHECK}
        DEPENDS ${MICROPY_MODULEDEFS} "${CMAKE_CURRENT_LIST_DIR}/modules_allowed.txt" "${T5_ROOT}/tools/py/check_modules.py"
                "${CMAKE_CURRENT_LIST_DIR}/include/t5_py/containment.h"
        VERBATIM)
    add_custom_target(t5_py_modcheck DEPENDS ${MODCHECK})
    add_dependencies(${COMPONENT_LIB} t5_py_modcheck)
endif()
