# OS updates over the network (plan 9.1): a URL as a source for t5_ota's
# install, the daily check for a newer release, and keeping the C6 radio's
# firmware matched to the OS (c6.c). Apart from t5_ota so the
# recovery image doesn't link the network stack.
idf_component_register(
    SRCS "ota_url.c" "update.c" "c6.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../t5_ota"            # ota_rule.h: which release is an update
    REQUIRES t5_core t5_ota
    PRIV_REQUIRES t5_net t5_storage esp_app_format
)
# The C6 firmware this OS expects: c6/VERSION, the version c6/ builds.
set(C6_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../c6/VERSION")
file(STRINGS "${C6_VERSION_FILE}" T5_C6_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${C6_VERSION_FILE}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE T5_C6_VERSION="${T5_C6_VERSION}")
