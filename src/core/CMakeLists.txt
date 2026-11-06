# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_core: basic math and mesh types. No third-party dependencies.
add_library(aerocad_core STATIC
    src/Mesh.cpp
    src/MeshCheck.cpp
)
add_library(AeroCAD::core ALIAS aerocad_core)
target_include_directories(aerocad_core PUBLIC include)
aerocad_set_warnings(aerocad_core)
