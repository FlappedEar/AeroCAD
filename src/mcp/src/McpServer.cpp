// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/McpServer.hpp"

#include <algorithm>
#include <map>
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

/// Sub-schemas at least this long (JSON characters) are listed once; later uses point to it.
constexpr std::size_t kSharedSchemaMinChars = 120;

constexpr const char* kInstructions =
    "AeroCAD is a CAD application. These tools run the same commands as its user interface, on the project "
    "that is open in the AeroCAD window; the user sees every change live in the 3D view and can undo it. "
    "Units: millimetres (mm) for lengths, degrees for angles; axes are right-handed with Z up. Part ids come from "
    "project_get_context or document_parts. A value marked \"(or expression)\" takes a number or an expression "
    "with user parameters (parameter_set) and units, e.g. \"width / 2\". "
    "Start with project_get_context to learn the part ids, sizes and the document revision, and call it again "
    "after changes; timeline_list lists the design features (sketches, extrudes, fits) by feature id. "
    "agent_guide gives step-by-step recipes for the common tasks (scan, part on a curved surface, fin/canard, "
    "solid, CFD wind tunnel, A/B variants, export): read it before a task you have not done here yet. "
    "view_screenshot shows you the 3D view as the user sees it, view_set turns the camera, and view_pick turns "
    "positions in the screenshot into 3D points on the model: look at the result after each important step "
    "instead of assuming it worked. Use geometry_measure to check dimensions and clearances instead of guessing. "
    "When a tool fails, its message says why; fix the parameters it names rather than retrying unchanged. "
    "Parameters not in a tool's schema are rejected. A schema part marked \"same as <tool> <parameter>\" has "
    "the fields of that tool's parameter. Every document change (tools marked \"Undoable.\") is recorded in "
    "the undo history (edit_undo / edit_redo), except project_new and project_open, which replace the document "
    "and clear it. Files written or deleted and programs started are "
    "never undone; each tool's description and annotations say what it does outside the document. File paths "
    "are absolute paths on the user's computer.";

} // namespace

std::string toolNameForCommand(std::string_view command)
{
    std::string out(command);
    std::replace(out.begin(), out.end(), '.', '_');
    return out;
}

std::string withToolNames(std::string text, const commands::CommandRegistry& registry)
{
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    for (const auto& info : registry.list()) {
        const std::string& name = info.name;
        for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
            const std::size_t end = at + name.size();
            if ((at > 0 && (word(text[at - 1]) || text[at - 1] == '.')) || (end < text.size() && word(text[end])))
                continue;
            text[at + name.find('.')] = '_';
        }
    }
    return text;
}

Json messagesWithToolNames(Json value, const commands::CommandRegistry& registry, bool isMessage)
{
    static const char* const messageKeys[] = {"message", "messages", "next", "hint", "note", "notes", "warning",
                                              "warnings", "error", "errors", "pick", "truncated", "guide", "reason",
                                              "reasons", "advice", "suggestion"};
    if (value.is_string())
        return isMessage ? Json(withToolNames(value.get<std::string>(), registry)) : value;
    if (value.is_array()) {
        for (auto& v : value)
            v = messagesWithToolNames(std::move(v), registry, isMessage);
        return value;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const bool message = isMessage || std::any_of(std::begin(messageKeys), std::end(messageKeys),
                                                          [&](const char* k) { return it.key() == k; });
            it.value() = messagesWithToolNames(std::move(it.value()), registry, message);
        }
    }
    return value;
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
        description += " Undoable."; // edit_undo; see the server instructions
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

    // The title is given once (not again in annotations), and destructiveHint only where it
    // means something: every listed character costs every agent session tokens (CAD-233).
    Json annotations{{"readOnlyHint", readOnly}, {"openWorldHint", e.network}};
    if (!readOnly)
        annotations["destructiveHint"] = destructive;
    return Json{{"name", toolNameForCommand(info.name)},
                {"title", info.title},
                {"description", description},
                {"inputSchema", schema},
                {"annotations", annotations}};
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

