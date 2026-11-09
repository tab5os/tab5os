# t5_imu's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_IMU_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/bmi270.c
    ${CMAKE_CURRENT_LIST_DIR}/bmi270_config.c)
