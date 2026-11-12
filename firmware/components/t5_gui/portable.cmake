# t5_gui has no sources that build off the device: it is LVGL through and
# through. Listing it puts its include/ on the host tests' path, where
# test_t5ui.c checks that the app-facing header needs no LVGL at all.
set(T5_GUI_PORTABLE_SRCS)
