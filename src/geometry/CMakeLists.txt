# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_geometry: B-rep kernel wrapper (Open CASCADE). OCCT types do not leak into
# public headers, so other modules and the UI build without OCCT include paths.
add_library(aerocad_geometry STATIC
    src/Shape.cpp
    src/Tessellation.cpp
    src/Primitives.cpp
    src/Transform.cpp
    src/Sketch.cpp
    src/SketchText.cpp
    src/Modeling.cpp
    src/Inspect.cpp
    src/SurfaceFit.cpp
    src/RegionFit.cpp
    src/Offset.cpp
    src/DrapeFit.cpp
    src/KernelError.cpp
)
add_library(AeroCAD::geometry ALIAS aerocad_geometry)
target_include_directories(aerocad_geometry PUBLIC include PRIVATE occt/include)
find_package(Threads REQUIRED)
target_link_libraries(aerocad_geometry PUBLIC AeroCAD::core PRIVATE aerocad_occt Threads::Threads)
aerocad_set_warnings(aerocad_geometry)

# Sketch text (CAD-263) needs OCCT's font support (Font_BRepFont, in TKV3d with TKService).
# Linked by file, like XDE: the imported targets drag in the visualization stack's
# development files (GL, X11), which a build machine need not have.
find_file(AEROCAD_OCCT_BREPFONT_HEADER Font_BRepFont.hxx PATHS ${OpenCASCADE_INCLUDE_DIR} NO_DEFAULT_PATH)
find_library(AEROCAD_OCCT_TKV3D TKV3d HINTS ${OpenCASCADE_LIBRARY_DIR})
find_library(AEROCAD_OCCT_TKSERVICE TKService HINTS ${OpenCASCADE_LIBRARY_DIR})
if(AEROCAD_OCCT_BREPFONT_HEADER AND AEROCAD_OCCT_TKV3D AND AEROCAD_OCCT_TKSERVICE)
    message(STATUS "Sketch text: fonts from ${AEROCAD_OCCT_TKV3D}")
    target_compile_definitions(aerocad_geometry PRIVATE AEROCAD_HAVE_FONTS)
    target_link_libraries(aerocad_geometry PRIVATE ${AEROCAD_OCCT_TKV3D} ${AEROCAD_OCCT_TKSERVICE})
else()
    message(WARNING "Open CASCADE font support (TKV3d, TKService) not found: sketch text is disabled")
endif()

# Interop target exposing the OCCT-backed Shape internals (OcctShape.hpp).
add_library(aerocad_geometry_occt INTERFACE)
add_library(AeroCAD::geometry_occt ALIAS aerocad_geometry_occt)
target_include_directories(aerocad_geometry_occt INTERFACE occt/include)
target_link_libraries(aerocad_geometry_occt INTERFACE aerocad_geometry aerocad_occt)
