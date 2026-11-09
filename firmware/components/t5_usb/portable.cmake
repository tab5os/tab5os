# t5_usb's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_USB_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/usb_proto.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_hid.c)
