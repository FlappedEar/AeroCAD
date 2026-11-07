// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <memory>
#include <vector>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Square grid in the plane z = height, from (0,0) to (size,size), n x n cells.
void addGrid(MeshBuilder& b, int n, double size, double height, bool flip = false)
{
    const double s = size / n;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const Vec3 p00{i * s, j * s, height}, p10{(i + 1) * s, j * s, height};
            const Vec3 p01{i * s, (j + 1) * s, height}, p11{(i + 1) * s, (j + 1) * s, height};
            if (flip) {
                b.addTriangle(p00, p11, p10);
                b.addTriangle(p00, p01, p11);
            } else {
                b.addTriangle(p00, p10, p11);
                b.addTriangle(p00, p11, p01);
            }
        }
    }
}

/// Closed cube [0,10]^3, each face an n x n grid (2 n^2 triangles per face).
TriangleMesh cube(int n)
{
    MeshBuilder b;
    const double s = 10.0 / n;
    // Each face: origin, two edge directions.
    const std::array<std::array<Vec3, 3>, 6> faces{{
        {{{0, 0, 0}, {0, 1, 0}, {1, 0, 0}}},  // z = 0
        {{{0, 0, 10}, {1, 0, 0}, {0, 1, 0}}}, // z = 10
        {{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}},  // y = 0
        {{{0, 10, 0}, {0, 0, 1}, {1, 0, 0}}}, // y = 10
        {{{0, 0, 0}, {0, 0, 1}, {0, 1, 0}}},  // x = 0
        {{{10, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, // x = 10
    }};
    for (const auto& f : faces) {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                const Vec3 p00 = f[0] + f[1] * (i * s) + f[2] * (j * s);
                const Vec3 p10 = f[0] + f[1] * ((i + 1) * s) + f[2] * (j * s);
                const Vec3 p01 = f[0] + f[1] * (i * s) + f[2] * ((j + 1) * s);
                const Vec3 p11 = f[0] + f[1] * ((i + 1) * s) + f[2] * ((j + 1) * s);
                b.addTriangle(p00, p10, p11);
                b.addTriangle(p00, p11, p01);
            }
        }
    }
    return b.take();
}

} // namespace

TEST_CASE("Adjacency connects triangles by shared edge positions", "[core][selection]")
{
    // Two triangles sharing an edge but not vertex indices (like tessellated B-rep faces).
    TriangleMesh m;
    const auto a = m.addVertex({0, 0, 0});
    const auto b = m.addVertex({1, 0, 0});
    const auto c = m.addVertex({0, 1, 0});
    const auto b2 = m.addVertex({1, 0, 0});
    const auto c2 = m.addVertex({0, 1, 0});
    const auto d = m.addVertex({1, 1, 0});
    m.addTriangle(a, b, c);
    m.addTriangle(b2, d, c2);
    const MeshAdjacency adj(m);
    CHECK(adj.neighbours(0)[1] == 1);
    CHECK(adj.neighbours(1)[2] == 0);
    CHECK(adj.neighbours(0)[0] == MeshAdjacency::none);
}

TEST_CASE("Smooth region stops at sharp edges", "[core][selection]")
{
    const TriangleMesh m = cube(4);
    REQUIRE(m.triangleCount() == 6 * 32);
    const MeshAdjacency adj(m);
    const auto seed = nearestTriangle(m, {5, 5, 10.1});
    REQUIRE(seed);
    const TriangleSet top = selectSmoothRegion(m, adj, *seed, {});
    CHECK(top.size() == 32);
    CHECK(selectionArea(m, top) == Approx(100.0));

    SmoothRegionOptions loose;
    loose.maxNeighbourAngleDeg = 95.0;
    CHECK(selectSmoothRegion(m, adj, *seed, loose).size() == m.triangleCount());
    loose.maxSeedAngleDeg = 45.0;
    CHECK(selectSmoothRegion(m, adj, *seed, loose).size() == 32);
}

