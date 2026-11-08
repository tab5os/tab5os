# OS updates, boot health and safe mode (plan 9.1, 9.3). The trusted update
# keys come from firmware/ota_keys/*.pem (tools/ota/otakeys.py), or from the
# directory in T5_OTA_KEYS_DIR.
set(T5_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(KEYS_C "${CMAKE_CURRENT_BINARY_DIR}/ota_keys.c")
if(NOT "$ENV{T5_OTA_KEYS_DIR}" STREQUAL "")
    set(KEYS_DIR "$ENV{T5_OTA_KEYS_DIR}")
else()
    set(KEYS_DIR "${T5_ROOT}/firmware/ota_keys")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(python PYTHON)
    execute_process(COMMAND ${python} "${T5_ROOT}/tools/ota/otakeys.py" "${KEYS_DIR}" "${KEYS_C}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "tools/ota/otakeys.py failed")
    endif()
    file(GLOB KEY_FILES "${KEYS_DIR}/*.pem")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${T5_ROOT}/tools/ota/otakeys.py" "${KEYS_DIR}" ${KEY_FILES})
endif()
idf_component_register(
    SRCS "ota.c" "${KEYS_C}"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES t5_core
    PRIV_REQUIRES app_update esp_partition esp_app_format bootloader_support esp_http_client mbedtls
                  esp_timer esp_system efuse hal
)
