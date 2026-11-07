// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/commands/Session.hpp"

#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

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

model::PartId addBrep(commands::Session& s, const std::string& name, const geometry::Shape& shape)
{
    model::Part p;
    p.name = name;
    p.kind = model::PartKind::Body;
    p.shape = shape;
    p.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(shape));
    return s.document().addPart(p);
}

model::PartId addMesh(commands::Session& s, const std::string& name, const geometry::Shape& shape)
{
    model::Part p;
    p.name = name;
    p.mesh = std::make_shared<const core::TriangleMesh>(geometry::tessellate(shape));
    return s.document().addPart(p);
}

} // namespace

TEST_CASE("inspect.measure: points, faces, edges, angle", "[commands][inspect]")
{
    commands::Session s;
    const auto box = addBrep(s, "Box", geometry::makeBox(10, 20, 30));

    const Json pts = run(s, "inspect.measure", {{"a", {{"point", {0, 0, 0}}}}, {"b", {{"point", {3, 4, 0}}}}});
    CHECK(pts["distance"].get<double>() == Approx(5));
    CHECK(pts["delta"][0].get<double>() == Approx(3));

    // Opposite faces of the box (picked at points on them): 10 mm apart, parallel.
    const Json faces = run(s, "inspect.measure",
                           {{"a", {{"part", box}, {"point", {0, 10, 15}}}}, {"b", {{"part", box}, {"point", {10, 10, 15}}}}});
    CHECK(faces["a"]["kind"] == "face");
    CHECK(faces["a"]["type"] == "plane");
    CHECK(faces["a"]["area"].get<double>() == Approx(600));
    CHECK(faces["distance"].get<double>() == Approx(10));
    CHECK(faces["angle"].get<double>() == Approx(180));

    // Adjacent faces: 90 degrees, touching.
    const Json adj = run(s, "inspect.measure",
                         {{"a", {{"part", box}, {"point", {0, 10, 15}}}}, {"b", {{"part", box}, {"point", {5, 0, 15}}}}});
    CHECK(adj["angle"].get<double>() == Approx(90));
    CHECK(adj["distance"].get<double>() == Approx(0).margin(1e-7));

    // A click next to an edge picks the edge; one near a corner the vertex.
    const Json edge = run(s, "inspect.measure", {{"a", {{"part", box}, {"point", {0, 0.05, 15}}, {"tolerance", 0.5}}}});
    CHECK(edge["a"]["kind"] == "edge");
    CHECK(edge["a"]["length"].get<double>() == Approx(30));
    const Json vertex = run(s, "inspect.measure", {{"a", {{"part", box}, {"point", {0.1, 0.1, 30}}, {"tolerance", 0.5}}}});
    CHECK(vertex["a"]["kind"] == "vertex");

    // Point to body.
    const Json body = run(s, "inspect.measure", {{"a", {{"point", {5, 10, 40}}}}, {"b", {{"part", box}, {"pick", "body"}}}});
    CHECK(body["distance"].get<double>() == Approx(10));
    CHECK(body["b"]["volume"].get<double>() == Approx(6000));

    // Cylinder face radius.
    const auto cyl = addBrep(s, "Cyl", geometry::makeCylinder(4, 10));
    const auto shapeBounds = s.document().part(cyl)->mesh->bounds();
    const Json cylFace = run(s, "inspect.measure",
                             {{"a", {{"part", cyl}, {"pick", "face"}, {"point", {shapeBounds.max.x, 0, 5}}}}});
    CHECK(cylFace["a"]["type"] == "cylinder");
    CHECK(cylFace["a"]["radius"].get<double>() == Approx(4));

    const auto r = s.execute("inspect.measure", {{"a", {{"part", 999}}}});
    CHECK_FALSE(r.ok);
}

