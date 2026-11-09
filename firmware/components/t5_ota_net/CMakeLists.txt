# OS updates from a URL (plan 9.1) as a source for t5_ota's install, apart
# from it so the recovery image doesn't link the network stack.
idf_component_register(
    SRCS "ota_url.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core t5_ota
    PRIV_REQUIRES t5_net
)