TEST_CASE("Connected brush does not go through thin walls", "[core][selection]")
{
    MeshBuilder b;
    addGrid(b, 20, 10.0, 0.0);
    addGrid(b, 20, 10.0, 0.5, true); // second sheet 0.5 mm above
    const TriangleMesh m = b.take();
    const MeshAdjacency adj(m);
    const Vec3 point{5, 5, 0.5};

    const TriangleSet through = selectBrushDab(m, nullptr, point, 2.0);
    const TriangleSet connected = selectBrushDab(m, &adj, point, 2.0);
    CHECK(through.size() > connected.size());
    for (const auto t : connected)
        CHECK(t >= 800); // only triangles of the upper sheet
}

TEST_CASE("Lasso selects visible triangles inside the polygon", "[core][selection]")
{
    MeshBuilder b;
    addGrid(b, 10, 10.0, 0.0);
    addGrid(b, 10, 10.0, -5.0); // hidden sheet below
    const TriangleMesh m = b.take();

    ViewParams view;
    view.eye = {5, 5, 50};
    view.forward = {0, 0, -1};
    view.up = {0, 1, 0};
    view.fovYDeg = 40;
    view.aspect = 1;
    // Whole viewport.
    const std::vector<std::array<double, 2>> all{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const TriangleSet visible = selectInLasso(m, view, all);
    CHECK(visible.size() == 200);
    for (const auto t : visible)
        CHECK(t < 200);
    CHECK(selectInLasso(m, view, all, {false}).size() == 400);

    // Right half of the screen = x > 5 (camera looks down -Z with +Y up, so +X is right).
    const std::vector<std::array<double, 2>> right{{0.5, 0}, {1, 0}, {1, 1}, {0.5, 1}};
    const TriangleSet half = selectInLasso(m, view, right);
    CHECK(half.size() == 100);
    for (const auto t : half) {
        const auto& tri = m.triangles()[t];
        CHECK(m.vertices()[tri[0]].x + m.vertices()[tri[1]].x + m.vertices()[tri[2]].x > 15.0);
    }
}

TEST_CASE("Grow, shrink, invert and combine", "[core][selection]")
{
    const TriangleMesh m = cube(4);
    const MeshAdjacency adj(m);
    const TriangleSet top = selectSmoothRegion(m, adj, *nearestTriangle(m, {5, 5, 10}), {});
    const TriangleSet grown = growSelection(adj, top, 1);
    CHECK(grown.size() > top.size());
    CHECK(growSelection(adj, grown, -1) == top);
    CHECK(invertSelection(m.triangleCount(), top).size() == m.triangleCount() - top.size());
    CHECK(combineSelection(grown, top, SelectionMode::Subtract).size() == grown.size() - top.size());
    CHECK(combineSelection(top, grown, SelectionMode::Subtract).empty());
    CHECK(combineSelection(top, grown, SelectionMode::Add) == grown);
    CHECK(combineSelection(top, grown, SelectionMode::Replace) == grown);
}

TEST_CASE("Adjacency cache returns the same object for the same mesh", "[core][selection]")
{
    auto mesh = std::make_shared<const TriangleMesh>(cube(2));
    const auto a = adjacencyFor(mesh);
    CHECK(adjacencyFor(mesh) == a);
    auto other = std::make_shared<const TriangleMesh>(cube(2));
    CHECK(adjacencyFor(other) != a);
}

TEST_CASE("Lasso with an orthographic view", "[core][selection]")
{
    MeshBuilder b;
    addGrid(b, 10, 10.0, 0.0);
    addGrid(b, 10, 10.0, -5.0);
    const TriangleMesh m = b.take();
    ViewParams view;
    view.eye = {5, 5, 50};
    view.forward = {0, 0, -1};
    view.up = {0, 1, 0};
    view.aspect = 1;
    view.orthographic = true;
    view.height = 20; // viewport shows x, y in [-5, 15]
    // Centre quarter of the viewport = [0, 10]^2, the whole visible sheet.
    const std::vector<std::array<double, 2>> centre{{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}};
    CHECK(selectInLasso(m, view, centre).size() == 200);
    CHECK(selectInLasso(m, view, centre, {false}).size() == 400);
}

TEST_CASE("Lasso keeps surfaces seen at a grazing angle", "[core][selection]")
{
    // 10 x 10 grid tilted 80 degrees about Y, fully visible from the camera.
    MeshBuilder b;
    const double angle = 80.0 * 3.14159265358979 / 180.0;
    const double c = std::cos(angle), s = std::sin(angle);
    auto at = [&](double x, double y) { return Vec3{x * c, y, -x * s}; };
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) {
            const double x0 = i - 5.0, x1 = i - 4.0, y0 = j - 5.0, y1 = j - 4.0;
            b.addTriangle(at(x0, y0), at(x1, y0), at(x1, y1));
            b.addTriangle(at(x0, y0), at(x1, y1), at(x0, y1));
        }
    }
    const TriangleMesh m = b.take();
    ViewParams view;
    view.eye = {0, 0, 50};
    view.forward = {0, 0, -1};
    view.up = {0, 1, 0};
    view.fovYDeg = 40;
    view.aspect = 1;
    const std::vector<std::array<double, 2>> all{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    CHECK(selectInLasso(m, view, all).size() == 200);
}

