// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using namespace aerocad;
using namespace aerocad::geometry;
using Catch::Approx;

namespace {

SketchCurve line(Vec2 a, Vec2 b)
{
    return {SketchCurve::Kind::Line, {a, b}};
}

std::vector<SketchCurve> rectangle(Vec2 a, Vec2 b)
{
    return {line(a, {b.x, a.y}), line({b.x, a.y}, b), line(b, {a.x, b.y}), line({a.x, b.y}, a)};
}

SketchCurve circle(Vec2 c, double r)
{
    SketchCurve s{SketchCurve::Kind::Circle, {c}};
    s.radius = r;
    return s;
}

} // namespace

TEST_CASE("Sketch: rectangle gives one profile", "[geometry][sketch]")
{
    const auto g = buildSketch(Plane::xy(), rectangle({0, 0}, {40, 20}));
    REQUIRE(g.profiles.size() == 1);
    CHECK(area(g.profiles[0]) == Approx(800.0));
    CHECK_FALSE(g.curves.isNull());
}

TEST_CASE("Sketch: circle inside a rectangle gives a ring and a disk", "[geometry][sketch]")
{
    auto curves = rectangle({0, 0}, {40, 40});
    curves.push_back(circle({20, 20}, 5));
    const auto g = buildSketch(Plane::xy(), curves);
    REQUIRE(g.profiles.size() == 2);
    const double pi = std::numbers::pi;
    double total = area(g.profiles[0]) + area(g.profiles[1]);
    CHECK(total == Approx(1600.0));
    const bool firstIsDisk = std::abs(area(g.profiles[0]) - 25 * pi) < 1e-3;
    const bool secondIsDisk = std::abs(area(g.profiles[1]) - 25 * pi) < 1e-3;
    CHECK(firstIsDisk != secondIsDisk);
}

TEST_CASE("Sketch: overlapping circles are split into three regions", "[geometry][sketch]")
{
    const auto g = buildSketch(Plane::xy(), {circle({0, 0}, 10), circle({10, 0}, 10)});
    CHECK(g.profiles.size() == 3);
}

TEST_CASE("Sketch: open lines and construction curves form no profile", "[geometry][sketch]")
{
    auto curves = rectangle({0, 0}, {10, 10});
    for (auto& c : curves)
        c.construction = true;
    curves.push_back(line({20, 0}, {30, 0}));
    const auto g = buildSketch(Plane::xy(), curves);
    CHECK(g.profiles.empty());
}

TEST_CASE("Sketch: dangling line inside a rectangle is ignored", "[geometry][sketch]")
{
    auto curves = rectangle({0, 0}, {10, 10});
    curves.push_back(line({2, 2}, {5, 5}));
    curves.push_back(line({10, 5}, {15, 5})); // touches the outline but leads nowhere
    const auto g = buildSketch(Plane::xy(), curves);
    REQUIRE(g.profiles.size() == 1);
    CHECK(area(g.profiles[0]) == Approx(100.0));
}

TEST_CASE("Sketch: arc closing a line gives a half disk", "[geometry][sketch]")
{
    std::vector<SketchCurve> curves{line({-10, 0}, {10, 0}),
                                    {SketchCurve::Kind::Arc, {{10, 0}, {0, 10}, {-10, 0}}}};
    const auto g = buildSketch(Plane::xy(), curves);
    REQUIRE(g.profiles.size() == 1);
    CHECK(area(g.profiles[0]) == Approx(50.0 * std::numbers::pi).epsilon(1e-6));
}

TEST_CASE("Sketch on a tilted plane", "[geometry][sketch]")
{
    Plane p{{0, 0, 5}, {1, 1, 0}, {0, 0, 1}};
    const auto g = buildSketch(p, rectangle({0, 0}, {10, 10}));
    REQUIRE(g.profiles.size() == 1);
    CHECK(area(g.profiles[0]) == Approx(100.0));
    const auto solid = extrude(g.profiles[0], p.normalized().normal, 2.0);
    CHECK(solid.volume() == Approx(200.0));
}

TEST_CASE("Extrude: one side, symmetric and negative", "[geometry][modeling]")
{
    const auto g = buildSketch(Plane::xy(), rectangle({0, 0}, {10, 20}));
    const auto a = extrude(g.profiles[0], {0, 0, 1}, 5);
    CHECK(a.volume() == Approx(1000.0));
    CHECK(a.bounds().max.z == Approx(5.0));

    const auto s = extrude(g.profiles[0], {0, 0, 1}, 5, 5);
    CHECK(s.volume() == Approx(2000.0));
    CHECK(s.bounds().min.z == Approx(-5.0));

    const auto n = extrude(g.profiles[0], {0, 0, 1}, -3);
    CHECK(n.volume() == Approx(600.0));
    CHECK(n.bounds().min.z == Approx(-3.0));
    CHECK(isValid(n));
}

