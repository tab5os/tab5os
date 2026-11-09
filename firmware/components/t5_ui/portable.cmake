# t5_ui's sources that build off the device, as absolute paths, for the
# component itself, the simulator and the host tests:
#   T5_UI_PORTABLE_SRCS  plain C (rotation maths, the orientation filter,
#                        framebuffer damage tracking)
#   T5_UI_LVGL_SRCS      LVGL and md over t5_ui.h (the simulator implements t5_ui.h in sim_ui.c)
set(T5_UI_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/rotation.c
    ${CMAKE_CURRENT_LIST_DIR}/orient.c
    ${CMAKE_CURRENT_LIST_DIR}/fbtrack.c)
set(T5_UI_LVGL_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/ui_md.c)
