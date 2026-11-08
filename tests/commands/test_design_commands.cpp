// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

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
