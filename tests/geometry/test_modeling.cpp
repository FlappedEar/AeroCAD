// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

namespace {

/// Volume of a drafted rectangle a x b extruded by h with taper t (degrees, positive narrows).
double frustum(double a, double b, double h, double t)
{
    const double k = 2.0 * h * std::tan(t * std::numbers::pi / 180.0);
    const double a2 = a - k;
    const double b2 = b - k;
    return h / 3.0 * (a * b + a2 * b2 + std::sqrt(a * b * a2 * b2));
}

} // namespace

TEST_CASE("Extrude with a taper angle", "[geometry][modeling]")
{
    const auto g = buildSketch(Plane::xy(), rectangle({0, 0}, {20, 10}));
    SECTION("positive narrows, negative widens")
    {
        const auto in = extrude(g.profiles[0], {0, 0, 1}, 10, 0, 5);
        CHECK(isValid(in));
        CHECK(in.volume() == Approx(frustum(20, 10, 10, 5)).epsilon(2e-3));
        CHECK(in.bounds().max.z == Approx(10.0).margin(1e-3));
        CHECK(in.bounds().max.x == Approx(20.0).margin(1e-3));
        const auto out = extrude(g.profiles[0], {0, 0, 1}, 10, 0, -5);
        CHECK(out.volume() == Approx(frustum(20, 10, 10, -5)).epsilon(2e-3));
        CHECK(out.bounds().max.x == Approx(20.0 + 10 * std::tan(5 * std::numbers::pi / 180)).margin(1e-2));
    }
    SECTION("negative distance and the back side taper away from the plane")
    {
        const auto down = extrude(g.profiles[0], {0, 0, 1}, -4, 0, 5);
        CHECK(down.bounds().min.z == Approx(-4.0).margin(1e-3));
        CHECK(down.volume() == Approx(frustum(20, 10, 4, 5)).epsilon(2e-3));
        const auto both = extrude(g.profiles[0], {0, 0, 1}, 10, 5, 5);
        CHECK(isValid(both));
        CHECK(both.bounds().min.z == Approx(-5.0).margin(1e-3));
        CHECK(both.volume() == Approx(frustum(20, 10, 10, 5) + frustum(20, 10, 5, 5)).epsilon(2e-3));
    }
    SECTION("against a flipped direction")
    {
        const auto flipped = extrude(g.profiles[0], {0, 0, -1}, 6, 0, 3);
        CHECK(flipped.bounds().min.z == Approx(-6.0).margin(1e-3));
        CHECK(flipped.volume() == Approx(frustum(20, 10, 6, 3)).epsilon(2e-3));
    }
    SECTION("a profile with a hole")
    {
        auto curves = rectangle({0, 0}, {20, 20});
        curves.push_back(circle({10, 10}, 4));
        const auto ring = buildSketch(Plane::xy(), curves);
        REQUIRE(ring.profiles.size() >= 1);
        const auto it = std::find_if(ring.profiles.begin(), ring.profiles.end(),
                                     [](const Shape& f) { return area(f) < 399.0 && area(f) > 300.0; });
        REQUIRE(it != ring.profiles.end());
        const auto solid = extrude(*it, {0, 0, 1}, 5, 0, 2);
        CHECK(isValid(solid));
        // The outside shrinks and the hole grows: about perimeter * h^2 * tan(t) / 2 less each.
        const double t = std::tan(2 * std::numbers::pi / 180);
        const double expected = 5 * area(*it) - (80.0 + 2 * std::numbers::pi * 4) * 25 * t / 2;
        CHECK(solid.volume() == Approx(expected).epsilon(5e-3));
    }
    SECTION("errors")
    {
        CHECK_THROWS_AS(extrude(g.profiles[0], {0, 0, 1}, 10, 0, 89), ModelingError);
        // 10 x 20 closes up after 5 / tan(30) = 8.7 mm.
        CHECK_THROWS_AS(extrude(g.profiles[0], {0, 0, 1}, 20, 0, 30), ModelingError);
    }
    SECTION("closing up is detected on a tilted sketch plane (Codex on PR #142)")
    {
        // 100 x 10 on a plane with normal (1, 0, 1): its world box reaches far along the normal.
        const Plane tilted{{0, 0, 0}, core::Vec3{1, 0, 1}.normalized(), core::Vec3{1, 0, -1}.normalized()};
        const auto t = buildSketch(tilted, rectangle({-50, -5}, {50, 5}));
        CHECK_THROWS_AS(extrude(t.profiles[0], tilted.normal, 20, 0, 30), ModelingError);
        const auto ok = extrude(t.profiles[0], tilted.normal, 5, 0, 30);
        CHECK(isValid(ok));
    }
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

TEST_CASE("Press Pull moves flat faces in and out", "[geometry][modeling]")
{
    const auto box = makeBox(10, 10, 10);
    std::size_t top = 0, side = 0;
    for (const auto& f : faces(box)) {
        if (f.normal.z > 0.9)
            top = f.index;
        if (f.normal.x > 0.9)
            side = f.index;
    }
    const auto pulled = pressPull(box, {top}, 5.0);
    CHECK(isValid(pulled));
    CHECK(pulled.volume() == Approx(1500).epsilon(1e-6));
    CHECK(faces(pulled).size() == 6); // still a box: the new sides merge with the old ones
    const auto pushed = pressPull(box, {top}, -3.0);
    CHECK(pushed.volume() == Approx(700).epsilon(1e-6));
    const auto both = pressPull(box, {top, side}, 2.0);
    CHECK(both.volume() == Approx(1000 + 200 + 200).epsilon(1e-6));
    CHECK_THROWS_AS(pressPull(box, {top}, -10.0), ModelingError);
    CHECK_THROWS_AS(pressPull(box, {top}, 0.0), ModelingError);
    CHECK_THROWS_AS(pressPull(box, {}, 1.0), ModelingError);
    const auto cyl = makeCylinder(5, 10);
    std::size_t round = 0;
    for (const auto& f : faces(cyl))
        if (f.type != "plane")
            round = f.index;
    // A curved face is offset (Fusion's Offset Face): the cylinder grows or shrinks.
    const auto grown = pressPull(cyl, {round}, 1.0);
    CHECK(isValid(grown));
    CHECK(grown.volume() == Approx(std::numbers::pi * 36 * 10).epsilon(1e-4));
    const auto shrunk = pressPull(cyl, {round}, -2.0);
    CHECK(isValid(shrunk));
    CHECK(shrunk.volume() == Approx(std::numbers::pi * 9 * 10).epsilon(1e-4));
    // A curved face picked with a flat one: both move.
    std::size_t lid = 0;
    for (const auto& f : faces(cyl))
        if (f.type == "plane" && f.normal.z > 0.9)
            lid = f.index;
    const auto both2 = pressPull(cyl, {round, lid}, 1.0);
    CHECK(isValid(both2));
    CHECK(both2.volume() == Approx(std::numbers::pi * 36 * 11).epsilon(1e-4));
    CHECK_THROWS_AS(pressPull(cyl, {round}, -6.0), ModelingError); // would turn it inside out
    // A fillet face blends into faces that stay: moving it alone would break the blend.
    const auto rounded = fillet(makeBox(20, 20, 20), {0}, 4.0);
    std::size_t blend = 0;
    for (const auto& f : faces(rounded))
        if (f.type != "plane")
            blend = f.index;
    CHECK_THROWS_WITH(pressPull(rounded, {blend}, 1.0), Catch::Matchers::ContainsSubstring("fillet"));
}

TEST_CASE("Press Pull on a curved face next to a shallow sharp edge", "[geometry][modeling]")
{
    // A cylinder under a cone with a 2 degree draft: the edge between them is sharp (not a
    // fillet), so the cylinder's side can still move; the cone is trimmed to meet it.
    const double top = 10 - 20 * std::tan(2 * std::numbers::pi / 180);
    const auto g = buildSketch(Plane::xz(), {line({0, 0}, {10, 0}), line({10, 0}, {10, 10}), line({10, 10}, {top, 30}),
                                             line({top, 30}, {0, 30}), line({0, 30}, {0, 0})});
    REQUIRE(g.profiles.size() == 1);
    const auto body = revolve(g.profiles[0], {0, 0, 0}, {0, 0, 1}, 360);
    std::size_t side = 0;
    for (const auto& f : faces(body))
        if (f.type == "cylinder")
            side = f.index;
    const auto pushed = pressPull(body, {side}, -0.5);
    CHECK(isValid(pushed));
    CHECK(pushed.volume() < body.volume());
}

TEST_CASE("Press Pull on a curved face with windows cut through it", "[geometry][modeling]")
{
    // The middle of the face's parameter range (the -X side) lies in a window: the curvature check must
    // sample a point on the face itself.
    const auto tube = boolean(makeCylinder(10, 40), translated(makeBox(20, 4, 30), {-20, -2, 5}), BooleanOp::Cut);
    std::size_t round = 0;
    for (const auto& f : faces(tube))
        if (f.type != "plane")
            round = f.index;
    const auto grown = pressPull(tube, {round}, 1.0);
    CHECK(isValid(grown));
    CHECK(grown.volume() > tube.volume());
    CHECK(grown.bounds().size().y == Approx(22).epsilon(1e-4));
}

TEST_CASE("Shell with no open face hollows the solid", "[geometry][modeling]")
{
    // Fusion's closed shell: the outside stays, a cavity of 1 mm walls is inside.
    const auto box = makeBox(10, 10, 10);
    const auto s = shell(box, {}, 1.0);
    CHECK(s.volume() == Approx(1000 - 8 * 8 * 8).epsilon(1e-6));
    CHECK(isValid(s));
}

TEST_CASE("Shell never returns an empty or grown body", "[geometry][modeling]")
{
    // A filleted box gave OCCT an empty result that was kept as an invisible body.
    const auto box = makeBox(40, 30, 6);
    const auto rounded = fillet(box, {0, 1, 2, 3}, 1.0);
    for (const std::vector<std::size_t>& open : {std::vector<std::size_t>{}, std::vector<std::size_t>{0}}) {
        try {
            const auto s = shell(rounded, open, 1.0);
            CHECK(s.volume() > 0.0);
            CHECK(s.volume() < rounded.volume());
            CHECK(isValid(s));
        } catch (const ModelingError&) {
            SUCCEED("reported as a failure");
        }
    }
}

TEST_CASE("Shell a box with a filleted edge (Fusion shells it)", "[geometry][modeling]")
{
    // OCCT cannot open a face tangent to a fillet (it failed for the top and the front of
    // this box) nor make the closed shell of it.
    const auto box = makeBox(40, 30, 20);
    std::size_t frontTop = 0;
    for (const auto& e : edges(box))
        if (std::abs(e.middle.y) < 1e-9 && std::abs(e.middle.z - 20) < 1e-9)
            frontTop = e.index;
    const double r = 6;
    const auto rounded = fillet(box, {frontTop}, r);
    const double corner = 1 - std::numbers::pi / 4; // a square less a quarter disc, per r^2
    REQUIRE(rounded.volume() == Approx(40 * 30 * 20 - 40 * r * r * corner).epsilon(1e-9));
    std::size_t top = 0;
    for (const auto& f : faces(rounded))
        if (f.type == "plane" && f.normal.z > 0.9)
            top = f.index;
    for (const double t : {1.0, 4.0}) {
        // Open at the top: in the section across the fillet, the cavity is the (30-2t) x
        // (20-t) rectangle less the part above the inner fillet (radius r-t), which ends at
        // y = r, z = 20-t; from there a wall face goes straight up to the top.
        const auto open = shell(rounded, {top}, t);
        CHECK(isValid(open));
        const double section = (30 - 2 * t) * (20 - t) - ((r - t) * r - std::numbers::pi * (r - t) * (r - t) / 4);
        CHECK(open.volume() == Approx(rounded.volume() - (40 - 2 * t) * section).epsilon(1e-6));
        // Closed: the cavity is the box shrunk by t with a fillet of radius r-t.
        const auto closed = shell(rounded, {}, t);
        CHECK(isValid(closed));
        const double cavity = (40 - 2 * t) * (30 - 2 * t) * (20 - 2 * t) - (40 - 2 * t) * (r - t) * (r - t) * corner;
        CHECK(closed.volume() == Approx(rounded.volume() - cavity).epsilon(1e-6));
    }
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
TEST_CASE("Offset of a closed spline is one smooth closed spline", "[geometry][sketch]")
{
    // OCCT returns several free-form pieces; interpolated one by one they would kink where
    // they meet.
    SketchCurve oval;
    oval.kind = SketchCurve::Kind::Spline;
    oval.closed = true;
    for (int i = 0; i < 8; ++i) {
        const double a = 2 * std::numbers::pi * i / 8;
        oval.points.push_back({20 * std::cos(a), 15 * std::sin(a)});
    }
    const Plane plane;
    double perimeter = 0;
    for (const auto& e : edges(curveEdges(plane, {oval})))
        perimeter += e.length;
    const double ovalArea = area(buildSketch(plane, {oval}).profiles.at(0));
    for (const double d : {2.0, -2.0}) {
        const auto out = offsetCurves({oval}, d);
        REQUIRE(out.size() == 1);
        CHECK(out[0].kind == SketchCurve::Kind::Spline);
        CHECK(out[0].closed);
        // A convex curve offset by d encloses perimeter * d + pi * d^2 more (Steiner), also
        // for d < 0 while the curvature radius stays above |d|.
        const double offsetArea = area(buildSketch(plane, {out[0]}).profiles.at(0));
        CHECK(offsetArea - ovalArea == Approx(perimeter * d + std::numbers::pi * d * d).epsilon(2e-3));
    }
}
TEST_CASE("Offset side: outwards for closed chains, left of open ones", "[geometry][sketch]")
{
    auto line = [](Vec2 a, Vec2 b) {
        SketchCurve c;
        c.kind = SketchCurve::Kind::Line;
        c.points = {a, b};
        return c;
    };
    auto bounds = [](const std::vector<SketchCurve>& cs) {
        double lo = 1e9, hi = -1e9;
        for (const auto& c : cs)
            for (const auto& q : c.points) {
                lo = std::min(lo, q.x);
                hi = std::max(hi, q.x);
            }
        return std::pair{lo, hi};
    };
    // A single line (OCCT finds no plane for it): the parallel line on the left.
    auto single = offsetCurves({line({0, 0}, {10, 0})}, 2);
    REQUIRE(single.size() == 1);
    CHECK(single[0].points[0].y == Approx(2));
    CHECK(single[0].points[1].y == Approx(2));
    CHECK(offsetCurves({line({0, 0}, {10, 0})}, -2)[0].points[0].y == Approx(-2));
    // An open L (right, then up): left of it is inside the corner.
    for (const auto& c : offsetCurves({line({0, 0}, {10, 0}), line({10, 0}, {10, 10})}, 2))
        for (const auto& q : c.points) {
            CHECK(q.x <= 8 + 1e-9);
            CHECK(q.y >= 2 - 1e-9);
        }
    // A square drawn either way round: positive is outwards, negative inwards.
    for (const bool ccw : {true, false}) {
        std::vector<Vec2> p = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        if (!ccw)
            std::reverse(p.begin(), p.end());
        std::vector<SketchCurve> square;
        for (std::size_t i = 0; i < 4; ++i)
            square.push_back(line(p[i], p[(i + 1) % 4]));
        CHECK(bounds(offsetCurves(square, 2)).first == Approx(-2));
        CHECK(bounds(offsetCurves(square, -2)).first == Approx(2));
    }
}

TEST_CASE("Split Body cuts a body by a plane into two sides", "[geometry][modeling]")
{
    const auto box = makeBox(40, 20, 10);
    const auto [back, front] = splitByPlane(box, {15, 0, 0}, {1, 0, 0});
    CHECK(isValid(back));
    CHECK(isValid(front));
    CHECK(back.volume() == Approx(15 * 20 * 10).epsilon(1e-9));
    CHECK(front.volume() == Approx(25 * 20 * 10).epsilon(1e-9));
    CHECK(back.bounds().max.x == Approx(15));
    CHECK(front.bounds().min.x == Approx(15));
    // A U shape cut across both legs: the front side is two separate pieces.
    const auto u = boolean(box, translated(makeBox(30, 20, 6), {5, 0, 4}), BooleanOp::Cut);
    const auto [base, legs] = splitByPlane(u, {0, 0, 4}, {0, 0, 1});
    CHECK(base.volume() == Approx(40 * 20 * 4).epsilon(1e-9));
    CHECK(legs.volume() == Approx(2 * 5 * 20 * 6).epsilon(1e-9));
    CHECK_THROWS_AS(splitByPlane(box, {50, 0, 0}, {1, 0, 0}), ModelingError);
    CHECK_THROWS_AS(splitByPlane(box, {40, 0, 0}, {1, 0, 0}), ModelingError); // touches a face only
}

TEST_CASE("Delete Face removes a hole and a fillet and heals the body", "[geometry][modeling]")
{
    const auto box = makeBox(40, 20, 10);
    // A through hole: its one cylindrical face goes, the top and bottom close over it.
    const auto drilled = boolean(box, translated(makeCylinder(3, 10), {20, 10, 0}), BooleanOp::Cut);
    std::size_t hole = 0;
    for (const auto& f : faces(drilled))
        if (f.type == "cylinder")
            hole = f.index;
    const auto healed = deleteFaces(drilled, {hole});
    CHECK(isValid(healed));
    CHECK(healed.volume() == Approx(8000).epsilon(1e-6));
    CHECK(faces(healed).size() == 6);
    // A fillet: the two faces it joined meet in a sharp edge again.
    const auto rounded = fillet(box, {0}, 4.0);
    std::size_t blend = 0;
    for (const auto& f : faces(rounded))
        if (f.type != "plane")
            blend = f.index;
    const auto sharp = deleteFaces(rounded, {blend});
    CHECK(isValid(sharp));
    CHECK(sharp.volume() == Approx(8000).epsilon(1e-6));
    // The top of a box: the sides never meet above it, so there is nothing to close it with.
    std::size_t top = 0;
    for (const auto& f : faces(box))
        if (f.normal.z > 0.9)
            top = f.index;
    CHECK_THROWS_AS(deleteFaces(box, {top}), ModelingError);
    CHECK_THROWS_AS(deleteFaces(box, {}), ModelingError);
    CHECK_THROWS_AS(deleteFaces(box, {99}), ModelingError);
}

TEST_CASE("Sphere and torus primitives", "[geometry][modeling]")
{
    const double pi = std::numbers::pi;
    const Shape ball = sphere({1, 2, 3}, 5);
    CHECK(ball.volume() == Approx(4.0 / 3.0 * pi * 125).epsilon(1e-6));
    CHECK(isValid(ball));
    CHECK(ball.bounds().min.x == Approx(-4).margin(0.05));
    CHECK(ball.bounds().max.z == Approx(8).margin(0.05));
    const Shape ring = torus({0, 0, 0}, 20, 5);
    CHECK(isValid(ring));
    CHECK(ring.volume() == Approx(2 * pi * pi * 20 * 25).epsilon(1e-6));
    CHECK(ring.bounds().max.z == Approx(5).margin(0.05));
    CHECK_THROWS_AS(sphere({}, 0), ModelingError);
    CHECK_THROWS_AS(torus({}, 5, 5), ModelingError);
    CHECK_THROWS_AS(torus({}, 5, -1), ModelingError);
}

TEST_CASE("Draft tilts side faces from the fixed plane", "[geometry][modeling]")
{
    // A 20 x 20 x 10 box, the +X side drafted 10 degrees from the bottom (XY) plane, pull +Z:
    // the side leans in, the top loses a 10 * tan(10 deg) wide sliver, the bottom stays.
    const auto box = makeBox(20, 20, 10);
    std::size_t side = 0, top = 0;
    for (const auto& f : faces(box)) {
        if (f.normal.x > 0.9)
            side = f.index;
        if (f.normal.z > 0.9)
            top = f.index;
    }
    const double t = std::tan(10 * std::numbers::pi / 180);
    const auto in = draft(box, {side}, {0, 0, 0}, {0, 0, 1}, 10);
    CHECK(isValid(in));
    CHECK(in.volume() == Approx(4000 - 0.5 * 10 * 10 * t * 20).epsilon(1e-6));
    CHECK(in.bounds().max.x == Approx(20));
    // Outwards: negative angle.
    const auto out = draft(box, {side}, {0, 0, 0}, {0, 0, 1}, -10);
    CHECK(out.volume() == Approx(4000 + 0.5 * 10 * 10 * t * 20).epsilon(1e-6));
    CHECK(out.bounds().max.x == Approx(20 + 10 * t));
    CHECK_THROWS_AS(draft(box, {side}, {0, 0, 0}, {0, 0, 1}, 0), ModelingError);
    CHECK_THROWS_AS(draft(box, {top}, {0, 0, 0}, {0, 0, 1}, 10), ModelingError); // parallel to the plane
    CHECK_THROWS_AS(draft(box, {}, {0, 0, 0}, {0, 0, 1}, 10), ModelingError);
}

TEST_CASE("Spline samples follow the interpolated curve through the fit points", "[geometry][sketch]")
{
    SketchCurve s;
    s.kind = SketchCurve::Kind::Spline;
    s.points = {{0, 0}, {10, 10}, {20, 0}};
    const auto open = sampleSpline(s, 8);
    REQUIRE(open.size() == 2 * 8 + 1);
    CHECK(open.front().x == Approx(0).margin(1e-6));
    CHECK(open.back().x == Approx(20));
    // The fit point in the middle lies on the curve, and the curve bulges past the chords.
    double best = 1e9, top = 0;
    for (const auto& p : open) {
        best = std::min(best, std::hypot(p.x - 10, p.y - 10));
        top = std::max(top, p.y);
    }
    CHECK(best < 0.5);
    CHECK(top >= 10 - 1e-6);

    s.closed = true;
    const auto closed = sampleSpline(s, 8);
    REQUIRE(closed.size() == 3 * 8 + 1);
    CHECK(closed.back().x == Approx(closed.front().x).margin(1e-6));
    CHECK(closed.back().y == Approx(closed.front().y).margin(1e-6));

    SketchCurve line;
    line.points = {{0, 0}, {1, 0}};
    CHECK_THROWS_AS(sampleSpline(line), ModelingError);
}