namespace {

/// A parameter schema made smaller for tools/list (CAD-231/233): descriptions name tools,
/// additionalProperties is left out (the server instructions say unknown parameters are
/// rejected, and the command still rejects them), and a large sub-schema that an earlier
/// tool (or parameter) already listed is replaced by a pointer to it.
void compactSchema(Json& schema, const std::string& where, std::map<std::string, std::string>& seen,
                   const commands::CommandRegistry& registry)
{
    if (!schema.is_object())
        return;
    schema.erase("additionalProperties");
    if (auto d = schema.find("description"); d != schema.end() && d->is_string())
        *d = withToolNames(d->get<std::string>(), registry);
    auto visit = [&](Json& sub, const std::string& subWhere) {
        if (!sub.is_object())
            return;
        Json body = sub;
        body.erase("description");
        const std::string key = body.dump();
        if (key.size() >= kSharedSchemaMinChars) {
            if (const auto it = seen.find(key); it != seen.end()) {
                const std::string own = sub.value("description", std::string());
                Json ref{{"description", withToolNames(own, registry) + (own.empty() ? "" : " ") + "Same as " +
                                             it->second + "."}};
                if (sub.contains("type"))
                    ref["type"] = sub["type"];
                sub = std::move(ref);
                return;
            }
            seen.emplace(key, subWhere);
        }
        compactSchema(sub, subWhere, seen, registry);
    };
    if (auto p = schema.find("properties"); p != schema.end() && p->is_object())
        for (auto it = p->begin(); it != p->end(); ++it)
            visit(it.value(), where + " " + it.key());
    if (auto items = schema.find("items"); items != schema.end())
        compactSchema(*items, where + "[]", seen, registry);
    for (const char* alt : {"oneOf", "anyOf"})
        if (auto a = schema.find(alt); a != schema.end() && a->is_array())
            for (auto& branch : *a)
                compactSchema(branch, where, seen, registry);
}

} // namespace

Json McpServer::listTools() const
{
    Json tools = Json::array();
    std::map<std::string, std::string> seen; // sub-schema -> "tool parameter" that listed it first
    for (const auto& info : m_registry.list()) {
        if (!isExposedAsTool(info))
            continue;
        // Descriptions name other commands (sketch.create); the agent knows them as tools.
        Json tool = toolDefinition(info);
        tool["description"] = withToolNames(tool["description"].get<std::string>(), m_registry);
        compactSchema(tool["inputSchema"], tool["name"].get<std::string>(), seen, m_registry);
        tools.push_back(std::move(tool));
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
        return Json{{"content", {{{"type", "text"}, {"text", withToolNames(result.error, m_registry)}}}},
                    {"isError", true}};
    // Only explanatory texts name tools; names, paths and other data stay as they are.
    return toolResultContent(messagesWithToolNames(result.data, m_registry));
}

Json toolResultContent(Json data)
{
    // An image (view.screenshot) goes to the agent as MCP image content, so it sees it,
    // and the rest of the result as text.
    Json image;
    if (data.is_object()) {
        if (auto it = data.find("image"); it != data.end() && it->is_object() && it->contains("data")) {
            image = std::move(*it);
            data.erase("image");
        }
    }
    std::string text = data.dump();
    if (text.size() > kMaxResultChars) {
        // MCP clients cut or refuse results this large (Claude: about 25 000 tokens), and the
        // agent then sees nothing. Long arrays are shortened, and the result says so.
        Json shortened = shortenArrays(data, kShortenedArrayItems);
        shortened["truncated"] =
            "This result had " + std::to_string(text.size()) + " characters, too many for the agent; arrays longer "
            "than " + std::to_string(kShortenedArrayItems) + " items were shortened (see the '... N more' entries). "
            "Ask for less (a part id, a smaller area, fewer samples) when you need all of it.";
        text = shortened.dump();
    }
    Json content = Json::array({Json{{"type", "text"}, {"text", text}}});
    if (!image.is_null())
        content.push_back(Json{{"type", "image"},
                               {"data", image.at("data")},
                               {"mimeType", image.value("mimeType", std::string("image/png"))}});
    return Json{{"content", content}, {"isError", false}};
}

Json shortenArrays(const Json& value, std::size_t maxItems)
{
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = shortenArrays(it.value(), maxItems);
        return out;
    }
    if (!value.is_array())
        return value;
    // Only the arrays that make the result large: small ones (a point, a short list) stay whole.
    if (value.size() <= maxItems || value.dump().size() < kMaxResultChars / 10) {
        Json out = Json::array();
        for (const auto& v : value)
            out.push_back(shortenArrays(v, maxItems));
        return out;
    }
    Json out = Json::array();
    const std::size_t n = std::min(value.size(), maxItems);
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(shortenArrays(value[i], maxItems));
    if (value.size() > maxItems)
        out.push_back("... " + std::to_string(value.size() - maxItems) + " more");
    return out;
}

} // namespace aerocad::mcp
