// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/commands/CommandRegistry.hpp"
#include "aerocad/commands/Project.hpp"
#include "aerocad/commands/ImportStage.hpp"

#include <optional>

namespace aerocad::commands {

/// Registers the built-in commands (document.*, part.*, edit.*, commands.list).
void registerBuiltinCommands(CommandRegistry& registry);

/// Registers the CAD commands (sketch.*, feature.*, timeline.*, parameter.*, body.*).
void registerDesignCommands(CommandRegistry& registry);

/// Registers the project commands (project.* save / open / new, variant.* A/B variants).
void registerProjectCommands(CommandRegistry& registry);
/// Registers the scan commands (mesh.*, part.align, part.calibrate, part.transform, ...).
void registerScanCommands(CommandRegistry& registry);

/// One open project with its command registry: the single entry point used by the UI
/// and (later) the MCP server.
class Session {
public:
    Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /// Runs a command. A document-changing command first ends an active preview.
    CommandResult execute(std::string_view name, const Json& params = Json::object());

    /// Runs a document-changing command as a temporary preview (feature dialogs). The
    /// previous preview is undone first. The preview never touches the history the user
    /// sees: ending it undoes the change and restores the undo/redo stack as it was before
    /// the first preview, redo entries included.
    CommandResult preview(std::string_view name, const Json& params = Json::object());
    /// Ends the preview, if any. Returns true if one was active.
    bool cancelPreview();
    [[nodiscard]] bool previewActive() const { return m_previewBase.has_value(); }

    [[nodiscard]] model::Document& document() { return m_document; }
    [[nodiscard]] const model::Document& document() const { return m_document; }
    [[nodiscard]] model::UndoStack& undoStack() { return m_undoStack; }
    [[nodiscard]] const model::UndoStack& undoStack() const { return m_undoStack; }
    [[nodiscard]] cad::Design& design() { return m_design; }
    [[nodiscard]] const cad::Design& design() const { return m_design; }
    [[nodiscard]] ProjectState& project() { return m_project; }
    [[nodiscard]] const ProjectState& project() const { return m_project; }
    [[nodiscard]] CommandRegistry& registry() { return m_registry; }
    [[nodiscard]] const CommandRegistry& registry() const { return m_registry; }
    /// Files parsed ahead of time for document.import (see ImportStage).
    [[nodiscard]] ImportStage& importStage() { return m_importStage; }

private:
    model::Document m_document;
    model::UndoStack m_undoStack;
    cad::Design m_design;
    ProjectState m_project;
    CommandRegistry m_registry;
    ImportStage m_importStage;
    CommandContext m_context;
    /// History before the first preview; set while a preview change is applied.
    std::optional<model::UndoStack::Snapshot> m_previewBase;
};

} // namespace aerocad::commands
