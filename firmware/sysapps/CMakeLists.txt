# System apps (plan 6.5-6.9, 7.6), built in but written against the SDK.
idf_component_register(
    SRCS "sysapps.c" "nav.c" "settings.c" "files.c" "appmanager.c" "power.c" "ports.c" "camera.c" "terminal.c" "editor.c" "codeview.c" "photo.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "../../sdk/include"
    REQUIRES t5_core
    PRIV_REQUIRES t5_app t5_ui md t5_shell t5_py t5_dev t5_ota t5_audio t5_camera t5_usb t5_storage t5_power t5_battery t5_net
                  t5_time t5_imu t5_bsp t5_display t5_i2c t5_ports t5_pkg lvgl__lvgl freertos esp_app_format app_update
                  esp_psram spi_flash esp_hw_support esp_driver_jpeg
)

# UI strings are cut to fit on purpose.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
