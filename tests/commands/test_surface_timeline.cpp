// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/core/Path.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <cmath>
#include <memory>
#include <random>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {
Json run(commands::Session& session, const std::string& command, const Json& params = Json::object())
{
    const auto result = session.execute(command, params);
    INFO(command << ": " << result.toJson().dump());
    REQUIRE(result.ok);
    return result.data;
}

model::PartId sheet(commands::Session& session)
{
    auto mesh = std::make_shared<core::TriangleMesh>();
    constexpr unsigned n = 12;
    for (unsigned y = 0; y <= n; ++y)
        for (unsigned x = 0; x <= n; ++x)
            mesh->addVertex({2.0 * x, 2.0 * y, 0.0});
    for (unsigned y = 0; y < n; ++y)
        for (unsigned x = 0; x < n; ++x) {
            const auto a = y * (n + 1) + x;
            mesh->addTriangle(a, a + 1, a + n + 2);
            mesh->addTriangle(a, a + n + 2, a + n + 1);
        }
    model::Part part;
    part.name = "Reference sheet";
    part.mesh = mesh;
    const auto id = session.document().addPart(part);
    run(session, "selection.brush", {{"id", id}, {"points", {{12, 12, 0}}}, {"radius", 40}});
    return id;
}
} // namespace

TEST_CASE("Fit clearance and thickness stay independently editable across history and projects",
          "[commands][surface-timeline][timeline]")
{
    commands::Session session;
    const auto source = sheet(session);
    const Json fit = run(session, "surface.fit_selection", {{"id", source}, {"tolerance", 0.1}, {"method", "plane"}});
    const Json offset = run(session, "surface.offset", {{"id", fit.at("id")}, {"distance", 2}});
    const Json solid = run(session, "surface.thicken", {{"id", offset.at("id")}, {"thickness", 3}});
    const Json timeline = run(session, "timeline.list");
    REQUIRE(timeline.at("features").size() == 3);
    REQUIRE(fit.at("kind") == "body");
    REQUIRE(fit.at("featureId").get<cad::FeatureId>() != 0);
    const auto offsetFeature = offset.at("feature").get<cad::FeatureId>();
    const auto solidFeature = solid.at("feature").get<cad::FeatureId>();
    const auto solidPart = solid.at("id").get<model::PartId>();
    const auto bounds = [&] { return session.document().part(solidPart)->mesh->bounds(); };
    const double volume = solid.at("volume").get<double>();
    CHECK(bounds().min.z == Approx(2).margin(0.001));
    CHECK(bounds().max.z == Approx(5).margin(0.001));
    run(session, "selection.clear", {{"id", source}});
    run(session, "feature.edit", {{"id", offsetFeature}, {"params", {{"distance", 4}}}});
    CHECK(bounds().min.z == Approx(4).margin(0.001));
    CHECK(bounds().max.z == Approx(7).margin(0.001));
    CHECK(session.design().feature(solidFeature)->params.at("thickness") == 3);
    run(session, "feature.edit", {{"id", solidFeature}, {"params", {{"thickness", 6}}}});
    CHECK(bounds().max.z == Approx(10).margin(0.001));
    CHECK(session.document().part(solidPart)->shape->volume() == Approx(2 * volume).epsilon(0.001));
    run(session, "edit.undo");
    run(session, "edit.undo");
    CHECK(bounds().min.z == Approx(2).margin(0.001));
    CHECK(bounds().max.z == Approx(5).margin(0.001));
    run(session, "edit.redo");
    CHECK(bounds().max.z == Approx(7).margin(0.001));

    run(session, "variant.create");
    run(session, "feature.edit", {{"id", solidFeature}, {"params", {{"thickness", 6}}}});
    run(session, "variant.activate", {{"variant", "A"}});
    CHECK(bounds().max.z == Approx(7).margin(0.001));
    const auto path = std::filesystem::temp_directory_path() /
                      ("aerocad_surface_timeline_" + std::to_string(std::random_device{}()) + ".aerocad");
    run(session, "project.save", {{"path", core::pathToUtf8(path)}});
    commands::Session reopened;
    run(reopened, "project.open", {{"path", core::pathToUtf8(path)}});
    std::filesystem::remove(path);
    REQUIRE(reopened.design().features().size() == 3);
    // The fitted scan result survives opening; exact downstream shapes may be
    // normalised by B-rep serialisation and cheaply rebuilt from that cached fit.
    const auto results = session.design().currentResults();
    const auto reopenedResults = reopened.design().currentResults();
    REQUIRE(!results.empty());
    REQUIRE(!reopenedResults.empty());
    CHECK(results.front().key == reopenedResults.front().key);
    CHECK(reopened.design().evaluatorRuns() <= 2);
    run(reopened, "variant.activate", {{"variant", "B"}});
    CHECK(reopened.document().part(solidPart)->mesh->bounds().max.z == Approx(10).margin(0.001));
    run(reopened, "feature.edit", {{"id", offsetFeature}, {"params", {{"distance", 1}}}});
    CHECK(reopened.document().part(solidPart)->mesh->bounds().max.z == Approx(7).margin(0.001));
}

