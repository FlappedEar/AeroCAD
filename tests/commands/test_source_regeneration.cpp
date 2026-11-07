// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <random>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {

Json run(commands::Session& session, const std::string& name, const Json& params = Json::object())
{
    const auto result = session.execute(name, params);
    INFO(name << " " << params.dump() << " -> " << result.error);
    REQUIRE(result.ok);
    return result.data;
}

struct SourceBody {
    commands::Session session;
    model::PartId source = 0;
    model::PartId part = 0;
    cad::FeatureId feature = 0;

    SourceBody()
    {
        source = run(session, "document.import",
                     {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl"}})
                     .at("id").get<model::PartId>();
        const auto body = run(session, "feature.base_body", {{"part", source}});
        feature = body.at("id").get<cad::FeatureId>();
        part = body.at("bodies")[0].at("partId").get<model::PartId>();
    }

    void checkSize(double size)
    {
        REQUIRE(session.document().part(part) != nullptr);
        CHECK(session.document().part(source)->mesh->bounds().size().x == Approx(size));
        CHECK(session.document().part(part)->mesh->bounds().size().x == Approx(size));
        CHECK(session.design().bodies().at(feature).shape.bounds().size().x == Approx(size));
        CHECK(session.design().bodies().at(feature).shape.volume() == Approx(size * size * size / 6.0));
        CHECK(session.design().status(feature).state == cad::FeatureStatus::State::Ok);
    }
};

struct TempProject {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("aerocad_source_" + std::to_string(std::random_device{}()) + ".aerocad");
    ~TempProject()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("Imported source scale rebuilds the body immediately and undo remains one step", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    const auto historySize = s.undoStack().size();
    run(s, "part.scale", {{"id", scene.source}, {"factor", 2}, {"origin", {0, 0, 0}}});
    scene.checkSize(20);
    CHECK(s.undoStack().size() == historySize + 1);
    run(s, "edit.undo");
    scene.checkSize(10);
    run(s, "edit.redo");
    scene.checkSize(20);

    // Queries and a failed source edit do not regenerate, dirty the project or add history.
    const auto revision = s.document().revision();
    const auto changes = s.project().changes;
    run(s, "timeline.list");
    run(s, "document.parts");
    CHECK_FALSE(s.execute("part.scale", {{"id", scene.source}, {"factor", -1}}).ok);
    CHECK(s.document().revision() == revision);
    CHECK(s.project().changes == changes);
    CHECK(s.undoStack().size() == historySize + 1);
}

TEST_CASE("Source changes keep saved and reopened geometry identical", "[commands][source][project]")
{
    TempProject project;
    SourceBody scene;
    auto& s = scene.session;
    run(s, "part.scale", {{"id", scene.source}, {"factor", 2}, {"origin", {0, 0, 0}}});
    const auto before = s.design().bodies().at(scene.feature).shape;
    run(s, "project.save", {{"path", core::pathToUtf8(project.path)}});
    commands::Session reopened;
    run(reopened, "project.open", {{"path", core::pathToUtf8(project.path)}});
    const auto& after = reopened.design().bodies().at(scene.feature).shape;
    CHECK(after.volume() == Approx(before.volume()));
    CHECK(after.bounds().size().x == Approx(before.bounds().size().x));
    CHECK(after.volume() == Approx(8000.0 / 6.0));
    REQUIRE(reopened.document().part(scene.part) != nullptr);
    CHECK(reopened.document().part(scene.part)->featureId == scene.feature);
}

TEST_CASE("Removing an imported source invalidates descendants and undo restores their identities",
          "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    const auto cut = run(s, "feature.box", {{"corner", {-1, -1, -1}}, {"size", {3, 3, 3}}, {"operation", "cut"}})
                         .at("id").get<cad::FeatureId>();
    const double volume = s.design().bodies().at(scene.feature).shape.volume();
    run(s, "part.remove", {{"id", scene.source}});
    CHECK(s.design().status(scene.feature).state == cad::FeatureStatus::State::Error);
    CHECK(s.design().status(cut).state == cad::FeatureStatus::State::Error);
    CHECK(s.document().part(scene.part) == nullptr);
    CHECK(s.design().bodies().empty());
    run(s, "edit.undo");
    REQUIRE(s.document().part(scene.part) != nullptr);
    CHECK(s.document().part(scene.part)->featureId == scene.feature);
    CHECK(s.design().bodies().at(scene.feature).shape.volume() == Approx(volume));
    CHECK(s.design().status(cut).state == cad::FeatureStatus::State::Ok);
    run(s, "edit.redo");
    CHECK(s.document().part(scene.part) == nullptr);
    CHECK(s.design().status(scene.feature).state == cad::FeatureStatus::State::Error);
}

TEST_CASE("Source transforms and calibration regenerate the complete downstream body", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    SECTION("translation and rotation")
    {
        run(s, "part.transform", {{"id", scene.source}, {"translate", {30, 20, 10}},
                                 {"axis", {0, 0, 1}}, {"angle", 90}, {"center", {0, 0, 0}}});
        const auto bounds = s.design().bodies().at(scene.feature).shape.bounds();
        CHECK(bounds.min.x == Approx(20));
        CHECK(bounds.min.y == Approx(20));
        CHECK(bounds.min.z == Approx(10));
        scene.checkSize(10);
    }
    SECTION("calibration")
    {
        run(s, "part.calibrate", {{"id", scene.source}, {"a", {0, 0, 0}}, {"b", {10, 0, 0}},
                                 {"distance", 20}});
        scene.checkSize(20);
    }
    SECTION("unit conversion")
    {
        run(s, "part.convert_units", {{"id", scene.source}, {"from", "cm"}});
        scene.checkSize(100);
    }
    run(s, "edit.undo");
    scene.checkSize(10);
    CHECK(s.design().bodies().at(scene.feature).shape.bounds().min.x == Approx(0).margin(1e-6));
}

TEST_CASE("Repairing a source hole updates its base body topology immediately", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    const auto closed = s.document().part(scene.source)->mesh;
    auto triangles = closed->triangles();
    triangles.pop_back();
    s.document().modifyPart(scene.source, [&](model::Part& p) {
        p.mesh = std::make_shared<const core::TriangleMesh>(closed->vertices(), triangles);
    });
    run(s, "document.parts");
    REQUIRE(s.design().bodies().at(scene.feature).shape.faceCount() == 3);
    REQUIRE(s.design().bodies().at(scene.feature).shape.solidCount() == 0);
    const auto repaired = run(s, "mesh.repair", {{"id", scene.source}, {"maxHoleEdges", 3}});
    REQUIRE(repaired.at("filledHoles") == 1);
    CHECK(s.design().bodies().at(scene.feature).shape.faceCount() == 4);
    CHECK(s.design().bodies().at(scene.feature).shape.solidCount() == 1);
    scene.checkSize(10);
    run(s, "edit.undo");
    CHECK(s.design().bodies().at(scene.feature).shape.faceCount() == 3);
    run(s, "edit.redo");
    CHECK(s.design().bodies().at(scene.feature).shape.faceCount() == 4);
}

TEST_CASE("Source previews regenerate and cancellation preserves redo and clean state", "[commands][source][preview]")
{
    SourceBody scene;
    auto& s = scene.session;
    run(s, "part.scale", {{"id", scene.source}, {"factor", 2}, {"origin", {0, 0, 0}}});
    run(s, "edit.undo");
    const auto historySize = s.undoStack().size();
    const auto changes = s.project().changes;
    const auto position = s.undoStack().position();
    REQUIRE(s.preview("part.scale", {{"id", scene.source}, {"factor", 3}, {"origin", {0, 0, 0}}}).ok);
    scene.checkSize(30);
    REQUIRE(s.cancelPreview());
    scene.checkSize(10);
    CHECK(s.undoStack().size() == historySize);
    CHECK(s.undoStack().position() == position);
    CHECK(s.project().changes == changes);
    run(s, "edit.redo");
    scene.checkSize(20);
}

TEST_CASE("A failed source rebuild reports the committed change and permits undo or preview cancellation",
          "[commands][source][preview]")
{
    SourceBody scene;
    auto& s = scene.session;
    // A non-standard exception models an evaluator/kernel failure outside Design's
    // ordinary feature-error handling. It must not escape the Session boundary.
    s.design().setFeatureEvaluator("part_on_surface", {
        [&](const cad::Feature&) -> std::string {
            if (s.document().part(scene.source)->mesh->bounds().size().x > 15)
                throw 77;
            return "original source";
        },
        [](const cad::Feature&, const cad::Design&) {
            cad::FeatureEvaluator::Output out;
            out.bodies.emplace_back(geometry::makeBox(1, 1, 1), "Dependent");
            return out;
        }});
    s.design().addFeature("part_on_surface", Json::object());
    s.design().syncDocument(s.document());
    const auto position = s.undoStack().position();
    const auto changes = s.project().changes;
    const Json params{{"id", scene.source}, {"factor", 2}, {"origin", {0, 0, 0}}};
    SECTION("committed edit can be undone despite a pending failed rebuild")
    {
        const auto result = s.execute("part.scale", params);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("source change remains applied") != std::string::npos);
        CHECK(s.project().changes == changes + 1);
        CHECK(s.undoStack().position() == position + 1);
        CHECK_FALSE(s.execute("document.parts").ok); // retry cannot expose stale derived geometry
        run(s, "edit.undo");
    }
    SECTION("a failed preview retains its rollback")
    {
        CHECK_FALSE(s.preview("part.scale", params).ok);
        REQUIRE(s.previewActive());
        CHECK(s.cancelPreview());
        CHECK(s.project().changes == changes);
    }
    CHECK(s.undoStack().position() == position);
    scene.checkSize(10);
    run(s, "document.parts");
}

TEST_CASE("Source metadata edits do not regenerate geometry", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    const auto sourceRevision = s.document().importedGeometryRevision();
    const auto bodyRevision = s.document().part(scene.part)->revision;
    const auto shape = s.design().bodies().at(scene.feature).shape;
    run(s, "part.rename", {{"id", scene.source}, {"name", "Renamed source"}});
    run(s, "part.set_visible", {{"id", scene.source}, {"visible", true}});
    run(s, "selection.set", {{"id", scene.source}, {"triangles", {0}}});
    run(s, "document.parts");
    run(s, "timeline.list");
    CHECK(s.document().importedGeometryRevision() == sourceRevision);
    CHECK(s.document().part(scene.part)->revision == bodyRevision);
    CHECK(s.design().bodies().at(scene.feature).shape.impl() == shape.impl());
}

TEST_CASE("Queries from document observers do not regenerate inside a source mutation", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    bool inSourceNotification = false;
    int bodyUpdates = 0;
    s.document().addObserver([&](model::Document::Change change, model::PartId id) {
        if (change != model::Document::Change::Modified)
            return;
        if (id == scene.source) {
            inSourceNotification = true;
            run(s, "document.parts");
            inSourceNotification = false;
        } else if (id == scene.part) {
            CHECK_FALSE(inSourceNotification);
            ++bodyUpdates;
            run(s, "timeline.list");
        }
    });
    run(s, "part.scale", {{"id", scene.source}, {"factor", 2}, {"origin", {0, 0, 0}}});
    scene.checkSize(20);
    CHECK(bodyUpdates == 1);
}

TEST_CASE("Replacing a source with the same id and revision never reuses stale base geometry", "[commands][source]")
{
    SourceBody scene;
    auto& s = scene.session;
    auto replacement = *s.document().part(scene.source);
    // Exercise a replace operation without a query between its remove/add phases.
    // The old per-part revision is intentionally reused (as with a loaded snapshot).
    auto larger = std::make_shared<core::TriangleMesh>(*replacement.mesh);
    larger->scale(2, {});
    replacement.mesh = larger;
    s.document().removePart(scene.source);
    s.document().addPart(replacement);
    run(s, "document.parts");
    scene.checkSize(20);
}

TEST_CASE("Exact shape replacement invalidates a base body even when the mesh is unchanged", "[commands][source]")
{
    commands::Session s;
    model::Part source;
    source.name = "Exact box";
    source.shape = geometry::makeBox(10, 10, 10);
    source.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(*source.shape));
    const auto id = s.document().addPart(source);
    const auto feature = run(s, "feature.base_body", {{"part", id}}).at("id").get<cad::FeatureId>();
    s.document().modifyPart(id, [](model::Part& p) { p.shape = geometry::makeBox(20, 10, 10); });
    TempProject project;
    run(s, "project.save", {{"path", core::pathToUtf8(project.path)}});
    CHECK(s.design().bodies().at(feature).shape.volume() == Approx(2000));
    commands::Session reopened;
    run(reopened, "project.open", {{"path", core::pathToUtf8(project.path)}});
    CHECK(reopened.design().bodies().at(feature).shape.volume() == Approx(2000));
}

TEST_CASE("Scan-backed surface features follow source edits and persist only current results",
          "[commands][source][fit]")
{
    commands::Session s;
    std::vector<core::Vec3> vertices;
    std::vector<core::TriangleMesh::Triangle> triangles;
    constexpr int nx = 40, ny = 20;
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            vertices.push_back({double(i), double(j), -0.005 * (i - 20) * (i - 20)});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (nx + 1) + i); };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            triangles.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            triangles.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    model::Part scan;
    scan.name = "Scan";
    scan.mesh = std::make_shared<const core::TriangleMesh>(std::move(vertices), std::move(triangles));
    const auto source = s.document().addPart(scan);
    const auto curves = Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}},
                                     {"width", 20}, {"height", 10}}});
    const auto sketch = run(s, "sketch.create",
                            {{"surface", {{"part", source}, {"point", {20, 10, 0}}}}, {"curves", curves}}).at("id");
    const auto made = run(s, "surface.part_from_sketch", {{"sketch", sketch}, {"thickness", 2.0}});
    const auto feature = made.at("feature").get<cad::FeatureId>();
    const auto part = made.at("id").get<model::PartId>();
    const auto before = s.document().part(part)->shape->bounds();
    const auto originalBrep = geometry::toBrep(*s.document().part(part)->shape, false);
    const auto runs = s.design().evaluatorRuns();
    run(s, "part.transform", {{"id", source}, {"translate", {0, 0, 5}}});
    REQUIRE(s.document().part(part) != nullptr);
    CHECK(s.design().evaluatorRuns() == runs + 1);
    CHECK(s.document().part(part)->shape->bounds().max.z == Approx(before.max.z + 5).margin(0.05));
    const auto changed = *s.document().part(part)->shape;
    TempProject project;
    run(s, "project.save", {{"path", core::pathToUtf8(project.path)}});
    commands::Session reopened;
    run(reopened, "project.open", {{"path", core::pathToUtf8(project.path)}});
    CHECK(reopened.design().evaluatorRuns() == 0);
    REQUIRE(reopened.document().part(part) != nullptr);
    // Display bounds include tessellation deflection, and B-rep text includes OCCT's
    // mutable Checked flags. Compare exact solid geometry by its symmetric difference.
    const auto& openedShape = *reopened.document().part(part)->shape;
    CHECK(geometry::boolean(changed, openedShape, geometry::BooleanOp::Cut).volume() == Approx(0).margin(1e-6));
    CHECK(geometry::boolean(openedShape, changed, geometry::BooleanOp::Cut).volume() == Approx(0).margin(1e-6));
    CHECK(openedShape.volume() == Approx(changed.volume()));
    run(s, "part.remove", {{"id", source}});
    CHECK(s.design().status(feature).state == cad::FeatureStatus::State::Error);
    CHECK(s.document().part(part) == nullptr);
    run(s, "edit.undo");
    REQUIRE(s.document().part(part) != nullptr);
    CHECK(s.document().part(part)->featureId == feature);
    CHECK(s.document().part(part)->shape->volume() == Approx(changed.volume()));
    run(s, "edit.undo");
    const bool undoneGeometryMatches = geometry::toBrep(*s.document().part(part)->shape, false) == originalBrep;
    CHECK(undoneGeometryMatches);
}
