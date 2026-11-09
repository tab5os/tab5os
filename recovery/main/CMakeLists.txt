idf_component_register(
    SRCS "recovery.c"
    PRIV_REQUIRES t5_core t5_pins t5_i2c t5_bsp t5_display t5_input t5_storage t5_ota lvgl__lvgl
                  nvs_flash app_update esp_partition espcoredump esp_system esp_timer console
)
