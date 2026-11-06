# External ports: named port table, pin broker, drivers, Modbus RTU, module profiles.
# Logic files build on the host too (test/host); ports_hw.c is the device layer.
idf_component_register(
    SRCS "ports_table.c" "broker.c" "ports.c" "modbus.c" "profiles.c" "ports_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_pins t5_i2c esp_driver_i2c esp_driver_uart esp_driver_spi esp_driver_gpio
                  esp_driver_ledc esp_adc
)
