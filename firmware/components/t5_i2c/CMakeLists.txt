idf_component_register(
    SRCS "t5_i2c.c" "t5_i2c_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_pins esp_driver_i2c
)
