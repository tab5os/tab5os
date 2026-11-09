include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(
    SRCS ${T5_CORE_PORTABLE_SRCS} "t5_event_idf.c" "t5_os_idf.c" "t5_sbfix_idf.c" "t5_logring_idf.c" "t5_fault_idf.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES esp_timer heap freertos log riscv spi_flash esp_hw_support esp_system hal soc
)

# ESP-IDF v6.1 double-frees stream buffers made WithCaps (t5_sbfix_idf.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=vStreamBufferGenericDeleteWithCaps"
                      "-u __wrap_vStreamBufferGenericDeleteWithCaps")

# App faults are recovered instead of panicking when the app is to blame
# (t5_fault_idf.c, fault.h).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xt_unhandled_exception"
                      "-Wl,--wrap=panicHandler" "-u __wrap_panicHandler"
                      "-u __wrap_xt_unhandled_exception")

# Third-party drivers' long-lived tasks can get PSRAM stacks (t5_os_idf.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xTaskCreatePinnedToCore"
                      "-u __wrap_xTaskCreatePinnedToCore")