TEST_CASE("Raycast finds the nearest surface hit", "[core][selection]")
{
    MeshBuilder b;
    addGrid(b, 4, 10.0, 0.0);
    addGrid(b, 4, 10.0, 5.0);
    const TriangleMesh m = b.take();
    const auto hit = raycast(m, {3, 3, 20}, {0, 0, -1});
    REQUIRE(hit);
    CHECK(hit->point.z == Approx(5.0));
    CHECK(hit->distance == Approx(15.0));
    CHECK_FALSE(raycast(m, {30, 30, 20}, {0, 0, -1}));
    CHECK_FALSE(raycast(m, {3, 3, 20}, {0, 0, 1}));
}

TEST_CASE("Seed angle limit above 90 degrees", "[core][selection]")
{
    // Consistently wound cylinder side, 36 segments (72 triangles), radius 10, height 10.
    MeshBuilder b;
    const int n = 36;
    for (int i = 0; i < n; ++i) {
        const double a0 = 2 * 3.14159265358979 * i / n, a1 = 2 * 3.14159265358979 * ((i + 1) % n) / n;
        const Vec3 p0{10 * std::cos(a0), 10 * std::sin(a0), 0}, p1{10 * std::cos(a1), 10 * std::sin(a1), 0};
        const Vec3 q0{p0.x, p0.y, 10}, q1{p1.x, p1.y, 10};
        b.addTriangle(p0, p1, q1);
        b.addTriangle(p0, q1, q0);
    }
    const TriangleMesh m = b.take();
    const MeshAdjacency adj(m);
    SmoothRegionOptions o;
    o.maxNeighbourAngleDeg = 20;
    o.maxSeedAngleDeg = 120;
    const auto seed = nearestTriangle(m, {10, 0.3, 5});
    REQUIRE(seed);
    const auto region = selectSmoothRegion(m, adj, *seed, o);
    // Within 120 degrees either way: about 2 * 120 / 360 of the 72 triangles.
    CHECK(region.size() < 60);
    CHECK(region.size() > 40);
    o.maxSeedAngleDeg = 180;
    CHECK(selectSmoothRegion(m, adj, *seed, o).size() == 72);
}

TEST_CASE("Shrink erodes selections at open mesh edges", "[core][selection]")
{
    MeshBuilder b;
    addGrid(b, 3, 3.0, 0.0);
    const TriangleMesh m = b.take();
    const MeshAdjacency adj(m);
    TriangleSet all(18);
    for (std::uint32_t i = 0; i < 18; ++i)
        all[i] = i;
    const auto shrunk = growSelection(adj, all, -1);
    CHECK(shrunk.size() < all.size());
    CHECK(growSelection(adj, all, 1) == all);
}

