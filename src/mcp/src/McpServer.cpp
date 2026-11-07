// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/McpServer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace aerocad::mcp {

namespace {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

class RpcError : public std::runtime_error {
public:
    RpcError(int code, const std::string& message) : std::runtime_error(message), m_code(code) {}
    [[nodiscard]] int code() const { return m_code; }

private:
    int m_code;
};

Json errorResponse(const Json& id, int code, const std::string& message)
{
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

constexpr const char* kInstructions =
    "AeroCAD is a CAD application. These tools run the same commands as its user interface, on the project "
    "that is open in the AeroCAD window; the user sees every change live in the 3D view and can undo it. "
    "Units: millimetres (mm) for lengths, degrees for angles; axes are right-handed with Z up. "
    "Start with project_get_context to learn the part ids, sizes and the document revision, and call it again "
    "after changes. Use geometry_measure to check dimensions and clearances instead of guessing. Every "
    "document change is recorded in the undo history (edit_undo / edit_redo), except project_new and "
    "project_open, which replace the document and clear it. Files written or deleted and programs started are "
    "never undone; each tool's description and annotations say what it does outside the document. File paths "
    "are absolute paths on the user's computer.";

} // namespace

std::string toolNameForCommand(std::string_view command)
{
    std::string out(command);
    std::replace(out.begin(), out.end(), '.', '_');
    return out;
}

bool isExposedAsTool(const commands::CommandInfo& info)
{
    return info.name != "commands.list";
}

Json toolDefinition(const commands::CommandInfo& info)
{
    Json schema = info.params;
    if (!schema.is_object() || schema.value("type", "") != "object")
        schema = Json{{"type", "object"}, {"properties", Json::object()}};
    if (auto it = schema.find("required"); it != schema.end() && it->is_array() && it->empty())
        schema.erase(it);

    const commands::CommandEffects& e = info.effects;
    std::string description = info.description;
    if (info.name == "edit.undo" || info.name == "edit.redo")
        ; // the history commands themselves: their descriptions say what they do
    else if (info.modifiesDocument && e.undoable)
        description += " Changes the document (undoable with edit_undo).";
    else if (info.modifiesDocument)
        description += " Replaces the document and clears the undo history: it cannot be undone with edit_undo.";
    if (e.writesFiles || e.deletesFiles || e.runsProcesses || e.network || e.installsSoftware) {
        std::vector<std::string> parts;
        if (e.writesFiles)
            parts.emplace_back(e.destructive ? "writes files (replacing existing ones)" : "writes files");
        if (e.deletesFiles)
            parts.emplace_back("deletes files");
        if (e.runsProcesses)
            parts.emplace_back("starts or stops programs");
        if (e.installsSoftware)
            parts.emplace_back("may download and install software");
        else if (e.network)
            parts.emplace_back("may connect to other machines");
        std::string list;
        for (std::size_t i = 0; i < parts.size(); ++i)
            list += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
        list[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(list[0])));
        description += " " + list + "; edit_undo does not reverse this.";
    }

    // readOnlyHint: nothing in the document or on the computer changes. destructiveHint (only
    // meaningful when not read-only): data may be lost; undoable document edits are not.
    const bool readOnly =
        !info.modifiesDocument && !e.writesFiles && !e.deletesFiles && !e.runsProcesses && !e.installsSoftware;
    const bool destructive = e.destructive || e.deletesFiles || (info.modifiesDocument && !e.undoable);

    return Json{{"name", toolNameForCommand(info.name)},
                {"title", info.title},
                {"description", description},
                {"inputSchema", schema},
                {"annotations",
                 {{"title", info.title},
                  {"readOnlyHint", readOnly},
                  {"destructiveHint", destructive},
                  {"openWorldHint", e.network}}}};
}

