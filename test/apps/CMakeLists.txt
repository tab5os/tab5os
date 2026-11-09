# Test apps for the Phase 5 exit criteria. Build: tools/t5/t5 apps
cmake_minimum_required(VERSION 3.22)
set(T5_SDK ${CMAKE_CURRENT_LIST_DIR}/../../sdk)
set(CMAKE_TOOLCHAIN_FILE ${T5_SDK}/cmake/toolchain-t5.cmake)
project(t5_test_apps C)
include(${T5_SDK}/cmake/t5_app.cmake)
t5_app(leaky SRCS leaky/main.c MANIFEST leaky/manifest.toml)
t5_app(crasher SRCS crasher/main.c MANIFEST crasher/manifest.toml)
t5_app(hang SRCS hang/main.c MANIFEST hang/manifest.toml)
t5_app(fscope SRCS fscope/main.c MANIFEST fscope/manifest.toml)
t5_app(fsshared SRCS fscope/main.c MANIFEST fscope/shared.toml)
t5_app(netleak SRCS netleak/main.c MANIFEST netleak/manifest.toml)
t5_app(nonet SRCS nonet/main.c MANIFEST nonet/manifest.toml)
t5_app(hwcrash SRCS hwcrash/main.c MANIFEST hwcrash/manifest.toml)
# The C template, filled in as `t5 new Hello` would (tools/t5/t5dev.py).
set(APP_ID com.example.hello)
set(APP_NAME Hello)
foreach(f main.c manifest.toml)
    configure_file(${T5_SDK}/templates/c/${f} ${CMAKE_CURRENT_BINARY_DIR}/hello/${f} @ONLY)
endforeach()
file(RELATIVE_PATH hello ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/hello)   # t5_app wants it relative
t5_app(hello SRCS ${hello}/main.c MANIFEST ${hello}/manifest.toml)

# Python test app: nothing to compile, the folder is the package.
file(GLOB fspy_files ${CMAKE_CURRENT_SOURCE_DIR}/fspy/*)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fspy.t5app
    COMMAND ${Python3_EXECUTABLE} ${T5_ROOT}/tools/t5/t5pack.py ${CMAKE_CURRENT_SOURCE_DIR}/fspy
            -o ${CMAKE_CURRENT_BINARY_DIR}/fspy.t5app
    DEPENDS ${fspy_files} ${T5_ROOT}/tools/t5/t5pack.py
    COMMENT "Packing fspy.t5app" VERBATIM)
add_custom_target(fspy_pkg ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/fspy.t5app)
