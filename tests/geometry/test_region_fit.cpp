// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/RegionFit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace aerocad;
using core::Vec3;

namespace {

/// Closed X x Y x Z box from six n x n grids (faces share vertices by position only, as in
/// STL files), outward winding.
core::TriangleMesh box(double x, double y, double z, int n)
{
    std::vector<Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    auto face = [&](Vec3 o, Vec3 a, Vec3 b) { // outward normal a x b
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                v.push_back(o + a * (static_cast<double>(i) / n) + b * (static_cast<double>(j) / n));
        auto id = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                t.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
                t.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
            }
    };
    face({0, 0, 0}, {0, y, 0}, {x, 0, 0});
    face({0, 0, z}, {x, 0, 0}, {0, y, 0});
    face({0, 0, 0}, {x, 0, 0}, {0, 0, z});
    face({0, y, 0}, {0, 0, z}, {x, 0, 0});
    face({0, 0, 0}, {0, 0, z}, {0, y, 0});
    face({x, 0, 0}, {0, y, 0}, {0, 0, z});
    return {std::move(v), std::move(t)};
}

/// Closed cylinder (can) of radius r and height h with flat caps.
core::TriangleMesh can(double r, double h, int segments, int rows, int rings)
{
    std::vector<Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    for (int i = 0; i <= rows; ++i)
        for (int j = 0; j < segments; ++j) {
            const double a = 2.0 * std::numbers::pi * j / segments;
            v.push_back({r * std::cos(a), r * std::sin(a), h * i / rows});
        }
    auto side = [&](int i, int j) { return static_cast<std::uint32_t>(i * segments + j % segments); };
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < segments; ++j) {
            t.push_back({side(i, j), side(i, j + 1), side(i + 1, j + 1)});
            t.push_back({side(i, j), side(i + 1, j + 1), side(i + 1, j)});
        }
    for (int top = 0; top < 2; ++top) {
        const double z = top != 0 ? h : 0.0;
        const auto centre = static_cast<std::uint32_t>(v.size());
        v.push_back({0, 0, z});
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int k = 1; k < rings; ++k)
            for (int j = 0; j < segments; ++j) {
                const double a = 2.0 * std::numbers::pi * j / segments;
                v.push_back({r * k / rings * std::cos(a), r * k / rings * std::sin(a), z});
            }
        auto ring = [&](int k, int j) -> std::uint32_t {
            if (k == 0)
                return centre;
            if (k == rings)
                return side(top != 0 ? rows : 0, j);
            return base + static_cast<std::uint32_t>((k - 1) * segments + j % segments);
        };
        for (int k = 0; k < rings; ++k)
            for (int j = 0; j < segments; ++j) {
                std::array<std::uint32_t, 3> a{ring(k, j), ring(k + 1, j), ring(k + 1, j + 1)};
                std::array<std::uint32_t, 3> b{ring(k, j), ring(k + 1, j + 1), ring(k, j + 1)};
                if (top == 0) {
                    std::swap(a[1], a[2]);
                    std::swap(b[1], b[2]);
                }
                t.push_back(a);
                if (k > 0)
                    t.push_back(b);
            }
    }
    return {std::move(v), std::move(t)};
}

} // namespace

TEST_CASE("Mesh splits into smooth regions at creases", "[core][selection]")
{
    const auto mesh = box(60, 40, 30, 10);
    const core::MeshAdjacency adjacency(mesh);
    const auto regions = core::segmentSmoothRegions(mesh, adjacency, {}, 30.0, 0.0);
    REQUIRE(regions.size() == 6);
    for (const auto& r : regions)
        CHECK(r.size() == 200);
    // Restricted to a selection: only its triangles.
    const auto two = core::segmentSmoothRegions(mesh, adjacency, regions[0], 30.0, 0.0);
    CHECK(two.size() == 1);
    // A large crease angle keeps the box together; tiny regions join their neighbour.
    CHECK(core::segmentSmoothRegions(mesh, adjacency, {}, 95.0, 0.0).size() == 1);
    CHECK(core::segmentSmoothRegions(mesh, adjacency, {}, 30.0, 1e6).size() == 1);
}

