idf_component_register(
    SRCS "ina226.c" "gauge.c" "policy.c" "battery.c" "battery_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_pins t5_bsp t5_power t5_display t5_imu t5_storage t5_time
                  esp_driver_tsens esp_driver_usb_serial_jtag
)
