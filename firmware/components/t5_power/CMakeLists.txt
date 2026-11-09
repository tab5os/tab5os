include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_POWER_PORTABLE_SRCS} "power_hw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_bsp t5_pins t5_time t5_storage esp_pm esp_hw_support esp_system
)
