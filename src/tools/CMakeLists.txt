# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad-cmd: runs command scripts headless (no Qt). Used by tools/validate_exports.py
# and handy for scripting: the same commands the UI and the MCP server use.
add_executable(aerocad_cmd aerocad_cmd.cpp)
set_target_properties(aerocad_cmd PROPERTIES OUTPUT_NAME aerocad-cmd)
target_link_libraries(aerocad_cmd PRIVATE AeroCAD::commands)
aerocad_set_warnings(aerocad_cmd)
