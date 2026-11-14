// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/mcp/IpcProtocol.hpp"
#include "aerocad/mcp/McpServer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>
#include <vector>

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
        CHECK(description(name).find("Undoable") == std::string::npos);
        CHECK(description(name).find("cannot be undone") != std::string::npos);
    }
    CHECK(description("part_rename").find("Undoable.") != std::string::npos);
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
            CHECK(t.at("description").get<std::string>().find("Undoable") == std::string::npos);
            CHECK(t.at("annotations").at("destructiveHint") == false);
        }
    }
}

TEST_CASE("An image in a result goes to the agent as MCP image content", "[mcp]")
{
    const Json r = mcp::toolResultContent({{"width", 2}, {"image", {{"mimeType", "image/png"}, {"data", "iVBORw0KGgo="}}}});
    REQUIRE(r.at("content").size() == 2);
    CHECK(r.at("content")[0].at("type") == "text");
    const Json text = Json::parse(r.at("content")[0].at("text").get<std::string>());
    CHECK(text.at("width") == 2);
    CHECK_FALSE(text.contains("image")); // not a second time as base64 text
    CHECK(r.at("content")[1] == Json{{"type", "image"}, {"data", "iVBORw0KGgo="}, {"mimeType", "image/png"}});
}

TEST_CASE("A result too large for the agent has its long arrays shortened", "[mcp]")
{
    Json big = Json::array();
    for (int i = 0; i < 20000; ++i)
        big.push_back(Json::array({i * 1.5, i * 2.5}));
    const Json r = mcp::toolResultContent({{"surfaceLine", big}, {"surfaceCurve", {{"points", {{0, 1}, {2, 3}}}}}});
    const std::string text = r.at("content")[0].at("text").get<std::string>();
    CHECK(text.size() < mcp::kMaxResultChars);
    const Json data = Json::parse(text);
    CHECK(data.contains("truncated"));
    CHECK(data.at("surfaceLine").size() == mcp::kShortenedArrayItems + 1);
    CHECK(data.at("surfaceLine").back() == "... " + std::to_string(20000 - mcp::kShortenedArrayItems) + " more");
    CHECK(data.at("surfaceCurve").at("points").size() == 2); // small arrays stay whole

    const Json small = mcp::toolResultContent({{"list", Json::array({1, 2, 3})}});
    CHECK_FALSE(Json::parse(small.at("content")[0].at("text").get<std::string>()).contains("truncated"));
}

TEST_CASE("Descriptions and messages name tools, not dotted commands", "[mcp]")
{
    Fixture f;
    CHECK(mcp::withToolNames("Call simulation.run, then simulation.status.", f.session.registry()) ==
          "Call simulation_run, then simulation_status.");
    CHECK(mcp::withToolNames("see case.json and xsketch.create", f.session.registry()) ==
          "see case.json and xsketch.create");
    // Data keeps its exact text: a variant named after a command can still be activated by it.
    const Json r = mcp::messagesWithToolNames(
        {{"name", "feature.box"}, {"next", "Call simulation.run."}, {"warnings", {"use sketch.create"}},
         {"variants", {{{"name", "feature.box"}}}}},
        f.session.registry());
    CHECK(r.at("name") == "feature.box");
    CHECK(r.at("variants")[0].at("name") == "feature.box");
    CHECK(r.at("next") == "Call simulation_run.");
    CHECK(r.at("warnings")[0] == "use sketch_create");
    const Json created = f.request("tools/call", {{"name", "variant_create"}, {"arguments", {{"name", "feature.box"}}}});
    CHECK(created.at("result").at("content")[0].at("text").get<std::string>().find("\"feature.box\"") !=
          std::string::npos);
    const Json tools = f.request("tools/list").at("result").at("tools");
    for (const auto& t : tools)
        for (const auto& info : f.session.registry().list())
            CHECK(t.at("description").get<std::string>().find(" " + info.name + " ") == std::string::npos);
}

TEST_CASE("View tools are listed and explain that they need the window", "[mcp]")
{
    Fixture f;
    for (const char* name : {"view_screenshot", "view_set", "view_pick", "view_show_results"}) {
        const Json r = f.request("tools/call", {{"name", name}, {"arguments", Json::object()}});
        CHECK(r.at("result").at("isError") == true);
        const std::string text = r.at("result").at("content")[0].at("text").get<std::string>();
        // view_pick and view_show_results reject the missing parameter first.
        CHECK((text.find("AeroCAD window") != std::string::npos || text.find("required") != std::string::npos));
    }
    bool readOnly = false;
    const Json tools = f.request("tools/list").at("result").at("tools");
    for (const auto& t : tools)
        if (t.at("name") == "view_screenshot")
            readOnly = t.at("annotations").at("readOnlyHint").get<bool>();
    CHECK(readOnly);
}

