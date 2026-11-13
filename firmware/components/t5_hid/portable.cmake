# t5_hid's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests. All of it.
set(T5_HID_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/hid_desc.c
    ${CMAKE_CURRENT_LIST_DIR}/hid_keys.c)
