# t5_app's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_APP_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/tracker.c
    ${CMAKE_CURRENT_LIST_DIR}/crashguard.c
    ${CMAKE_CURRENT_LIST_DIR}/elfsym.c
    ${CMAKE_CURRENT_LIST_DIR}/abi.c
    ${CMAKE_CURRENT_LIST_DIR}/energy.c
    ${CMAKE_CURRENT_LIST_DIR}/isolate.c
    ${CMAKE_CURRENT_LIST_DIR}/fspolicy.c
    ${CMAKE_CURRENT_LIST_DIR}/session.c)
