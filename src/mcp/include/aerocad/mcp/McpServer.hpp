// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/commands/Session.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::mcp {

using Json = commands::Json;

/// Runs commands on behalf of the MCP server: in-process (SessionBackend, headless mode
/// and tests) or in the running application over local IPC (IpcClient).
class Backend {
public:
    virtual ~Backend() = default;
    virtual commands::CommandResult execute(const std::string& command, const Json& params) = 0;
};

/// Backend that runs commands in a Session owned by the caller.
class SessionBackend : public Backend {
public:
    explicit SessionBackend(commands::Session& session) : m_session(session) {}
    commands::CommandResult execute(const std::string& command, const Json& params) override
    {
        return m_session.execute(command, params);
    }

private:
    commands::Session& m_session;
};

/// MCP tool names may only contain [A-Za-z0-9_-]: "document.import" -> "document_import".
[[nodiscard]] std::string toolNameForCommand(std::string_view command);

/// Builds the MCP tool definition (name, title, description, inputSchema, annotations)
/// of a command.
[[nodiscard]] Json toolDefinition(const commands::CommandInfo& info);

/// Commands that are not exposed as tools (internal introspection).
/// `text` with command names (sketch.create) written as their tool names (sketch_create).
[[nodiscard]] std::string withToolNames(std::string text, const commands::CommandRegistry& registry);

/// `value` with withToolNames applied to the strings under explanatory keys (message, hint,
/// warnings, next, ...) only: part names, paths and other data are returned unchanged.
[[nodiscard]] Json messagesWithToolNames(Json value, const commands::CommandRegistry& registry,
                                         bool isMessage = false);

[[nodiscard]] bool isExposedAsTool(const commands::CommandInfo& info);

/// Results longer than this (characters of JSON) have their long arrays shortened.
inline constexpr std::size_t kMaxResultChars = 60000;
/// Items kept of each array in a shortened result.
inline constexpr std::size_t kShortenedArrayItems = 40;

/// The tools/call result for a successful command: its JSON as text, an "image" member
/// ({mimeType, data: base64}) as MCP image content, and long arrays shortened when the text
/// would be too large for the agent to read.
[[nodiscard]] Json toolResultContent(Json data);

/// Copy of `value` with every array cut to `maxItems` items plus a "... N more" entry.
[[nodiscard]] Json shortenArrays(const Json& value, std::size_t maxItems);

struct ServerInfo {
    std::string name = "aerocad";
    std::string title = "AeroCAD";
    std::string version;
};

/// Model Context Protocol server logic (JSON-RPC 2.0), independent of the transport.
/// Implements initialize, ping, tools/list and tools/call. The tool list comes from a
/// command registry (the server's own copy of the application's commands, so the list is
/// available even before the application is running); tool calls go to the backend.
class McpServer {
public:
    McpServer(const commands::CommandRegistry& registry, Backend& backend, ServerInfo info);

    /// Handles one JSON-RPC message. Returns the response, or std::nullopt for
    /// notifications and for responses sent by the client.
    std::optional<Json> handle(const Json& message);

    /// Parses one line of the stdio transport (newline-delimited JSON) and handles it.
    /// Returns the serialized response line (without the trailing newline), if any.
    std::optional<std::string> handleLine(std::string_view line);

    /// Name and version reported by the client in initialize ("" before).
    [[nodiscard]] const std::string& clientName() const { return m_clientName; }

    /// Protocol versions this server understands, newest first.
    [[nodiscard]] static const std::vector<std::string>& supportedProtocolVersions();

private:
    Json initialize(const Json& params);
    Json listTools() const;
    Json callTool(const Json& params);

    const commands::CommandRegistry& m_registry;
    Backend& m_backend;
    ServerInfo m_info;
    std::string m_clientName;
};

} // namespace aerocad::mcp
