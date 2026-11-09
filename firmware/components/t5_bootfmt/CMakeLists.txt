# Byte formats shared by the bootloader, the OS, the recovery image, host
# tests and tools/t5/t5const.py: the boot-control record (bootctl.h) and the
# start of an ESP-IDF app image (espimg.h). Headers only, no dependencies,
# so the bootloader can include them.
idf_component_register(INCLUDE_DIRS "include")
