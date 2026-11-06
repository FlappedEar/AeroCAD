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

TEST_CASE("Fit a surface to a selection and undo it", "[commands][fit]")
{
    commands::Session session;
    // 40 x 40 mm gently curved sheet.
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int n = 40;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({double(i), double(j), 0.01 * (i - 20) * (i - 20)});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    model::Part sheet;
    sheet.name = "Sheet";
    sheet.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    const auto id = session.document().addPart(sheet);

    CHECK_FALSE(session.execute("surface.fit_selection", {{"id", id}}).ok); // nothing selected

    REQUIRE(session.execute("selection.brush", {{"id", id}, {"points", {{20, 20, 0}}}, {"radius", 12.0}}).ok);
    auto r = session.execute("surface.fit_selection", {{"id", id}, {"tolerance", 0.1}});
    REQUIRE(r.ok);
    CHECK(r.data.at("hasBrep") == true);
    CHECK(r.data.at("fit").at("withinTolerance") == true);
    CHECK(r.data.at("fit").at("meshToSurfaceMax").get<double>() < 0.1);
    CHECK(r.data.at("fit").at("method") == "unfold");
    CHECK(r.data.at("fit").at("holes") == 0);
    CHECK(session.document().partCount() == 2);
    // The plane method on request; anything else is refused by the schema.
    auto plane = session.execute("surface.fit_selection", {{"id", id}, {"tolerance", 0.1}, {"method", "plane"}});
    REQUIRE(plane.ok);
    CHECK(plane.data.at("fit").at("method") == "plane");
    CHECK_FALSE(session.execute("surface.fit_selection", {{"id", id}, {"method", "cylinder"}}).ok);
    REQUIRE(session.execute("edit.undo").ok);

    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().partCount() == 1);
    REQUIRE(session.execute("edit.redo").ok);
    CHECK(session.document().partCount() == 2);

    const auto surfaceId = r.data.at("id").get<model::PartId>();
    CHECK_FALSE(session.execute("surface.thicken", {{"id", id}, {"thickness", 2.0}}).ok); // mesh only

    auto offset = session.execute("surface.offset", {{"id", surfaceId}, {"distance", 2.0}});
    REQUIRE(offset.ok);
    CHECK(offset.data.at("hasBrep") == true);

    auto solid = session.execute("surface.thicken", {{"id", surfaceId}, {"thickness", 2.0}, {"side", "both"}});
    REQUIRE(solid.ok);
    CHECK(solid.data.at("volume").get<double>() > 0.0);
    CHECK(session.document().partCount() == 4);

    // The solid can become a timeline body for the modelling tools.
    auto body = session.execute("feature.base_body", {{"part", solid.data.at("id")}});
    INFO(body.toJson().dump());
    REQUIRE(body.ok);
    REQUIRE(session.execute("edit.undo").ok);

    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().partCount() == 3);
}

TEST_CASE("Fit surfaces to a whole mesh gives a solid part", "[commands][fit]")
{
    commands::Session session;
    // 30 x 20 x 10 mm box, 8 x 8 cells per face.
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    auto face = [&](core::Vec3 o, core::Vec3 a, core::Vec3 b) {
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int j = 0; j <= 8; ++j)
            for (int i = 0; i <= 8; ++i)
                v.push_back(o + a * (i / 8.0) + b * (j / 8.0));
        auto id = [base](int i, int j) { return base + static_cast<std::uint32_t>(j * 9 + i); };
        for (int j = 0; j < 8; ++j)
            for (int i = 0; i < 8; ++i) {
                t.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
                t.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
            }
    };
    face({0, 0, 0}, {0, 20, 0}, {30, 0, 0});
    face({0, 0, 10}, {30, 0, 0}, {0, 20, 0});
    face({0, 0, 0}, {30, 0, 0}, {0, 0, 10});
    face({0, 20, 0}, {0, 0, 10}, {30, 0, 0});
    face({0, 0, 0}, {0, 0, 10}, {0, 20, 0});
    face({30, 0, 0}, {0, 20, 0}, {0, 0, 10});
    model::Part scan;
    scan.name = "Scan";
    scan.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    const auto id = session.document().addPart(scan);

    CHECK_FALSE(session.execute("surface.fit_regions", {{"id", id}, {"useSelection", true}}).ok); // none
    auto r = session.execute("surface.fit_regions", {{"id", id}});
    INFO(r.toJson().dump());
    REQUIRE(r.ok);
    CHECK(r.data.at("fit").at("solid") == true);
    CHECK(r.data.at("fit").at("faces") == 6);
    CHECK(r.data.at("fit").at("volume").get<double>() == Approx(6000.0).epsilon(0.001));
    CHECK(r.data.at("fit").at("withinTolerance") == true);
    CHECK(r.data.at("hasBrep") == true);
    CHECK(session.document().partCount() == 2);
    // The solid can become a timeline body.
    CHECK(session.execute("feature.base_body", {{"part", r.data.at("id")}}).ok);
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().partCount() == 1);
}

