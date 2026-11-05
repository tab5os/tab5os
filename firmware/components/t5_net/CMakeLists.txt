idf_component_register(
    SRCS "netcfg.c" "c6img.c" "net.c" "ble.c" "ble_util.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_bsp t5_pins t5_power t5_time esp_wifi esp_netif esp_event nvs_flash esp_partition
                  esp_driver_gpio lwip bt espressif__esp_hosted
)
