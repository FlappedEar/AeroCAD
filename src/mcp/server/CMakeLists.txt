# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad-mcp: MCP server executable (stdio), started by MCP clients such as Claude.
add_executable(aerocad_mcp_server main.cpp)
set_target_properties(aerocad_mcp_server PROPERTIES OUTPUT_NAME aerocad-mcp)
target_link_libraries(aerocad_mcp_server PRIVATE AeroCAD::buildinfo AeroCAD::mcp_ipc Qt6::Core)
aerocad_set_warnings(aerocad_mcp_server)
install(TARGETS aerocad_mcp_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
