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
