// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/MeshCheck.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/core/Measure.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"
#include "aerocad/project/ProjectFile.hpp"

#include <Standard_ConstructionError.hxx>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

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

TEST_CASE("Isolate shows one part only and Unisolate brings back what was visible", "[commands]")
{
    commands::Session session;
    for (int i = 0; i < 4; ++i)
        REQUIRE(session.execute("feature.box", {{"size", {5, 5, 5}}, {"corner", {10.0 * i, 0, 0}}}).ok);
    std::vector<model::PartId> ids;
    for (const auto& p : session.document().parts())
        ids.push_back(p.id);
    REQUIRE(ids.size() == 4);
    const auto visible = [&] {
        std::vector<bool> v;
        for (const auto id : ids)
            v.push_back(session.document().part(id)->visible);
        return v;
    };
    // The last box was hidden by hand before: it stays hidden after Unisolate.
    REQUIRE(session.execute("part.set_visible", {{"id", ids[3]}, {"visible", false}}).ok);
    // Isolating a hidden part shows it.
    auto r = session.execute("part.isolate", {{"id", ids[3]}});
    REQUIRE(r.ok);
    CHECK(visible() == std::vector<bool>{false, false, false, true});
    CHECK(r.data.at("hidden") == Json({ids[0], ids[1], ids[2]}));
    REQUIRE(session.execute("edit.undo").ok); // one step
    CHECK(visible() == std::vector<bool>{true, true, true, false});
    REQUIRE(session.execute("edit.redo").ok);
    CHECK(visible() == std::vector<bool>{false, false, false, true});
    REQUIRE(session.execute("edit.undo").ok);

    r = session.execute("part.isolate", {{"id", ids[1]}});
    REQUIRE(r.ok);
    CHECK(visible() == std::vector<bool>{false, true, false, false});
    const Json hidden = r.data.at("hidden");
    CHECK(hidden == Json({ids[0], ids[2]}));
    r = session.execute("part.unisolate", {{"ids", hidden}});
    REQUIRE(r.ok);
    CHECK(visible() == std::vector<bool>{true, true, true, false});
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(visible() == std::vector<bool>{false, true, false, false});
    // Unknown or deleted ids are skipped.
    CHECK(session.execute("part.unisolate", {{"ids", {ids[0], 9999}}}).ok);
    CHECK(visible() == std::vector<bool>{true, true, false, false});
    CHECK_FALSE(session.execute("part.isolate", {{"id", 9999}}).ok);
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
    REQUIRE(r.data.contains("centerOfMass")); // closed mesh: from the enclosed tetrahedra
    CHECK(r.data.at("centerOfMass")[0].get<double>() == Approx(2.5));
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
    CHECK(r.data.at("surfaceArea").get<double>() == Approx(2 * (200.0 + 600.0 + 300.0)));
    REQUIRE(r.data.contains("centerOfMass"));
    CHECK(r.data.at("centerOfMass")[0].get<double>() == Approx(5.0));
    CHECK(r.data.at("centerOfMass")[1].get<double>() == Approx(10.0));
    CHECK(r.data.at("centerOfMass")[2].get<double>() == Approx(15.0));
    CHECK(core::openEdgeCount(*box.mesh) == 0); // per-face tessellation vertices are welded
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
    // Bounds of the selected top face (for Zoom to Selection).
    CHECK(r.data.at("bounds").at("min")[2].get<double>() == Approx(10.0));
    CHECK(r.data.at("bounds").at("max")[2].get<double>() == Approx(10.0));
    CHECK(r.data.at("bounds").at("center")[0].get<double>() == Approx(5.0));
    CHECK(r.data.at("bounds").at("diagonal").get<double>() == Approx(std::sqrt(200.0)));

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

    // The solid already is a timeline body, directly usable by modelling tools.
    CHECK(solid.data.at("kind") == "body");
    CHECK(solid.data.at("featureId").get<cad::FeatureId>() != 0);

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
    CHECK(r.data.at("fit").at("smoothSeams") == 0); // a box has creases only
    // The app's "Show deviation from the scan": the scan coloured by its distance to the fit.
    CHECK(r.data.at("source") == "surface.fit_regions of part " + std::to_string(id));
    auto dev = session.execute(
        "inspect.deviation",
        {{"measured", id}, {"reference", r.data.at("id")}, {"tolerance", 0.25}, {"range", 0.5}, {"maxDistance", 2.0}});
    INFO(dev.toJson().dump());
    REQUIRE(dev.ok);
    CHECK(dev.data.at("withinTolerance").get<double>() > 99.0);
    // Measured on the exact surfaces, not the display mesh: it agrees with the fit report.
    const double largest = std::max(std::abs(dev.data.at("stats").at("min").get<double>()),
                                    std::abs(dev.data.at("stats").at("max").get<double>()));
    CHECK(largest < r.data.at("fit").at("meshToSurfaceMax").get<double>() + 0.01);
    REQUIRE(session.execute("inspect.clear").ok);
    CHECK(session.document().partCount() == 2);
    // The fitted solid already is a timeline body.
    CHECK(r.data.at("kind") == "body");
    CHECK(r.data.at("featureId").get<cad::FeatureId>() != 0);
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

        // The sketch is shown lying on the scan, not on its plane above it.
        const model::Part* sketchPart = nullptr;
        for (const auto& p : session.document().parts())
            if (p.kind == model::PartKind::Sketch)
                sketchPart = &p;
        REQUIRE(sketchPart != nullptr);
        REQUIRE(sketchPart->edgeSegments.size() > 8); // split finely to follow the curve
        double worst = 0.0;
        for (const auto& q : sketchPart->edgeSegments)
            worst = std::max(worst, std::abs(q.z - (-0.005 * (q.x - 30) * (q.x - 30))));
        CHECK(worst < 0.05);

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

    SECTION("Across the surface: a plane standing on it, for a fin")
    {
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scanId}, {"point", {30, 15, 0}}, {"across", true}}}});
        INFO(sk.toJson().dump());
        REQUIRE(sk.ok);
        auto profiles = session.execute("sketch.profiles", {{"sketch", sk.data.at("id")}});
        REQUIRE(profiles.ok);
        const auto& pl = profiles.data.at("plane");
        // X along the surface normal (+Z here), the plane standing on the surface; the
        // surface faces up, so the fin's normal follows the long side (X).
        CHECK(pl.at("xDir").at(2).get<double>() > 0.99);
        CHECK(std::abs(pl.at("normal").at(2).get<double>()) < 0.01);
        CHECK(std::abs(pl.at("origin").at(2).get<double>()) < 0.1); // on the surface
        CHECK(profiles.data.at("onSurface").at("across") == true);
    }

    SECTION("A fin drawn across the surface stands on it, its root following the curve")
    {
        // Height 0..10 above the surface (x), 16 mm chord (y); the root reaches 2 mm in.
        const Json fin = Json::array({{{"type", "rectangle"}, {"corner", {-2, -8}}, {"opposite", {10, 8}}}});
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}},
                                   {"curves", fin}});
        INFO(sk.toJson().dump());
        REQUIRE(sk.ok);
        // The sketch reports where its plane cuts the scan (sketch coordinates): the line to
        // draw the root into, at x = 0 where the plane stands on it.
        auto prof = session.execute("sketch.profiles", {{"sketch", sk.data.at("id")}, {"surfaceLine", true}});
        REQUIRE(prof.ok);
        const auto& line = prof.data.at("surfaceLine");
        CHECK(line.size() > 20);
        double nearRoot = 1e9;
        for (const auto& q : line)
            if (std::abs(q.at(1).get<double>()) < 1.0)
                nearRoot = std::min(nearRoot, std::abs(q.at(0).get<double>()));
        CHECK(nearRoot < 0.05);

        auto part = session.execute("surface.fin_from_sketch", {{"sketch", sk.data.at("id")}, {"thickness", 3.0}});
        INFO(part.toJson().dump());
        REQUIRE(part.ok);
        CHECK(part.data.at("fit").at("part") == scanId);
        CHECK(part.data.at("hasBrep") == true);
        const auto& box = part.data.at("bounds");
        // Tip 10 mm above the plane's origin on the surface; the root cut at the surface,
        // which falls away along X (slope -0.1 at x = 40): no part of the fin under it.
        CHECK(box.at("max").at(2).get<double>() == Approx(-0.5 + 10.0).margin(0.3));
        CHECK(box.at("min").at(2).get<double>() > -0.5 - 0.3);
        CHECK(box.at("min").at(2).get<double>() < -0.5 + 0.1);
        // A roundish area facing up: the fin stands across world X, 3 mm thick along it.
        CHECK(box.at("size").at(0).get<double>() == Approx(3.0).margin(1.0));
        CHECK(box.at("size").at(1).get<double>() == Approx(16.0).margin(0.2));
        CHECK(part.data.at("volume").get<double>() == Approx(3.0 * 16.0 * 10.0).epsilon(0.03));
        for (const auto& q : session.document().part(part.data.at("id").get<model::PartId>())->mesh->vertices())
            CHECK(q.z > -0.005 * (q.x - 30) * (q.x - 30) - 0.05); // nothing below the scan

        // On a fitted surface the fin is cut by it directly.
        std::vector<std::uint32_t> all(session.document().part(scanId)->mesh->triangleCount());
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = static_cast<std::uint32_t>(i);
        REQUIRE(session.execute("selection.set", {{"id", scanId}, {"triangles", all}}).ok);
        auto fitted = session.execute("surface.fit_selection", {{"id", scanId}, {"tolerance", 0.05}});
        REQUIRE(fitted.ok);
        REQUIRE(session.execute("selection.clear", {{"id", scanId}}).ok);
        auto onFit = session.execute("sketch.create", {{"surface", {{"part", fitted.data.at("id")}, {"point", {20, 15, -0.5}}, {"across", true}}},
                                                       {"curves", fin}});
        REQUIRE(onFit.ok);
        auto finOnFit = session.execute("surface.fin_from_sketch", {{"sketch", onFit.data.at("id")}, {"thickness", 2.0}});
        INFO(finOnFit.toJson().dump());
        REQUIRE(finOnFit.ok);
        CHECK_FALSE(finOnFit.data.contains("fit"));
        CHECK(finOnFit.data.at("volume").get<double>() > 2.0 * 16.0 * 9.0);
        // Where it was clicked, not in the middle of the surface.
        const auto& fb = finOnFit.data.at("bounds");
        CHECK((fb.at("min").at(0).get<double>() + fb.at("max").at(0).get<double>()) / 2 == Approx(20.0).margin(1.0));
        // With a root fillet on the fitted surface: one closed solid, flared on both sides.
        auto flared = session.execute("surface.fin_from_sketch",
                                      {{"sketch", onFit.data.at("id")}, {"thickness", 2.0}, {"rootFillet", 2.0}});
        INFO(flared.toJson().dump());
        REQUIRE(flared.ok);
        CHECK(flared.data.at("volume").get<double>() > finOnFit.data.at("volume").get<double>());
        // The fin stands across Y here (2 mm thick, leaning with the surface): 2 x r wider.
        CHECK(flared.data.at("bounds").at("size").at(1).get<double>() ==
              Approx(fb.at("size").at(1).get<double>() + 2 * 2.0).margin(0.6));
        CHECK(core::checkMesh(core::weldVertices(
                  *session.document().part(flared.data.at("id").get<model::PartId>())->mesh)).watertight());

        // Drawn the other way (most of it on the inner side): the bigger side is kept.
        auto flipped = session.execute(
            "sketch.create", {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}},
                              {"curves", Json::array({{{"type", "rectangle"}, {"corner", {-10, -8}}, {"opposite", {2, 8}}}})}});
        REQUIRE(flipped.ok);
        auto inner = session.execute("surface.fin_from_sketch", {{"sketch", flipped.data.at("id")}, {"thickness", 3.0}});
        INFO(inner.toJson().dump());
        REQUIRE(inner.ok);
        CHECK(inner.data.at("volume").get<double>() == Approx(3.0 * 16.0 * 10.0).epsilon(0.03));
        CHECK(inner.data.at("bounds").at("max").at(2).get<double>() < -0.5 + 0.3);

        // A fin that floats above the surface is refused with a hint.
        auto floating = session.execute(
            "sketch.create", {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}},
                              {"curves", Json::array({{{"type", "rectangle"}, {"corner", {1, -8}}, {"opposite", {10, 8}}}})}});
        REQUIRE(floating.ok);
        auto refused = session.execute("surface.fin_from_sketch", {{"sketch", floating.data.at("id")}, {"thickness", 3.0}});
        CHECK_FALSE(refused.ok);
        CHECK(refused.toJson().dump().find("root") != std::string::npos);

        // A low fin with its root only just into the surface: the root carried down does not
        // turn it into a fin on the inner side.
        auto low = session.execute(
            "sketch.create", {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}},
                              {"curves", Json::array({{{"type", "rectangle"}, {"corner", {-0.2, -8}}, {"opposite", {1.5, 8}}}})}});
        REQUIRE(low.ok);
        auto lowFin = session.execute("surface.fin_from_sketch", {{"sketch", low.data.at("id")}, {"thickness", 2.0}});
        INFO(lowFin.toJson().dump());
        REQUIRE(lowFin.ok);
        CHECK(lowFin.data.at("volume").get<double>() == Approx(2.0 * 16.0 * 1.5).epsilon(0.2));
        CHECK(lowFin.data.at("bounds").at("max").at(2).get<double>() > -0.5 + 1.2);

        // A triangle whose centroid is outside but most of whose area is inside: the bigger
        // side is kept, as drawn.
        auto tri = session.execute(
            "sketch.create", {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}},
                              {"curves", Json::array({{{"type", "line"}, {"start", {-10, -10}}, {"end", {-10, 10}}},
                                                      {{"type", "line"}, {"start", {-10, 10}}, {"end", {22, 0}}},
                                                      {{"type", "line"}, {"start", {22, 0}}, {"end", {-10, -10}}}})}});
        REQUIRE(tri.ok);
        auto triFin = session.execute("surface.fin_from_sketch", {{"sketch", tri.data.at("id")}, {"thickness", 2.0}});
        INFO(triFin.toJson().dump());
        REQUIRE(triFin.ok);
        CHECK(triFin.data.at("bounds").at("max").at(2).get<double>() < -0.5 + 0.5);

        // Two outlines, most of the first above the surface and most of the second below:
        // one side is kept for both, and neither root extension is kept as fin.
        auto outlineBox = [](double x0, double x1, double y0, double y1) {
            return Json::array({{{"type", "line"}, {"start", {x0, y0}}, {"end", {x1, y0}}},
                                {{"type", "line"}, {"start", {x1, y0}}, {"end", {x1, y1}}},
                                {{"type", "line"}, {"start", {x1, y1}}, {"end", {x0, y1}}},
                                {{"type", "line"}, {"start", {x0, y1}}, {"end", {x0, y0}}}});
        };
        Json pair = outlineBox(-1, 10, -15, -5);
        for (auto& c : outlineBox(-2, 1, 5, 15))
            pair.push_back(c);
        auto two = session.execute(
            "sketch.create",
            {{"surface", {{"part", scanId}, {"point", {40, 15, -0.5}}, {"across", true}}}, {"curves", pair}});
        REQUIRE(two.ok);
        auto twoFins = session.execute("surface.fin_from_sketch", {{"sketch", two.data.at("id")}, {"thickness", 2.0}});
        INFO(twoFins.toJson().dump());
        REQUIRE(twoFins.ok);
        CHECK(twoFins.data.at("volume").get<double>() == Approx(2.0 * (10.0 * 10.0 + 1.0 * 10.0)).epsilon(0.1));
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
        // Built on the scan the surface came from, under the outline.
        CHECK(part.data.at("fit").at("part") == scanId);
        CHECK(part.data.at("fit").at("method") == "draped");
        CHECK(part.data.at("volume").get<double>() == Approx(1.5 * trimmed.data.at("area").get<double>()).epsilon(0.03));
    }

    SECTION("An outline drawn past the fitted surface is fitted again on the scan")
    {
        REQUIRE(session.execute("selection.brush", {{"id", scanId}, {"points", {{30, 15, 0}}}, {"radius", 6.0}}).ok);
        auto fit = session.execute("surface.fit_selection", {{"id", scanId}, {"tolerance", 0.05}});
        REQUIRE(fit.ok);
        const auto surfaceId = fit.data.at("id").get<model::PartId>();
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", surfaceId}}},
                                   {"curves", Json::array({{{"type", "center_rectangle"},
                                                            {"center", {0, 0}},
                                                            {"width", 40},
                                                            {"height", 20}}})}});
        REQUIRE(sk.ok);
        auto part = session.execute("surface.part_from_sketch", {{"sketch", sk.data.at("id")}, {"thickness", 1.0}});
        INFO(part.toJson().dump());
        REQUIRE(part.ok);
        CHECK(part.data.at("fit").at("part") == scanId);
        CHECK(part.data.at("warnings").empty());
        CHECK(part.data.at("bounds").at("size").at(0).get<double>() == Approx(40.0).margin(0.3));
        CHECK(part.data.at("bounds").at("size").at(1).get<double>() == Approx(20.0).margin(0.3));
    }

    SECTION("A part already made from the scan does not stand in for the scan")
    {
        auto first = session.execute("sketch.create",
                                     {{"surface", {{"part", scanId}, {"point", {30, 15, 0}}}},
                                      {"curves", Json::array({{{"type", "center_rectangle"},
                                                               {"center", {0, 0}},
                                                               {"width", 20},
                                                               {"height", 10}}})}});
        REQUIRE(first.ok);
        auto part = session.execute("surface.part_from_sketch", {{"sketch", first.data.at("id")}, {"thickness", 1.0}});
        REQUIRE(part.ok);
        const auto solidId = part.data.at("id").get<model::PartId>();
        // The sketch is hidden with the part, and one undo brings it back and removes the part.
        auto visibleSketch = [&]() {
            const auto parts = session.execute("document.parts");
            for (const auto& p : parts.data.at("parts"))
                if (p.at("kind") == "sketch" && p.at("featureId") == first.data.at("id"))
                    return p.at("visible").get<bool>();
            FAIL("no sketch part");
            return false;
        };
        CHECK_FALSE(visibleSketch());
        REQUIRE(session.execute("edit.undo").ok);
        CHECK(visibleSketch());
        CHECK(session.document().part(solidId) == nullptr);
        REQUIRE(session.execute("edit.redo").ok);
        CHECK_FALSE(visibleSketch());
        REQUIRE(session.document().part(solidId) != nullptr);

        auto second = session.execute("sketch.create",
                                      {{"surface", {{"part", solidId}}},
                                       {"curves", Json::array({{{"type", "center_rectangle"},
                                                                {"center", {0, 0}},
                                                                {"width", 8},
                                                                {"height", 4}}})}});
        REQUIRE(second.ok);
        // Not silently built on the scan underneath: a solid is no surface to cut from.
        auto onSolid = session.execute("surface.part_from_sketch", {{"sketch", second.data.at("id")}, {"thickness", 1.0}});
        INFO(onSolid.toJson().dump());
        CHECK_FALSE(onSolid.ok);
    }

    CHECK_FALSE(session.execute("surface.part_from_sketch", {{"sketch", 99}, {"thickness", 2.0}}).ok);

    // An ordinary sketch has no surface line.
    auto plain = session.execute("sketch.create", {{"plane", "XY"}});
    REQUIRE(plain.ok);
    auto plainProfiles = session.execute("sketch.profiles", {{"sketch", plain.data.at("id")}, {"surfaceLine", true}});
    INFO(plainProfiles.toJson().dump());
    REQUIRE(plainProfiles.ok);
    CHECK(plainProfiles.data.at("surfaceLine").empty());
}

