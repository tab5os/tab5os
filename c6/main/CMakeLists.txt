idf_component_register(SRCS "main.c" "monitor.c" INCLUDE_DIRS "." REQUIRES esp_hosted
                       PRIV_REQUIRES nvs_flash app_update esp_event esp_wifi esp_timer esp_ringbuf)
