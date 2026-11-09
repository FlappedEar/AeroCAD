// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/DrapeFit.hpp"
#include "aerocad/geometry/Sketch.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace aerocad;
using Catch::Approx;

namespace {

double truth(double x, double y)
{
    return 6.0 * std::sin(x / 18.0) * std::cos(y / 23.0) + 0.02 * x;
}

/// 100 x 100 mm freeform height field, n x n cells, Gaussian noise on z.
core::TriangleMesh noisyField(int n, double sigma)
{
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, sigma);
    const double s = 100.0 / n;
    std::vector<core::Vec3> v;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({i * s, j * s, truth(i * s, j * s) + noise(rng)});
    std::vector<core::TriangleMesh::Triangle> t;
    auto idx = [n](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    return {std::move(v), std::move(t)};
}

geometry::Shape solidOf(const geometry::Shape& face)
{
    return geometry::thickenSurface(face, 1.0);
}

/// Open tube of radius r and height h along z: n segments around, m rows.
core::TriangleMesh tube(double r, double h, int n, int m)
{
    std::vector<core::Vec3> v;
    for (int i = 0; i <= m; ++i)
        for (int j = 0; j < n; ++j) {
            const double a = 2.0 * std::numbers::pi * j / n;
            v.push_back({r * std::cos(a), r * std::sin(a), h * i / m});
        }
    auto id = [n](int i, int j) { return static_cast<std::uint32_t>(i * n + j % n); };
    std::vector<core::TriangleMesh::Triangle> t;
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j) {
            t.push_back({id(i, j), id(i, j + 1), id(i + 1, j + 1)});
            t.push_back({id(i, j), id(i + 1, j + 1), id(i + 1, j)});
        }
    return {std::move(v), std::move(t)};
}

/// Ellipsoid with semi-axes a, b, c (one vertex at each pole), outward normals.
core::TriangleMesh ellipsoid(double a, double b, double c, int rings, int segments)
{
    std::vector<core::Vec3> v{{0, 0, c}};
    for (int i = 1; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            const double th = std::numbers::pi * i / rings, ph = 2.0 * std::numbers::pi * j / segments;
            v.push_back({a * std::sin(th) * std::cos(ph), b * std::sin(th) * std::sin(ph), c * std::cos(th)});
        }
    v.push_back({0, 0, -c});
    auto id = [&](int i, int j) {
        if (i == 0)
            return 0u;
        if (i == rings)
            return static_cast<std::uint32_t>(1 + (rings - 1) * segments);
        return static_cast<std::uint32_t>(1 + (i - 1) * segments + j % segments);
    };
    std::vector<core::TriangleMesh::Triangle> t;
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            if (i < rings - 1)
                t.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            if (i > 0)
                t.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    return {std::move(v), std::move(t)};
}

template <typename Keep> core::TriangleSet selectWhere(const core::TriangleMesh& mesh, Keep keep)
{
    core::TriangleSet out;
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        const core::Vec3 c =
            (mesh.vertices()[tri[0]] + mesh.vertices()[tri[1]] + mesh.vertices()[tri[2]]) * (1.0 / 3.0);
        if (keep(c))
            out.push_back(t);
    }
    return out;
}

} // namespace

TEST_CASE("Fit a trimmed surface to a noisy selected region", "[geometry][fit]")
{
    const core::TriangleMesh mesh = noisyField(120, 0.05);
    const core::TriangleSet selection = core::selectInSphere(mesh, {50, 50, truth(50, 50)}, 30.0);
    REQUIRE(selection.size() > 1000);

    for (const auto method : {geometry::SurfaceFitMethod::Auto, geometry::SurfaceFitMethod::Plane}) {
        geometry::SurfaceFitOptions options;
        options.method = method;
        const auto fit = geometry::fitSurface(mesh, selection, options);
        INFO("method " << fit.report.method);
        CHECK(fit.report.method == (method == geometry::SurfaceFitMethod::Plane ? "plane" : "unfold"));
        REQUIRE_FALSE(fit.face.isNull());
        CHECK(fit.face.faceCount() == 1);
        CHECK(fit.report.trimmed);
        CHECK(fit.report.meshToSurfaceMax < 0.25);
        CHECK(fit.report.meshToSurfaceRms < 0.08);
        CHECK(fit.report.surfaceToMeshMax < 0.25);
        CHECK(fit.report.withinTolerance);
        CHECK(fit.report.holes == 0);

        // Trimmed to the selection: about the 60 mm disc, not the whole field.
        const auto size = fit.face.bounds().size();
        CHECK(size.x < 64.0);
        CHECK(size.x > 54.0);
        CHECK(size.y < 64.0);
    }
}