TEST_CASE("Sketch and part next to a grille opening of a noisy bumper scan", "[commands][fit]")
{
    // A bumper-like scan: 600 x 300 mm, curved both ways, facing +Y, 3 mm sampling,
    // 0.3 mm noise and a few spikes, with a 200 x 60 mm grille opening.
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0.0, 0.3);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const int nu = 200;
    const int nw = 100;
    auto skin = [](double u, double w) { return -u * u / 1200.0 - (w - 150.0) * (w - 150.0) / 1500.0; };
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    for (int j = 0; j <= nw; ++j)
        for (int i = 0; i <= nu; ++i) {
            const double u = -300.0 + 3.0 * i, w = 3.0 * j;
            double y = skin(u, w) + noise(rng);
            if (unit(rng) < 0.002)
                y += 4.0 * (unit(rng) - 0.5); // scanner spikes
            v.push_back({u, y, w});
        }
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (nu + 1) + i); };
    auto inGrille = [](double u, double w) { return std::abs(u) < 100.0 && w > 80.0 && w < 140.0; };
    for (int j = 0; j < nw; ++j)
        for (int i = 0; i < nu; ++i) {
            if (inGrille(-300.0 + 3.0 * (i + 0.5), 3.0 * (j + 0.5)))
                continue;
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i + 1, j)});
            t.push_back({idx(i, j), idx(i, j + 1), idx(i + 1, j + 1)});
        }
    model::Part scanPart;
    scanPart.name = "Bumper scan";
    scanPart.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    commands::Session session;
    const auto scanId = session.document().addPart(scanPart);
    const auto& mesh = *session.document().part(scanId)->mesh;

    // Clicked from 2 m in front, in the middle of the grille: the ray goes through.
    const core::Vec3 eye{0, 2000, 110};
    const core::Vec3 ray{0, -1, 0};
    REQUIRE_FALSE(core::raycast(mesh, eye, ray));
    const auto near = core::nearestToRay(mesh, eye, ray, 4.0);
    REQUIRE(near);
    // On the rim of the opening, the nearer of its top and bottom edges.
    CHECK(std::abs(near->point.x) < 100.0);
    CHECK((std::abs(near->point.z - 80.0) < 4.0 || std::abs(near->point.z - 140.0) < 4.0));

    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", scanId}, {"point", {near->point.x, near->point.y, near->point.z}}}},
                               {"curves", Json::array({{{"type", "center_rectangle"},
                                                        {"center", {0, 0}},
                                                        {"width", 80},
                                                        {"height", 100}}})}});
    INFO(sk.toJson().dump());
    REQUIRE(sk.ok);
    // Over the skin around the click, below the opening; tall enough to reach into it.
    const auto& origin = sk.data.at("params").at("plane").at("origin");
    CHECK(origin.at(2).get<double>() > 20.0);
    CHECK(origin.at(2).get<double>() < 80.0);
    const auto sketchId = sk.data.at("id").get<std::uint64_t>();

    auto part = session.execute("surface.part_from_sketch", {{"sketch", sketchId}, {"thickness", 2.0}, {"side", "outside"}});
    INFO(part.toJson().dump());
    REQUIRE(part.ok);
    CHECK(part.data.at("fit").at("method") == "draped");
    // Follows the noisy skin about as closely as its noise, spikes and all.
    CHECK(part.data.at("fit").at("rmsDeviation").get<double>() < 0.6);
    // Part of the outline is over the opening: bridged, and said so.
    CHECK(part.data.at("fit").at("bridged").get<double>() > 0.05);
    CHECK_FALSE(part.data.at("warnings").empty());
    CHECK(part.data.at("volume").get<double>() > 0.0);
    CHECK(part.data.at("volume").get<double>() == Approx(2.0 * part.data.at("area").get<double>()).epsilon(0.05));
}

