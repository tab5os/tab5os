# t5_power's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_POWER_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/arbiter.c
    ${CMAKE_CURRENT_LIST_DIR}/wake_reason.c
    ${CMAKE_CURRENT_LIST_DIR}/rail.c
    ${CMAKE_CURRENT_LIST_DIR}/wakelock.c
    ${CMAKE_CURRENT_LIST_DIR}/suspend.c
    ${CMAKE_CURRENT_LIST_DIR}/autosave.c
    ${CMAKE_CURRENT_LIST_DIR}/power.c)
