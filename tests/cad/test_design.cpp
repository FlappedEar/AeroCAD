// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/Design.hpp"
#include "aerocad/cad/Expression.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/model/Part.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using namespace aerocad;
using cad::Design;
using cad::Json;
using Catch::Approx;

namespace {

Json rectSketch(double w, double h, const char* plane = "XY")
{
    return Json{{"plane", plane},
                {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", w}, {"height", h}}})}};
}

double bodyVolume(const Design& d, cad::FeatureId id)
{
    return d.bodies().at(id).shape.volume();
}

} // namespace

TEST_CASE("Expressions", "[cad][expression]")
{
    using cad::evaluateExpression;
    CHECK(evaluateExpression("1 + 2 * 3") == Approx(7));
    CHECK(evaluateExpression("(1 + 2) * 3") == Approx(9));
    CHECK(evaluateExpression("-2 ^ 2") == Approx(-4));
    CHECK(evaluateExpression("2 ^ 3 ^ 2") == Approx(512));
    CHECK(evaluateExpression("10 mm + 1 cm") == Approx(20));
    CHECK(evaluateExpression("1 in") == Approx(25.4));
    CHECK(evaluateExpression("0.5m") == Approx(500));
    CHECK(evaluateExpression("sqrt(16) + max(1, 5, 3) + cos(60)") == Approx(9.5));
    CHECK(evaluateExpression("2 * pi") == Approx(2 * std::numbers::pi));
    CHECK(evaluateExpression("1.5e1") == Approx(15));
    const auto lookup = [](std::string_view n) -> std::optional<double> {
        if (n == "width")
            return 40.0;
        return std::nullopt;
    };
    CHECK(evaluateExpression("width / 2", lookup) == Approx(20));
    CHECK_THROWS_AS(evaluateExpression("height", lookup), cad::ExpressionError);
    CHECK_THROWS_AS(evaluateExpression("1 +"), cad::ExpressionError);
    CHECK_THROWS_AS(evaluateExpression("(1"), cad::ExpressionError);
    CHECK_THROWS_AS(evaluateExpression("1 / 0"), cad::ExpressionError);
    CHECK_THROWS_AS(evaluateExpression("2 x"), cad::ExpressionError);
}

TEST_CASE("Sketch and extrude a new body", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature("sketch", rectSketch(40, 20));
    REQUIRE(d.sketches().at(sk).geometry.profiles.size() == 1);
    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", 10}});
    REQUIRE(d.bodies().size() == 1);
    CHECK(bodyVolume(d, ex) == Approx(8000));
    CHECK(d.bodies().at(ex).name == "Body1");
    CHECK(d.feature(ex)->name == "Extrude1");
    CHECK(d.sketches().at(sk).consumed);
}

TEST_CASE("Extrude taper angle", "[cad][design]")
{
    Design d;
    d.setParameter("draft", "3");
    const auto sk = d.addFeature("sketch", rectSketch(40, 20));
    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", 10}, {"taper", "draft"}});
    CHECK(bodyVolume(d, ex) < 8000);
    CHECK(bodyVolume(d, ex) > 7000);
    const auto sym = d.addFeature("extrude", {{"sketch", sk}, {"distance", 5}, {"extent", "symmetric"}, {"taper", 3},
                                              {"operation", "new_body"}});
    // Each 5 mm half narrows from the full profile, so less is drafted away than over 10 mm.
    CHECK(bodyVolume(d, sym) > bodyVolume(d, ex));
    CHECK(bodyVolume(d, sym) < 8000);
    CHECK_THROWS_AS(d.editFeature(ex, {{"taper", 60}}), cad::DesignError); // closes up
    CHECK(d.evaluate(d.feature(ex)->params.at("taper")) == Approx(3));
    CHECK_THROWS_AS(d.addFeature("extrude", {{"sketch", sk}, {"extent", "to_object"}, {"to", {{"body", ex}}},
                                             {"taper", 2}}),
                    cad::DesignError);
}

TEST_CASE("Failing features are rejected and leave the design unchanged", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature("sketch", rectSketch(10, 10));
    CHECK_THROWS_AS(d.addFeature("extrude", {{"sketch", sk}}), cad::DesignError); // no distance
    CHECK_THROWS_AS(d.addFeature("extrude", {{"sketch", 99}, {"distance", 1}}), cad::DesignError);
    CHECK_THROWS_AS(d.addFeature("nonsense", Json::object()), cad::DesignError);
    CHECK(d.features().size() == 1);
    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", 5}});
    CHECK_THROWS_AS(d.editFeature(ex, {{"distance", 0}}), cad::DesignError);
    CHECK(d.evaluate(d.feature(ex)->params.at("distance")) == Approx(5));
    CHECK(bodyVolume(d, ex) == Approx(500));
}

TEST_CASE("Parametric edit regenerates downstream features", "[cad][design]")
{
    Design d;
    d.setParameter("w", "40");
    d.setParameter("h", "w / 2");
    const auto sk = d.addFeature("sketch", {{"plane", "XY"},
                                            {"curves", Json::array({{{"type", "rectangle"},
                                                                     {"corner", {0, 0}},
                                                                     {"width", "w"},
                                                                     {"height", "h"}}})}});
    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", "10 mm"}});
    const auto fil = d.addFeature("fillet", {{"body", ex}, {"edges", Json::array({{{"point", {0, 0, 5}}}})}, {"radius", 2}});
    CHECK(d.status(fil).state == cad::FeatureStatus::State::Ok);
    const double pi = std::numbers::pi;
    CHECK(bodyVolume(d, ex) == Approx(40 * 20 * 10 - (4 - pi) * 10).epsilon(1e-6));

    d.setParameter("w", "60");
    CHECK(bodyVolume(d, ex) == Approx(60 * 30 * 10 - (4 - pi) * 10).epsilon(1e-6));

    d.editFeature(ex, {{"distance", 20}});
    CHECK(bodyVolume(d, ex) == Approx(60 * 30 * 20 - (4 - pi) * 20).epsilon(1e-6));

    CHECK_THROWS_AS(d.setParameter("w", "h * 2"), cad::DesignError); // cycle
    CHECK_THROWS_AS(d.setParameter("pi", "3"), cad::DesignError);
    CHECK_THROWS_AS(d.setParameter("1x", "3"), cad::DesignError);
    CHECK(d.parameterValue("w") == Approx(60));
}

TEST_CASE("Join, cut and intersect operations", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature("sketch", rectSketch(20, 20));
    const auto body = d.addFeature("extrude", {{"sketch", sk}, {"distance", 10}});

    // Hole through the block: circle on the top face, cut downwards.
    const auto top = d.addFeature("sketch", {{"face", {{"body", body}, {"point", {10, 10, 10}}}},
                                             {"curves", Json::array({{{"type", "circle"},
                                                                      {"center", {0, 0}},
                                                                      {"diameter", 10}}})}});
    const auto& plane = d.sketches().at(top).plane;
    CHECK(plane.origin.z == Approx(10));
    CHECK(plane.normal.z == Approx(1));
    d.addFeature("extrude", {{"sketch", top}, {"distance", 10}, {"flip", true}, {"operation", "cut"}});
    const double pi = std::numbers::pi;
    CHECK(bodyVolume(d, body) == Approx(4000 - pi * 25 * 10).epsilon(1e-6));

    // Boss on top joins the body.
    const auto boss = d.addFeature("box", {{"corner", {0, 0, 10}}, {"size", {5, 5, 5}}, {"operation", "join"}});
    CHECK(d.bodies().size() == 1);
    CHECK_FALSE(d.bodies().contains(boss));
    CHECK(bodyVolume(d, body) == Approx(4000 - pi * 250 + 125).epsilon(1e-6));

    // Cutting with something that misses every body fails.
    CHECK_THROWS_AS(d.addFeature("box", {{"corner", {100, 100, 100}}, {"size", {1, 1, 1}}, {"operation", "cut"}}),
                    cad::DesignError);

    // Intersect keeps only the common part.
    d.addFeature("box", {{"corner", {0, 0, 0}}, {"size", {10, 20, 10}}, {"operation", "intersect"}});
    CHECK(bodyVolume(d, body) == Approx(2000 - pi * 250 / 2).epsilon(1e-6));
}

TEST_CASE("Profiles chosen by index or by point", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature(
        "sketch", {{"plane", "XY"},
                   {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 40}},
                                           {{"type", "circle"}, {"center", {20, 20}}, {"radius", 5}}})}});
    REQUIRE(d.sketches().at(sk).geometry.profiles.size() == 2);
    const auto disk = d.addFeature("extrude", {{"sketch", sk}, {"points", Json::array({{20, 20}})}, {"distance", 10}});
    const double pi = std::numbers::pi;
    CHECK(bodyVolume(d, disk) == Approx(pi * 25 * 10).epsilon(1e-6));
    const auto ring = d.addFeature("extrude", {{"sketch", sk}, {"points", Json::array({{2, 2}})}, {"distance", 5}});
    CHECK(bodyVolume(d, ring) == Approx((1600 - pi * 25) * 5).epsilon(1e-6));
    const auto all = d.addFeature("extrude", {{"sketch", sk}, {"distance", 1}, {"flip", true}});
    CHECK(bodyVolume(d, all) == Approx(1600).epsilon(1e-6));
    CHECK(d.bodies().at(all).shape.solidCount() == 1); // profiles are merged
}

