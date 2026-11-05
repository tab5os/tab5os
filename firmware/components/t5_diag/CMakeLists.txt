idf_component_register(
    SRCS "t5_diag.c" "diag_sys.c" "diag_i2c.c" "diag_exp.c" "diag_dev.c" "diag_bench.c" "diag_log.c" "diag_elf.c" "diag_power.c" "diag_net.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES console
    PRIV_REQUIRES t5_core t5_pins t5_i2c t5_bsp t5_time t5_imu t5_storage t5_display t5_input t5_power t5_battery t5_net lwip
                  esp_psram esp_timer esp_hw_support app_update spi_flash efuse esp_system esp_driver_gpio esp_pm
)

# Bench check 11: build elfbench.elf with the target compiler and embed it.
# Position-independent, no libc, entry app_main: the same shape as a Phase 5 app.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(STRINGS "${CMAKE_BINARY_DIR}/toolchain/cflags" _t5_arch REGEX "^-m")
    set(_elf_src "${CMAKE_CURRENT_LIST_DIR}/elfbench/elf_entry.c")
    set(_elf_out "${CMAKE_CURRENT_BINARY_DIR}/elfbench.elf")
    add_custom_command(
        OUTPUT ${_elf_out}
        COMMAND ${CMAKE_C_COMPILER} ${_t5_arch} -O2 -fPIC -shared -nostdlib -nostartfiles
                -e app_main -ffunction-sections -fdata-sections -Wl,--gc-sections -s
                -o ${_elf_out} ${_elf_src}
        DEPENDS ${_elf_src} "${CMAKE_CURRENT_LIST_DIR}/elfbench/bench_kernel.h"
        COMMENT "Building elfbench.elf"
        VERBATIM)
    add_custom_target(t5_elfbench DEPENDS ${_elf_out})
    target_add_binary_data(${COMPONENT_LIB} ${_elf_out} BINARY DEPENDS t5_elfbench)
endif()
