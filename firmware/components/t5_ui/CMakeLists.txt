include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_UI_PORTABLE_SRCS} ${T5_UI_LVGL_SRCS} "ui.c" "ui_hw.c" "hid_input.c" "lv_mem_t5.c" "ppa_img_t5.c" "ppa_fill_t5.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core t5_bsp lvgl__lvgl
    PRIV_REQUIRES md t5_display t5_input t5_imu t5_power t5_storage t5_usb esp_driver_ppa esp_mm heap
)

# LVGL's allocator lives here (LV_USE_CUSTOM_MALLOC); lvgl references it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

# Faded and ARGB8888 images on the PPA (ppa_img_t5.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_draw_ppa_img" "-u __wrap_lv_draw_ppa_img")

# Translucent fills (scrims) on the PPA's blender (ppa_fill_t5.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_draw_ppa_fill" "-u __wrap_lv_draw_ppa_fill")