TEST_CASE("Revolve, shell, chamfer, combine", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature("sketch", {{"plane", "XZ"},
                                            {"curves", Json::array({{{"type", "rectangle"},
                                                                     {"corner", {5, 0}},
                                                                     {"width", 5},
                                                                     {"height", 10}}})}});
    const auto tube = d.addFeature("revolve", {{"sketch", sk}, {"axis", "Z"}});
    const double pi = std::numbers::pi;
    CHECK(bodyVolume(d, tube) == Approx(pi * 75 * 10).epsilon(1e-6));

    const auto block = d.addFeature("box", {{"corner", {20, 0, 0}}, {"size", {10, 10, 10}}});
    d.addFeature("chamfer", {{"body", block}, {"edges", Json::array({{{"point", {25, 0, 10}}}})}, {"distance", 1}});
    CHECK(bodyVolume(d, block) == Approx(1000 - 0.5 * 10).epsilon(1e-6));
    d.addFeature("shell", {{"body", block}, {"faces", Json::array({{{"point", {25, 5, 0}}}})}, {"thickness", 1}});
    CHECK(bodyVolume(d, block) > 300);
    CHECK(bodyVolume(d, block) < 450); // hollow: about 1000 - 8 * 8 * 9

    const auto pin = d.addFeature("cylinder", {{"center", {0, 0, -5}}, {"radius", 7}, {"height", 20}});
    d.addFeature("combine", {{"target", tube}, {"tools", {pin}}, {"operation", "cut"}});
    CHECK(d.bodies().size() == 2);
    CHECK(bodyVolume(d, tube) == Approx(pi * (100 - 49) * 10).epsilon(1e-6));
}