TEST_CASE("Smooth region: winding from edge direction, sharp folds stay bends", "[core][selection]")
{
    // Two triangles on the shared edge (0,0,0)-(1,0,0).
    auto pair = [](const Vec3& apex, bool flipSecond) {
        std::vector<Vec3> v{{0, 0, 0}, {1, 0, 0}, {0.5, 1, 0}, apex};
        std::vector<TriangleMesh::Triangle> t{{0, 1, 2}};
        t.push_back(flipSecond ? TriangleMesh::Triangle{0, 1, 3} : TriangleMesh::Triangle{1, 0, 3});
        return TriangleMesh(std::move(v), std::move(t));
    };

    // Consistently wound, folded 170 degrees: a crease, not a smooth continuation.
    const double a = 10.0 * std::numbers::pi / 180.0;
    const TriangleMesh fold = pair({0.5, std::cos(a), std::sin(a)}, false);
    const MeshAdjacency foldAdj(fold);
    CHECK_FALSE(foldAdj.windingFlipped(0, 0));
    CHECK(selectSmoothRegion(fold, foldAdj, 0, {}).size() == 1);

    // Flat but the second triangle is wound the other way (unwelded STL): still one region.
    const TriangleMesh flat = pair({0.5, -1, 0}, true);
    const MeshAdjacency flatAdj(flat);
    CHECK(flatAdj.windingFlipped(0, 0));
    CHECK(selectSmoothRegion(flat, flatAdj, 0, {}).size() == 2);
}

namespace {

/// A wavy sheet over [0, 40]^2 facing +Z, and a second layer `below` under it with a square
/// hole through the upper sheet (the triangles over [15, 25]^2 are missing).
TriangleMesh layeredSheet(double below)
{
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 40;
    for (const double dz : {0.0, -below}) {
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                v.push_back({double(i), double(j), std::sin(i * 0.1) + dz});
        auto idx = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                if (dz == 0.0 && i >= 15 && i < 25 && j >= 15 && j < 25)
                    continue;
                t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
                t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
            }
    }
    return TriangleMesh(std::move(v), std::move(t));
}

} // namespace

TEST_CASE("MeshProjector finds the skin seen from outside, never a layer behind it", "[core][selection]")
{
    const TriangleMesh mesh = layeredSheet(3.0);
    const MeshProjector projector(mesh, {0, 0, 1});

    // From any height along the line: the upper sheet.
    for (const double z : {50.0, 0.0, -50.0}) {
        const auto hit = projector.drop({5.5, 7.5, z});
        REQUIRE(hit.has_value());
        CHECK(hit->z == Approx(std::sin(0.55)).margin(0.02));
    }
    // Through the hole: the layer behind is the first thing met.
    const auto through = projector.drop({20.5, 20.5, 50.0});
    REQUIRE(through.has_value());
    CHECK(through->z == Approx(std::sin(2.05) - 3.0).margin(0.02));
    // Past the edge.
    CHECK_FALSE(projector.drop({60, 5, 0}).has_value());
    CHECK_FALSE(projector.drop({41, 5, 0}).has_value());
}

TEST_CASE("MeshProjector ray casts match the brute force ones", "[core][selection]")
{
    const TriangleMesh mesh = layeredSheet(3.0);
    const MeshProjector projector(mesh, {0.3, -0.2, 1.0});
    int hits = 0;
    for (int k = 0; k < 400; ++k) {
        // Rays from all around, straight down to grazing, some missing.
        const double a = k * 0.37, b = 0.05 + 0.0037 * k;
        const Vec3 origin{20 + 60 * std::cos(a), 20 + 60 * std::sin(a), 5 + 40 * b};
        const Vec3 target{(k * 7) % 50 - 5.0, (k * 13) % 50 - 5.0, 0.0};
        const Vec3 direction = target - origin;
        const auto expected = raycast(mesh, origin, direction);
        const auto actual = projector.raycast(origin, direction);
        REQUIRE(expected.has_value() == actual.has_value());
        if (expected) {
            CHECK(actual->distance == Approx(expected->distance).margin(1e-9));
            ++hits;
        }
    }
    CHECK(hits > 100);
    // Starting past the mesh and pointing away.
    CHECK_FALSE(projector.raycast({100, 100, 10}, {1, 1, 0}).has_value());
}

