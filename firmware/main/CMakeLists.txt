idf_component_register(
    SRCS "main.c"
    PRIV_REQUIRES t5_core t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input t5_diag t5_power t5_battery t5_net t5_ui t5_app t5_ports t5_pkg t5_audio t5_camera t5_usb t5_shell t5_py t5_dev t5_ota sysapps app_update
)
