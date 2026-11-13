// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/core/Path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <numbers>
#include <set>
#include <utility>

using namespace aerocad;
using commands::Json;
using Catch::Approx;

namespace {

Json run(commands::Session& s, const std::string& name, const Json& params = Json::object())
{
    const auto r = s.execute(name, params);
    INFO(name << " " << params.dump() << " -> " << r.error);
    REQUIRE(r.ok);
    return r.data;
}

std::size_t countKind(const commands::Session& s, model::PartKind kind)
{
    std::size_t n = 0;
    for (const auto& p : s.document().parts())
        n += p.kind == kind ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("Design commands: sketch, extrude, fillet with undo/redo", "[commands][cad]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30 mm"}});
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "center_rectangle"},
                                                  {"center", {0, 0}},
                                                  {"width", "w"},
                                                  {"height", "w"}}})}});
    CHECK(sk.at("profiles") == 1);
    CHECK(sk.at("name") == "Sketch1");
    const auto sketchId = sk.at("id").get<std::uint64_t>();

    const Json ex = run(s, "feature.extrude", {{"sketch", sketchId}, {"distance", 10}});
    const auto bodyId = ex.at("id").get<std::uint64_t>();
    REQUIRE(ex.at("bodies").size() == 1);
    CHECK(ex.at("bodies")[0].at("volume").get<double>() == Approx(9000));
    CHECK(ex.at("bodies")[0].at("partId").get<std::uint64_t>() != 0);
    CHECK(countKind(s, model::PartKind::Body) == 1);
    CHECK(countKind(s, model::PartKind::Sketch) == 1);

    const Json edges = run(s, "body.edges", {{"body", bodyId}});
    CHECK(edges.at("edges").size() == 12);
    const Json faces = run(s, "body.faces", {{"body", "Body1"}});
    CHECK(faces.at("faces").size() == 6);

    run(s, "feature.fillet", {{"body", bodyId}, {"edges", Json::array({{{"point", {15, 15, 5}}}})}, {"radius", 3}});
    const double pi = std::numbers::pi;
    const double filleted = 9000 - (9 - 9 * pi / 4) * 10;
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(filleted).epsilon(1e-6));

    run(s, "parameter.set", {{"name", "w"}, {"expression", "40"}});
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(16000 - (9 - 9 * pi / 4) * 10).epsilon(1e-6));

    run(s, "edit.undo"); // w back to 30
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(filleted).epsilon(1e-6));
    run(s, "edit.undo"); // remove fillet
    CHECK(s.design().bodies().at(bodyId).shape.volume() == Approx(9000));
    run(s, "edit.undo"); // remove extrude
    CHECK(countKind(s, model::PartKind::Body) == 0);
    CHECK(s.document().part(ex.at("bodies")[0].at("partId").get<std::uint64_t>()) == nullptr);
    run(s, "edit.redo");
    CHECK(countKind(s, model::PartKind::Body) == 1);
    CHECK(s.document().part(ex.at("bodies")[0].at("partId").get<std::uint64_t>()) != nullptr);

    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("features").size() == 2);
    CHECK(tl.at("parameters")[0].at("value").get<double>() == Approx(30));
}

TEST_CASE("Design commands: errors are reported and nothing is recorded", "[commands][cad]")
{
    commands::Session s;
    const auto before = s.undoStack().size();
    auto r = s.execute("feature.extrude", {{"sketch", 42}, {"distance", 5}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("No sketch") != std::string::npos);
    r = s.execute("sketch.create", {{"curves", Json::array({{{"type", "hexagon"}}})}});
    CHECK_FALSE(r.ok); // rejected by the schema
    r = s.execute("sketch.create", {{"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", "r"}}})}});
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("unknown parameter 'r'") != std::string::npos);
    CHECK(s.undoStack().size() == before);
    CHECK(s.design().features().empty());
}

TEST_CASE("Design commands: body parts cannot be scaled or removed directly", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.box", {{"size", {10, 10, 10}}});
    const auto partId = s.document().parts().at(0).id;
    CHECK_FALSE(s.execute("part.remove", {{"id", partId}}).ok);
    CHECK_FALSE(s.execute("part.scale", {{"id", partId}, {"factor", 2}}).ok);
    CHECK(run(s, "part.rename", {{"id", partId}, {"name", "Block"}}).at("name") == "Block");
    const Json parts = run(s, "document.parts");
    CHECK(parts.at("parts")[0].at("kind") == "body");
}

TEST_CASE("Design commands: move/rotate a body is a timeline step", "[commands][cad]")
{
    commands::Session s;
    const auto box = run(s, "feature.box", {{"size", {20, 10, 4}}}).at("id").get<std::uint64_t>();
    // 90 degrees about Z through the box centre (10, 5, 2), then 100 mm along X.
    const auto move = run(s, "feature.move", {{"body", box}, {"axis", "Z"}, {"angle", 90}, {"translate", {100, 0, 0}}})
                          .at("id")
                          .get<std::uint64_t>();
    auto bounds = [&] { return s.design().bodies().at(box).shape.bounds(); };
    CHECK(bounds().min.x == Approx(105).margin(1e-6));
    CHECK(bounds().max.x == Approx(115).margin(1e-6));
    CHECK(bounds().min.y == Approx(-5).margin(1e-6));
    CHECK(bounds().max.y == Approx(15).margin(1e-6));
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(800));

    run(s, "feature.edit", {{"id", move}, {"params", {{"angle", 0}}}});
    CHECK(bounds().min.x == Approx(100).margin(1e-6));
    CHECK(bounds().max.y == Approx(10).margin(1e-6));
    CHECK_FALSE(s.execute("feature.move", {{"body", box}, {"axis", "Q"}, {"angle", 10}}).ok);

    run(s, "edit.undo");
    run(s, "edit.undo");
    CHECK(bounds().min.x == Approx(0).margin(1e-6));
}

TEST_CASE("Design commands: move scales a body about its centre in the same step", "[commands][cad]")
{
    commands::Session s;
    const auto box = run(s, "feature.box", {{"size", {20, 10, 4}}}).at("id").get<std::uint64_t>();
    // 150 % about the box centre (10, 5, 2), 90 degrees about Z, then 100 mm along X.
    run(s, "feature.move",
        {{"body", box}, {"scale", 1.5}, {"axis", "Z"}, {"angle", 90}, {"translate", {100, 0, 0}}});
    const auto b = s.design().bodies().at(box).shape.bounds();
    CHECK(b.min.x == Approx(110 - 7.5).margin(1e-6));
    CHECK(b.max.x == Approx(110 + 7.5).margin(1e-6));
    CHECK(b.min.y == Approx(5 - 15).margin(1e-6));
    CHECK(b.max.y == Approx(5 + 15).margin(1e-6));
    CHECK(b.min.z == Approx(2 - 3).margin(1e-6));
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(800 * 1.5 * 1.5 * 1.5));
    CHECK_FALSE(s.execute("feature.move", {{"body", box}, {"scale", 0}}).ok);
    run(s, "edit.undo");
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(800));
}

TEST_CASE("Design commands: a region selection is dropped when the body's tessellation changes", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.box", {{"size", {10, 10, 10}}});
    const auto partId = s.document().parts().at(0).id;
    run(s, "selection.set", {{"id", partId}, {"triangles", {0, 1}}});
    CHECK(run(s, "selection.get", {{"id", partId}}).at("selectedTriangles") == 2);

    run(s, "feature.cylinder", {{"center", {5, 5, -1}}, {"radius", 2}, {"height", 12}, {"operation", "cut"}});
    CHECK(run(s, "selection.get", {{"id", partId}}).at("selectedTriangles") == 0);
}

TEST_CASE("Design commands: timeline marker, suppress, delete, edit", "[commands][cad]")
{
    commands::Session s;
    const auto box = run(s, "feature.box", {{"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    const auto hole =
        run(s, "feature.cylinder", {{"center", {5, 5, -1}}, {"radius", 2}, {"height", 12}, {"operation", "cut"}})
            .at("id")
            .get<std::uint64_t>();
    const double pi = std::numbers::pi;
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000 - pi * 40).epsilon(1e-6));

    run(s, "feature.edit", {{"id", hole}, {"params", {{"radius", 3}}}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000 - pi * 90).epsilon(1e-6));
    CHECK_FALSE(s.execute("feature.edit", {{"id", hole}, {"params", {{"radius", -1}}}}).ok);

    run(s, "feature.suppress", {{"id", hole}, {"suppressed", true}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000));
    run(s, "feature.suppress", {{"id", hole}, {"suppressed", false}});

    const Json tl = run(s, "timeline.set_marker", {{"position", 1}});
    CHECK(tl.at("features")[1].at("status") == "rolled_back");
    run(s, "timeline.set_marker");
    run(s, "feature.delete", {{"id", hole}});
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(1000));
    run(s, "edit.undo");
    CHECK(s.design().features().size() == 2);

    const Json list = run(s, "commands.list");
    bool hasExtrude = false;
    for (const auto& c : list.at("commands"))
        hasExtrude = hasExtrude || c.at("name") == "feature.extrude";
    CHECK(hasExtrude);
}

TEST_CASE("Design commands: imported mesh becomes a body that can be cut", "[commands][cad]")
{
    commands::Session s;
    const std::string path = std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl";
    const Json imported = run(s, "document.import", {{"path", path}});
    const auto partId = imported.at("id").get<std::uint64_t>();
    const Json body = run(s, "feature.base_body", {{"part", partId}});
    const auto bodyId = body.at("id").get<std::uint64_t>();
    REQUIRE(s.design().bodies().contains(bodyId));
    const double tetra = s.design().bodies().at(bodyId).shape.volume();
    CHECK(tetra > 0);
    CHECK_FALSE(s.document().part(partId)->visible);

    run(s, "feature.box", {{"corner", {-1, -1, -1}}, {"size", {3, 3, 3}}, {"operation", "cut"}});
    CHECK(s.design().bodies().at(bodyId).shape.volume() < tetra);

    run(s, "edit.undo");
    run(s, "edit.undo");
    CHECK(s.design().bodies().empty());
    CHECK(s.document().part(partId)->visible);
}

TEST_CASE("Design commands: sketch constraints drive the model through parameters", "[commands][cad][solver]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "len"}, {"expression", "50 mm"}});
    // A free-hand rectangle (as drawn with the mouse), then dimensioned.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {3, 2}}, {"opposite", {41, 19}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    CHECK(sk.at("degreesOfFreedom") == 4); // position + width + height
    const Json c = run(s, "sketch.add_constraints",
                       {{"sketch", id},
                        {"constraints", Json::array({{{"type", "coincident"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}},
                                                                              "origin"})}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", "len"}},
                                                     {{"type", "length"}, {"curve", 1}, {"value", 20}}})}});
    CHECK(c.at("degreesOfFreedom") == 0);
    const Json ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 10}});
    CHECK(ex.at("bodies")[0].at("volume").get<double>() == Approx(50 * 20 * 10));

    // Changing the parameter moves the sketch and the body.
    run(s, "parameter.set", {{"name", "len"}, {"expression", "80 mm"}});
    Json tl = run(s, "timeline.list");
    CHECK(tl.at("bodies")[0].at("volume").get<double>() == Approx(80 * 20 * 10));

    // Curves and constraints are listed with the solved geometry.
    const Json curves = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(curves.at("curves").size() == 4);
    CHECK(curves.at("curves")[0].at("points")[1][0].get<double>() == Approx(80));
    CHECK(curves.at("constraints").size() == 3);
    CHECK(curves.at("constraints")[1].at("resolved").get<double>() == Approx(80));

    // Editing a dimension, undo and conflicting constraints.
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", 2}, {"value", 30}});
    tl = run(s, "timeline.list");
    CHECK(tl.at("bodies")[0].at("volume").get<double>() == Approx(80 * 30 * 10));
    run(s, "edit.undo");
    tl = run(s, "timeline.list");
    CHECK(tl.at("bodies")[0].at("volume").get<double>() == Approx(80 * 20 * 10));

    const auto bad = s.execute("sketch.add_constraints",
                               {{"sketch", id},
                                {"constraints", Json::array({{{"type", "length"}, {"curve", 2}, {"value", 10}}})}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("conflict") != std::string::npos);
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", 0}, {"remove", true}});
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("degreesOfFreedom") == 2);
}

TEST_CASE("Design commands: a slot keeps its shape when dimensioned", "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "slot"}, {"start", {0, 0}}, {"end", {30, 0}}, {"width", 10}}})},
                         {"constraints", Json::array({{{"type", "radius"}, {"curve", 1}, {"value", 8}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    const Json ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 1}});
    const double pi = std::numbers::pi;
    // Both ends follow the radius (tangent lines), the length between centres stays free but unchanged.
    const double v = ex.at("bodies")[0].at("volume").get<double>();
    CHECK(v == Approx(pi * 64 + 16 * 30).epsilon(1e-3));
}

TEST_CASE("Design commands: fixing a dimensioned line keeps its solved position", "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}},
                                                 {{"type", "circle"}, {"center", {30, 0}}, {"radius", 3}}})},
                         {"constraints", Json::array({{{"type", "length"}, {"curve", 0}, {"value", 20}},
                                                      {{"type", "radius"}, {"curve", 1}, {"value", 5}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({0, 1})}}})}});
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    const double len = std::hypot(
        c.at("curves")[0].at("points")[1][0].get<double>() - c.at("curves")[0].at("points")[0][0].get<double>(),
        c.at("curves")[0].at("points")[1][1].get<double>() - c.at("curves")[0].at("points")[0][1].get<double>());
    CHECK(len == Approx(20));
    CHECK(c.at("curves")[1].at("radius").get<double>() == Approx(5));
    CHECK(c.at("degreesOfFreedom") == 0);
}

TEST_CASE("Design commands: a picked fillet edge follows the body when a dimension grows", "[commands][cad]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30"}});
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"},
                                                  {"corner", {0, 0}},
                                                  {"width", "w"},
                                                  {"height", 20}}})}});
    const Json ex = run(s, "feature.extrude", {{"sketch", sk.at("id")}, {"distance", 10}});
    const auto body = ex.at("id").get<std::uint64_t>();
    // The vertical edge at the corner (w, 20), picked by a point on it.
    const Json f = run(s, "feature.fillet",
                       {{"body", body}, {"edges", Json::array({{{"point", {30, 20, 5}}}})}, {"radius", 3}});
    const Json& stored = f.at("params").at("edges")[0];
    CHECK(stored.contains("relative"));
    CHECK(stored.at("kind") == "line");
    const double pi = std::numbers::pi;
    const double cornerLoss = (1 - pi / 4) * 9;
    CHECK(f.at("bodies")[0].at("volume").get<double>() == Approx(30 * 20 * 10 - cornerLoss * 10));

    // Old point (30, 20, 5) is now nearer to the long top/bottom edges; the stored
    // reference still finds the vertical corner edge.
    run(s, "parameter.set", {{"name", "w"}, {"expression", "80"}});
    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("features")[2].at("status") == "ok");
    CHECK(tl.at("bodies")[0].at("volume").get<double>() == Approx(80 * 20 * 10 - cornerLoss * 10));
}

TEST_CASE("Design commands: previews keep the undo history", "[commands][preview]")
{
    commands::Session s;
    run(s, "feature.box", {{"size", {10, 10, 10}}});
    run(s, "feature.box", {{"corner", {20, 0, 0}}, {"size", {10, 10, 10}}});
    run(s, "edit.undo"); // the second box can be redone
    REQUIRE(s.undoStack().canRedo());

    // Previews replace each other and do not drop the redo entry.
    REQUIRE(s.preview("feature.cylinder", {{"diameter", 5}, {"height", 5}}).ok);
    REQUIRE(s.preview("feature.cylinder", {{"diameter", 6}, {"height", 5}}).ok);
    CHECK(s.design().bodies().size() == 2);
    CHECK(s.cancelPreview());
    CHECK(s.design().bodies().size() == 1);
    CHECK(s.undoStack().canRedo());
    CHECK_FALSE(s.cancelPreview());

    // A failing preview leaves nothing behind.
    CHECK_FALSE(s.preview("feature.fillet", {{"body", 1}, {"edges", Json::array()}, {"radius", 1}}).ok);
    CHECK_FALSE(s.previewActive());
    CHECK(s.undoStack().canRedo());

    // Undo while a preview is shown ends the preview first, then undoes the real step.
    REQUIRE(s.preview("feature.cylinder", {{"diameter", 5}, {"height", 5}}).ok);
    run(s, "edit.undo");
    CHECK_FALSE(s.previewActive());
    CHECK(s.design().bodies().empty());
    CHECK_FALSE(s.cancelPreview());
    run(s, "edit.redo");
    run(s, "edit.redo");
    CHECK(s.design().bodies().size() == 2);
}

TEST_CASE("Design commands: mirror and patterns", "[commands][cad][pattern]")
{
    commands::Session s;
    const Json box = run(s, "feature.box", {{"size", {10, 10, 10}}, {"corner", {5, 0, 0}}});
    const auto id = box.at("id").get<std::uint64_t>();
    Json r = run(s, "feature.mirror", {{"body", id}, {"plane", "YZ"}});
    CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(2000));
    CHECK(r.at("bodies")[0].at("bounds").at("min")[0].get<double>() == Approx(-15));
    run(s, "edit.undo");

    r = run(s, "feature.rectangular_pattern",
            {{"body", id}, {"count", 3}, {"spacing", 20}, {"count2", 2}, {"spacing2", 15}});
    CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(6000));
    CHECK(r.at("bodies")[0].at("bounds").at("max")[0].get<double>() == Approx(55));
    run(s, "edit.undo");

    r = run(s, "feature.circular_pattern", {{"body", id}, {"count", 4}, {"operation", "new_body"}});
    REQUIRE(r.at("bodies").size() == 2);
    CHECK(r.at("bodies")[1].at("volume").get<double>() == Approx(3000));
}

TEST_CASE("Design commands: loft and sweep", "[commands][cad][pattern]")
{
    commands::Session s;
    const Json a = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 20}, {"height", 20}}})}});
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"offset", 30},
                        {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 10}, {"height", 10}}})}});
    const Json loft = run(s, "feature.loft", {{"sections", Json::array({a.at("id"), b.at("id")})}});
    // Frustum of a square pyramid: h/3 (A1 + A2 + sqrt(A1 A2)).
    CHECK(loft.at("bodies")[0].at("volume").get<double>() == Approx(30.0 / 3 * (400 + 100 + 200)).epsilon(1e-4));

    commands::Session t;
    const Json prof = run(t, "sketch.create",
                          {{"plane", "XY"}, {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 2}}})}});
    const Json path = run(t, "sketch.create",
                          {{"plane", "XZ"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {0, 40}}}})}});
    const Json sw = run(t, "feature.sweep", {{"sketch", prof.at("id")}, {"path", {{"sketch", path.at("id")}}}});
    CHECK(sw.at("bodies")[0].at("volume").get<double>() == Approx(std::numbers::pi * 4 * 40).epsilon(1e-4));
}

TEST_CASE("Design commands: construction planes carry sketches", "[commands][cad][plane]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "h"}, {"expression", "12"}});
    const Json pl = run(s, "feature.plane", {{"base", "XY"}, {"offset", "h"}});
    const auto plId = pl.at("id").get<std::uint64_t>();
    CHECK(pl.at("plane").at("origin")[2].get<double>() == Approx(12));

    // A sketch on the plane follows it when the parameter changes.
    const Json sk = run(s, "sketch.create",
                        {{"plane", {{"plane", plId}}},
                         {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 10}, {"height", 10}}})}});
    Json r = run(s, "feature.extrude", {{"sketch", sk.at("id")}, {"distance", 5}});
    CHECK(r.at("bodies")[0].at("bounds").at("min")[2].get<double>() == Approx(12));
    run(s, "parameter.set", {{"name", "h"}, {"expression", "20"}});
    r = run(s, "timeline.list");
    CHECK(r.at("bodies")[0].at("bounds").at("min")[2].get<double>() == Approx(20));

    // Plane at angle: XY turned 90 degrees about its x axis has a normal along -Y or +Y.
    r = run(s, "feature.plane", {{"base", "XY"}, {"angle", 90}});
    CHECK(std::abs(r.at("plane").at("normal")[1].get<double>()) == Approx(1));

    // Midplane between the XY plane and the top face of the body (z = 25).
    r = run(s, "feature.plane", {{"base", "XY"}, {"base2", {{"body", r.at("bodies")[0].at("id")}, {"point", {0, 0, 25}}}}});
    CHECK(r.at("plane").at("origin")[2].get<double>() == Approx(12.5));

    // Through three points; collinear points are rejected.
    r = run(s, "feature.plane", {{"points", Json::array({{0, 0, 0}, {10, 0, 0}, {0, 0, 10}})}});
    CHECK(std::abs(r.at("plane").at("normal")[1].get<double>()) == Approx(1));
    const auto bad = s.execute("feature.plane", {{"points", Json::array({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}})}});
    CHECK_FALSE(bad.ok);

    // The plane is a part of kind "plane" in the document.
    const Json parts = run(s, "document.parts");
    bool found = false;
    for (const auto& p : parts.at("parts"))
        found = found || p.at("kind") == "plane";
    CHECK(found);
}

TEST_CASE("Design commands: plane at angle about an axis or edge (Fusion: Plane at Angle)", "[commands][cad]")
{
    commands::Session s;
    const auto near = [](const Json& v, double x, double y, double z) {
        return v.at(0).get<double>() == Approx(x).margin(1e-6) && v.at(1).get<double>() == Approx(y).margin(1e-6) &&
               v.at(2).get<double>() == Approx(z).margin(1e-6);
    };
    // XY turned 90 degrees about the line through (10, 0, 0) along Y: the plane stands on that line.
    const Json r = run(s, "feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", {{"origin", {10, 0, 0}}, {"direction", {0, 1, 0}}}}});
    CHECK(std::abs(r.at("plane").at("normal")[0].get<double>()) == Approx(1).margin(1e-9));
    CHECK(near(r.at("plane").at("origin"), 10, 0, 10));
    // A construction axis gives the same, and the world axis names work.
    const Json ax = run(s, "feature.axis", {{"from", {10, 0, 0}}, {"to", {10, 5, 0}}});
    const Json c = run(s, "feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", {{"feature", ax.at("id")}}}});
    CHECK(near(c.at("plane").at("origin"), 10, 0, 10));
    const Json x = run(s, "feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", "X"}});
    CHECK(std::abs(x.at("plane").at("normal")[1].get<double>()) == Approx(1).margin(1e-9));
    // Both rotation lines, a zero direction and an object without a line are errors.
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"angle", 90}, {"axis", "y"}, {"about", "X"}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", {{"line", 1}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"angle", 90}, {"about", "W"}}).ok);
    // A bad line is an error at angle 0 too; offset applies after the turn; a long direction is normalised.
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"angle", 0}, {"about", "W"}}).ok);
    const Json o = run(s, "feature.plane", {{"base", "XY"}, {"angle", 90}, {"offset", 5}, {"about", {{"origin", {10, 0, 0}}, {"direction", {0, 5, 0}}}}});
    CHECK(near(o.at("plane").at("origin"), 15, 0, 10)); // the normal after the turn is +X: 5 further along it
}

TEST_CASE("Design commands: plane along a path (Fusion: Plane Along Path)", "[commands][cad]")
{
    commands::Session s;
    const auto near = [](const Json& v, double x, double y, double z) {
        return v.at(0).get<double>() == Approx(x).margin(1e-6) && v.at(1).get<double>() == Approx(y).margin(1e-6) &&
               v.at(2).get<double>() == Approx(z).margin(1e-6);
    };
    const auto line = run(s, "sketch.create",
                          {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {40, 0}}}})}})
                          .at("id").get<std::uint64_t>();
    // Halfway along a line along X: the plane stands across it there.
    const Json mid = run(s, "feature.plane", {{"path", {{"sketch", line}}}, {"position", 0.5}});
    CHECK(near(mid.at("plane").at("origin"), 20, 0, 0));
    CHECK(std::abs(mid.at("plane").at("normal")[0].get<double>()) == Approx(1).margin(1e-9));
    // The default is the start; offset moves along the path direction.
    const Json start = run(s, "feature.plane", {{"path", {{"sketch", line}}}, {"offset", 5}});
    CHECK(near(start.at("plane").at("origin"), 5, 0, 0));
    // Two joined lines: a quarter of 20 + 20 is the first line's middle; a sketch can be placed on the plane.
    const auto bend = run(s, "sketch.create",
                          {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {20, 0}}},
                                                                     {{"type", "line"}, {"start", {20, 0}}, {"end", {20, 20}}}})}})
                          .at("id").get<std::uint64_t>();
    const Json q = run(s, "feature.plane", {{"path", {{"sketch", bend}}}, {"position", 0.25}});
    CHECK(near(q.at("plane").at("origin"), 10, 0, 0));
    const Json three = run(s, "feature.plane", {{"path", {{"sketch", bend}}}, {"position", 0.75}});
    CHECK(near(three.at("plane").at("origin"), 20, 10, 0));
    CHECK(std::abs(three.at("plane").at("normal")[1].get<double>()) == Approx(1).margin(1e-9));
    run(s, "sketch.create", {{"plane", {{"plane", mid.at("id")}}}, {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 3}}})}});
    // Errors: out of range, both a path and points, no such sketch.
    CHECK_FALSE(s.execute("feature.plane", {{"path", {{"sketch", line}}}, {"position", 1.5}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"path", {{"sketch", line}}}, {"points", Json::array({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}})}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"path", {{"sketch", 9999}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"path", {{"sketch", line}}}, {"base", "XZ"}, {"angle", 30}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"base", "XY"}, {"position", 0.5}}).ok);
    // The path sketch stays visible: a plane only reads its geometry.
    for (const auto& p : run(s, "document.parts").at("parts"))
        if (p.at("kind") == "sketch" && p.at("featureId") == line)
            CHECK(p.value("visible", true));
}

TEST_CASE("Design commands: tangent plane (Fusion: Tangent Plane)", "[commands][cad]")
{
    commands::Session s;
    const auto near = [](const Json& v, double x, double y, double z) {
        return v.at(0).get<double>() == Approx(x).margin(1e-4) && v.at(1).get<double>() == Approx(y).margin(1e-4) &&
               v.at(2).get<double>() == Approx(z).margin(1e-4);
    };
    // A cylinder of radius 10 about Z: a plane touching it at the +X side stands at x = 10, normal +X.
    const auto cyl = run(s, "feature.cylinder", {{"center", {0, 0, 0}}, {"radius", 10}, {"height", 30}}).at("id").get<std::uint64_t>();
    const Json r = run(s, "feature.plane", {{"tangent", {{"body", cyl}, {"point", {12, 0, 15}}}}});
    CHECK(near(r.at("plane").at("origin"), 10, 0, 15));
    CHECK(near(r.at("plane").at("normal"), 1, 0, 0));
    // The other side, and an offset along the outward normal.
    const Json l = run(s, "feature.plane", {{"tangent", {{"body", cyl}, {"point", {0, -9, 5}}}}, {"offset", 4}});
    CHECK(near(l.at("plane").at("origin"), 0, -14, 5));
    CHECK(near(l.at("plane").at("normal"), 0, -1, 0));
    // A sketch can be placed on it, and a sphere works as well.
    run(s, "sketch.create", {{"plane", {{"plane", r.at("id")}}}, {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 2}}})}});
    const auto ball = run(s, "feature.sphere", {{"center", {50, 0, 0}}, {"diameter", 14}}).at("id").get<std::uint64_t>();
    const Json b = run(s, "feature.plane", {{"tangent", {{"body", ball}, {"point", {50, 0, 9}}}}});
    CHECK(near(b.at("plane").at("origin"), 50, 0, 7));
    CHECK(near(b.at("plane").at("normal"), 0, 0, 1));
    // The face is stored with a hint, so the plane follows when the cylinder moves.
    run(s, "feature.edit", {{"id", cyl}, {"params", {{"center", {5, 0, 0}}}}});
    CHECK(s.design().planes().at(r.at("id").get<std::uint64_t>()).plane.origin.x == Approx(15).margin(1e-4));
    // Moved sideways, the plane keeps the same spot of the face (still the +X side), not the old world point.
    run(s, "feature.edit", {{"id", cyl}, {"params", {{"center", {0, 7, 0}}}}});
    const auto& moved = s.design().planes().at(r.at("id").get<std::uint64_t>()).plane;
    CHECK(moved.origin.x == Approx(10).margin(1e-4));
    CHECK(moved.origin.y == Approx(7).margin(1e-4));
    CHECK(moved.normal.x == Approx(1).margin(1e-4));
    // Errors: no such body, other modes together with `tangent`, a malformed reference.
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", {{"body", 9999}, {"point", {0, 0, 0}}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", {{"body", cyl}, {"point", {12, 0, 15}}}}, {"base", "XY"}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", 5}}).ok);
    // The touch point is required, and an index must be a plain number.
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", {{"body", cyl}, {"index", 0}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", {{"body", cyl}, {"index", "a"}, {"point", {12, 0, 15}}}}}).ok);
    CHECK_FALSE(s.execute("feature.plane", {{"tangent", {{"body", cyl}, {"index", -1}, {"point", {12, 0, 15}}}}}).ok);
}

TEST_CASE("Design commands: loft of rings stays hollow", "[commands][cad][pattern]")
{
    commands::Session s;
    auto ring = [&](double z, double inner) {
        const Json sk = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"offset", z},
                             {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 10}},
                                                     {{"type", "circle"}, {"center", {0, 0}}, {"radius", inner}}})}});
        const auto id = sk.at("id").get<std::uint64_t>();
        const Json profiles = run(s, "sketch.profiles", {{"sketch", id}});
        std::size_t best = 0;
        double area = 0;
        for (const auto& p : profiles.at("profiles")) {
            if (p.at("area").get<double>() > area) {
                area = p.at("area").get<double>();
                best = p.at("index").get<std::size_t>();
            }
        }
        return Json{{"sketch", id}, {"profile", best}};
    };
    const Json a = ring(0, 5);
    const Json b = ring(10, 5);
    const Json r = run(s, "feature.loft", {{"sections", Json::array({a, b})}});
    CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(3.14159265358979 * (100 - 25) * 10).epsilon(1e-3));

    // A ring and a disk cannot be matched.
    const Json disk = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"offset", 20},
                           {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 8}}})}});
    CHECK_FALSE(s.execute("feature.loft", {{"sections", Json::array({b, disk.at("id")})}}).ok);
}

TEST_CASE("Design commands: deleting sketch curves (CAD-122)", "[commands][cad][solver]")
{
    commands::Session s;
    // A rectangle, a circle and a line; the circle and the line carry dimensions.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {20, 10}}, {"radius", 4}},
                                                 {{"type", "line"}, {"start", {50, 0}}, {"end", {60, 0}}}})},
                         {"constraints", Json::array({{{"type", "radius"}, {"curve", 4}, {"value", 5}},
                                                      {{"type", "length"}, {"curve", 5}, {"value", 12}},
                                                      {{"type", "length"}, {"curve", 1}, {"value", 20}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    CHECK(sk.at("profiles") == 2);

    SECTION("a whole item and its constraint go")
    {
        run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {4}}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 5);
        CHECK(c.at("curves")[4].at("kind") == "line");
        REQUIRE(c.at("constraints").size() == 2);
        // The line's length now refers to curve 4.
        CHECK(c.at("constraints")[0].at("curve") == 4);
        CHECK(c.at("constraints")[0].at("resolved").get<double>() == Approx(12));
        CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);
        // One undo step brings it back.
        run(s, "edit.undo");
        CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 6);
        CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("constraints").size() == 3);
    }

    SECTION("one side of a rectangle: the other sides stay lines that keep their direction")
    {
        run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {2}}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 5);
        for (int i = 0; i < 3; ++i)
            CHECK(c.at("curves")[i].at("kind") == "line");
        CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1); // only the circle
        // The right side (still curve 1) keeps its length and stays vertical when dimensioned again.
        run(s, "sketch.add_constraints",
            {{"sketch", id}, {"constraints", Json::array({{{"type", "length"}, {"curve", 0}, {"value", 30}}})}});
        const Json after = run(s, "sketch.curves", {{"sketch", id}});
        const auto& right = after.at("curves")[1].at("points");
        CHECK(right[0][0].get<double>() == Approx(right[1][0].get<double>()).margin(1e-6));
        CHECK(std::abs(right[1][1].get<double>() - right[0][1].get<double>()) == Approx(20).margin(1e-6));
        const auto& bottom = after.at("curves")[0].at("points");
        CHECK(bottom[0][1].get<double>() == Approx(bottom[1][1].get<double>()).margin(1e-6));
    }

    SECTION("bad indices are refused and change nothing")
    {
        const auto bad = s.execute("sketch.delete_curves", {{"sketch", id}, {"curves", {9}}});
        CHECK_FALSE(bad.ok);
        CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 6);
    }
}

TEST_CASE("Design commands: deleting sketch curves re-indexes a revolve axis", "[commands][cad]")
{
    commands::Session s;
    // A circle first, then a closed triangle with its axis line on the left.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XZ"},
                         {"curves", Json::array({{{"type", "circle"}, {"center", {50, 50}}, {"radius", 2}},
                                                 {{"type", "line"}, {"start", {10, 0}}, {"end", {10, 20}}},
                                                 {{"type", "line"}, {"start", {10, 20}}, {"end", {20, 0}}},
                                                 {{"type", "line"}, {"start", {20, 0}}, {"end", {10, 0}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    const Json profiles = run(s, "sketch.profiles", {{"sketch", id}});
    Json triangle;
    for (const auto& p : profiles.at("profiles"))
        if (p.at("area").get<double>() > 50)
            triangle = p.at("centroid");
    REQUIRE(triangle.is_array());
    run(s, "feature.revolve",
        {{"sketch", id}, {"points", Json::array({triangle})}, {"axis", {{"line", 1}}}, {"angle", 360}});
    const double volume = run(s, "timeline.list").at("bodies")[0].at("volume").get<double>();
    // Removing the circle moves the axis line to index 0; the cone stays the same.
    run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {0}}});
    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("features")[1].at("params").at("axis").at("line") == 0);
    CHECK(tl.at("features")[1].at("status") == "ok");
    CHECK(tl.at("bodies")[0].at("volume").get<double>() == Approx(volume));
}

TEST_CASE("Design commands: deleting a side of a later shape keeps its own constraints (CAD-122 review)",
          "[commands][cad][solver]")
{
    commands::Session s;
    // A circle first: the rectangle's sides are curves 1..4.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "circle"}, {"center", {-30, 0}}, {"radius", 4}},
                                                 {{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {3}}}); // the rectangle's top
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 4);
    CHECK(c.at("curves")[0].at("kind") == "circle");
    std::vector<std::pair<std::string, int>> kept;
    for (const auto& k : c.at("constraints"))
        kept.emplace_back(k.at("type").get<std::string>(), k.at("curve").get<int>());
    // bottom (1) horizontal, right (2) and left (3) vertical; nothing on the circle.
    CHECK(kept == std::vector<std::pair<std::string, int>>{{"horizontal", 1}, {"vertical", 2}, {"vertical", 3}});
}

TEST_CASE("Design commands: splitting a dimensioned shape keeps its solved size (CAD-122 review)",
          "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})},
                         {"constraints", Json::array({{{"type", "length"}, {"curve", 0}, {"value", 60}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {0}}}); // the dimensioned bottom
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 3);
    const auto& top = c.at("curves")[1].at("points");
    CHECK(std::abs(top[1][0].get<double>() - top[0][0].get<double>()) == Approx(60).margin(1e-6));
}

TEST_CASE("Design commands: a deleted revolve axis line stays missing when lines are added (CAD-122 review)",
          "[commands][cad]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XZ"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, -5}}, {"end", {0, 25}}, {"construction", true}},
                                                 {{"type", "rectangle"}, {"corner", {5, 0}}, {"width", 5}, {"height", 20}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "feature.revolve", {{"sketch", id}, {"axis", {{"line", 0}}}});
    run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {0}}});
    Json tl = run(s, "timeline.list");
    CHECK(tl.at("features")[1].at("status") == "error");
    // A new line takes the next index; the revolve must not turn about it.
    run(s, "sketch.add_curves",
        {{"sketch", id}, {"curves", Json::array({{{"type", "line"}, {"start", {30, 0}}, {"end", {30, 20}}}})}});
    tl = run(s, "timeline.list");
    CHECK(tl.at("features")[1].at("status") == "error");
    CHECK(tl.at("features")[1].at("error").get<std::string>().find("deleted") != std::string::npos);
}

