# Tab5 OS bootloader (plan 9.2): IDF's second stage plus the boot-attempt
# counter that sends the device to the recovery image after three failed
# boots. The counter format is shared with t5_ota (bootctl.h).
idf_component_register(SRCS "bootloader_start.c"
                       INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/t5_ota/include"
                       REQUIRES bootloader bootloader_support esp_rom spi_flash)

idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")
