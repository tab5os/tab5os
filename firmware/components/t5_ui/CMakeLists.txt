idf_component_register(
    SRCS "rotation.c" "orient.c" "fbtrack.c" "ui.c" "ui_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core lvgl__lvgl
    PRIV_REQUIRES t5_display t5_input t5_imu t5_power t5_storage esp_driver_ppa heap
)