TEST_CASE("Region fitting and sketch trimming retain source and sketch dependencies",
          "[commands][surface-timeline][timeline]")
{
    commands::Session session;
    const auto source = sheet(session);
    const Json fit = run(session, "surface.fit_regions", {{"id", source}, {"useSelection", false}});
    const auto curves = [](double width) {
        return Json::array({{{"type", "center_rectangle"}, {"center", {12, 12}}, {"width", width}, {"height", 10}}});
    };
    const Json sketch = run(session, "sketch.create", {{"plane", "XY"}, {"curves", curves(10)}});
    const Json trim = run(session, "surface.trim_by_sketch", {{"id", fit.at("id")}, {"sketch", sketch.at("id")}});
    const Json solid = run(session, "surface.thicken", {{"id", trim.at("id")}, {"thickness", 3}});
    REQUIRE(run(session, "timeline.list").at("features").size() == 4);
    const auto part = solid.at("id").get<model::PartId>();
    CHECK(session.document().part(part)->shape->volume() == Approx(300).epsilon(0.001));
    run(session, "feature.edit", {{"id", sketch.at("id")}, {"params", {{"curves", curves(16)}}}});
    CHECK(session.document().part(part)->shape->volume() == Approx(480).epsilon(0.001));
    run(session, "feature.suppress", {{"id", fit.at("feature")}, {"suppressed", true}});
    CHECK(session.document().part(part) == nullptr);
    run(session, "edit.undo");
    REQUIRE(session.document().part(part) != nullptr);
    CHECK(session.document().part(part)->shape->volume() == Approx(480).epsilon(0.001));
}

TEST_CASE("A saved fit selection rejects remeshing instead of silently selecting different triangles",
          "[commands][surface-timeline][timeline]")
{
    commands::Session session;
    const auto source = sheet(session);
    const Json fit = run(session, "surface.fit_selection", {{"id", source}, {"method", "plane"}});
    const Json offset = run(session, "surface.offset", {{"id", fit.at("id")}, {"distance", 2}});
    const auto original = session.document().part(source)->mesh;
    auto vertices = original->vertices();
    auto triangles = original->triangles();
    // Same counts and in-range indices, but different connectivity: even this must fail.
    std::swap(triangles[0][1], triangles[0][2]);
    const auto replaced = std::make_shared<const core::TriangleMesh>(std::move(vertices), std::move(triangles));
    session.document().modifyPart(source, [&](model::Part& part) { part.mesh = replaced; });
    session.design().regenerate();
    session.design().syncDocument(session.document());
    CHECK(session.design().status(fit.at("feature")).state == cad::FeatureStatus::State::Error);
    CHECK(session.design().status(fit.at("feature")).message.find("topology changed") != std::string::npos);
    CHECK(session.document().part(offset.at("id")) == nullptr);
    session.document().modifyPart(source, [&](model::Part& part) { part.mesh = original; });
    session.design().regenerate();
    session.design().syncDocument(session.document());
    REQUIRE(session.document().part(offset.at("id")) != nullptr);
    CHECK(session.design().status(fit.at("feature")).state == cad::FeatureStatus::State::Ok);
}

TEST_CASE("Legacy baked surface results remain explicitly frozen after reopening", "[commands][surface-timeline][project]")
{
    commands::Session session;
    model::Part legacy;
    legacy.name = "Legacy thickened result";
    legacy.source = "surface.thicken of part 42";
    legacy.shape = geometry::makeBox(10, 10, 3);
    legacy.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(*legacy.shape));
    const auto id = session.document().addPart(legacy);
    const auto path = std::filesystem::temp_directory_path() /
                      ("aerocad_frozen_surface_" + std::to_string(std::random_device{}()) + ".aerocad");
    run(session, "project.save", {{"path", core::pathToUtf8(path)}});
    commands::Session reopened;
    run(reopened, "project.open", {{"path", core::pathToUtf8(path)}});
    std::filesystem::remove(path);
    CHECK(reopened.design().features().empty());
    REQUIRE(reopened.document().part(id) != nullptr);
    CHECK(model::frozenSurfaceResult(*reopened.document().part(id)));
    const Json part = run(reopened, "document.parts").at("parts").at(0);
    CHECK(part.at("kind") == "imported");
    CHECK(part.at("featureId") == 0);
    CHECK(part.at("frozen") == true);
    legacy.source = "surface.stl";
    CHECK_FALSE(model::frozenSurfaceResult(legacy)); // An imported filename is not operation provenance.
}

