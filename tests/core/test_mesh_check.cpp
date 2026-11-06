// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshCheck.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Closed tetrahedron with outward normals, volume 1000/6 mm^3.
TriangleMesh tetrahedron()
{
    return TriangleMesh({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}});
}

} // namespace

TEST_CASE("Closed tetrahedron is watertight", "[core][check]")
{
    const MeshCheck c = checkMesh(tetrahedron());
    CHECK(c.watertight());
    CHECK(c.closed());
    CHECK(c.manifold());
    CHECK(c.oriented());
    CHECK(c.shells == 1);
    CHECK(c.vertices == 4);
    CHECK(c.signedVolume == Approx(1000.0 / 6.0));
    CHECK(c.problems().empty());
}

TEST_CASE("Mesh check finds holes, flipped and inverted triangles", "[core][check]")
{
    SECTION("open mesh")
    {
        TriangleMesh m({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}});
        const MeshCheck c = checkMesh(m);
        CHECK_FALSE(c.closed());
        CHECK(c.boundaryEdges == 3);
        CHECK(c.manifold());
        CHECK(c.oriented());
        CHECK_FALSE(c.watertight());
        REQUIRE(c.problems().size() == 1);
        CHECK(c.problems()[0] == "not closed: 3 open edges");
    }
    SECTION("one flipped triangle")
    {
        TriangleMesh m({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 3, 2}});
        const MeshCheck c = checkMesh(m);
        CHECK(c.closed());
        CHECK(c.inconsistentEdges == 3);
        CHECK_FALSE(c.oriented());
    }
    SECTION("all normals inwards")
    {
        TriangleMesh m({{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}}, {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}});
        const MeshCheck c = checkMesh(m);
        CHECK(c.closed());
        CHECK(c.inconsistentEdges == 0);
        CHECK(c.signedVolume < 0.0);
        CHECK_FALSE(c.oriented());
        REQUIRE(c.problems().size() == 1);
        CHECK(c.problems()[0] == "normals point inwards (negative volume)");
    }
    SECTION("non-manifold edge: three triangles on one edge")
    {
        TriangleMesh m = tetrahedron();
        const auto extra = m.addVertex({5, -10, 0});
        m.addTriangle(0, 1, extra);
        const MeshCheck c = checkMesh(m);
        CHECK(c.nonManifoldEdges == 1);
        CHECK_FALSE(c.manifold());
        CHECK_FALSE(c.closed());
    }
    SECTION("degenerate triangle")
    {
        TriangleMesh m = tetrahedron();
        const auto mid = m.addVertex({5, 0, 0});
        m.addTriangle(0, mid, 1); // collinear: zero area
        CHECK(checkMesh(m).degenerateTriangles == 1);
    }
    SECTION("empty")
    {
        const MeshCheck c = checkMesh(TriangleMesh{});
        CHECK_FALSE(c.watertight());
        CHECK(c.problems() == std::vector<std::string>{"mesh is empty"});
    }
}

TEST_CASE("Unwelded per-face vertices are checked by position", "[core][check]")
{
    // Same tetrahedron, every triangle with its own three vertices (like a B-rep tessellation).
    const TriangleMesh welded = tetrahedron();
    TriangleMesh soup;
    for (const auto& t : welded.triangles()) {
        const auto a = soup.addVertex(welded.vertices()[t[0]]);
        const auto b = soup.addVertex(welded.vertices()[t[1]]);
        const auto c = soup.addVertex(welded.vertices()[t[2]]);
        soup.addTriangle(a, b, c);
    }
    REQUIRE(soup.vertexCount() == 12);
    const MeshCheck c = checkMesh(soup);
    CHECK(c.watertight());
    CHECK(c.vertices == 4);

    const TriangleMesh merged = weldVertices(soup);
    CHECK(merged.vertexCount() == 4);
    CHECK(merged.triangleCount() == 4);
    CHECK(checkMesh(merged).watertight());
}

TEST_CASE("Separate bodies are counted as shells", "[core][check]")
{
    TriangleMesh m = tetrahedron();
    TriangleMesh other = tetrahedron();
    TriangleMesh shifted;
    for (const auto& v : other.vertices())
        shifted.addVertex({v.x + 100, v.y, v.z});
    for (const auto& t : other.triangles())
        shifted.addTriangle(t[0], t[1], t[2]);
    m.append(shifted);
    const MeshCheck c = checkMesh(m);
    CHECK(c.shells == 2);
    CHECK(c.watertight());
    CHECK(c.signedVolume == Approx(2000.0 / 6.0));
}