TEST_CASE("inspect.measure: wall thickness, axis angles and sketch curves", "[commands][inspect]")
{
    commands::Session s;
    const auto box = addBrep(s, "Box", geometry::makeBox(10, 20, 30));

    // A face of a solid: straight through to the opposite wall.
    const Json face = run(s, "inspect.measure", {{"a", {{"part", box}, {"pick", "face"}, {"point", {0, 10, 15}}}}});
    REQUIRE(face.contains("thickness"));
    CHECK(face["thickness"]["value"].get<double>() == Approx(10));
    CHECK(face["thickness"]["pointB"][0].get<double>() == Approx(10));
    const Json top = run(s, "inspect.measure", {{"a", {{"part", box}, {"pick", "face"}, {"point", {5, 10, 30}}}}});
    CHECK(top["thickness"]["value"].get<double>() == Approx(30));
    // Two selections report a distance, not a thickness.
    const Json two = run(s, "inspect.measure",
                         {{"a", {{"part", box}, {"point", {0, 10, 15}}}}, {"b", {{"part", box}, {"point", {10, 10, 15}}}}});
    CHECK_FALSE(two.contains("thickness"));

    // A closed mesh (an STL part): the same on its triangles.
    const auto stl = addMesh(s, "Box STL", geometry::translated(geometry::makeBox(4, 4, 4), {50, 0, 0}));
    const Json meshPoint = run(s, "inspect.measure", {{"a", {{"part", stl}, {"point", {52, 2, 4}}}}});
    REQUIRE(meshPoint.contains("thickness"));
    CHECK(meshPoint["thickness"]["value"].get<double>() == Approx(4).margin(1e-6));

    // An open mesh (a scan) has no wall thickness.
    {
        model::Part sheet;
        sheet.name = "Sheet";
        auto m = std::make_shared<core::TriangleMesh>();
        m->addVertex({100, 0, 0});
        m->addVertex({110, 0, 0});
        m->addVertex({110, 10, 0});
        m->addVertex({100, 10, 0});
        m->addTriangle(0, 1, 2);
        m->addTriangle(0, 2, 3);
        sheet.mesh = m;
        const auto id = s.document().addPart(sheet);
        const Json open = run(s, "inspect.measure", {{"a", {{"part", id}, {"point", {105, 5, 0}}}}});
        CHECK_FALSE(open.contains("thickness"));
    }

    // A hole axis (cylinder face) against faces: parallel to a side face, square to the top.
    const auto cyl = addBrep(s, "Cyl", geometry::translated(geometry::makeCylinder(4, 10), {0, 40, 0}));
    const Json side = run(s, "inspect.measure", {{"a", {{"part", cyl}, {"pick", "face"}, {"point", {4, 40, 5}}}},
                                                 {"b", {{"part", box}, {"pick", "face"}, {"point", {0, 10, 15}}}}});
    CHECK(side["angle"].get<double>() == Approx(0).margin(1e-6));
    const Json square = run(s, "inspect.measure", {{"a", {{"part", cyl}, {"pick", "face"}, {"point", {4, 40, 5}}}},
                                                   {"b", {{"part", box}, {"pick", "face"}, {"point", {5, 10, 30}}}}});
    CHECK(square["angle"].get<double>() == Approx(90));

    // Sketch curves: a circle and a line on XY, measured exactly.
    const auto sk = s.execute("sketch.create",
                              {{"plane", "XY"},
                               {"curves", Json::array({{{"type", "circle"}, {"center", {0, -50}}, {"radius", 5}},
                                                       {{"type", "line"}, {"start", {10, -50}}, {"end", {20, -50}}}})}});
    INFO(sk.toJson().dump());
    REQUIRE(sk.ok);
    model::PartId sketch = 0;
    for (const auto& p : s.document().parts())
        if (p.kind == model::PartKind::Sketch)
            sketch = p.id;
    REQUIRE(sketch != 0);
    const Json circle = run(s, "inspect.measure", {{"a", {{"part", sketch}, {"point", {0, -45.05, 0}}, {"tolerance", 0.5}}}});
    CHECK(circle["a"]["kind"] == "edge");
    CHECK(circle["a"]["type"] == "circle");
    CHECK(circle["a"]["radius"].get<double>() == Approx(5));
    const Json between = run(s, "inspect.measure", {{"a", {{"part", sketch}, {"point", {5, -50, 0}}, {"tolerance", 0.5}}},
                                                    {"b", {{"part", sketch}, {"point", {15, -50, 0}}, {"tolerance", 0.5}}}});
    CHECK(between["b"]["length"].get<double>() == Approx(10));
    CHECK(between["distance"].get<double>() == Approx(5));
    CHECK(between["method"] == "exact");
    // Sketch to solid: from the line end (20, -50, 0) to the box corner (10, 0, 0). (The
    // design replaced the hand-made bodies above when the sketch was added.)
    const auto box2 = addBrep(s, "Box 2", geometry::makeBox(10, 20, 30));
    const Json toBox = run(s, "inspect.measure", {{"a", {{"part", sketch}, {"point", {20, -50, 0}}, {"tolerance", 0.5}}},
                                                  {"b", {{"part", box2}, {"pick", "body"}}}});
    CHECK(toBox["a"]["kind"] == "vertex");
    CHECK(toBox["distance"].get<double>() == Approx(std::hypot(10.0, 50.0)));

    // A whole sketch against a mesh inside its circle: measured to the curves, not the
    // filled disk.
    const auto inside = addMesh(s, "Inside", geometry::translated(geometry::makeBox(2, 2, 2), {-1, -51, -1}));
    const Json whole = run(s, "inspect.measure", {{"a", {{"part", sketch}}}, {"b", {{"part", inside}, {"pick", "body"}}}});
    CHECK(whole["distance"].get<double>() == Approx(5 - std::sqrt(2.0)).epsilon(0.01));
    const Json clearance = run(s, "inspect.clearance", {{"a", sketch}, {"b", inside}});
    CHECK(clearance["distance"].get<double>() == Approx(5 - std::sqrt(2.0)).epsilon(0.01));
}