TEST_CASE("Press Pull in the timeline follows an edited box", "[cad][design]")
{
    Design d;
    const auto block = d.addFeature("box", {{"size", {10, 10, 10}}});
    const auto pp = d.addFeature("press_pull", {{"body", block},
                                               {"faces", Json::array({{{"point", {5, 5, 10}}}})},
                                               {"distance", 4}});
    CHECK(bodyVolume(d, block) == Approx(1400).epsilon(1e-6));
    d.editFeature(pp, {{"body", block}, {"faces", Json::array({{{"point", {5, 5, 10}}}})}, {"distance", -2}});
    CHECK(bodyVolume(d, block) == Approx(800).epsilon(1e-6));
}

TEST_CASE("Split Body in the timeline makes a second body and follows an edited box", "[cad][design]")
{
    Design d;
    const auto block = d.addFeature("box", {{"size", {40, 20, 10}}});
    const auto split = d.addFeature("split", {{"body", block}, {"plane", "YZ"}, {"offset", 15}});
    CHECK(d.bodies().size() == 2);
    CHECK(bodyVolume(d, block) == Approx(15 * 200).epsilon(1e-6));
    CHECK(bodyVolume(d, split) == Approx(25 * 200).epsilon(1e-6));
    d.editFeature(block, {{"size", {60, 20, 10}}});
    CHECK(bodyVolume(d, block) == Approx(15 * 200).epsilon(1e-6));
    CHECK(bodyVolume(d, split) == Approx(45 * 200).epsilon(1e-6));
    d.editFeature(split, {{"offset", 30}});
    CHECK(bodyVolume(d, block) == Approx(30 * 200).epsilon(1e-6));
    CHECK(bodyVolume(d, split) == Approx(30 * 200).epsilon(1e-6));
    // The new body takes features like any other.
    d.addFeature("press_pull", {{"body", split}, {"faces", Json::array({{{"point", {60, 10, 5}}}})}, {"distance", 5}});
    CHECK(bodyVolume(d, split) == Approx(35 * 200).epsilon(1e-6));
    CHECK_THROWS_AS(d.addFeature("split", {{"body", block}, {"plane", "YZ"}, {"offset", 100}}), cad::DesignError);
}