TEST_CASE("Revolve a rectangle into a tube", "[geometry][modeling]")
{
    const auto g = buildSketch(Plane::xz(), rectangle({5, 0}, {10, 10}));
    REQUIRE(g.profiles.size() == 1);
    const auto tube = revolve(g.profiles[0], {0, 0, 0}, {0, 0, 1}, 360);
    const double pi = std::numbers::pi;
    CHECK(tube.volume() == Approx(pi * (100 - 25) * 10).epsilon(1e-6));
}

TEST_CASE("Booleans", "[geometry][modeling]")
{
    const auto a = makeBox(10, 10, 10);
    const auto b = makeBox(5, 5, 20);
    CHECK(boolean(a, b, BooleanOp::Cut).volume() == Approx(1000 - 250));
    CHECK(boolean(a, b, BooleanOp::Union).volume() == Approx(1000 + 250));
    CHECK(boolean(a, b, BooleanOp::Intersect).volume() == Approx(250));
    // Union of two touching boxes is merged into one clean box (6 faces).
    CHECK(boolean(makeBox(10, 10, 10), extrude(buildSketch(Plane::xy(), rectangle({10, 0}, {20, 10})).profiles[0],
                                               {0, 0, 1}, 10),
                  BooleanOp::Union)
              .faceCount() == 6);
    CHECK(boolean(b, a, BooleanOp::Cut).volume() == Approx(250));
}

TEST_CASE("Cut removing everything gives an empty shape", "[geometry][modeling]")
{
    const auto r = boolean(makeBox(1, 1, 1), makeBox(5, 5, 5), BooleanOp::Cut);
    CHECK(r.volume() == Approx(0.0));
}

TEST_CASE("Fillet and chamfer edges of a box", "[geometry][modeling]")
{
    const auto box = makeBox(10, 10, 10);
    const auto es = edges(box);
    REQUIRE(es.size() == 12);
    const auto f = fillet(box, {es[0].index}, 2.0);
    const double pi = std::numbers::pi;
    CHECK(f.volume() == Approx(1000 - (4 - pi) * 10).epsilon(1e-6));
    CHECK(isValid(f));
    const auto c = chamfer(box, {es[0].index, es[1].index}, 1.0);
    CHECK(c.volume() < 1000.0);
    CHECK(isValid(c));
    CHECK_THROWS_AS(fillet(box, {99}, 1.0), ModelingError);
    CHECK_THROWS_AS(fillet(box, {0}, 50.0), ModelingError);
}

TEST_CASE("A fillet that does not fit names the largest radius that does", "[geometry][modeling]")
{
    const auto box = makeBox(10, 10, 4);
    std::size_t top = 0;
    for (const auto& e : edges(box))
        if (e.type == "line" && std::abs(e.start.z - 4) < 1e-9 && std::abs(e.end.z - 4) < 1e-9)
            top = e.index;
    try {
        (void)fillet(box, {top}, 20.0);
        FAIL("a 20 mm fillet on a 4 mm high box should fail");
    } catch (const ModelingError& e) {
        const std::string msg = e.what();
        CHECK(msg.find("20 mm radius does not fit this edge") != std::string::npos);
        CHECK(msg.find("the largest that works is about") != std::string::npos);
    }
    try {
        (void)chamfer(box, {top}, 20.0);
        FAIL("a 20 mm chamfer on a 4 mm high box should fail");
    } catch (const ModelingError& e) {
        CHECK(std::string(e.what()).find("the largest that works is about") != std::string::npos);
    }
}

TEST_CASE("Fillet follows a tangent chain", "[geometry][modeling]")
{
    // Rounded vertical corners make the top outline one tangent chain: picking one top edge
    // rounds the whole outline, as Fusion's Tangent chain.
    auto box = makeBox(20, 20, 10);
    std::vector<std::size_t> vertical;
    for (const auto& e : edges(box))
        if (e.type == "line" && std::abs(e.end.z - e.start.z) > 1)
            vertical.push_back(e.index);
    REQUIRE(vertical.size() == 4);
    box = fillet(box, vertical, 3.0);
    std::vector<std::size_t> top;
    for (const auto& e : edges(box))
        if (std::abs(e.start.z - 10) < 1e-6 && std::abs(e.end.z - 10) < 1e-6)
            top.push_back(e.index);
    REQUIRE(top.size() == 8);
    const auto one = fillet(box, {top.front()}, 1.0);
    const auto all = fillet(box, top, 1.0);
    CHECK(isValid(one));
    CHECK(one.volume() == Approx(all.volume()).epsilon(1e-6));
    CHECK(one.volume() < box.volume() - 1.0);
}

