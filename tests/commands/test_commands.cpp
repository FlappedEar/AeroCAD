// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/core/Path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
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

namespace {

/// Adds a closed 10 mm cube (each face 2 triangles) as a part and returns its id.
std::uint64_t addCubePart(commands::Session& session)
{
    core::MeshBuilder b;
    const core::Vec3 p[8] = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0},
                             {0, 0, 10}, {10, 0, 10}, {10, 10, 10}, {0, 10, 10}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    for (const auto& q : quads) {
        b.addTriangle(p[q[0]], p[q[1]], p[q[2]]);
        b.addTriangle(p[q[0]], p[q[2]], p[q[3]]);
    }
    model::Part part;
    part.name = "Cube";
    part.mesh = std::make_shared<const core::TriangleMesh>(b.take());
    return session.document().addPart(part);
}

} // namespace

TEST_CASE("Selection commands are undoable", "[commands][selection]")
{
    commands::Session session;
    const auto id = addCubePart(session);

    auto r = session.execute("selection.pick_region", {{"id", id}, {"point", {5, 5, 10}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 2);
    CHECK(r.data.at("areaMm2").get<double>() == Approx(100.0));

    r = session.execute("selection.pick_region", {{"id", id}, {"point", {5, 5, 0}}, {"mode", "add"}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 4);

    r = session.execute("selection.grow", {{"id", id}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 12);

    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    r = session.execute("selection.get", {{"id", id}, {"includeTriangles", true}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 2);
    CHECK(r.data.at("triangles").size() == 2);

    r = session.execute("selection.invert", {{"id", id}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 10);
    REQUIRE(session.execute("selection.clear", {{"id", id}}).ok);
    CHECK(session.document().part(id)->selection == nullptr);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().part(id)->selection->size() == 10);

    r = session.execute("document.parts");
    CHECK(r.data.at("parts")[0].at("selectedTriangles") == 10);
}

TEST_CASE("Brush, lasso and set selection", "[commands][selection]")
{
    commands::Session session;
    const auto id = addCubePart(session);

    auto r = session.execute("selection.brush", {{"id", id}, {"points", {{5, 5, 10}}}, {"radius", 4.0}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 2); // both top triangle centroids within 4 mm

    r = session.execute("selection.lasso",
                        {{"id", id},
                         {"polygon", {{0, 0}, {1, 0}, {1, 1}, {0, 1}}},
                         {"view", {{"eye", {5, 5, 100}}, {"forward", {0, 0, -1}}, {"up", {0, 1, 0}}, {"fov", 40}, {"aspect", 1}}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 2); // only the top face is visible from above

    r = session.execute("selection.set", {{"id", id}, {"triangles", {0, 1, 1, 3}}});
    REQUIRE(r.ok);
    CHECK(r.data.at("selectedTriangles") == 3);
    CHECK_FALSE(session.execute("selection.set", {{"id", id}, {"triangles", {99}}}).ok);
    CHECK_FALSE(session.execute("selection.brush", {{"id", id}, {"points", {{0, 0, 0}}}, {"radius", 0}}).ok);
}