TEST_CASE("Design commands: a deleted axis or path can be picked again (CAD-122 review)", "[commands][cad]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XZ"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, -5}}, {"end", {0, 25}}, {"construction", true}},
                                                 {{"type", "rectangle"}, {"corner", {5, 0}}, {"width", 5}, {"height", 20}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    const auto rev = run(s, "feature.revolve", {{"sketch", id}, {"axis", {{"line", 0}}}}).at("id").get<std::uint64_t>();
    run(s, "sketch.delete_curves", {{"sketch", id}, {"curves", {0}}});
    run(s, "sketch.add_curves",
        {{"sketch", id}, {"curves", Json::array({{{"type", "line"}, {"start", {0, -5}}, {"end", {0, 25}}, {"construction", true}}})}});
    // The dialog sends the new axis as a merge patch.
    run(s, "feature.edit", {{"id", rev}, {"params", {{"axis", {{"line", 4}}}}}});
    Json tl = run(s, "timeline.list");
    CHECK(tl.at("features")[1].at("status") == "ok");
    CHECK_FALSE(tl.at("features")[1].at("params").at("axis").contains("deletedLine"));

    // Sweep: a deleted path curve, then the path sketch picked again (all its curves).
    const Json path = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {0, 40}}},
                                                   {{"type", "line"}, {"start", {50, 0}}, {"end", {50, 40}}}})}});
    const auto pid = path.at("id").get<std::uint64_t>();
    const Json prof = run(s, "sketch.create",
                          {{"plane", "XZ"}, {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 2}}})}});
    const auto sw = run(s, "feature.sweep",
                        {{"sketch", prof.at("id")}, {"path", {{"sketch", pid}, {"curves", {0}}}}}).at("id").get<std::uint64_t>();
    run(s, "sketch.delete_curves", {{"sketch", pid}, {"curves", {0}}});
    tl = run(s, "timeline.list");
    CHECK(tl.at("features").back().at("status") == "error");
    run(s, "feature.edit", {{"id", sw}, {"params", {{"path", {{"sketch", pid}}}}}});
    tl = run(s, "timeline.list");
    CHECK(tl.at("features").back().at("status") == "ok");
}

TEST_CASE("Design commands: dragging sketch points (CAD-123)", "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {80, 0}}, {"radius", 5}}})},
                         {"constraints", Json::array({{{"type", "radius"}, {"curve", 4}, {"value", 5}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();

    // The top-right corner of the rectangle goes to (50, 30): it stays a rectangle.
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 1}, {"point", "end"}}}, {"to", {50, 30}}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& right = c.at("curves")[1].at("points");
    CHECK(right[1][0].get<double>() == Approx(50).margin(1e-4));
    CHECK(right[1][1].get<double>() == Approx(30).margin(1e-4));
    CHECK(right[0][0].get<double>() == Approx(50).margin(1e-4)); // still vertical
    const auto& bottom = c.at("curves")[0].at("points");
    CHECK(bottom[0][1].get<double>() == Approx(bottom[1][1].get<double>()).margin(1e-6));
    CHECK(c.at("constraints").size() == 1); // the hold is not kept

    // The circle's centre moves; its radius dimension holds.
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 4}, {"point", "center"}}}, {"to", {90, 10}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[4].at("points")[0][0].get<double>() == Approx(90).margin(1e-4));
    CHECK(c.at("curves")[4].at("radius").get<double>() == Approx(5).margin(1e-6));

    // A fixed point does not move, and nothing changes.
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}}})}});
    const auto bad = s.execute("sketch.move_point",
                               {{"sketch", id}, {"point", {{"curve", 0}, {"point", "start"}}}, {"to", {-10, -10}}});
    CHECK_FALSE(bad.ok);
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[0].at("points")[0][0].get<double>() == Approx(0).margin(1e-6));

    // One undo step per drag.
    run(s, "edit.undo"); // the fix
    run(s, "edit.undo"); // the circle drag
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[4].at("points")[0][0].get<double>() == Approx(80).margin(1e-4));
}

TEST_CASE("Design commands: fit points between the ends of a spline (CAD-198)", "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "spline"}, {"points", {{0, 0}, {10, 10}, {20, 0}, {30, 5}}}},
                                                 {{"type", "line"}, {"start", {0, 30}}, {"end", {40, 30}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();

    // Dragging the second fit point moves only it; the ends stay.
    run(s, "sketch.move_point",
        {{"sketch", id}, {"point", {{"curve", 0}, {"point", "fit"}, {"index", 1}}}, {"to", {10, 20}}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& pts = c.at("curves")[0].at("points");
    REQUIRE(pts.size() == 4);
    CHECK(pts[1][0].get<double>() == Approx(10).margin(1e-4));
    CHECK(pts[1][1].get<double>() == Approx(20).margin(1e-4));
    CHECK(pts[0][0].get<double>() == Approx(0).margin(1e-6));
    CHECK(pts[2][0].get<double>() == Approx(20).margin(1e-6));
    CHECK(pts[3][1].get<double>() == Approx(5).margin(1e-6));

    // A fit point can be constrained too: coincident with the end of the line.
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "coincident"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "fit"}, {"index", 2}},
                                                               {{"curve", 1}, {"point", "end"}}})}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& a = c.at("curves")[0].at("points")[2];
    const auto& b = c.at("curves")[1].at("points")[1];
    CHECK(a[0].get<double>() == Approx(b[0].get<double>()).margin(1e-5));
    CHECK(a[1].get<double>() == Approx(b[1].get<double>()).margin(1e-5));

    // A Fix on a fit point holds it where it is (not at the spline's start).
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "fit"}, {"index", 1}}})}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& fixAt = c.at("constraints").back().at("at")[0];
    CHECK(fixAt[0].get<double>() == Approx(10).margin(1e-4));
    CHECK(fixAt[1].get<double>() == Approx(20).margin(1e-4));
    CHECK(c.at("curves")[0].at("points")[1][1].get<double>() == Approx(20).margin(1e-4));

    // A Fix on the whole spline holds its inner fit points too.
    const Json sk2 = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "spline"}, {"points", {{0, 50}, {10, 60}, {20, 50}}}}})},
                          {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({0})}}})}});
    const auto id2 = sk2.at("id").get<std::uint64_t>();
    const auto held = s.execute("sketch.move_point",
                                {{"sketch", id2}, {"point", {{"curve", 0}, {"point", "fit"}, {"index", 1}}}, {"to", {10, 70}}});
    CHECK_FALSE(held.ok);
    c = run(s, "sketch.curves", {{"sketch", id2}});
    CHECK(c.at("curves")[0].at("points")[1][1].get<double>() == Approx(60).margin(1e-6));

    // An index past the fit points is an error, not a crash.
    const auto bad = s.execute("sketch.move_point",
                               {{"sketch", id}, {"point", {{"curve", 0}, {"point", "fit"}, {"index", 9}}}, {"to", {0, 0}}});
    CHECK_FALSE(bad.ok);
    const auto line = s.execute("sketch.move_point",
                                {{"sketch", id}, {"point", {{"curve", 1}, {"point", "fit"}, {"index", 0}}}, {"to", {0, 0}}});
    CHECK_FALSE(line.ok);
}

TEST_CASE("Design commands: dragging keeps expressions and regular polygons (CAD-123)", "[commands][cad][solver]")
{
    {
        // A centre rectangle's corner dragged past the opposite corner stays where it is put.
        commands::Session t;
        const auto rid = run(t, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                             .at("id")
                             .get<std::uint64_t>();
        run(t, "sketch.move_point", {{"sketch", rid}, {"point", {{"curve", 0}, {"point", "start"}}}, {"to", {30, 20}}});
        const Json rc = run(t, "sketch.curves", {{"sketch", rid}});
        const auto& p = rc.at("curves")[0].at("points")[0];
        CHECK(p[0].get<double>() == Approx(30).margin(1e-4));
        CHECK(p[1].get<double>() == Approx(20).margin(1e-4));
    }
    commands::Session s;
    run(s, "parameter.set", {{"name", "r"}, {"expression", "6 mm"}});
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {20, 0}}},
                                                 {{"type", "circle"}, {"center", {50, 0}}, {"radius", "r"}},
                                                 {{"type", "polygon"}, {"center", {0, 50}}, {"radius", 10}, {"sides", 5}}})}})
                        .at("id")
                        .get<std::uint64_t>();

    // A line end is dragged: the circle does not move and keeps its parameter.
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 0}, {"point", "end"}}}, {"to", {25, 5}}});
    run(s, "parameter.set", {{"name", "r"}, {"expression", "8 mm"}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[1].at("radius").get<double>() == Approx(8));

    // A polygon vertex goes where it is dragged, and the polygon stays regular.
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 4}, {"point", "start"}}}, {"to", {0, 65}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& v = c.at("curves")[4].at("points")[0];
    CHECK(v[0].get<double>() == Approx(0).margin(1e-4));
    CHECK(v[1].get<double>() == Approx(65).margin(1e-4));
    std::vector<double> sides;
    for (std::size_t i = 2; i < 7; ++i) {
        const auto& p = c.at("curves")[i].at("points");
        sides.push_back(std::hypot(p[1][0].get<double>() - p[0][0].get<double>(), p[1][1].get<double>() - p[0][1].get<double>()));
    }
    for (const double l : sides)
        CHECK(l == Approx(sides[0]).epsilon(1e-6));
    // Regular: every vertex at the same distance from the centre of the vertices.
    double cx = 0, cy = 0;
    for (std::size_t i = 2; i < 7; ++i) {
        cx += c.at("curves")[i].at("points")[0][0].get<double>() / 5;
        cy += c.at("curves")[i].at("points")[0][1].get<double>() / 5;
    }
    const auto& p0 = c.at("curves")[2].at("points")[0];
    const double r0 = std::hypot(p0[0].get<double>() - cx, p0[1].get<double>() - cy);
    for (std::size_t i = 3; i < 7; ++i) {
        const auto& p = c.at("curves")[i].at("points")[0];
        CHECK(std::hypot(p[0].get<double>() - cx, p[1].get<double>() - cy) == Approx(r0).epsilon(1e-6));
    }
}

TEST_CASE("Design commands: extrude to object (CAD-125)", "[commands][cad]")
{
    commands::Session s;
    // A round bar along X at z = 30 (radius 10) and a 10 x 10 square under it.
    const auto barSketch = run(s, "sketch.create",
                               {{"plane", "YZ"},
                                {"curves", Json::array({{{"type", "circle"}, {"center", {0, 30}}, {"radius", 10}}})}})
                               .at("id")
                               .get<std::uint64_t>();
    const auto bar = run(s, "feature.extrude", {{"sketch", barSketch}, {"distance", 20}, {"extent", "symmetric"}})
                         .at("id")
                         .get<std::uint64_t>();
    const auto sq = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {0, 0}}, {"width", 10}, {"height", 10}}})}})
                        .at("id")
                        .get<std::uint64_t>();

    // Up to the bar: the top follows its round underside.
    // V = 10 * (10 * 30 - integral of sqrt(100 - y^2) over [-5, 5]).
    const double expected = 10.0 * (300.0 - (5.0 * std::sqrt(75.0) + 100.0 * std::asin(0.5)));
    Json ex = run(s, "feature.extrude", {{"sketch", sq}, {"extent", "to_object"}, {"to", {{"body", bar}}}});
    REQUIRE(ex.at("bodies").size() == 2); // the bar and the new body
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(expected).epsilon(1e-4));
    const auto ext = ex.at("id").get<std::uint64_t>();

    // The other way (flip) nothing is there: an error, nothing built.
    const auto bad = s.execute("feature.edit", {{"id", ext}, {"params", {{"flip", true}}}});
    CHECK_FALSE(bad.ok);

    // Up to a picked flat face: the bar's end cap is not in the way, a box top is.
    run(s, "edit.undo"); // the extrude
    const auto box = run(s, "feature.box", {{"corner", {-20, -20, 12}}, {"size", {40, 40, 3}}}).at("id").get<std::uint64_t>();
    (void)box;
    ex = run(s, "feature.extrude",
             {{"sketch", sq}, {"extent", "to_object"}, {"to", {{"body", box}, {"faces", Json::array({{{"point", {0, 0, 15}}}})}}}});
    // Through the box (its top face at z = 15 picked): 10 x 10 x 15.
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(1500).epsilon(1e-6));
    // The picked top face is followed when the box gets taller (z 12..20): the old pick point
    // (z = 15) is now nearer the bottom, but the hints keep the top.
    run(s, "feature.edit", {{"id", box}, {"params", {{"size", {40, 40, 8}}}}});
    Json bodiesNow = run(s, "timeline.list").at("bodies");
    CHECK(bodiesNow.back().at("volume").get<double>() == Approx(2000).epsilon(1e-6));
    run(s, "feature.edit", {{"id", box}, {"params", {{"size", {40, 40, 3}}}}});

    // A target narrower than the profile does not stop all of it: refused, not a column
    // running past the object.
    const auto wide = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "center_rectangle"}, {"center", {100, 0}}, {"width", 20}, {"height", 20}}})}})
                          .at("id")
                          .get<std::uint64_t>();
    const auto small = run(s, "feature.box", {{"corner", {95, -5, 20}}, {"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    CHECK_FALSE(s.execute("feature.extrude", {{"sketch", wide}, {"extent", "to_object"}, {"to", {{"body", small}}}}).ok);
}

TEST_CASE("Design commands: construction curves (CAD-126)", "[commands][cad]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "line"}, {"start", {0, 0}}, {"end", {40, 20}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    CHECK(sk.at("profiles") == 2); // the diagonal splits the rectangle

    // The diagonal as construction: one profile again; indices do not change.
    Json r = run(s, "sketch.set_construction", {{"sketch", id}, {"curves", {4}}});
    CHECK(r.at("profiles") == 1);
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[4].at("construction") == true);
    CHECK(c.at("curves")[0].at("construction") == false);

    // One side of the rectangle: the shape keeps its constraints (still a rectangle when a
    // side is dimensioned), and the outline is open: no profile.
    r = run(s, "sketch.set_construction", {{"sketch", id}, {"curves", {2}}});
    CHECK(r.at("profiles") == 0);
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves").size() == 5);
    CHECK(c.at("curves")[2].at("construction") == true);
    run(s, "sketch.add_constraints", {{"sketch", id}, {"constraints", Json::array({{{"type", "length"}, {"curve", 1}, {"value", 30}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& left = c.at("curves")[3].at("points");
    CHECK(std::abs(left[0][1].get<double>() - left[1][1].get<double>()) == Approx(30).margin(1e-4)); // parallel sides follow

    // Back to normal: the outline closes again; one undo step each.
    run(s, "sketch.set_construction", {{"sketch", id}, {"curves", {2}}, {"construction", false}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[2].at("construction") == false);
    run(s, "edit.undo");
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[2].at("construction") == true);
}

TEST_CASE("Design commands: trim sketch curves (CAD-126)", "[commands][cad]")
{
    commands::Session s;
    // A plus sign: two lines crossing at (0, 0), and a circle of radius 10 through both.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {-20, 0}}, {"end", {20, 0}}},
                                                 {{"type", "line"}, {"start", {0, -20}}, {"end", {0, 20}}},
                                                 {{"type", "circle"}, {"center", {0, 0}}, {"radius", 10}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    auto lineLength = [](const Json& c) {
        const auto& p = c.at("points");
        return std::hypot(p[1][0].get<double>() - p[0][0].get<double>(), p[1][1].get<double>() - p[0][1].get<double>());
    };

    // The horizontal line's end outside the circle (x 10..20) goes.
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {15, 0.3}}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 3);
    // Line 0 is now the remaining piece -20..10 (added at the end).
    const auto& last = c.at("curves")[2];
    CHECK(last.at("kind") == "line");
    CHECK(lineLength(last) == Approx(30).margin(1e-6));

    // The circle's quarter between the two lines (upper right) goes: a 270 degree arc stays.
    c = run(s, "sketch.curves", {{"sketch", id}});
    std::size_t circle = 0;
    for (std::size_t i = 0; i < c.at("curves").size(); ++i)
        if (c.at("curves")[i].at("kind") == "circle")
            circle = i;
    run(s, "sketch.trim", {{"sketch", id}, {"curve", circle}, {"at", {7, 7}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    bool arc = false;
    for (const auto& k : c.at("curves")) {
        if (k.at("kind") != "arc")
            continue;
        arc = true;
        const auto& p = k.at("points");
        // From (0, 10) round the left to (10, 0): the through point is at (-10, 0)-ish side.
        CHECK(p[1][0].get<double>() < 0);
        const std::set<std::pair<long, long>> ends{{std::lround(p[0][0].get<double>()), std::lround(p[0][1].get<double>())},
                                                   {std::lround(p[2][0].get<double>()), std::lround(p[2][1].get<double>())}};
        CHECK(ends == std::set<std::pair<long, long>>{{0, 10}, {10, 0}});
    }
    CHECK(arc);

    // A line that crosses nothing is removed whole; one undo step.
    const std::size_t before = c.at("curves").size();
    run(s, "sketch.add_curves", {{"sketch", id}, {"curves", Json::array({{{"type", "line"}, {"start", {50, 50}}, {"end", {60, 50}}}})}});
    run(s, "sketch.trim", {{"sketch", id}, {"curve", before}, {"at", {55, 50}}});
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == before);
    run(s, "edit.undo");
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == before + 1);
}

TEST_CASE("Design commands: mirror sketch curves (CAD-126)", "[commands][cad][solver]")
{
    commands::Session s;
    // A vertical centre line at x = 0, a triangle on its right and a circle.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, -30}}, {"end", {0, 30}}, {"construction", true}},
                                                 {{"type", "line"}, {"start", {0, 0}}, {"end", {20, 0}}},
                                                 {{"type", "line"}, {"start", {20, 0}}, {"end", {0, 15}}},
                                                 {{"type", "circle"}, {"center", {10, 20}}, {"radius", 3}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.mirror", {{"sketch", id}, {"curves", {1, 2, 3}}, {"line", 0}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 7);
    CHECK(c.at("curves")[4].at("points")[1][0].get<double>() == Approx(-20).margin(1e-6));
    CHECK(c.at("curves")[6].at("points")[0][0].get<double>() == Approx(-10).margin(1e-6));
    CHECK(c.at("curves")[6].at("radius").get<double>() == Approx(3).margin(1e-6));
    // Closed with the mirrored side: one triangle profile (two halves) plus the circles.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() >= 3);

    // The copies follow: a dimension on the original moves the mirrored point too (the
    // mirror line is fixed, else the solver may move it as well).
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"curve", 0}}, {{"type", "length"}, {"curve", 1}, {"value", 25}}, {{"type", "radius"}, {"curve", 3}, {"value", 5}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const double xo = c.at("curves")[1].at("points")[1][0].get<double>();
    const double xm = c.at("curves")[4].at("points")[1][0].get<double>();
    CHECK(xm == Approx(-xo).margin(1e-5));
    CHECK(c.at("curves")[6].at("radius").get<double>() == Approx(5).margin(1e-6));

    // A spline's copy keeps its ends mirrored.
    {
        commands::Session t;
        const auto sid = run(t, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "line"}, {"start", {0, -30}}, {"end", {0, 30}}},
                                                      {{"type", "spline"}, {"points", Json::array({{5, 0}, {10, 8}, {20, 3}})}}})}})
                             .at("id")
                             .get<std::uint64_t>();
        run(t, "sketch.mirror", {{"sketch", sid}, {"curves", {1}}, {"line", 0}});
        run(t, "sketch.add_constraints", {{"sketch", sid}, {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({0})}}})}});
        run(t, "sketch.move_point", {{"sketch", sid}, {"point", {{"curve", 1}, {"point", "end"}}}, {"to", {25, 6}}});
        const Json sc = run(t, "sketch.curves", {{"sketch", sid}});
        const auto& copy = sc.at("curves")[2].at("points");
        CHECK(copy.back()[0].get<double>() == Approx(-25).margin(1e-5));
        CHECK(copy.back()[1].get<double>() == Approx(6).margin(1e-5));
    }

    // The mirror line must be a line; one undo step.
    CHECK_FALSE(s.execute("sketch.mirror", {{"sketch", id}, {"curves", {1}}, {"line", 3}}).ok);
    run(s, "edit.undo");
    run(s, "edit.undo");
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 4);
}

TEST_CASE("Design commands: sketch fillet (CAD-126)", "[commands][cad][solver]")
{
    commands::Session s;
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    // Bottom (0) and right (1) sides meet at (40, 0).
    run(s, "sketch.fillet", {{"sketch", id}, {"curves", {0, 1}}, {"radius", 5}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 5);
    CHECK(c.at("curves")[4].at("kind") == "arc");
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 1}});
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(800 - (25 - std::numbers::pi * 25 / 4)).epsilon(1e-6));

    // The radius is a dimension: editing it updates the corner (and the extrude).
    c = run(s, "sketch.curves", {{"sketch", id}});
    std::size_t radiusIndex = 0;
    for (std::size_t i = 0; i < c.at("constraints").size(); ++i)
        if (c.at("constraints")[i].at("type") == "radius")
            radiusIndex = i;
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", radiusIndex}, {"value", 8}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& arcCurve = c.at("curves")[4];
    CHECK(std::hypot(arcCurve.at("points")[0][0].get<double>() - arcCurve.at("center")[0].get<double>(),
                     arcCurve.at("points")[0][1].get<double>() - arcCurve.at("center")[1].get<double>()) == Approx(8).margin(1e-6));
    // Still tangent: the arc ends where the (still horizontal) bottom line ends.
    const auto& bottom = c.at("curves")[2].at("points");
    CHECK(bottom[0][1].get<double>() == Approx(bottom[1][1].get<double>()).margin(1e-6));
    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("features").back().at("status") == "ok"); // the extrude still has its profile

    // Too large a radius, or lines that do not meet: refused.
    CHECK_FALSE(s.execute("sketch.fillet", {{"sketch", id}, {"curves", {0, 1}}, {"radius", 50}}).ok);

    // Rounding a corner of two construction lines gives a construction arc (no new profile).
    const auto cid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {20, 0}}, {"construction", true}},
                                                  {{"type", "line"}, {"start", {20, 0}}, {"end", {20, 20}}, {"construction", true}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    run(s, "sketch.fillet", {{"sketch", cid}, {"curves", {0, 1}}, {"radius", 5}});
    const Json cc = run(s, "sketch.curves", {{"sketch", cid}});
    REQUIRE(cc.at("curves").size() == 3);
    for (const auto& curve : cc.at("curves"))
        CHECK(curve.at("construction") == true);
}

TEST_CASE("Design commands: Sketch Chamfer (CAD-254)", "[commands][cad][solver]")
{
    commands::Session s;
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };
    // Bottom (0) and right (1) sides meet at (40, 0).
    run(s, "sketch.chamfer", {{"sketch", id}, {"curves", {0, 1}}, {"distance", 5}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    // Left two sides stay (0, 1); the shortened lines (2, 3), the chamfer (4), the cut-off
    // corner as two construction lines (5, 6).
    REQUIRE(c.at("curves").size() == 7);
    CHECK(near(c.at("curves")[4].at("points")[0], 35, 0));
    CHECK(near(c.at("curves")[4].at("points")[1], 40, 5));
    CHECK(c.at("curves")[4].at("construction") == false);
    CHECK(c.at("curves")[5].at("construction") == true);
    CHECK(c.at("curves")[6].at("construction") == true);
    CHECK(c.at("degreesOfFreedom") == 4); // as the rectangle: position, width, height
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 1}});
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(800 - 12.5).epsilon(1e-6));

    // The distances are dimensions: editing them moves the chamfer (and the extrude follows).
    std::vector<std::size_t> lengths;
    for (std::size_t i = 0; i < c.at("constraints").size(); ++i)
        if (c.at("constraints")[i].at("type") == "length")
            lengths.push_back(i);
    REQUIRE(lengths.size() == 2);
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", lengths[0]}, {"value", 8}});
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", lengths[1]}, {"value", 8}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    // The rectangle's size is free (not dimensioned), so measure from the cut-off corner:
    // where the shortened bottom (2) and right (3) lines meet when extended.
    const double cornerY = c.at("curves")[2].at("points")[0][1].get<double>();
    const double cornerX = c.at("curves")[3].at("points")[0][0].get<double>();
    INFO(c.at("curves").dump());
    CHECK(near(c.at("curves")[4].at("points")[0], cornerX - 8, cornerY));
    CHECK(near(c.at("curves")[4].at("points")[1], cornerX, cornerY + 8));
    CHECK(run(s, "timeline.list").at("features").back().at("status") == "ok");

    // One undo step brings the corner back (after undoing the edits and the extrude).
    for (int i = 0; i < 4; ++i)
        run(s, "edit.undo");
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 4);

    // Refused: too long for a side, lines that do not meet, no distance.
    CHECK_FALSE(s.execute("sketch.chamfer", {{"sketch", id}, {"curves", {0, 1}}, {"distance", 25}}).ok);
    CHECK_FALSE(s.execute("sketch.chamfer", {{"sketch", id}, {"curves", {0, 2}}, {"distance", 5}}).ok);
    CHECK_FALSE(s.execute("sketch.chamfer", {{"sketch", id}, {"curves", {0, 1}}, {"distance", 0}}).ok);
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 4);

    // A turned corner (not along the axes) is cut the same way.
    const auto tid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {30, 10}}},
                                                  {{"type", "line"}, {"start", {30, 10}}, {"end", {10, 40}}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    run(s, "sketch.chamfer", {{"sketch", tid}, {"curves", {0, 1}}, {"distance", 5}});
    const Json t = run(s, "sketch.curves", {{"sketch", tid}});
    const double la = std::hypot(30.0, 10.0), lb = std::hypot(20.0, 30.0);
    CHECK(near(t.at("curves")[2].at("points")[0], 30 - 5 * 30 / la, 10 - 5 * 10 / la));
    CHECK(near(t.at("curves")[2].at("points")[1], 30 - 5 * 20 / lb, 10 + 5 * 30 / lb));

    // Two neighbouring corners: the second chamfer replaces the side shared with the first,
    // and the first chamfer's cut-off corner stays tied to that side (no free rotation).
    const auto rid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    run(s, "sketch.chamfer", {{"sketch", rid}, {"curves", {0, 1}}, {"distance", 5}});
    // Now top (0), left (1), shortened bottom (2), shortened right (3, from (40, 20)), ...
    Json r = run(s, "sketch.curves", {{"sketch", rid}});
    REQUIRE(near(r.at("curves")[3].at("points")[0], 40, 20));
    run(s, "sketch.chamfer", {{"sketch", rid}, {"curves", {3, 0}}, {"distance", 4}});
    r = run(s, "sketch.curves", {{"sketch", rid}});
    INFO(r.dump());
    REQUIRE(r.at("curves").size() == 10);
    CHECK(r.at("degreesOfFreedom") == 4);
    std::size_t collinear = 0;
    for (const auto& k : r.at("constraints"))
        collinear += k.at("type") == "collinear" ? 1 : 0;
    CHECK(collinear == 4);
}

TEST_CASE("Design commands: Sketch Ellipse (CAD-256)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "ellipse"}, {"center", {10, 5}}, {"major_radius", 20},
                                                  {"minor_radius", 8}, {"angle", 30}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 1);
    const Json e = c.at("curves")[0];
    CHECK(e.at("kind") == "ellipse");
    CHECK(near(e.at("center"), 10, 5));
    CHECK(e.at("majorRadius").get<double>() == Approx(20));
    CHECK(e.at("minorRadius").get<double>() == Approx(8));
    CHECK(e.at("angle").get<double>() == Approx(30));
    CHECK(e.at("samples").size() > 32);
    // Centre, axis end, minor radius: 5 degrees of freedom.
    CHECK(c.at("degreesOfFreedom") == 5);
    // A closed profile: extruded, its area is pi a b.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 2}});
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(std::numbers::pi * 20 * 8 * 2).epsilon(1e-4));
    run(s, "edit.undo");

    // Dimensions: major and minor radius, the major axis horizontal, the centre at the origin.
    c = run(s, "sketch.add_constraints",
            {{"sketch", id},
             {"constraints", Json::array({{{"type", "major_radius"}, {"curve", 0}, {"value", 25}},
                                          {{"type", "minor_radius"}, {"curve", 0}, {"value", 10}},
                                          {{"type", "horizontal"}, {"curve", 0}},
                                          {{"type", "coincident"},
                                           {"points", Json::array({{{"curve", 0}, {"point", "center"}}, "origin"})}}})}});
    CHECK(c.at("degreesOfFreedom") == 0);
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    CHECK(near(c.at("curves")[0].at("center"), 0, 0));
    CHECK(near(c.at("curves")[0].at("points")[1], 25, 0));
    CHECK(c.at("curves")[0].at("minorRadius").get<double>() == Approx(10));
    // Edited like any dimension.
    std::size_t minorIdx = 0;
    for (std::size_t i = 0; i < c.at("constraints").size(); ++i)
        if (c.at("constraints")[i].at("type") == "minor_radius")
            minorIdx = i;
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", minorIdx}, {"value", 12}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[0].at("minorRadius").get<double>() == Approx(12));

    // Refused: no radius, a radius on the wrong curve.
    CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"},
                                            {"curves", Json::array({{{"type", "ellipse"}, {"center", {0, 0}},
                                                                     {"major_radius", 0}, {"minor_radius", 3}}})}})
                    .ok);

    // A minor radius longer than the major one is the same oval turned: still a profile.
    const auto tall = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "ellipse"}, {"center", {0, 0}}, {"major_radius", 5},
                                                    {"minor_radius", 15}}})}})
                          .at("id")
                          .get<std::uint64_t>();
    const auto tex = run(s, "feature.extrude", {{"sketch", tall}, {"distance", 1}});
    CHECK(tex.at("bodies").back().at("volume").get<double>() == Approx(std::numbers::pi * 5 * 15).epsilon(1e-4));

    // Mirror, move, scale and pattern keep it an ellipse.
    const auto mid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "line"}, {"start", {0, -50}}, {"end", {0, 50}}, {"construction", true}},
                                                  {{"type", "ellipse"}, {"center", {20, 0}}, {"major_radius", 10},
                                                   {"minor_radius", 4}, {"angle", 90}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    const int dofBefore = run(s, "sketch.curves", {{"sketch", mid}}).at("degreesOfFreedom").get<int>();
    run(s, "sketch.mirror", {{"sketch", mid}, {"curves", {1}}, {"line", 0}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    REQUIRE(c.at("curves").size() == 3);
    CHECK(c.at("curves")[2].at("kind") == "ellipse");
    CHECK(near(c.at("curves")[2].at("center"), -20, 0));
    CHECK(c.at("curves")[2].at("majorRadius").get<double>() == Approx(10));
    CHECK(c.at("curves")[2].at("minorRadius").get<double>() == Approx(4));
    // The copy follows the original (symmetric + equal radii): no extra freedom.
    CHECK(c.at("degreesOfFreedom").get<int>() == dofBefore);

    run(s, "sketch.move_curves", {{"sketch", mid}, {"curves", {1}}, {"angle", 90}, {"pivot", {20, 0}}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    CHECK(c.at("curves")[1].at("kind") == "ellipse");
    CHECK(near(c.at("curves")[1].at("center"), 20, 0));
    CHECK(c.at("curves")[1].at("minorRadius").get<double>() == Approx(4));
    CHECK(near(c.at("curves")[1].at("points")[1], 10, 0)); // the axis turned from +y to -x

    const auto sc = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "ellipse"}, {"center", {0, 0}}, {"major_radius", 6},
                                                  {"minor_radius", 3}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.add_constraints", {{"sketch", sc},
                                      {"constraints", Json::array({{{"type", "major_radius"}, {"curve", 0}, {"value", 6}},
                                                                   {{"type", "minor_radius"}, {"curve", 0}, {"value", 3}}})}});
    run(s, "sketch.scale_curves", {{"sketch", sc}, {"curves", {0}}, {"factor", 2}, {"point", {0, 0}}});
    c = run(s, "sketch.curves", {{"sketch", sc}});
    CHECK(c.at("curves")[0].at("majorRadius").get<double>() == Approx(12));
    CHECK(c.at("curves")[0].at("minorRadius").get<double>() == Approx(6));
    run(s, "sketch.circular_pattern", {{"sketch", sc}, {"curves", {0}}, {"center", {30, 0}}, {"count", 3}});
    c = run(s, "sketch.curves", {{"sketch", sc}});
    REQUIRE(c.at("curves").size() == 3);
    for (const auto& k : c.at("curves")) {
        CHECK(k.at("kind") == "ellipse");
        CHECK(k.at("minorRadius").get<double>() == Approx(6));
        CHECK(k.at("majorRadius").get<double>() == Approx(12));
    }

    // Extend, Fillet and Break (nothing crosses it) refuse it; Offset gives a closed spline;
    // Delete removes it. Trim of an ellipse is covered by the CAD-258 test.
    CHECK_FALSE(s.execute("sketch.extend", {{"sketch", sc}, {"curve", 0}, {"at", {12, 0}}}).ok);
    CHECK_FALSE(s.execute("sketch.break", {{"sketch", sc}, {"curve", 0}, {"at", {12, 0}}}).ok);
    CHECK_FALSE(s.execute("sketch.fillet", {{"sketch", sc}, {"curves", {0, 1}}, {"radius", 1}}).ok);
    run(s, "sketch.offset", {{"sketch", sc}, {"curves", {0}}, {"distance", 1}});
    c = run(s, "sketch.curves", {{"sketch", sc}});
    REQUIRE(c.at("curves").size() == 4);
    CHECK(c.at("curves")[3].at("kind") == "spline");
    CHECK(c.at("curves")[3].at("closed") == true);
    run(s, "sketch.delete_curves", {{"sketch", sc}, {"curves", {0, 3}}});
    CHECK(run(s, "sketch.curves", {{"sketch", sc}}).at("curves").size() == 2);
}

TEST_CASE("Design commands: Trim and Break an ellipse (CAD-258)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y, double tol = 1e-6) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < tol;
    };
    // How far (x, y) is off the ellipse x^2/a^2 + y^2/b^2 = 1 (axis-aligned, centred at c).
    auto onEllipse = [](const Json& p, double cx, double a, double b) {
        const double x = (p[0].get<double>() - cx) / a, y = p[1].get<double>() / b;
        return std::abs(x * x + y * y - 1) < 1e-9;
    };
    // A half ellipse closed by its minor axis: trim the left half and the line's overhangs.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "ellipse"}, {"center", {0, 0}}, {"major_radius", 20},
                                                  {"minor_radius", 10}},
                                                 {{"type", "line"}, {"start", {0, -15}}, {"end", {0, 15}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {-20, 0}}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    // The line stays first; the right half of the ellipse is the new last curve.
    const Json arc = c.at("curves")[1];
    CHECK(arc.at("kind") == "elliptical_arc");
    CHECK(near(arc.at("points")[0], 0, -10));
    CHECK(near(arc.at("points")[3], 0, 10));
    CHECK(arc.at("majorRadius").get<double>() == Approx(20));
    CHECK(arc.at("minorRadius").get<double>() == Approx(10));
    // Its drawing runs through (20, 0), the right side.
    bool right = false;
    for (const auto& p : arc.at("samples"))
        right = right || near(p, 20, 0, 1e-3);
    CHECK(right);
    // The line's overhangs: cut exactly at the ellipse.
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {0, 13}}});
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 1}, {"at", {0, -13}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    std::size_t line = c.at("curves")[0].at("kind") == "line" ? 0 : 1;
    const Json l = c.at("curves")[line];
    CHECK(((near(l.at("points")[0], 0, -10) && near(l.at("points")[1], 0, 10)) ||
           (near(l.at("points")[0], 0, 10) && near(l.at("points")[1], 0, -10))));
    // A closed profile: half of pi a b.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 2}});
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(std::numbers::pi * 20 * 10 / 2 * 2).epsilon(1e-4));
    run(s, "edit.undo");

    // The arc keeps its ellipse's dimensions; changed, the outline stays closed.
    const std::size_t ea = 1 - line;
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "major_radius"}, {"curve", ea}, {"value", 30}},
                                      {{"type", "minor_radius"}, {"curve", ea}, {"value", 12}},
                                      {{"type", "horizontal"}, {"curve", ea}},
                                      {{"type", "vertical"}, {"curve", line}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    CHECK(c.at("curves")[ea].at("majorRadius").get<double>() == Approx(30));
    CHECK(c.at("curves")[ea].at("minorRadius").get<double>() == Approx(12));
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);

    // Break: an ellipse crossed by a line becomes two elliptical arcs.
    const auto bid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "ellipse"}, {"center", {100, 0}}, {"major_radius", 20},
                                                   {"minor_radius", 10}},
                                                  {{"type", "line"}, {"start", {90, -20}}, {"end", {90, 20}}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    run(s, "sketch.break", {{"sketch", bid}, {"curve", 0}, {"at", {120, 0}}});
    c = run(s, "sketch.curves", {{"sketch", bid}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 3);
    for (std::size_t i = 1; i < 3; ++i) {
        CHECK(c.at("curves")[i].at("kind") == "elliptical_arc");
        CHECK(onEllipse(c.at("curves")[i].at("points")[0], 100, 20, 10));
        CHECK(std::abs(c.at("curves")[i].at("points")[0][0].get<double>() - 90) < 1e-9);
    }
    // The pieces still bound the regions: the line cuts the ellipse in two.
    CHECK(run(s, "sketch.profiles", {{"sketch", bid}}).at("profiles").size() == 2);

    // A line and a circle are cut exactly where an ellipse crosses them.
    const auto tid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "ellipse"}, {"center", {200, 0}}, {"major_radius", 20},
                                                   {"minor_radius", 10}},
                                                  {{"type", "line"}, {"start", {170, 0}}, {"end", {230, 0}}},
                                                  {{"type", "circle"}, {"center", {200, 10}}, {"radius", 4}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", tid}, {"curve", 1}, {"at", {228, 0}}});
    c = run(s, "sketch.curves", {{"sketch", tid}});
    INFO(c.dump());
    bool cutAt220 = false;
    for (const auto& k : c.at("curves"))
        if (k.at("kind") == "line")
            for (const auto& p : k.at("points"))
                cutAt220 = cutAt220 || near(p, 220, 0, 1e-9);
    CHECK(cutAt220);
    // The circle's part outside the ellipse goes; what is left ends on the ellipse.
    std::size_t circle = 0;
    for (std::size_t i = 0; i < c.at("curves").size(); ++i)
        if (c.at("curves")[i].at("kind") == "circle")
            circle = i;
    run(s, "sketch.trim", {{"sketch", tid}, {"curve", circle}, {"at", {200, 14}}});
    c = run(s, "sketch.curves", {{"sketch", tid}});
    INFO(c.dump());
    const Json rest = c.at("curves").back();
    REQUIRE(rest.at("kind") == "arc");
    CHECK(onEllipse(rest.at("points")[0], 200, 20, 10));
    CHECK(onEllipse(rest.at("points")[2], 200, 20, 10));

    // Mirror keeps an elliptical arc and adds no freedom.
    const auto mid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "line"}, {"start", {300, -50}}, {"end", {300, 50}}, {"construction", true}},
                                                  {{"type", "elliptical_arc"}, {"center", {330, 0}}, {"major_radius", 20},
                                                   {"minor_radius", 8}, {"angle", 0}, {"start_angle", 20}, {"end_angle", 110}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    c = run(s, "sketch.curves", {{"sketch", mid}});
    const double r20 = 20 * std::numbers::pi / 180, r110 = 110 * std::numbers::pi / 180;
    const double sx = 330 + 20 * std::cos(r20), sy = 8 * std::sin(r20), ex2 = 330 + 20 * std::cos(r110), ey = 8 * std::sin(r110);
    CHECK(near(c.at("curves")[1].at("points")[0], sx, sy));
    CHECK(near(c.at("curves")[1].at("points")[3], ex2, ey));
    const int dofBefore = c.at("degreesOfFreedom").get<int>();
    CHECK(dofBefore == 4 + 7);
    run(s, "sketch.mirror", {{"sketch", mid}, {"curves", {1}}, {"line", 0}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 3);
    CHECK(c.at("curves")[2].at("kind") == "elliptical_arc");
    // Mirrored about x = 300, from the mirrored end to the mirrored start.
    CHECK(near(c.at("curves")[2].at("points")[0], 600 - ex2, ey));
    CHECK(near(c.at("curves")[2].at("points")[3], 600 - sx, sy));
    CHECK(c.at("degreesOfFreedom").get<int>() == dofBefore);

    // Offset of an elliptical arc: a spline (an offset ellipse is not an ellipse).
    run(s, "sketch.offset", {{"sketch", mid}, {"curves", {1}}, {"distance", 2}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    CHECK(c.at("curves").back().at("kind") == "spline");

    // Scale the right half of an ellipse (ends at the minor axis tips): its axis scales too
    // (Codex review of #222: the ends and centre alone did not hold it).
    const auto half = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "elliptical_arc"}, {"center", {0, 0}}, {"major_radius", 20},
                                                    {"minor_radius", 10}, {"angle", 0}, {"start_angle", -90}, {"end_angle", 90}}})}})
                          .at("id")
                          .get<std::uint64_t>();
    run(s, "sketch.scale_curves", {{"sketch", half}, {"curves", {0}}, {"factor", 2}, {"point", {0, 0}}});
    c = run(s, "sketch.curves", {{"sketch", half}});
    INFO(c.dump());
    CHECK(c.at("curves")[0].at("majorRadius").get<double>() == Approx(40));
    CHECK(c.at("curves")[0].at("minorRadius").get<double>() == Approx(20));
    CHECK(near(c.at("curves")[0].at("points")[2], 40, 0, 1e-6));
    run(s, "sketch.move_curves", {{"sketch", half}, {"curves", {0}}, {"angle", 90}, {"pivot", {0, 0}}});
    c = run(s, "sketch.curves", {{"sketch", half}});
    INFO(c.dump());
    CHECK(near(c.at("curves")[0].at("points")[2], 0, 40, 1e-6));
    CHECK(c.at("curves")[0].at("majorRadius").get<double>() == Approx(40));

    // Two crossings between two samples along the ellipse (a tiny circle across it, Codex
    // review of #222): the trim cuts at them instead of deleting the whole ellipse.
    const auto tiny = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "ellipse"}, {"center", {0, 0}}, {"major_radius", 20}, {"minor_radius", 10}, {"angle", 0}},
                                                   {{"type", "circle"}, {"center", {19.9998096144, 0.04363309285}}, {"radius", 0.02}}})}})
                          .at("id")
                          .get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", tiny}, {"curve", 0}, {"at", {-20, 0}}});
    c = run(s, "sketch.curves", {{"sketch", tiny}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    CHECK(c.at("curves")[1].at("kind") == "elliptical_arc");

    // Fix on the axis point keeps it where it is; Fix on a whole elliptical arc or ellipse
    // holds all of it (Codex review of #222).
    const auto fixes = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "elliptical_arc"}, {"center", {0, 0}}, {"major_radius", 20},
                                                     {"minor_radius", 10}, {"angle", 30}, {"start_angle", -90}, {"end_angle", 90}},
                                                    {{"type", "ellipse"}, {"center", {100, 0}}, {"major_radius", 20}, {"minor_radius", 10}, {"angle", 0}},
                                                    {{"type", "elliptical_arc"}, {"center", {200, 0}}, {"major_radius", 20},
                                                     {"minor_radius", 10}, {"angle", 0}, {"start_angle", 20}, {"end_angle", 110}}})}})
                           .at("id")
                           .get<std::uint64_t>();
    const double ax = 20 * std::cos(std::numbers::pi / 6), ay = 20 * std::sin(std::numbers::pi / 6);
    run(s, "sketch.add_constraints", {{"sketch", fixes}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 2}, {"point", "axis"}}})}}})}});
    c = run(s, "sketch.curves", {{"sketch", fixes}});
    INFO(c.dump());
    CHECK(near(c.at("curves")[2].at("points")[2], 220, 0, 1e-6));
    run(s, "sketch.add_constraints", {{"sketch", fixes}, {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({0, 1, 2})}}})}});
    c = run(s, "sketch.curves", {{"sketch", fixes}});
    INFO(c.dump());
    CHECK(c.at("degreesOfFreedom").get<int>() == 0);
    CHECK(near(c.at("curves")[0].at("points")[2], ax, ay, 1e-6));
    const auto r = s.execute("sketch.add_constraints", {{"sketch", fixes}, {"constraints", Json::array({{{"type", "major_radius"}, {"curve", 0}, {"value", 30}}})}});
    if (r.ok) {
        c = run(s, "sketch.curves", {{"sketch", fixes}});
        CHECK(c.at("curves")[0].at("majorRadius").get<double>() == Approx(20));
    }
}

