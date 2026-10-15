cmake_minimum_required(VERSION 3.16)

# Firmware version: PROJECT_VER defaults to `git describe --always --tags --dirty`.
# An untagged repository therefore reports a bare commit hash (e.g. "02af898").
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Defines embpet_component_warnings(); must come before project() so that component
# CMakeLists.txt files can call it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/embpet_warnings.cmake)

project(embpet)

# esp_http_client logs the request line, which carries the bot token, at DEBUG. Compile its
# logging (and that of the transport and TLS layers below it) out above WARN (2 =
# ESP_LOG_WARN), so no runtime log level, not even during a `sys log` change, can print it.
# components/tgbot/tgapi.c also caps these tags at runtime.
foreach(_comp esp_http_client tcp_transport esp-tls)
    idf_component_get_property(_lib ${_comp} COMPONENT_LIB)
    target_compile_definitions(${_lib} PRIVATE LOG_LOCAL_LEVEL=2)
endforeach()
