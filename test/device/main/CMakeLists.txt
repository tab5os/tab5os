include(${CMAKE_CURRENT_LIST_DIR}/../components.cmake)
idf_component_register(
    SRCS "test_main.c" "test_services.c" "test_phase23.c"
    WHOLE_ARCHIVE   # TEST_CASE registers through constructors; keep them
    PRIV_REQUIRES unity ${T5_DEVICE_TEST_COMPONENTS}
)