TEST_CASE("Fit rejects tiny selections", "[geometry][fit]")
{
    const core::TriangleMesh mesh = noisyField(10, 0.0);
    CHECK_THROWS_AS(geometry::fitSurface(mesh, {0, 1}), geometry::SurfaceFitError);
}

TEST_CASE("Offset and thicken a fitted surface", "[geometry][offset]")
{
    const core::TriangleMesh mesh = noisyField(60, 0.0);
    const core::TriangleSet selection = core::selectInSphere(mesh, {50, 50, truth(50, 50)}, 25.0);
    const auto fit = geometry::fitSurface(mesh, selection, {});
    REQUIRE_FALSE(fit.face.isNull());
    const double area = geometry::surfaceArea(fit.face);
    REQUIRE(area > 1500.0);

    // The fitted normal follows the mesh normals (+z here), so +2 mm moves the surface up.
    const geometry::Shape offset = geometry::offsetSurface(fit.face, 2.0);
    CHECK(offset.faceCount() == 1);
    CHECK(offset.solidCount() == 0);
    // Bounds of the tessellation: BRepBndLib boxes of offset surfaces are loose.
    const auto b0 = geometry::tessellate(fit.face).bounds();
    const auto b1 = geometry::tessellate(offset).bounds();
    CHECK(std::abs((b1.max.z - b0.max.z) - 2.0) < 0.3);
    CHECK(std::abs((b1.min.z - b0.min.z) - 2.0) < 0.3);

    for (auto side : {geometry::ThickenSide::Outside, geometry::ThickenSide::Inside, geometry::ThickenSide::Both}) {
        const geometry::Shape solid = geometry::thickenSurface(fit.face, 3.0, side);
        CHECK(solid.solidCount() == 1);
        CHECK(std::abs(solid.volume() - 3.0 * area) < 0.1 * 3.0 * area);
    }
    const auto inside = geometry::tessellate(geometry::thickenSurface(fit.face, 3.0, geometry::ThickenSide::Inside)).bounds();
    CHECK(inside.max.z < b0.max.z + 0.3);
    CHECK(inside.min.z < b0.min.z - 2.5);

    CHECK_THROWS_AS(geometry::thickenSurface(fit.face, 0.0), geometry::OffsetError);
    CHECK_THROWS_AS(geometry::offsetSurface(geometry::Shape{}, 1.0), geometry::OffsetError);
    CHECK_THROWS_AS(geometry::offsetSurface(solidOf(fit.face), 1.0), geometry::OffsetError);
}

TEST_CASE("Fit reports selected mesh left outside the trimmed face", "[geometry][fit]")
{
    // Whole 100 x 100 mm flat field: the smoothed outline rounds the four corners.
    const core::TriangleMesh mesh = noisyField(40, 0.0);
    core::TriangleSet all(mesh.triangleCount());
    for (std::uint32_t t = 0; t < all.size(); ++t)
        all[t] = t;
    const auto fit = geometry::fitSurface(mesh, all, {});
    REQUIRE(fit.report.trimmed);
    INFO("outsideTrimMax " << fit.report.outsideTrimMax);
    CHECK(fit.report.outsideTrimMax > 0.0);
    bool warned = false;
    for (const auto& w : fit.report.warnings)
        warned = warned || w.find("outside the face") != std::string::npos;
    CHECK(warned == (fit.report.outsideTrimMax > 0.25));
}

