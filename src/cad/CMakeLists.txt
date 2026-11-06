# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_cad: the parametric design (feature timeline, user parameters, regeneration).
# Feature parameters are JSON, the same objects the command layer and the MCP server use.
add_library(aerocad_cad STATIC
    src/Expression.cpp
    src/Design.cpp
)
add_library(AeroCAD::cad ALIAS aerocad_cad)
target_include_directories(aerocad_cad PUBLIC include)
target_link_libraries(aerocad_cad PUBLIC AeroCAD::model AeroCAD::geometry nlohmann_json::nlohmann_json)
aerocad_set_warnings(aerocad_cad)
