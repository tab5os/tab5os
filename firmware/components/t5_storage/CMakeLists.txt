idf_component_register(
    SRCS "storage.c" "storage_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc vfs esp_hw_support esp_timer heap
)
