# Shell (plan 6.2-6.4): launcher, status bar, quick settings, keyboard and
# gestures. The UI files call the OS only through shell_sys.h, so the
# simulator builds them unchanged with sim/sim_shell_sys.c.
idf_component_register(
    SRCS "shell.c" "statusbar.c" "qs.c" "keyboard.c" "gesture.c" "launcher.c" "shell_sys_idf.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES md t5_ui t5_app t5_battery t5_net t5_power t5_time t5_display t5_audio t5_camera t5_usb lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
