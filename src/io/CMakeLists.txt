# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_io: file import (and later export).
add_library(aerocad_io STATIC
    src/Import.cpp
    src/StlReader.cpp
    src/ObjReader.cpp
    src/ThreeMfReader.cpp
    src/CadReader.cpp
)
add_library(AeroCAD::io ALIAS aerocad_io)
target_include_directories(aerocad_io PUBLIC include PRIVATE src)
target_link_libraries(aerocad_io
    PUBLIC AeroCAD::core AeroCAD::geometry
    PRIVATE AeroCAD::geometry_occt pugixml::pugixml miniz::miniz)
aerocad_set_warnings(aerocad_io)
