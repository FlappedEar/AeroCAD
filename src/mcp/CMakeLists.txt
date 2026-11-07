# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_mcp: Model Context Protocol server logic (JSON-RPC over any transport) that maps
# the command layer to MCP tools. No Qt, so it is unit tested without a GUI.
add_library(aerocad_mcp STATIC
    src/McpServer.cpp
    src/IpcProtocol.cpp
)
add_library(AeroCAD::mcp ALIAS aerocad_mcp)
target_include_directories(aerocad_mcp PUBLIC include)
target_link_libraries(aerocad_mcp PUBLIC AeroCAD::commands)
aerocad_set_warnings(aerocad_mcp)

# The local IPC (Qt Network local sockets) and the aerocad-mcp executable need Qt.
if(AEROCAD_BUILD_APP)
    add_subdirectory(ipc)
    add_subdirectory(server)
endif()
