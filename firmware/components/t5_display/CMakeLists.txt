idf_component_register(
    SRCS "display.c" "display_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_bsp t5_pins esp_lcd esp_driver_ledc esp_hw_support esp_mm
)

# Backlight off first thing in a panic (display_hw.c, __wrap_esp_panic_handler).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
