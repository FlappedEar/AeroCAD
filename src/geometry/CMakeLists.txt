# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_geometry: B-rep kernel wrapper (Open CASCADE). OCCT types do not leak into
# public headers, so other modules and the UI build without OCCT include paths.
add_library(aerocad_geometry STATIC
    src/Shape.cpp
    src/Tessellation.cpp
    src/Primitives.cpp
    src/Transform.cpp
    src/Sketch.cpp
    src/Modeling.cpp
    src/SurfaceFit.cpp
    src/RegionFit.cpp
    src/Offset.cpp
)
add_library(AeroCAD::geometry ALIAS aerocad_geometry)
target_include_directories(aerocad_geometry PUBLIC include PRIVATE occt/include)
target_link_libraries(aerocad_geometry PUBLIC AeroCAD::core PRIVATE aerocad_occt)
aerocad_set_warnings(aerocad_geometry)

# Interop target exposing the OCCT-backed Shape internals (OcctShape.hpp).
add_library(aerocad_geometry_occt INTERFACE)
add_library(AeroCAD::geometry_occt ALIAS aerocad_geometry_occt)
target_include_directories(aerocad_geometry_occt INTERFACE occt/include)
target_link_libraries(aerocad_geometry_occt INTERFACE aerocad_geometry aerocad_occt)
