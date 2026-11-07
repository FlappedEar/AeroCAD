// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/IpcProtocol.hpp"
#include "aerocad/mcp/McpServer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace aerocad;
using mcp::Json;

namespace {

struct Fixture {
    commands::Session session;
    mcp::SessionBackend backend{session};
    mcp::McpServer server{session.registry(), backend, {"aerocad", "AeroCAD", "0.0.0-test"}};

    Json request(const std::string& method, Json params = Json::object(), int id = 1)
    {
        auto response = server.handle({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        REQUIRE(response.has_value());
        CHECK(response->at("jsonrpc") == "2.0");
        CHECK(response->at("id") == id);
        return *response;
    }
};

} // namespace

TEST_CASE("Tool names follow the MCP naming rules", "[mcp]")
{
    CHECK(mcp::toolNameForCommand("document.import") == "document_import");
    CHECK(mcp::toolNameForCommand("project.get_context") == "project_get_context");
}

TEST_CASE("initialize negotiates the protocol version and reports tools capability", "[mcp]")
{
    Fixture f;
    Json r = f.request("initialize", {{"protocolVersion", "2025-06-18"},
                                      {"capabilities", Json::object()},
                                      {"clientInfo", {{"name", "claude-code"}, {"version", "2.0"}}}});
    CHECK(r.at("result").at("protocolVersion") == "2025-06-18");
    CHECK(r.at("result").at("capabilities").contains("tools"));
    CHECK(r.at("result").at("serverInfo").at("name") == "aerocad");
    CHECK(r.at("result").at("instructions").get<std::string>().find("mm") != std::string::npos);
    CHECK(f.server.clientName() == "claude-code 2.0");

    // Unknown (e.g. future) versions get the newest version the server supports.
    r = f.request("initialize", {{"protocolVersion", "2099-01-01"}});
    CHECK(r.at("result").at("protocolVersion") == mcp::McpServer::supportedProtocolVersions().front());
}

TEST_CASE("Notifications and client responses get no reply", "[mcp]")
{
    Fixture f;
    CHECK_FALSE(f.server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).has_value());
    CHECK_FALSE(f.server.handle({{"jsonrpc", "2.0"}, {"id", 5}, {"result", Json::object()}}).has_value());
    CHECK_FALSE(f.server.handleLine("   ").has_value());
}

TEST_CASE("Protocol errors use JSON-RPC error codes", "[mcp]")
{
    Fixture f;
    auto line = f.server.handleLine("{not json");
    REQUIRE(line.has_value());
    CHECK(Json::parse(*line).at("error").at("code") == -32700);

    CHECK(f.request("resources/list").at("error").at("code") == -32601);
    CHECK(f.request("tools/call", {{"name", "no_such_tool"}}).at("error").at("code") == -32602);
    CHECK(f.request("tools/call", {{"name", "commands_list"}}).at("error").at("code") == -32602);
    CHECK(f.request("ping").at("result") == Json::object());
}

TEST_CASE("tools/list exposes every command with its schema", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    REQUIRE(tools.is_array());
    CHECK(tools.size() == f.session.registry().list().size() - 1); // without commands.list

    bool sawImport = false, sawContext = false, sawMeasure = false;
    for (const auto& t : tools) {
        const std::string name = t.at("name").get<std::string>();
        CHECK(name.find('.') == std::string::npos);
        CHECK(t.at("inputSchema").at("type") == "object");
        CHECK_FALSE(t.at("description").get<std::string>().empty());
        CHECK(t.at("annotations").contains("readOnlyHint"));
        if (name == "document_import") {
            sawImport = true;
            CHECK(t.at("inputSchema").at("required") == Json::array({"path"}));
            CHECK(t.at("annotations").at("readOnlyHint") == false);
        }
        if (name == "project_get_context") {
            sawContext = true;
            CHECK(t.at("annotations").at("readOnlyHint") == true);
            CHECK_FALSE(t.at("inputSchema").contains("required"));
        }
        sawMeasure = sawMeasure || name == "geometry_measure";
    }
    CHECK(sawImport);
    CHECK(sawContext);
    CHECK(sawMeasure);
}

TEST_CASE("tools/call runs the command and reports command errors as tool errors", "[mcp]")
{
    Fixture f;
    Json r = f.request("tools/call", {{"name", "geometry_measure"},
                                      {"arguments", {{"kind", "distance"}, {"points", {{0, 0, 0}, {0, 0, 12.5}}}}}});
    Json result = r.at("result");
    CHECK(result.at("isError") == false);
    const Json data = Json::parse(result.at("content")[0].at("text").get<std::string>());
    CHECK(data.at("distance").get<double>() == Catch::Approx(12.5));

    r = f.request("tools/call", {{"name", "project_get_context"}});
    CHECK(r.at("result").at("isError") == false);

    // Invalid arguments and failing commands are tool results with isError, so the
    // model sees the message and can correct itself.
    r = f.request("tools/call", {{"name", "part_scale"}, {"arguments", {{"id", 1}, {"factor", -1}}}});
    CHECK(r.at("result").at("isError") == true);
    CHECK(r.at("result").at("content")[0].at("text").get<std::string>().find("factor") != std::string::npos);

    r = f.request("tools/call", {{"name", "edit_undo"}});
    CHECK(r.at("result").at("isError") == true);
}

TEST_CASE("The IPC endpoint can be overridden", "[mcp]")
{
    CHECK_FALSE(mcp::defaultIpcEndpoint().empty());
}

TEST_CASE("Tool annotations state effects outside the document truthfully (CAD-81)", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    auto tool = [&](const std::string& name) {
        for (const auto& t : tools)
            if (t.at("name") == name)
                return t;
        FAIL("no tool " << name);
        return Json();
    };
    auto hints = [&](const std::string& name) { return tool(name).at("annotations"); };
    auto description = [&](const std::string& name) { return tool(name).at("description").get<std::string>(); };

    // Writing, deleting, installing and running programs are not read-only.
    for (const char* name : {"project_save", "document_export", "document_export_for_print", "simulation_run",
                             "simulation_refine", "simulation_cancel", "simulation_delete",
                             "simulation_install_engine", "simulation_set_engine", "simulation_import"}) {
        INFO(name);
        CHECK(hints(name).at("readOnlyHint") == false);
    }
    // Lost data: overwritten or deleted files, or a replaced document.
    for (const char* name : {"project_save", "document_export", "document_export_for_print", "simulation_delete",
                             "project_new", "project_open", "part_remove"}) {
        INFO(name);
        CHECK(hints(name).at("destructiveHint") == true);
    }
    // Remote engine, downloads.
    for (const char* name : {"simulation_run", "simulation_install_engine", "simulation_set_engine",
                             "simulation_environment"}) {
        INFO(name);
        CHECK(hints(name).at("openWorldHint") == true);
    }
    // Plain, undoable document edits and pure queries.
    CHECK(hints("part_rename").at("readOnlyHint") == false);
    CHECK(hints("part_rename").at("destructiveHint") == false);
    CHECK(hints("part_rename").at("openWorldHint") == false);
    CHECK(hints("simulation_results").at("readOnlyHint") == true);
    CHECK(hints("simulation_environment").at("readOnlyHint") == true);
    CHECK(hints("project_get_context").at("openWorldHint") == false);

    // No false undo promise: project_new and project_open clear the history.
    for (const char* name : {"project_new", "project_open"}) {
        INFO(name);
        CHECK(description(name).find("undoable with edit_undo") == std::string::npos);
        CHECK(description(name).find("cannot be undone") != std::string::npos);
    }
    CHECK(description("part_rename").find("undoable with edit_undo") != std::string::npos);
    CHECK(description("simulation_delete").find("Deletes files") != std::string::npos);
    CHECK(description("simulation_install_engine").find("install software") != std::string::npos);
    CHECK(description("project_save").find("edit_undo does not reverse this") != std::string::npos);
}

TEST_CASE("Every command with effects outside the document is declared (CAD-81)", "[mcp]")
{
    // A parameter naming a file or folder means the command touches the file system; it must
    // say so in its effects (the table in CommandEffects.cpp), so new commands cannot slip by.
    commands::Session session;
    for (const auto& info : session.registry().list()) {
        const Json& props = info.params.value("properties", Json::object());
        auto isFileParam = [&](const char* key) {
            return props.contains(key) && props.at(key).value("type", Json()) == "string"; // not a sweep path
        };
        if (!isFileParam("path") && !isFileParam("folder"))
            continue;
        INFO(info.name);
        const auto& e = info.effects;
        CHECK((e.readsFiles || e.writesFiles || e.deletesFiles));
    }
}

TEST_CASE("CAD-81: undo and redo do not describe themselves as undoable with edit_undo", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    for (const auto& t : tools) {
        const std::string name = t.at("name").get<std::string>();
        if (name == "edit_undo" || name == "edit_redo") {
            INFO(name);
            CHECK(t.at("description").get<std::string>().find("undoable with edit_undo") == std::string::npos);
            CHECK(t.at("annotations").at("destructiveHint") == false);
        }
    }
}
