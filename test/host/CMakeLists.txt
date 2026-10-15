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

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (default Debug for host tests)" FORCE)
endif()

project(embpet_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(EMBPET_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

enable_testing()

# Single source of truth for the warning set (defines EMBPET_WARNING_FLAGS; no IDF commands run).
include(${EMBPET_ROOT}/cmake/embpet_warnings.cmake)

# ASan + UBSan on every target (compile and link) when EMBPET_HOST_SANITIZE is ON (default). UB aborts the test instead of printing and
# continuing. LeakSanitizer is unsupported on macOS ARM; ASan simply omits leak checking there.
option(EMBPET_HOST_SANITIZE "Build host tests with ASan+UBSan" ON)
if(EMBPET_HOST_SANITIZE)
    set(EMBPET_SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined
                              -fno-omit-frame-pointer)
else()
    set(EMBPET_SANITIZE_FLAGS "")
endif()

# Third-party code is built without -Werror; project code and tests with it.
add_library(unity STATIC "${UNITY_DIR}/unity.c")
target_compile_options(unity PUBLIC ${EMBPET_SANITIZE_FLAGS})
target_link_options(unity PUBLIC ${EMBPET_SANITIZE_FLAGS})
target_include_directories(unity SYSTEM PUBLIC "${UNITY_DIR}")
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)   # cJSON numbers are double
target_compile_options(unity PRIVATE -w)

add_library(cjson STATIC "${CJSON_DIR}/cJSON.c")
target_include_directories(cjson SYSTEM PUBLIC "${CJSON_DIR}")
target_compile_options(cjson PRIVATE -w)
target_compile_options(cjson PUBLIC ${EMBPET_SANITIZE_FLAGS})
target_link_options(cjson PUBLIC ${EMBPET_SANITIZE_FLAGS})

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
    target_compile_options(${T_NAME} PRIVATE ${EMBPET_WARNING_FLAGS} ${EMBPET_SANITIZE_FLAGS})
    target_link_options(${T_NAME} PRIVATE ${EMBPET_SANITIZE_FLAGS})
    add_test(NAME ${T_NAME} COMMAND ${T_NAME})
endfunction()

# --- tests ---
embpet_add_host_test(
    NAME     selftest
    SOURCES  selftest/test_selftest.c selftest/selftest_core.c
    INCLUDES selftest
    WITH_CJSON)

embpet_add_host_test(
    NAME     prefix
    SOURCES  prefix/test_prefix.c ${EMBPET_ROOT}/components/tgbot/prefix_core.c
    INCLUDES ${EMBPET_ROOT}/components/tgbot/include)

embpet_add_host_test(
    NAME     pending
    SOURCES  pending/test_pending.c ${EMBPET_ROOT}/components/tgbot/pending_core.c
             ${EMBPET_ROOT}/components/tgbot/prefix_core.c
    INCLUDES ${EMBPET_ROOT}/components/tgbot/include)

embpet_add_host_test(
    NAME     cfg
    SOURCES  cfg/test_cfg.c ${EMBPET_ROOT}/components/netcfg/cfg_core.c
    INCLUDES ${EMBPET_ROOT}/components/netcfg/include)

embpet_add_host_test(
    NAME     escape
    SOURCES  escape/test_escape.c ${EMBPET_ROOT}/components/proto/escape_core.c
    INCLUDES ${EMBPET_ROOT}/components/proto/include)

embpet_add_host_test(
    NAME     proto
    SOURCES  proto/test_proto.c ${EMBPET_ROOT}/components/proto/proto_core.c
    INCLUDES ${EMBPET_ROOT}/components/proto/include
    WITH_CJSON)

embpet_add_host_test(
    NAME     pulse
    SOURCES  pulse/test_pulse.c ${EMBPET_ROOT}/components/io/pulse_core.c
    INCLUDES ${EMBPET_ROOT}/components/io/include)

embpet_add_host_test(
    NAME     debounce
    SOURCES  debounce/test_debounce.c ${EMBPET_ROOT}/components/io/debounce_core.c
    INCLUDES ${EMBPET_ROOT}/components/io/include)

embpet_add_host_test(
    NAME     wifi
    SOURCES  wifi/test_wifi.c ${EMBPET_ROOT}/components/netcfg/wifi_core.c
             ${EMBPET_ROOT}/components/netcfg/cfg_core.c
    INCLUDES ${EMBPET_ROOT}/components/netcfg/include)
