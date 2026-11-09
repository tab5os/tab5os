# md's sources that build off the device, as absolute paths, for the
# component itself, the simulator and the host tests:
#   T5_MD_PORTABLE_SRCS  plain C (the MD2 tokens)
#   T5_MD_LVGL_SRCS      the widgets, theme and compositing, on LVGL
#   T5_MD_ASSET_SRCS     the generated fonts and baked shadows as committed
#                        (the image carries them deflated: CMakeLists.txt)
set(T5_MD_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/md_tokens.c)
set(T5_MD_LVGL_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/md_theme.c
    ${CMAKE_CURRENT_LIST_DIR}/md_text.c
    ${CMAKE_CURRENT_LIST_DIR}/md_ripple.c
    ${CMAKE_CURRENT_LIST_DIR}/md_shadow.c
    ${CMAKE_CURRENT_LIST_DIR}/md_widgets.c
    ${CMAKE_CURRENT_LIST_DIR}/md_controls.c
    ${CMAKE_CURRENT_LIST_DIR}/md_gate.c
    ${CMAKE_CURRENT_LIST_DIR}/md_freeze.c
    ${CMAKE_CURRENT_LIST_DIR}/md_zasset.c)
file(GLOB T5_MD_ASSET_SRCS ${CMAKE_CURRENT_LIST_DIR}/assets/fonts/*.c ${CMAKE_CURRENT_LIST_DIR}/assets/shadows/*.c)
