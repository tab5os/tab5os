# HID devices the Tab5 plays (descriptors, reports, text to keys) for USB-C
# device mode and Bluetooth LE HID. Pure: no IDF deps beyond t5_core's types.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(SRCS ${T5_HID_PORTABLE_SRCS} INCLUDE_DIRS "include" REQUIRES t5_core)
