# t5_pkg's sources that build off the device (against test/host/mocks), as
# absolute paths, for the component and the host tests.
set(T5_PKG_PORTABLE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/inflate.c
    ${CMAKE_CURRENT_LIST_DIR}/zip.c
    ${CMAKE_CURRENT_LIST_DIR}/toml.c
    ${CMAKE_CURRENT_LIST_DIR}/manifest.c
    ${CMAKE_CURRENT_LIST_DIR}/png.c
    ${CMAKE_CURRENT_LIST_DIR}/pkg.c)
