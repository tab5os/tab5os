idf_component_register(
    SRCS "rotation.c" "orient.c" "fbtrack.c" "ui.c" "ui_hw.c" "lv_mem_t5.c" "ppa_img_t5.c" "ppa_fill_t5.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core lvgl__lvgl
    PRIV_REQUIRES t5_display t5_input t5_imu t5_power t5_storage esp_driver_ppa esp_mm heap
)

# The UI's internal draw bands double as the H.264 encoder's reference
# buffer while recording (ui.c, "Lending").
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_h264_aligned_calloc" "-Wl,--wrap=heap_caps_free")

# LVGL's allocator lives here (LV_USE_CUSTOM_MALLOC); lvgl references it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

# Faded and ARGB8888 images on the PPA (ppa_img_t5.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_draw_ppa_img" "-u __wrap_lv_draw_ppa_img")

# Translucent fills (scrims) on the PPA's blender (ppa_fill_t5.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lv_draw_ppa_fill" "-u __wrap_lv_draw_ppa_fill")
