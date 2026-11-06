# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_model: the in-memory project (document, parts) and undo history.
add_library(aerocad_model STATIC
    src/Document.cpp
    src/UndoStack.cpp
)
add_library(AeroCAD::model ALIAS aerocad_model)
target_include_directories(aerocad_model PUBLIC include)
target_link_libraries(aerocad_model PUBLIC AeroCAD::core AeroCAD::geometry)
aerocad_set_warnings(aerocad_model)