TEST_CASE("inspect.clearance between B-rep bodies and meshes", "[commands][inspect]")
{
    commands::Session s;
    const auto a = addBrep(s, "A", geometry::makeBox(10, 10, 10));
    const auto b = addBrep(s, "B", geometry::translated(geometry::makeBox(10, 10, 10), {15, 0, 0}));
    const Json gap = run(s, "inspect.clearance", {{"a", a}, {"b", b}});
    CHECK(gap["distance"].get<double>() == Approx(5));
    CHECK(gap["method"] == "exact");
    CHECK(gap["interference"] == false);

    const auto c = addBrep(s, "C", geometry::translated(geometry::makeBox(10, 10, 10), {8, 0, 0}));
    const Json overlap = run(s, "inspect.clearance", {{"a", a}, {"b", c}});
    CHECK(overlap["interference"] == true);
    CHECK(overlap["interferenceVolume"].get<double>() == Approx(200));

    const auto scan = addMesh(s, "Scan", geometry::translated(geometry::makeBox(10, 10, 10), {0, 0, 12}));
    const Json mesh = run(s, "inspect.clearance", {{"a", a}, {"b", scan}});
    CHECK(mesh["distance"].get<double>() == Approx(2));
    CHECK(mesh["method"] == "mesh");
    CHECK(mesh["interference"].is_null());

    CHECK_FALSE(s.execute("inspect.clearance", {{"a", a}, {"b", a}}).ok);

    // A selected B-rep face against a mesh body measures from that face, not the whole part:
    // the x = 0 face of A is 11 mm from a mesh slab starting at x = 11.
    const auto slab = addMesh(s, "Slab", geometry::translated(geometry::makeBox(2, 10, 10), {11, 0, 0}));
    const Json face = run(s, "inspect.measure",
                          {{"a", {{"part", a}, {"pick", "face"}, {"point", {0, 5, 5}}}}, {"b", {{"part", slab}, {"pick", "body"}}}});
    CHECK(face["distance"].get<double>() == Approx(11));
    CHECK(face["pointA"][0].get<double>() == Approx(0));
    const Json edge = run(s, "inspect.measure",
                          {{"a", {{"part", a}, {"pick", "edge"}, {"point", {0, 0, 5}}}}, {"b", {{"part", slab}, {"pick", "body"}}}});
    CHECK(edge["distance"].get<double>() == Approx(11));
}

