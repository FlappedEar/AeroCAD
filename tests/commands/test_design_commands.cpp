// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
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
}