TEST_CASE("Fit rejects a selection of separate patches", "[geometry][fit]")
{
    const core::TriangleMesh mesh = noisyField(60, 0.0);
    core::TriangleSet two = core::selectInSphere(mesh, {25, 25, truth(25, 25)}, 10.0);
    const core::TriangleSet second = core::selectInSphere(mesh, {75, 75, truth(75, 75)}, 10.0);
    two.insert(two.end(), second.begin(), second.end());
    core::normalizeSelection(two);
    CHECK_THROWS_AS(geometry::fitSurface(mesh, two, {}), geometry::SurfaceFitError);

    // A stray triangle (a brush edge touching the region at a corner) is left out.
    core::TriangleSet stray = core::selectInSphere(mesh, {50, 50, truth(50, 50)}, 20.0);
    stray.push_back(0);
    core::normalizeSelection(stray);
    const auto fit = geometry::fitSurface(mesh, stray, {});
    CHECK(fit.report.withinTolerance);
    bool warned = false;
    for (const auto& w : fit.report.warnings)
        warned = warned || w.find("Left out 1 ") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("Fit works on a sparse selection", "[geometry][fit]")
{
    // 4 x 4 vertices (16, the minimum), planar: the neighbourhood radius adapts.
    std::vector<core::Vec3> v;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            v.push_back({i * 10.0, j * 10.0, 0.1 * i});
    std::vector<core::TriangleMesh::Triangle> tris;
    for (std::uint32_t j = 0; j < 3; ++j)
        for (std::uint32_t i = 0; i < 3; ++i) {
            tris.push_back({j * 4 + i, j * 4 + i + 1, j * 4 + i + 5});
            tris.push_back({j * 4 + i, j * 4 + i + 5, j * 4 + i + 4});
        }
    const core::TriangleMesh mesh(std::move(v), std::move(tris));
    core::TriangleSet all(mesh.triangleCount());
    for (std::uint32_t t = 0; t < all.size(); ++t)
        all[t] = t;
    const auto fit = geometry::fitSurface(mesh, all, {});
    CHECK_FALSE(fit.face.isNull());
    CHECK(fit.report.meshToSurfaceMax < 0.05);
    // The smoothed outline may pass a little beyond the open mesh border (10 mm cells).
    CHECK(fit.report.surfaceToMeshMax < 1.0);
}

TEST_CASE("Fit unfolds a region wrapped more than 180 degrees", "[geometry][fit]")
{
    // 220 degrees of a 30 mm radius cylinder, 60 mm high: no height field over any plane.
    const core::TriangleMesh mesh = tube(30.0, 80.0, 180, 60);
    const auto wrap = selectWhere(mesh, [](const core::Vec3& c) {
        return std::abs(std::atan2(c.y, c.x)) < 110.0 * std::numbers::pi / 180.0 && c.z > 10.0 && c.z < 70.0;
    });
    const auto fit = geometry::fitSurface(mesh, wrap, {});
    CHECK(fit.report.method == "unfold");
    CHECK(fit.report.trimmed);
    CHECK(fit.report.withinTolerance);
    CHECK(fit.report.meshToSurfaceMax < 0.05);
    CHECK(fit.report.surfaceToMeshMax < 0.05);
    CHECK(std::abs(geometry::surfaceArea(fit.face) - core::selectionArea(mesh, wrap)) <
          0.02 * core::selectionArea(mesh, wrap));
    // The fitted normal follows the mesh (outward): offsetting grows the radius.
    const auto out = geometry::tessellate(geometry::offsetSurface(fit.face, 2.0)).bounds();
    CHECK(out.max.y > 31.5);

    geometry::SurfaceFitOptions plane;
    plane.method = geometry::SurfaceFitMethod::Plane;
    CHECK_FALSE(geometry::fitSurface(mesh, wrap, plane).report.withinTolerance);
}

TEST_CASE("Fit unfolds a long curved strip and a large dense region", "[geometry][fit]")
{
    // 150 degrees of the equator band of a sphere, about 14 mm wide (whole rows of quads).
    const core::TriangleMesh sphere = ellipsoid(40.0, 40.0, 40.0, 90, 180);
    const auto band = selectWhere(sphere, [](const core::Vec3& c) {
        return std::abs(std::atan2(c.y, c.x)) < 75.0 * std::numbers::pi / 180.0 && std::abs(c.z) < 7.2;
    });
    const auto strip = geometry::fitSurface(sphere, band, {});
    CHECK(strip.report.withinTolerance);
    CHECK(strip.report.meshToSurfaceMax < 0.05);

    // About 27 000 triangles: unfolded on a coarsened copy, still checked on all of them.
    const core::TriangleMesh dense = tube(30.0, 80.0, 300, 100);
    // Limits on quad borders (1.2 degree segments, 0.8 mm rows), so no lone triangles.
    const auto region = selectWhere(dense, [](const core::Vec3& c) {
        return std::abs(std::atan2(c.y, c.x)) < 110.4 * std::numbers::pi / 180.0 && c.z > 10.4 && c.z < 69.6;
    });
    REQUIRE(region.size() > 25000);
    const auto fit = geometry::fitSurface(dense, region, {});
    CHECK(fit.report.withinTolerance);
    CHECK(fit.report.meshToSurfaceMax < 0.05);
}

TEST_CASE("Fit cuts holes of the selection out of the surface", "[geometry][fit]")
{
    // Mirror-housing-like shell: lower half of an 80 x 50 x 40 mm ellipsoid, with an
    // opening at the bottom.
    const core::TriangleMesh shell = ellipsoid(80.0, 50.0, 40.0, 90, 180);
    const auto cup = selectWhere(shell, [](const core::Vec3& c) { return c.z < 0.0 && c.z > -36.0; });
    const auto fit = geometry::fitSurface(shell, cup, {});
    CHECK(fit.report.method == "unfold");
    CHECK(fit.report.holes == 1);
    CHECK(fit.report.withinTolerance);
    CHECK(std::abs(geometry::surfaceArea(fit.face) - core::selectionArea(shell, cup)) <
          0.02 * core::selectionArea(shell, cup));

    // Plane method: a flat ring.
    const core::TriangleMesh field = noisyField(60, 0.0);
    const auto ring = selectWhere(field, [](const core::Vec3& c) {
        const double r = std::hypot(c.x - 50.0, c.y - 50.0);
        return r < 30.0 && r > 12.0;
    });
    for (const auto method : {geometry::SurfaceFitMethod::Plane, geometry::SurfaceFitMethod::Unfold}) {
        geometry::SurfaceFitOptions options;
        options.method = method;
        const auto flat = geometry::fitSurface(field, ring, options);
        INFO("method " << flat.report.method);
        CHECK(flat.report.holes == 1);
        CHECK(flat.report.withinTolerance);
        CHECK(std::abs(geometry::surfaceArea(flat.face) - core::selectionArea(field, ring)) <
              0.03 * core::selectionArea(field, ring));
    }
}

TEST_CASE("Fit keeps the outer outline when a wiggly hole is longer", "[geometry][fit]")
{
    // Flat disc of radius 50 with a star-shaped hole (radii 15..40, 24 points): the hole's
    // border is longer than the outer circle.
    const core::TriangleMesh field = noisyField(200, 0.0);
    const auto star = [](double x, double y) {
        const double a = std::atan2(y, x), r = std::hypot(x, y);
        const double t = std::abs(std::fmod(a / (2.0 * std::numbers::pi) * 24.0 + 24.0, 2.0) - 1.0);
        return r < 15.0 + 25.0 * t;
    };
    const auto sheet = selectWhere(field, [&](const core::Vec3& c) {
        const double x = c.x - 50.0, y = c.y - 50.0;
        return std::hypot(x, y) < 48.0 && !star(x, y);
    });
    for (const auto method : {geometry::SurfaceFitMethod::Plane, geometry::SurfaceFitMethod::Unfold}) {
        geometry::SurfaceFitOptions options;
        options.method = method;
        const auto fit = geometry::fitSurface(field, sheet, options);
        INFO("method " << fit.report.method);
        CHECK(fit.report.holes == 1);
        CHECK(std::abs(geometry::surfaceArea(fit.face) - core::selectionArea(field, sheet)) <
              0.05 * core::selectionArea(field, sheet));
    }
}

TEST_CASE("Unfold needs an open sheet; auto falls back to the plane", "[geometry][fit]")
{
    const core::TriangleMesh sphere = ellipsoid(40.0, 40.0, 40.0, 30, 60);
    core::TriangleSet all(sphere.triangleCount());
    for (std::uint32_t t = 0; t < all.size(); ++t)
        all[t] = t;
    geometry::SurfaceFitOptions unfold;
    unfold.method = geometry::SurfaceFitMethod::Unfold;
    CHECK_THROWS_AS(geometry::fitSurface(sphere, all, unfold), geometry::SurfaceFitError);

    // A flat field with a handle-free but non-manifold fan is rare; a closed shell is the
    // common case: auto still returns a (poor) plane fit with warnings.
    const auto fit = geometry::fitSurface(sphere, all, {});
    CHECK(fit.report.method == "plane");
    CHECK_FALSE(fit.report.warnings.empty());
}

TEST_CASE("Fit refuses a surface that covers little of the selection (CAD-255)", "[geometry][fit]")
{
    // A torus with a small patch cut out: one short border but a handle, so it is no
    // sheet. The plane fallback trims to that border and the face is a sliver inside the
    // cut: it used to report deviation 0 "within the tolerance" with nothing measured.
    const double R = 60.0, r = 20.0;
    const int n = 72, m = 36;
    std::vector<core::Vec3> v;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            const double a = 2.0 * std::numbers::pi * i / n, b = 2.0 * std::numbers::pi * j / m;
            v.push_back({(R + r * std::cos(b)) * std::cos(a), (R + r * std::cos(b)) * std::sin(a), r * std::sin(b)});
        }
    auto id = [&](int i, int j) { return static_cast<std::uint32_t>((i % n) * m + (j % m)); };
    std::vector<core::TriangleMesh::Triangle> t;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < m; ++j) {
            t.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
            t.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
        }
    const core::TriangleMesh torus(std::move(v), std::move(t));
    const core::TriangleSet open = selectWhere(torus, [&](const core::Vec3& c) {
        return (c - core::Vec3{R + r, 0, 0}).length() > 8.0;
    });
    REQUIRE(open.size() < torus.triangleCount());
    // Before CAD-255: a face of 1e-13 mm^2, meshToSurfaceMax 0, outsideTrimMax 159 mm.
    std::string error;
    try {
        (void)geometry::fitSurface(torus, open, {});
    } catch (const geometry::SurfaceFitError& e) {
        error = e.what();
    }
    CHECK(error.find("covers only") != std::string::npos);
}