TEST_CASE("Imported source edits rebuild every surface descendant at the Session boundary",
          "[commands][surface-timeline][source-regeneration]")
{
    commands::Session session;
    const auto source = sheet(session);
    const Json fit = run(session, "surface.fit_selection", {{"id", source}, {"method", "plane"}});
    const Json offset = run(session, "surface.offset", {{"id", fit.at("id")}, {"distance", 2}});
    const Json solid = run(session, "surface.thicken", {{"id", offset.at("id")}, {"thickness", 3}});
    const auto part = solid.at("id").get<model::PartId>();
    const auto top = [&] { return session.document().part(part)->mesh->bounds().max.z; };
    CHECK(top() == Approx(5).margin(0.001));
    run(session, "part.transform", {{"id", source}, {"translate", {0, 0, 10}}});
    CHECK(top() == Approx(15).margin(0.001));
    run(session, "edit.undo");
    CHECK(top() == Approx(5).margin(0.001));
    run(session, "edit.redo");
    CHECK(top() == Approx(15).margin(0.001));
    run(session, "part.remove", {{"id", source}});
    CHECK(session.document().part(part) == nullptr);
    CHECK(session.design().status(fit.at("feature")).state == cad::FeatureStatus::State::Error);
    run(session, "edit.undo");
    REQUIRE(session.document().part(part) != nullptr);
    CHECK(top() == Approx(15).margin(0.001));
}

TEST_CASE("Surface evaluator cache includes exact source geometry even with an unchanged display mesh",
          "[commands][surface-timeline][source-regeneration]")
{
    commands::Session session;
    const auto scan = sheet(session);
    const Json fit = run(session, "surface.fit_selection", {{"id", scan}, {"method", "plane"}});
    const auto* fitted = session.document().part(fit.at("id"));
    model::Part reference;
    reference.name = "Exact reference surface";
    reference.shape = fitted->shape;
    SECTION("unchanged display mesh") { reference.mesh = fitted->mesh; }
    SECTION("exact shape without display mesh") { reference.mesh.reset(); }
    const auto source = session.document().addPart(reference);
    const Json offset = run(session, "surface.offset", {{"id", source}, {"distance", 2}});
    const Json solid = run(session, "surface.thicken", {{"id", offset.at("id")}, {"thickness", 3}});
    const auto part = solid.at("id").get<model::PartId>();
    CHECK(session.document().part(part)->mesh->bounds().max.z == Approx(5).margin(0.001));
    const auto originalMesh = session.document().part(source)->mesh;
    const auto moved = geometry::translated(*reference.shape, {0, 0, 10});
    session.document().modifyPart(source, [&](model::Part& p) { p.shape = moved; });
    run(session, "timeline.list"); // Session synchronises the pending source edit.
    CHECK(session.document().part(source)->mesh == originalMesh);
    CHECK(session.document().part(part)->mesh->bounds().max.z == Approx(15).margin(0.001));
}

TEST_CASE("Approximate fitted references retain measured quality through offset and thickness",
          "[commands][surface-timeline][fit-tolerance]")
{
    commands::Session session;
    const auto source = sheet(session);
    const auto original = session.document().part(source)->mesh;
    auto vertices = original->vertices();
    for (auto& v : vertices)
        v.z = 0.2 * std::sin(v.x) * std::sin(v.y);
    const auto noisy = std::make_shared<const core::TriangleMesh>(std::move(vertices), original->triangles());
    session.document().modifyPart(source, [&](model::Part& p) { p.mesh = noisy; });
    const Json fit = run(session, "surface.fit_selection",
                         {{"id", source}, {"method", "plane"}, {"gridSize", 4}, {"tolerance", 0.0001}});
    REQUIRE_FALSE(fit.at("fit").at("withinTolerance").get<bool>());
    CHECK(fit.at("fit").at("maxDeviation").get<double>() > 0.0001);
    const Json offset = run(session, "surface.offset", {{"id", fit.at("id")}, {"distance", 2}});
    const Json solid = run(session, "surface.thicken", {{"id", offset.at("id")}, {"thickness", 3}});
    CHECK(solid.at("fit").at("maxDeviation") == fit.at("fit").at("maxDeviation"));
    auto check = run(session, "document.export_check", {{"ids", {solid.at("id")}}});
    CHECK(check.at("parts").at(0).value("approximate", false));
    const std::string warnings = check.at("parts").at(0).at("warnings").dump();
    CHECK(warnings.find("up to 0 mm") == std::string::npos);
    CHECK(warnings.find("reference fit") != std::string::npos);
    CHECK(solid.at("fitSourceFeature") == fit.at("feature"));
    // A looser tolerance can keep the same geometry but changes its quality report;
    // descendant caches must not retain the old approximation label.
    run(session, "feature.edit", {{"id", fit.at("feature")}, {"params", {{"tolerance", 1.0}}}});
    check = run(session, "document.export_check", {{"ids", {solid.at("id")}}});
    CHECK_FALSE(check.at("parts").at(0).value("approximate", false));
}
