# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_commands: the command layer. Every user-visible operation is a named command
# with JSON parameters and a JSON result, so the UI, scripts and the MCP server (AI agent)
# drive the application through exactly the same code path.
add_library(aerocad_commands STATIC
    src/CommandRegistry.cpp
    src/ParamsValidation.cpp
    src/BuiltinCommands.cpp
    src/SelectionCommands.cpp
    src/DesignCommands.cpp
    src/Session.cpp
)
add_library(AeroCAD::commands ALIAS aerocad_commands)
target_include_directories(aerocad_commands PUBLIC include PRIVATE src)
target_link_libraries(aerocad_commands
    PUBLIC AeroCAD::model AeroCAD::cad nlohmann_json::nlohmann_json
    PRIVATE AeroCAD::io)
aerocad_set_warnings(aerocad_commands)