TEST_CASE("Chamfer types in the timeline", "[cad][design]")
{
    Design d;
    const auto block = d.addFeature("box", {{"size", {10, 10, 10}}});
    const Json edge = Json::array({{{"point", {5, 0, 10}}}});
    const auto ch = d.addFeature("chamfer", {{"body", block}, {"edges", edge}, {"type", "two_distances"},
                                             {"distance", 1}, {"distance2", "1 + 2"}});
    CHECK(bodyVolume(d, block) == Approx(1000 - 15).epsilon(1e-6));
    d.editFeature(ch, {{"type", "distance_angle"}, {"distance", 2}, {"angle", 45}, {"distance2", nullptr}});
    CHECK(bodyVolume(d, block) == Approx(1000 - 20).epsilon(1e-6));
    CHECK_THROWS_AS(d.addFeature("chamfer", {{"body", block}, {"edges", Json::array({{{"point", {5, 10, 10}}}})},
                                             {"type", "fancy"}, {"distance", 1}}),
                    cad::DesignError);
}

TEST_CASE("Timeline: suppress, marker, remove and restore", "[cad][design]")
{
    Design d;
    const auto sk = d.addFeature("sketch", rectSketch(10, 10));
    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", 10}});
    const auto ch = d.addFeature("chamfer", {{"body", ex}, {"edges", Json::array({0})}, {"distance", 1}});
    const auto snapshot = d.state();

    d.setSuppressed(ch, true);
    CHECK(d.status(ch).state == cad::FeatureStatus::State::Suppressed);
    CHECK(bodyVolume(d, ex) == Approx(1000));
    d.setSuppressed(ch, false);

    d.setMarker(1);
    CHECK(d.bodies().empty());
    CHECK(d.status(ex).state == cad::FeatureStatus::State::RolledBack);
    // A feature added at the marker goes before the rolled-back ones.
    const auto sk2 = d.addFeature("sketch", rectSketch(1, 1));
    CHECK(d.features()[1].id == sk2);
    CHECK(d.marker() == 2);
    d.setMarker(d.features().size());

    d.removeFeature(sk);
    CHECK(d.status(ex).state == cad::FeatureStatus::State::Error);
    CHECK(d.bodies().empty());

    d.restore(snapshot);
    CHECK(d.features().size() == 3);
    CHECK(d.bodies().size() == 1);
    CHECK_THROWS_AS(d.setMarker(10), cad::DesignError);
}

