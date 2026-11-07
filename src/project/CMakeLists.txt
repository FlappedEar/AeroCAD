# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_project: the .aerocad project file (zip container with a versioned JSON manifest,
# lossless meshes and exact B-rep shapes) and atomic file writing.
add_library(aerocad_project STATIC
    src/ProjectFile.cpp
    src/AtomicFile.cpp
    src/MeshCodec.cpp
)
add_library(AeroCAD::project ALIAS aerocad_project)
target_include_directories(aerocad_project PUBLIC include)
target_link_libraries(aerocad_project
    PUBLIC AeroCAD::model AeroCAD::cad nlohmann_json::nlohmann_json
    PRIVATE miniz::miniz)
aerocad_set_warnings(aerocad_project)
