# t5_shell's sources that build off the device, as absolute paths, for the
# component itself, the simulator and the host tests. They reach the OS
# only through shell_sys.h (shell_sys_idf.c on the device).
#   T5_SHELL_PORTABLE_SRCS  plain C (recents, names, the battery glyph, clock text)
#   T5_SHELL_LVGL_SRCS      the launcher, status bar, app and gesture bars, quick settings, keyboard, gestures
set(T5_SHELL_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/shell_logic.c)
set(T5_SHELL_LVGL_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/shell.c
    ${CMAKE_CURRENT_LIST_DIR}/statusbar.c
    ${CMAKE_CURRENT_LIST_DIR}/bars.c
    ${CMAKE_CURRENT_LIST_DIR}/standby.c
    ${CMAKE_CURRENT_LIST_DIR}/qs.c
    ${CMAKE_CURRENT_LIST_DIR}/keyboard.c
    ${CMAKE_CURRENT_LIST_DIR}/gesture.c
    ${CMAKE_CURRENT_LIST_DIR}/launcher.c)
