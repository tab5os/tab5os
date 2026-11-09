include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_STORAGE_PORTABLE_SRCS} "storage_hw.c" "secret.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES mbedtls efuse esp_hw_support fatfs sdmmc esp_driver_sdmmc vfs esp_timer heap
)