TEST_CASE("Design commands: Trim and Break a spline (CAD-260)", "[commands][cad]")
{
    commands::Session s;
    // How far a point is from a polyline.
    auto off = [](const Json& q, const Json& poly) {
        double best = 1e300;
        for (std::size_t i = 0; i + 1 < poly.size(); ++i) {
            const double ax = poly[i][0].get<double>(), ay = poly[i][1].get<double>();
            const double dx = poly[i + 1][0].get<double>() - ax, dy = poly[i + 1][1].get<double>() - ay;
            const double qx = q[0].get<double>(), qy = q[1].get<double>();
            const double l2 = dx * dx + dy * dy;
            const double f = l2 > 0 ? std::clamp(((qx - ax) * dx + (qy - ay) * dy) / l2, 0.0, 1.0) : 0.0;
            best = std::min(best, std::hypot(qx - ax - f * dx, qy - ay - f * dy));
        }
        return best;
    };
    auto count = [](const Json& c, const char* kind) {
        return std::count_if(c.at("curves").begin(), c.at("curves").end(), [&](const Json& k) { return k.at("kind") == kind; });
    };

    // An arch (open spline) over a line: trim both tails of the arch and both line ends
    // outside it, and the arch with the line is one profile.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "spline"}, {"points", {{0, 0}, {10, 8}, {20, 10}, {30, 8}, {40, 0}}}},
                                                 {{"type", "line"}, {"start", {-5, 5}}, {"end", {45, 5}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    const Json original = c.at("curves")[0].at("samples");
    REQUIRE(original.size() > 10);
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {1, 1}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    CHECK(c.at("curves")[1].at("kind") == "spline");
    {
        const Json& pts = c.at("curves")[1].at("points");
        CHECK(pts.front()[1].get<double>() == Approx(5).margin(1e-9));
        CHECK(pts.back()[0].get<double>() == Approx(40).margin(1e-9));
        // The piece follows the original arch.
        double worst = 0;
        for (const auto& q : c.at("curves")[1].at("samples"))
            worst = std::max(worst, off(q, original));
        CHECK(worst < 0.02);
    }
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 1}, {"at", {39, 1}}});
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {-3, 5}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    std::size_t line = 0;
    for (std::size_t i = 0; i < c.at("curves").size(); ++i)
        if (c.at("curves")[i].at("kind") == "line")
            line = i;
    run(s, "sketch.trim", {{"sketch", id}, {"curve", line}, {"at", {43, 5}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    CHECK(count(c, "spline") == 1);
    {
        const Json& arch = c.at("curves")[c.at("curves")[0].at("kind") == "spline" ? 0 : 1];
        double worst = 0;
        for (const auto& q : arch.at("samples"))
            worst = std::max(worst, off(q, original));
        CHECK(worst < 0.02);
    }
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);

    // Break: the arch splits into three splines where the line crosses it.
    const auto br = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "spline"}, {"points", {{0, 0}, {10, 8}, {20, 10}, {30, 8}, {40, 0}}}},
                                                 {{"type", "line"}, {"start", {-5, 5}}, {"end", {45, 5}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    const Json pv = run(s, "sketch.break_preview", {{"sketch", br}, {"curve", 0}, {"at", {20, 10}}});
    INFO(pv.dump());
    CHECK(pv.at("breakable") == true);
    CHECK(pv.at("kind") == "spline");
    CHECK(pv.at("samples").size() > 2);
    run(s, "sketch.break", {{"sketch", br}, {"curve", 0}, {"at", {20, 10}}});
    c = run(s, "sketch.curves", {{"sketch", br}});
    INFO(c.dump());
    CHECK(count(c, "spline") == 3);

    // A closed spline cut by a line: Trim the top, the bottom stays as an open spline.
    const auto cl = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "spline"}, {"points", {{10, 0}, {0, 10}, {-10, 0}, {0, -10}}}, {"closed", true}},
                                                 {{"type", "line"}, {"start", {-15, 2}}, {"end", {15, 2}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    c = run(s, "sketch.curves", {{"sketch", cl}});
    const Json loop = c.at("curves")[0].at("samples");
    run(s, "sketch.trim", {{"sketch", cl}, {"curve", 0}, {"at", {0, 10}}});
    c = run(s, "sketch.curves", {{"sketch", cl}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    const Json& rest = c.at("curves")[1];
    CHECK(rest.at("kind") == "spline");
    CHECK(rest.value("closed", false) == false);
    CHECK(rest.at("points").front()[1].get<double>() == Approx(2).margin(1e-9));
    CHECK(rest.at("points").back()[1].get<double>() == Approx(2).margin(1e-9));
    double worst = 0;
    for (const auto& q : rest.at("samples"))
        worst = std::max(worst, off(q, loop));
    CHECK(worst < 0.05); // the reference is the loop's drawn samples (16 a span), coarser than the trim's
    for (const double x : {-14.0, 14.0}) {
        c = run(s, "sketch.curves", {{"sketch", cl}});
        std::size_t ln = 0;
        for (std::size_t i = 0; i < c.at("curves").size(); ++i)
            if (c.at("curves")[i].at("kind") == "line")
                ln = i;
        run(s, "sketch.trim", {{"sketch", cl}, {"curve", ln}, {"at", {x, 2}}});
    }
    CHECK(run(s, "sketch.profiles", {{"sketch", cl}}).at("profiles").size() == 1);
    // One undo step per trim.
    run(s, "edit.undo");
    run(s, "edit.undo");
    run(s, "edit.undo");
    c = run(s, "sketch.curves", {{"sketch", cl}});
    CHECK(c.at("curves")[0].at("kind") == "spline");
    CHECK(c.at("curves")[0].value("closed", false) == true);
}

TEST_CASE("Design commands: a trimmed spline gets no needless fit points (CAD-260)", "[commands][cad]")
{
    // A straight spline with uneven fit points cut in its long span (Codex review of #224):
    // the piece is straight, so it needs no points beyond its ends and the fit point.
    commands::Session s;
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "spline"}, {"points", {{0, 0}, {1, 0}, {100, 0}}}},
                                                 {{"type", "line"}, {"start", {50, -5}}, {"end", {50, 5}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", id}, {"curve", 0}, {"at", {75, 0}}});
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    const Json& piece = c.at("curves")[1];
    CHECK(piece.at("kind") == "spline");
    CHECK(piece.at("points").size() <= 4);
    CHECK(piece.at("points").back()[0].get<double>() == Approx(50).margin(1e-9));
}

TEST_CASE("Design commands: Break preview of a long spline stays fast (CAD-260)", "[commands][cad]")
{
    // Two long wavy splines crossing many times (Codex review of #224: the preview compared
    // every segment of one with every segment of the other, and every sample of a piece with
    // the whole original). Hovering Break must answer at once.
    commands::Session s;
    Json a = Json::array(), b = Json::array();
    for (int i = 0; i < 300; ++i) {
        a.push_back({i, 5 * std::sin(i / 3.0)});
        b.push_back({i, 5 * std::cos(i / 3.0)});
    }
    const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "spline"}, {"points", a}}, {{"type", "spline"}, {"points", b}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    const auto t0 = std::chrono::steady_clock::now();
    const Json pv = run(s, "sketch.break_preview", {{"sketch", id}, {"curve", 0}, {"at", {150, 5 * std::sin(50.0)}}});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(pv.dump().substr(0, 400));
    CHECK(pv.at("breakable") == true);
    CHECK(pv.at("kind") == "spline");
    CHECK(seconds < 3.0);
}

TEST_CASE("Design commands: Sketch Conic Curve (CAD-262)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y, double tol = 1e-6) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < tol;
    };
    // A parabola (rho 0.5) from (0, 0) to (20, 0) towards the apex (10, 10), closed by a line.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}},
                                                 {{"type", "line"}, {"start", {20, 0}}, {"end", {0, 0}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    const Json k = c.at("curves")[0];
    CHECK(k.at("kind") == "conic");
    CHECK(k.at("rho").get<double>() == Approx(0.5));
    CHECK(near(k.at("points")[0], 0, 0));
    CHECK(near(k.at("points")[1], 10, 10));
    CHECK(near(k.at("points")[2], 20, 0));
    // Its shoulder (middle) is rho of the way from the chord's middle to the apex.
    const Json& smp = k.at("samples");
    REQUIRE(smp.size() % 2 == 1);
    CHECK(near(smp[smp.size() / 2], 10, 5, 1e-9));
    // Start, end and apex (4 of them shared with the line's ends): 2 more for the apex.
    CHECK(c.at("degreesOfFreedom") == 6);
    // One profile: a parabolic segment, 2/3 of its base times its height.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 1}});
    CHECK(ex.at("bodies").back().at("volume").get<double>() == Approx(2.0 / 3.0 * 20 * 5).epsilon(1e-6));
    run(s, "edit.undo");

    // A lower rho bulges less (an ellipse), a higher one more (a hyperbola).
    for (const double rho : {0.25, 0.8}) {
        const auto r = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}},
                                                     {"rho", rho}}})}})
                           .at("id")
                           .get<std::uint64_t>();
        const Json m = run(s, "sketch.curves", {{"sketch", r}}).at("curves")[0].at("samples");
        CHECK(near(m[m.size() / 2], 10, 10 * rho, 1e-9));
    }

    // Tangent to a line: the apex moves onto the line's direction.
    const auto tg = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}},
                                                 {{"type", "line"}, {"start", {-5, -8}}, {"end", {0, 0}}},
                                                 {{"type", "line"}, {"start", {20, 0}}, {"end", {25, -8}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.add_constraints",
        {{"sketch", tg},
         {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({1, 2})}},
                                      {{"type", "tangent"}, {"curves", Json::array({1, 0})}},
                                      {{"type", "tangent"}, {"curves", Json::array({0, 2})}}})}});
    c = run(s, "sketch.curves", {{"sketch", tg}});
    INFO(c.dump());
    {
        const Json& p = c.at("curves")[0].at("points");
        // Where the two lines' directions meet.
        CHECK(near(p[1], 10, 16));
        CHECK(near(p[0], 0, 0));
        CHECK(near(p[2], 20, 0));
    }
    // A line apart from the conic (Codex review of #226): tangent puts the conic's end on the
    // line too, not only its direction along it.
    const auto apart = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}},
                                                    {{"type", "line"}, {"start", {0, 5}}, {"end", {10, 15}}}})}})
                           .at("id")
                           .get<std::uint64_t>();
    run(s, "sketch.add_constraints",
        {{"sketch", apart},
         {"constraints", Json::array({{{"type", "fix"}, {"curve", 1}}, {{"type", "tangent"}, {"curves", Json::array({1, 0})}}})}});
    c = run(s, "sketch.curves", {{"sketch", apart}});
    INFO(c.dump());
    {
        const Json& p = c.at("curves")[0].at("points");
        // On y = x + 5, and the apex too (the line's direction from there).
        CHECK(p[0][1].get<double>() - p[0][0].get<double>() == Approx(5).margin(1e-6));
        CHECK(p[1][1].get<double>() - p[1][0].get<double>() == Approx(5).margin(1e-6));
    }

    // Tangent to an arc is refused (only lines for now).
    const auto ta = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "conic"}, {"start", {-10, -10}}, {"end", {10, -10}}, {"apex", {2, 5}}},
                                                 {{"type", "circle"}, {"center", {30, 0}}, {"radius", 10}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    CHECK_FALSE(s.execute("sketch.add_constraints",
                          {{"sketch", ta}, {"constraints", Json::array({{{"type", "tangent"}, {"curves", Json::array({0, 1})}}})}})
                    .ok);

    // The apex is a constraint point: put it on the origin.
    run(s, "sketch.add_constraints",
        {{"sketch", ta},
         {"constraints", Json::array({{{"type", "coincident"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "apex"}}, "origin"})}}})}});
    CHECK(near(run(s, "sketch.curves", {{"sketch", ta}}).at("curves")[0].at("points")[1], 0, 0));

    // Mirror, move and scale keep it a conic (rho stays).
    const auto mid = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "line"}, {"start", {-5, -50}}, {"end", {-5, 50}}, {"construction", true}},
                                                  {{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}},
                                                   {"rho", 0.3}}})}})
                         .at("id")
                         .get<std::uint64_t>();
    const int dofBefore = run(s, "sketch.curves", {{"sketch", mid}}).at("degreesOfFreedom").get<int>();
    run(s, "sketch.mirror", {{"sketch", mid}, {"curves", {1}}, {"line", 0}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 3);
    CHECK(c.at("curves")[2].at("kind") == "conic");
    CHECK(c.at("curves")[2].at("rho").get<double>() == Approx(0.3));
    CHECK(near(c.at("curves")[2].at("points")[0], -10, 0));
    CHECK(near(c.at("curves")[2].at("points")[1], -20, 10));
    CHECK(near(c.at("curves")[2].at("points")[2], -30, 0));
    // The copy follows the original (symmetric points): no extra freedom.
    CHECK(c.at("degreesOfFreedom").get<int>() == dofBefore);

    run(s, "sketch.move_curves", {{"sketch", mid}, {"curves", {1}}, {"angle", 90}, {"pivot", {0, 0}}});
    c = run(s, "sketch.curves", {{"sketch", mid}});
    CHECK(c.at("curves")[1].at("kind") == "conic");
    CHECK(near(c.at("curves")[1].at("points")[1], -10, 10));
    CHECK(near(c.at("curves")[1].at("points")[2], 0, 20));
    CHECK(c.at("curves")[1].at("rho").get<double>() == Approx(0.3));

    const auto sc = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}},
                                                  {"rho", 0.4}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.scale_curves", {{"sketch", sc}, {"curves", {0}}, {"factor", 2}, {"point", {0, 0}}});
    c = run(s, "sketch.curves", {{"sketch", sc}});
    CHECK(near(c.at("curves")[0].at("points")[1], 20, 20));
    CHECK(near(c.at("curves")[0].at("points")[2], 40, 0));
    CHECK(c.at("curves")[0].at("rho").get<double>() == Approx(0.4));

    run(s, "sketch.circular_pattern", {{"sketch", sc}, {"curves", {0}}, {"center", {0, -30}}, {"count", 3}});
    c = run(s, "sketch.curves", {{"sketch", sc}});
    REQUIRE(c.at("curves").size() == 3);
    for (const auto& pc : c.at("curves")) {
        CHECK(pc.at("kind") == "conic");
        CHECK(pc.at("rho").get<double>() == Approx(0.4));
    }
    run(s, "edit.undo");

    // Turned without a pivot: about the middle of the curve itself (not of its apex).
    {
        const auto turn = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 30}},
                                                        {"rho", 0.3}}})}})
                              .at("id")
                              .get<std::uint64_t>();
        auto middle = [&](const Json& samples) {
            double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
            for (const auto& q : samples)
                for (int d = 0; d < 2; ++d) {
                    lo[d] = std::min(lo[d], q[d].get<double>());
                    hi[d] = std::max(hi[d], q[d].get<double>());
                }
            return std::array<double, 2>{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2};
        };
        const auto m0 = middle(run(s, "sketch.curves", {{"sketch", turn}}).at("curves")[0].at("samples"));
        run(s, "sketch.move_curves", {{"sketch", turn}, {"curves", {0}}, {"angle", 90}});
        const auto m1 = middle(run(s, "sketch.curves", {{"sketch", turn}}).at("curves")[0].at("samples"));
        CHECK(m1[0] == Approx(m0[0]).margin(0.05));
        CHECK(m1[1] == Approx(m0[1]).margin(0.05));
    }

    // Fixed, it holds its ends and apex.
    run(s, "sketch.add_constraints", {{"sketch", sc}, {"constraints", Json::array({{{"type", "fix"}, {"curve", 0}}})}});
    CHECK(run(s, "sketch.curves", {{"sketch", sc}}).at("degreesOfFreedom") == 0);

    // Other curves trim at it; Trim, Break and Extend of the conic itself are refused.
    const auto tr = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}},
                                                 {{"type", "line"}, {"start", {10, -5}}, {"end", {10, 10}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", tr}, {"curve", 1}, {"at", {10, 8}}});
    c = run(s, "sketch.curves", {{"sketch", tr}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 2);
    CHECK(near(c.at("curves")[1].at("points")[1], 10, 5, 1e-3));
    // A line below it extends up to it.
    run(s, "sketch.add_curves", {{"sketch", tr}, {"curves", Json::array({{{"type", "line"}, {"start", {5, -5}}, {"end", {5, 1}}}})}});
    run(s, "sketch.extend", {{"sketch", tr}, {"curve", 2}, {"at", {5, 0.9}}});
    c = run(s, "sketch.curves", {{"sketch", tr}});
    INFO(c.dump());
    CHECK(near(c.at("curves")[2].at("points")[1], 5, 3.75, 1e-3)); // y = x (20 - x) / 20
    run(s, "sketch.delete_curves", {{"sketch", tr}, {"curves", {2}}});
    CHECK_FALSE(s.execute("sketch.trim", {{"sketch", tr}, {"curve", 0}, {"at", {3, 3}}}).ok);
    CHECK_FALSE(s.execute("sketch.break", {{"sketch", tr}, {"curve", 0}, {"at", {3, 3}}}).ok);
    CHECK_FALSE(s.execute("sketch.extend", {{"sketch", tr}, {"curve", 0}, {"at", {1, 0.5}}}).ok);
    // Offset gives a spline beside it; Delete removes it.
    run(s, "sketch.offset", {{"sketch", tr}, {"curves", {0}}, {"distance", 1}});
    c = run(s, "sketch.curves", {{"sketch", tr}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 3);
    CHECK(c.at("curves")[2].at("kind") == "spline");
    run(s, "sketch.delete_curves", {{"sketch", tr}, {"curves", {0, 2}}});
    CHECK(run(s, "sketch.curves", {{"sketch", tr}}).at("curves").size() == 1);

    // Refused: rho out of range, the apex on the chord, start = end.
    for (const Json& bad : {Json{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}, {"rho", 0}},
                            Json{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 10}}, {"rho", 1}},
                            Json{{"type", "conic"}, {"start", {0, 0}}, {"end", {20, 0}}, {"apex", {10, 0}}},
                            Json{{"type", "conic"}, {"start", {0, 0}}, {"end", {0, 0}}, {"apex", {10, 10}}}})
        CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"}, {"curves", Json::array({bad})}}).ok);
}

TEST_CASE("Design commands: sketch offset (CAD-126)", "[commands][cad]")
{
    commands::Session s;
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {100, 0}}, {"radius", 5}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    // The rectangle 2 mm outwards: 4 sides and 4 corner arcs; the ring between them is
    // its own profile.
    run(s, "sketch.offset", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"distance", 2}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves").size() == 5 + 8);
    double minX = 1e9;
    for (std::size_t i = 5; i < c.at("curves").size(); ++i)
        for (const auto& p : c.at("curves")[i].at("points"))
            minX = std::min(minX, p[0].get<double>());
    CHECK(minX == Approx(-2).margin(1e-6));
    // A circle 1 mm inwards.
    run(s, "sketch.offset", {{"sketch", id}, {"curves", {4}}, {"distance", -1}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves").back().at("kind") == "circle");
    CHECK(c.at("curves").back().at("radius").get<double>() == Approx(4).margin(1e-6));
    // Too far inwards collapses: refused.
    CHECK_FALSE(s.execute("sketch.offset", {{"sketch", id}, {"curves", {4}}, {"distance", -6}}).ok);

    // A closed spline gives a closed (periodic) spline: smooth at the seam, its end point
    // not repeated.
    Json oval = Json::array();
    for (int i = 0; i < 8; ++i) {
        const double a = 2 * std::numbers::pi * i / 8;
        oval.push_back({200 + 20 * std::cos(a), 10 * std::sin(a)});
    }
    const auto sp = run(s, "sketch.create",
                        {{"plane", "XY"}, {"curves", Json::array({{{"type", "spline"}, {"points", oval}, {"closed", true}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    run(s, "sketch.offset", {{"sketch", sp}, {"curves", {0}}, {"distance", 2}});
    c = run(s, "sketch.curves", {{"sketch", sp}});
    REQUIRE(c.at("curves").size() == 2);
    const auto& outer = c.at("curves")[1];
    CHECK(outer.at("kind") == "spline");
    CHECK(outer.at("closed") == true);
    const auto& pts = outer.at("points");
    CHECK(std::hypot(pts.front()[0].get<double>() - pts.back()[0].get<double>(),
                     pts.front()[1].get<double>() - pts.back()[1].get<double>()) > 1.0);
}

TEST_CASE("Design commands: project body edges into a sketch (CAD-126)", "[commands][cad]")
{
    commands::Session s;
    // A cylinder r 10 standing on XY at (30, 0), and a box; a sketch on a plane above.
    run(s, "feature.cylinder", {{"center", {30, 0, 0}}, {"radius", 10}, {"height", 15}});
    const auto box = run(s, "feature.box", {{"corner", {-20, -5, 0}}, {"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"offset", 40}}).at("id").get<std::uint64_t>();
    // The box's top edge along x at y = -5, z = 10 and a vertical edge (seen end-on: left out).
    run(s, "sketch.project", {{"sketch", id}, {"body", box}, {"edges", Json::array({{{"point", {-15, -5, 10}}}, {{"point", {-20, -5, 5}}}})}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 1);
    CHECK(c.at("curves")[0].at("kind") == "line");
    CHECK(c.at("curves")[0].at("points")[0][1].get<double>() == Approx(-5).margin(1e-6));
    // The cylinder's top circle stays a circle.
    run(s, "sketch.project", {{"sketch", id}, {"body", "Body1"}, {"edges", Json::array({{{"point", {30, 10, 15}}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 2);
    CHECK(c.at("curves")[1].at("kind") == "circle");
    CHECK(c.at("curves")[1].at("radius").get<double>() == Approx(10).margin(1e-6));
    CHECK(c.at("curves")[1].at("points")[0][0].get<double>() == Approx(30).margin(1e-6));
    CHECK(c.at("degreesOfFreedom") == 0); // fixed
    // The projected circle bounds a profile to extrude.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);

    // Seen edge-on from a side plane, the rim is its diameter: a line, not a spline that
    // runs there and back.
    const auto side = run(s, "sketch.create", {{"plane", "XZ"}, {"offset", 30}}).at("id").get<std::uint64_t>();
    run(s, "sketch.project", {{"sketch", side}, {"body", "Body1"}, {"edges", Json::array({{{"point", {30, 10, 15}}}})}});
    c = run(s, "sketch.curves", {{"sketch", side}});
    REQUIRE(c.at("curves").size() == 1);
    CHECK(c.at("curves")[0].at("kind") == "line");
    const auto& ends = c.at("curves")[0].at("points");
    CHECK(std::hypot(ends[0][0].get<double>() - ends[1][0].get<double>(), ends[0][1].get<double>() - ends[1][1].get<double>()) ==
          Approx(20).margin(1e-6));
    CHECK(c.at("degreesOfFreedom") == 0);
}

TEST_CASE("Design commands: body.faces gives the nearest face and its normal at a point", "[commands][cad]")
{
    // Codex on PR #125: on a cylinder, a pick on the side opposite the face's parameter
    // middle is nearer the caps' planes than the side's tangent plane there.
    commands::Session s;
    run(s, "feature.cylinder", {{"center", {0, 0, 0}}, {"radius", 10}, {"height", 20}});
    const double k = std::sqrt(0.5);
    for (const auto& [px, py] : {std::pair{10.0, 0.0}, std::pair{-10.0, 0.0}, std::pair{0.0, -10.0},
                                 std::pair{10 * k, 10 * k}}) {
        const Json r = run(s, "body.faces", {{"body", "Body1"}, {"point", {px, py, 10}}});
        const auto& faces = r.at("faces");
        const auto idx = r.at("nearest").at("index").get<std::size_t>();
        CHECK(faces.at(idx).at("type").get<std::string>() == "cylinder");
        const auto& n = r.at("nearest").at("normal");
        CHECK(n[0].get<double>() == Approx(px / 10).margin(1e-6));
        CHECK(n[1].get<double>() == Approx(py / 10).margin(1e-6));
        CHECK(n[2].get<double>() == Approx(0).margin(1e-6));
    }
    const Json top = run(s, "body.faces", {{"body", "Body1"}, {"point", {3, 2, 20}}});
    CHECK(top.at("nearest").at("normal")[2].get<double>() == Approx(1).margin(1e-9));
    CHECK_FALSE(run(s, "body.faces", {{"body", "Body1"}}).contains("nearest"));
}

TEST_CASE("Design commands: body.edges / body.faces before a feature see the rolled-back body", "[commands][cad]")
{
    // The size arrow of an edited fillet is placed on the body as it was before the fillet.
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 30, 20}}});
    const Json fillet =
        run(s, "feature.fillet", {{"body", "Body1"}, {"edges", Json::array({{{"point", {28, 0, 20}}}})}, {"radius", 6}});
    const auto filletId = fillet.at("id").get<std::uint64_t>();
    CHECK(run(s, "body.edges", {{"body", "Body1"}}).at("edges").size() > 12);
    const Json before = run(s, "body.edges", {{"body", "Body1"}, {"before", filletId}, {"point", {28, 0, 20}}});
    CHECK(before.at("edges").size() == 12);
    const auto& e = before.at("edges").at(before.at("nearest").at("index").get<std::size_t>());
    CHECK(e.at("middle")[0].get<double>() == Approx(20));
    CHECK(e.at("middle")[1].get<double>() == Approx(0).margin(1e-9));
    CHECK(e.at("middle")[2].get<double>() == Approx(20));
    CHECK(run(s, "body.faces", {{"body", "Body1"}, {"before", filletId}}).at("faces").size() == 6);
    // The design itself is untouched: still filleted, marker at the end, nothing to undo added.
    const double pi = std::numbers::pi;
    CHECK(s.design().bodies().begin()->second.shape.volume() == Approx(24000 - 40 * 36 * (1 - pi / 4)).epsilon(1e-6));
    CHECK(s.design().marker() == s.design().features().size());
}

TEST_CASE("Design commands: timeline.move_feature reorders the timeline, refuses broken orders", "[commands][cad]")
{
    // CAD-160 (Fusion: drag a feature in the timeline).
    commands::Session s;
    const auto box = run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 30, 20}}}).at("id").get<std::uint64_t>();
    const auto fillet = run(s, "feature.fillet",
                            {{"body", box}, {"edges", Json::array({{{"point", {28, 0, 20}}}})}, {"radius", 4}})
                            .at("id")
                            .get<std::uint64_t>();
    const auto other = run(s, "feature.box", {{"corner", {100, 0, 0}}, {"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    const auto order = [&] {
        std::vector<std::uint64_t> ids;
        const Json tl = run(s, "timeline.list");
        for (const auto& f : tl.at("features"))
            ids.push_back(f.at("id").get<std::uint64_t>());
        return ids;
    };
    REQUIRE(order() == std::vector<std::uint64_t>{box, fillet, other});

    // An independent body can go first; everything still works.
    const Json moved = run(s, "timeline.move_feature", {{"id", other}, {"position", 0}});
    CHECK(order() == std::vector<std::uint64_t>{other, box, fillet});
    for (const auto& f : moved.at("features"))
        CHECK(f.at("status").get<std::string>() == "ok");
    CHECK(s.design().bodies().size() == 2);

    // The fillet cannot go before the box it rounds: refused, nothing changes.
    const commands::CommandResult bad = s.execute("timeline.move_feature", {{"id", fillet}, {"position", 0}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("Cannot move it there") != std::string::npos);
    CHECK(order() == std::vector<std::uint64_t>{other, box, fillet});
    const double pi = std::numbers::pi;
    CHECK(s.design().bodies().at(box).shape.volume() == Approx(24000 - 40 * 16 * (1 - pi / 4)).epsilon(1e-6));

    // Across the marker: rolled back features stay rolled back, the moved one follows its side.
    run(s, "timeline.set_marker", {{"position", 2}}); // other, box active; fillet rolled back
    run(s, "timeline.move_feature", {{"id", other}, {"position", 2}});
    CHECK(order() == std::vector<std::uint64_t>{box, fillet, other});
    const Json tl = run(s, "timeline.list");
    CHECK(tl.at("marker").get<std::size_t>() == 1);
    CHECK(tl.at("features")[1].at("status").get<std::string>() == "rolled_back");
    CHECK(tl.at("features")[2].at("status").get<std::string>() == "rolled_back");

    // One undo step each.
    run(s, "edit.undo");
    CHECK(order() == std::vector<std::uint64_t>{other, box, fillet});
    CHECK(run(s, "timeline.list").at("marker").get<std::size_t>() == 2);
    run(s, "edit.undo"); // marker
    run(s, "edit.undo"); // first move
    CHECK(order() == std::vector<std::uint64_t>{box, fillet, other});
}

TEST_CASE("Design commands: parameter.evaluate evaluates without recording (CAD-199)", "[commands][cad]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "w"}, {"expression", "30 mm"}});
    CHECK(run(s, "parameter.evaluate", {{"expression", "w / 2"}}).at("value").get<double>() == Approx(15.0));
    CHECK(run(s, "parameter.evaluate", {{"expression", "1 cm"}}).at("value").get<double>() == Approx(10.0));
    CHECK(run(s, "parameter.evaluate", {{"expression", 7}}).at("value").get<double>() == Approx(7.0));
    CHECK_FALSE(s.execute("parameter.evaluate", {{"expression", "nope * 2"}}).ok);
    // Nothing was recorded: one undo removes the parameter, then there is nothing left.
    REQUIRE(s.execute("edit.undo").ok);
    CHECK_FALSE(s.execute("edit.undo").ok);
}

TEST_CASE("Design commands: three-point rectangle at an angle (CAD-203)", "[commands][cad][solver]")
{
    commands::Session s;
    run(s, "parameter.set", {{"name", "h"}, {"expression", "10 mm"}});
    // One side from (0, 0) to (30, 40) (50 long), the other 10 to its left; and one with a
    // negative height, to the right of its first side.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "three_point_rectangle"}, {"start", {0, 0}}, {"end", {30, 40}},
                                                  {"height", "h"}},
                                                 {{"type", "three_point_rectangle"}, {"start", {100, 0}},
                                                  {"end", {120, 0}}, {"height", -5}}})}});
    CHECK(sk.at("profiles") == 2);
    const auto id = sk.at("id").get<std::uint64_t>();
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 8);
    const auto& far = c.at("curves")[1].at("points")[1];
    CHECK(far[0].get<double>() == Approx(22).margin(1e-6));
    CHECK(far[1].get<double>() == Approx(46).margin(1e-6));
    const auto& below = c.at("curves")[5].at("points")[1];
    CHECK(below[0].get<double>() == Approx(120).margin(1e-6));
    CHECK(below[1].get<double>() == Approx(-5).margin(1e-6));

    const Json ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 5}});
    double volume = 0;
    for (const auto& b : ex.at("bodies"))
        volume += b.at("volume").get<double>();
    CHECK(volume == Approx((50 * 10 + 20 * 5) * 5.0));

    // Dragging the far corner keeps a rectangle: the sides stay perpendicular and parallel.
    run(s, "edit.undo");
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 1}, {"point", "end"}}}, {"to", {10, 60}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    auto dir = [&](int i) {
        const auto& p = c.at("curves")[i].at("points");
        return std::array<double, 2>{p[1][0].get<double>() - p[0][0].get<double>(),
                                     p[1][1].get<double>() - p[0][1].get<double>()};
    };
    const auto d0 = dir(0), d1 = dir(1), d2 = dir(2), d3 = dir(3);
    CHECK(d0[0] * d1[0] + d0[1] * d1[1] == Approx(0).margin(1e-6));
    CHECK(d0[0] * d2[1] - d0[1] * d2[0] == Approx(0).margin(1e-6));
    CHECK(d1[0] * d3[1] - d1[1] * d3[0] == Approx(0).margin(1e-6));
    // The stored item follows the drag (its height is no longer the expression).
    const Json tl = run(s, "timeline.list");
    const Json item = tl.at("features")[0].at("params").at("curves")[0];
    CHECK(item.at("type") == "three_point_rectangle");
    CHECK(item.at("height").is_number());

    // One side made construction splits the item into lines; the drag still keeps a
    // rectangle (the perpendicular constraint is carried over, not dropped).
    run(s, "sketch.set_construction", {{"sketch", id}, {"curves", {3}}});
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 1}, {"point", "end"}}}, {"to", {0, 70}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[3].at("construction") == true);
    const auto e0 = dir(0), e1 = dir(1), e2 = dir(2), e3 = dir(3);
    CHECK(e0[0] * e1[0] + e0[1] * e1[1] == Approx(0).margin(1e-6));
    CHECK(e0[0] * e2[1] - e0[1] * e2[0] == Approx(0).margin(1e-6));
    CHECK(e1[0] * e3[1] - e1[1] * e3[0] == Approx(0).margin(1e-6));

    // A zero height is refused.
    CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"},
                                            {"curves", Json::array({{{"type", "three_point_rectangle"},
                                                                     {"start", {0, 0}}, {"end", {10, 0}},
                                                                     {"height", 0}}})}})
                    .ok);
}

