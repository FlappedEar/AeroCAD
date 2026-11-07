// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/cad/Design.hpp"
#include "aerocad/model/Document.hpp"
#include "aerocad/model/UndoStack.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::cfd {
class RunManager;
}

namespace aerocad::commands {

using Json = nlohmann::json;

class CommandRegistry;
class InspectState;
struct ProjectState;
class ImportStage;

/// State a command operates on.
struct CommandContext {
    model::Document& document;
    model::UndoStack& undoStack;
    const CommandRegistry* registry = nullptr; ///< For introspection (commands.list).
    cad::Design* design = nullptr;             ///< Parametric timeline (sketch.*, feature.*).
    InspectState* inspect = nullptr;           ///< Analyses (inspect.*).
    ProjectState* project = nullptr;           ///< File path, variants (project.*, variant.*).
    ImportStage* importStage = nullptr;        ///< Pre-parsed files for document.import.
    cfd::RunManager* simulations = nullptr;    ///< Wind tunnel runs (simulation.*).
};

/// What a command does besides reading the document, for clients that decide what to allow
/// (MCP tool annotations, permission prompts). Document changes are `modifiesDocument`;
/// everything else is listed here (see declareCommandEffects for the table).
struct CommandEffects {
    bool readsFiles = false;       ///< Reads files given by the caller (import, open).
    bool writesFiles = false;      ///< Creates or replaces files (save, export, simulation runs).
    bool deletesFiles = false;     ///< Removes files (simulation.delete).
    /// Starts or stops programs that keep running (the OpenFOAM solver, Colima). Short probes
    /// such as `docker version` do not count.
    bool runsProcesses = false;
    bool network = false;          ///< May reach other machines or the internet (remote engine, downloads).
    bool installsSoftware = false; ///< May download or install software.
    /// Document changes can be undone with edit.undo. False for project.new / project.open,
    /// which clear the history.
    bool undoable = true;
    /// May lose data: overwritten or deleted files, unsaved work, or (part.remove) user data
    /// removed from the document.
    bool destructive = false;
};

/// Static description of a command. `params` is a JSON Schema (subset, see
/// ParamsValidation) of the parameter object; it is exported as-is to MCP tool
/// definitions, so keep descriptions meaningful for an AI agent.
struct CommandInfo {
    std::string name;        ///< Dotted identifier, e.g. "document.import".
    std::string title;       ///< Short human-readable label for menus.
    std::string description; ///< What the command does.
    Json params = Json{{"type", "object"}, {"properties", Json::object()}};
    bool modifiesDocument = false;
    CommandEffects effects{};
};

/// Error reported by a command handler (invalid parameters, failed operation, ...).
class CommandError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct CommandResult {
    bool ok = false;
    Json data;         ///< Result object on success.
    std::string error; ///< Message on failure.

    static CommandResult success(Json data) { return {true, std::move(data), {}}; }
    static CommandResult failure(std::string message) { return {false, Json::object(), std::move(message)}; }

    [[nodiscard]] Json toJson() const;
};

class CommandRegistry {
public:
    /// Handler: validated params in, result object out. Throw CommandError on failure.
    using Handler = std::function<Json(CommandContext&, const Json& params)>;
    /// Called after every executed command (successful or not).
    using Listener = std::function<void(const CommandInfo&, const Json& params, const CommandResult&)>;

    /// Registers a command. Throws std::invalid_argument on duplicate names.
    void add(CommandInfo info, Handler handler);

    [[nodiscard]] bool contains(std::string_view name) const;
    /// Replaces the effects of a registered command. Throws std::invalid_argument for an
    /// unknown name.
    void setEffects(std::string_view name, const CommandEffects& effects);
    [[nodiscard]] const CommandInfo* info(std::string_view name) const;
    [[nodiscard]] std::vector<CommandInfo> list() const;

    /// Validates params against the command schema and runs it. Never throws for
    /// command-level errors; they are reported in the result.
    CommandResult execute(CommandContext& context, std::string_view name, const Json& params = Json::object()) const;

    void setListener(Listener listener) { m_listener = std::move(listener); }

private:
    struct Entry {
        CommandInfo info;
        Handler handler;
    };
    std::map<std::string, Entry, std::less<>> m_commands;
    Listener m_listener;
};

} // namespace aerocad::commands
