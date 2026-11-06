// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"
#include "aerocad/geometry/Tessellation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

using namespace aerocad;

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

} // namespace

TEST_CASE("Fit a trimmed surface to a noisy selected region", "[geometry][fit]")
{
    const core::TriangleMesh mesh = noisyField(120, 0.05);
    const core::TriangleSet selection = core::selectInSphere(mesh, {50, 50, truth(50, 50)}, 30.0);
    REQUIRE(selection.size() > 1000);

    const auto fit = geometry::fitSurface(mesh, selection, {});
    REQUIRE_FALSE(fit.face.isNull());
    CHECK(fit.face.faceCount() == 1);
    CHECK(fit.report.trimmed);
    CHECK(fit.report.meshToSurfaceMax < 0.25);
    CHECK(fit.report.meshToSurfaceRms < 0.08);
    CHECK(fit.report.surfaceToMeshMax < 0.25);
    CHECK(fit.report.withinTolerance);

    // Trimmed to the selection: about the 60 mm disc, not the whole field.
    const auto size = fit.face.bounds().size();
    CHECK(size.x < 64.0);
    CHECK(size.x > 54.0);
    CHECK(size.y < 64.0);
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
