# Tab5 OS simulator (plan 4.1): LVGL 9.6 (the firmware's pinned version) on
# SDL at 1280x720 / 720x1280, the same md sources as the firmware, and mocked
# services. Build and run: tools/t5/t5 sim
cmake_minimum_required(VERSION 3.22)
project(t5sim C)
set(CMAKE_C_STANDARD 11)

include(FetchContent)
set(LVGL_LOCAL ${CMAKE_CURRENT_LIST_DIR}/../firmware/managed_components/lvgl__lvgl)
if(EXISTS ${LVGL_LOCAL}/lvgl.h)
    set(LVGL_DIR ${LVGL_LOCAL})                  # same tree the firmware uses
else()
    FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.6.0 GIT_SHALLOW TRUE)
    FetchContent_Populate(lvgl)
    set(LVGL_DIR ${lvgl_SOURCE_DIR})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

file(GLOB_RECURSE LVGL_SRCS ${LVGL_DIR}/src/*.c)
list(FILTER LVGL_SRCS EXCLUDE REGEX "/(thorvg|espressif|nxp|renesas|sifli|nema_gfx|vg_lite|eve|opengles|dma2d|arm2d|helium|neon)/")
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_LIST_DIR} ${LVGL_DIR} ${LVGL_DIR}/include/lvgl)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2 m)
target_compile_options(lvgl PRIVATE -w)

set(FW ${CMAKE_CURRENT_LIST_DIR}/../firmware/components)
file(GLOB MD_ASSETS ${FW}/md/assets/fonts/*.c ${FW}/md/assets/shadows/*.c)
add_executable(t5sim
    main.c sim_ui.c sim_services.c png.c
    ${FW}/md/md_tokens.c ${FW}/md/md_theme.c ${FW}/md/md_text.c ${FW}/md/md_ripple.c
    ${FW}/md/md_shadow.c ${FW}/md/md_widgets.c ${FW}/md/md_gate.c ${MD_ASSETS}
    ${FW}/t5_ui/rotation.c ${FW}/t5_ui/orient.c ${FW}/t5_ui/fbtrack.c)
target_include_directories(t5sim PRIVATE ${FW}/md ${FW}/md/include ${FW}/t5_ui/include ${FW}/t5_core/include)
target_compile_options(t5sim PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(t5sim PRIVATE lvgl)