TEST_CASE("Chamfer types: two distances and distance with angle", "[geometry][modeling]")
{
    const auto box = makeBox(10, 10, 10);
    const std::size_t e0 = edges(box).front().index;
    ChamferOptions two;
    two.type = ChamferType::TwoDistances;
    two.distance = 1.0;
    two.distance2 = 3.0;
    const auto c2 = chamfer(box, {e0}, two);
    CHECK(isValid(c2));
    CHECK(c2.volume() == Approx(1000 - 0.5 * 1 * 3 * 10).epsilon(1e-6));
    two.flip = true;
    CHECK(chamfer(box, {e0}, two).volume() == Approx(1000 - 15).epsilon(1e-6));

    ChamferOptions da;
    da.type = ChamferType::DistanceAngle;
    da.distance = 2.0;
    da.angle = 30.0;
    const auto ca = chamfer(box, {e0}, da);
    CHECK(isValid(ca));
    CHECK(ca.volume() == Approx(1000 - 0.5 * 2 * 2 * std::tan(std::numbers::pi / 6) * 10).epsilon(1e-6));
    da.angle = 90.0;
    CHECK_THROWS_AS(chamfer(box, {e0}, da), ModelingError);
}

TEST_CASE("Shell a box", "[geometry][modeling]")
{
    const auto box = makeBox(10, 10, 10);
    const auto fs = faces(box);
    std::size_t top = 0;
    for (const auto& f : fs)
        if (f.normal.z > 0.9)
            top = f.index;
    const auto s = shell(box, {top}, 1.0);
    CHECK(s.volume() == Approx(1000 - 8 * 8 * 9).epsilon(1e-6));
}

TEST_CASE("Faces, edges and face planes", "[geometry][modeling]")
{
    const auto box = makeBox(10, 20, 30);
    const auto fs = faces(box);
    REQUIRE(fs.size() == 6);
    bool foundTop = false;
    for (const auto& f : fs) {
        CHECK(f.type == "plane");
        const auto p = facePlane(box, f.index);
        REQUIRE(p.has_value());
        CHECK(p->normal.dot(f.normal) == Approx(1.0));
        if (f.normal.z > 0.9) {
            foundTop = true;
            CHECK(p->origin.z == Approx(30.0));
            CHECK(f.area == Approx(200.0));
        }
    }
    CHECK(foundTop);
    const auto cyl = makeCylinder(5, 10);
    bool hasCylinder = false;
    for (const auto& f : faces(cyl)) {
        if (f.type == "cylinder") {
            hasCylinder = true;
            CHECK_FALSE(facePlane(cyl, f.index).has_value());
        }
    }
    CHECK(hasCylinder);
}

TEST_CASE("Face containment for profile picking", "[geometry][sketch]")
{
    auto curves = rectangle({0, 0}, {40, 40});
    curves.push_back(circle({20, 20}, 5));
    const auto g = buildSketch(Plane::xy(), curves);
    int containing = 0;
    for (const auto& p : g.profiles)
        containing += faceContains(p, {20, 20, 0}) ? 1 : 0;
    CHECK(containing == 1);
}

TEST_CASE("Mesh to B-rep: a closed mesh becomes a solid with merged planar faces", "[geometry][modeling]")
{
    const auto box = makeBox(10, 20, 30);
    const core::TriangleMesh mesh = tessellate(box);
    REQUIRE(mesh.triangleCount() >= 12);
    const auto brep = meshToBrep(mesh);
    CHECK(brep.solidCount() == 1);
    CHECK(brep.volume() == Approx(6000).epsilon(1e-6));
    CHECK(brep.faceCount() == 6); // coplanar triangles merged
    const auto cut = boolean(brep, makeBox(5, 5, 50), BooleanOp::Cut);
    CHECK(cut.volume() == Approx(6000 - 750).epsilon(1e-6));
    CHECK_THROWS_AS(meshToBrep(mesh, 2), ModelingError);
    CHECK_THROWS_AS(meshToBrep(core::TriangleMesh{}), ModelingError);
}

TEST_CASE("Mesh to B-rep: standalone faces next to a closed component are kept", "[geometry][modeling]")
{
    core::TriangleMesh mesh = tessellate(makeBox(10, 10, 10));
    const auto a = mesh.addVertex({50, 0, 0});
    const auto b = mesh.addVertex({60, 0, 0});
    const auto c = mesh.addVertex({50, 10, 0});
    mesh.addTriangle(a, b, c);
    const auto brep = meshToBrep(mesh);
    CHECK(brep.solidCount() == 1);
    CHECK(brep.volume() == Approx(1000).epsilon(1e-6));
    CHECK(brep.faceCount() == 7);
    CHECK(brep.bounds().max.x == Approx(60.0));
}
