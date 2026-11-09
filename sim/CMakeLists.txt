# Tab5 OS simulator (plan 4.1): LVGL (the firmware's pinned version) on
# SDL at 1280x720 / 720x1280, the same md, shell and t5_ui sources as the
# firmware (each component's portable.cmake lists them), and mocked
# services. Build and run: tools/t5/t5 sim
cmake_minimum_required(VERSION 3.22)
project(t5sim C)
set(CMAKE_C_STANDARD 11)

include(FetchContent)
set(LVGL_LOCAL ${CMAKE_CURRENT_LIST_DIR}/../firmware/managed_components/lvgl__lvgl)
if(EXISTS ${LVGL_LOCAL}/lvgl.h)
    set(LVGL_DIR ${LVGL_LOCAL})                  # same tree the firmware uses
else()
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../firmware/components/t5_ui/idf_component.yml lvgl_pin
         REGEX "^ *lvgl/lvgl: *\"[0-9.]+\"")
    string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" lvgl_pin "${lvgl_pin}")
    FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v${lvgl_pin} GIT_SHALLOW TRUE)
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
include(${FW}/md/portable.cmake)
include(${FW}/t5_ui/portable.cmake)
include(${FW}/t5_shell/portable.cmake)
add_executable(t5sim
    main.c sim_ui.c sim_services.c sim_shell_sys.c png.c
    ${T5_SHELL_PORTABLE_SRCS} ${T5_SHELL_LVGL_SRCS}
    ${T5_MD_PORTABLE_SRCS} ${T5_MD_LVGL_SRCS} ${T5_MD_ASSET_SRCS}
    ${T5_UI_PORTABLE_SRCS} ${T5_UI_LVGL_SRCS})
target_include_directories(t5sim PRIVATE ${FW}/md ${FW}/md/include ${FW}/t5_ui ${FW}/t5_ui/include
    ${FW}/t5_core/include ${FW}/t5_bsp/include ${FW}/t5_battery/include ${FW}/t5_app/include ${FW}/t5_pkg/include ${FW}/t5_storage/include
    ${FW}/t5_shell ${FW}/t5_shell/include
    ${LVGL_DIR}/src)  # LVGL private headers (md_freeze.c), as on the device
target_compile_options(t5sim PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
target_link_libraries(t5sim PRIVATE lvgl)
