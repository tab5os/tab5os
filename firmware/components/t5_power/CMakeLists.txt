idf_component_register(
    SRCS "arbiter.c" "wake_reason.c" "rail.c" "wakelock.c" "suspend.c" "autosave.c" "power.c" "power_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_bsp t5_pins t5_display t5_input t5_imu t5_time t5_storage esp_pm esp_hw_support esp_system
)