TEST_CASE("Document sync keeps part ids, names and visibility", "[cad][design]")
{
    Design d;
    model::Document doc;
    const auto sk = d.addFeature("sketch", rectSketch(10, 10));
    d.syncDocument(doc);
    REQUIRE(doc.partCount() == 1);
    const auto sketchPart = doc.parts()[0].id;
    CHECK(doc.parts()[0].kind == model::PartKind::Sketch);
    CHECK(doc.parts()[0].visible);
    CHECK_FALSE(doc.parts()[0].edgeSegments.empty());

    const auto ex = d.addFeature("extrude", {{"sketch", sk}, {"distance", 10}});
    d.syncDocument(doc);
    REQUIRE(doc.partCount() == 2);
    CHECK_FALSE(doc.part(sketchPart)->visible); // consumed sketch is hidden
    const model::Part* body = nullptr;
    for (const auto& p : doc.parts())
        if (p.kind == model::PartKind::Body)
            body = &p;
    REQUIRE(body != nullptr);
    const auto bodyPart = body->id;
    CHECK(body->featureId == ex);
    CHECK(body->shape.has_value());
    doc.modifyPart(bodyPart, [](model::Part& p) { p.name = "Bracket"; });

    d.editFeature(ex, {{"distance", 20}});
    d.syncDocument(doc);
    CHECK(doc.part(bodyPart)->name == "Bracket");
    CHECK(doc.part(bodyPart)->shape->volume() == Approx(2000));

    const auto snapshot = d.state();
    d.removeFeature(ex);
    d.syncDocument(doc);
    CHECK(doc.part(bodyPart) == nullptr);
    CHECK(doc.part(sketchPart)->visible); // no longer used: shown again
    d.restore(snapshot);
    d.syncDocument(doc);
    REQUIRE(doc.part(bodyPart) != nullptr); // same id after undo
    CHECK(doc.part(bodyPart)->shape->volume() == Approx(2000));
    CHECK(doc.part(bodyPart)->name == "Bracket"); // user settings come back too

    doc.modifyPart(bodyPart, [](model::Part& p) { p.visible = false; });
    d.setSuppressed(ex, true);
    d.syncDocument(doc);
    CHECK(doc.part(bodyPart) == nullptr);
    d.setSuppressed(ex, false);
    d.syncDocument(doc);
    REQUIRE(doc.part(bodyPart) != nullptr);
    CHECK_FALSE(doc.part(bodyPart)->visible);
}

TEST_CASE("Automatic targets need real contact; duplicate targets are merged", "[cad][design]")
{
    Design d;
    // Bounding boxes overlap, the cylinders do not touch.
    const auto a = d.addFeature("cylinder", {{"center", {0, 0, 0}}, {"radius", 1}, {"height", 10}});
    CHECK_THROWS_AS(d.addFeature("cylinder", {{"center", {1.5, 1.5, 0}}, {"radius", 1}, {"height", 10},
                                              {"operation", "intersect"}}),
                    cad::DesignError);
    const auto b = d.addFeature("cylinder", {{"center", {1.5, 1.5, 0}}, {"radius", 1}, {"height", 10},
                                             {"operation", "join"}});
    CHECK(d.bodies().size() == 2); // not touching: join made a new body
    CHECK(d.bodies().contains(b));

    const auto box = d.addFeature("box", {{"corner", {-2, -2, 9}}, {"size", {5, 5, 2}}, {"operation", "join"},
                                          {"bodies", Json::array({a, "Body1"})}});
    CHECK_FALSE(d.bodies().contains(box));
    CHECK(d.bodies().at(a).shape.volume() > 50 - 1e-6);
}

TEST_CASE("Hole: through and blind", "[cad][design]")
{
    Design d;
    const auto box = d.addFeature("box", {{"size", {20, 20, 10}}});
    const double pi = std::numbers::pi;
    d.addFeature("hole", {{"body", box}, {"point", {5, 5, 10}}, {"diameter", 4}});
    CHECK(bodyVolume(d, box) == Approx(4000 - pi * 4 * 10).epsilon(1e-6));
    d.addFeature("hole", {{"body", box}, {"point", {15, 15, 10}}, {"diameter", 4}, {"depth", 3}});
    CHECK(bodyVolume(d, box) == Approx(4000 - pi * 4 * 13).epsilon(1e-6));
    // Side face, through.
    d.addFeature("hole", {{"body", box}, {"point", {0, 10, 5}}, {"diameter", 2}});
    CHECK(bodyVolume(d, box) < 4000 - pi * 4 * 13);
}

