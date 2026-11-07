# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_buildinfo: the build stamp shown to users instead of a version number,
# e.g. "2026.11.07-0747 (1a2b3c4)": local date and time of the build plus the short git hash.
#
# The stamp is written at build time (not configure time), so every build gets a fresh one.
# Only the tiny generated BuildStamp.cpp recompiles; the executables that use it relink.
set(AEROCAD_BUILD_STAMP_CPP ${CMAKE_CURRENT_BINARY_DIR}/BuildStamp.cpp)

add_custom_target(aerocad_build_stamp
    COMMAND ${CMAKE_COMMAND}
        -DOUT=${AEROCAD_BUILD_STAMP_CPP}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/BuildStamp.cmake
    BYPRODUCTS ${AEROCAD_BUILD_STAMP_CPP}
    COMMENT "Generating build stamp"
    VERBATIM)

add_library(aerocad_buildinfo STATIC ${AEROCAD_BUILD_STAMP_CPP})
add_library(AeroCAD::buildinfo ALIAS aerocad_buildinfo)
add_dependencies(aerocad_buildinfo aerocad_build_stamp)
target_include_directories(aerocad_buildinfo PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
