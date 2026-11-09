// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Align.hpp"
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
    // The centroid of a tetrahedron is the mean of its corners, wherever the origin is.
    const auto centre = enclosedCentroid(m);
    REQUIRE(centre.has_value());
    CHECK(centre->x == Approx(2.5));
    CHECK(centre->y == Approx(2.5));
    CHECK(centre->z == Approx(2.5));
    CHECK_FALSE(enclosedCentroid(TriangleMesh{}).has_value());
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

TEST_CASE("A picked normal on a noisy scan is the surface's, not one triangle's (CAD-232)", "[measure]")
{
    // A 40 x 40 mm grid in z = 0 with +-0.3 mm noise on 2 mm cells, like a scan.
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 21;
    std::uint32_t seed = 12345;
    auto noise = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<double>(seed >> 8) / static_cast<double>(1u << 24) - 0.5) * 0.6;
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            v.push_back({2.0 * i, 2.0 * j, noise()});
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const auto a = static_cast<std::uint32_t>(j * n + i);
            t.push_back({a, a + 1, a + static_cast<std::uint32_t>(n) + 1});
            t.push_back({a, a + static_cast<std::uint32_t>(n) + 1, a + static_cast<std::uint32_t>(n)});
        }
    // The far wall of a thin shell, 1 mm below and facing down.
    auto base = static_cast<std::uint32_t>(v.size());
    v.push_back({0, 0, -1});
    v.push_back({40, 0, -1});
    v.push_back({0, 40, -1});
    t.push_back({base, base + 2, base + 1});
    // A large triangle tilted by 45 degrees that just reaches the neighbourhood: only its
    // part inside it may count, not its whole area.
    base = static_cast<std::uint32_t>(v.size());
    v.push_back({23.5, 0, 0.5});
    v.push_back({23.5, 40, 0.5});
    v.push_back({123.5, 20, 100.5});
    t.push_back({base, base + 2, base + 1});
    const TriangleMesh mesh(v, t);

    const auto hit = closestPointOnMesh(mesh, {20.3, 20.7, 5.0});
    REQUIRE(hit);
    const Vec3 raw = mesh.faceNormal(hit->triangle);
    const double radius = defaultNormalRadius(mesh, hit->triangle);
    CHECK(radius >= 2.0 * 2.0);
    const Vec3 smooth = patchNormal(mesh, hit->point, hit->triangle, radius);
    CHECK(smooth.z > raw.z);
    CHECK(smooth.z > 0.99); // within about 8 degrees of the surface normal
}