TEST_CASE("Fit on extra data and trim to a subset of it", "[geometry][fit]")
{
    const core::TriangleMesh mesh = tube(30.0, 80.0, 180, 60);
    const auto data = selectWhere(mesh, [](const core::Vec3& c) {
        return std::abs(std::atan2(c.y, c.x)) < 110.0 * std::numbers::pi / 180.0 && c.z > 10.0 && c.z < 70.0;
    });
    const auto trim = selectWhere(mesh, [](const core::Vec3& c) {
        return std::abs(std::atan2(c.y, c.x)) < 60.0 * std::numbers::pi / 180.0 && c.z > 20.0 && c.z < 60.0;
    });
    for (const auto method : {geometry::SurfaceFitMethod::Unfold, geometry::SurfaceFitMethod::Plane}) {
        geometry::SurfaceFitOptions options;
        options.method = method;
        options.trimTo = trim;
        const auto fit = geometry::fitSurface(mesh, data, options);
        INFO(fit.report.method);
        CHECK(fit.report.trimmed);
        // The face covers the trim subset only, and only its points are measured.
        CHECK(std::abs(geometry::surfaceArea(fit.face) - core::selectionArea(mesh, trim)) <
              0.02 * core::selectionArea(mesh, trim));
        if (method == geometry::SurfaceFitMethod::Unfold) {
            CHECK(fit.report.withinTolerance);
            CHECK(fit.report.outsideTrimMax < 0.5); // the smoothed outline vs. the triangle staircase
        }
    }
}

