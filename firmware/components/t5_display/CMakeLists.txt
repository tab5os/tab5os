include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_DISPLAY_PORTABLE_SRCS} "display_hw.c" "shot.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core t5_bsp t5_storage
    PRIV_REQUIRES t5_pins mbedtls esp_lcd esp_driver_ledc esp_hw_support esp_mm
)

# Backlight off first thing in a panic (display_hw.c, __wrap_esp_panic_handler).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
