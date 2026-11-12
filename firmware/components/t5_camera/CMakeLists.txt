# Camera (plan 7.4, 7.5): esp_video (SC202CS on MIPI CSI, ISP), the preview
# as a video plane, hardware JPEG (stills, Motion JPEG in AVI) and H.264.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_CAMERA_PORTABLE_SRCS} "camera.c" "h264_ram.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core
    PRIV_REQUIRES t5_bsp t5_i2c t5_pins t5_power lvgl__lvgl esp_driver_ppa esp_driver_jpeg esp_driver_i2c
                  esp_mm heap esp_timer vfs espressif__esp_video espressif__esp_h264 espressif__esp_cam_sensor
                  espressif__esp_sccb_intf
)

# The encoder's allocator hands out the lent UI block (h264_ram.c), and
# esp_h264 frees its reference buffer with heap_caps_free: in esp_h264 alone
# that goes to t5_camera_h264_free, which keeps the lent block and passes
# everything else on. esp_h264's target may not exist yet here, so the
# define is added once the project has every component.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_h264_aligned_calloc")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(h264_lib espressif__esp_h264 COMPONENT_LIB)
    # A deferred call's arguments are expanded when it runs: EVAL fixes the name now.
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL
        target_compile_definitions [[${h264_lib}]] PRIVATE heap_caps_free=t5_camera_h264_free)")
endif()