namespace {

/// A rough scan: a curved outer skin (z = f(x, y)) with noise, a hole through it, a layer
/// 4 mm behind it and a deep pit in the middle of the outline (a grille opening).
core::TriangleMesh roughScan()
{
    std::mt19937 rng(3);
    std::normal_distribution<double> noise(0.0, 0.05);
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int n = 300; // 0.5 mm
    auto skin = [](double x, double y) { return 12.0 * std::sin(x / 40.0) + 0.002 * y * y; };
    auto addSheet = [&](double offset, bool outer) {
        const auto base = static_cast<std::uint32_t>(v.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                const double x = i * 0.5, y = j * 0.5;
                double z = skin(x, y) + offset + noise(rng);
                if (outer && std::hypot(x - 90.0, y - 60.0) < 6.0)
                    z -= 30.0; // the pit
                v.push_back({x, y, z});
            }
        auto idx = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (n + 1) + i); };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                if (outer && std::hypot(i * 0.5 - 50.0, j * 0.5 - 90.0) < 5.0)
                    continue; // the hole
                t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
                t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
            }
    };
    addSheet(0.0, true);
    addSheet(-4.0, false);
    return core::TriangleMesh(std::move(v), std::move(t));
}

geometry::Shape rectangleOutline(const geometry::Plane& plane, double x, double y, double w, double h)
{
    using K = geometry::SketchCurve::Kind;
    const std::vector<geometry::Vec2> c{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    std::vector<geometry::SketchCurve> curves;
    for (std::size_t i = 0; i < 4; ++i)
        curves.push_back({K::Line, {c[i], c[(i + 1) % 4]}});
    return geometry::buildSketch(plane, curves).profiles.at(0);
}

geometry::Shape circleOutline(const geometry::Plane& plane, double x, double y, double r)
{
    return geometry::buildSketch(plane, {{geometry::SketchCurve::Kind::Circle, {{x, y}}, r}}).profiles.at(0);
}

geometry::DrapeArea areaOver(double u0, double v0, double u1, double v1, double margin)
{
    geometry::DrapeArea a;
    a.origin = {0, 0, 50};
    a.x = {1, 0, 0};
    a.y = {0, 1, 0};
    a.normal = {0, 0, 1};
    a.u0 = u0 - margin;
    a.v0 = v0 - margin;
    a.u1 = u1 + margin;
    a.v1 = v1 + margin;
    a.insideU0 = u0;
    a.insideV0 = v0;
    a.insideU1 = u1;
    a.insideV1 = v1;
    return a;
}

} // namespace

