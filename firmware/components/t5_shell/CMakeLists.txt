# Shell (plan 6.2-6.4): launcher, status bar, quick settings, keyboard and
# gestures. The UI files call the OS only through shell_sys.h, so the
# simulator builds them unchanged with sim/sim_shell_sys.c.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_SHELL_PORTABLE_SRCS} ${T5_SHELL_LVGL_SRCS} "shell_sys_idf.c" "app_icon.c" "accent.c" "activity.c" "ambient.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core md
    PRIV_REQUIRES t5_ui t5_app t5_pkg t5_storage t5_battery t5_net t5_power t5_time t5_display t5_audio t5_camera t5_usb t5_ota
                  lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
