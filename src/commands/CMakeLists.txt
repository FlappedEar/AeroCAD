# SPDX-License-Identifier: GPL-3.0-or-later
# aerocad_commands: the command layer. Every user-visible operation is a named command
# with JSON parameters and a JSON result, so the UI, scripts and the MCP server (AI agent)
# drive the application through exactly the same code path.
add_library(aerocad_commands STATIC
    src/CommandRegistry.cpp
    src/ParamsValidation.cpp
    src/BuiltinCommands.cpp
    src/AgentCommands.cpp
    src/SelectionCommands.cpp
    src/ExportCommands.cpp
    src/DesignCommands.cpp
    src/InspectCommands.cpp
    src/SimulationCommands.cpp
    src/ProjectCommands.cpp
    src/SurfaceCommands.cpp
    src/ImportStage.cpp
    src/ScanCommands.cpp
    src/Session.cpp
    src/CommandEffects.cpp
)
add_library(AeroCAD::commands ALIAS aerocad_commands)
target_include_directories(aerocad_commands PUBLIC include PRIVATE src)
target_link_libraries(aerocad_commands
    PUBLIC AeroCAD::model AeroCAD::cad AeroCAD::io AeroCAD::cfd nlohmann_json::nlohmann_json
    PRIVATE AeroCAD::project)
aerocad_set_warnings(aerocad_commands)
