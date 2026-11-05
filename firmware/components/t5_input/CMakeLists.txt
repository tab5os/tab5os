idf_component_register(
    SRCS "touch.c" "touch_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core t5_display
    PRIV_REQUIRES t5_i2c t5_pins esp_driver_gpio
)