namespace {

/// A gently curved sheet z = 5 + 0.5 sin(x / 5), triangles facing +Z (a scan's outside).
std::shared_ptr<const core::TriangleMesh> wavySheet()
{
    auto mesh = std::make_shared<core::TriangleMesh>();
    const int n = 50;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const double x = -20.0 + i;
            const double y = -20.0 + j;
            mesh->addVertex({x, y, 5.0 + 0.5 * std::sin(x / 5.0)});
        }
    auto at = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            mesh->addTriangle(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            mesh->addTriangle(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    return mesh;
}

/// Volume of the 10 mm box [0, 10]^3 above the sheet.
double boxAboveSheet()
{
    // Integral over x in [0, 10] of (10 - 5 - 0.5 sin(x / 5)), times 10 along y.
    return 10.0 * (50.0 - 0.5 * 5.0 * (1.0 - std::cos(2.0)));
}

} // namespace

TEST_CASE("Combine with a scan, a surface and a solid that are not in the timeline", "[cad][design]")
{
    model::Part scan;
    scan.id = 7;
    scan.name = "scan";
    scan.mesh = wavySheet();
    model::Part solid;
    solid.id = 8;
    solid.name = "block";
    solid.shape = geometry::makeBox(10, 10, 2); // z 0..2
    model::Part surface;
    surface.id = 9;
    surface.name = "skin";
    {
        core::TriangleSet all(scan.mesh->triangleCount());
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = static_cast<std::uint32_t>(i);
        surface.shape = geometry::fitSurface(*scan.mesh, all).face;
    }
    Design d;
    d.setPartLookup([&](model::PartId id) -> const model::Part* {
        return id == 7 ? &scan : id == 8 ? &solid : id == 9 ? &surface : nullptr;
    });

    const auto box = d.addFeature("box", {{"size", {10, 10, 10}}});
    const auto cut = d.addFeature("combine", {{"target", box}, {"tools", Json::array({{{"part", 7}}})}, {"operation", "cut"}});
    CHECK(bodyVolume(d, box) == Approx(boxAboveSheet()).epsilon(0.01));
    d.editFeature(cut, {{"flip", true}});
    CHECK(bodyVolume(d, box) == Approx(1000 - boxAboveSheet()).epsilon(0.01));
    d.editFeature(cut, {{"flip", nullptr}, {"operation", "intersect"}});
    CHECK(bodyVolume(d, box) == Approx(1000 - boxAboveSheet()).epsilon(0.01));
    d.editFeature(cut, {{"operation", "cut"}, {"tools", Json::array({{{"part", 9}}})}});
    CHECK(bodyVolume(d, box) == Approx(boxAboveSheet()).epsilon(0.01));
    // An exact solid part is a normal boolean tool.
    d.editFeature(cut, {{"tools", Json::array({{{"part", 8}}})}});
    CHECK(bodyVolume(d, box) == Approx(800).epsilon(1e-6));
    // A scan cannot be joined.
    CHECK_THROWS_AS(d.addFeature("combine", {{"target", box}, {"tools", Json::array({{{"part", 7}}})}, {"operation", "join"}}),
                    cad::DesignError);
    // A scan far from the body does not reach it.
    const auto far = d.addFeature("box", {{"corner", {200, 200, 0}}, {"size", {5, 5, 5}}});
    CHECK_THROWS_AS(d.addFeature("combine", {{"target", far}, {"tools", Json::array({{{"part", 7}}})}, {"operation", "cut"}}),
                    cad::DesignError);
}

TEST_CASE("Cut by a curved scan: pieces whose centre lies on the other side", "[cad][design]")
{
    // Sheet z = x^2 / 20 facing up: the piece of the box under it is a thin crescent whose
    // centre of mass (x = 0, z = 1.5) lies above the sheet.
    model::Part scan;
    scan.id = 3;
    scan.name = "bowl";
    auto mesh = std::make_shared<core::TriangleMesh>();
    const int n = 60;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const double x = -15.0 + 0.5 * i;
            mesh->addVertex({x, -15.0 + 0.5 * j, x * x / 20.0});
        }
    auto at = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            mesh->addTriangle(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            mesh->addTriangle(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    scan.mesh = mesh;
    Design d;
    d.setPartLookup([&](model::PartId id) -> const model::Part* { return id == 3 ? &scan : nullptr; });
    const auto box = d.addFeature("box", {{"corner", {-10, -5, 0}}, {"size", {20, 10, 10}}});
    const double under = 10.0 * 2000.0 / 60.0; // y length times the integral of x^2 / 20
    const auto cut = d.addFeature("combine", {{"target", box}, {"tools", Json::array({{{"part", 3}}})}, {"operation", "cut"}});
    CHECK(bodyVolume(d, box) == Approx(2000 - under).epsilon(0.01));
    d.editFeature(cut, {{"operation", "intersect"}});
    CHECK(bodyVolume(d, box) == Approx(under).epsilon(0.02));
}
