# App runtime (plan 5.4-5.12). abi_table.c is generated from the published
# symbol list, sdk/abi/api_v1.sym, by tools/abi/abi.py.
set(T5_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(ABI_SYM "${T5_ROOT}/sdk/abi/api_v1.sym")
set(ABI_TABLE "${CMAKE_CURRENT_BINARY_DIR}/abi_table.c")

idf_component_register(
    SRCS "tracker.c" "crashguard.c" "elfsym.c" "abi.c" "lifecycle.c" "wrappers.c" "perm.c" "anr.c"
         "launcher.c" "apps_hw.c" "${ABI_TABLE}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core t5_ports lvgl__lvgl freertos
    PRIV_REQUIRES md t5_ui t5_pkg t5_power t5_storage t5_time t5_battery t5_imu t5_net lwip
                  espressif__elf_loader espcoredump spi_flash esp_system heap
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    add_custom_command(
        OUTPUT ${ABI_TABLE}
        COMMAND ${python} ${T5_ROOT}/tools/abi/abi.py gen ${ABI_SYM} ${ABI_TABLE}
        DEPENDS ${ABI_SYM} ${T5_ROOT}/tools/abi/abi.py
        COMMENT "Generating the app ABI table from api_v1.sym"
        VERBATIM)
    add_custom_target(t5_abi_table DEPENDS ${ABI_TABLE})
    add_dependencies(${COMPONENT_LIB} t5_abi_table)
    # Exported LVGL functions must be in the image even if the OS never calls them.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-redundant-decls)
endif()