TEST_CASE("Fin on a scan with a layer behind the skin and holes: the root sits on the outer skin", "[commands][fit]")
{
    // A curved front facing -Y (like a bumper), 0.5 mm triangles, with a hole under part of
    // the fin's root and an inner layer 3 mm behind it (the inside of the bumper) that
    // faces the other way.
    commands::Session session;
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    auto skinY = [](double x, double z) { return 0.004 * x * x + 0.002 * (z - 40) * (z - 40); };
    const int nx = 240, nz = 160;
    const double step = 0.5;
    auto layer = [&](double behind, bool flip, bool holes) {
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int j = 0; j <= nz; ++j)
            for (int i = 0; i <= nx; ++i) {
                const double x = -60 + i * step, z = j * step;
                v.push_back({x, skinY(x, z) + behind, z});
            }
        auto idx = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (nx + 1) + i); };
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                const double x = -60 + (i + 0.5) * step, z = (j + 0.5) * step;
                if (holes && (x - 8) * (x - 8) + (z - 40) * (z - 40) < 9.0)
                    continue; // a 3 mm hole right under the root
                if (!flip) {
                    t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
                    t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
                } else {
                    t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i + 1, j)});
                    t.push_back({idx(i, j), idx(i, j + 1), idx(i + 1, j + 1)});
                }
            }
    };
    layer(0.0, false, true);  // outer skin, facing -Y
    layer(3.0, true, false);  // inner layer behind it
    model::Part scan;
    scan.name = "Bumper";
    scan.mesh = std::make_shared<const core::TriangleMesh>(core::TriangleMesh(std::move(v), std::move(t)));
    const auto scanId = session.document().addPart(scan);
    REQUIRE(scan.mesh->faceNormal(0).y < 0.0);

    // A level canard: its plane stands across the front, X out of the bumper (-Y).
    auto sk = session.execute(
        "sketch.create",
        {{"surface", {{"part", scanId}, {"point", {0, 0, 40}}, {"across", true}}},
         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {-4, -25}}, {"opposite", {30, 25}}}})}});
    INFO(sk.toJson().dump());
    REQUIRE(sk.ok);
    auto profiles = session.execute("sketch.profiles", {{"sketch", sk.data.at("id")}});
    REQUIRE(profiles.ok);
    CHECK(profiles.data.at("plane").at("xDir").at(1).get<double>() < -0.95);              // out of the bumper
    CHECK(std::abs(profiles.data.at("plane").at("normal").at(2).get<double>()) > 0.95); // level fin

    auto fin = session.execute("surface.fin_from_sketch", {{"sketch", sk.data.at("id")}, {"thickness", 3.0}});
    INFO(fin.toJson().dump());
    REQUIRE(fin.ok);
    CHECK(fin.data.at("fit").at("part") == scanId);
    CHECK(fin.data.at("fit").at("rmsDeviation").get<double>() < 0.05);
    // The root lies on the outer skin: nothing of the fin behind it (not cut at the inner
    // layer, not left hanging into the hole), and it reaches it everywhere.
    const auto* part = session.document().part(fin.data.at("id").get<model::PartId>());
    REQUIRE(part != nullptr);
    double deepest = -1e9;
    for (const auto& q : part->mesh->vertices()) {
        CHECK(q.y < skinY(q.x, q.z) + 0.1);
        deepest = std::max(deepest, q.y - skinY(q.x, q.z));
    }
    CHECK(deepest > -0.1);
    // Chord 50 mm, 30 mm out at its centre, 3 mm thick; the skin curves away towards the
    // ends of the chord, so the fin is a little longer there.
    const double volume = fin.data.at("volume").get<double>();
    CHECK(volume > 3.0 * 50.0 * 30.0);
    CHECK(volume < 3.0 * 50.0 * 30.0 * 1.15);

    // With a root fillet: the same fin flares into the skin, tangent to it, and is still one
    // closed solid sitting on the outer skin.
    auto filleted = session.execute("surface.fin_from_sketch",
                                    {{"sketch", sk.data.at("id")}, {"thickness", 3.0}, {"rootFillet", 4.0}});
    INFO(filleted.toJson().dump());
    REQUIRE(filleted.ok);
    CHECK(filleted.data.at("rootFillet").get<double>() == 4.0);
    const auto* flared = session.document().part(filleted.data.at("id").get<model::PartId>());
    REQUIRE(flared != nullptr);
    CHECK(core::checkMesh(core::weldVertices(*flared->mesh)).watertight());
    // A concave fillet adds (1 - pi/4) r^2 of section along the root, which runs 2 x 50 + 2 x 3 mm.
    const double added = filleted.data.at("volume").get<double>() - volume;
    const double expected = (1.0 - 3.14159 / 4.0) * 16.0 * 106.0;
    CHECK(added > 0.6 * expected);
    CHECK(added < 1.4 * expected);
    double rootWidth = 0.0, flaredDeepest = -1e9;
    for (const auto& q : flared->mesh->vertices()) {
        CHECK(q.y < skinY(q.x, q.z) + 0.1);
        flaredDeepest = std::max(flaredDeepest, q.y - skinY(q.x, q.z));
        if (q.y > skinY(q.x, q.z) - 0.3 && std::abs(q.x) < 10.0)
            rootWidth = std::max(rootWidth, std::abs(q.z - 40.0));
    }
    CHECK(flaredDeepest > -0.1);
    // On the skin, the fillets reach r beyond each side of the 3 mm fin.
    CHECK(rootWidth == Approx(1.5 + 4.0).margin(0.6));

    // A fillet that cannot fit does not cost the fin: it gets a smaller one or none, and
    // says so.
    auto huge = session.execute("surface.fin_from_sketch",
                                {{"sketch", sk.data.at("id")}, {"thickness", 3.0}, {"rootFillet", 40.0}});
    INFO(huge.toJson().dump());
    REQUIRE(huge.ok);
    CHECK(huge.data.at("rootFillet").get<double>() < 40.0);
    REQUIRE(huge.data.at("warnings").size() == 1);
    CHECK(huge.data.at("warnings").at(0).get<std::string>().find("root fillet of 40 mm") != std::string::npos);
    // Absurdly big: the skin is still fitted for the radius actually tried.
    auto absurd = session.execute("surface.fin_from_sketch",
                                  {{"sketch", sk.data.at("id")}, {"thickness", 3.0}, {"rootFillet", 10000.0}});
    INFO(absurd.toJson().dump());
    REQUIRE(absurd.ok);
    CHECK(absurd.data.at("rootFillet").get<double>() <= 6.0);
    CHECK(core::checkMesh(core::weldVertices(
              *session.document().part(huge.data.at("id").get<model::PartId>())->mesh)).watertight());
}

