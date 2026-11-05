idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES t5_core t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input t5_diag app_update
)