TEST_CASE("A part on a scan follows the outer skin across holes, pits and inner layers", "[geometry][surface]")
{
    const auto scan = roughScan();
    const core::MeshProjector projector(scan, {0, 0, 1});
    const double side = 100.0;
    const geometry::Plane plane{{0, 0, 50}, {0, 0, 1}, {1, 0, 0}};
    const auto outline = rectangleOutline(plane, 30, 30, side, side);

    const auto outside = geometry::partOnScan(projector, areaOver(30, 30, 130, 130, 10), {outline}, 2.0,
                                              geometry::ThickenSide::Outside);
    CHECK(outside.report.rmsDeviation < 0.2);
    CHECK(outside.report.bridgedFraction > 0.0);
    CHECK(outside.report.bridgedFraction < 0.05);
    REQUIRE_FALSE(outside.solid.isNull());
    // Slightly more than 100 x 100: the skin is curved. The thickness is along its normal.
    CHECK(outside.skinArea > side * side);
    CHECK(outside.skinArea < 1.2 * side * side);
    CHECK(outside.solid.volume() == Approx(2.0 * outside.skinArea).epsilon(0.03));

    const auto mesh = geometry::tessellate(outside.solid);
    const auto box = mesh.bounds();
    CHECK(box.size().x == Approx(side).margin(0.1));
    CHECK(box.size().y == Approx(side).margin(0.1));
    // Rides on the skin at every point, not on the layer behind, not down the pit.
    double lowest = 1e9;
    for (const auto& p : mesh.vertices())
        lowest = std::min(lowest, p.z - (12.0 * std::sin(p.x / 40.0) + 0.002 * p.y * p.y));
    CHECK(lowest > -1.0);

    const auto inside = geometry::partOnScan(projector, areaOver(30, 30, 130, 130, 10), {outline}, 2.0,
                                             geometry::ThickenSide::Inside);
    CHECK(inside.solid.volume() == Approx(2.0 * inside.skinArea).epsilon(0.03));
    CHECK(geometry::tessellate(inside.solid).bounds().max.z < box.max.z - 1.0);
}

