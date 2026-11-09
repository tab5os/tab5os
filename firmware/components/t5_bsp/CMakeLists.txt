include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_BSP_PORTABLE_SRCS} "board_hw.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core t5_pins
    PRIV_REQUIRES t5_i2c esp_driver_gpio
)
