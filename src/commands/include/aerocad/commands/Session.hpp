// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/commands/CommandRegistry.hpp"

namespace aerocad::commands {

/// Registers the built-in commands (document.*, part.*, edit.*, commands.list).
void registerBuiltinCommands(CommandRegistry& registry);

/// Registers the commands that give an AI agent context (project.get_context,
/// geometry.measure).
void registerAgentCommands(CommandRegistry& registry);
/// Registers the CAD commands (sketch.*, feature.*, timeline.*, parameter.*, body.*).
void registerDesignCommands(CommandRegistry& registry);

/// One open project with its command registry: the single entry point used by the UI
/// and (later) the MCP server.
class Session {
public:
    Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    CommandResult execute(std::string_view name, const Json& params = Json::object());

    [[nodiscard]] model::Document& document() { return m_document; }
    [[nodiscard]] const model::Document& document() const { return m_document; }
    [[nodiscard]] model::UndoStack& undoStack() { return m_undoStack; }
    [[nodiscard]] const model::UndoStack& undoStack() const { return m_undoStack; }
    [[nodiscard]] cad::Design& design() { return m_design; }
    [[nodiscard]] const cad::Design& design() const { return m_design; }
    [[nodiscard]] CommandRegistry& registry() { return m_registry; }
    [[nodiscard]] const CommandRegistry& registry() const { return m_registry; }

private:
    model::Document m_document;
    model::UndoStack m_undoStack;
    cad::Design m_design;
    CommandRegistry m_registry;
    CommandContext m_context;
};

} // namespace aerocad::commands
