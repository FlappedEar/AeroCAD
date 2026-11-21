// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshIntersect.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <utility>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Axis-aligned cube [lo, hi]^3 with outward normals.
TriangleMesh cube(double lo, double hi, const Vec3& shift = {})
{
    std::vector<Vec3> v;
    for (int i = 0; i < 8; ++i)
        v.push_back(Vec3{i & 1 ? hi : lo, i & 2 ? hi : lo, i & 4 ? hi : lo} + shift);
    std::vector<TriangleMesh::Triangle> t{{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                                          {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    return TriangleMesh(std::move(v), std::move(t));
}

TriangleMesh joined(const TriangleMesh& a, const TriangleMesh& b)
{
    TriangleMesh m = a;
    m.append(b);
    return m;
}

} // namespace

TEST_CASE("A cube does not intersect itself", "[core][intersect]")
{
    const SelfIntersections s = findSelfIntersections(cube(0, 10));
    CHECK_FALSE(s.any());
}

TEST_CASE("Two cubes that cut through each other cross along the walls", "[core][intersect]")
{
    const SelfIntersections s = findSelfIntersections(joined(cube(0, 10), cube(0, 10, {5, 5, 5})));
    CHECK(s.any());
    CHECK(s.longest >= 5.0);
    REQUIRE_FALSE(s.examples.empty());
}

TEST_CASE("Cubes that only touch or stand apart do not cross", "[core][intersect]")
{
    // Face to face: the coinciding walls overlap, so this is reported (as coplanar).
    const SelfIntersections faces = findSelfIntersections(joined(cube(0, 10), cube(0, 10, {10, 0, 0})));
    CHECK(faces.any());
    CHECK(faces.examples.front().coplanar);
    CHECK_FALSE(findSelfIntersections(joined(cube(0, 10), cube(0, 10, {10.001, 0, 0}))).any());
    CHECK_FALSE(findSelfIntersections(joined(cube(0, 10), cube(0, 10, {10, 10, 10}))).any()); // corner to corner
    CHECK_FALSE(findSelfIntersections(joined(cube(0, 10), cube(0, 10, {30, 0, 0}))).any());
}

TEST_CASE("A crossing of a few microns is found", "[core][intersect]")
{
    // A second cube that sinks 0.0005 mm into the first through its top wall.
    const SelfIntersections s = findSelfIntersections(joined(cube(0, 10), cube(0, 10, {1, 1, 9.9995})));
    CHECK(s.any());
    CHECK(s.longest > 5.0); // along the walls of the upper cube
    // Only a corner pokes through: one triangle cuts the top wall in a 0.0005 mm segment.
    TriangleMesh m = cube(0, 10);
    const auto top = m.addVertex({5, 5, 10.0005});
    m.addTriangle(top, 2, 3);
    const SelfIntersections tip = findSelfIntersections(m);
    CHECK(tip.any());
}

TEST_CASE("Triangles sharing an edge cross only when they fold onto each other", "[core][intersect]")
{
    TriangleMesh fan({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {10, 10, 0}, {0, 10, 5}}, {{0, 1, 2}, {1, 3, 2}});
    CHECK_FALSE(findSelfIntersections(fan).any());
    // A third triangle on the same edge, lying over the first.
    TriangleMesh folded({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {2, 8, 0}}, {{0, 1, 2}, {0, 1, 3}});
    const SelfIntersections s = findSelfIntersections(folded);
    CHECK(s.pairs == 1);
    CHECK(s.examples.front().coplanar);
}

TEST_CASE("A crossing of a few microns is lifted by moving one vertex", "[core][intersect]")
{
    // A triangle in the plane z = 0 and a second one whose corner sticks 0.001 mm through it.
    const TriangleMesh mesh({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {3, 3, -0.001}, {4, 3, 0.5}, {3, 4, 0.5}},
                            {{0, 1, 2}, {3, 4, 5}});
    REQUIRE(findSelfIntersections(mesh).pairs == 1);

    const IntersectionRepair fixed = untangleSelfIntersections(mesh, 0.01);
    CHECK_FALSE(fixed.remaining.any());
    CHECK(fixed.moved == 1);
    CHECK(fixed.largestMove < 0.01);
    CHECK_FALSE(findSelfIntersections(fixed.mesh).any());

    // Not further than allowed: the crossing stays and is reported.
    const IntersectionRepair limited = untangleSelfIntersections(mesh, 1e-5);
    CHECK(limited.remaining.pairs == 1);
    CHECK(limited.moved == 0);
}

TEST_CASE("Untangling an empty mesh gives an empty mesh", "[core][intersect]")
{
    const IntersectionRepair r = untangleSelfIntersections(TriangleMesh{}, 0.01);
    CHECK(r.mesh.isEmpty());
    CHECK_FALSE(r.remaining.any());
}

TEST_CASE("Triangles sharing a corner cross when their cut ends on a border", "[core][intersect]")
{
    // The cut runs from the shared corner to (5, 5, 0), which lies on the border of the first.
    const TriangleMesh mesh({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {10, 10, 1}, {0, 0, -1}}, {{0, 1, 2}, {0, 3, 4}});
    const SelfIntersections s = findSelfIntersections(mesh);
    CHECK(s.pairs == 1);
    CHECK(s.longest == Approx(std::sqrt(50.0)).epsilon(1e-9));
    // Only the corner in common, the rest apart: no crossing.
    CHECK_FALSE(findSelfIntersections(TriangleMesh({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {-3, -3, 1}, {-4, -4, -1}},
                                                   {{0, 1, 2}, {0, 3, 4}}))
                    .any());
}

TEST_CASE("Coplanar overlap is found where a determinant in long double rounds the wrong way", "[core][intersect]")
{
    // A big flat triangle with coordinates that are exact in float, and a smaller one strictly inside it.
    const double k = 1.0 / 65536.0;
    const double xs[3] = {7584332 * k, 48068 * k, 3141192 * k};
    const double ys[3] = {318812 * k, 111912 * k, 7942356 * k};
    std::vector<Vec3> v;
    for (int i = 0; i < 3; ++i)
        v.push_back({xs[i], ys[i], xs[i] + ys[i]});
    const double w[3][3] = {{0.5, 0.25, 0.25}, {0.25, 0.5, 0.25}, {0.25, 0.25, 0.5}};
    for (const auto& weights : w) {
        Vec3 p{};
        for (int i = 0; i < 3; ++i)
            p = p + v[static_cast<std::size_t>(i)] * weights[i];
        v.push_back(p);
    }
    const SelfIntersections s = findSelfIntersections(TriangleMesh(v, {{0, 1, 2}, {3, 4, 5}}));
    CHECK(s.pairs == 1);
    CHECK(s.examples.front().coplanar);
}