TEST_CASE("A part on a scan: outline past the scan's edge, curved outline, no scan under it", "[geometry][surface]")
{
    const auto scan = roughScan();
    const core::MeshProjector projector(scan, {0, 0, 1});
    const geometry::Plane plane{{0, 0, 50}, {0, 0, 1}, {1, 0, 0}};

    // Reaches 20 mm past the scan: still one clean part, and the report says so.
    const auto past = geometry::partOnScan(projector, areaOver(100, 40, 170, 110, 10),
                                           {rectangleOutline(plane, 100, 40, 70, 70)}, 2.0,
                                           geometry::ThickenSide::Outside);
    CHECK(past.report.bridgedFraction > 0.2);
    CHECK(geometry::tessellate(past.solid).bounds().size().x == Approx(70.0).margin(0.1));

    const auto round = geometry::partOnScan(projector, areaOver(40, 40, 100, 100, 10),
                                            {circleOutline(plane, 70, 70, 30)}, 3.0,
                                            geometry::ThickenSide::Both);
    CHECK(round.solid.volume() == Approx(3.0 * round.skinArea).epsilon(0.05));
    CHECK(round.skinArea == Approx(std::numbers::pi * 900.0).epsilon(0.1));

    CHECK_THROWS_AS(geometry::partOnScan(projector, areaOver(1000, 1000, 1100, 1100, 10),
                                         {rectangleOutline(plane, 1000, 1000, 100, 100)}, 2.0,
                                         geometry::ThickenSide::Outside),
                    geometry::SurfaceFitError);
}

TEST_CASE("A part on a scan is as thick as asked on a slope, also when thinner than the sampling", "[geometry][surface]")
{
    // A plane at 45 degrees; the 100 mm outline with its margin samples at about 0.83 mm.
    const core::TriangleMesh slope({{-50, -50, -50}, {200, -50, 200}, {200, 200, 200}, {-50, 200, -50}},
                                   {{0, 1, 2}, {0, 2, 3}});
    const core::MeshProjector projector(slope, {0, 0, 1});
    const geometry::Plane plane{{0, 0, 300}, {0, 0, 1}, {1, 0, 0}};
    const auto outline = rectangleOutline(plane, 0, 0, 100, 100);
    geometry::DrapeArea area = areaOver(0, 0, 100, 100, 10);
    area.origin = plane.origin;

    for (const double thickness : {0.5, 1.0, 2.0, 6.0}) {
        const auto part = geometry::partOnScan(projector, area, {outline}, thickness, geometry::ThickenSide::Outside);
        INFO("thickness " << thickness);
        CHECK(part.solid.volume() == Approx(thickness * part.skinArea).epsilon(0.02));
    }
    const auto both = geometry::partOnScan(projector, area, {outline}, 1.0, geometry::ThickenSide::Both);
    CHECK(both.solid.volume() == Approx(1.0 * both.skinArea).epsilon(0.02));
}

namespace {

/// A gently curved sheet sampled every 1 mm (z = f(x, y)), with normal noise of `sigma` mm.
core::TriangleMesh noisySheet(double sigma, double (*shape)(double, double))
{
    std::mt19937 rng(11);
    std::normal_distribution<double> noise(0.0, sigma);
    std::vector<core::Vec3> v;
    std::vector<core::TriangleMesh::Triangle> t;
    const int n = 160;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({double(i), double(j), shape(i, j) + (sigma > 0.0 ? noise(rng) : 0.0)});
    auto idx = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            t.push_back({idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)});
            t.push_back({idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)});
        }
    return core::TriangleMesh(std::move(v), std::move(t));
}

double gentle(double x, double y) { return 10.0 * std::sin(x / 50.0) + 0.001 * y * y; }

} // namespace

