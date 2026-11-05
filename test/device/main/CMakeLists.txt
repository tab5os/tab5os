idf_component_register(
    SRCS "test_main.c" "test_services.c"
    WHOLE_ARCHIVE   # TEST_CASE registers through constructors; keep them
    PRIV_REQUIRES unity t5_core t5_pins t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input
)
