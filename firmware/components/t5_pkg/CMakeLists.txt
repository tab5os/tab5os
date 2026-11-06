idf_component_register(
    SRCS "inflate.c" "zip.c" "toml.c" "manifest.c" "pkg.c"
    INCLUDE_DIRS "include"
    REQUIRES t5_core
)
