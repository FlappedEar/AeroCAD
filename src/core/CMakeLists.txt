# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_core: basic math and mesh types. No third-party dependencies.
add_library(aerocad_core STATIC
    src/Mesh.cpp
    src/Measure.cpp
    src/MeshSelection.cpp
    src/MeshCheck.cpp
    src/MeshQuery.cpp
    src/MeshSection.cpp
    src/Align.cpp
    src/MeshRepair.cpp
    src/MeshSimplify.cpp
    src/Units.cpp
)
add_library(AeroCAD::core ALIAS aerocad_core)
target_include_directories(aerocad_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(aerocad_core PRIVATE Threads::Threads)
aerocad_set_warnings(aerocad_core)
