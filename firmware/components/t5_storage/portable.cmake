# t5_storage's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_STORAGE_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/storage.c
    ${CMAKE_CURRENT_LIST_DIR}/paths.c)
