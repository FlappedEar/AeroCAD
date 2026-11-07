// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Inspect.hpp"
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
