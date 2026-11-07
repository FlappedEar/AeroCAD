// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Inspect.hpp"
#include "aerocad/geometry/Modeling.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using namespace aerocad;
using Catch::Approx;

namespace {

core::Polyline square(double x0, double y0, double x1, double y1, double z)
{
    return {{{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}}, true};
}

} // namespace

TEST_CASE("Section caps: region with a hole", "[geometry][inspect]")
{
    const core::CutPlane plane{{0, 0, 5}, {0, 0, 1}};
    const core::TriangleMesh cap = geometry::triangulateRegion({square(0, 0, 10, 10, 5), square(2, 2, 8, 8, 5)}, plane);
    REQUIRE_FALSE(cap.isEmpty());
    CHECK(cap.surfaceArea() == Approx(64));
    for (std::size_t t = 0; t < cap.triangleCount(); ++t)
        CHECK(cap.faceNormal(t).z == Approx(1));

    // Open polylines are ignored.
    core::Polyline open = square(0, 0, 1, 1, 5);
    open.closed = false;
    CHECK(geometry::triangulateRegion({open}, plane).isEmpty());
}

TEST_CASE("Pick and measure sub-shapes", "[geometry][inspect]")
{
    const geometry::Shape box = geometry::makeBox(10, 10, 10);
    const auto face = geometry::pickSubShape(box, {5, 5, 10}, 0.1);
    REQUIRE(face);
    CHECK(face->kind == geometry::SubShape::Kind::Face);
    const auto dir = geometry::subShapeDirection(box, *face);
    REQUIRE(dir);
    CHECK(dir->vector.z == Approx(1));

    const auto vertex = geometry::pickSubShape(box, {0.05, 0, 0}, 0.1);
    REQUIRE(vertex);
    CHECK(vertex->kind == geometry::SubShape::Kind::Vertex);
    CHECK(geometry::vertices(box).size() == 8);

    const auto d = geometry::distance(core::Vec3{5, 5, 20}, box, *face);
    CHECK(d.distance == Approx(10));
    CHECK(geometry::subShapeProperties(box, {}).volume == Approx(1000));
}

TEST_CASE("Mesh distances are refined on the exact faces up and down", "[geometry][inspect]")
{
    const auto box = geometry::makeBox(10, 10, 10);
    const std::vector<core::Vec3> points{{5, 5, 11}, {5, 5, 12}, {5, 5, 9.5}, {5, 5, 13}};
    // Under- and overestimated mesh values (an inscribed triangulation can be either);
    // the last is below the threshold and kept.
    std::vector<double> values{0.9, 3.0, -0.45, 0.01};
    CHECK(geometry::refineShapeDistances(box, points, values, 0.05, 100) == 3);
    CHECK(values[0] == Approx(1.0).margin(1e-6));
    CHECK(values[1] == Approx(2.0).margin(1e-6));
    CHECK(values[2] == Approx(-0.5).margin(1e-6));
    CHECK(values[3] == 0.01);
}

TEST_CASE("Drawn sketch lines become one edge per smooth stretch", "[geometry][inspect]")
{
    // A quarter circle of radius 10 in 30 segments, then a sharp turn into a 5 mm line.
    std::vector<core::Vec3> segs;
    core::Vec3 prev{10, 0, 0};
    for (int i = 1; i <= 30; ++i) {
        const double a = (std::numbers::pi / 2.0) * i / 30.0;
        const core::Vec3 p{10 * std::cos(a), 10 * std::sin(a), 0};
        segs.push_back(prev);
        segs.push_back(p);
        prev = p;
    }
    segs.push_back(prev);
    segs.push_back(prev + core::Vec3{0, -5, 0});
    const geometry::Shape shape = geometry::polylineEdges(segs);
    const auto all = geometry::edges(shape);
    REQUIRE(all.size() == 2);
    double arc = 0.0;
    double line = 0.0;
    for (const auto& e : all)
        (e.length > 6 ? arc : line) = e.length;
    CHECK(arc == Approx(10 * std::numbers::pi / 2).epsilon(0.01));
    CHECK(line == Approx(5));
    CHECK(geometry::distance(core::Vec3{0, 0, 0}, shape, {}).distance == Approx(std::hypot(0.0, 5.0)).epsilon(0.02));
    CHECK_THROWS(geometry::polylineEdges({}));
}
