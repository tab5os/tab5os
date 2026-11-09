# Developer mode (plan 8.5): the authenticated HTTP endpoint for the t5 CLI.
idf_component_register(
    SRCS "dev.c" "control.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core
    PRIV_REQUIRES esp_http_server esp_app_format esp_hw_support heap mbedtls md lvgl__lvgl t5_app t5_display t5_ota t5_pkg
                  t5_power t5_py t5_shell t5_storage t5_ui
)
