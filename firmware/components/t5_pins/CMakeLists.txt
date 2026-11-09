# Pin map and the board table. Header-only data plus pure lookups: no IDF deps.
include(${CMAKE_CURRENT_LIST_DIR}/portable.cmake)
idf_component_register(SRCS ${T5_PINS_PORTABLE_SRCS} INCLUDE_DIRS "include")
