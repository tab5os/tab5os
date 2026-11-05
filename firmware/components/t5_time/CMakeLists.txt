idf_component_register(
    SRCS "rtc.c" "t5_time.c" "t5_time_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_pins
)