TEST_CASE("Sketch over a curved surface, trim it with the outline and thicken", "[commands][fit]")
{
    commands::Session session;
    // 60 x 30 mm sheet curved along X (like a wing skin), facing +Z.
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int nx = 60;
    const int ny = 30;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            v.push_back({double(i), double(j), -0.005 * (i - 30) * (i - 30)});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    model::Part sheet;
    sheet.name = "Wing";
    sheet.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    const auto scanId = session.document().addPart(sheet);

    // On the scan itself: only the area around the click counts.
    auto local = session.execute("sketch.create", {{"surface", {{"part", scanId}, {"point", {5, 15, -3.125}}, {"radius", 4.0}}}});
    INFO(local.toJson().dump());
    REQUIRE(local.ok);
    CHECK(local.data.at("surface").at("area") == "area around the point");
    REQUIRE(session.execute("edit.undo").ok);

    std::vector<std::uint32_t> all(sheet.mesh->triangleCount());
    for (std::size_t i = 0; i < all.size(); ++i)
        all[i] = static_cast<std::uint32_t>(i);
    REQUIRE(session.execute("selection.set", {{"id", scanId}, {"triangles", all}}).ok);
    auto fit = session.execute("surface.fit_selection", {{"id", scanId}, {"tolerance", 0.05}});
    REQUIRE(fit.ok);
    const auto surfaceId = fit.data.at("id").get<model::PartId>();

    // Clicking the fitted surface sketches over its best-fit plane: normal the way the
    // surface faces (+Z), X along the long side, lifted to the top of the surface.
    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", surfaceId}, {"point", {30, 15, 0}}}},
                               {"curves", Json::array({{{"type", "center_rectangle"},
                                                        {"center", {0, 0}},
                                                        {"width", 40},
                                                        {"height", 10}}})}});
    INFO(sk.toJson().dump());
    REQUIRE(sk.ok);
    CHECK(sk.data.at("surface").at("area") == "whole part");
    CHECK(sk.data.at("profiles") == 1);
    const auto sketchId = sk.data.at("id").get<std::uint64_t>();
    auto profiles = session.execute("sketch.profiles", {{"sketch", sketchId}});
    REQUIRE(profiles.ok);
    const auto& plane = profiles.data.at("plane");
    CHECK(plane.at("normal").at(2).get<double>() > 0.99);
    CHECK(std::abs(plane.at("xDir").at(0).get<double>()) > 0.99);
    CHECK(plane.at("origin").at(2).get<double>() == Approx(0.0).margin(0.1)); // top of the arch
    CHECK(plane.at("origin").at(1).get<double>() == Approx(15.0).margin(0.1));

    CHECK_FALSE(session.execute("sketch.create", {{"plane", "XY"}, {"surface", {{"part", surfaceId}}}}).ok);
    CHECK_FALSE(session.execute("surface.trim_by_sketch", {{"id", scanId}, {"sketch", sketchId}}).ok); // mesh only

    // The outline projected onto the arch: 40 x 10 mm seen from above, a bit more along
    // the curve.
    auto trimmed = session.execute("surface.trim_by_sketch", {{"id", surfaceId}, {"sketch", sketchId}});
    INFO(trimmed.toJson().dump());
    REQUIRE(trimmed.ok);
    const double area = trimmed.data.at("area").get<double>();
    CHECK(area > 400.0);
    CHECK(area < 420.0);
    CHECK(trimmed.data.at("hasBrep") == true);
    const auto& box = trimmed.data.at("bounds");
    CHECK(box.at("max").at(0).get<double>() - box.at("min").at(0).get<double>() == Approx(40.0).margin(0.2));
    CHECK(box.at("max").at(1).get<double>() - box.at("min").at(1).get<double>() == Approx(10.0).margin(0.2));
    // The trimmed skin still follows the curve: its ends sit 2 mm lower than the middle.
    CHECK(box.at("max").at(2).get<double>() - box.at("min").at(2).get<double>() == Approx(2.0).margin(0.1));

    auto outside = session.execute("surface.trim_by_sketch", {{"id", surfaceId}, {"sketch", sketchId}, {"keep", "outside"}});
    REQUIRE(outside.ok);
    CHECK(outside.data.at("area").get<double>() > 1800.0 - area - 20.0);

    auto solid = session.execute("surface.thicken", {{"id", trimmed.data.at("id")}, {"thickness", 2.0}, {"side", "inside"}});
    INFO(solid.toJson().dump());
    REQUIRE(solid.ok);
    CHECK(solid.data.at("volume").get<double>() == Approx(2.0 * area).epsilon(0.03));

    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(session.document().partCount() == 3); // scan, surface, sketch
}

