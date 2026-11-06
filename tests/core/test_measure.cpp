// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Measure.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Closed tetrahedron with outward-facing triangles, edges of 10 mm along the axes.
TriangleMesh tetrahedron()
{
    TriangleMesh m({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
    return m;
}

} // namespace

TEST_CASE("Closest point on a triangle covers vertex, edge and face regions", "[core][measure]")
{
    const Vec3 a{0, 0, 0}, b{10, 0, 0}, c{0, 10, 0};
    CHECK(closestPointOnTriangle({-5, -5, 3}, a, b, c) == a);
    CHECK(closestPointOnTriangle({20, -1, 0}, a, b, c) == b);
    CHECK(closestPointOnTriangle({5, -3, 0}, a, b, c) == Vec3{5, 0, 0});
    CHECK(closestPointOnTriangle({2, 2, 7}, a, b, c) == Vec3{2, 2, 0});
}

TEST_CASE("Closest point on a mesh, volume and closedness", "[core][measure]")
{
    const TriangleMesh m = tetrahedron();
    const auto hit = closestPointOnMesh(m, {2, 2, -4});
    REQUIRE(hit.has_value());
    CHECK(hit->distance == Approx(4.0));
    CHECK(hit->point == Vec3{2, 2, 0});

    CHECK(enclosedVolume(m) == Approx(1000.0 / 6.0));
    CHECK(openEdgeCount(m) == 0);

    TriangleMesh open({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {{0, 1, 2}});
    CHECK(openEdgeCount(open) == 3);
    CHECK_FALSE(closestPointOnMesh(TriangleMesh{}, {0, 0, 0}).has_value());
}

TEST_CASE("openEdgeCount welds per-face vertices", "[core][measure]")
{
    // The tetrahedron with separate vertices for every face, as B-rep tessellation emits.
    const TriangleMesh shared = tetrahedron();
    TriangleMesh split;
    for (const auto& t : shared.triangles()) {
        const auto a = split.addVertex(shared.vertices()[t[0]]);
        const auto b = split.addVertex(shared.vertices()[t[1]]);
        const auto c = split.addVertex(shared.vertices()[t[2]]);
        split.addTriangle(a, b, c);
    }
    CHECK(split.vertexCount() == 12);
    CHECK(openEdgeCount(split) == 0);
}
