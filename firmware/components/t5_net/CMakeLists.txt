idf_component_register(
    SRCS "netcfg.c" "c6img.c" "net.c" "ble.c" "ble_util.c" "hosted_compat.c"
         "netproto.c" "sock.c" "http.c" "ws.c" "mqtt.c" "httpd.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_storage t5_bsp t5_pins t5_power t5_time esp_wifi esp_netif esp_event nvs_flash esp_partition
                  esp_driver_gpio lwip bt espressif__esp_hosted esp-tls mbedtls esp_timer espressif__mqtt esp_mm heap
)

# esp_hosted host vs the factory C6 firmware: keep SDIO RX staging dynamic
# when the C6 advertises no capacity (hosted_compat.c).
# Wi-Fi packet buffers from PSRAM instead of internal RAM (hosted_compat.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_bus_sdio_set_rx_staging_target"
                      "-u __wrap_eh_host_bus_sdio_set_rx_staging_target"
                      "-Wl,--wrap=eh_host_port_dma_alloc" "-u __wrap_eh_host_port_dma_alloc")