TEST_CASE("inspect.section compares scan and part sections", "[commands][inspect]")
{
    commands::Session s;
    int notifications = 0;
    s.inspect().setListener([&] { ++notifications; });
    const auto part = addBrep(s, "Part", geometry::makeBox(10, 10, 10));
    const auto scan = addMesh(s, "Scan", geometry::translated(geometry::makeBox(12, 12, 10), {-1, -1, 0}));

    const Json r = run(s, "inspect.section", {{"plane", "xy"}, {"offset", 5}, {"includeCurves", true}});
    CHECK(r["enabled"] == true);
    CHECK(r["normal"][2].get<double>() == Approx(1));
    REQUIRE(r["parts"].size() == 2);
    for (const auto& p : r["parts"]) {
        CHECK(p["closedCurves"].get<int>() == 1);
        const double expected = p["id"].get<model::PartId>() == part ? 100.0 : 144.0;
        CHECK(p["area"].get<double>() == Approx(expected));
        CHECK(p["polylines"][0]["closed"] == true);
    }
    CHECK(s.inspect().section().enabled);
    CHECK(s.inspect().section().cuts(scan));
    CHECK(notifications == 1);

    const Json only = run(s, "inspect.section", {{"origin", {0, 5, 0}}, {"normal", {0, 1, 0}}, {"parts", {part}}, {"flip", true}});
    REQUIRE(only["parts"].size() == 1);
    CHECK(only["normal"][1].get<double>() == Approx(-1));
    CHECK_FALSE(s.inspect().section().cuts(scan));

    const Json st = run(s, "inspect.state");
    CHECK(st["section"]["enabled"] == true);

    run(s, "inspect.section", {{"enabled", false}});
    CHECK_FALSE(s.inspect().section().enabled);
    CHECK_FALSE(s.execute("inspect.section", {{"normal", {0, 0, 0}}}).ok);
}

TEST_CASE("inspect.deviation and inspect.thickness heat maps", "[commands][inspect]")
{
    commands::Session s;
    const auto part = addBrep(s, "Part", geometry::makeBox(10, 10, 10));
    const auto scan = addMesh(s, "Scan", geometry::translated(geometry::makeBox(10, 10, 10.4), {0, 0, -0.2}));

    const Json dev = run(s, "inspect.deviation", {{"measured", scan}, {"reference", part}, {"tolerance", 0.25}});
    CHECK(dev["kind"] == "deviation");
    CHECK(dev["stats"]["max"].get<double>() == Approx(0.2));
    CHECK(dev["stats"]["min"].get<double>() >= -1e-9);
    CHECK(dev["rangeMax"].get<double>() == Approx(0.2));
    CHECK(dev["withinTolerance"].get<double>() == Approx(100));
    const auto& hm = s.inspect().heatMap();
    CHECK(hm.enabled);
    CHECK(hm.part == scan);
    REQUIRE(hm.values);
    CHECK(hm.values->size() == s.document().part(scan)->mesh->vertexCount());

    const Json th = run(s, "inspect.thickness", {{"id", part}});
    CHECK(th["kind"] == "thickness");
    CHECK(th["stats"]["min"].get<double>() == Approx(10).margin(1e-4));
    CHECK(s.inspect().heatMap().kind == commands::HeatMap::Kind::Thickness);

    run(s, "inspect.clear", {{"what", "heatmap"}});
    CHECK_FALSE(s.inspect().heatMap().enabled);
    CHECK_FALSE(s.execute("inspect.deviation", {{"measured", scan}, {"reference", scan}}).ok);

    // Inspect commands never touch the undo history.
    CHECK_FALSE(s.undoStack().canUndo());
}
