# t5ui: the app UI API (API level 2). t5ui_core.c checks calls; a backend
# draws them. t5ui_lvgl9.c is the backend on LVGL 9 and md, the only place
# apps' UI calls meet them, so the renderer can change without breaking apps.
idf_component_register(SRCS "t5ui_core.c" "t5ui_lvgl9.c" INCLUDE_DIRS "include" REQUIRES t5_core
                       PRIV_REQUIRES t5_app t5_ui md lvgl__lvgl)
