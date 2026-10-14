# Standalone native (macOS/Linux) CMake project for host unit tests of the
# pure-logic modules (components/<comp>/*_core.c, no ESP-IDF includes).
# Not an ESP-IDF project. Run via scripts/test_host.sh.
#
# Adding a test (from a later bead): create test/host/<name>/test_<name>.c
# (Unity, with its own main()) and add to the list at the bottom:
#
#   embpet_add_host_test(
#       NAME     tgbot_core
#       SOURCES  tgbot/test_tgbot_core.c ${EMBPET_ROOT}/components/tgbot/tgbot_core.c
#       INCLUDES ${EMBPET_ROOT}/components/tgbot/include
#       WITH_CJSON)                     # optional: link cJSON
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "IDF_PATH is not set. Run '. scripts/export.sh' first "
                        "(or use scripts/test_host.sh, which does that).")
endif()
set(IDF_PATH "$ENV{IDF_PATH}")
set(UNITY_DIR "${IDF_PATH}/components/unity/unity/src")
set(CJSON_DIR "${IDF_PATH}/components/json/cJSON")
foreach(_f "${UNITY_DIR}/unity.c" "${CJSON_DIR}/cJSON.c")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "Missing ${_f}; check IDF_PATH='${IDF_PATH}'.")
    endif()
endforeach()

project(embpet_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(EMBPET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

enable_testing()

# Third-party code is built without -Werror; project code and tests with it.
add_library(unity STATIC "${UNITY_DIR}/unity.c")
target_include_directories(unity SYSTEM PUBLIC "${UNITY_DIR}")
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)   # cJSON numbers are double
target_compile_options(unity PRIVATE -w)

add_library(cjson STATIC "${CJSON_DIR}/cJSON.c")
target_include_directories(cjson SYSTEM PUBLIC "${CJSON_DIR}")
target_compile_options(cjson PRIVATE -w)

function(embpet_add_host_test)
    cmake_parse_arguments(T "WITH_CJSON" "NAME" "SOURCES;INCLUDES" ${ARGN})
    if(NOT T_NAME OR NOT T_SOURCES)
        message(FATAL_ERROR "embpet_add_host_test: NAME and SOURCES are required")
    endif()
    add_executable(${T_NAME} ${T_SOURCES})
    target_include_directories(${T_NAME} PRIVATE ${T_INCLUDES})
    target_link_libraries(${T_NAME} PRIVATE unity)
    if(T_WITH_CJSON)
        target_link_libraries(${T_NAME} PRIVATE cjson)
    endif()
    target_compile_options(${T_NAME} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${T_NAME} COMMAND ${T_NAME})
endfunction()

# --- tests ---
embpet_add_host_test(
    NAME     selftest
    SOURCES  selftest/test_selftest.c selftest/selftest_core.c
    INCLUDES selftest
    WITH_CJSON)
