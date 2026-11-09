# t5_battery's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_BATTERY_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/ina226.c
    ${CMAKE_CURRENT_LIST_DIR}/gauge.c
    ${CMAKE_CURRENT_LIST_DIR}/policy.c
    ${CMAKE_CURRENT_LIST_DIR}/history.c
    ${CMAKE_CURRENT_LIST_DIR}/battery.c)
