cmake_minimum_required(VERSION 3.16)

# Firmware version: PROJECT_VER defaults to `git describe --always --tags --dirty`.
# An untagged repository therefore reports a bare commit hash (e.g. "02af898").
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Defines embpet_component_warnings(); must come before project() so that component
# CMakeLists.txt files can call it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/embpet_warnings.cmake)

project(embpet)
