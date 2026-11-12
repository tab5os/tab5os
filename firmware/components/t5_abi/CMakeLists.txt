# The app ABI table, generated from sdk/abi/api_v1.sym by tools/abi/abi.py.
# A leaf: it depends on every service whose functions apps may call, so that
# t5_app (the loader and resolver) doesn't. main registers the table at boot.
set(T5_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(ABI_SYM "${T5_ROOT}/sdk/abi/api_v1.sym")
set(ABI_TABLE "${CMAKE_CURRENT_BINARY_DIR}/abi_table.c")

idf_component_register(
    SRCS "${ABI_TABLE}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${T5_ROOT}/sdk/include"
    PRIV_REQUIRES t5_core t5_app t5_pkg md t5_ui t5_power t5_storage t5_time t5_battery t5_imu t5_net t5_audio
                  t5_camera t5_hid t5_usb t5_shell t5_ports lvgl__lvgl freertos lwip
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    add_custom_command(
        OUTPUT ${ABI_TABLE}
        COMMAND ${python} ${T5_ROOT}/tools/abi/abi.py gen ${ABI_SYM} ${ABI_TABLE}
        # abi.py reads the permission table through t5const.py.
        DEPENDS ${ABI_SYM} ${T5_ROOT}/tools/abi/abi.py ${T5_ROOT}/tools/t5/t5const.py
                ${T5_ROOT}/firmware/components/t5_pkg/manifest.c
                ${T5_ROOT}/firmware/components/t5_pkg/include/t5_pkg/manifest.h
        COMMENT "Generating the app ABI table from api_v1.sym"
        VERBATIM)
    add_custom_target(t5_abi_table DEPENDS ${ABI_TABLE})
    add_dependencies(${COMPONENT_LIB} t5_abi_table)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-redundant-decls)
endif()
