# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_mcp_ipc: local IPC between the application and the aerocad-mcp process
# (Qt Network local sockets: Unix domain sockets / Windows named pipes). Qt Core only.
add_library(aerocad_mcp_ipc STATIC
    src/IpcServer.cpp
    src/IpcClient.cpp
    include/aerocad/mcp/IpcServer.hpp
    include/aerocad/mcp/IpcClient.hpp
)
add_library(AeroCAD::mcp_ipc ALIAS aerocad_mcp_ipc)
set_target_properties(aerocad_mcp_ipc PROPERTIES AUTOMOC ON)
target_include_directories(aerocad_mcp_ipc PUBLIC include)
target_link_libraries(aerocad_mcp_ipc PUBLIC AeroCAD::mcp Qt6::Core Qt6::Network)
aerocad_set_source_warnings(src/IpcServer.cpp src/IpcClient.cpp)