TEST_CASE("A closed box mesh becomes a solid", "[geometry][fit]")
{
    const auto mesh = box(60, 40, 30, 20);
    const auto fit = geometry::fitRegions(mesh, {});
    CHECK(fit.patches.size() == 6);
    CHECK(fit.faces == 6);
    CHECK(fit.freeEdges == 0);
    REQUIRE(fit.solid);
    CHECK(std::abs(fit.volume - 60.0 * 40.0 * 30.0) < 1.0);
    CHECK(fit.shape.solidCount() == 1);
    for (const auto& p : fit.patches)
        CHECK(p.report.withinTolerance);

    // Inconsistent winding (common in STL files) changes nothing.
    auto tris = mesh.triangles();
    for (std::size_t k = 0; k < tris.size(); k += 3)
        std::swap(tris[k][1], tris[k][2]);
    const auto flipped = geometry::fitRegions(core::TriangleMesh(mesh.vertices(), std::move(tris)), {});
    CHECK(flipped.solid);
    CHECK(std::abs(flipped.volume - 60.0 * 40.0 * 30.0) < 1.0);
}

TEST_CASE("A closed can is split at its tube and becomes a solid", "[geometry][fit]")
{
    const auto mesh = can(20, 50, 90, 30, 10);
    const auto fit = geometry::fitRegions(mesh, {});
    // Two caps and the tube, which does not unfold onto a disc and is cut in two.
    CHECK(fit.faces == 4);
    REQUIRE(fit.solid);
    // The mesh polygon has 90 sides: its volume is a little below the circle's.
    const double polygonVolume = 0.5 * 90 * 20.0 * 20.0 * std::sin(2.0 * std::numbers::pi / 90) * 50.0;
    CHECK(std::abs(fit.volume - polygonVolume) < 0.005 * polygonVolume);

    // The two tube halves meet smoothly: each is fit on data past the seam.
    CHECK(fit.smoothSeams == 2);
    CHECK(fit.seamAngleMaxDeg < 0.3);
    geometry::RegionFitOptions separate;
    separate.blendRings = 0;
    const auto c0 = geometry::fitRegions(mesh, {}, separate);
    REQUIRE(c0.smoothSeams == 2);
    CHECK(fit.seamAngleMaxDeg < 0.5 * c0.seamAngleMaxDeg);
}

TEST_CASE("Part of a mesh gives an open shell", "[geometry][fit]")
{
    const auto mesh = box(60, 40, 30, 10);
    const core::MeshAdjacency adjacency(mesh);
    auto regions = core::segmentSmoothRegions(mesh, adjacency, {}, 30.0, 0.0);
    core::TriangleSet three;
    for (int k = 0; k < 3; ++k)
        three.insert(three.end(), regions[static_cast<std::size_t>(k)].begin(),
                     regions[static_cast<std::size_t>(k)].end());
    core::normalizeSelection(three);
    const auto fit = geometry::fitRegions(mesh, three);
    CHECK(fit.faces == 3);
    CHECK_FALSE(fit.solid);
    CHECK(fit.freeEdges > 0);
    CHECK(fit.shape.faceCount() == 3);
    CHECK(fit.shape.solidCount() == 0);
}

TEST_CASE("Separate closed parts and cavities are all kept", "[geometry][fit]")
{
    // Two boxes apart: two solids.
    const auto a = box(30, 20, 10, 8);
    std::vector<Vec3> v = a.vertices();
    auto t = a.triangles();
    const auto offset = static_cast<std::uint32_t>(v.size());
    for (const auto& p : a.vertices())
        v.push_back(p + Vec3{50, 0, 0});
    for (const auto& tri : a.triangles())
        t.push_back({tri[0] + offset, tri[1] + offset, tri[2] + offset});
    const auto two = geometry::fitRegions(core::TriangleMesh(v, t), {});
    REQUIRE(two.solid);
    CHECK(two.shape.solidCount() == 2);
    CHECK(std::abs(two.volume - 2 * 6000.0) < 2.0);

    // A box with a box-shaped cavity (inner faces point inward): one solid with a void.
    std::vector<Vec3> w = box(60, 40, 30, 10).vertices();
    auto u = box(60, 40, 30, 10).triangles();
    const auto inner = box(20, 10, 10, 6);
    const auto base = static_cast<std::uint32_t>(w.size());
    for (const auto& p : inner.vertices())
        w.push_back(p + Vec3{20, 15, 10});
    for (const auto& tri : inner.triangles())
        u.push_back({tri[0] + base, tri[2] + base, tri[1] + base});
    const auto hollow = geometry::fitRegions(core::TriangleMesh(w, u), {});
    REQUIRE(hollow.solid);
    CHECK(hollow.shape.solidCount() == 1);
    CHECK(std::abs(hollow.volume - (72000.0 - 2000.0)) < 5.0);
}
