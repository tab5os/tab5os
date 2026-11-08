# Coding standard (docs/coding-standard.md): compiler side.
#
# ESP-IDF already builds with -Wall -Wextra -Werror, but downgrades the
# -Wextra and unused-* warnings to plain warnings. Our own components get
# them back as errors (zero warnings). The strict components (the ones that
# must never fail, and the parsers that read untrusted input) also get the
# extra warnings below and, with -DT5_ANALYZE=ON, GCC's static analyzer.
#
# Call after project(). Components that aren't part of this build are skipped.

set(T5_STD_FLAGS
    -Werror=extra
    -Werror=unused-function
    -Werror=unused-variable
    -Werror=unused-but-set-variable
    -Werror=deprecated-declarations)

set(T5_STRICT_FLAGS
    -Wshadow                 # rule 6: no name hides another
    -Wformat=2               # rule 10
    -Wvla                    # rules 1 and 3: stack use known at compile time
    -Wstack-usage=3072       # rule 1: no frame big enough to threaten a task stack
    -Werror=unused-result)   # rule 7: T5_MUST_CHECK results are checked

option(T5_ANALYZE "Run GCC's static analyzer (-fanalyzer) on the strict components" OFF)

function(t5_apply_standard)
    cmake_parse_arguments(T5S "" "" "COMPONENTS;STRICT" ${ARGN})
    idf_build_get_property(built BUILD_COMPONENTS)
    foreach(c ${T5S_COMPONENTS} ${T5S_STRICT})
        if(NOT c IN_LIST built)
            continue()
        endif()
        idf_component_get_property(lib ${c} COMPONENT_LIB)
        get_target_property(type ${lib} TYPE)
        if(type STREQUAL "INTERFACE_LIBRARY")
            continue()
        endif()
        target_compile_options(${lib} PRIVATE ${T5_STD_FLAGS})
        if(c IN_LIST T5S_STRICT)
            target_compile_options(${lib} PRIVATE ${T5_STRICT_FLAGS})
            if(T5_ANALYZE)
                target_compile_options(${lib} PRIVATE -fanalyzer)
            endif()
        endif()
    endforeach()
endfunction()
