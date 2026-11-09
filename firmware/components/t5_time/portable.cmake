# t5_time's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_TIME_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/rtc.c
    ${CMAKE_CURRENT_LIST_DIR}/t5_time.c
    ${CMAKE_CURRENT_LIST_DIR}/tz.c)
