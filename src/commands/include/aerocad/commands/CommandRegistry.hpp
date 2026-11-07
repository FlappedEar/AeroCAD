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

namespace aerocad::commands {

using Json = nlohmann::json;

class CommandRegistry;
struct ProjectState;
class ImportStage;

/// State a command operates on.
struct CommandContext {
    model::Document& document;
    model::UndoStack& undoStack;
    const CommandRegistry* registry = nullptr; ///< For introspection (commands.list).
    cad::Design* design = nullptr;             ///< Parametric timeline (sketch.*, feature.*).
    ProjectState* project = nullptr;           ///< File path, variants (project.*, variant.*).
    ImportStage* importStage = nullptr;        ///< Pre-parsed files for document.import.
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
