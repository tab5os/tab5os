idf_component_register(
    SRCS "t5_err.c" "t5_event.c" "t5_event_names.c" "t5_event_idf.c" "t5_os_idf.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES esp_timer
)
