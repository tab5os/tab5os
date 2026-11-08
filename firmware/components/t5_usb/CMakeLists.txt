# USB (plan 7.7, 7.8): USB-A host classes and the USB-C role.
idf_component_register(
    SRCS "usb.c" "usb_midi.c" "usb_input.c" "usbc.c" "usb_proto.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES t5_power t5_storage t5_ui md lvgl__lvgl fatfs vfs esp_timer heap esp_hal_usb soc
                  espressif__usb espressif__usb_host_hid espressif__usb_host_msc espressif__usb_host_cdc_acm
                  espressif__usb_host_uvc espressif__esp_tinyusb
)