TEST_CASE("Design commands: symmetric on two lines or two circles (CAD-206)", "[commands][cad][solver]")
{
    commands::Session s;
    // Axis x = 0 (curve 0, fixed); line 2 runs the other way, so its end pairs with the start of
    // line 1; circles 3 and 4.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, -50}}, {"end", {0, 50}}, {"construction", true}},
                                                 {{"type", "line"}, {"start", {10, 0}}, {"end", {20, 10}}},
                                                 {{"type", "line"}, {"start", {-22, 13}}, {"end", {-11, 1}}},
                                                 {{"type", "circle"}, {"center", {20, -20}}, {"radius", 4}},
                                                 {{"type", "circle"}, {"center", {-18, -22}}, {"radius", 6}}})},
                         {"constraints", Json::array({{{"type", "fix"}, {"curves", {0}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "symmetric"}, {"curves", {1, 2}}, {"curve", 0}},
                                      {{"type", "symmetric"}, {"curves", {3, 4}}, {"curve", 0}}})}});
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    const auto pt = [&](int i, int k) { return c.at("curves")[i].at("points")[k]; };
    const auto mirrored = [](const Json& p, const Json& q) {
        CHECK(p[0].get<double>() == Approx(-q[0].get<double>()).margin(1e-6));
        CHECK(p[1].get<double>() == Approx(q[1].get<double>()).margin(1e-6));
    };
    mirrored(pt(1, 0), pt(2, 1)); // start of 1 with the end of 2 (the nearer end), not flipped
    mirrored(pt(1, 1), pt(2, 0));
    mirrored(pt(3, 0), pt(4, 0));
    CHECK(c.at("curves")[3].at("radius").get<double>() == Approx(c.at("curves")[4].at("radius").get<double>()));
    // Stored as point symmetries (+ equal radii): fix, 2 for the lines, 2 for the circles.
    const auto& list = c.at("constraints");
    REQUIRE(list.size() == 5);
    CHECK(list[1].at("points")[1] == Json({{"curve", 2}, {"point", "end"}}));
    CHECK(list[4].at("type") == "equal");
    // One undo step removes all of them.
    run(s, "edit.undo");
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("constraints").size() == 1);

    // Refused: a line with a circle, a circle as the axis, an item as its own axis.
    for (const Json& bad : {Json{{"type", "symmetric"}, {"curves", {1, 3}}, {"curve", 0}},
                            Json{{"type", "symmetric"}, {"curves", {1, 2}}, {"curve", 3}},
                            Json{{"type", "symmetric"}, {"curves", {1, 2}}, {"curve", 1}}}) {
        const auto r = s.execute("sketch.add_constraints", {{"sketch", id}, {"constraints", Json::array({bad})}});
        CHECK_FALSE(r.ok);
    }
}

TEST_CASE("Design commands: symmetric on two arcs mirrors the whole arc (CAD-206)", "[commands][cad][solver]")
{
    commands::Session s;
    // Axis x = 0 (fixed). Arcs 1 and 2 both counter-clockwise (the start of 1 pairs with the end
    // of 2); arcs 3 (counter-clockwise) and 4 (clockwise) pair start to start. Circle 5.
    const auto arc = [](Json st, Json th, Json en) { return Json{{"type", "arc"}, {"start", st}, {"through", th}, {"end", en}}; };
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, -80}}, {"end", {0, 80}}, {"construction", true}},
                                                 arc({30, -30}, {40, -25}, {30, -20}),
                                                 arc({-31, -21}, {-41, -26}, {-30, -31}),
                                                 arc({50, 30}, {60, 35}, {50, 40}),
                                                 arc({-50, 29}, {-61, 34}, {-51, 39}),
                                                 {{"type", "circle"}, {"center", {20, 60}}, {"radius", 4}}})},
                         {"constraints", Json::array({{{"type", "fix"}, {"curves", {0}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "symmetric"}, {"curves", {1, 2}}, {"curve", 0}},
                                      {{"type", "symmetric"}, {"curves", {3, 4}}, {"curve", 0}}})}});
    // The whole arc (start, middle, end) is the mirror image, also after a dimension change.
    const auto check = [&]() {
        const Json c = run(s, "sketch.curves", {{"sketch", id}}).at("curves");
        const auto mirrored = [](const Json& p, const Json& q) {
            CHECK(p[0].get<double>() == Approx(-q[0].get<double>()).margin(1e-6));
            CHECK(p[1].get<double>() == Approx(q[1].get<double>()).margin(1e-6));
        };
        for (int k = 0; k < 3; ++k) {
            mirrored(c[1].at("points")[k], c[2].at("points")[2 - k]); // same direction: reversed
            mirrored(c[3].at("points")[k], c[4].at("points")[k]);     // opposite direction
        }
        const auto& p = c[1].at("points");
        CHECK(std::hypot(p[0][0].get<double>() - p[1][0].get<double>(), p[0][1].get<double>() - p[1][1].get<double>()) > 1);
    };
    check();
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "radius"}, {"curve", 1}, {"value", 9}}, {{"type", "radius"}, {"curve", 3}, {"value", 7}}})}});
    check();
    // A circle and an arc cannot be mirror images.
    const auto r = s.execute("sketch.add_constraints",
                             {{"sketch", id}, {"constraints", Json::array({{{"type", "symmetric"}, {"curves", {5, 1}}, {"curve", 0}}})}});
    CHECK_FALSE(r.ok);
}

TEST_CASE("Design commands: tangent arc from the end of a line or arc (CAD-212)", "[commands][cad][solver]")
{
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    // From the end of the line (heading +x) to (20, 10): a quarter circle about (10, 10); then on
    // from that arc's end (heading +y) to (10, 30): radius 25 about (-5, 10). Both in one call.
    run(s, "sketch.add_curves",
        {{"sketch", id},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {20, 10}}},
                                 {{"type", "tangent_arc"}, {"from", {{"curve", 1}, {"point", "end"}}}, {"end", {10, 30}}}})}});
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    REQUIRE(c.at("curves").size() == 3);
    const auto at = [](const Json& p, double x, double y) {
        CHECK(p[0].get<double>() == Approx(x).margin(1e-6));
        CHECK(p[1].get<double>() == Approx(y).margin(1e-6));
    };
    at(c.at("curves")[1].at("points")[0], 10, 0);
    at(c.at("curves")[1].at("points")[1], 10 + 10 * std::sqrt(0.5), 10 - 10 * std::sqrt(0.5));
    at(c.at("curves")[1].at("points")[2], 20, 10);
    at(c.at("curves")[2].at("points")[0], 20, 10);
    at(c.at("curves")[2].at("points")[2], 10, 30);
    const auto centre = [](const Json& a) {
        const double ax = a[0][0].get<double>(), ay = a[0][1].get<double>(), bx = a[1][0].get<double>(),
                     by = a[1][1].get<double>(), ex = a[2][0].get<double>(), ey = a[2][1].get<double>();
        const double den = 2 * (ax * (by - ey) + bx * (ey - ay) + ex * (ay - by));
        return Json::array({((ax * ax + ay * ay) * (by - ey) + (bx * bx + by * by) * (ey - ay) + (ex * ex + ey * ey) * (ay - by)) / den,
                            ((ax * ax + ay * ay) * (ex - bx) + (bx * bx + by * by) * (ax - ex) + (ex * ex + ey * ey) * (bx - ax)) / den});
    };
    at(centre(c.at("curves")[2].at("points")), -5, 10);
    const auto& cons = c.at("constraints");
    REQUIRE(cons.size() == 4); // tangent + coincident per arc
    CHECK(cons[0].at("type") == "tangent");
    CHECK(cons[1].at("type") == "coincident");
    CHECK(cons[2].at("curves") == Json::array({1, 2}));
    // One undo step removes both arcs.
    run(s, "edit.undo");
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == 1);
    run(s, "edit.redo");

    // The arc stays tangent when the line changes: lengthen it to 20 and the arc's centre
    // stays straight above the line's end.
    run(s, "sketch.add_constraints",
        {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                      {{"type", "horizontal"}, {"curve", 0}},
                                                      {{"type", "length"}, {"curve", 0}, {"value", 20}}})}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& l = c.at("curves")[0].at("points");
    const auto& a = c.at("curves")[1].at("points");
    at(l[1], 20, 0);
    at(a[0], 20, 0);
    // The arc's centre stays straight above the line's end (tangent), and the second arc's
    // centre stays on the line through the first arc's centre and their shared end.
    CHECK(centre(a)[0].get<double>() == Approx(20).margin(1e-6));
    const Json c1 = centre(a), c2 = centre(c.at("curves")[2].at("points")), j = c.at("curves")[2].at("points")[0];
    const double cross = (c1[0].get<double>() - j[0].get<double>()) * (c2[1].get<double>() - j[1].get<double>()) -
                         (c1[1].get<double>() - j[1].get<double>()) * (c2[0].get<double>() - j[0].get<double>());
    const double l1 = std::hypot(c1[0].get<double>() - j[0].get<double>(), c1[1].get<double>() - j[1].get<double>());
    const double l2 = std::hypot(c2[0].get<double>() - j[0].get<double>(), c2[1].get<double>() - j[1].get<double>());
    CHECK(cross / (l1 * l2) == Approx(0).margin(1e-4)); // sine of the angle (curve points are rounded; the join is a constraint)

    // Refused: an end on the tangent line, a circle as the source.
    CHECK_FALSE(s.execute("sketch.add_curves", {{"sketch", id},
                                                {"curves", Json::array({{{"type", "tangent_arc"},
                                                                         {"from", {{"curve", 0}, {"point", "end"}}},
                                                                         {"end", {40, 0}}}})}}).ok);
    run(s, "sketch.add_curves", {{"sketch", id}, {"curves", Json::array({{{"type", "circle"}, {"center", {50, 50}}, {"radius", 3}}})}});
    CHECK_FALSE(s.execute("sketch.add_curves", {{"sketch", id},
                                                {"curves", Json::array({{{"type", "tangent_arc"},
                                                                         {"from", {{"curve", 3}, {"point", "end"}}},
                                                                         {"end", {60, 60}}}})}}).ok);

    // A batch that fails part-way changes nothing: the line before the bad arc is not kept,
    // and there is no undo step for it.
    const auto before = run(s, "sketch.curves", {{"sketch", id}}).at("curves").size();
    const auto undoBefore = run(s, "edit.history").dump();
    CHECK_FALSE(s.execute("sketch.add_curves", {{"sketch", id},
                                                {"curves", Json::array({{{"type", "line"}, {"start", {70, 70}}, {"end", {80, 70}}},
                                                                        {{"type", "tangent_arc"},
                                                                         {"from", {{"curve", 0}, {"point", "end"}}},
                                                                         {"end", {40, 0}}}})}}).ok);
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves").size() == before);
    CHECK(run(s, "edit.history").dump() == undoBefore);
}

TEST_CASE("Design commands: a tangent arc stays on a dimensioned line's end (CAD-212)", "[commands][cad][solver]")
{
    // Codex: drawn to (10, 0), dimensioned to 20; the arc starts at the solved end (20, 0),
    // which is not where the line was stored, so it must be joined explicitly.
    commands::Session s;
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                         {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                      {{"type", "horizontal"}, {"curve", 0}},
                                                      {{"type", "length"}, {"curve", 0}, {"value", 20}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", id},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {30, 10}}}})}});
    run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", 2}, {"value", 25}});
    const Json c = run(s, "sketch.curves", {{"sketch", id}});
    const auto& l = c.at("curves")[0].at("points");
    const auto& a = c.at("curves")[1].at("points");
    CHECK(l[1][0].get<double>() == Approx(25).margin(1e-6));
    CHECK(a[0][0].get<double>() == Approx(25).margin(1e-6));
    CHECK(a[0][1].get<double>() == Approx(0).margin(1e-6));

    // from.point defaults to "end" for both the geometry and the join.
    const Json e = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"curves", Json::array({0})}}})}});
    const auto eid = e.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", eid}, {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}}}, {"end", {20, 10}}}})}});
    const Json ec = run(s, "sketch.curves", {{"sketch", eid}});
    const auto& ea = ec.at("curves")[1].at("points");
    CHECK(ea[0][0].get<double>() == Approx(10).margin(1e-6));
    CHECK(ea[0][1].get<double>() == Approx(0).margin(1e-6));


    // Codex: a line drawn at y = 0 and dimensioned to y = 20 lies, as stored, on the other side of
    // the new arc's centre (10, 10); the arc must still keep its end and turn downwards.
    const Json h = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}})},
                                                      {"at", Json::array({Json::array({0, 20})})}},
                                                     {{"type", "horizontal"}, {"curve", 0}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", 10}}})}});
    const auto hid = h.at("id").get<std::uint64_t>();
    const Json hl = run(s, "sketch.curves", {{"sketch", hid}});
    REQUIRE(hl.at("curves")[0].at("points")[1][1].get<double>() == Approx(20).margin(1e-6));
    run(s, "sketch.add_curves",
        {{"sketch", hid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {20, 10}}}})}});
    const Json hc = run(s, "sketch.curves", {{"sketch", hid}});
    const auto& ha = hc.at("curves")[1].at("points");
    CHECK(ha[0][0].get<double>() == Approx(10).margin(1e-6));
    CHECK(ha[0][1].get<double>() == Approx(20).margin(1e-6));
    CHECK(ha[2][0].get<double>() == Approx(20).margin(1e-6));
    CHECK(ha[2][1].get<double>() == Approx(10).margin(1e-6));


    // Codex: a quarter arc drawn with radius 10 and dimensioned to 20, continued inwards to
    // (-15, 5): the new arc (radius 15) lies inside the source, which is now the larger one.
    const Json q = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "arc"}, {"start", {10, 0}}, {"through", {7.0710678118654755, 7.0710678118654755}}, {"end", {0, 10}}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "center"}}})}},
                                                     {{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}, {"at", Json::array({Json::array({20, 0})})}},
                                                     {{"type", "radius"}, {"curve", 0}, {"value", 20}}})}});
    const auto qid = q.at("id").get<std::uint64_t>();
    const Json qs = run(s, "sketch.curves", {{"sketch", qid}});
    const auto& qe = qs.at("curves")[0].at("points")[2];
    INFO(qs.dump());
    REQUIRE(std::hypot(qe[0].get<double>(), qe[1].get<double>()) == Approx(20).margin(1e-6));
    const double tx = qe[0].get<double>(), ty = qe[1].get<double>();
    // A point inside the source circle, on the side the arc turns to (left, towards the centre).
    run(s, "sketch.add_curves",
        {{"sketch", qid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {tx - 15, ty - 15}}}})}});
    const Json qc = run(s, "sketch.curves", {{"sketch", qid}});
    const auto& qa = qc.at("curves")[1].at("points");
    INFO(qc.dump());
    CHECK(qa[2][0].get<double>() == Approx(tx - 15).margin(1e-6));
    CHECK(qa[2][1].get<double>() == Approx(ty - 15).margin(1e-6));


    // Codex: a line stored horizontal but held vertical by its fixes; the free arc end must stay
    // where it was put although the solve starts from the stored line.
    const Json v = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 0}, {"point", "end"}}})},
                                                      {"at", Json::array({Json::array({0, 0}), Json::array({0, 10})})}}})}});
    const auto vid = v.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", vid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {-10, 20}}}})}});
    const Json vc = run(s, "sketch.curves", {{"sketch", vid}});
    const auto& va = vc.at("curves")[1].at("points");
    INFO(vc.dump());
    CHECK(va[0][0].get<double>() == Approx(0).margin(1e-6));
    CHECK(va[0][1].get<double>() == Approx(10).margin(1e-6));
    CHECK(va[2][0].get<double>() == Approx(-10).margin(1e-6));
    CHECK(va[2][1].get<double>() == Approx(20).margin(1e-6));

    // The end may be given as expressions, like other sketch points.
    const Json x = run(s, "sketch.create",
                       {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})}});
    const auto xid = x.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", xid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {"10 + 10", "5 * 2"}}}})}});
    const Json xc = run(s, "sketch.curves", {{"sketch", xid}});
    const auto& xa = xc.at("curves")[1].at("points");
    CHECK(xa[2][0].get<double>() == Approx(20).margin(1e-6));
    CHECK(xa[2][1].get<double>() == Approx(10).margin(1e-6));


    // Codex: the line's end solves to (0, 0), where its start is stored; the new arc starting there
    // must be joined to the end (by its constraint), not to the stored start.
    const Json w = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 0}, {"point", "end"}}})},
                                                      {"at", Json::array({Json::array({-10, 0}), Json::array({0, 0})})}}})}});
    const auto wid = w.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", wid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {10, 10}}}})}});
    const Json wc = run(s, "sketch.curves", {{"sketch", wid}});
    const auto& wl = wc.at("curves")[0].at("points");
    const auto& wa = wc.at("curves")[1].at("points");
    INFO(wc.dump());
    CHECK(wl[0][0].get<double>() == Approx(-10).margin(1e-6));
    CHECK(wa[0][0].get<double>() == Approx(0).margin(1e-6));
    CHECK(wa[0][1].get<double>() == Approx(0).margin(1e-6));
    CHECK(wa[2][0].get<double>() == Approx(10).margin(1e-6));
    CHECK(wa[2][1].get<double>() == Approx(10).margin(1e-6));


    // Codex: a line drawn on from a settled arc's end in the same batch stays joined to it (the
    // settle moves the stored arc, so the join becomes a constraint).
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                     {{"type", "vertical"}, {"curve", 0}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", 10}}})}});
    const auto bid = b.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", bid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {-10, 20}}},
                                 {{"type", "line"}, {"start", {-10, 20}}, {"end", {-20, 20}}}})}});
    run(s, "sketch.edit_constraint", {{"sketch", bid}, {"index", 2}, {"value", 15}});
    const Json bc = run(s, "sketch.curves", {{"sketch", bid}});
    const auto& ba = bc.at("curves")[1].at("points");
    const auto& bl = bc.at("curves")[2].at("points");
    INFO(bc.dump());
    CHECK(ba[0][1].get<double>() == Approx(15).margin(1e-6));
    CHECK(bl[0][0].get<double>() == Approx(ba[2][0].get<double>()).margin(1e-6));
    CHECK(bl[0][1].get<double>() == Approx(ba[2][1].get<double>()).margin(1e-6));


    // Codex: the requested end (10, 0) is where the line's end is stored, not where it is shown;
    // the arc must not be welded to it (that collapsed the arc onto (0, 10)).
    const Json e2 = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                         {"constraints", Json::array({{{"type", "fix"},
                                                       {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 0}, {"point", "end"}}})},
                                                       {"at", Json::array({Json::array({0, 0}), Json::array({0, 10})})}}})}});
    const auto e2id = e2.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", e2id},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {10, 0}}}})}});
    const Json e2c = run(s, "sketch.curves", {{"sketch", e2id}});
    const auto& e2a = e2c.at("curves")[1].at("points");
    INFO(e2c.dump());
    CHECK(e2a[0][1].get<double>() == Approx(10).margin(1e-4));
    CHECK(e2a[2][0].get<double>() == Approx(10).margin(1e-4));
    CHECK(e2a[2][1].get<double>() == Approx(0).margin(1e-4));

    // An end put on another curve's end (as shown) is joined to it.
    const Json j2 = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}},
                                                 {{"type", "line"}, {"start", {20, 10}}, {"end", {30, 10}}}})}});
    const auto j2id = j2.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", j2id},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {20, 10}}}})}});
    const Json j2c = run(s, "sketch.curves", {{"sketch", j2id}});
    bool joined = false;
    for (const auto& k : j2c.at("constraints"))
        if (k.at("type") == "coincident" && k.at("points")[0].at("curve") == 2 && k.at("points")[0].at("point") == "end" &&
            k.at("points")[1].at("curve") == 1 && k.at("points")[1].at("point") == "start")
            joined = true;
    CHECK(joined);

    // Codex: an open spline drawn on from a settled arc's end in the same batch stays joined too.
    const Json sp = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                         {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                      {{"type", "vertical"}, {"curve", 0}},
                                                      {{"type", "length"}, {"curve", 0}, {"value", 10}}})}});
    const auto spid = sp.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", spid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {-10, 20}}},
                                 {{"type", "spline"}, {"points", Json::array({Json::array({-10, 20}), Json::array({-15, 25}), Json::array({-20, 20})})}}})}});
    run(s, "sketch.edit_constraint", {{"sketch", spid}, {"index", 2}, {"value", 15}});
    const Json spc = run(s, "sketch.curves", {{"sketch", spid}});
    const auto& spa = spc.at("curves")[1].at("points");
    const auto& sps = spc.at("curves")[2].at("points");
    INFO(spc.dump());
    CHECK(sps[0][0].get<double>() == Approx(spa[2][0].get<double>()).margin(1e-6));
    CHECK(sps[0][1].get<double>() == Approx(spa[2][1].get<double>()).margin(1e-6));


    // side -2 (inside, second the larger) only fits two arcs/circles: on a line it is refused
    // and the sketch is unchanged.
    const Json lc = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {-20, 0}}, {"end", {20, 0}}},
                                                 {{"type", "circle"}, {"center", {0, -5}}, {"radius", 10}}})}});
    const auto lcid = lc.at("id").get<std::uint64_t>();
    const auto lcBefore = run(s, "sketch.curves", {{"sketch", lcid}}).dump();
    CHECK_FALSE(s.execute("sketch.add_constraints",
                          {{"sketch", lcid},
                           {"constraints", Json::array({{{"type", "tangent"}, {"curves", Json::array({0, 1})}, {"side", -2}}})}})
                    .ok);
    CHECK(run(s, "sketch.curves", {{"sketch", lcid}}).dump() == lcBefore);
}

TEST_CASE("Design commands: sketch edits start from the solved geometry (CAD-216)", "[commands][cad][solver]")
{
    commands::Session s;
    auto pts = [&](std::uint64_t id, int curve) { return run(s, "sketch.curves", {{"sketch", id}}).at("curves")[curve].at("points"); };

    // Codex (#184): a line stored (0,0)-(10,0) and fixed at (0,0)-(0,10). A line added from
    // (10, 0) used to weld to the stale stored end and solve to (0, 10); it stays at (10, 0).
    const Json a = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 0}, {"point", "end"}}})},
                                                      {"at", Json::array({Json::array({0, 0}), Json::array({0, 10})})}}})}});
    const auto aid = a.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", aid}, {"curves", Json::array({{{"type", "line"}, {"start", {10, 0}}, {"end", {20, 0}}}})}});
    {
        const Json src = pts(aid, 0), added = pts(aid, 1);
        INFO(src.dump() << " " << added.dump());
        CHECK(src[1][0].get<double>() == Approx(0).margin(1e-6));
        CHECK(src[1][1].get<double>() == Approx(10).margin(1e-6));
        CHECK(added[0][0].get<double>() == Approx(10).margin(1e-6));
        CHECK(added[0][1].get<double>() == Approx(0).margin(1e-6));
    }
    // The same after a tangent arc that ends at (10, 0), in one batch.
    run(s, "sketch.add_curves",
        {{"sketch", aid},
         {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {10, 0}}},
                                 {{"type", "line"}, {"start", {10, 0}}, {"end", {10, -10}}}})}});
    {
        const Json arc = pts(aid, 2), line = pts(aid, 3);
        INFO(arc.dump() << " " << line.dump());
        CHECK(arc[2][0].get<double>() == Approx(10).margin(1e-4));
        CHECK(arc[2][1].get<double>() == Approx(0).margin(1e-4));
        CHECK(line[0][0].get<double>() == Approx(10).margin(1e-4));
        CHECK(line[0][1].get<double>() == Approx(0).margin(1e-4));
    }

    // A line drawn on from a dimensioned end joins it without a constraint of its own, so it
    // follows when the dimension changes (Fusion).
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                     {{"type", "horizontal"}, {"curve", 0}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", 20}}})}});
    const auto bid = b.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", bid}, {"curves", Json::array({{{"type", "line"}, {"start", {20, 0}}, {"end", {20, 10}}}})}});
    const std::size_t constraintsBefore = run(s, "sketch.curves", {{"sketch", bid}}).at("constraints").size();
    run(s, "sketch.edit_constraint", {{"sketch", bid}, {"index", 2}, {"value", 25}});
    {
        const Json c = run(s, "sketch.curves", {{"sketch", bid}});
        const Json up = c.at("curves")[1].at("points");
        INFO(c.dump());
        CHECK(c.at("constraints").size() == constraintsBefore);
        CHECK(up[0][0].get<double>() == Approx(25).margin(1e-6));
        CHECK(up[0][1].get<double>() == Approx(0).margin(1e-6));
    }
    // Each edit starts by storing the curves where they were shown: the line's item now ends
    // at 20 (shown when the dimension was edited), not at 10 (where it was drawn).
    {
        const Json tl = run(s, "timeline.list");
        const Json item = tl.at("features")[1].at("params").at("curves")[0];
        INFO(item.dump());
        CHECK(item.at("end")[0].get<double>() == Approx(20).margin(1e-6));
    }

    // Items the solver did not move keep their expressions.
    run(s, "parameter.set", {{"name", "r"}, {"expression", "5 mm"}});
    const Json c = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "circle"}, {"center", {50, 50}}, {"radius", "r"}},
                                                {{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                        {"constraints", Json::array({{{"type", "length"}, {"curve", 1}, {"value", 20}}})}});
    const auto cid = c.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", cid}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 30}}, {"end", {10, 30}}}})}});
    {
        const Json tl = run(s, "timeline.list");
        const Json items = tl.at("features")[2].at("params").at("curves");
        INFO(items.dump());
        CHECK(items[0].at("radius") == "r");
        CHECK(items[1].at("end")[0].get<double>() != Approx(10).margin(1e-3));
    }

    // A polygon pulled out of regular stays a polygon item; its shown shape does not change.
    const Json d = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "polygon"}, {"center", {0, 0}}, {"radius", 10}, {"sides", 4}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 1}, {"point", "start"}}})},
                                                      {"at", Json::array({Json::array({10, 0}), Json::array({2, 8})})}}})}});
    const auto did = d.at("id").get<std::uint64_t>();
    const Json shownBefore = run(s, "sketch.curves", {{"sketch", did}}).at("curves");
    run(s, "sketch.add_curves",
        {{"sketch", did}, {"curves", Json::array({{{"type", "line"}, {"start", {40, 0}}, {"end", {50, 0}}}})}});
    {
        const Json tl = run(s, "timeline.list");
        CHECK(tl.at("features")[3].at("params").at("curves")[0].at("type") == "polygon");
        const Json shown = run(s, "sketch.curves", {{"sketch", did}}).at("curves");
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t k = 0; k < 2; ++k)
                for (std::size_t x = 0; x < 2; ++x)
                    CHECK(shown[i].at("points")[k][x].get<double>() ==
                          Approx(shownBefore[i].at("points")[k][x].get<double>()).margin(1e-6));
    }

    // Codex (#190): a line welded to a vertex of such a polygon stays as written with it, so
    // they stay joined when the vertex moves later.
    const Json e = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "polygon"}, {"center", {0, 0}}, {"radius", 10}, {"sides", 4}},
                                                {{"type", "line"}, {"start", {0, 10}}, {"end", {0, 30}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 1}, {"point", "start"}}})},
                                                      {"at", Json::array({Json::array({10, 0}), Json::array({2, 8})})}}})}});
    const auto eid = e.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", eid}, {"curves", Json::array({{{"type", "line"}, {"start", {40, 0}}, {"end", {50, 0}}}})}});
    {
        const Json tl = run(s, "timeline.list");
        const Json items = tl.at("features")[4].at("params").at("curves");
        INFO(items.dump());
        CHECK(items[0].at("type") == "polygon");
        CHECK(items[1].at("start") == Json::array({0, 10}));
    }
    run(s, "sketch.edit_constraint", {{"sketch", eid}, {"index", 0}, {"remove", true}});
    run(s, "sketch.add_constraints",
        {{"sketch", eid},
         {"constraints", Json::array({{{"type", "fix"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 1}, {"point", "start"}}})},
                                       {"at", Json::array({Json::array({10, 0}), Json::array({3, 7})})}}})}});
    {
        const Json c2 = run(s, "sketch.curves", {{"sketch", eid}});
        const Json vertex = c2.at("curves")[1].at("points")[0];
        const Json line = c2.at("curves")[4].at("points")[0];
        INFO(c2.dump());
        CHECK(vertex[0].get<double>() == Approx(3).margin(1e-6));
        CHECK(vertex[1].get<double>() == Approx(7).margin(1e-6));
        CHECK(line[0].get<double>() == Approx(3).margin(1e-6));
        CHECK(line[1].get<double>() == Approx(7).margin(1e-6));
    }

    // Codex (#190): ends that only meet after solving are not welded by the write-back. Line A
    // ends at (10, 0) by its length; line B, drawn at (20, 0)-(30, 0), is fixed at (10, 0)-(20, 0).
    // A later edit must not join them, so A's length can still change.
    const Json f = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}},
                                                {{"type", "line"}, {"start", {20, 0}}, {"end", {30, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                     {{"type", "horizontal"}, {"curve", 0}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", 10}},
                                                     {{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 1}, {"point", "start"}}, {{"curve", 1}, {"point", "end"}}})},
                                                      {"at", Json::array({Json::array({10, 0}), Json::array({20, 0})})}}})}});
    const auto fid = f.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", fid}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 40}}, {"end", {10, 40}}}})}});
    run(s, "sketch.edit_constraint", {{"sketch", fid}, {"index", 2}, {"value", 15}});
    {
        const Json c3 = run(s, "sketch.curves", {{"sketch", fid}});
        INFO(c3.dump());
        CHECK(c3.at("curves")[0].at("points")[1][0].get<double>() == Approx(15).margin(1e-6));
        CHECK(c3.at("curves")[1].at("points")[0][0].get<double>() == Approx(10).margin(1e-6));
    }

    // Codex (#190): like the solver, an end joined to an earlier end is not one later ends join
    // to. Starts at x = 0, 0.75e-6, 1.5e-6: the third is apart from the first two. Both fixed at
    // (0, 0), the write-back must not weld the third to the first.
    const Json g = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {0, 50}}},
                                                {{"type", "line"}, {"start", {0.75e-6, 0}}, {"end", {50, 50}}},
                                                {{"type", "line"}, {"start", {1.5e-6, 0}}, {"end", {50, 0}}}})},
                        {"constraints", Json::array({{{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 2}, {"point", "start"}}})},
                                                      {"at", Json::array({Json::array({0, 0})})}},
                                                     {{"type", "fix"},
                                                      {"points", Json::array({{{"curve", 0}, {"point", "start"}}})},
                                                      {"at", Json::array({Json::array({0, 0})})}}})}});
    const auto gid = g.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", gid}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 80}}, {"end", {10, 80}}}})}});
    run(s, "sketch.edit_constraint", {{"sketch", gid}, {"index", 1}, {"remove", true}});
    run(s, "sketch.add_constraints",
        {{"sketch", gid},
         {"constraints", Json::array({{{"type", "fix"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "start"}}})},
                                       {"at", Json::array({Json::array({1, 0})})}}})}});
    {
        const Json c4 = run(s, "sketch.curves", {{"sketch", gid}});
        INFO(c4.dump());
        CHECK(c4.at("curves")[0].at("points")[0][0].get<double>() == Approx(1).margin(1e-6));
        CHECK(c4.at("curves")[2].at("points")[0][0].get<double>() == Approx(0).margin(1e-6));
    }

    // Codex (#190): an unchanged expression inside a point stays when the item is rewritten.
    run(s, "parameter.set", {{"name", "x"}, {"expression", "5 mm"}});
    const Json h = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "circle"}, {"center", {"x", 0}}, {"radius", 10}}})},
                        {"constraints", Json::array({{{"type", "radius"}, {"curve", 0}, {"value", 7}}})}});
    const auto hid = h.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", hid}, {"curves", Json::array({{{"type", "line"}, {"start", {40, 0}}, {"end", {50, 0}}}})}});
    {
        const Json tl = run(s, "timeline.list");
        Json item;
        for (const auto& feat : tl.at("features"))
            if (feat.at("id") == hid)
                item = feat.at("params").at("curves")[0];
        INFO(item.dump());
        CHECK(item.at("center")[0] == "x");
        CHECK(item.at("radius").get<double>() == Approx(7).margin(1e-6));
    }
    run(s, "parameter.set", {{"name", "x"}, {"expression", "8 mm"}});
    CHECK(run(s, "sketch.curves", {{"sketch", hid}}).at("curves")[0].at("points")[0][0].get<double>() ==
          Approx(8).margin(1e-6));

    // Codex (#190): a rectangle whose corner keeps an expression while its size changes.
    const Json r = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "rectangle"}, {"corner", {"x", 0}}, {"width", 10}, {"height", 10}}})},
                        {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}},
                                                     {{"type", "length"}, {"curve", 0}, {"value", 20}}})}});
    const auto rid = r.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", rid}, {"curves", Json::array({{{"type", "line"}, {"start", {80, 0}}, {"end", {90, 0}}}})}});
    {
        const Json tl = run(s, "timeline.list");
        Json item;
        for (const auto& feat : tl.at("features"))
            if (feat.at("id") == rid)
                item = feat.at("params").at("curves")[0];
        INFO(item.dump());
        CHECK(item.at("type") == "rectangle");
        CHECK(item.at("corner")[0] == "x");
        const Json bottom = run(s, "sketch.curves", {{"sketch", rid}}).at("curves")[0].at("points");
        CHECK(bottom[1][0].get<double>() - bottom[0][0].get<double>() == Approx(20).margin(1e-6));
    }

    // Codex (#190): a circle written with a diameter and a radius expression (the diameter wins)
    // whose solved radius equals the expression.
    run(s, "parameter.set", {{"name", "rr"}, {"expression", "15 mm"}});
    const Json q = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "circle"}, {"center", {200, 0}}, {"diameter", 20}, {"radius", "rr"}}})},
                        {"constraints", Json::array({{{"type", "radius"}, {"curve", 0}, {"value", 15}}})}});
    const auto qid = q.at("id").get<std::uint64_t>();
    run(s, "sketch.add_curves",
        {{"sketch", qid}, {"curves", Json::array({{{"type", "line"}, {"start", {80, 0}}, {"end", {90, 0}}}})}});
    CHECK(run(s, "sketch.curves", {{"sketch", qid}}).at("curves")[0].at("radius").get<double>() == Approx(15).margin(1e-6));
}