TEST_CASE("Part on surface: one step from an outline over a scan to a solid", "[commands][fit]")
{
    commands::Session session;
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int nx = 60;
    const int ny = 30;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            v.push_back({double(i), double(j), -0.005 * (i - 30) * (i - 30)});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    model::Part sheet;
    sheet.name = "Wing";
    sheet.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    const auto scanId = session.document().addPart(sheet);
    const Json rect = Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 30}, {"height", 12}}});

    SECTION("Straight on the scan, no selection or fit first")
    {
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scanId}, {"point", {30, 15, 0}}}}, {"curves", rect}});
        INFO(sk.toJson().dump());
        REQUIRE(sk.ok);
        const auto sketchId = sk.data.at("id").get<std::uint64_t>();
        auto profiles = session.execute("sketch.profiles", {{"sketch", sketchId}});
        REQUIRE(profiles.ok);
        CHECK(profiles.data.at("onSurface").at("part") == scanId);

        auto part = session.execute("surface.part_from_sketch", {{"sketch", sketchId}, {"thickness", 2.0}});
        INFO(part.toJson().dump());
        REQUIRE(part.ok);
        CHECK(part.data.at("fit").at("withinTolerance") == true);
        // Edges exactly where the outline is: 30 x 12 seen from above.
        const auto& box = part.data.at("bounds");
        CHECK(box.at("size").at(0).get<double>() == Approx(30.0).margin(0.3));
        CHECK(box.at("size").at(1).get<double>() == Approx(12.0).margin(0.3));
        const double area = part.data.at("area").get<double>();
        CHECK(area == Approx(30.0 * 12.0).epsilon(0.02));
        CHECK(part.data.at("volume").get<double>() == Approx(2.0 * area).epsilon(0.03));
        // Inward: below the scan surface, which peaks at z = 0.
        CHECK(box.at("max").at(2).get<double>() == Approx(0.0).margin(0.1));

        REQUIRE(session.execute("edit.undo").ok);
        CHECK(session.document().partCount() == 2); // scan and sketch
    }

    SECTION("An arc that bulges far past its defining points is covered")
    {
        // 270 degree arc of radius 12 around the sketch origin, closed by a line.
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scanId}, {"point", {30, 15, 0}}}},
                                   {"curves", Json::array({{{"type", "arc"}, {"start", {12, 0}}, {"through", {0, 12}}, {"end", {0, -12}}},
                                                           {{"type", "line"}, {"start", {0, -12}}, {"end", {12, 0}}}})}});
        REQUIRE(sk.ok);
        auto part = session.execute("surface.part_from_sketch",
                                    {{"sketch", sk.data.at("id")}, {"thickness", 1.0}});
        INFO(part.toJson().dump());
        REQUIRE(part.ok);
        CHECK(part.data.at("bounds").at("size").at(0).get<double>() == Approx(24.0).margin(0.3));
        CHECK(part.data.at("bounds").at("size").at(1).get<double>() == Approx(24.0).margin(0.3));
    }

    SECTION("On a surface fitted to a ragged brushed region, the outline sets the edges")
    {
        // A round brushed region, smaller than the outline drawn over it.
        REQUIRE(session.execute("selection.brush", {{"id", scanId}, {"points", {{30, 15, 0}}}, {"radius", 9.0}}).ok);
        auto fit = session.execute("surface.fit_selection", {{"id", scanId}, {"tolerance", 0.05}});
        REQUIRE(fit.ok);
        const auto surfaceId = fit.data.at("id").get<model::PartId>();
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", surfaceId}}}, {"curves", Json::array({{{"type", "center_rectangle"},
                                                                                                  {"center", {0, 0}},
                                                                                                  {"width", 20},
                                                                                                  {"height", 16}}})}});
        REQUIRE(sk.ok);
        const auto sketchId = sk.data.at("id").get<std::uint64_t>();

        auto trimmed = session.execute("surface.trim_by_sketch", {{"id", surfaceId}, {"sketch", sketchId}});
        INFO(trimmed.toJson().dump());
        REQUIRE(trimmed.ok);
        CHECK(trimmed.data.at("bounds").at("size").at(0).get<double>() == Approx(20.0).margin(0.3));
        CHECK(trimmed.data.at("bounds").at("size").at(1).get<double>() == Approx(16.0).margin(0.3));
        // Without extend, the ragged round border of the fit is kept.
        auto clipped = session.execute("surface.trim_by_sketch", {{"id", surfaceId}, {"sketch", sketchId}, {"extend", false}});
        REQUIRE(clipped.ok);
        CHECK(clipped.data.at("area").get<double>() < trimmed.data.at("area").get<double>() - 10.0);

        auto part = session.execute("surface.part_from_sketch", {{"sketch", sketchId}, {"thickness", 1.5}, {"side", "outside"}});
        INFO(part.toJson().dump());
        REQUIRE(part.ok);
        CHECK_FALSE(part.data.contains("fit")); // the fitted surface is used as it is
        CHECK(part.data.at("volume").get<double>() == Approx(1.5 * trimmed.data.at("area").get<double>()).epsilon(0.03));
    }

    CHECK_FALSE(session.execute("surface.part_from_sketch", {{"sketch", 99}, {"thickness", 2.0}}).ok);
}
