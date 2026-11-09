# t5_net's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_NET_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/netcfg.c
    ${CMAKE_CURRENT_LIST_DIR}/c6img.c
    ${CMAKE_CURRENT_LIST_DIR}/ble_util.c
    ${CMAKE_CURRENT_LIST_DIR}/netproto.c)
