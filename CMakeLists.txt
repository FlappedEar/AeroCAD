# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)

project(AeroCAD
    VERSION 0.1.0
    DESCRIPTION "CAD application for designing parts on top of arbitrary 3D models"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables go to bin/ (also avoids a clash between the AeroCAD executable and the
# AeroCAD/ QML module directory Qt generates next to it).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
include(GNUInstallDirs)

option(AEROCAD_BUILD_APP "Build the Qt Quick application" ON)
option(AEROCAD_BUILD_TESTS "Build unit tests" ON)
option(AEROCAD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AeroCADWarnings)
include(AeroCADDependencies)

if(AEROCAD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(AEROCAD_BUILD_TESTS)
    add_subdirectory(tests)
endif()