TEST_CASE("Design commands: Extend a sketch line or arc to the nearest curve (CAD-225)", "[commands][cad]")
{
    commands::Session s;
    auto pts = [&](std::uint64_t id, int curve) { return run(s, "sketch.curves", {{"sketch", id}}).at("curves")[curve].at("points"); };
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };

    // A line up to a wall, keeping its constraint; then an arc on around its circle.
    const Json a = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {5, 0}}},
                                                {{"type", "line"}, {"start", {10, -20}}, {"end", {10, 20}}},
                                                {{"type", "line"}, {"start", {20, -20}}, {"end", {20, 20}}}})},
                        {"constraints", Json::array({{{"type", "horizontal"}, {"curve", 0}}})}});
    const auto id = a.at("id").get<std::uint64_t>();

    // Preview: what it would make, without a change.
    const Json pv = run(s, "sketch.extend_preview", {{"sketch", id}, {"curve", 0}, {"at", {4, 0.2}}});
    INFO(pv.dump());
    CHECK(pv.at("extendable") == true);
    CHECK(pv.at("end") == "end");
    CHECK(near(pv.at("points")[1], 10, 0));
    CHECK(near(pts(id, 0)[1], 5, 0));

    run(s, "sketch.extend", {{"sketch", id}, {"curve", 0}, {"at", {4, 0.2}}});
    CHECK(near(pts(id, 0)[0], 0, 0));
    CHECK(near(pts(id, 0)[1], 10, 0)); // the nearest wall, not the one at x = 20
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("constraints").size() == 1);
    // Once more: on to the next wall.
    run(s, "sketch.extend", {{"sketch", id}, {"curve", 0}, {"at", {9, 0}}});
    // The end now touches the first wall's middle, not its end, so it is free.
    CHECK(near(pts(id, 0)[1], 20, 0));

    // Nothing lies beyond the start: refused, nothing changes.
    const Json lineBefore = pts(id, 0);
    {
        const auto r = s.execute("sketch.extend", {{"sketch", id}, {"curve", 0}, {"at", {1, 0}}});
        INFO(r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("Nothing to extend to") != std::string::npos);
        CHECK(pts(id, 0) == lineBefore);
    }

    // An arc about (0, 0), radius 10, from (10, 0) to (0, 10) counter-clockwise, extended at its end
    // to the line x = -6: it ends at (-6, 8).
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "arc"}, {"start", {10, 0}}, {"through", {7.0710678118654755, 7.0710678118654755}}, {"end", {0, 10}}},
                                                {{"type", "line"}, {"start", {-6, -20}}, {"end", {-6, 20}}}})}});
    const auto bid = b.at("id").get<std::uint64_t>();
    run(s, "sketch.extend", {{"sketch", bid}, {"curve", 0}, {"at", {1, 9}}});
    {
        const Json p = pts(bid, 0);
        INFO(p.dump());
        CHECK(near(p[0], 10, 0));
        CHECK(near(p[2], -6, 8));
        CHECK(std::hypot(p[1][0].get<double>(), p[1][1].get<double>()) == Approx(10).margin(1e-6));
    }
    // Its start, clockwise on, meets the line at (-6, -8).
    run(s, "sketch.extend", {{"sketch", bid}, {"curve", 0}, {"at", {9, -1}}});
    CHECK(near(pts(bid, 0)[0], -6, -8));
    CHECK(near(pts(bid, 0)[2], -6, 8));

    // Refused: an end joined to another curve, a rectangle side, a circle.
    const Json c = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {5, 0}}},
                                                {{"type", "line"}, {"start", {5, 0}}, {"end", {5, 5}}},
                                                {{"type", "line"}, {"start", {10, -20}}, {"end", {10, 20}}},
                                                {{"type", "rectangle"}, {"corner", {-20, -2}}, {"width", 5}, {"height", 4}},
                                                {{"type", "circle"}, {"center", {30, 0}}, {"radius", 2}}})}});
    const auto cid = c.at("id").get<std::uint64_t>();
    auto refused = [&](int curve, std::vector<double> at, const char* why) {
        const auto r = s.execute("sketch.extend", {{"sketch", cid}, {"curve", curve}, {"at", at}});
        INFO(r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find(why) != std::string::npos);
    };
    refused(0, {4, 0}, "joined");
    refused(3, {-16, -2}, "rectangle");
    refused(7, {32, 0}, "lines and arcs");
    const Json np = run(s, "sketch.extend_preview", {{"sketch", cid}, {"curve", 0}, {"at", {4, 0}}});
    CHECK(np.at("extendable") == false);
    CHECK(np.at("reason").get<std::string>().find("joined") != std::string::npos);

    // A length dimension holds the line: the solver would slide it (2.5..7.5) instead of
    // lengthening it, so Extend is refused and nothing moves (Codex review on #197).
    const Json d = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {5, 0}}},
                                                {{"type", "line"}, {"start", {10, -20}}, {"end", {10, 20}}}})},
                        {"constraints", Json::array({{{"type", "length"}, {"curve", 0}, {"value", 5}}})}});
    const auto did = d.at("id").get<std::uint64_t>();
    const Json held = pts(did, 0);
    const auto steps = run(s, "edit.history").dump();
    {
        const auto r = s.execute("sketch.extend", {{"sketch", did}, {"curve", 0}, {"at", {4, 0}}});
        INFO(r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("constraints") != std::string::npos);
    }
    CHECK(near(pts(did, 0)[0], held[0][0].get<double>(), held[0][1].get<double>()));
    CHECK(near(pts(did, 0)[1], held[1][0].get<double>(), held[1][1].get<double>()));
    CHECK(run(s, "edit.history").dump() == steps); // no undo step

    // A spline in the way bounds the extension too: the line stops at it (x = 10), not at the
    // line beyond (x = 20); an arc stops where it meets one (Codex review on #197).
    const Json e = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {5, 0}}},
                                                {{"type", "spline"}, {"points", {{12, -6}, {10, 0}, {12, 6}}}},
                                                {{"type", "line"}, {"start", {20, -20}}, {"end", {20, 20}}},
                                                {{"type", "arc"}, {"start", {-30, 0}}, {"through", {-32.928932188134524, 7.0710678118654755}}, {"end", {-40, 10}}},
                                                {{"type", "spline"}, {"points", {{-46, -20}, {-46, 0}, {-46, 20}}}}})}});
    const auto eid = e.at("id").get<std::uint64_t>();
    run(s, "sketch.extend", {{"sketch", eid}, {"curve", 0}, {"at", {4, 0}}});
    {
        const Json p = pts(eid, 0);
        INFO(p.dump());
        CHECK(p[1][0].get<double>() == Approx(10).margin(1e-3));
        CHECK(p[1][1].get<double>() == Approx(0).margin(1e-9));
    }
    // The arc about (-40, 0), radius 10, from (-30, 0) to (-40, 10): on to the spline at x = -46.
    run(s, "sketch.extend", {{"sketch", eid}, {"curve", 3}, {"at", {-39, 9.5}}});
    {
        const Json p = pts(eid, 3);
        INFO(p.dump());
        CHECK(p[2][0].get<double>() == Approx(-46).margin(1e-3));
        CHECK(p[2][1].get<double>() == Approx(8).margin(1e-3));
    }
}

TEST_CASE("Design commands: Break a sketch curve where other curves cross it (CAD-229)", "[commands][cad]")
{
    commands::Session s;
    auto curves = [&](std::uint64_t id) { return run(s, "sketch.curves", {{"sketch", id}}).at("curves"); };
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };

    // A line crossed by two lines breaks into three; nothing is removed.
    const Json a = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {30, 0}}},
                                                {{"type", "line"}, {"start", {10, -5}}, {"end", {10, 5}}},
                                                {{"type", "line"}, {"start", {20, -5}}, {"end", {20, 5}}}})},
                        {"constraints", Json::array({{{"type", "horizontal"}, {"curve", 0}}})}});
    const auto id = a.at("id").get<std::uint64_t>();

    // Preview: the middle piece under the click, without a change.
    const Json pv = run(s, "sketch.break_preview", {{"sketch", id}, {"curve", 0}, {"at", {15, 0.3}}});
    INFO(pv.dump());
    CHECK(pv.at("breakable") == true);
    CHECK(pv.at("kind") == "line");
    CHECK(near(pv.at("points")[0], 10, 0));
    CHECK(near(pv.at("points")[1], 20, 0));
    CHECK(curves(id).size() == 3);

    run(s, "sketch.break", {{"sketch", id}, {"curve", 0}, {"at", {15, 0.3}}});
    {
        const Json c = curves(id);
        INFO(c.dump());
        REQUIRE(c.size() == 5);
        // The two crossing lines first, then the pieces in order along the line.
        CHECK(near(c[2].at("points")[0], 0, 0));
        CHECK(near(c[2].at("points")[1], 10, 0));
        CHECK(near(c[3].at("points")[0], 10, 0));
        CHECK(near(c[3].at("points")[1], 20, 0));
        CHECK(near(c[4].at("points")[0], 20, 0));
        CHECK(near(c[4].at("points")[1], 30, 0));
        // The constraint on the broken line goes, as in Trim.
        CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("constraints").empty());
    }
    // One undo step brings the whole line back.
    run(s, "edit.undo");
    CHECK(curves(id).size() == 3);
    run(s, "edit.redo");

    // A piece with nothing more crossing it: refused, nothing changes.
    {
        const auto r = s.execute("sketch.break", {{"sketch", id}, {"curve", 3}, {"at", {15, 0}}});
        INFO(r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("Nothing to break at") != std::string::npos);
        CHECK(curves(id).size() == 5);
        CHECK(run(s, "sketch.break_preview", {{"sketch", id}, {"curve", 3}, {"at", {15, 0}}}).at("breakable") == false);
    }

    // A circle crossed by a line breaks into two arcs that make the circle again.
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 10}},
                                                {{"type", "line"}, {"start", {-20, 0}}, {"end", {20, 0}}}})}});
    const auto bid = b.at("id").get<std::uint64_t>();
    run(s, "sketch.break", {{"sketch", bid}, {"curve", 0}, {"at", {0, 10}}});
    {
        const Json c = curves(bid);
        INFO(c.dump());
        // The line is not broken (only the clicked curve is), the circle is two arcs.
        REQUIRE(c.size() == 3);
        CHECK(c[0].at("kind") == "line");
        CHECK(c[1].at("kind") == "arc");
        CHECK(c[2].at("kind") == "arc");
        // The picked half is the upper one, through (0, 10); the other through (0, -10).
        CHECK(near(c[1].at("points")[1], 0, 10));
        CHECK(near(c[2].at("points")[1], 0, -10));
        // Two closed half-discs: the arcs and the line between the crossings bound profiles.
        CHECK(!run(s, "sketch.profiles", {{"sketch", bid}}).at("profiles").empty());
    }

    // A spline crossing the line counts too (and in Trim, which shares the code).
    const Json c = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {20, 0}}},
                                                {{"type", "spline"}, {"points", {{12, -6}, {10, 0}, {12, 6}}}}})}});
    const auto cid = c.at("id").get<std::uint64_t>();
    run(s, "sketch.trim", {{"sketch", cid}, {"curve", 0}, {"at", {15, 0}}});
    {
        const Json k = curves(cid);
        INFO(k.dump());
        REQUIRE(k.size() == 2);
        CHECK(k[1].at("kind") == "line");
        CHECK(k[1].at("points")[1][0].get<double>() == Approx(10).margin(1e-3));
    }
}

TEST_CASE("Design commands: sketch points (CAD-235)", "[commands][cad][solver]")
{
    commands::Session s;
    auto curves = [&](std::uint64_t id) { return run(s, "sketch.curves", {{"sketch", id}}).at("curves"); };
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };

    // A rectangle with a free point inside, and a point on one corner.
    const Json a = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                {{"type", "point"}, {"at", {10, 5}}},
                                                {{"type", "point"}, {"at", {40, 20}}}})}});
    const auto id = a.at("id").get<std::uint64_t>();
    {
        const Json c = curves(id);
        INFO(c.dump());
        REQUIRE(c.size() == 6);
        CHECK(c[4].at("kind") == "point");
        CHECK(near(c[4].at("points")[0], 10, 5));
    }
    // Points bound no profile: still the one rectangle.
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 1);

    // Dimensions place the free point: 12 from the left side, 6 above the bottom.
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "horizontal_distance"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 4}, {"point", "start"}}})},
                                       {"value", 12}},
                                      {{"type", "vertical_distance"},
                                       {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 4}, {"point", "start"}}})},
                                       {"value", 6}}})}});
    {
        // Relative to the corner (the rectangle itself is free to move).
        const Json c = curves(id);
        INFO(c.dump());
        const Json corner = c[0].at("points")[0];
        CHECK(near(c[4].at("points")[0], corner[0].get<double>() + 12, corner[1].get<double>() + 6));
    }

    // The corner point is that corner: dragging the corner takes the point with it.
    run(s, "sketch.move_point", {{"sketch", id}, {"point", {{"curve", 1}, {"point", "end"}}}, {"to", {44, 22}}});
    {
        const Json c = curves(id);
        INFO(c.dump());
        CHECK(near(c[5].at("points")[0], c[1].at("points")[1][0].get<double>(), c[1].at("points")[1][1].get<double>()));
    }

    // Mirror a point about a line, construction, delete; trim refuses a point.
    const Json b = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, -10}}, {"end", {0, 10}}},
                                                {{"type", "point"}, {"at", {5, 3}}}})}});
    const auto bid = b.at("id").get<std::uint64_t>();
    run(s, "sketch.mirror", {{"sketch", bid}, {"curves", Json::array({1})}, {"line", 0}});
    {
        const Json c = curves(bid);
        INFO(c.dump());
        REQUIRE(c.size() == 3);
        CHECK(c[2].at("kind") == "point");
        CHECK(near(c[2].at("points")[0], -5, 3));
    }
    run(s, "sketch.set_construction", {{"sketch", bid}, {"curves", Json::array({2})}});
    CHECK(curves(bid)[2].at("construction") == true);
    CHECK_FALSE(s.execute("sketch.trim", {{"sketch", bid}, {"curve", 1}, {"at", {5, 3}}}).ok);
    CHECK_FALSE(s.execute("sketch.offset", {{"sketch", bid}, {"curves", Json::array({1})}, {"distance", 2}}).ok);
    run(s, "sketch.delete_curves", {{"sketch", bid}, {"curves", Json::array({1})}});
    CHECK(curves(bid).size() == 2);

    // A point on a line end is joined to it: Extend refuses that end (Codex review on #203).
    const Json e = run(s, "sketch.create",
                       {{"plane", "XY"},
                        {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}},
                                                {{"type", "line"}, {"start", {20, -5}}, {"end", {20, 5}}},
                                                {{"type", "point"}, {"at", {10, 0}}}})}});
    const auto eid = e.at("id").get<std::uint64_t>();
    {
        const auto r = s.execute("sketch.extend", {{"sketch", eid}, {"curve", 0}, {"at", {9, 0}}});
        INFO(r.error);
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("joined") != std::string::npos);
        CHECK(near(curves(eid)[0].at("points")[1], 10, 0));
    }
    // "center" (and "end") of a point mean the point, in the command layer too: fix it there.
    run(s, "sketch.add_constraints",
        {{"sketch", eid}, {"constraints", Json::array({{{"type", "fix"}, {"point", {{"curve", 2}, {"point", "center"}}}}})}});
    run(s, "sketch.add_constraints",
        {{"sketch", eid},
         {"constraints", Json::array({{{"type", "distance"},
                                       {"points", Json::array({{{"curve", 2}, {"point", "end"}}, {{"curve", 0}, {"point", "start"}}})},
                                       {"value", 8}}})}});
    {
        const Json c = curves(eid);
        INFO(c.dump());
        CHECK(near(c[2].at("points")[0], 10, 0)); // fixed where it was
        CHECK(std::hypot(c[0].at("points")[0][0].get<double>() - 10, c[0].at("points")[0][1].get<double>()) ==
              Approx(8).margin(1e-6));
    }

    // A sketch of points only evaluates (nothing to extrude, but no error).
    const Json c = run(s, "sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "point"}, {"at", {1, 2}}}})}});
    CHECK(curves(c.at("id").get<std::uint64_t>()).size() == 1);
    CHECK(run(s, "sketch.profiles", {{"sketch", c.at("id")}}).at("profiles").empty());

    // A tangent arc that ends on a sketch point stays joined to it (Codex review): drag the
    // point and the arc's end follows.
    {
        const Json t = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}},
                                                    {{"type", "point"}, {"at", {20, 10}}}})}});
        const auto tid = t.at("id").get<std::uint64_t>();
        run(s, "sketch.add_curves",
            {{"sketch", tid},
             {"curves", Json::array({{{"type", "tangent_arc"}, {"from", {{"curve", 0}, {"point", "end"}}}, {"end", {20, 10}}}})}});
        run(s, "sketch.move_point", {{"sketch", tid}, {"point", {{"curve", 1}, {"point", "start"}}}, {"to", {25, 10}}});
        const Json tc = curves(tid);
        REQUIRE(tc.size() == 3);
        CHECK(near(tc[1].at("points")[0], 25, 10));
        CHECK(near(tc[2].at("points")[2], 25, 10));
    }
}

TEST_CASE("Design commands: holes at sketch points (CAD-241)", "[commands][cad]")
{
    commands::Session s;
    const double pi = std::numbers::pi;
    auto volume = [&] { return run(s, "timeline.list").at("bodies")[0].at("volume").get<double>(); };
    auto status = [&](std::uint64_t id) {
        const Json tl = run(s, "timeline.list");
        for (const auto& f : tl.at("features"))
            if (f.at("id") == id)
                return f.at("status").get<std::string>();
        return std::string("missing");
    };
    // A 40 x 20 x 10 plate above the XY plane and a sketch on XY with three points and a line.
    const auto box = run(s, "feature.box", {{"size", {40, 20, 10}}}).at("id").get<std::uint64_t>();
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "point"}, {"at", {10, 10}}},
                                                 {{"type", "line"}, {"start", {0, 30}}, {"end", {40, 30}}},
                                                 {{"type", "point"}, {"at", {20, 10}}},
                                                 {{"type", "point"}, {"at", {30, 10}}}})}});
    const auto sid = sk.at("id").get<std::uint64_t>();

    // All points, through: the plate is on the +Z side, so the holes go up into it.
    const auto hole = run(s, "feature.hole", {{"body", box}, {"sketch", sid}, {"diameter", 4}}).at("id").get<std::uint64_t>();
    CHECK(volume() == Approx(8000 - 3 * pi * 4 * 10).epsilon(1e-6));
    // Blind, and only two of the points.
    run(s, "feature.edit", {{"id", hole}, {"params", {{"depth", 4}, {"through", false}, {"sketch_points", {0, 3}}}}});
    CHECK(volume() == Approx(8000 - 2 * pi * 4 * 4).epsilon(1e-6));
    // Flip drills away from the plate: nothing is cut (but the hair of clearance the tool starts with).
    run(s, "feature.edit", {{"id", hole}, {"params", {{"flip", true}}}});
    CHECK(volume() == Approx(8000).margin(0.05));
    run(s, "feature.edit", {{"id", hole}, {"params", {{"flip", false}}}});

    // Moving a point moves its hole (no edge of the sketch changes): off the plate, one hole left.
    run(s, "sketch.move_point", {{"sketch", sid}, {"point", {{"curve", 3}, {"point", "start"}}}, {"to", {60, 10}}});
    CHECK(volume() == Approx(8000 - pi * 4 * 4).epsilon(1e-6));
    run(s, "edit.undo");
    CHECK(volume() == Approx(8000 - 2 * pi * 4 * 4).epsilon(1e-6));

    // Deleting the line keeps the picks on the same points (indices follow).
    run(s, "sketch.delete_curves", {{"sketch", sid}, {"curves", {1}}});
    CHECK(status(hole) == "ok");
    CHECK(volume() == Approx(8000 - 2 * pi * 4 * 4).epsilon(1e-6));
    // Deleting a picked point fails the hole until it is picked again.
    run(s, "sketch.delete_curves", {{"sketch", sid}, {"curves", {0}}});
    CHECK(status(hole) == "error");
    run(s, "feature.edit", {{"id", hole}, {"params", {{"sketch_points", {0, 1}}}}});
    CHECK(status(hole) == "ok");
    CHECK(volume() == Approx(8000 - 2 * pi * 4 * 4).epsilon(1e-6));

    // Through all reaches a body well away from the sketch plane (Codex review): a plate at
    // Z = 100 over a point on XY.
    {
        commands::Session far;
        const auto plate = run(far, "feature.box", {{"corner", {0, 0, 100}}, {"size", {40, 20, 10}}}).at("id").get<std::uint64_t>();
        const auto pts = run(far, "sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "point"}, {"at", {10, 10}}}})}})
                             .at("id")
                             .get<std::uint64_t>();
        run(far, "feature.hole", {{"body", plate}, {"sketch", pts}, {"diameter", 4}});
        CHECK(run(far, "timeline.list").at("bodies")[0].at("volume").get<double>() == Approx(8000 - pi * 4 * 10).epsilon(1e-6));
    }

    // A sketch without points, or both placements, is refused.
    const auto empty = run(s, "sketch.create",
                           {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {5, 0}}}})}})
                           .at("id")
                           .get<std::uint64_t>();
    CHECK_FALSE(s.execute("feature.hole", {{"body", box}, {"sketch", empty}, {"diameter", 4}}).ok);
    CHECK_FALSE(s.execute("feature.hole", {{"body", box}, {"sketch", sid}, {"point", {5, 5, 10}}, {"diameter", 4}}).ok);
}

TEST_CASE("Design commands: sketch Move/Copy (CAD-244)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };
    // A 40 x 20 rectangle at the origin and a circle (diameter dimension) beside it.
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {60, 10}}, {"radius", 5}}})},
                         {"constraints", Json::array({{{"type", "diameter"}, {"curve", 4}, {"value", 10}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    auto curves = [&] { return run(s, "sketch.curves", {{"sketch", id}}); };

    SECTION("move a circle and the whole rectangle")
    {
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {4}}, {"dx", 10}});
        CHECK(near(curves().at("curves")[4].at("points")[0], 70, 10));
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"dx", 5}, {"dy", -5}});
        const Json c = curves();
        CHECK(near(c.at("curves")[0].at("points")[0], 5, -5));
        CHECK(near(c.at("curves")[2].at("points")[0], 45, 15));
        CHECK(near(c.at("curves")[4].at("points")[0], 70, 10)); // not picked: stays
        // One undo step per move.
        run(s, "edit.undo");
        CHECK(near(curves().at("curves")[0].at("points")[0], 0, 0));
    }
    SECTION("one side moved: the sides joined to it stretch")
    {
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {1}}, {"dx", 10}});
        const Json c = curves();
        CHECK(near(c.at("curves")[0].at("points")[1], 50, 0));
        CHECK(near(c.at("curves")[2].at("points")[0], 50, 20));
        CHECK(near(c.at("curves")[0].at("points")[0], 0, 0));
    }
    SECTION("turn the rectangle: it becomes a 3-point rectangle and stays one")
    {
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"angle", 90}, {"pivot", {0, 0}}});
        const Json c = curves();
        CHECK(near(c.at("curves")[0].at("points")[0], 0, 0));
        CHECK(near(c.at("curves")[0].at("points")[1], 0, 40));
        CHECK(near(c.at("curves")[2].at("points")[0], -20, 40));
        const Json& items = s.design().features().back().params.at("curves");
        CHECK(items[0].at("type") == "three_point_rectangle");
        CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 2);
    }
    SECTION("copy: whole items and the constraints among them, turned about the middle")
    {
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3, 4}}, {"dy", 50}, {"copy", true}});
        Json c = curves();
        REQUIRE(c.at("curves").size() == 10);
        CHECK(near(c.at("curves")[5].at("points")[0], 0, 50));
        CHECK(near(c.at("curves")[9].at("points")[0], 60, 60));
        // The copied circle keeps its own diameter dimension: change the original's, the copy stays.
        CHECK(c.at("constraints").size() == 2);
        run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", 0}, {"value", 6}});
        c = curves();
        CHECK(c.at("curves")[4].at("radius").get<double>() == Approx(3));
        CHECK(c.at("curves")[9].at("radius").get<double>() == Approx(5));
        CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 4);
        // A turned copy of the rectangle (about its middle, (20, 10)).
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"angle", 90}, {"copy", true}});
        c = curves();
        REQUIRE(c.at("curves").size() == 14);
        CHECK(near(c.at("curves")[10].at("points")[0], 30, -10));
        CHECK(near(c.at("curves")[10].at("points")[1], 30, 30));
        CHECK(near(c.at("curves")[0].at("points")[0], 0, 0)); // the original stays
    }
    SECTION("refused when constraints hold the curves; nothing changes")
    {
        run(s, "sketch.add_constraints",
            {{"sketch", id}, {"constraints", Json::array({{{"type", "coincident"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}, "origin"})}}})}});
        const Json was = curves();
        CHECK_FALSE(s.execute("sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"dx", 5}}).ok);
        CHECK(curves() == was);
        // A copy leaves the origin constraint with the original.
        run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"dx", 50}, {"dy", 50}, {"copy", true}});
        const Json c = curves();
        CHECK(near(c.at("curves")[5].at("points")[0], 50, 50));
        CHECK(c.at("constraints").size() == 2);
    }
    SECTION("a moved circle keeps its size (Codex review)")
    {
        // Radius 5 at (0, 5), tangent to a fixed line along y = 0: moving it up cannot keep
        // both, so the move is refused instead of growing the circle.
        const Json t = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "line"}, {"start", {-20, 0}}, {"end", {20, 0}}},
                                                    {{"type", "circle"}, {"center", {0, 5}}, {"radius", 5}}})},
                            {"constraints", Json::array({{{"type", "fix"}, {"curve", 0}}, {{"type", "tangent"}, {"curves", {0, 1}}}})}});
        const auto tid = t.at("id").get<std::uint64_t>();
        CHECK_FALSE(s.execute("sketch.move_curves", {{"sketch", tid}, {"curves", {1}}, {"dy", 5}}).ok);
        const Json c = run(s, "sketch.curves", {{"sketch", tid}});
        CHECK(c.at("curves")[1].at("radius").get<double>() == Approx(5));
        CHECK(near(c.at("curves")[1].at("points")[0], 0, 5));
    }
    SECTION("one side of a polygon moves where asked; the polygon stays regular (Codex review)")
    {
        const Json t = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "polygon"}, {"center", {0, 0}}, {"radius", 10}, {"sides", 5}}})}});
        const auto tid = t.at("id").get<std::uint64_t>();
        const Json was = run(s, "sketch.curves", {{"sketch", tid}}).at("curves")[0].at("points");
        run(s, "sketch.move_curves", {{"sketch", tid}, {"curves", {0}}, {"dx", 3}, {"dy", 2}});
        const Json c = run(s, "sketch.curves", {{"sketch", tid}});
        CHECK(near(c.at("curves")[0].at("points")[0], was[0][0].get<double>() + 3, was[0][1].get<double>() + 2));
        CHECK(near(c.at("curves")[0].at("points")[1], was[1][0].get<double>() + 3, was[1][1].get<double>() + 2));
    }
    SECTION("a three-point arc through any point moves and copies (Codex review)")
    {
        const Json t = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "arc"}, {"start", {5, 0}}, {"through", {3, 4}}, {"end", {0, 5}}}})}});
        const auto tid = t.at("id").get<std::uint64_t>();
        run(s, "sketch.move_curves", {{"sketch", tid}, {"curves", {0}}, {"dx", 10}});
        run(s, "sketch.move_curves", {{"sketch", tid}, {"curves", {0}}, {"dy", 10}, {"copy", true}});
        const Json c = run(s, "sketch.curves", {{"sketch", tid}});
        REQUIRE(c.at("curves").size() == 2);
        CHECK(near(c.at("curves")[0].at("points")[0], 15, 0));
        CHECK(near(c.at("curves")[0].at("points")[2], 10, 5));
        CHECK(near(c.at("curves")[1].at("points")[0], 15, 10));
        CHECK(near(c.at("curves")[1].at("points")[2], 10, 15));
    }
    SECTION("a turned copy leaves horizontal and vertical with the original (Codex review)")
    {
        const Json t = run(s, "sketch.create",
                           {{"plane", "XY"},
                            {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {10, 0}}}})},
                            {"constraints", Json::array({{{"type", "horizontal"}, {"curve", 0}}, {{"type", "length"}, {"curve", 0}, {"value", 10}}})}});
        const auto tid = t.at("id").get<std::uint64_t>();
        run(s, "sketch.move_curves", {{"sketch", tid}, {"curves", {0}}, {"angle", 90}, {"pivot", {0, 0}}, {"copy", true}});
        const Json c = run(s, "sketch.curves", {{"sketch", tid}});
        REQUIRE(c.at("curves").size() == 2);
        CHECK(near(c.at("curves")[1].at("points")[0], 0, 0));
        CHECK(near(c.at("curves")[1].at("points")[1], 0, 10));
        CHECK(c.at("constraints").size() == 3); // the copy's length, not its horizontal
    }
}

TEST_CASE("Design commands: Sketch Scale (CAD-253)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };
    // A 40 x 20 rectangle at the origin (curves 0-3) and a circle with a diameter dimension (4).
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {60, 10}}, {"radius", 5}}})},
                         {"constraints", Json::array({{{"type", "diameter"}, {"curve", 4}, {"value", 10}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    auto curves = [&] { return run(s, "sketch.curves", {{"sketch", id}}); };

    SECTION("a dimensioned circle about its centre: the dimension scales with it, one undo step")
    {
        run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {4}}, {"factor", 2}});
        const Json c = curves();
        CHECK(near(c.at("curves")[4].at("points")[0], 60, 10)); // the default point: its middle
        CHECK(std::abs(c.at("curves")[4].at("radius").get<double>() - 10) < 1e-6);
        CHECK(std::abs(c.at("constraints")[0].at("value").get<double>() - 20) < 1e-9);
        CHECK(c.at("degreesOfFreedom") == sk.at("degreesOfFreedom"));
        run(s, "edit.undo");
        CHECK(std::abs(curves().at("curves")[4].at("radius").get<double>() - 5) < 1e-6);
    }
    SECTION("a dimension to the sketch origin positions the curve: it is not scaled")
    {
        run(s, "sketch.add_constraints",
            {{"sketch", id},
             {"constraints", Json::array({{{"type", "horizontal_distance"},
                                           {"points", Json::array({"origin", {{"curve", 4}, {"point", "center"}}})},
                                           {"value", 60}}})}});
        run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {4}}, {"factor", 2}});
        const Json c = curves();
        CHECK(near(c.at("curves")[4].at("points")[0], 60, 10));
        CHECK(std::abs(c.at("curves")[4].at("radius").get<double>() - 10) < 1e-6);
        CHECK(std::abs(c.at("constraints")[0].at("value").get<double>() - 20) < 1e-9); // the diameter
        CHECK(std::abs(c.at("constraints")[1].at("value").get<double>() - 60) < 1e-9); // the origin distance
    }
    SECTION("the whole rectangle about a corner")
    {
        run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"factor", 0.5}, {"point", {0, 0}}});
        const Json c = curves();
        CHECK(near(c.at("curves")[0].at("points")[0], 0, 0));
        CHECK(near(c.at("curves")[0].at("points")[1], 20, 0));
        CHECK(near(c.at("curves")[1].at("points")[1], 20, 10));
        CHECK(near(c.at("curves")[4].at("points")[0], 60, 10)); // not picked: stays
        CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 2);
    }
    SECTION("everything about a point away from it: positions and sizes scale")
    {
        run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3, 4}}, {"factor", 3}, {"point", {10, 0}}});
        const Json c = curves();
        CHECK(near(c.at("curves")[0].at("points")[0], -20, 0));
        CHECK(near(c.at("curves")[2].at("points")[0], 100, 60));
        CHECK(near(c.at("curves")[4].at("points")[0], 160, 30));
        CHECK(std::abs(c.at("curves")[4].at("radius").get<double>() - 15) < 1e-6);
    }
    SECTION("a dimension written as an expression stays one: (expr)*factor")
    {
        run(s, "sketch.edit_constraint", {{"sketch", id}, {"index", 0}, {"value", "4*2.5"}});
        run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {4}}, {"factor", 0.5}});
        const Json c = curves();
        CHECK(std::abs(c.at("curves")[4].at("radius").get<double>() - 2.5) < 1e-6);
        const Json v = c.at("constraints")[0].at("value");
        INFO(v.dump());
        CHECK(v.is_string());
    }
    SECTION("refused when constraints hold the curves; nothing changes")
    {
        // A fixed corner holds the rectangle where it is.
        run(s, "sketch.add_constraints", {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}}})}});
        const auto res = s.execute("sketch.scale_curves", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"factor", 2}});
        CHECK_FALSE(res.ok);
        CHECK(res.error.find("cannot be scaled") != std::string::npos);
        CHECK(near(curves().at("curves")[0].at("points")[1], 40, 0));
        CHECK_FALSE(s.execute("sketch.scale_curves", {{"sketch", id}, {"curves", {4}}, {"factor", 0}}).ok);
        CHECK_FALSE(s.execute("sketch.scale_curves", {{"sketch", id}, {"curves", {4}}, {"factor", -2}}).ok);
    }
}

