# USB (plan 7.7, 7.8): USB-A host classes and the USB-C role.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_USB_PORTABLE_SRCS} "usb.c" "usb_midi.c" "usbc.c" "usbdev.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core t5_storage
    PRIV_REQUIRES t5_audio t5_power fatfs vfs esp_timer heap esp_hal_usb soc
                  espressif__usb espressif__usb_host_hid espressif__usb_host_msc espressif__usb_host_cdc_acm
                  espressif__usb_host_uvc espressif__esp_tinyusb
)

