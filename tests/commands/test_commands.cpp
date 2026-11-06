// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Measure.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {

/// A unit-less ASCII STL tetrahedron written to a unique temp file.
class TempStl {
public:
    TempStl()
    {
        std::random_device rd;
        m_path = std::filesystem::temp_directory_path() / ("aerocad_cmd_" + std::to_string(rd()) + ".stl");
        std::ofstream out(m_path, std::ios::binary);
        out << "solid tetra\n";
        const char* facets[] = {
            "0 0 0  0 10 0  10 0 0", "0 0 0  10 0 0  0 0 10", "0 0 0  0 0 10  0 10 0", "10 0 0  0 10 0  0 0 10"};
        for (const char* f : facets) {
            std::string s(f);
            out << " facet normal 0 0 0\n  outer loop\n";
            std::size_t pos = 0;
            for (int i = 0; i < 3; ++i) {
                double x = 0, y = 0, z = 0;
                std::size_t used = 0;
                x = std::stod(s.substr(pos), &used);
                pos += used;
                y = std::stod(s.substr(pos), &used);
                pos += used;
                z = std::stod(s.substr(pos), &used);
                pos += used;
                out << "   vertex " << x << ' ' << y << ' ' << z << "\n";
            }
            out << "  endloop\n endfacet\n";
        }
        out << "endsolid tetra\n";
    }
    ~TempStl() { std::filesystem::remove(m_path); }
    TempStl(const TempStl&) = delete;
    TempStl& operator=(const TempStl&) = delete;