TEST_CASE("Design commands: sketch Circular and Rectangular Pattern (CAD-251)", "[commands][cad][solver]")
{
    commands::Session s;
    auto near = [](const Json& p, double x, double y) {
        return std::hypot(p[0].get<double>() - x, p[1].get<double>() - y) < 1e-6;
    };
    // A 40 x 20 rectangle at the origin (curves 0-3) and a circle with a diameter dimension (4).
    const Json sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}},
                                                 {{"type", "circle"}, {"center", {60, 10}}, {"radius", 5}}})},
                         {"constraints", Json::array({{{"type", "diameter"}, {"curve", 4}, {"value", 10}}})}});
    const auto id = sk.at("id").get<std::uint64_t>();
    auto curves = [&] { return run(s, "sketch.curves", {{"sketch", id}}); };

    SECTION("circular, full circle: evenly around the centre, one undo step")
    {
        run(s, "sketch.circular_pattern", {{"sketch", id}, {"curves", {4}}, {"center", {60, 0}}, {"count", 4}});
        const Json c = curves();
        REQUIRE(c.at("curves").size() == 8);
        CHECK(near(c.at("curves")[4].at("points")[0], 60, 10)); // the original stays
        CHECK(near(c.at("curves")[5].at("points")[0], 50, 0));  // 90 degrees counterclockwise
        CHECK(near(c.at("curves")[6].at("points")[0], 60, -10));
        CHECK(near(c.at("curves")[7].at("points")[0], 70, 0));
        for (std::size_t i = 5; i < 8; ++i)
            CHECK(std::abs(c.at("curves")[i].at("radius").get<double>() - 5) < 1e-6);
        // The diameter dimension comes along with each copy.
        std::size_t diameters = 0;
        for (const auto& k : c.at("constraints"))
            diameters += k.at("type") == "diameter" ? 1 : 0;
        CHECK(diameters == 4);
        run(s, "edit.undo");
        CHECK(curves().at("curves").size() == 5);
    }
    SECTION("circular over part of a circle: first and last instances the angle apart")
    {
        run(s, "sketch.circular_pattern",
            {{"sketch", id}, {"curves", {4}}, {"center", {60, 0}}, {"count", 3}, {"angle", 90}});
        const Json c = curves();
        REQUIRE(c.at("curves").size() == 7);
        CHECK(near(c.at("curves")[5].at("points")[0], 60 - 10 * std::sqrt(0.5), 10 * std::sqrt(0.5)));
        CHECK(near(c.at("curves")[6].at("points")[0], 50, 0));
        // Clockwise with a negative angle.
        run(s, "edit.undo");
        run(s, "sketch.circular_pattern",
            {{"sketch", id}, {"curves", {4}}, {"center", {60, 0}}, {"count", 2}, {"angle", -90}});
        CHECK(near(curves().at("curves")[5].at("points")[0], 70, 0));
    }
    SECTION("circular pattern of a whole rectangle: turned copies stay closed rectangles")
    {
        run(s, "sketch.circular_pattern", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"center", {0, 0}}, {"count", 4}});
        const Json c = curves();
        REQUIRE(c.at("curves").size() == 5 + 12);
        // The 90 degree copy: (40, 0) -> (0, 40), (40, 20) -> (-20, 40).
        CHECK(near(c.at("curves")[5].at("points")[0], 0, 0));
        CHECK(near(c.at("curves")[5].at("points")[1], 0, 40));
        CHECK(near(c.at("curves")[6].at("points")[1], -20, 40));
        // The 180 degree copy.
        CHECK(near(c.at("curves")[9].at("points")[1], -40, 0));
        CHECK(near(c.at("curves")[10].at("points")[1], -40, -20));
        const Json p = run(s, "sketch.profiles", {{"sketch", id}});
        CHECK(p.at("profiles").size() == 5); // four rectangles and the circle
    }
    SECTION("rectangular: rows and columns, negative spacing goes the other way")
    {
        run(s, "sketch.rectangular_pattern",
            {{"sketch", id}, {"curves", {4}}, {"count_x", 3}, {"spacing_x", 15}, {"count_y", 2}, {"spacing_y", -20}});
        const Json c = curves();
        REQUIRE(c.at("curves").size() == 10);
        CHECK(near(c.at("curves")[5].at("points")[0], 75, 10));
        CHECK(near(c.at("curves")[6].at("points")[0], 90, 10));
        CHECK(near(c.at("curves")[7].at("points")[0], 60, -10));
        CHECK(near(c.at("curves")[8].at("points")[0], 75, -10));
        CHECK(near(c.at("curves")[9].at("points")[0], 90, -10));
        run(s, "edit.undo");
        CHECK(curves().at("curves").size() == 5);
    }
    SECTION("rectangular along a turned direction; a whole rectangle stays a rectangle")
    {
        run(s, "sketch.rectangular_pattern",
            {{"sketch", id}, {"curves", {4}}, {"count_x", 2}, {"spacing_x", 10}, {"angle", 90}});
        CHECK(near(curves().at("curves")[5].at("points")[0], 60, 20));
        run(s, "sketch.rectangular_pattern", {{"sketch", id}, {"curves", {0, 1, 2, 3}}, {"count_y", 2}, {"spacing_y", 30}});
        const Json c = curves();
        REQUIRE(c.at("curves").size() == 6 + 4);
        CHECK(near(c.at("curves")[6].at("points")[0], 0, 30));
        CHECK(near(c.at("curves")[8].at("points")[0], 40, 50));
    }
    SECTION("refused without enough instances, a spacing or an angle; nothing changes")
    {
        CHECK_FALSE(s.execute("sketch.circular_pattern", {{"sketch", id}, {"curves", {4}}, {"center", {0, 0}}, {"count", 1}}).ok);
        CHECK_FALSE(s.execute("sketch.circular_pattern",
                              {{"sketch", id}, {"curves", {4}}, {"center", {0, 0}}, {"count", 3}, {"angle", 0}}).ok);
        CHECK_FALSE(s.execute("sketch.circular_pattern",
                              {{"sketch", id}, {"curves", {4}}, {"center", {0, 0}}, {"count", 3}, {"angle", 400}}).ok);
        CHECK_FALSE(s.execute("sketch.rectangular_pattern", {{"sketch", id}, {"curves", {4}}, {"count_x", 1}}).ok);
        CHECK_FALSE(s.execute("sketch.rectangular_pattern", {{"sketch", id}, {"curves", {4}}, {"count_x", 3}}).ok);
        CHECK_FALSE(s.execute("sketch.rectangular_pattern", {{"sketch", id}, {"curves", {9}}, {"count_x", 2}, {"spacing_x", 5}}).ok);
        CHECK(curves().at("curves").size() == 5);
    }
}

TEST_CASE("Design commands: Sketch Text (CAD-263)", "[commands][cad][solver]")
{
    if (!geometry::textSupported() || !commands::Session{}.execute("sketch.create", {{"plane", "XY"},
            {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "I"}}})}}).ok) {
        WARN("no system font: skipped");
        return;
    }
    commands::Session s;
    auto bbox = [](const Json& outlines) {
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (const auto& line : outlines)
            for (const auto& p : line)
                for (int k = 0; k < 2; ++k) {
                    lo[k] = std::min(lo[k], p[k].get<double>());
                    hi[k] = std::max(hi[k], p[k].get<double>());
                }
        return std::array<double, 4>{lo[0], lo[1], hi[0], hi[1]};
    };
    // "O8" at (10, 20), 10 mm tall: two letters, each one profile with its holes (O: 1, 8: 2).
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "text"}, {"at", {10, 20}}, {"text", "O8"}, {"height", 10}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    Json c = run(s, "sketch.curves", {{"sketch", id}});
    INFO(c.dump());
    REQUIRE(c.at("curves").size() == 1);
    const Json t = c.at("curves")[0];
    CHECK(t.at("kind") == "text");
    CHECK(t.at("text") == "O8");
    CHECK(t.at("height").get<double>() == Approx(10));
    CHECK(t.at("angle").get<double>() == Approx(0).margin(1e-9));
    // The letters sit on the baseline and are about a height tall; the start is the origin.
    const auto bb = bbox(t.at("outlines"));
    CHECK(bb[0] >= 10 - 1.5);
    CHECK(bb[1] >= 20 - 1.5);
    CHECK(bb[3] - bb[1] > 4);
    CHECK(bb[3] - bb[1] < 12);
    CHECK(bb[2] - bb[0] > 6);
    CHECK(run(s, "sketch.profiles", {{"sketch", id}}).at("profiles").size() == 2);
    // Extruded, the holes stay open: the volume is below that of the letters' bounding boxes.
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 2}});
    const double vol = ex.at("bodies").back().at("volume").get<double>();
    CHECK(vol > 0);
    CHECK(vol < (bb[2] - bb[0]) * (bb[3] - bb[1]) * 2 * 0.8);
    run(s, "edit.undo");

    // Turned 90 degrees, the same letters stand on their side.
    const auto up = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "O8"}, {"height", 10}, {"angle", 90}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    const auto bu = bbox(run(s, "sketch.curves", {{"sketch", up}}).at("curves")[0].at("outlines"));
    CHECK(bu[3] - bu[1] > bb[2] - bb[0] - 1);
    CHECK(bu[2] - bu[0] < bb[3] - bb[1] + 1);

    // Turned without a pivot (Codex review of #227): about the middle of the letters, as the
    // tool previews it, so the letters stay where they are (their middle does not move).
    {
        const auto turn = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "text"}, {"at", {10, 20}}, {"text", "O8"}, {"height", 10}}})}})
                              .at("id")
                              .get<std::uint64_t>();
        const auto before = bbox(run(s, "sketch.curves", {{"sketch", turn}}).at("curves")[0].at("outlines"));
        run(s, "sketch.move_curves", {{"sketch", turn}, {"curves", {0}}, {"angle", 90}});
        const auto after = bbox(run(s, "sketch.curves", {{"sketch", turn}}).at("curves")[0].at("outlines"));
        CHECK((after[0] + after[2]) / 2 == Approx((before[0] + before[2]) / 2).margin(0.05));
        CHECK((after[1] + after[3]) / 2 == Approx((before[1] + before[3]) / 2).margin(0.05));
    }

    // Move, scale and a circular pattern keep it text; the start and the height point are
    // its constraint points.
    run(s, "sketch.move_curves", {{"sketch", id}, {"curves", {0}}, {"dx", 5}, {"dy", -3}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[0].at("kind") == "text");
    CHECK(c.at("curves")[0].at("points")[0][0].get<double>() == Approx(15));
    CHECK(c.at("curves")[0].at("points")[0][1].get<double>() == Approx(17));
    run(s, "sketch.scale_curves", {{"sketch", id}, {"curves", {0}}, {"factor", 2}, {"point", {15, 17}}});
    c = run(s, "sketch.curves", {{"sketch", id}});
    CHECK(c.at("curves")[0].at("height").get<double>() == Approx(20));
    CHECK(c.at("curves")[0].at("text") == "O8");
    run(s, "sketch.add_constraints",
        {{"sketch", id},
         {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}})}, {"at", Json::array({Json::array({0, 0})})}},
                                      {{"type", "horizontal"}, {"points", Json::array({{{"curve", 0}, {"point", "start"}}, {{"curve", 0}, {"point", "end"}}})}}})}});
    CHECK(run(s, "sketch.curves", {{"sketch", id}}).at("curves")[0].at("points")[0][0].get<double>() == Approx(0).margin(1e-6));

    // Construction text is shown but bounds no profile.
    const auto cn = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "A"}, {"construction", true}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    CHECK(run(s, "sketch.profiles", {{"sketch", cn}}).at("profiles").empty());

    // Refused: empty text, no height; Mirror, Offset, Trim, Break, Extend and Fillet on it.
    CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "  "}}})}}).ok);
    CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "A"}, {"height", 0}}})}}).ok);
    const auto rf = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "text"}, {"at", {0, 0}}, {"text", "A"}},
                                                 {{"type", "line"}, {"start", {0, -5}}, {"end", {0, 10}}}})}})
                        .at("id")
                        .get<std::uint64_t>();
    CHECK_FALSE(s.execute("sketch.mirror", {{"sketch", rf}, {"curves", {0}}, {"line", 1}}).ok);
    CHECK_FALSE(s.execute("sketch.offset", {{"sketch", rf}, {"curves", {0}}, {"distance", 1}}).ok);
    CHECK_FALSE(s.execute("sketch.trim", {{"sketch", rf}, {"curve", 0}, {"at", {1, 1}}}).ok);
    CHECK_FALSE(s.execute("sketch.break", {{"sketch", rf}, {"curve", 0}, {"at", {1, 1}}}).ok);
    CHECK_FALSE(s.execute("sketch.extend", {{"sketch", rf}, {"curve", 0}, {"at", {1, 1}}}).ok);
    // Delete removes it.
    run(s, "sketch.delete_curves", {{"sketch", rf}, {"curves", {0}}});
    CHECK(run(s, "sketch.curves", {{"sketch", rf}}).at("curves").size() == 1);
}

TEST_CASE("Design commands: extrude start offset moves the whole extrusion (CAD-268)", "[commands][cad]")
{
    commands::Session s;
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 20}, {"height", 10}}})}})
                        .at("id").get<std::uint64_t>();
    const auto ex = run(s, "feature.extrude", {{"sketch", id}, {"distance", 5}, {"start_offset", 7}});
    const auto bodyId = ex.at("bodies")[0].at("id").get<std::uint64_t>();
    CHECK(ex.at("bodies")[0].at("volume").get<double>() == Approx(20 * 10 * 5));
    const auto b = s.design().bodies().at(bodyId).shape.bounds();
    CHECK(b.min.z == Approx(7).margin(1e-6));
    CHECK(b.max.z == Approx(12).margin(1e-6));

    SECTION("a negative offset and flip go the other way")
    {
        run(s, "feature.edit", {{"id", ex.at("id")}, {"params", {{"flip", true}}}});
        const auto fb = s.design().bodies().at(bodyId).shape.bounds();
        CHECK(fb.min.z == Approx(-12).margin(1e-6));
        CHECK(fb.max.z == Approx(-7).margin(1e-6));
    }
}

TEST_CASE("Design commands: revolve Symmetric and Two Sides extent (CAD-268)", "[commands][cad]")
{
    commands::Session s;
    // A 10 x 10 square in the XZ plane, 10 mm from the Z axis: revolving it by a total of 90 degrees
    // gives a quarter of the full ring volume.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XZ"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {10, 0}}, {"width", 10}, {"height", 10}}})}})
                        .at("id").get<std::uint64_t>();
    const double pi = 3.14159265358979323846;
    const double full = pi * (20.0 * 20.0 - 10.0 * 10.0) * 10.0;

    SECTION("symmetric: the angle is the total, half on each side")
    {
        const auto r = run(s, "feature.revolve", {{"sketch", id}, {"axis", "Z"}, {"extent", "symmetric"}, {"angle", 90}});
        const auto bodyId = r.at("bodies")[0].at("id").get<std::uint64_t>();
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(full / 4).epsilon(1e-6));
        const auto b = s.design().bodies().at(bodyId).shape.bounds();
        // Centred on the sketch plane (the +X side), so it reaches equally far to +Y and -Y.
        CHECK(b.max.y == Approx(-b.min.y).epsilon(1e-6));
        CHECK(b.max.x == Approx(20).epsilon(1e-6));
    }

    SECTION("two sides: angle forwards, angle2 backwards")
    {
        const auto r = run(s, "feature.revolve",
                           {{"sketch", id}, {"axis", "Z"}, {"extent", "two_sides"}, {"angle", 30}, {"angle2", 60}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(full / 4).epsilon(1e-6));
    }

    SECTION("a full symmetric revolve stays a full revolve")
    {
        const auto r = run(s, "feature.revolve", {{"sketch", id}, {"axis", "Z"}, {"extent", "symmetric"}, {"angle", 360}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(full).epsilon(1e-6));
    }

    SECTION("a symmetric revolve needs a positive angle")
    {
        CHECK_FALSE(s.execute("feature.revolve", {{"sketch", id}, {"axis", "Z"}, {"extent", "symmetric"}, {"angle", -90}}).ok);
        CHECK_FALSE(s.execute("feature.revolve", {{"sketch", id}, {"axis", "Z"}, {"extent", "symmetric"}, {"angle", 0}}).ok);
    }

    SECTION("two sides adding up to more than a turn is refused")
    {
        const auto bad = s.execute("feature.revolve",
                                   {{"sketch", id}, {"axis", "Z"}, {"extent", "two_sides"}, {"angle", 200}, {"angle2", 200}});
        CHECK_FALSE(bad.ok);
    }
}

TEST_CASE("Design commands: polygon modes and slot modes (CAD-269)", "[commands][cad]")
{
    commands::Session s;
    const double pi = 3.14159265358979323846;
    // Area of a regular n-gon with apothem a: n a^2 tan(pi/n).
    auto areaOf = [&](const Json& curves) {
        const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"curves", curves}}).at("id").get<std::uint64_t>();
        return run(s, "sketch.profiles", {{"sketch", id}}).at("profiles")[0].at("area").get<double>();
    };

    SECTION("circumscribed: the radius reaches the middle of the sides")
    {
        const double a = 10;
        CHECK(areaOf(Json::array({{{"type", "polygon"}, {"mode", "circumscribed"}, {"center", {0, 0}}, {"radius", a},
                                   {"sides", 6}}})) == Approx(6 * a * a * std::tan(pi / 6)).epsilon(1e-9));
        const double inscribed = areaOf(Json::array({{{"type", "polygon"}, {"center", {0, 0}}, {"radius", a}, {"sides", 6}}}));
        CHECK(inscribed == Approx(3 * std::sqrt(3.0) / 2 * a * a).epsilon(1e-9));
    }

    SECTION("edge: a side from start to end, the polygon on its left")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "polygon"}, {"mode", "edge"}, {"start", {0, 0}},
                                                      {"end", {10, 0}}, {"sides", 5}}})}})
                            .at("id").get<std::uint64_t>();
        const Json prof = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles")[0];
        // Pentagon of side 10: area = 5/4 * 10^2 / tan(pi/5), above the x axis.
        CHECK(prof.at("area").get<double>() == Approx(5.0 / 4 * 100 / std::tan(pi / 5)).epsilon(1e-9));
        CHECK(prof.at("centroid")[1].get<double>() > 0);
        CHECK(prof.at("centroid")[0].get<double>() == Approx(5).epsilon(1e-9));
        // The curves list shows it as an ordinary polygon item afterwards.
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        CHECK(c.at("curves").size() == 5);
    }

    SECTION("slot: overall and centre point match the centre-to-centre slot")
    {
        const double w = 6, centres = 20;
        const double expected = centres * w + pi * (w / 2) * (w / 2);
        CHECK(areaOf(Json::array({{{"type", "slot"}, {"start", {0, 0}}, {"end", {centres, 0}}, {"width", w}}})) ==
              Approx(expected).epsilon(1e-9));
        CHECK(areaOf(Json::array({{{"type", "slot"}, {"mode", "overall"}, {"start", {-3, 0}}, {"end", {23, 0}}, {"width", w}}})) ==
              Approx(expected).epsilon(1e-9));
        CHECK(areaOf(Json::array({{{"type", "slot"}, {"mode", "center_point"}, {"center", {10, 0}}, {"end", {20, 0}},
                                   {"width", w}}})) == Approx(expected).epsilon(1e-9));
        CHECK_FALSE(s.execute("sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "slot"}, {"mode", "overall"}, {"start", {0, 0}},
                                                        {"end", {5, 0}}, {"width", 6}}})}}).ok);
    }
}

TEST_CASE("Design commands: circle tangent to two or three lines (CAD-270)", "[commands][cad][solver]")
{
    commands::Session s;
    // A triangle with legs 30 and 40 (hypotenuse 50): its incircle has radius (30 + 40 - 50) / 2 = 10.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {40, 0}}},
                                                 {{"type", "line"}, {"start", {40, 0}}, {"end", {0, 30}}},
                                                 {{"type", "line"}, {"start", {0, 30}}, {"end", {0, 0}}}})}})
                        .at("id").get<std::uint64_t>();

    SECTION("3-tangent: the circle nearest the click is the incircle, and it stays tangent")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {10, 10}}}})}});
        Json c = run(s, "sketch.curves", {{"sketch", id}});
        const auto& circle = c.at("curves")[3];
        REQUIRE(circle.at("kind") == "circle");
        CHECK(circle.at("radius").get<double>() == Approx(10).epsilon(1e-9));
        CHECK(circle.at("points")[0][0].get<double>() == Approx(10).epsilon(1e-9));
        CHECK(circle.at("points")[0][1].get<double>() == Approx(10).epsilon(1e-9));
        CHECK(c.at("degreesOfFreedom").get<int>() == 6); // the 3 lines (6 DOF) are free, the circle is fully constrained
        // Stretching the base keeps the circle tangent to all three lines.
        run(s, "sketch.add_constraints",
            {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"points", Json::array({{{"curve", 2}, {"point", "end"}}})}},
                                                         {{"type", "length"}, {"curve", 0}, {"value", 60}}})}});
        c = run(s, "sketch.curves", {{"sketch", id}});
        const auto& moved = c.at("curves")[3];
        const double r = moved.at("radius").get<double>();
        const auto& ctr = moved.at("points")[0];
        // Distance from the centre to each side (as an infinite line) equals the radius.
        const double cx = ctr[0].get<double>(), cy = ctr[1].get<double>();
        for (int i = 0; i < 3; ++i) {
            const auto& pts = c.at("curves")[static_cast<std::size_t>(i)].at("points");
            const double ax = pts[0][0].get<double>(), ay = pts[0][1].get<double>();
            const double bx = pts[1][0].get<double>(), by = pts[1][1].get<double>();
            const double dist = std::abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) / std::hypot(bx - ax, by - ay);
            CHECK(dist == Approx(r).epsilon(1e-6));
        }
        const auto& base = c.at("curves")[0].at("points");
        CHECK(std::hypot(base[1][0].get<double>() - base[0][0].get<double>(), base[1][1].get<double>() - base[0][1].get<double>()) ==
              Approx(60).epsilon(1e-6));
    }

    SECTION("an excircle is picked by clicking outside")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {60, 60}}}})}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        CHECK(c.at("curves")[3].at("radius").get<double>() > 10.5);
    }

    SECTION("2-tangent: the click picks the wedge and the size; a typed radius fixes it")
    {
        const Json a = run(s, "sketch.add_curves",
                           {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 2}}, {"near", {5, 5}}}})}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        const auto& circle = c.at("curves")[3];
        // The corner is the origin, both lines are axes: the centre on the bisector nearest (5, 5).
        CHECK(circle.at("points")[0][0].get<double>() == Approx(5).epsilon(1e-9));
        CHECK(circle.at("points")[0][1].get<double>() == Approx(5).epsilon(1e-9));
        CHECK(circle.at("radius").get<double>() == Approx(5).epsilon(1e-9));
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 2}}, {"near", {-3, 3}}, {"radius", 4}}})}});
        const Json d = run(s, "sketch.curves", {{"sketch", id}});
        CHECK(d.at("curves")[4].at("radius").get<double>() == Approx(4).epsilon(1e-9));
        CHECK(d.at("curves")[4].at("points")[0][0].get<double>() == Approx(-4).epsilon(1e-9));
        (void)a;
    }

    SECTION("2-tangent between parallel lines: half the gap, centred on the midline nearest the click")
    {
        const auto par = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {50, 0}}},
                                                      {{"type", "line"}, {"start", {0, 12}}, {"end", {50, 12}}}})}})
                             .at("id").get<std::uint64_t>();
        run(s, "sketch.add_curves",
            {{"sketch", par}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {20, 3}}}})}});
        const Json c = run(s, "sketch.curves", {{"sketch", par}});
        const auto& k = c.at("curves")[2];
        CHECK(k.at("radius").get<double>() == Approx(6).epsilon(1e-9));
        CHECK(k.at("points")[0][0].get<double>() == Approx(20).epsilon(1e-9));
        CHECK(k.at("points")[0][1].get<double>() == Approx(6).epsilon(1e-9));
        CHECK_FALSE(s.execute("sketch.add_curves",
                              {{"sketch", par}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {20, 3}}, {"radius", 3}}})}}).ok);
    }

    SECTION("splines and points are not accepted")
    {
        run(s, "sketch.add_curves", {{"sketch", id}, {"curves", Json::array({{{"type", "spline"}, {"points", {{100, 0}, {110, 10}, {120, 0}}}}})}});
        CHECK_FALSE(s.execute("sketch.add_curves",
                              {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 3}}, {"near", {5, 5}}}})}}).ok);
    }
}

TEST_CASE("Design commands: circle tangent to arcs and circles (CAD-273)", "[commands][cad][solver]")
{
    commands::Session s;
    // Circle A (centre (0,0), R 10), circle B (centre (40,0), R 6) and a line y = -30.
    const auto id = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 10}},
                                                 {{"type", "circle"}, {"center", {40, 0}}, {"radius", 6}},
                                                 {{"type", "line"}, {"start", {-60, -30}}, {"end", {100, -30}}}})}})
                        .at("id").get<std::uint64_t>();
    auto last = [&]() {
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        return c.at("curves").back();
    };
    auto ctr = [](const Json& c) { return std::pair<double, double>{c.at("points")[0][0].get<double>(), c.at("points")[0][1].get<double>()}; };

    SECTION("3-tangent: two circles and a line, below between them")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {20, -20}}}})}});
        const Json c = last();
        REQUIRE(c.at("kind") == "circle");
        const double r = c.at("radius").get<double>();
        const auto [x, y] = ctr(c);
        CHECK(std::abs(y + 30) == Approx(r).epsilon(1e-6));                    // touches the line
        CHECK(std::hypot(x, y) == Approx(10 + r).epsilon(1e-6));               // outside A
        CHECK(std::hypot(x - 40, y) == Approx(6 + r).epsilon(1e-6));           // outside B
        CHECK(x > 0);
        CHECK(x < 40);
    }

    SECTION("3-tangent: a circle around both circles, touching them from inside")
    {
        // Hugging A and B from outside: the circle contains both and touches the line y = -30 from above.
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {20, 20}}}})}});
        const Json c = last();
        const double r = c.at("radius").get<double>();
        const auto [x, y] = ctr(c);
        CHECK(std::abs(y + 30) == Approx(r).epsilon(1e-6));
        CHECK(std::hypot(x, y) == Approx(std::abs(r - 10)).epsilon(1e-6));
        CHECK(std::hypot(x - 40, y) == Approx(std::abs(r - 6)).epsilon(1e-6));
    }

    SECTION("2-tangent with a radius: touching a circle and a line")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 2}}, {"near", {-30, -5}}, {"radius", 25}}})}});
        const Json c = last();
        CHECK(c.at("radius").get<double>() == Approx(25).epsilon(1e-9));
        const auto [x, y] = ctr(c);
        CHECK(std::abs(y + 30) == Approx(25).epsilon(1e-6));
        CHECK(std::hypot(x, y) == Approx(35).epsilon(1e-6));
        CHECK(x < 0);
    }

    SECTION("2-tangent without a radius: through the click, touching two circles")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {20, 12}}}})}});
        const Json c = last();
        const double r = c.at("radius").get<double>();
        const auto [x, y] = ctr(c);
        CHECK(std::hypot(x - 20, y - 12) == Approx(r).epsilon(1e-6)); // passes through the click
        const double dA = std::hypot(x, y), dB = std::hypot(x - 40, y);
        CHECK((std::abs(dA - (10 + r)) < 1e-6 || std::abs(dA - std::abs(r - 10)) < 1e-6));
        CHECK((std::abs(dB - (6 + r)) < 1e-6 || std::abs(dB - std::abs(r - 6)) < 1e-6));
    }

    SECTION("large sketches: the solution does not depend on the model's size (Codex review)")
    {
        const auto big = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "circle"}, {"center", {-100000, 0}}, {"radius", 1000}},
                                                      {{"type", "circle"}, {"center", {100000, 0}}, {"radius", 1000}},
                                                      {{"type", "circle"}, {"center", {0, 100000}}, {"radius", 1000}}})}})
                             .at("id").get<std::uint64_t>();
        run(s, "sketch.add_curves",
            {{"sketch", big}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {0, 0}}}})}});
        const Json c = run(s, "sketch.curves", {{"sketch", big}}).at("curves").back();
        const double r = c.at("radius").get<double>();
        const auto [x, y] = ctr(c);
        CHECK(r == Approx(99000).epsilon(1e-6)); // two equally near circles about the origin: the smaller one
        CHECK(x == Approx(0).margin(1e-3));
        CHECK(y == Approx(0).margin(1e-3));
    }

    SECTION("concentric circles with a radius: the centre nearest the click on the valid locus (Codex review)")
    {
        const auto con = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 10}},
                                                      {{"type", "circle"}, {"center", {0, 0}}, {"radius", 30}}})}})
                             .at("id").get<std::uint64_t>();
        run(s, "sketch.add_curves",
            {{"sketch", con},
             {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {25, 0}}, {"radius", 10}}})}});
        const Json c = run(s, "sketch.curves", {{"sketch", con}}).at("curves").back();
        const auto [x, y] = ctr(c);
        CHECK(c.at("radius").get<double>() == Approx(10).epsilon(1e-9));
        CHECK(x == Approx(20).epsilon(1e-6));
        CHECK(y == Approx(0).margin(1e-6));
        // A radius that cannot touch both is still an error.
        CHECK_FALSE(s.execute("sketch.add_curves",
                              {{"sketch", con},
                               {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {25, 0}}, {"radius", 3}}})}})
                        .ok);
    }

    SECTION("it stays tangent when a circle is resized")
    {
        run(s, "sketch.add_curves",
            {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {20, -20}}}})}});
        run(s, "sketch.add_constraints",
            {{"sketch", id}, {"constraints", Json::array({{{"type", "radius"}, {"curve", 0}, {"value", 12}}})}});
        // The solver is free to move everything: check tangency against the curves as they are now.
        const Json all = run(s, "sketch.curves", {{"sketch", id}});
        const Json& cs = all.at("curves");
        const Json& c = cs[3];
        const double r = c.at("radius").get<double>();
        const auto [x, y] = ctr(c);
        const auto [ax, ay] = ctr(cs[0]);
        CHECK(cs[0].at("radius").get<double>() == Approx(12).epsilon(1e-6));
        CHECK(std::hypot(x - ax, y - ay) == Approx(12 + r).epsilon(1e-4));
        const double lx0 = cs[2].at("points")[0][0].get<double>(), ly0 = cs[2].at("points")[0][1].get<double>();
        const double lx1 = cs[2].at("points")[1][0].get<double>(), ly1 = cs[2].at("points")[1][1].get<double>();
        const double len = std::hypot(lx1 - lx0, ly1 - ly0);
        CHECK(std::abs((lx1 - lx0) * (y - ly0) - (ly1 - ly0) * (x - lx0)) / len == Approx(r).epsilon(1e-4));
        const auto [bx, by] = ctr(cs[1]);
        CHECK(std::hypot(x - bx, y - by) == Approx(cs[1].at("radius").get<double>() + r).epsilon(1e-4));
    }

    SECTION("a radius with three curves, or no tangent circle, is an error")
    {
        CHECK_FALSE(s.execute("sketch.add_curves",
                              {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1, 2}}, {"near", {20, -20}}, {"radius", 3}}})}}).ok);
        // A radius too small to reach two circles 40 apart.
        CHECK_FALSE(s.execute("sketch.add_curves",
                              {{"sketch", id}, {"curves", Json::array({{{"type", "tangent_circle"}, {"curves", {0, 1}}, {"near", {20, 0}}, {"radius", 0.1}}})}}).ok);
    }
}

TEST_CASE("Design commands: Scale, uniform and non-uniform (CAD-271)", "[commands][cad]")
{
    commands::Session s;
    const auto sk = run(s, "sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 20}, {"height", 10}}})}})
                        .at("id").get<std::uint64_t>();
    const auto ex = run(s, "feature.extrude", {{"sketch", sk}, {"distance", 5}});
    const auto bodyId = ex.at("bodies")[0].at("id").get<std::uint64_t>();

    SECTION("non-uniform: each axis has its own factor, about the box centre by default")
    {
        const auto r = run(s, "feature.scale", {{"body", bodyId}, {"factor_x", 2}, {"factor_z", 0.5}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(20 * 10 * 5 * 2 * 0.5).epsilon(1e-6));
        const auto b = s.design().bodies().at(bodyId).shape.bounds();
        CHECK(b.max.x - b.min.x == Approx(40).epsilon(1e-6));
        CHECK(b.max.y - b.min.y == Approx(10).epsilon(1e-6));
        CHECK(b.max.z - b.min.z == Approx(2.5).epsilon(1e-6));
        CHECK((b.max.x + b.min.x) / 2 == Approx(10).epsilon(1e-6)); // the centre stayed
    }

    SECTION("uniform about a given centre")
    {
        run(s, "feature.scale", {{"body", bodyId}, {"factor", 3}, {"center", {0, 0, 0}}});
        const auto b = s.design().bodies().at(bodyId).shape.bounds();
        CHECK(b.min.x == Approx(0).margin(1e-6));
        CHECK(b.max.x == Approx(60).epsilon(1e-6));
        CHECK(b.max.z == Approx(15).epsilon(1e-6));
    }

    SECTION("a zero or mixed request is refused")
    {
        CHECK_FALSE(s.execute("feature.scale", {{"body", bodyId}, {"factor", 0}}).ok);
        CHECK_FALSE(s.execute("feature.scale", {{"body", bodyId}, {"factor", 2}, {"factor_x", 2}}).ok);
    }
}

TEST_CASE("Design commands: Pattern on Path (CAD-271)", "[commands][cad]")
{
    commands::Session s;
    // A 20 x 4 x 4 bar at (50, 0), and a quarter-circle path (centre origin, radius 50) leaving it
    // upwards, plus a straight path along X.
    const auto bar = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "rectangle"}, {"corner", {50, -2}}, {"width", 20}, {"height", 4}}})}})
                         .at("id").get<std::uint64_t>();
    const auto bodyId = run(s, "feature.extrude", {{"sketch", bar}, {"distance", 4}}).at("bodies")[0].at("id").get<std::uint64_t>();
    const auto arc = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "center_arc"}, {"center", {0, 0}}, {"radius", 50},
                                                   {"startAngle", 0}, {"endAngle", 90}}})}})
                         .at("id").get<std::uint64_t>();
    const auto line = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "line"}, {"start", {0, 20}}, {"end", {100, 20}}}})}})
                          .at("id").get<std::uint64_t>();

    SECTION("identical orientation: copies are only translated; the whole path is the default extent")
    {
        const auto r = run(s, "feature.path_pattern",
                           {{"body", bodyId}, {"path", {{"sketch", arc}}}, {"count", 2}, {"operation", "new_body"}});
        REQUIRE(r.at("bodies").size() == 2);
        const auto b = s.design().bodies().at(r.at("bodies")[1].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.x == Approx(0).margin(1e-6));
        CHECK(b.max.x == Approx(20).margin(1e-6));
        CHECK(b.min.y == Approx(48).margin(1e-5));
        CHECK(b.max.y == Approx(52).margin(1e-5));
    }

    SECTION("path direction: the copy turns with the path")
    {
        const auto r = run(s, "feature.path_pattern",
                           {{"body", bodyId}, {"path", {{"sketch", arc}}}, {"count", 2}, {"orientation", "path_direction"},
                            {"operation", "new_body"}});
        const auto b = s.design().bodies().at(r.at("bodies")[1].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.x == Approx(-2).margin(1e-5));
        CHECK(b.max.x == Approx(2).margin(1e-5));
        CHECK(b.min.y == Approx(50).margin(1e-5));
        CHECK(b.max.y == Approx(70).margin(1e-5));
    }

    SECTION("spacing along a straight path joins the copies into the body")
    {
        const auto r = run(s, "feature.path_pattern",
                           {{"body", bodyId}, {"path", {{"sketch", line}}}, {"count", 3}, {"distance_type", "spacing"},
                            {"distance", 30}});
        CHECK(r.at("bodies").size() == 1);
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(3 * 20 * 4 * 4).epsilon(1e-6));
        const auto b = s.design().bodies().at(bodyId).shape.bounds();
        CHECK(b.max.x == Approx(70 + 60).epsilon(1e-6));
    }

    SECTION("a closed path spreads the instances evenly, none on top of the first")
    {
        const auto ring = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 50}}})}})
                              .at("id").get<std::uint64_t>();
        const auto r = run(s, "feature.path_pattern",
                           {{"body", bodyId}, {"path", {{"sketch", ring}}}, {"count", 4}, {"operation", "new_body"}});
        // Four instances in all: the body and three copies in one new body.
        REQUIRE(r.at("bodies").size() == 2);
        const double single = s.design().bodies().at(bodyId).shape.volume();
        CHECK(r.at("bodies")[1].at("volume").get<double>() == Approx(3 * single).epsilon(1e-6));
    }

    SECTION("spacing needs a distance; count must be whole")
    {
        CHECK_FALSE(s.execute("feature.path_pattern", {{"body", bodyId}, {"path", {{"sketch", line}}}, {"count", 3}, {"distance_type", "spacing"}}).ok);
        CHECK_FALSE(s.execute("feature.path_pattern", {{"body", bodyId}, {"path", {{"sketch", line}}}, {"count", 2.5}}).ok);
    }

    SECTION("running off the path is an error")
    {
        CHECK_FALSE(s.execute("feature.path_pattern",
                              {{"body", bodyId}, {"path", {{"sketch", line}}}, {"count", 5}, {"distance_type", "spacing"},
                               {"distance", 30}}).ok);
        CHECK_FALSE(s.execute("feature.path_pattern", {{"body", bodyId}, {"path", {{"sketch", line}}}, {"count", 1}}).ok);
    }
}