TEST_CASE("Part on surface is a timeline step: edit, follow the sketch, delete, save and reopen", "[commands][fit][timeline]")
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
    auto rect = [](double width) {
        return Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", width}, {"height", 12}}});
    };
    auto sk = session.execute("sketch.create", {{"surface", {{"part", scanId}, {"point", {30, 15, 0}}}}, {"curves", rect(30)}});
    REQUIRE(sk.ok);
    const auto sketchId = sk.data.at("id").get<cad::FeatureId>();

    auto made = session.execute("surface.part_from_sketch", {{"sketch", sketchId}, {"thickness", 2.0}});
    INFO(made.toJson().dump());
    REQUIRE(made.ok);
    CHECK(made.data.at("kind") == "body");
    const auto featureId = made.data.at("feature").get<cad::FeatureId>();
    const auto partId = made.data.at("id").get<model::PartId>();
    const double volume = made.data.at("volume").get<double>();
    CHECK(made.data.at("featureName") == "PartOnSurface1");

    auto timeline = [&] {
        auto r = session.execute("timeline.list");
        REQUIRE(r.ok);
        return r.data;
    };
    auto partVolume = [&]() -> double {
        const model::Part* p = session.document().part(partId);
        return p != nullptr && p->shape ? p->shape->volume() : 0.0;
    };
    {
        const Json tl = timeline();
        REQUIRE(tl.at("features").size() == 2);
        const Json& f = tl.at("features").at(1);
        CHECK(f.at("type") == "part_on_surface");
        CHECK(f.at("status") == "ok");
        CHECK(f.at("report").at("fit").at("withinTolerance") == true);
    }

    // An unrelated step does not fit the scan again.
    const std::size_t runs = session.design().evaluatorRuns();
    REQUIRE(session.execute("feature.box", {{"size", {5, 5, 5}}, {"corner", {100, 0, 0}}}).ok);
    CHECK(session.design().evaluatorRuns() == runs);

    // Edit the step: twice as thick, the same part.
    REQUIRE(session.execute("feature.edit", {{"id", featureId}, {"params", {{"thickness", 4.0}}}}).ok);
    CHECK(partVolume() == Approx(2.0 * volume).epsilon(0.03));
    // Edit the sketch before it: the part follows the wider outline.
    REQUIRE(session.execute("feature.edit", {{"id", sketchId}, {"params", {{"curves", rect(40)}}}}).ok);
    CHECK(partVolume() == Approx(2.0 * volume * 40.0 / 30.0).epsilon(0.04));
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(partVolume() == Approx(volume).epsilon(1e-6));
    CHECK(session.design().evaluatorRuns() == runs + 2); // undo used the cached results

    // A small parameter in an amplified expression: every change of it rebuilds the step.
    REQUIRE(session.execute("parameter.set", {{"name", "f"}, {"expression", "0.000001"}}).ok);
    REQUIRE(session.execute("feature.edit", {{"id", featureId}, {"params", {{"thickness", "f * 2000000"}}}}).ok);
    CHECK(partVolume() == Approx(volume).epsilon(1e-6));
    REQUIRE(session.execute("parameter.set", {{"name", "f"}, {"expression", "0.0000014"}}).ok);
    CHECK(partVolume() == Approx(1.4 * volume).epsilon(0.03));
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    CHECK(partVolume() == Approx(volume).epsilon(1e-6));

    // A broken parameter the step does not use leaves it alone.
    REQUIRE(session.execute("parameter.set", {{"name", "a"}, {"expression", "1"}}).ok);
    REQUIRE(session.execute("parameter.set", {{"name", "b"}, {"expression", "a * 2"}}).ok);
    REQUIRE(session.execute("parameter.remove", {{"name", "a"}}).ok);
    CHECK(timeline().at("features").at(1).at("status") == "ok");
    CHECK(partVolume() == Approx(volume).epsilon(1e-6));
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);
    REQUIRE(session.execute("edit.undo").ok);

    // The outline turned into construction geometry: same edges, no profile, so the step
    // is rebuilt and reports it instead of keeping the old part.
    {
        Json construction = rect(30);
        construction[0]["construction"] = true;
        REQUIRE(session.execute("feature.edit", {{"id", sketchId}, {"params", {{"curves", construction}}}}).ok);
        const Json tl = timeline();
        CHECK(tl.at("features").at(1).at("status") == "error");
        REQUIRE(session.execute("edit.undo").ok);
        CHECK(timeline().at("features").at(1).at("status") == "ok");
    }

    // A thickness that cannot be built is refused and changes nothing.
    CHECK_FALSE(session.execute("feature.edit", {{"id", featureId}, {"params", {{"thickness", -1.0}}}}).ok);
    CHECK(partVolume() == Approx(volume).epsilon(1e-6));

    SECTION("delete and undo")
    {
        REQUIRE(session.execute("feature.delete", {{"id", featureId}}).ok);
        CHECK(session.document().part(partId) == nullptr);
        REQUIRE(session.execute("edit.undo").ok);
        CHECK(partVolume() == Approx(volume).epsilon(1e-6));
    }

    SECTION("two projects opened one after the other: the second scan is not taken for the first")
    {
        const auto stem = std::filesystem::temp_directory_path() / ("aerocad_two_" + std::to_string(std::random_device{}()));
        const auto fileA = core::pathToUtf8(stem.string() + "_a.aerocad");
        const auto fileB = core::pathToUtf8(stem.string() + "_b.aerocad");
        REQUIRE(session.execute("project.save", {{"path", fileA}}).ok);
        REQUIRE(session.execute("part.scale", {{"id", scanId}, {"factor", 1.5}}).ok);
        // Rebuild the step on the scaled scan (changing a part does not replay the timeline).
        REQUIRE(session.execute("feature.edit", {{"id", featureId}, {"params", {{"thickness", 2.5}}}}).ok);
        REQUIRE(session.execute("project.save", {{"path", fileB}}).ok);
        commands::Session other;
        REQUIRE(other.execute("project.open", {{"path", fileA}}).ok);
        REQUIRE(other.execute("project.open", {{"path", fileB}}).ok);
        // Both scans are part 1 at revision 0 once opened; each project's stored result is used.
        CHECK(other.design().evaluatorRuns() == 0);
        std::error_code ec;
        std::filesystem::remove(fileA, ec);
        std::filesystem::remove(fileB, ec);
    }

    SECTION("saved in the project and opened without refitting")
    {
        const auto file = std::filesystem::temp_directory_path() / ("aerocad_timeline_" + std::to_string(std::random_device{}()) + ".aerocad");
        REQUIRE(session.execute("project.save", {{"path", core::pathToUtf8(file)}}).ok);
        commands::Session reopened;
        auto opened = reopened.execute("project.open", {{"path", core::pathToUtf8(file)}});
        INFO(opened.toJson().dump());
        REQUIRE(opened.ok);
        const model::Part* p = reopened.document().part(partId);
        REQUIRE(p != nullptr);
        CHECK(p->kind == model::PartKind::Body);
        REQUIRE(p->shape);
        CHECK(p->shape->volume() == Approx(volume).epsilon(0.01));
        // Its result was stored with the project: opening does not fit the scan again.
        CHECK(reopened.design().evaluatorRuns() == 0);
        // And it can still be edited there.
        REQUIRE(reopened.execute("feature.edit", {{"id", featureId}, {"params", {{"thickness", 4.0}}}}).ok);
        CHECK(reopened.document().part(partId)->shape->volume() == Approx(2.0 * volume).epsilon(0.03));
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
}

