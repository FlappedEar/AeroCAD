// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/MeshSection.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Closed axis-aligned box [x0, x1] x [y0, y1] x [z0, z1], outward winding.
TriangleMesh box(double x0, double y0, double z0, double x1, double y1, double z1)
{
    MeshBuilder b;
    const Vec3 p[8] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                       {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    auto quad = [&](int a, int c, int d, int e) {
        b.addTriangle(p[a], p[c], p[d]);
        b.addTriangle(p[a], p[d], p[e]);
    };
    quad(0, 3, 2, 1); // bottom (-z)
    quad(4, 5, 6, 7); // top (+z)
    quad(0, 1, 5, 4); // -y
    quad(2, 3, 7, 6); // +y
    quad(1, 2, 6, 5); // +x
    quad(3, 0, 4, 7); // -x
    return b.take();
}

std::shared_ptr<const TriangleMesh> shared(TriangleMesh m)
{
    return std::make_shared<const TriangleMesh>(std::move(m));
}

} // namespace

TEST_CASE("MeshBvh closest point and ray cast", "[core][inspect]")
{
    const MeshBvh bvh(shared(box(0, 0, 0, 10, 10, 10)));

    const auto above = bvh.closestPoint({5, 5, 13});
    REQUIRE(above);
    CHECK(above->distance == Approx(3));
    CHECK(above->point.z == Approx(10));

    const auto corner = bvh.closestPoint({-3, -4, 5});
    REQUIRE(corner);
    CHECK(corner->distance == Approx(5));

    CHECK_FALSE(bvh.closestPoint({5, 5, 13}, 2.0));

    const auto hit = bvh.raycast({5, 5, -5}, {0, 0, 2});
    REQUIRE(hit);
    CHECK(hit->t == Approx(5));
    CHECK(hit->point.z == Approx(0));
    CHECK_FALSE(bvh.raycast({5, 5, -5}, {0, 0, -1}));
    CHECK_FALSE(bvh.raycast({5, 5, -5}, {0, 0, 1}, 4.0));
    CHECK_FALSE(bvh.raycast({50, 5, 5}, {1, 0, 0})); // misses every box, infinite limit
}

TEST_CASE("Mesh distances, deviation and thickness", "[core][inspect]")
{
    const MeshBvh a(shared(box(0, 0, 0, 10, 10, 10)));
    const MeshBvh b(shared(box(13, 2, 2, 20, 8, 8)));
    const auto d = minimumDistance(a, b);
    REQUIRE(d);
    CHECK(d->distance == Approx(3));
    CHECK(d->pointA.x == Approx(10));
    CHECK(d->pointB.x == Approx(13));

    // A slightly larger box measured against the reference: +0.5 everywhere outside.
    const TriangleMesh scan = box(-0.5, -0.5, -0.5, 10.5, 10.5, 10.5);
    const auto dev = signedVertexDistances(scan, a);
    REQUIRE(dev.size() == scan.vertexCount());
    for (const double v : dev)
        CHECK(v == Approx(std::sqrt(0.75))); // corners: sqrt(3 * 0.5^2)
    const TriangleMesh inner = box(1, 1, 1, 9, 9, 9);
    for (const double v : signedVertexDistances(inner, a))
        CHECK(v == Approx(-1));

    const MeshBvh plate(shared(box(0, 0, 0, 40, 40, 2)));
    const auto thick = vertexThickness(plate);
    const ValueStats s = valueStats(thick);
    REQUIRE(s.count > 0);
    CHECK(s.min == Approx(2).margin(1e-6));
    CHECK(s.max == Approx(2).margin(1e-6)); // every vertex touches the top or bottom face

    const ValueStats st = valueStats({1.0, std::nan(""), 3.0});
    CHECK(st.count == 2);
    CHECK(st.mean == Approx(2));
    CHECK(st.max == Approx(3));
    CHECK(st.maxIndex == 2);
}

TEST_CASE("Mesh section: clip, curves and area", "[core][inspect]")
{
    const TriangleMesh cube = box(0, 0, 0, 10, 10, 10);
    const CutPlane plane{{0, 0, 4}, {0, 0, 1}};

    const TriangleMesh clipped = clipMesh(cube, plane);
    const BoundingBox bb = clipped.bounds();
    CHECK(bb.max.z == Approx(4));
    CHECK(bb.min.z == Approx(0));
    // Bottom 100 + four sides 4 x 10 = 260 (open at the cut).
    CHECK(clipped.surfaceArea() == Approx(260));

    const auto curves = sectionCurves(cube, plane);
    REQUIRE(curves.size() == 1);
    CHECK(curves[0].closed);
    CHECK(curves[0].length() == Approx(40));
    CHECK(sectionArea(curves, plane) == Approx(100));

    // Hollow tube: outer 10x10 minus inner 6x6 (inner box with inward-facing walls).
    TriangleMesh tube = box(0, 0, 0, 10, 10, 10);
    const TriangleMesh hole = box(2, 2, -1, 8, 8, 11);
    for (std::size_t t = 0; t < hole.triangleCount(); ++t) {
        const auto& tri = hole.triangles()[t];
        const auto base = static_cast<std::uint32_t>(tube.vertexCount());
        tube.addVertex(hole.vertices()[tri[0]]);
        tube.addVertex(hole.vertices()[tri[1]]);
        tube.addVertex(hole.vertices()[tri[2]]);
        tube.addTriangle(base, base + 2, base + 1);
    }
    const auto tubeCurves = sectionCurves(tube, plane);
    REQUIRE(tubeCurves.size() == 2);
    const auto depth = nestingDepths(tubeCurves, plane);
    CHECK(depth[0] + depth[1] == 1);
    CHECK(sectionArea(tubeCurves, plane) == Approx(100 - 36));

    // Diagonal plane through four vertices: still one closed loop.
    const CutPlane diagonal{{10, 0, 0}, {1, 1, 0}};
    const auto onVertices = sectionCurves(cube, diagonal);
    REQUIRE(onVertices.size() == 1);
    CHECK(onVertices[0].closed);
    CHECK(sectionArea(onVertices, diagonal) == Approx(std::sqrt(200.0) * 10));
    CHECK(clipMesh(cube, diagonal).bounds().max.x == Approx(10));

    // Open surface (a single square) gives an open polyline.
    MeshBuilder sq;
    sq.addTriangle({0, 0, 0}, {10, 0, 0}, {10, 10, 0});
    sq.addTriangle({0, 0, 0}, {10, 10, 0}, {0, 10, 0});
    const auto open = sectionCurves(sq.take(), {{5, 0, 0}, {1, 0, 0}});
    REQUIRE(open.size() == 1);
    CHECK_FALSE(open[0].closed);
    CHECK(open[0].length() == Approx(10));

    CHECK(clipMesh(cube, {{0, 0, -1}, {0, 0, 1}}).isEmpty());
    CHECK(clipMesh(cube, {{0, 0, 11}, {0, 0, 1}}).triangleCount() == cube.triangleCount());
}
