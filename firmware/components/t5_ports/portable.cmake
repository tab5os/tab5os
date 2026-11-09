# t5_ports's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_PORTS_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/ports_table.c
    ${CMAKE_CURRENT_LIST_DIR}/broker.c
    ${CMAKE_CURRENT_LIST_DIR}/ports.c
    ${CMAKE_CURRENT_LIST_DIR}/modbus.c
    ${CMAKE_CURRENT_LIST_DIR}/profiles.c)