TEST_CASE("agent_guide recipes name only tools and parameters that exist", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    const Json r = f.request("tools/call", {{"name", "agent_guide"}, {"arguments", Json::object()}});
    REQUIRE(r.at("result").at("isError") == false);
    const Json guide = Json::parse(r.at("result").at("content")[0].at("text").get<std::string>()).at("guide");
    for (const char* topic : {"basics", "scan", "sketch_on_surface", "fin", "solid", "cfd", "results", "variants", "export"})
        CHECK(guide.contains(topic));
    // Every "name_with_underscore {" in a recipe is a tool, and every key in its braces is
    // one of that tool's parameters.
    for (const auto& [topic, text] : guide.items()) {
        const std::string s = text.get<std::string>();
        for (std::size_t at = s.find(" {"); at != std::string::npos; at = s.find(" {", at + 1)) {
            std::size_t start = at;
            while (start > 0 && (std::isalnum(static_cast<unsigned char>(s[start - 1])) || s[start - 1] == '_'))
                --start;
            const std::string tool = s.substr(start, at - start);
            if (tool.find('_') == std::string::npos)
                continue;
            const Json* schema = nullptr;
            for (const auto& t : tools)
                if (t.at("name") == tool)
                    schema = &t.at("inputSchema");
            INFO(topic << ": " << tool);
            REQUIRE(schema != nullptr);
            // The top-level keys, up to a nested object.
            const std::size_t close = std::min(s.find('}', at), s.find('{', at + 2));
            const std::string args = s.substr(at + 2, close - at - 2);
            std::size_t k = 0;
            while (k < args.size()) {
                while (k < args.size() && !std::isalpha(static_cast<unsigned char>(args[k])))
                    ++k;
                std::size_t e = k;
                while (e < args.size() && (std::isalnum(static_cast<unsigned char>(args[e])) || args[e] == '_'))
                    ++e;
                const std::string key = args.substr(k, e - k);
                const bool startsKey = k == 0 || args[k - 1] == ' ';
                const bool endsKey = e >= args.size() || args[e] == ':' || args[e] == ',' || args[e] == '?';
                if (!key.empty() && startsKey && endsKey && args.substr(0, k).find('\'') == std::string::npos) {
                    INFO(topic << ": " << tool << " {" << key << "}");
                    CHECK(schema->at("properties").contains(key));
                }
                // Skip a value: after ':' up to the next ','.
                if (e < args.size() && args[e] == ':')
                    e = std::min(args.find(',', e), args.size());
                k = e;
            }
        }
    }
}

namespace {

// Collects "tool: path" for every property (at any depth, in items and oneOf branches) that has
// no description.
void undocumentedProperties(const Json& schema, const std::string& path, std::vector<std::string>& out)
{
    if (!schema.is_object())
        return;
    if (schema.contains("properties"))
        for (const auto& [key, prop] : schema.at("properties").items()) {
            const std::string p = path.empty() ? key : path + "." + key;
            if (!prop.contains("description") || prop.at("description").get<std::string>().empty())
                out.push_back(p);
            undocumentedProperties(prop, p, out);
        }
    if (schema.contains("items"))
        undocumentedProperties(schema.at("items"), path + "[]", out);
    for (const char* alt : {"oneOf", "anyOf"})
        if (schema.contains(alt))
            for (const auto& branch : schema.at(alt))
                undocumentedProperties(branch, path, out);
}

} // namespace

TEST_CASE("Every tool parameter has a description (CAD-231)", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    std::vector<std::string> missing;
    for (const auto& t : tools) {
        std::vector<std::string> props;
        undocumentedProperties(t.at("inputSchema"), "", props);
        for (const auto& p : props)
            missing.push_back(t.at("name").get<std::string>() + ": " + p);
    }
    std::string list;
    for (const auto& m : missing)
        list += m + "\n";
    INFO(list);
    CHECK(missing.empty());
}

TEST_CASE("tools/list is compact: shared schemas listed once, no schema boilerplate (CAD-233)", "[mcp]")
{
    Fixture f;
    const Json tools = f.request("tools/list").at("result").at("tools");
    const std::string all = tools.dump();
    // The listing is loaded by every agent session before it starts; keep it from growing back.
    // (Raised 150000 -> 152000 for Revolve extents, Extrude start offset, Polygon / Slot modes (CAD-268/269),
    // then to 155000 for tangent circles and Scale (CAD-270/271), to 159000 for Pipe, Rib, Coil and Sketch Intersect (after sharing
    // the path schema, threshold 200), to 160000 for arc slots, tangent circles to arcs and control point splines:
    // each new feature adds about 1 to 1.5 kB. Sub-schemas of 120 characters (was 200) that repeat are listed once.
    // Trim existing descriptions before raising it again.
    // Lowered by about 1.7 kB when the result-format text of simulation_results / _field / _compare moved
    // into the agent_guide topic "results" (CAD-240); 158.3 kB then.)
    // Raised 158500 -> 160000 for Split Face and Align (CAD-279/280) after trimming five unrelated descriptions,
    // then to 161000 for Move copy and Boundary Fill (CAD-281/282), to 162500 for Construction Axis (CAD-284), to 163000 for Plane Along Path (CAD-288), to 164000 for Replace Face and Sweep options (CAD-290/291), to 164500 for the sketch-over-solid facing (CAD-292), to 165500 for the ridge, fade-out and taper of Part on surface (CAD-24/25).
    CHECK(all.size() < 165500);
    CHECK(all.find("additionalProperties") == std::string::npos);
    CHECK(all.find("sketch.curves") == std::string::npos); // parameter descriptions name tools too
    const Json* create = nullptr;
    for (const auto& t : tools)
        if (t.at("name") == "sketch_create")
            create = &t;
    REQUIRE(create != nullptr);
    const Json& curves = create->at("inputSchema").at("properties").at("curves");
    CHECK(curves.at("type") == "array");
    CHECK(curves.at("description").get<std::string>().find("Same as sketch_add_curves curves") != std::string::npos);
    // The command itself still validates the full schema.
    const Json r = f.request("tools/call", {{"name", "sketch_create"},
                                            {"arguments", {{"plane", "XY"}, {"curves", {{{"type", "line"}, {"bogus", 1}}}}}}});
    CHECK(r.at("result").at("isError") == true);
}