TEST_CASE("projectOntoMesh takes the skin from outside and reports misses", "[core][selection]")
{
    const TriangleMesh mesh = layeredSheet(3.0);
    const std::vector<Vec3> pts{{5.5, 7.5, -10}, {60, 60, 10}};
    std::vector<char> missed;
    const auto out = projectOntoMesh(mesh, pts, {0, 0, 1}, nullptr, &missed);
    CHECK(out[0].z == Approx(std::sin(0.55)).margin(0.02));
    CHECK(missed[0] == 0);
    CHECK(missed[1] != 0);
    CHECK(out[1].z == Approx(10.0));
}

TEST_CASE("projectOntoMesh drapes points onto a curved sheet", "[core][selection]")
{
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 40;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({double(i), double(j), 0.01 * (i - 20) * (i - 20)});
    auto idx = [](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    const TriangleMesh mesh(std::move(v), std::move(t));
    // Points on a plane above, one below, one off the sheet.
    const std::vector<Vec3> pts{{5, 5, 10}, {20, 20, 10}, {33.5, 12.25, -3}, {60, 60, 10}};
    std::size_t hits = 0;
    const auto out = projectOntoMesh(mesh, pts, {0, 0, -1}, &hits);
    REQUIRE(out.size() == 4);
    CHECK(hits == 3);
    CHECK(out[0].z == Approx(0.01 * 15 * 15).margin(0.02));
    CHECK(out[1].z == Approx(0.0).margin(0.02));
    CHECK(out[2].z == Approx(0.01 * 13.5 * 13.5).margin(0.02));
    CHECK(out[2].x == Approx(33.5));
    CHECK(out[3].z == Approx(10.0)); // missed: stays
}

TEST_CASE("nearestToRay: a click into a hole takes the surface at its edge", "[core][selection]")
{
    // A 20 x 20 plate at z = 0 with a 6 x 6 opening in the middle (cells 7..12).
    MeshBuilder b;
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j) {
            if (i >= 7 && i < 13 && j >= 7 && j < 13)
                continue;
            const Vec3 p00{double(i), double(j), 0}, p10{double(i + 1), double(j), 0};
            const Vec3 p01{double(i), double(j + 1), 0}, p11{double(i + 1), double(j + 1), 0};
            b.addTriangle(p00, p10, p11);
            b.addTriangle(p00, p11, p01);
        }
    const TriangleMesh m = b.take();
    const Vec3 origin{10.2, 10.6, 100};
    const Vec3 down{0, 0, -2};
    REQUIRE_FALSE(raycast(m, origin, down)); // through the opening

    const auto hit = nearestToRay(m, origin, down, 4.0);
    REQUIRE(hit);
    // The nearest point of the opening's rim: (10, 13) is 2.4 off the ray, any other more.
    CHECK(hit->point.x == Approx(10.0));
    CHECK(hit->point.y == Approx(13.0));
    CHECK(hit->point.z == Approx(0.0));
    CHECK(hit->distance == Approx(50.0)); // in units of |direction|
    const auto& tri = m.triangles()[hit->triangle];
    CHECK(((m.vertices()[tri[0]] - hit->point).length() < 1e-12 || (m.vertices()[tri[1]] - hit->point).length() < 1e-12 ||
           (m.vertices()[tri[2]] - hit->point).length() < 1e-12));

    // Too far from anything for the angle, behind the origin, or a degenerate ray.
    CHECK_FALSE(nearestToRay(m, origin, down, 1.0));
    CHECK_FALSE(nearestToRay(m, {10.2, 10.6, -100}, down, 4.0));
    CHECK_FALSE(nearestToRay(m, origin, {0, 0, 0}, 4.0));
}