TEST_CASE("Design commands: Pipe (CAD-271)", "[commands][cad]")
{
    commands::Session s;
    const auto line = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {100, 0}}}})}})
                          .at("id").get<std::uint64_t>();
    const auto arc = run(s, "sketch.create",
                         {{"plane", "XY"},
                          {"curves", Json::array({{{"type", "center_arc"}, {"center", {0, 0}}, {"radius", 50},
                                                   {"startAngle", 0}, {"endAngle", 90}}})}})
                         .at("id").get<std::uint64_t>();
    const double pi = 3.14159265358979323846;

    SECTION("a solid round pipe along a line")
    {
        const auto r = run(s, "feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 10}});
        REQUIRE(r.at("bodies").size() == 1);
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi * 25 * 100).epsilon(1e-6));
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.x == Approx(0).margin(1e-6));
        CHECK(b.max.x == Approx(100).margin(1e-6));
        CHECK(b.max.y - b.min.y == Approx(10).epsilon(1e-6));
        CHECK(b.max.z - b.min.z == Approx(10).epsilon(1e-6));
    }

    SECTION("a hollow tube has a wall")
    {
        const auto r = run(s, "feature.pipe", {{"path", {{"sketch", line}}}, {"radius", 5}, {"thickness", 2}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi * (25 - 9) * 100).epsilon(1e-6));
    }

    SECTION("a square section, solid and hollow")
    {
        const auto a = run(s, "feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 10}, {"section", "square"}});
        CHECK(a.at("bodies")[0].at("volume").get<double>() == Approx(10 * 10 * 100).epsilon(1e-6));
        const auto b = run(s, "feature.pipe",
                           {{"path", {{"sketch", line}}}, {"diameter", 10}, {"section", "square"}, {"thickness", 2}});
        CHECK(b.at("bodies").back().at("volume").get<double>() == Approx((100 - 36) * 100).epsilon(1e-6));
    }

    SECTION("a hollow pipe follows a curved path and a closed one")
    {
        const auto r = run(s, "feature.pipe", {{"path", {{"sketch", arc}}}, {"diameter", 10}, {"thickness", 2}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi * (25 - 9) * (pi / 2 * 50)).epsilon(1e-3));
        CHECK(geometry::isValid(s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape));
        const auto ring = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 40}}})}})
                              .at("id").get<std::uint64_t>();
        const auto t = run(s, "feature.pipe", {{"path", {{"sketch", ring}}}, {"diameter", 10}, {"thickness", 2}});
        CHECK(t.at("bodies").back().at("volume").get<double>() == Approx(pi * (25 - 9) * (2 * pi * 40)).epsilon(1e-3));
    }

    SECTION("it follows a curved path")
    {
        const auto r = run(s, "feature.pipe", {{"path", {{"sketch", arc}}}, {"diameter", 10}});
        // Pappus: section area times the path length of its centre.
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi * 25 * (pi / 2 * 50)).epsilon(1e-4));
    }

    SECTION("deleting sketch curves keeps the pipe on its own curve (Codex review of #238)")
    {
        const auto two = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "line"}, {"start", {0, 30}}, {"end", {20, 30}}},
                                                      {{"type", "line"}, {"start", {0, 60}}, {"end", {50, 60}}}})}})
                             .at("id").get<std::uint64_t>();
        run(s, "feature.pipe", {{"path", {{"sketch", two}, {"curves", {1}}}}, {"diameter", 10}});
        auto status = [&] {
            const Json tl = run(s, "timeline.list");
            for (const auto& f : tl.at("features"))
                if (f.at("type") == "pipe")
                    return f.at("status").get<std::string>();
            return std::string("missing");
        };
        auto pipeVolume = [&] {
            const auto bodies = run(s, "timeline.list").at("bodies");
            return bodies.empty() ? -1.0 : bodies[0].at("volume").get<double>();
        };
        REQUIRE(pipeVolume() == Approx(pi * 25 * 50).epsilon(1e-6));
        run(s, "sketch.delete_curves", {{"sketch", two}, {"curves", {0}}});
        CHECK(status() == "ok");
        CHECK(pipeVolume() == Approx(pi * 25 * 50).epsilon(1e-6)); // still the 50 long line
        run(s, "sketch.delete_curves", {{"sketch", two}, {"curves", {0}}});
        CHECK(status() == "error");
    }

    SECTION("bad sizes are errors")
    {
        CHECK_FALSE(s.execute("feature.pipe", {{"path", {{"sketch", line}}}}).ok);
        CHECK_FALSE(s.execute("feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 0}}).ok);
        CHECK_FALSE(s.execute("feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 10}, {"thickness", 5}}).ok);
        CHECK_FALSE(s.execute("feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 10}, {"section", "hex"}}).ok);
        CHECK_FALSE(s.execute("feature.pipe", {{"path", {{"sketch", line}}}, {"diameter", 10}, {"radius", 5}}).ok);
    }
}

TEST_CASE("Design commands: Rib (CAD-271)", "[commands][cad]")
{
    commands::Session s;
    const auto line = run(s, "sketch.create",
                          {{"plane", "XY"},
                           {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {50, 0}}}})}})
                          .at("id").get<std::uint64_t>();
    const double pi = 3.14159265358979323846;

    SECTION("normal: the thickened curve stands up from the sketch plane")
    {
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 10}});
        REQUIRE(r.at("bodies").size() == 1);
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx((50 * 4) * 10).epsilon(1e-4));
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.z == Approx(0).margin(1e-6));
        CHECK(b.max.z == Approx(10).margin(1e-6));
        CHECK(b.min.x == Approx(0).margin(1e-5));
        CHECK(b.max.x == Approx(50).margin(1e-5));
    }

    SECTION("flip stands it down")
    {
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 10}, {"flip", true}});
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.z == Approx(-10).margin(1e-6));
        CHECK(b.max.z == Approx(0).margin(1e-6));
    }

    SECTION("parallel: the wall reaches along the plane, thickness centred on it")
    {
        const auto r = run(s, "feature.rib",
                           {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 10}, {"direction", "parallel"}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(50 * 10 * 4).epsilon(1e-6));
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.min.y == Approx(0).margin(1e-6));
        CHECK(b.max.y == Approx(10).margin(1e-6));
        CHECK(b.min.z == Approx(-2).margin(1e-6));
        CHECK(b.max.z == Approx(2).margin(1e-6));
        const auto f = run(s, "feature.rib",
                           {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 10}, {"direction", "parallel"}, {"flip", true}});
        const auto bf = s.design().bodies().at(f.at("bodies").back().at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(bf.max.y == Approx(0).margin(1e-6));
        CHECK(bf.min.y == Approx(-10).margin(1e-6));
    }

    SECTION("a curved rib follows the curve")
    {
        const auto arc = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "center_arc"}, {"center", {0, 0}}, {"radius", 50},
                                                       {"startAngle", 0}, {"endAngle", 90}}})}})
                             .at("id").get<std::uint64_t>();
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", arc}}}, {"thickness", 4}, {"depth", 10}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi / 4 * (52 * 52 - 48 * 48) * 10).epsilon(1e-3));
    }

    SECTION("a closed curve gives a ring wall")
    {
        const auto ring = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 30}}})}})
                              .at("id").get<std::uint64_t>();
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", ring}}}, {"thickness", 4}, {"depth", 10}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(pi * (32 * 32 - 28 * 28) * 10).epsilon(1e-4));
        // A parallel rib needs a direction, which a closed curve does not have.
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", ring}}}, {"thickness", 4}, {"depth", 10}, {"direction", "parallel"}}).ok);
    }

    SECTION("to next body: the wall grows until it reaches the body (Fusion: Rib, To Next)")
    {
        run(s, "feature.box", {{"corner", {-10, -10, 30}}, {"size", {80, 30, 5}}});
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"extent", "to_object"},
                                              {"to", {{"body", "Body1"}}}, {"operation", "new_body"}});
        const auto id = r.at("bodies").back().at("id").get<std::uint64_t>();
        CHECK(s.design().bodies().at(id).shape.volume() == Approx(50 * 4 * 30).epsilon(1e-4));
        CHECK(s.design().bodies().at(id).shape.bounds().max.z == Approx(30).margin(1e-6));
        // Nothing to reach: an error. A missing target is an error too.
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"extent", "to_object"},
                                              {"to", {{"body", "Body1"}}}, {"flip", true}, {"operation", "new_body"}}).ok);
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"extent", "to_object"}}).ok);
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"extent", "far"}, {"depth", 5}}).ok);
    }

    SECTION("a web: curves that cross or stand apart are walls of one body (Fusion: Web)")
    {
        const auto cross = run(s, "sketch.create",
                               {{"plane", "XY"},
                                {"curves", Json::array({{{"type", "line"}, {"start", {-25, 0}}, {"end", {25, 0}}},
                                                        {{"type", "line"}, {"start", {0, -25}}, {"end", {0, 25}}}})}})
                               .at("id").get<std::uint64_t>();
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", cross}}}, {"thickness", 4}, {"depth", 10}});
        REQUIRE(r.at("bodies").size() == 1);
        // Two 50 x 4 walls share a 4 x 4 square: 2 * 2000 - 160.
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(2 * 50 * 4 * 10 - 4 * 4 * 10).epsilon(1e-4));
        const auto apart = run(s, "sketch.create",
                               {{"plane", "XY"},
                                {"curves", Json::array({{{"type", "line"}, {"start", {100, 0}}, {"end", {120, 0}}},
                                                        {{"type", "line"}, {"start", {100, 30}}, {"end", {140, 30}}}})}})
                               .at("id").get<std::uint64_t>();
        const auto w = run(s, "feature.rib", {{"path", {{"sketch", apart}}}, {"thickness", 2}, {"depth", 5}, {"direction", "parallel"}});
        CHECK(w.at("bodies").back().at("volume").get<double>() == Approx((20 + 40) * 5 * 2).epsilon(1e-4));
        // A web to the next body: both walls reach the plate above.
        run(s, "feature.box", {{"corner", {90, -10, 12}}, {"size", {60, 60, 3}}});
        const auto up = run(s, "feature.rib", {{"path", {{"sketch", apart}}}, {"thickness", 2}, {"extent", "to_object"},
                                               {"to", {{"body", "Body3"}}}, {"operation", "new_body"}});
        CHECK(up.at("bodies").back().at("volume").get<double>() == Approx((20 + 40) * 2 * 12).epsilon(1e-4));
        // Two joined lines plus a separate one: the separate wall must not be dropped.
        const auto mixed = run(s, "sketch.create",
                               {{"plane", "XY"},
                                {"curves", Json::array({{{"type", "line"}, {"start", {200, 0}}, {"end", {220, 0}}},
                                                        {{"type", "line"}, {"start", {220, 0}}, {"end", {220, 20}}},
                                                        {{"type", "line"}, {"start", {200, 50}}, {"end", {240, 50}}}})}})
                               .at("id").get<std::uint64_t>();
        const auto m = run(s, "feature.rib", {{"path", {{"sketch", mixed}}}, {"thickness", 2}, {"depth", 5}, {"operation", "new_body"}});
        CHECK(m.at("bodies").back().at("volume").get<double>() == Approx((20 + 20 + 40) * 2 * 5).epsilon(0.03));
    }

    SECTION("to next body: a picked face follows the body when it is edited")
    {
        const auto plate = run(s, "feature.box", {{"corner", {-10, -10, 30}}, {"size", {80, 30, 5}}}).at("id").get<std::uint64_t>();
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"extent", "to_object"},
                                              {"to", {{"body", plate}, {"faces", {{{"point", {25, 0, 30}}}}}}}, {"operation", "new_body"}});
        const auto id = r.at("bodies").back().at("id").get<std::uint64_t>();
        CHECK(s.design().bodies().at(id).shape.bounds().max.z == Approx(30).margin(1e-6));
        // The plate moves down by 10: its underside, the picked face, is now at z = 20.
        run(s, "feature.edit", {{"id", plate}, {"params", {{"corner", {-10, -10, 20}}}}});
        CHECK(s.design().bodies().at(id).shape.bounds().max.z == Approx(20).margin(1e-6));
    }

    SECTION("to next body, parallel: the wall reaches along the plane up to the body")
    {
        run(s, "feature.box", {{"corner", {-10, 25, -10}}, {"size", {70, 10, 20}}});
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"direction", "parallel"},
                                              {"extent", "to_object"}, {"to", {{"body", "Body1"}}}, {"operation", "new_body"}});
        const auto id = r.at("bodies").back().at("id").get<std::uint64_t>();
        CHECK(s.design().bodies().at(id).shape.volume() == Approx(50 * 25 * 4).epsilon(1e-4));
        const auto b = s.design().bodies().at(id).shape.bounds();
        CHECK(b.max.y == Approx(25).margin(1e-6));
        CHECK(b.min.z == Approx(-2).margin(1e-6));
    }

    SECTION("to next body, parallel, along a bent path (several faces)")
    {
        const auto bent = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {30, 0}}},
                                                       {{"type", "line"}, {"start", {30, 0}}, {"end", {50, 10}}}})}})
                              .at("id").get<std::uint64_t>();
        run(s, "feature.box", {{"corner", {-10, 40, -10}}, {"size", {80, 10, 20}}});
        const auto r = run(s, "feature.rib", {{"path", {{"sketch", bent}}}, {"thickness", 4}, {"direction", "parallel"},
                                              {"extent", "to_object"}, {"to", {{"body", "Body1"}}}, {"operation", "new_body"}});
        const auto id = r.at("bodies").back().at("id").get<std::uint64_t>();
        const auto b = s.design().bodies().at(id).shape.bounds();
        CHECK(b.max.y == Approx(40).margin(1e-6));
        CHECK(b.min.y == Approx(0).margin(1e-6));
        CHECK(s.design().bodies().at(id).shape.volume() > 4 * 40 * 30);
    }

    SECTION("bad values are errors")
    {
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 0}, {"depth", 10}}).ok);
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 0}}).ok);
        CHECK_FALSE(s.execute("feature.rib", {{"path", {{"sketch", line}}}, {"thickness", 4}, {"depth", 10}, {"direction", "up"}}).ok);
    }
}

TEST_CASE("Design commands: Coil (CAD-271)", "[commands][cad]")
{
    commands::Session s;
    const double pi = 3.14159265358979323846;
    // Outer diameter 30, wire 4: helix radius 13. Section area times the length of the centroid path.
    const double sectionArea = pi * 2 * 2;

    SECTION("revolutions and pitch")
    {
        const auto r = run(s, "feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8}});
        REQUIRE(r.at("bodies").size() == 1);
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(sectionArea * 2 * pi * 13 * 5).epsilon(2e-3));
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(geometry::isValid(s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape));
        // (Bounding boxes of swept surfaces are not tight, so only the start is checked exactly.)
        CHECK(b.max.x - b.min.x >= 30 - 1e-3);
        CHECK(b.min.z == Approx(-2).margin(1e-3));
        CHECK(b.max.z >= 40 + 2 - 1e-3);
    }

    SECTION("height and pitch give the turns; height and revolutions give the pitch")
    {
        const auto a = run(s, "feature.coil", {{"diameter", 30}, {"section_size", 4}, {"height", 40}, {"pitch", 8}});
        CHECK(a.at("bodies")[0].at("volume").get<double>() == Approx(sectionArea * 2 * pi * 13 * 5).epsilon(2e-3));
        const auto b = run(s, "feature.coil", {{"diameter", 30}, {"section_size", 4}, {"height", 40}, {"revolutions", 5}});
        CHECK(b.at("bodies").back().at("volume").get<double>() == Approx(sectionArea * 2 * pi * 13 * 5).epsilon(2e-3));
    }

    SECTION("about a construction axis or an {origin, direction} axis: the coil starts on it")
    {
        const Json ax = run(s, "feature.axis", {{"from", {100, 50, 0}}, {"to", {100, 50, 10}}});
        const auto r = run(s, "feature.coil",
                           {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8}, {"axis", {{"feature", ax.at("id")}}}});
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(0.5 * (b.min.x + b.max.x) == Approx(100).margin(0.5));
        CHECK(0.5 * (b.min.y + b.max.y) == Approx(50).margin(0.5));
        CHECK(b.min.z == Approx(-2).margin(1e-3));
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(sectionArea * 2 * pi * 13 * 5).epsilon(2e-3));
        const auto g = run(s, "feature.coil",
                           {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8},
                            {"axis", {{"origin", {0, 0, 0}}, {"direction", {1, 0, 0}}}}, {"operation", "new_body"}});
        const auto gb = s.design().bodies().at(g.at("bodies").back().at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(gb.max.x >= 40 + 2 - 1e-3);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8},
                                               {"axis", {{"feature", 9999}}}}).ok);
        // An object that names no axis, or a zero direction, is an error instead of the Z axis.
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8},
                                               {"axis", {{"line", 2}}}}).ok);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8},
                                               {"axis", {{"origin", {0, 0, 0}}, {"direction", {0, 0, 0}}}}}).ok);
        // An explicit `center` places the coil's start there, whatever the axis origin.
        const auto c = run(s, "feature.coil",
                           {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8}, {"center", {0, 0, 0}},
                            {"axis", {{"feature", ax.at("id")}}}, {"operation", "new_body"}});
        const auto cb = s.design().bodies().at(c.at("bodies").back().at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(0.5 * (cb.min.x + cb.max.x) == Approx(0).margin(0.5));
    }

    SECTION("a square wire, a left-handed coil about X")
    {
        const auto r = run(s, "feature.coil",
                           {{"diameter", 30}, {"section_size", 4}, {"revolutions", 3}, {"pitch", 8}, {"section", "square"},
                            {"handedness", "left"}, {"axis", "X"}});
        CHECK(r.at("bodies")[0].at("volume").get<double>() == Approx(16 * 2 * pi * 13 * 3).epsilon(2e-3));
        const auto b = s.design().bodies().at(r.at("bodies")[0].at("id").get<std::uint64_t>()).shape.bounds();
        CHECK(b.max.x - b.min.x == Approx(24 + 4).margin(1e-2));
    }

    SECTION("a tight pitch still gives a valid solid or a clear error")
    {
        const auto r = s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 4.5}});
        if (r.ok)
            CHECK(geometry::isValid(s.design().bodies().at(r.data.at("bodies")[0].at("id").get<std::uint64_t>()).shape));
        else
            CHECK_FALSE(r.error.empty());
    }

    SECTION("bad values are errors")
    {
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}}).ok);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8}, {"height", 40}}).ok);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 3}}).ok);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 3}, {"section_size", 4}, {"revolutions", 5}, {"pitch", 8}}).ok);
        CHECK_FALSE(s.execute("feature.coil", {{"diameter", 30}, {"section_size", 4}, {"revolutions", 0}, {"pitch", 8}}).ok);
    }
}

TEST_CASE("Design commands: Intersect a body with a sketch plane (CAD-274)", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.cylinder", {{"center", {30, 0, 0}}, {"radius", 10}, {"height", 15}});
    const auto box = run(s, "feature.box", {{"corner", {-20, -5, 0}}, {"size", {10, 10, 10}}}).at("id").get<std::uint64_t>();
    const double pi = 3.14159265358979323846;

    SECTION("a box cut by a plane gives its four edges, fixed, and a profile")
    {
        const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"offset", 5}}).at("id").get<std::uint64_t>();
        run(s, "sketch.intersect", {{"sketch", id}, {"body", box}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 4);
        for (const auto& curve : c.at("curves"))
            CHECK(curve.at("kind") == "line");
        CHECK(c.at("degreesOfFreedom") == 0);
        const auto profiles = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() == Approx(100).epsilon(1e-6));
    }

    SECTION("a cylinder cut across gives a circle; along its axis, a rectangle")
    {
        const auto top = run(s, "sketch.create", {{"plane", "XY"}, {"offset", 7}}).at("id").get<std::uint64_t>();
        run(s, "sketch.intersect", {{"sketch", top}, {"body", "Body1"}});
        const Json c = run(s, "sketch.curves", {{"sketch", top}});
        REQUIRE(c.at("curves").size() == 1);
        CHECK(c.at("curves")[0].at("kind") == "circle");
        CHECK(c.at("curves")[0].at("radius").get<double>() == Approx(10).margin(1e-6));
        const auto side = run(s, "sketch.create", {{"plane", "XZ"}, {"offset", 0}}).at("id").get<std::uint64_t>();
        run(s, "sketch.intersect", {{"sketch", side}, {"body", "Body1"}});
        const auto profiles = run(s, "sketch.profiles", {{"sketch", side}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() == Approx(20 * 15).epsilon(1e-6));
    }

    SECTION("a slanted plane through a cylinder gives a closed curve (an ellipse)")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", {{"origin", {30, 0, 7.5}}, {"normal", {0.5, 0, std::sqrt(3.0) / 2}}}}})
                            .at("id").get<std::uint64_t>();
        run(s, "sketch.intersect", {{"sketch", id}, {"body", "Body1"}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 1);
        CHECK(c.at("curves")[0].at("kind") == "spline");
        const auto profiles = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() == Approx(pi * 100 / (std::sqrt(3.0) / 2)).epsilon(5e-3));
    }

    SECTION("a plane that misses the body is an error")
    {
        const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"offset", 100}}).at("id").get<std::uint64_t>();
        CHECK_FALSE(s.execute("sketch.intersect", {{"sketch", id}, {"body", box}}).ok);
    }
}

TEST_CASE("Design commands: arc slot (CAD-273)", "[commands][cad]")
{
    commands::Session s;
    const double pi = 3.14159265358979323846;
    auto make = [&](const Json& item) {
        return run(s, "sketch.create", {{"plane", "XY"}, {"curves", Json::array({item})}}).at("id").get<std::uint64_t>();
    };

    SECTION("a quarter turn: an annular sector with round ends")
    {
        const auto id = make({{"type", "arc_slot"}, {"center", {0, 0}}, {"radius", 30}, {"width", 10},
                              {"start_angle", 0}, {"end_angle", 90}});
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 4);
        for (const auto& curve : c.at("curves"))
            CHECK(curve.at("kind") == "arc");
        const auto profiles = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() == Approx(2 * (pi / 2) * 30 * 5 + pi * 25).epsilon(1e-6));
    }

    SECTION("an arc through the x axis (start above end modulo 360)")
    {
        const auto id = make({{"type", "arc_slot"}, {"center", {5, 5}}, {"radius", 20}, {"width", 4},
                              {"start_angle", 300}, {"end_angle", 30}});
        const auto profiles = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() == Approx(2 * (pi / 2) * 20 * 2 + pi * 4).epsilon(1e-6));
    }

    SECTION("it stays a slot when the solver runs (a dimension is added)")
    {
        const auto id = make({{"type", "arc_slot"}, {"center", {0, 0}}, {"radius", 30}, {"width", 10},
                              {"start_angle", 0}, {"end_angle", 90}});
        run(s, "sketch.add_constraints",
            {{"sketch", id}, {"constraints", Json::array({{{"type", "radius"}, {"curve", 0}, {"value", 40}}})}});
        const auto profiles = run(s, "sketch.profiles", {{"sketch", id}}).at("profiles");
        REQUIRE(profiles.size() == 1);
        CHECK(profiles[0].at("area").get<double>() > 0);
    }

    SECTION("bad values are errors")
    {
        for (const Json& item : {Json{{"type", "arc_slot"}, {"center", {0, 0}}, {"radius", 4}, {"width", 10}, {"start_angle", 0}, {"end_angle", 90}},
                                 Json{{"type", "arc_slot"}, {"center", {0, 0}}, {"radius", 30}, {"width", 0}, {"start_angle", 0}, {"end_angle", 90}},
                                 Json{{"type", "arc_slot"}, {"center", {0, 0}}, {"radius", 30}, {"width", 5}, {"start_angle", 0}, {"end_angle", 360}}})
            CHECK_FALSE(s.execute("sketch.create", {{"plane", "XY"}, {"curves", Json::array({item})}}).ok);
    }
}

TEST_CASE("Design commands: control point spline (CAD-274)", "[commands][cad]")
{
    commands::Session s;
    auto samples = [&](std::uint64_t id) {
        const Json c = run(s, "sketch.curves", {{"sketch", id}});
        REQUIRE(c.at("curves").size() == 1);
        REQUIRE(c.at("curves")[0].at("kind") == "spline");
        return c.at("curves")[0].at("points");
    };

    SECTION("four control points make a cubic Bezier through the end points")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "control_spline"}, {"points", {{0, 0}, {10, 20}, {30, 20}, {40, 0}}}}})}})
                            .at("id").get<std::uint64_t>();
        const Json p = samples(id);
        CHECK(p.front()[0].get<double>() == Approx(0).margin(1e-9));
        CHECK(p.front()[1].get<double>() == Approx(0).margin(1e-9));
        CHECK(p.back()[0].get<double>() == Approx(40).margin(1e-9));
        CHECK(p.back()[1].get<double>() == Approx(0).margin(1e-9));
        // The middle of the Bezier is (P0 + 3 P1 + 3 P2 + P3) / 8 = (20, 15).
        const Json& mid = p[p.size() / 2];
        CHECK(mid[0].get<double>() == Approx(20).margin(1e-6));
        CHECK(mid[1].get<double>() == Approx(15).margin(1e-6));
    }

    SECTION("the curve stays inside the control polygon and does not pass through inner control points")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "control_spline"}, {"points", {{0, 0}, {10, 30}, {20, 0}, {30, 30}, {40, 0}}}}})}})
                            .at("id").get<std::uint64_t>();
        const Json p = samples(id);
        double top = 0;
        for (const auto& q : p)
            top = std::max(top, q[1].get<double>());
        CHECK(top < 30);
        CHECK(top > 5);
    }

    SECTION("a closed control point spline is a closed loop")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "control_spline"}, {"closed", true},
                                                      {"points", {{0, 0}, {40, 0}, {40, 40}, {0, 40}}}}})}})
                            .at("id").get<std::uint64_t>();
        const Json c = run(s, "sketch.curves", {{"sketch", id}}).at("curves")[0];
        CHECK(c.at("kind") == "spline");
        // A uniform periodic cubic B-spline of a square is the rounded square inside it.
        double lo = 1e9, hi = -1e9;
        for (const auto& q : c.at("points")) {
            lo = std::min(lo, q[0].get<double>());
            hi = std::max(hi, q[0].get<double>());
        }
        CHECK(lo > 0);
        CHECK(hi < 40);
    }

    SECTION("too few control points, and a bad degree, are errors")
    {
        CHECK_FALSE(s.execute("sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "control_spline"}, {"points", {{0, 0}, {10, 10}, {20, 0}}}}})}})
                        .ok);
        CHECK(s.execute("sketch.create",
                        {{"plane", "XY"},
                         {"curves", Json::array({{{"type", "control_spline"}, {"degree", 2}, {"points", {{0, 0}, {10, 10}, {20, 0}}}}})}})
                  .ok);
        CHECK_FALSE(s.execute("sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "control_spline"}, {"degree", 9}, {"points", {{0, 0}, {10, 10}, {20, 0}}}}})}})
                        .ok);
    }
}

TEST_CASE("Design commands: sketch stretch (CAD-274)", "[commands][cad]")
{
    commands::Session s;
    auto curves = [&](std::uint64_t id) { return run(s, "sketch.curves", {{"sketch", id}}).at("curves"); };

    SECTION("a window over the top of a rectangle stretches it")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                            .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", id}, {"window", {{-5, 15}, {45, 25}}}, {"dy", 10}});
        double lo = 1e9, hi = -1e9, left = 1e9, right = -1e9;
        for (const auto& c : curves(id))
            for (const auto& q : c.at("points")) {
                lo = std::min(lo, q[1].get<double>());
                hi = std::max(hi, q[1].get<double>());
                left = std::min(left, q[0].get<double>());
                right = std::max(right, q[0].get<double>());
            }
        CHECK(lo == Approx(0).margin(1e-6));
        CHECK(hi == Approx(30).margin(1e-6)); // taller by 10, the bottom stayed
        CHECK(left == Approx(0).margin(1e-6));
        CHECK(right == Approx(40).margin(1e-6)); // as wide as before
    }

    SECTION("a line crossing the window edge keeps its far end")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {50, 0}}}})}})
                            .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", id}, {"window", {{40, -5}, {60, 5}}}, {"dx", 10}});
        const Json p = curves(id)[0].at("points");
        CHECK(p[0][0].get<double>() == Approx(0).margin(1e-6));
        CHECK(p[1][0].get<double>() == Approx(60).margin(1e-6));
    }

    SECTION("curves wholly inside the window move, others stay")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"},
                             {"curves", Json::array({{{"type", "circle"}, {"center", {20, 20}}, {"radius", 5}},
                                                     {{"type", "circle"}, {"center", {100, 20}}, {"radius", 5}}})}})
                            .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", id}, {"window", {{10, 10}, {30, 30}}}, {"dx", 7}, {"dy", -3}});
        const Json c = curves(id);
        CHECK(c[0].at("points")[0][0].get<double>() == Approx(27).margin(1e-6));
        CHECK(c[0].at("points")[0][1].get<double>() == Approx(17).margin(1e-6));
        CHECK(c[0].at("radius").get<double>() == Approx(5).margin(1e-6));
        CHECK(c[1].at("points")[0][0].get<double>() == Approx(100).margin(1e-6));
    }

    SECTION("joined corners, a dimensioned circle and one corner of a rectangle (review)")
    {
        // A corner joined by coincident constraints, with the window over it.
        const auto poly = run(s, "sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {30, 0}}},
                                                       {{"type", "line"}, {"start", {30, 0}}, {"end", {30, 20}}}})},
                               {"constraints", Json::array({{{"type", "coincident"},
                                                             {"points", Json::array({{{"curve", 0}, {"point", "end"}}, {{"curve", 1}, {"point", "start"}}})}}})}})
                              .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", poly}, {"window", {{25, -5}, {35, 5}}}, {"dx", 10}, {"dy", 5}});
        const Json c = curves(poly);
        CHECK(c[0].at("points")[1][0].get<double>() == Approx(40).margin(1e-6));
        CHECK(c[0].at("points")[1][1].get<double>() == Approx(5).margin(1e-6));
        CHECK(c[1].at("points")[0][0].get<double>() == Approx(40).margin(1e-6));
        CHECK(c[1].at("points")[1][1].get<double>() == Approx(20).margin(1e-6)); // the far end stayed

        // A circle with a radius dimension, wholly inside the window.
        const auto ring = run(s, "sketch.create",
                              {{"plane", "XY"}, {"curves", Json::array({{{"type", "circle"}, {"center", {20, 20}}, {"radius", 5}}})},
                               {"constraints", Json::array({{{"type", "radius"}, {"curve", 0}, {"value", 5}}})}})
                              .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", ring}, {"window", {{10, 10}, {30, 30}}}, {"dx", 4}});
        CHECK(curves(ring)[0].at("points")[0][0].get<double>() == Approx(24).margin(1e-6));

        // The window over one corner of a rectangle moves just that corner.
        const auto box = run(s, "sketch.create",
                             {{"plane", "XY"},
                              {"curves", Json::array({{{"type", "rectangle"}, {"corner", {0, 0}}, {"width", 40}, {"height", 20}}})}})
                             .at("id").get<std::uint64_t>();
        run(s, "sketch.stretch", {{"sketch", box}, {"window", {{35, 15}, {45, 25}}}, {"dx", 5}, {"dy", 10}});
        double right = -1e9, top = -1e9, left = 1e9, bottom = 1e9;
        for (const auto& cv : curves(box))
            for (const auto& q : cv.at("points")) {
                right = std::max(right, q[0].get<double>());
                top = std::max(top, q[1].get<double>());
                left = std::min(left, q[0].get<double>());
                bottom = std::min(bottom, q[1].get<double>());
            }
        CHECK(right == Approx(45).margin(1e-6));
        CHECK(top == Approx(30).margin(1e-6));
        CHECK(left == Approx(0).margin(1e-6));
        CHECK(bottom == Approx(0).margin(1e-6));
    }

    SECTION("a window that is not two corners is refused")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {50, 0}}}})}})
                            .at("id").get<std::uint64_t>();
        CHECK_FALSE(s.execute("sketch.stretch", {{"sketch", id}, {"window", {{1}, {2, 3}}}, {"dx", 5}}).ok);
    }

    SECTION("an empty window, and a window held by constraints, are refused")
    {
        const auto id = run(s, "sketch.create",
                            {{"plane", "XY"}, {"curves", Json::array({{{"type", "line"}, {"start", {0, 0}}, {"end", {50, 0}}}})}})
                            .at("id").get<std::uint64_t>();
        CHECK_FALSE(s.execute("sketch.stretch", {{"sketch", id}, {"window", {{100, 100}, {120, 120}}}, {"dx", 5}}).ok);
        run(s, "sketch.add_constraints", {{"sketch", id}, {"constraints", Json::array({{{"type", "fix"}, {"curve", 0}}})}});
        CHECK_FALSE(s.execute("sketch.stretch", {{"sketch", id}, {"window", {{40, -5}, {60, 5}}}, {"dx", 10}}).ok);
        CHECK(curves(id)[0].at("points")[1][0].get<double>() == Approx(50).margin(1e-6));
    }
}

TEST_CASE("Design commands: body.faces and body.edges give axes of cylinders, circles and lines", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.cylinder", {{"center", {30, 0, 0}}, {"radius", 10}, {"height", 20}});
    const auto along = [](const Json& axis, double x, double y) {
        const Json& d = axis.at("direction");
        return axis.at("origin")[0].get<double>() == Approx(x).margin(1e-6) && axis.at("origin")[1].get<double>() == Approx(y).margin(1e-6)
               && std::abs(d[2].get<double>()) == Approx(1.0).margin(1e-6);
    };
    const Json faces = run(s, "body.faces", {{"body", "Body1"}});
    int cylinders = 0;
    for (const auto& f : faces.at("faces")) {
        if (f.at("type") == "cylinder") {
            ++cylinders;
            REQUIRE(f.contains("axis"));
            CHECK(along(f.at("axis"), 30, 0));
        } else {
            CHECK_FALSE(f.contains("axis")); // caps are planes
        }
    }
    CHECK(cylinders == 1);
    const Json edges = run(s, "body.edges", {{"body", "Body1"}});
    int circles = 0, lines = 0;
    for (const auto& e : edges.at("edges")) {
        REQUIRE(e.contains("axis"));
        if (e.at("type") == "circle") {
            ++circles;
            CHECK(along(e.at("axis"), 30, 0)); // through the centre, along the normal
        } else if (e.at("type") == "line") {
            ++lines;
        }
    }
    CHECK(circles == 2);
    CHECK(lines == 1); // the seam
    for (const auto& e : edges.at("edges"))
        CHECK((e.at("type") == "line") == e.value("seam", false)); // only the line is a seam
    // A torus gives its axis too.
    commands::Session t;
    run(t, "feature.torus", {{"center", {5, 6, 7}}, {"diameter", 40}, {"tube_diameter", 10}});
    const Json torusFaces = run(t, "body.faces", {{"body", "Body1"}});
    bool found = false;
    for (const auto& f : torusFaces.at("faces")) {
        if (f.at("type") == "torus") {
            found = true;
            CHECK(f.at("axis").at("origin")[0].get<double>() == Approx(5).margin(1e-6));
            CHECK(f.at("axis").at("origin")[1].get<double>() == Approx(6).margin(1e-6));
            CHECK(std::abs(f.at("axis").at("direction")[2].get<double>()) == Approx(1.0).margin(1e-6));
        }
    }
    CHECK(found);
}

TEST_CASE("Design commands: variable radius fillet runs from the start of the edge to its end", "[commands][cad]")
{
    const auto blendZ = [](commands::Session& s) {
        double sum = 0, weight = 0;
        const Json faces = run(s, "body.faces", {{"body", "Body1"}});
        for (const auto& f : faces.at("faces")) {
            if (f.at("type") == "plane")
                continue;
            sum += f.at("center")[2].get<double>() * f.at("area").get<double>();
            weight += f.at("area").get<double>();
        }
        REQUIRE(weight > 0);
        return sum / weight;
    };
    for (const bool reversed : {false, true}) {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {20, 20, 20}}});
        // The vertical edge at the origin corner; body.edges gives its start and end.
        const Json edges = run(s, "body.edges", {{"body", "Body1"}});
        int index = -1;
        bool startLow = false;
        for (const auto& e : edges.at("edges")) {
            const auto a = e.at("start"), b = e.at("end");
            if (std::abs(a[0].get<double>()) < 1e-9 && std::abs(a[1].get<double>()) < 1e-9 && std::abs(b[0].get<double>()) < 1e-9
                && std::abs(b[1].get<double>()) < 1e-9 && e.at("type") == "line") {
                index = e.at("index").get<int>();
                startLow = a[2].get<double>() < b[2].get<double>();
            }
        }
        REQUIRE(index >= 0);
        // Small radius at the edge start, large at its end: the blend is bigger around the end.
        run(s, "feature.fillet", {{"body", "Body1"}, {"edges", {index}}, {"radius", reversed ? 6 : 1}, {"radius_end", reversed ? 1 : 6}});
        const double z = blendZ(s);
        CHECK(((z > 10.0) == (startLow != reversed)));
    }
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {20, 20, 20}}});
    CHECK_FALSE(s.execute("feature.fillet", {{"body", "Body1"}, {"edges", {0}}, {"radius", 1}, {"radius_end", -2}}).ok);
    CHECK_FALSE(s.execute("feature.fillet", {{"body", "Body1"}, {"edges", {0}}, {"radius", 1}, {"radius_end", 0}}).ok);
}

TEST_CASE("Design commands: variable fillet radius follows body.edges start and end on every edge", "[commands][cad]")
{
    for (int index = 0; index < 12; ++index) {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {20, 20, 20}}});
        const Json edges = run(s, "body.edges", {{"body", "Body1"}});
        const Json& e = edges.at("edges").at(index);
        const auto a = e.at("start"), b = e.at("end");
        run(s, "feature.fillet", {{"body", "Body1"}, {"edges", {index}}, {"radius", 1}, {"radius_end", 6}});
        // The blend face is the only one that is not a plane; its centre lies towards the end of the edge.
        const Json faces = run(s, "body.faces", {{"body", "Body1"}});
        double along = 0;
        int blends = 0;
        for (const auto& f : faces.at("faces")) {
            if (f.at("type") == "plane")
                continue;
            ++blends;
            double dot = 0, len2 = 0;
            for (int k = 0; k < 3; ++k) {
                const double d = b[k].get<double>() - a[k].get<double>();
                dot += (f.at("center")[k].get<double>() - e.at("middle")[k].get<double>()) * d;
                len2 += d * d;
            }
            along = dot / std::sqrt(len2);
        }
        INFO("edge " << index);
        CHECK(blends == 1);
        CHECK(along > 0.5);
    }
}


