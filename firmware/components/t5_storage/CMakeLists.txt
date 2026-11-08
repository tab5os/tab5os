idf_component_register(
    SRCS "storage.c" "storage_hw.c" "secret.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES mbedtls efuse esp_hw_support fatfs sdmmc esp_driver_sdmmc vfs esp_hw_support esp_timer heap
)
