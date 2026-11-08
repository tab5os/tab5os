# Camera (plan 7.4, 7.5): esp_video (SC202CS on MIPI CSI, ISP), PPA preview,
# hardware JPEG and H.264.
idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core
    PRIV_REQUIRES t5_bsp t5_i2c t5_pins t5_power t5_ui lvgl__lvgl esp_driver_ppa esp_driver_jpeg esp_driver_i2c
                  esp_mm heap esp_timer vfs espressif__esp_video espressif__esp_h264 espressif__esp_cam_sensor
                  espressif__esp_sccb_intf
)