TEST_CASE("Design commands: thread (Fusion: Create > Thread)", "[commands][cad]")
{
    const auto bodyVolume = [](commands::Session& s, const char* body) {
        const Json bodies = run(s, "timeline.list").at("bodies");
        for (const auto& b : bodies)
            if (b.at("name") == body)
                return b.at("volume").get<double>();
        return -1.0;
    };
    const auto cylinderFace = [](commands::Session& s, const char* body) {
        const Json faces = run(s, "body.faces", {{"body", body}});
        for (const auto& f : faces.at("faces"))
            if (f.at("type") == "cylinder")
                return f.at("index").get<int>();
        return -1;
    };
    SECTION("an outer cylinder is cut inwards and keeps its diameter")
    {
        commands::Session s;
        run(s, "feature.cylinder", {{"center", {0, 0, 0}}, {"radius", 5}, {"height", 20}});
        const double before = bodyVolume(s, "Body1");
        const int face = cylinderFace(s, "Body1");
        REQUIRE(face >= 0);
        run(s, "feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 2}});
        const double after = bodyVolume(s, "Body1");
        // A 1.2 mm deep V groove of about 0.8 * pitch at the surface over ~8 turns removes roughly
        // turns * 2 pi r * (0.5 * (0.8 + 0.1) * 2 * 1.2) mm3 ~ 8 * 31.4 * 1.1 = 280 mm3 (less: it narrows inwards).
        const double removed = before - after;
        INFO("removed " << removed);
        CHECK(removed > 80.0);
        CHECK(removed < 400.0);
        CHECK(run(s, "timeline.list").at("bodies").size() == 1);
    }
    SECTION("a hole is cut outwards, left-handed and flipped threads work too")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        run(s, "feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 8}});
        const double before = bodyVolume(s, "Body1");
        const int face = cylinderFace(s, "Body1");
        REQUIRE(face >= 0);
        run(s, "feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 1.5}, {"handedness", "left"}, {"flip", true}});
        const double removed = before - bodyVolume(s, "Body1");
        INFO("removed " << removed);
        CHECK(removed > 60.0); // the groove cut material from the wall (inwards it would only graze the bore)
        CHECK(removed < 300.0);
        // Right-handed, from the other end and with a length: about the same groove, only shorter.
        commands::Session r;
        run(r, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        run(r, "feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 8}});
        run(r, "feature.thread", {{"body", "Body1"}, {"faces", {cylinderFace(r, "Body1")}}, {"pitch", 1.5}});
        const double right = before - bodyVolume(r, "Body1");
        CHECK(right == Approx(removed).epsilon(0.03)); // mirror images remove the same material
        commands::Session part;
        run(part, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        run(part, "feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 8}});
        run(part, "feature.thread", {{"body", "Body1"}, {"faces", {cylinderFace(part, "Body1")}}, {"pitch", 1.5}, {"length", 6}});
        const double shorter = before - bodyVolume(part, "Body1");
        CHECK(shorter > 20.0);
        CHECK(shorter < 0.65 * right);
    }
    SECTION("a face that is not a cylinder, a missing pitch and a too long pitch are reported")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {10, 10, 10}}});
        CHECK_FALSE(s.execute("feature.thread", {{"body", "Body1"}, {"faces", {0}}, {"pitch", 2}}).ok);
        commands::Session c;
        run(c, "feature.cylinder", {{"center", {0, 0, 0}}, {"radius", 5}, {"height", 4}});
        const int face = cylinderFace(c, "Body1");
        CHECK_FALSE(c.execute("feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 6}}).ok);
        CHECK_FALSE(c.execute("feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 0}}).ok);
        CHECK_FALSE(c.execute("feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 1}, {"length", -2}}).ok);
        CHECK_FALSE(c.execute("feature.thread", {{"body", "Body1"}, {"faces", {face}}, {"pitch", 1}, {"handedness", "sideways"}}).ok);
    }
}

TEST_CASE("Design commands: boundary fill makes a body of the enclosed spaces (Fusion: Boundary Fill)", "[commands][cad]")
{
    const auto volumes = [](commands::Session& s) {
        std::vector<std::pair<std::string, double>> out;
        const Json list = run(s, "timeline.list");
        for (const auto& b : list.at("bodies"))
            out.emplace_back(b.at("name").get<std::string>(), b.at("volume").get<double>());
        return out;
    };
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {20, 20, 20}}});
    run(s, "feature.box", {{"corner", {10, 0, 0}}, {"size", {20, 20, 20}}});
    SECTION("two overlapping boxes give three spaces")
    {
        run(s, "feature.boundary_fill", {{"tools", {"Body1", "Body2"}}});
        const auto v = volumes(s);
        REQUIRE(v.size() == 3);
        CHECK(v.at(2).second == Catch::Approx(12000).epsilon(1e-6)); // 6000 + 2000 + 4000 = all three, one body
    }
    SECTION("a point picks the overlap only")
    {
        run(s, "feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"cells", {{{"point", {15, 10, 10}}}}}});
        const auto v = volumes(s);
        REQUIRE(v.size() == 3);
        CHECK(v.at(2).second == Catch::Approx(10 * 20 * 20).epsilon(1e-6));
    }
    SECTION("the same space picked twice counts once; a negative index is an error")
    {
        run(s, "feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"cells", {{{"point", {15, 10, 10}}}, {{"point", {16, 11, 11}}}}}});
        CHECK(volumes(s).at(2).second == Catch::Approx(10 * 20 * 20).epsilon(1e-6));
        CHECK_FALSE(s.execute("feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"cells", {-1}}}).ok);
    }
    SECTION("remove tools leaves the new body only; a point in no space is an error")
    {
        CHECK_FALSE(s.execute("feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"cells", {{{"point", {100, 0, 0}}}}}}).ok);
        CHECK_FALSE(s.execute("feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"cells", {7}}}).ok);
        run(s, "feature.boundary_fill", {{"tools", {"Body1", "Body2"}}, {"removeTools", true}});
        CHECK(volumes(s).size() == 1);
    }
    CHECK_FALSE(s.execute("feature.boundary_fill", {{"tools", Json::array()}}).ok);
    SECTION("an imported part is accepted by the schema (a plain mesh is then refused for lacking an exact shape)")
    {
        const Json imported = run(s, "document.import", {{"path", std::string(AEROCAD_TEST_DATA_DIR) + "/tetrahedron.stl"}});
        const auto r = s.execute("feature.boundary_fill", {{"tools", {"Body1", {{"part", imported.at("id")}}}}});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("no exact shape") != std::string::npos);
    }
}

TEST_CASE("Design commands: construction axis (Fusion: Construct > Axis)", "[commands][cad]")
{
    const auto close = [](const Json& v, double x, double y, double z) {
        return v.at(0).get<double>() == Approx(x).margin(1e-6) && v.at(1).get<double>() == Approx(y).margin(1e-6) &&
               v.at(2).get<double>() == Approx(z).margin(1e-6);
    };
    commands::Session s;
    SECTION("through two points, and an unusable pair")
    {
        const Json r = run(s, "feature.axis", {{"from", {10, 0, 0}}, {"to", {10, 0, 5}}});
        CHECK(close(r.at("axis").at("origin"), 10, 0, 0));
        CHECK(close(r.at("axis").at("direction"), 0, 0, 1));
        CHECK_FALSE(s.execute("feature.axis", {{"from", {1, 2, 3}}, {"to", {1, 2, 3}}}).ok);
        CHECK_FALSE(s.execute("feature.axis", {{"from", {1, 2, 3}}}).ok);
        CHECK_FALSE(s.execute("feature.axis", {{"plane2", "XY"}}).ok);
        const Json y = run(s, "feature.axis", {{"line", "Y"}});
        CHECK(close(y.at("axis").at("direction"), 0, 1, 0));
        const Json given = run(s, "feature.axis", {{"line", {{"origin", {1, 2, 3}}, {"direction", {0, 0, 2}}}}});
        CHECK(close(given.at("axis").at("origin"), 1, 2, 3));
        CHECK(close(given.at("axis").at("direction"), 0, 0, 1));
        CHECK_FALSE(s.execute("feature.axis", {{"line", "W"}}).ok);
        // A copy of another axis, and `point` does not go with two planes.
        const Json copy = run(s, "feature.axis", {{"line", {{"feature", given.at("id")}}}});
        CHECK(close(copy.at("axis").at("origin"), 1, 2, 3));
        CHECK_FALSE(s.execute("feature.axis", {{"plane", "XY"}, {"plane2", "XZ"}, {"point", {1, 1, 1}}}).ok);
    }
    SECTION("exactly one way to define it")
    {
        CHECK_FALSE(s.execute("feature.axis", Json::object()).ok);
        CHECK_FALSE(s.execute("feature.axis", {{"from", {0, 0, 0}}, {"to", {1, 0, 0}}, {"plane", "XY"}}).ok);
    }
    SECTION("at the intersection of two planes")
    {
        const Json r = run(s, "feature.axis", {{"plane", "XY"}, {"plane2", "XZ"}});
        CHECK(std::abs(r.at("axis").at("direction").at(0).get<double>()) == Approx(1).margin(1e-9));
        CHECK(r.at("axis").at("origin").at(1).get<double>() == Approx(0).margin(1e-9));
        CHECK(r.at("axis").at("origin").at(2).get<double>() == Approx(0).margin(1e-9));
        // Planes offset from the origin: z = 5 and y = 3 meet along the line y = 3, z = 5.
        const Json up = run(s, "feature.plane", {{"base", "XY"}, {"offset", 5}});
        const Json side = run(s, "feature.plane", {{"base", "XZ"}, {"offset", 3}});
        const Json r2 = run(s, "feature.axis", {{"plane", {{"plane", up.at("id")}}}, {"plane2", {{"plane", side.at("id")}}}});
        const Json o = r2.at("axis").at("origin");
        const Json d = r2.at("axis").at("direction");
        CHECK(std::abs(d.at(0).get<double>()) == Approx(1).margin(1e-9));
        CHECK(std::abs(o.at(1).get<double>()) == Approx(3).margin(1e-9));
        CHECK(o.at(2).get<double>() == Approx(5).margin(1e-9));
        CHECK_FALSE(s.execute("feature.axis", {{"plane", "XY"}, {"plane2", "XY"}}).ok);
    }
    SECTION("perpendicular to a plane through a point")
    {
        const Json r = run(s, "feature.axis", {{"plane", "XY"}, {"point", {4, 5, 6}}});
        CHECK(close(r.at("axis").at("origin"), 4, 5, 6));
        CHECK(std::abs(r.at("axis").at("direction").at(2).get<double>()) == Approx(1).margin(1e-9));
        const Json plain = run(s, "feature.axis", {{"plane", "YZ"}});
        CHECK(std::abs(plain.at("axis").at("direction").at(0).get<double>()) == Approx(1).margin(1e-9));
    }
    SECTION("through a straight or circular edge and a cylinder face")
    {
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {10, 20, 30}}});
        // The edge along Y at x = 0, z = 0.
        const Json e = run(s, "feature.axis", {{"edge", {{"body", "Body1"}, {"point", {0, 10, 0}}}}});
        CHECK(std::abs(e.at("axis").at("direction").at(1).get<double>()) == Approx(1).margin(1e-9));
        CHECK(e.at("axis").at("origin").at(0).get<double>() == Approx(0).margin(1e-9));
        run(s, "feature.cylinder", {{"center", {50, 50, 0}}, {"radius", 5}, {"height", 20}});
        const Json c = run(s, "feature.axis", {{"face", {{"body", "Body2"}, {"point", {55, 50, 10}}}}});
        CHECK(c.at("axis").at("origin").at(0).get<double>() == Approx(50).margin(1e-6));
        CHECK(c.at("axis").at("origin").at(1).get<double>() == Approx(50).margin(1e-6));
        CHECK(std::abs(c.at("axis").at("direction").at(2).get<double>()) == Approx(1).margin(1e-9));
        // A flat face has no axis; a missing body is an error.
        CHECK_FALSE(s.execute("feature.axis", {{"face", {{"body", "Body1"}, {"point", {5, 10, 30}}}}}).ok);
        CHECK_FALSE(s.execute("feature.axis", {{"edge", {{"body", "Nope"}, {"index", 0}}}}).ok);
    }
    SECTION("it is a part in the document, saved with the project and not exported or simulated")
    {
        const Json r = run(s, "feature.axis", {{"from", {0, 0, 0}}, {"to", {0, 0, 5}}});
        const auto id = r.at("id").get<std::uint64_t>();
        const model::Part* part = nullptr;
        for (const auto& q : s.document().parts())
            if (q.featureId == id)
                part = &q;
        REQUIRE(part != nullptr);
        CHECK(part->kind == model::PartKind::Axis);
        REQUIRE(part->mesh != nullptr);
        CHECK(part->mesh->triangleCount() == 12);
        const auto file = std::filesystem::temp_directory_path() / ("aerocad_axis_" + std::to_string(std::random_device{}()) + ".aerocad");
        REQUIRE(s.execute("project.save", {{"path", core::pathToUtf8(file)}}).ok);
        commands::Session reopened;
        REQUIRE(reopened.execute("project.open", {{"path", core::pathToUtf8(file)}}).ok);
        bool found = false;
        for (const auto& q : reopened.document().parts())
            found = found || (q.featureId == id && q.kind == model::PartKind::Axis);
        CHECK(found);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        CHECK_FALSE(s.execute("part.duplicate", {{"id", part->id}}).ok);
        CHECK_FALSE(s.execute("simulation.run", {{"id", part->id}}).ok);
    }
    SECTION("revolve, circular pattern and move turn about it")
    {
        // A vertical axis at x = 100.
        const Json ax = run(s, "feature.axis", {{"from", {100, 0, 0}}, {"to", {100, 0, 1}}});
        const auto id = ax.at("id").get<std::uint64_t>();
        run(s, "feature.box", {{"corner", {110, 0, 0}}, {"size", {10, 10, 10}}});
        run(s, "feature.circular_pattern", {{"body", "Body1"}, {"axis", {{"feature", id}}}, {"count", 4}, {"operation", "new_body"}});
        const Json after = run(s, "timeline.list");
        // Four boxes (the original and three copies) in one body; the copies sit round the axis.
        const auto last = after.at("bodies").back().at("bounds");
        CHECK(last.at("min").at(0).get<double>() < 90);
        CHECK(last.at("max").at(0).get<double>() > 109);
        // Move: a quarter turn about the axis by name moves the box from +X of it to +Y of it.
        run(s, "feature.box", {{"corner", {110, -5, 0}}, {"size", {10, 10, 10}}});
        const std::string name = ax.at("name").get<std::string>();
        run(s, "feature.move", {{"body", "Body3"}, {"axis", {{"feature", name}}}, {"angle", 90}});
        const Json moved = run(s, "timeline.list");
        for (const auto& b : moved.at("bodies")) {
            if (b.at("name") != "Body3")
                continue;
            // The box centre (115, 0) is 15 from the axis; a quarter turn puts it at (100, 15).
            const auto mn = b.at("bounds").at("min");
            const auto mx = b.at("bounds").at("max");
            CHECK((mn.at(0).get<double>() + mx.at(0).get<double>()) / 2 == Approx(100).margin(1e-6));
            CHECK((mn.at(1).get<double>() + mx.at(1).get<double>()) / 2 == Approx(15).margin(1e-6));
        }
        // An axis that is not there.
        CHECK_FALSE(s.execute("feature.move", {{"body", "Body3"}, {"axis", {{"feature", 999}}}, {"angle", 90}}).ok);
        // Revolve a profile about it: a 10 x 10 square 10 away from the axis gives a ring of 100 * 2 pi * 15.
        const auto sk = run(s, "sketch.create", {{"plane", "XZ"},
                                                  {"curves", Json::array({{{"type", "rectangle"}, {"corner", {110, 0}}, {"width", 10}, {"height", 10}}})}})
                            .at("id").get<std::uint64_t>();
        const Json rev = run(s, "feature.revolve", {{"sketch", sk}, {"axis", {{"feature", id}}}});
        CHECK(rev.at("bodies").back().at("volume").get<double>() == Approx(100 * 2 * 3.14159265358979 * 15).epsilon(1e-4));
    }
}

TEST_CASE("Design commands: Move with Create Copy leaves the original (Fusion: Move/Copy)", "[commands][cad]")
{
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {10, 10, 10}}});
    run(s, "feature.move", {{"body", "Body1"}, {"translate", {50, 0, 0}}, {"copy", true}});
    const Json list = run(s, "timeline.list");
    REQUIRE(list.at("bodies").size() == 2);
    CHECK(list.at("bodies").at(0).at("bounds").at("min").at(0).get<double>() == Catch::Approx(0).margin(1e-6));
    CHECK(list.at("bodies").at(1).at("bounds").at("min").at(0).get<double>() == Catch::Approx(50).margin(1e-6));
    // Without it the body itself moves.
    run(s, "feature.move", {{"body", "Body1"}, {"translate", {0, 20, 0}}});
    const Json after = run(s, "timeline.list");
    REQUIRE(after.at("bodies").size() == 2);
    CHECK(after.at("bodies").at(0).at("bounds").at("min").at(1).get<double>() == Catch::Approx(20).margin(1e-6));
    CHECK(after.at("bodies").at(1).at("bounds").at("min").at(1).get<double>() == Catch::Approx(0).margin(1e-6));
    // A turned and scaled copy: 2x about its own centre, then turned 90 degrees about Z: still 20 mm wide.
    run(s, "feature.move", {{"body", "Body1"}, {"axis", "Z"}, {"angle", 90}, {"scale", 2}, {"copy", true}});
    const Json three = run(s, "timeline.list");
    REQUIRE(three.at("bodies").size() == 3);
    CHECK(three.at("bodies").at(2).at("volume").get<double>() == Catch::Approx(8000).epsilon(1e-6));
    CHECK(three.at("bodies").at(0).at("volume").get<double>() == Catch::Approx(1000).epsilon(1e-6));
}

TEST_CASE("Design commands: align lays a face of a body on a face of another (Fusion: Align)", "[commands][cad]")
{
    const auto bounds = [](commands::Session& s, const char* name) {
        const Json list = run(s, "timeline.list");
        for (const auto& b : list.at("bodies"))
            if (b.at("name") == name)
                return b.at("bounds");
        return Json();
    };
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 10}}});
    run(s, "feature.box", {{"corner", {100, 100, 100}}, {"size", {10, 10, 10}}});
    SECTION("the bottom face drops onto the top face, same footprint")
    {
        run(s, "feature.align", {{"body", "Body2"}, {"face", {{"point", {105, 105, 100}}}}, {"to", {{"body", "Body1"}, {"point", {20, 20, 10}}}}});
        const Json b = bounds(s, "Body2");
        CHECK(b.at("min").at(2).get<double>() == Catch::Approx(10).margin(1e-6));
        CHECK(b.at("max").at(2).get<double>() == Catch::Approx(20).margin(1e-6));
        CHECK(b.at("min").at(0).get<double>() == Catch::Approx(100).margin(1e-6));
    }
    SECTION("centre and offset")
    {
        run(s, "feature.align", {{"body", "Body2"}, {"face", {{"point", {105, 105, 100}}}}, {"to", {{"body", "Body1"}, {"point", {20, 20, 10}}}},
                                 {"center", true}, {"offset", 2}});
        const Json b = bounds(s, "Body2");
        CHECK(b.at("min").at(2).get<double>() == Catch::Approx(12).margin(1e-6));
        CHECK(b.at("min").at(0).get<double>() == Catch::Approx(15).margin(1e-6));
        CHECK(b.at("max").at(1).get<double>() == Catch::Approx(25).margin(1e-6));
    }
    SECTION("a side face is turned onto the face, and a flipped one faces the same way")
    {
        run(s, "feature.align", {{"body", "Body2"}, {"face", {{"point", {110, 105, 105}}}}, {"to", {{"body", "Body1"}, {"point", {20, 20, 10}}}}});
        const Json b = bounds(s, "Body2");
        CHECK(b.at("min").at(2).get<double>() == Catch::Approx(10).margin(1e-6));
        CHECK(b.at("max").at(2).get<double>() == Catch::Approx(20).margin(1e-6));
        // The +X side face now faces down: it opposes the +Z top face, so the body stands on it. Same volume.
        CHECK(run(s, "timeline.list").at("bodies").at(1).at("volume").get<double>() == Catch::Approx(1000).epsilon(1e-6));
    }
    SECTION("flip: the face looks the same way as the target and the gap counts from it")
    {
        // The small box's top face (+Z) on the big box's top face (+Z), same direction: the box stands on its own
        // top face turned over, so it keeps its orientation and its top face is at z = 10 + 3.
        run(s, "feature.align", {{"body", "Body2"}, {"face", 0}, {"to", {{"body", "Body1"}, {"point", {20, 20, 10}}}}, {"flip", true}, {"offset", 3}});
        const Json faces = run(s, "body.faces", {{"body", "Body2"}});
        bool found = false;
        for (const auto& f : faces.at("faces")) {
            if (f.at("normal").at(2).get<double>() > 0.99) {
                found = true;
                CHECK(f.at("center").at(2).get<double>() == Catch::Approx(13).margin(1e-6));
            }
        }
        CHECK(found);
    }
    SECTION("opposed normals turn half way round; errors are clear")
    {
        run(s, "feature.align", {{"body", "Body2"}, {"face", {{"point", {105, 105, 110}}}}, {"to", {{"body", "Body1"}, {"point", {20, 20, 10}}}}});
        CHECK(bounds(s, "Body2").at("min").at(2).get<double>() == Catch::Approx(10).margin(1e-6));
        CHECK_FALSE(s.execute("feature.align", {{"body", "Body2"}, {"face", {{"point", {105, 105, 110}}}}, {"to", {{"body", "Body2"}, {"point", {105, 105, 100}}}}}).ok);
        CHECK_FALSE(s.execute("feature.align", {{"body", "Body2"}, {"face", {{"body", "Body1"}, {"point", {20, 20, 10}}}}, {"to", "XY"}}).ok);
    }
    SECTION("onto an origin plane")
    {
        run(s, "feature.align", {{"body", "Body2"}, {"face", {{"point", {105, 105, 100}}}}, {"to", "XY"}});
        // The body's bottom face faces down like the plane's back side: the box stands on the XY plane.
        CHECK(bounds(s, "Body2").at("min").at(2).get<double>() == Catch::Approx(0).margin(1e-6));
        CHECK(bounds(s, "Body2").at("max").at(2).get<double>() == Catch::Approx(10).margin(1e-6));
    }
}

TEST_CASE("Design commands: split face keeps one solid and cuts the picked face (Fusion: Split Face)", "[commands][cad]")
{
    const auto faceCount = [](commands::Session& s) {
        const Json faces = run(s, "body.faces", {{"body", "Body1"}});
        return faces.at("faces").size();
    };
    const auto topFace = [](commands::Session& s) {
        const Json faces = run(s, "body.faces", {{"body", "Body1"}, {"point", {20, 20, 20}}});
        return faces.at("nearest").at("index").get<int>();
    };
    SECTION("a plane through the top face adds a face and keeps the volume")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}});
        REQUIRE(faceCount(s) == 6);
        const int face = topFace(s);
        run(s, "feature.split_face", {{"body", "Body1"}, {"faces", {face}}, {"plane", "YZ"}, {"offset", 10}});
        CHECK(faceCount(s) == 7);
        const Json bodies = run(s, "timeline.list").at("bodies");
        REQUIRE(bodies.size() == 1);
        CHECK(bodies.at(0).at("volume").get<double>() == Catch::Approx(32000).margin(0.01));
    }
    SECTION("the two halves of the face can be pressed separately")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}});
        const int face = topFace(s);
        run(s, "feature.split_face", {{"body", "Body1"}, {"faces", {face}}, {"plane", "YZ"}, {"offset", 10}});
        // The half over x > 10 is 30 x 40; pull it 5 mm up.
        const Json half = run(s, "body.faces", {{"body", "Body1"}, {"point", {30, 20, 20}}});
        run(s, "feature.press_pull", {{"body", "Body1"}, {"faces", {half.at("nearest").at("index")}}, {"distance", 5}});
        const Json bodies = run(s, "timeline.list").at("bodies");
        CHECK(bodies.at(0).at("volume").get<double>() == Catch::Approx(32000 + 30 * 40 * 5).margin(0.5));
    }
    SECTION("a body far from the plane's origin is still split")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 5000, 0}}, {"size", {40, 40, 20}}});
        const Json faces = run(s, "body.faces", {{"body", "Body1"}, {"point", {20, 5020, 20}}});
        run(s, "feature.split_face", {{"body", "Body1"}, {"faces", {faces.at("nearest").at("index")}}, {"plane", "YZ"}, {"offset", 10}});
        CHECK(faceCount(s) == 7);
    }
    SECTION("a plane that misses the face is an error")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}});
        const int face = topFace(s);
        CHECK_FALSE(s.execute("feature.split_face", {{"body", "Body1"}, {"faces", {face}}, {"plane", "YZ"}, {"offset", 100}}).ok);
        CHECK_FALSE(s.execute("feature.split_face", {{"body", "Body1"}, {"faces", {face}}, {"plane", "XY"}, {"offset", 20}}).ok);
        CHECK_FALSE(s.execute("feature.split_face", {{"body", "Body1"}, {"faces", Json::array()}}).ok);
        // A plane along the face's own edge only touches it, and a repeated index is the same face.
        CHECK_FALSE(s.execute("feature.split_face", {{"body", "Body1"}, {"faces", {face}}, {"plane", "YZ"}, {"offset", 0}}).ok);
        run(s, "feature.split_face", {{"body", "Body1"}, {"faces", {face, face}}, {"plane", "YZ"}, {"offset", 10}});
        CHECK(faceCount(s) == 7);
    }
}

TEST_CASE("Design commands: extrude extent All cuts through every body (Fusion: Extent All)", "[commands][cad]")
{
    const auto volume = [](commands::Session& s) {
        const Json bodies = run(s, "timeline.list").at("bodies");
        return bodies.at(0).at("volume").get<double>();
    };
    for (const std::string extent : {"all", "all_two_sides"}) {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}});
        // A 10 x 10 square in the XY plane halfway up: the hole goes the whole way down (flip) or both ways.
        const auto id = run(s, "sketch.create",
                            {{"plane", {{"origin", {0, 0, 10}}, {"normal", {0, 0, 1}}, {"xDir", {1, 0, 0}}}},
                             {"curves", Json::array({{{"type", "rectangle"}, {"corner", {15, 15}}, {"width", 10}, {"height", 10}}})}})
                            .at("id").get<std::uint64_t>();
        run(s, "feature.extrude", {{"sketch", id}, {"extent", extent}, {"flip", true}, {"operation", "cut"}, {"bodies", Json::array({"Body1"})}});
        const double expected = extent == "all" ? 32000.0 - 10 * 10 * 10 : 32000.0 - 10 * 10 * 20;
        INFO(extent);
        CHECK(volume(s) == Approx(expected).epsilon(1e-4));
    }
    commands::Session s;
    run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}});
    const auto id = run(s, "sketch.create", {{"plane", "XY"}, {"curves", Json::array({{{"type", "rectangle"}, {"corner", {15, 15}}, {"width", 10}, {"height", 10}}})}})
                        .at("id").get<std::uint64_t>();
    CHECK_FALSE(s.execute("feature.extrude", {{"sketch", id}, {"extent", "everything"}, {"distance", 5}}).ok);
    // Through all makes sense as a cut or an intersection only, and takes no taper.
    CHECK_FALSE(s.execute("feature.extrude", {{"sketch", id}, {"extent", "all"}}).ok);
    CHECK_FALSE(s.execute("feature.extrude", {{"sketch", id}, {"extent", "all"}, {"operation", "join"}}).ok);
    CHECK_FALSE(s.execute("feature.extrude", {{"sketch", id}, {"extent", "all"}, {"operation", "cut"}, {"taper", 3}}).ok);
}

TEST_CASE("Design commands: counterbore and countersink holes (Fusion: Hole type)", "[commands][cad]")
{
    const double pi = 3.14159265358979323846;
    const auto volume = [](commands::Session& s) { return run(s, "timeline.list").at("bodies").at(0).at("volume").get<double>(); };
    const double plain = 30.0 * 30.0 * 12.0;
    SECTION("counterbore")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        run(s, "feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 6}, {"hole_type", "counterbore"},
                                {"cbore_diameter", 10}, {"cbore_depth", 4}});
        // A 6 mm hole through 12 mm plus the extra ring of the 10 mm seat over 4 mm.
        const double expected = plain - pi * 9 * 12 - pi * (25 - 9) * 4;
        CHECK(volume(s) == Approx(expected).epsilon(1e-4));
        CHECK_FALSE(s.execute("feature.hole", {{"body", "Body1"}, {"point", {5, 5, 12}}, {"diameter", 6}, {"hole_type", "counterbore"},
                                               {"cbore_diameter", 5}, {"cbore_depth", 2}}).ok);
        CHECK_FALSE(s.execute("feature.hole", {{"body", "Body1"}, {"point", {5, 5, 12}}, {"diameter", 6}, {"hole_type", "counterbore"},
                                               {"cbore_diameter", 10}}).ok);
    }
    SECTION("countersink")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        run(s, "feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 6}, {"hole_type", "countersink"},
                                {"csink_diameter", 12}});
        // A 90 degree countersink 12 mm wide over a 6 mm hole is a cone 3 mm deep: a frustum minus the hole's share.
        const double rBig = 6, rSmall = 3, h = 3;
        const double frustum = pi * h / 3.0 * (rBig * rBig + rBig * rSmall + rSmall * rSmall);
        const double expected = plain - pi * 9 * 12 - (frustum - pi * 9 * h);
        CHECK(volume(s) == Approx(expected).epsilon(2e-4));
        CHECK_FALSE(s.execute("feature.hole", {{"body", "Body1"}, {"point", {5, 5, 12}}, {"diameter", 6}, {"hole_type", "countersink"},
                                               {"csink_diameter", 12}, {"csink_angle", 178}}).ok);
        CHECK_FALSE(s.execute("feature.hole", {{"body", "Body1"}, {"point", {5, 5, 12}}, {"diameter", 6}, {"hole_type", "slotted"}}).ok);
    }
    SECTION("a tiny hole with a very wide countersink still works (the cone is not cut past its apex)")
    {
        commands::Session s;
        run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {30, 30, 12}}});
        const auto r = s.execute("feature.hole", {{"body", "Body1"}, {"point", {15, 15, 12}}, {"diameter", 0.3}, {"hole_type", "countersink"},
                                                  {"csink_diameter", 1}, {"csink_angle", 174}});
        INFO(r.error);
        CHECK(r.ok);
    }
}

TEST_CASE("Design commands: replace face moves a flat face onto a plane or another body's face (Fusion: Replace Face)", "[commands][cad]")
{
    const auto volumeOf = [](commands::Session& s, std::size_t i = 0) {
        return run(s, "timeline.list").at("bodies").at(i).at("volume").get<double>();
    };
    const auto faceAt = [](commands::Session& s, const char* body, const Json& point) {
        return run(s, "body.faces", {{"body", body}, {"point", point}}).at("nearest").at("index").get<int>();
    };
    const auto makeBox = [](commands::Session& s) { run(s, "feature.box", {{"corner", {0, 0, 0}}, {"size", {40, 40, 20}}}); };
    const Json xyPlane = {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}};

    SECTION("a plane above grows the body up to it")
    {
        commands::Session s;
        makeBox(s);
        run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                                        {"to", {{"plane", xyPlane}, {"offset", 30}}}});
        CHECK(volumeOf(s) == Catch::Approx(40 * 40 * 30.0).margin(0.01));
        CHECK(run(s, "body.faces", {{"body", "Body1"}}).at("faces").size() == 6);
    }
    SECTION("a plane inside cuts the body back to it")
    {
        commands::Session s;
        makeBox(s);
        run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                                        {"to", {{"plane", xyPlane}, {"offset", 12}}}});
        CHECK(volumeOf(s) == Catch::Approx(40 * 40 * 12.0).margin(0.01));
    }
    SECTION("a tilted plane makes a sloped top")
    {
        commands::Session s;
        makeBox(s);
        // z = 21 - 0.1 x crosses the old top (z = 20) at x = 10: it grows on one side and is cut on the other; the mean height is 19.
        run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                                        {"to", {{"plane", {{"origin", {0, 0, 21}}, {"normal", {0.1, 0, 1}}}}}}});
        CHECK(volumeOf(s) == Catch::Approx(40 * 40 * 19.0).margin(0.5));
    }
    SECTION("a face of another body is the tool")
    {
        commands::Session s;
        makeBox(s);
        run(s, "feature.box", {{"corner", {0, 0, 35}}, {"size", {40, 40, 5}}});
        const int toolFace = faceAt(s, "Body2", {20, 20, 35});
        run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                                        {"to", {{"body", "Body2"}, {"faces", {toolFace}}}}});
        CHECK(volumeOf(s, 0) == Catch::Approx(40 * 40 * 35.0).margin(0.01));
        CHECK(volumeOf(s, 1) == Catch::Approx(40 * 40 * 5.0).margin(0.01));
    }
    SECTION("a whole body as the tool: the nearest face of it")
    {
        commands::Session s;
        makeBox(s);
        run(s, "feature.box", {{"corner", {0, 0, 35}}, {"size", {40, 40, 5}}});
        run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}}, {"to", {{"body", "Body2"}}}});
        CHECK(volumeOf(s, 0) == Catch::Approx(40 * 40 * 35.0).margin(0.01));
    }
    SECTION("the tool's face is kept through edits of the tool body")
    {
        commands::Session s;
        makeBox(s);
        run(s, "feature.box", {{"corner", {0, 0, 35}}, {"size", {40, 40, 5}}});
        const auto tool = run(s, "timeline.list").at("bodies").at(1).at("id");
        const Json r = run(s, "feature.replace_face",
                           {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                            {"to", {{"body", "Body2"}, {"faces", Json::array({Json{{"point", {20, 20, 35}}}})}}}});
        const auto& params = s.design().feature(r.at("id").get<cad::FeatureId>())->params;
        CHECK(params.at("to").at("faces").at(0).contains("relative"));
        // Make the tool box taller from below: its bottom face is still the one found.
        run(s, "feature.edit", {{"id", tool}, {"params", {{"corner", {0, 0, 32}}, {"size", {40, 40, 8}}}}});
        CHECK(volumeOf(s, 0) == Catch::Approx(40 * 40 * 32.0).margin(0.01));
    }
    SECTION("the replaced face follows the body when an earlier step moves it")
    {
        commands::Session s;
        makeBox(s);
        const auto box = run(s, "timeline.list").at("bodies").at(0).at("id");
        const Json r = run(s, "feature.replace_face", {{"body", "Body1"}, {"faces", {faceAt(s, "Body1", {20, 20, 20})}},
                                                       {"to", {{"plane", xyPlane}, {"offset", 30}}}});
        REQUIRE(r.contains("id"));
        run(s, "feature.edit", {{"id", box}, {"params", {{"size", {40, 40, 25}}}}});
        CHECK(volumeOf(s) == Catch::Approx(40 * 40 * 30.0).margin(0.01));
    }
    SECTION("errors")
    {
        commands::Session s;
        makeBox(s);
        const int top = faceAt(s, "Body1", {20, 20, 20});
        // A plane the face never reaches, a face that does not exist, no faces, no tool.
        CHECK_FALSE(s.execute("feature.replace_face", {{"body", "Body1"}, {"faces", {top}},
                                                       {"to", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {1, 0, 0}}}}}}}).ok);
        CHECK_FALSE(s.execute("feature.replace_face", {{"body", "Body1"}, {"faces", {99}}, {"to", {{"plane", xyPlane}, {"offset", 30}}}}).ok);
        CHECK_FALSE(s.execute("feature.replace_face", {{"body", "Body1"}, {"faces", Json::array()}, {"to", {{"plane", xyPlane}}}}).ok);
        CHECK_FALSE(s.execute("feature.replace_face", {{"body", "Body1"}, {"faces", {top}}}).ok);
        // Its own faces are not a tool.
        CHECK_FALSE(s.execute("feature.replace_face", {{"body", "Body1"}, {"faces", {top}}, {"to", {{"body", "Body1"}}}}).ok);
        // A curved face is refused with a hint.
        run(s, "feature.cylinder", {{"center", {100, 0, 0}}, {"radius", 10}, {"height", 20}});
        const int side = faceAt(s, "Body2", {110, 0, 10});
        const auto r = s.execute("feature.replace_face", {{"body", "Body2"}, {"faces", {side}}, {"to", {{"plane", xyPlane}, {"offset", 30}}}});
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("curved") != std::string::npos);
        // Nothing was added by the failures.
        CHECK(volumeOf(s) == Catch::Approx(32000).margin(0.01));
    }
}
