idf_component_register(
    SRCS "bmi270.c" "bmi270_config.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_pins
)