    [[nodiscard]] std::string utf8() const { return core::pathToUtf8(m_path); }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("commands.list describes every command with a schema", "[commands]")
{
    commands::Session session;
    const auto r = session.execute("commands.list");
    REQUIRE(r.ok);
    const auto& list = r.data.at("commands");
    CHECK(list.size() == session.registry().list().size());
    bool hasImport = false;
    for (const auto& c : list) {
        CHECK(c.at("params").at("type") == "object");
        CHECK_FALSE(c.at("description").get<std::string>().empty());
        hasImport = hasImport || c.at("name") == "document.import";
    }
    CHECK(hasImport);
}

TEST_CASE("Unknown commands and invalid params fail without throwing", "[commands]")
{
    commands::Session session;
    auto r = session.execute("does.not.exist");
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Unknown command") != std::string::npos);

    r = session.execute("document.import", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("path") != std::string::npos);

    r = session.execute("document.import", {{"path", 42}});
    CHECK_FALSE(r.ok);

    r = session.execute("part.remove", {{"id", 1}, {"bogus", true}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("unknown parameter") != std::string::npos);

    r = session.execute("part.scale", {{"id", 1}, {"factor", 0}});
    CHECK_FALSE(r.ok);

    r = session.execute("part.remove", {{"id", 7}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("No part") != std::string::npos);

    CHECK(r.toJson().at("ok") == false);
}

TEST_CASE("Import, modify, undo and redo through commands", "[commands]")
{
    TempStl file;
    commands::Session session;

    auto r = session.execute("document.import", {{"path", file.utf8()}});
    REQUIRE(r.ok);
    const auto id = r.data.at("id").get<std::uint64_t>();
    CHECK(r.data.at("triangles") == 4);
    CHECK(r.data.at("vertices") == 4);
    CHECK(r.data.at("format") == "STL");
    CHECK(session.document().partCount() == 1);

    r = session.execute("part.scale", {{"id", id}, {"factor", 2.0}, {"origin", {0, 0, 0}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("bounds").at("size")[0].get<double>() == Approx(20.0));

    r = session.execute("part.rename", {{"id", id}, {"name", "Fairing"}});
    REQUIRE(r.ok);
    r = session.execute("part.set_visible", {{"id", id}, {"visible", false}});
    REQUIRE(r.ok);
    CHECK(session.execute("document.bounds").data.at("bounds").is_null());

    // Undo visibility, rename, scale, import.
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().part(id)->visible);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().part(id)->name != "Fairing");
    const auto revisionBeforeUndo = session.document().part(id)->revision;
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().part(id)->mesh->bounds().size().x == Approx(10.0));
    CHECK(session.document().part(id)->revision > revisionBeforeUndo); // views must refresh
    r = session.execute("edit.undo");
    REQUIRE(r.ok);
    CHECK(r.data.at("undone").get<std::string>().rfind("Import", 0) == 0);
    CHECK(session.document().partCount() == 0);
    CHECK_FALSE(session.execute("edit.undo").ok);

    // Redo everything.
    for (int i = 0; i < 4; ++i)
        REQUIRE(session.execute("edit.redo").ok);
    const auto* part = session.document().part(id);
    REQUIRE(part != nullptr);
    CHECK(part->name == "Fairing");
    CHECK_FALSE(part->visible);
    CHECK(part->mesh->bounds().size().x == Approx(20.0));
    CHECK_FALSE(session.execute("edit.redo").ok);

    r = session.execute("document.parts");
    REQUIRE(r.ok);
    CHECK(r.data.at("parts").size() == 1);

    REQUIRE(session.execute("part.remove", {{"id", id}}).ok);
    CHECK(session.document().partCount() == 0);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().part(id) != nullptr);
}

TEST_CASE("Import of a missing file reports an error", "[commands]")
{
    commands::Session session;
    const auto r = session.execute("document.import", {{"path", "/definitely/not/here.stl"}});
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK(session.undoStack().size() == 0);
}

TEST_CASE("Listener sees every executed command", "[commands]")
{
    commands::Session session;
    std::vector<std::string> seen;
    session.registry().setListener(
        [&](const commands::CommandInfo& info, const Json&, const commands::CommandResult&) { seen.push_back(info.name); });
    session.execute("document.parts");
    session.execute("edit.undo");
    CHECK(seen == std::vector<std::string>{"document.parts", "edit.undo"});
}

TEST_CASE("project.get_context reports units, revision and parts", "[commands][agent]")
{
    TempStl file;
    commands::Session session;
    auto r = session.execute("project.get_context");
    REQUIRE(r.ok);
    CHECK(r.data.at("units").at("length") == "mm");
    CHECK(r.data.at("revision") == 0);
    CHECK(r.data.at("parts").empty());

    REQUIRE(session.execute("document.import", {{"path", file.utf8()}, {"name", "Tetra"}}).ok);
    r = session.execute("project.get_context");
    REQUIRE(r.ok);
    CHECK(r.data.at("revision").get<std::uint64_t>() > 0);
    REQUIRE(r.data.at("parts").size() == 1);
    CHECK(r.data.at("parts")[0].at("name") == "Tetra");
    CHECK(r.data.at("parts")[0].at("bounds").at("size")[2].get<double>() == Approx(10.0));
    CHECK(r.data.at("history").at("canUndo") == true);
}

TEST_CASE("geometry.measure: distance, part, point and clearance", "[commands][agent]")
{
    TempStl file;
    commands::Session session;

    auto r = session.execute("geometry.measure", {{"kind", "distance"}, {"points", {{0, 0, 0}, {3, 4, 0}}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("distance").get<double>() == Approx(5.0));
    CHECK(r.data.at("unit") == "mm");

    const auto a = session.execute("document.import", {{"path", file.utf8()}}).data.at("id").get<std::uint64_t>();
    r = session.execute("geometry.measure", {{"kind", "part"}, {"id", a}});
    REQUIRE(r.ok);
    CHECK(r.data.at("volume").get<double>() == Approx(1000.0 / 6.0));
    CHECK(r.data.at("closed") == true);
    CHECK(r.data.at("warnings").empty());

    r = session.execute("geometry.measure", {{"kind", "point_to_part"}, {"id", a}, {"point", {2, 2, -4}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("distance").get<double>() == Approx(4.0));

    // Second tetrahedron, half size, moved to x = 15..20: 5 mm from the first one.
    const auto b = session.execute("document.import", {{"path", file.utf8()}}).data.at("id").get<std::uint64_t>();
    REQUIRE(session.execute("part.scale", {{"id", b}, {"factor", 0.5}, {"origin", {30, 0, 0}}}).ok);
    r = session.execute("geometry.measure", {{"kind", "clearance"}, {"id", a}, {"other_id", b}});
    REQUIRE(r.ok);
    CHECK(r.data.at("distance").get<double>() == Approx(5.0));
    CHECK_FALSE(r.data.at("method").get<std::string>().empty());

    // Missing kind-specific parameters are reported clearly.
    r = session.execute("geometry.measure", {{"kind", "point_to_part"}, {"id", a}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("point") != std::string::npos);
    r = session.execute("geometry.measure", {{"kind", "volume"}});
    CHECK_FALSE(r.ok);
}

TEST_CASE("geometry.measure: B-rep parts are closed with exact volume", "[commands][agent]")
{
    commands::Session session;
    model::Part box;
    box.name = "Box";
    box.shape = geometry::makeBox(10, 20, 30);
    box.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(*box.shape));
    const auto id = session.document().addPart(box);

    const auto r = session.execute("geometry.measure", {{"kind", "part"}, {"id", id}});
    REQUIRE(r.ok);
    CHECK(r.data.at("closed") == true);
    CHECK(r.data.at("openEdges") == 0);
    CHECK(r.data.at("volume").get<double>() == Approx(6000.0));
    CHECK(core::openEdgeCount(*box.mesh) == 0); // per-face tessellation vertices are welded
}
