# t5ui: the app UI API (API level 2). The only place apps' UI calls meet
# LVGL and md, so the renderer can change without breaking apps.
idf_component_register(SRCS "t5ui.c" INCLUDE_DIRS "include" REQUIRES t5_core PRIV_REQUIRES t5_app t5_ui md lvgl__lvgl)
