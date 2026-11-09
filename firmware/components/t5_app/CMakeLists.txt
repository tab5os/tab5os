# App runtime (plan 5.4-5.12). The ABI table it resolves imports against is
# t5_abi's, registered at boot (t5_abi_set_table).
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_APP_PORTABLE_SRCS} "lifecycle.c" "wrappers.c" "perm.c" "anr.c" "apps_hw.c" "picker.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # wrappers.h (bound by t5_abi) has the signatures of the UI, USB and
    # socket functions it stands in for.
    REQUIRES t5_core t5_pkg t5_ui t5_usb lwip lvgl__lvgl freertos
    PRIV_REQUIRES md t5_ports t5_power t5_storage t5_time t5_battery
                  espressif__elf_loader espcoredump spi_flash esp_system heap esp_timer
)
