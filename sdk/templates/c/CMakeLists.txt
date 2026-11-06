cmake_minimum_required(VERSION 3.22)
if(NOT T5_SDK)
    set(T5_SDK ${CMAKE_CURRENT_LIST_DIR}/../..)   # inside the repo; `t5 new` writes the real path
endif()
set(CMAKE_TOOLCHAIN_FILE ${T5_SDK}/cmake/toolchain-t5.cmake)
project(hello C)
include(${T5_SDK}/cmake/t5_app.cmake)
t5_app(hello SRCS main.c MANIFEST manifest.toml)