TEST_CASE("A part on a noisy scan runs through the noise and is not called an approximation (CAD-141)",
          "[geometry][surface]")
{
    const geometry::Plane plane{{0, 0, 50}, {0, 0, 1}, {1, 0, 0}};
    const auto outline = rectangleOutline(plane, 20, 20, 120, 120);

    // Noise of 0.2 mm: 99% of the scan is within about 0.5 mm of its true shape, more than
    // the 0.25 mm tolerance, so no surface can follow the scan within it.
    const auto noisy = noisySheet(0.2, gentle);
    const core::MeshProjector projector(noisy, {0, 0, 1});
    const auto part = geometry::partOnScan(projector, areaOver(20, 20, 140, 140, 10), {outline}, 2.0,
                                           geometry::ThickenSide::Outside);
    INFO("noise " << part.report.scanNoise << ", p99 " << part.report.p99Deviation << ", rms "
                  << part.report.rmsDeviation << ", poles " << part.report.uPoles << " x " << part.report.vPoles);
    CHECK(part.report.p99Deviation > 0.25);
    // Samples between the scan's points average a few of them: less noise than at a point.
    CHECK(part.report.scanNoise > 0.1);
    CHECK(part.report.scanNoise < 0.2);
    CHECK(part.report.withinScanNoise);
    // It does not chase the noise with fine control points (64 x 64 at the finest), so the
    // skin is much closer to the true shape than the scan is.
    CHECK(part.report.uPoles < 40);
    double squares = 0.0;
    std::size_t count = 0;
    const auto skin = geometry::tessellate(part.solid);
    for (const auto& p : skin.vertices()) {
        const double d = p.z - gentle(p.x, p.y);
        if (std::abs(d) < 1.0 && p.x > 25 && p.x < 135 && p.y > 25 && p.y < 135) {
            squares += d * d;
            ++count;
        }
    }
    REQUIRE(count > 20);
    CHECK(std::sqrt(squares / double(count)) < 0.05);

    // Without noise nothing is mistaken for it.
    const auto clean = noisySheet(0.0, gentle);
    const core::MeshProjector cleanProjector(clean, {0, 0, 1});
    geometry::DrapeFitOptions tight;
    tight.tolerance = 0.001;
    const auto exact = geometry::partOnScan(cleanProjector, areaOver(20, 20, 140, 140, 10), {outline}, 2.0,
                                            geometry::ThickenSide::Outside, tight);
    CHECK(exact.report.scanNoise < 0.01);
    CHECK_FALSE(exact.report.withinScanNoise);
}

namespace {

double corrugated(double x, double y)
{
    return 0.5 * std::sin(2.0 * std::numbers::pi * x / 5.0) + 0.5 * std::sin(2.0 * std::numbers::pi * y / 5.0);
}

double plain(double, double) { return 0.0; }

} // namespace

TEST_CASE("Fine clean shape is not taken for scan noise; a thin noisy outline is (CAD-142, Codex)",
          "[geometry][surface]")
{
    const geometry::Plane plane{{0, 0, 50}, {0, 0, 1}, {1, 0, 0}};

    // A clean 5 mm corrugation, finer than the control points: the noise estimate is
    // large, but the deviation left has a pattern, so it stays an approximation.
    const auto rippled = noisySheet(0.0, corrugated);
    const core::MeshProjector projector(rippled, {0, 0, 1});
    const auto part = geometry::partOnScan(projector, areaOver(10, 10, 150, 150, 0),
                                           {rectangleOutline(plane, 10, 10, 140, 140)}, 2.0,
                                           geometry::ThickenSide::Outside);
    INFO("noise " << part.report.scanNoise << ", p99 " << part.report.p99Deviation);
    CHECK(part.report.p99Deviation > 0.25);
    CHECK_FALSE(part.report.withinScanNoise);

    // A 120 x 12 mm strip (a fin's root) on a noisy flat scan: the noise is still told
    // from the samples along it.
    const auto noisy = noisySheet(0.2, plain);
    const core::MeshProjector noisyProjector(noisy, {0, 0, 1});
    geometry::DrapeArea strip = areaOver(20, 70, 140, 82, 0);
    geometry::DrapeFitReport report;
    (void)geometry::skinOnScan(noisyProjector, strip, &report);
    INFO("strip noise " << report.scanNoise << ", p99 " << report.p99Deviation);
    CHECK(report.scanNoise > 0.1);
}