TEST_CASE("Part on surface on a timeline body is rebuilt from the body, also when the project opens", "[commands][fit][timeline]")
{
    commands::Session session;
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int nx = 20;
    const int ny = 10;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            v.push_back({2.0 * i, 2.0 * j, -0.01 * (2 * i - 20) * (2 * i - 20)});
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

    // The wing as a timeline body (INSERT > To Body), then a part sketched on that body.
    auto base = session.execute("feature.base_body", {{"part", scanId}});
    INFO(base.toJson().dump());
    REQUIRE(base.ok);
    const auto baseFeature = base.data.at("id").get<cad::FeatureId>();
    model::PartId bodyPart = 0;
    for (const auto& p : session.document().parts())
        if (p.kind == model::PartKind::Body && p.featureId == baseFeature)
            bodyPart = p.id;
    REQUIRE(bodyPart != 0);
    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", bodyPart}, {"point", {20, 10, 0}}}},
                               {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 16}, {"height", 8}}})}});
    INFO(sk.toJson().dump());
    REQUIRE(sk.ok);
    auto made = session.execute("surface.part_from_sketch", {{"sketch", sk.data.at("id")}, {"thickness", 1.5}});
    INFO(made.toJson().dump());
    REQUIRE(made.ok);
    const auto partId = made.data.at("id").get<model::PartId>();
    const double volume = made.data.at("volume").get<double>();
    CHECK(volume > 0.0);

    const auto file = std::filesystem::temp_directory_path() / ("aerocad_onbody_" + std::to_string(std::random_device{}()) + ".aerocad");
    REQUIRE(session.execute("project.save", {{"path", core::pathToUtf8(file)}}).ok);
    commands::Session reopened;
    auto opened = reopened.execute("project.open", {{"path", core::pathToUtf8(file)}});
    INFO(opened.toJson().dump());
    REQUIRE(opened.ok);
    auto tl = reopened.execute("timeline.list");
    REQUIRE(tl.ok);
    for (const auto& f : tl.data.at("features")) {
        INFO(f.dump());
        CHECK(f.at("status") == "ok");
    }
    // The body was not in the document yet while the timeline was rebuilt; the stored
    // result still matches, so nothing is refitted.
    CHECK(reopened.design().evaluatorRuns() == 0);
    const model::Part* p = reopened.document().part(partId);
    REQUIRE(p != nullptr);
    REQUIRE(p->shape);
    CHECK(p->shape->volume() == Approx(volume).epsilon(0.01));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST_CASE("A rejected scale leaves the model, history and dirty state unchanged (CAD-88)", "[commands]")
{
    commands::Session session;
    auto r = session.execute("document.import", {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome.step"}});
    REQUIRE(r.ok);
    const auto id = r.data.at("id").get<std::uint64_t>();
    const Json boundsBefore = session.execute("document.bounds").data.at("bounds");
    const model::Part before = *session.document().part(id);
    const std::size_t historyBefore = session.undoStack().position();
    const bool dirtyBefore = session.project().dirty();

    // The audit's factor: used to abort the process with an uncaught Standard_ConstructionError.
    for (double factor : {1e-310, 1e-7, 1e7}) {
        r = session.execute("part.scale", {{"id", id}, {"factor", factor}});
        CHECK_FALSE(r.ok);
    }
    // Past the schema (as an undo/redo or a future command could): the kernel layer rejects it.
    CHECK_THROWS_AS(geometry::scaled(*before.shape, 1e-310, {}), std::invalid_argument);

    const model::Part* after = session.document().part(id);
    REQUIRE(after != nullptr);
    CHECK(after->revision == before.revision);
    CHECK(after->mesh == before.mesh);
    CHECK(session.undoStack().position() == historyBefore);
    CHECK(session.project().dirty() == dirtyBefore);
    CHECK(session.execute("document.bounds").data.at("bounds") == boundsBefore);

    // The session still works afterwards.
    r = session.execute("part.scale", {{"id", id}, {"factor", 2.0}});
    CHECK(r.ok);
}

TEST_CASE("No exception escapes a command, whatever its type (CAD-88)", "[commands]")
{
    model::Document document;
    model::UndoStack undo;
    commands::CommandContext context{document, undo};
    commands::CommandRegistry registry;
    registry.add({"test.kernel", "Kernel failure", "Throws an Open CASCADE exception.", Json::object()},
                 [](commands::CommandContext&, const Json&) -> Json {
                     throw Standard_ConstructionError("gp_Trsf::SetScale");
                 });
    registry.add({"test.other", "Unknown failure", "Throws a non-standard exception.", Json::object()},
                 [](commands::CommandContext&, const Json&) -> Json { throw 42; });

    auto r = registry.execute(context, "test.kernel", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("Standard_ConstructionError") != std::string::npos);
    CHECK(r.error.find("gp_Trsf::SetScale") != std::string::npos);

    r = registry.execute(context, "test.other", Json::object());
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("unknown error") != std::string::npos);
}

TEST_CASE("A part edit that throws leaves the part untouched (CAD-88)", "[commands]")
{
    model::Document document;
    model::Part part;
    part.name = "A";
    const auto id = document.addPart(part);
    const auto revision = document.part(id)->revision;
    int notifications = 0;
    document.addObserver([&](model::Document::Change, model::PartId) { ++notifications; });
    CHECK_THROWS(document.modifyPart(id, [](model::Part& p) {
        p.name = "half way";
        throw std::runtime_error("kernel failed");
    }));
    CHECK(document.part(id)->name == "A");
    CHECK(document.part(id)->revision == revision);
    CHECK(notifications == 0);
}

TEST_CASE("A fit reports the deviation its tolerance verdict is based on (CAD-110)", "[commands][fit]")
{
    // The whole dome scan: the mesh lies within 0.25 mm of the surface, but the surface strays
    // further from the mesh at the jagged rim and the hole. The verdict uses both directions,
    // so the reported maxDeviation must too.
    commands::Session session;
    auto scan =
        session.execute("document.import", {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome_scan.stl"}});
    REQUIRE(scan.ok);
    const auto id = scan.data.at("id").get<model::PartId>();
    REQUIRE(session.execute("selection.invert", {{"id", id}}).ok); // everything
    auto r = session.execute("surface.fit_selection", {{"id", id}, {"tolerance", 0.25}});
    INFO(r.toJson().dump());
    REQUIRE(r.ok);
    const auto& fit = r.data.at("fit");
    const double both = std::max(fit.at("meshToSurfaceMax").get<double>(), fit.at("surfaceToMeshMax").get<double>());
    CHECK(fit.at("maxDeviation").get<double>() == Approx(both));
    CHECK(fit.at("withinTolerance").get<bool>() == (fit.at("maxDeviation").get<double>() <= 0.25));
    CHECK(fit.at("withinTolerance") == false);
    CHECK(fit.at("meshToSurfaceMax").get<double>() < 0.25); // the number the GUI used to show

    // Scan to solid: the same rule over all patches.
    REQUIRE(session.execute("selection.clear", {{"id", id}}).ok);
    auto whole = session.execute("surface.fit_regions", {{"id", id}, {"tolerance", 0.25}});
    INFO(whole.toJson().dump());
    REQUIRE(whole.ok);
    double worst = 0.0;
    for (const auto& p : whole.data.at("fit").at("patches"))
        if (p.contains("meshToSurfaceMax"))
            worst = std::max({worst, p.at("meshToSurfaceMax").get<double>(), p.at("surfaceToMeshMax").get<double>()});
    CHECK(whole.data.at("fit").at("maxDeviation").get<double>() == Approx(worst));
}

TEST_CASE("A part whose fit misses the tolerance is kept as a marked approximation (CAD-85)", "[commands][surface]")
{
    commands::Session session;
    auto scan = session.execute("document.import",
                                {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome.stl"}});
    REQUIRE(scan.ok);
    const auto scanId = scan.data.at("id").get<model::PartId>();
    const Json square = Json::array({{{"type", "line"}, {"start", {-15, -15}}, {"end", {15, -15}}},
                                     {{"type", "line"}, {"start", {15, -15}}, {"end", {15, 15}}},
                                     {{"type", "line"}, {"start", {15, 15}}, {"end", {-15, 15}}},
                                     {{"type", "line"}, {"start", {-15, 15}}, {"end", {-15, -15}}}});
    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", scanId}, {"point", {0, 0, 45.7}}}}, {"curves", square}});
    REQUIRE(sk.ok);

    // 0.01 mm cannot be met over the flat triangles of a tessellated dome (CAD-141: a clean
    // mesh, so this is missed shape, not the scan's noise).
    auto part = session.execute("surface.part_from_sketch",
                                {{"sketch", sk.data.at("id")}, {"thickness", 2.0}, {"tolerance", 0.01}});
    INFO(part.toJson().dump());
    REQUIRE(part.ok);
    const auto featureId = part.data.at("feature").get<std::uint64_t>();
    auto timeline = session.execute("timeline.list");
    Json feature;
    for (const auto& f : timeline.data.at("features"))
        if (f.at("id") == featureId)
            feature = f;
    REQUIRE(feature.is_object());
    const Json& fit = feature.at("report").at("fit");
    CHECK_FALSE(fit.at("withinTolerance").get<bool>());
    CHECK(fit.at("tolerance").get<double>() == Approx(0.01));
    CHECK(fit.at("p99Deviation").get<double>() > 0.01);
    CHECK(fit.at("maxDeviation").get<double>() >= fit.at("p99Deviation").get<double>());
    CHECK(feature.value("approximate", false));
    const auto& warnings = feature.at("report").at("warnings");
    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings[0].get<std::string>().find("Approximation") != std::string::npos);
    CHECK(part.data.at("warnings")[0].get<std::string>().find("Approximation") != std::string::npos);

    // The export check names it too.
    auto check = session.execute("document.export_check", {{"ids", {part.data.at("id")}}});
    REQUIRE(check.ok);
    INFO(check.toJson().dump());
    CHECK(check.data.at("parts")[0].value("approximate", false));

    // An achievable tolerance: no approximation flag.
    auto loose = session.execute("surface.part_from_sketch",
                                 {{"sketch", sk.data.at("id")}, {"thickness", 2.0}, {"tolerance", 0.5}});
    REQUIRE(loose.ok);
    timeline = session.execute("timeline.list");
    for (const auto& f : timeline.data.at("features"))
        if (f.at("id") == loose.data.at("feature"))
            CHECK_FALSE(f.value("approximate", false));
}

TEST_CASE("A part on a noisy scan is not an approximation when what is left is the scan's noise (CAD-141)",
          "[commands][surface]")
{
    commands::Session session;
    auto scan = session.execute("document.import",
                                {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome_scan.stl"}});
    REQUIRE(scan.ok);
    const Json square = Json::array({{{"type", "rectangle"}, {"corner", {-15, -15}}, {"opposite", {15, 15}}}});
    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", scan.data.at("id")}, {"point", {0, 0, 45.7}}}}, {"curves", square}});
    REQUIRE(sk.ok);
    // The scan has 0.05 mm noise: no surface is within 0.01 mm of it.
    auto part = session.execute("surface.part_from_sketch",
                                {{"sketch", sk.data.at("id")}, {"thickness", 2.0}, {"tolerance", 0.01}});
    INFO(part.toJson().dump());
    REQUIRE(part.ok);
    const Json& fit = part.data.at("fit");
    CHECK(fit.at("p99Deviation").get<double>() > 0.01);
    CHECK(fit.at("scanNoise").get<double>() > 0.005);
    CHECK(fit.at("withinScanNoise").get<bool>());
    CHECK(fit.at("withinTolerance").get<bool>());
    CHECK(part.data.at("warnings").empty());
    auto timeline = session.execute("timeline.list");
    for (const auto& f : timeline.data.at("features"))
        if (f.at("id") == part.data.at("feature"))
            CHECK_FALSE(f.value("approximate", false));
}

TEST_CASE("CAD-85: stacked fins are an approximation when any fin misses the tolerance", "[commands][surface]")
{
    commands::Session session;
    auto scan = session.execute("document.import",
                                {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome_scan.stl"}});
    REQUIRE(scan.ok);
    const Json fin = Json::array({{{"type", "rectangle"}, {"corner", {-2, -8}}, {"opposite", {10, 8}}}});
    auto sk = session.execute("sketch.create",
                              {{"surface", {{"part", scan.data.at("id")}, {"point", {40, 0, 45}}, {"across", true}}},
                               {"curves", fin}});
    REQUIRE(sk.ok);
    // The first fin alone fits within 0.04 mm (p99 about 0.026 mm) ...
    auto one = session.execute("surface.fin_from_sketch",
                               {{"sketch", sk.data.at("id")}, {"thickness", 3.0}, {"tolerance", 0.04}});
    INFO(one.toJson().dump());
    REQUIRE(one.ok);
    REQUIRE(one.data.at("fit").at("withinTolerance").get<bool>());
    // ... the second, 40 mm further down the dome, does not (p99 about 0.06 mm).
    auto two = session.execute("surface.fin_from_sketch", {{"sketch", sk.data.at("id")},
                                                           {"thickness", 3.0},
                                                           {"tolerance", 0.04},
                                                           {"count", 2},
                                                           {"spacing", -40.0}});
    INFO(two.toJson().dump());
    REQUIRE(two.ok);
    const Json& fit = two.data.at("fit");
    CHECK_FALSE(fit.at("withinTolerance").get<bool>());
    CHECK(fit.at("p99Deviation").get<double>() > 0.04);
    bool warned = false;
    for (const auto& w : two.data.at("warnings"))
        warned = warned || w.get<std::string>().find("Approximation") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("CAD-85: fit reports saved before the tolerance was recorded are fitted again on open",
          "[commands][surface]")
{
    const auto file = std::filesystem::temp_directory_path() /
                      ("aerocad_cad85_" + std::to_string(std::random_device{}()) + ".aerocad");
    {
        commands::Session session;
        auto scan = session.execute("document.import",
                                    {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/models/dome.stl"}});
        REQUIRE(scan.ok);
        const Json square = Json::array({{{"type", "rectangle"}, {"corner", {-15, -15}}, {"opposite", {15, 15}}}});
        auto sk = session.execute("sketch.create",
                                  {{"surface", {{"part", scan.data.at("id")}, {"point", {0, 0, 45.7}}}}, {"curves", square}});
        REQUIRE(sk.ok);
        REQUIRE(session.execute("surface.part_from_sketch",
                                {{"sketch", sk.data.at("id")}, {"thickness", 2.0}, {"tolerance", 0.01}})
                    .ok);
        REQUIRE(session.execute("project.save", {{"path", core::pathToUtf8(file)}}).ok);
    }
    // Make it look like a file from before CAD-85: no tolerance, no approximation warning.
    project::ProjectData data = project::loadProject(file);
    bool edited = false;
    for (auto& r : data.timelineResults) {
        Json& report = r.output.report;
        if (report.is_object() && report.contains("fit")) {
            report["fit"].erase("tolerance");
            report["fit"].erase("p99Deviation");
            report["warnings"] = Json::array();
            edited = true;
        }
    }
    REQUIRE(edited);
    project::saveProject(file, data);

    commands::Session session;
    REQUIRE(session.execute("project.open", {{"path", core::pathToUtf8(file)}}).ok);
    auto timeline = session.execute("timeline.list");
    Json feature;
    for (const auto& f : timeline.data.at("features"))
        if (f.contains("report") && f.at("report").contains("fit"))
            feature = f;
    REQUIRE(feature.is_object());
    INFO(feature.dump());
    CHECK(feature.value("approximate", false));
    CHECK(feature.at("report").at("fit").at("tolerance").get<double>() == Approx(0.01));
    CHECK(feature.at("report").at("warnings")[0].get<std::string>().find("Approximation") != std::string::npos);
    std::filesystem::remove(file);
}

TEST_CASE("CAD-80: replacing the project starts a new generation and drops staged imports", "[commands]")
{
    commands::Session session;
    const std::uint64_t first = session.project().generation;
    session.importStage().put("/pending/scan.stl", io::ImportResult{});
    REQUIRE(session.execute("project.new").ok);
    const std::uint64_t second = session.project().generation;
    CHECK(second != first);
    // A file parsed for the old project is not imported into the new one.
    CHECK_FALSE(session.importStage().take("/pending/scan.stl").has_value());

    // Ordinary edits and saves keep the generation; opening a project changes it.
    REQUIRE(session.execute("feature.box", {{"size", {10, 10, 10}}}).ok);
    const auto file = std::filesystem::temp_directory_path() /
                      ("aerocad_generation_" + std::to_string(std::random_device{}()) + ".aerocad");
    REQUIRE(session.execute("project.save", {{"path", core::pathToUtf8(file)}}).ok);
    CHECK(session.project().generation == second);
    REQUIRE(session.execute("project.open", {{"path", core::pathToUtf8(file)}}).ok);
    CHECK(session.project().generation != second);
    std::filesystem::remove(file);
}

TEST_CASE("Parameter errors tell an agent what the command accepts", "[commands]")
{
    commands::Session s;
    auto r = s.execute("document.export", {{"id", 1}, {"path", "/tmp/x.stl"}});
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.find("did you mean 'ids'?") != std::string::npos);
    CHECK(r.error.find("accepted: ") != std::string::npos);
    r = s.execute("feature.box", Json::object());
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.find("size: required (") != std::string::npos); // with the parameter's description
    r = s.execute("geometry.measure", {{"kind", "volume"}});
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.find("\"volume\" not allowed (one of") != std::string::npos);
    r = s.execute("geometry.measure", {{"kind", 3}});
    REQUIRE_FALSE(r.ok);
    CHECK(r.error.find("expected string, got number") != std::string::npos);
}

TEST_CASE("project.get_context says what each part is and which feature made it", "[commands]")
{
    commands::Session s;
    REQUIRE(s.execute("feature.box", {{"size", {10, 20, 30}}}).ok);
    const auto r = s.execute("project.get_context");
    REQUIRE(r.ok);
    const auto& part = r.data.at("parts").at(0);
    CHECK(part.at("kind") == "body");
    CHECK(part.at("featureId") == 1);
}
