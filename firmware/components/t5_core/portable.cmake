# t5_core's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_CORE_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/t5_err.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_crc.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_event.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_event_names.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_res.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_io_hold.c)