const std::vector<std::string>& McpServer::supportedProtocolVersions()
{
    static const std::vector<std::string> versions{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return versions;
}

McpServer::McpServer(const commands::CommandRegistry& registry, Backend& backend, ServerInfo info)
    : m_registry(registry), m_backend(backend), m_info(std::move(info))
{
}

std::optional<std::string> McpServer::handleLine(std::string_view line)
{
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return std::nullopt;
    Json message = Json::parse(line.begin(), line.end(), nullptr, false);
    if (message.is_discarded())
        return errorResponse(nullptr, kParseError, "Parse error").dump();
    auto response = handle(message);
    if (!response)
        return std::nullopt;
    return response->dump();
}

std::optional<Json> McpServer::handle(const Json& message)
{
    if (!message.is_object() || !message.contains("method") || !message["method"].is_string()) {
        // A response to a server request (we send none) or garbage.
        if (message.is_object() && (message.contains("result") || message.contains("error")))
            return std::nullopt;
        return errorResponse(message.is_object() ? message.value("id", Json()) : Json(), kInvalidRequest,
                             "Invalid request");
    }
    const std::string method = message["method"].get<std::string>();
    const bool isNotification = !message.contains("id");
    const Json id = message.value("id", Json());
    const Json params = message.value("params", Json::object());

    if (isNotification)
        return std::nullopt; // notifications/initialized, notifications/cancelled, ...

    try {
        Json result;
        if (method == "initialize")
            result = initialize(params);
        else if (method == "ping")
            result = Json::object();
        else if (method == "tools/list")
            result = listTools();
        else if (method == "tools/call")
            result = callTool(params);
        else
            throw RpcError(kMethodNotFound, "Method not found: " + method);
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    } catch (const RpcError& e) {
        return errorResponse(id, e.code(), e.what());
    } catch (const std::exception& e) {
        return errorResponse(id, kInvalidParams, e.what());
    }
}

Json McpServer::initialize(const Json& params)
{
    const auto& versions = supportedProtocolVersions();
    std::string version = versions.front();
    if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) {
        const auto requested = params["protocolVersion"].get<std::string>();
        if (std::find(versions.begin(), versions.end(), requested) != versions.end())
            version = requested;
    }
    if (params.contains("clientInfo") && params["clientInfo"].is_object()) {
        const auto& ci = params["clientInfo"];
        m_clientName = ci.value("name", std::string("MCP client"));
        if (ci.contains("version") && ci["version"].is_string())
            m_clientName += " " + ci["version"].get<std::string>();
    }
    return Json{{"protocolVersion", version},
                {"capabilities", {{"tools", {{"listChanged", false}}}}},
                {"serverInfo", {{"name", m_info.name}, {"title", m_info.title}, {"version", m_info.version}}},
                {"instructions", kInstructions}};
}

Json McpServer::listTools() const
{
    Json tools = Json::array();
    for (const auto& info : m_registry.list()) {
        if (isExposedAsTool(info))
            tools.push_back(toolDefinition(info));
    }
    return Json{{"tools", tools}};
}

Json McpServer::callTool(const Json& params)
{
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
        throw RpcError(kInvalidParams, "tools/call: missing tool name");
    const std::string tool = params["name"].get<std::string>();
    const commands::CommandInfo* command = nullptr;
    for (const auto& info : m_registry.list()) {
        if (isExposedAsTool(info) && toolNameForCommand(info.name) == tool) {
            command = m_registry.info(info.name);
            break;
        }
    }
    if (command == nullptr)
        throw RpcError(kInvalidParams, "Unknown tool: " + tool);

    Json arguments = params.value("arguments", Json::object());
    if (arguments.is_null())
        arguments = Json::object();

    const commands::CommandResult result = m_backend.execute(command->name, arguments);
    if (!result.ok)
        return Json{{"content", {{{"type", "text"}, {"text", result.error}}}}, {"isError", true}};
    return Json{{"content", {{{"type", "text"}, {"text", result.data.dump()}}}}, {"isError", false}};
}

} // namespace aerocad::mcp
