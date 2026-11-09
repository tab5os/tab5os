include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_IMU_PORTABLE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES t5_core
    PRIV_REQUIRES t5_i2c t5_pins
)
