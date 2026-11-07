// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshCheck.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace aerocad::core;
using Catch::Approx;

namespace {

/// Axis-aligned cube [lo, hi]^3 with outward normals (or inward when `inward`).
TriangleMesh cube(double lo, double hi, bool inward = false)
{
    std::vector<Vec3> v;
    for (int i = 0; i < 8; ++i)
        v.push_back({i & 1 ? hi : lo, i & 2 ? hi : lo, i & 4 ? hi : lo});
    std::vector<TriangleMesh::Triangle> t{{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                                          {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    if (inward) {
        for (auto& tri : t)
            std::swap(tri[1], tri[2]);
    }
    return TriangleMesh(std::move(v), std::move(t));
}

TriangleMesh translated(const TriangleMesh& m, const Vec3& d)
{
    std::vector<Vec3> v;
    for (const auto& p : m.vertices())
        v.push_back(p + d);
    return TriangleMesh(std::move(v), m.triangles());
}

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
        CHECK(c.problems()[0] == "normals point inwards: 1 inside-out shell");
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

TEST_CASE("Orientation is checked per shell, cavities must point inwards", "[core][check]")
{
    SECTION("inverted small body next to a large one")
    {
        TriangleMesh m = tetrahedron();
        TriangleMesh small({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}});
        m.append(translated(small, {50, 0, 0}));
        const MeshCheck c = checkMesh(m);
        CHECK(c.signedVolume > 0.0); // the total alone would hide the inverted body
        CHECK(c.invertedShells == 1);
        CHECK_FALSE(c.oriented());
        CHECK_FALSE(c.watertight());
    }
    SECTION("hollow cube with an inward-facing cavity is valid")
    {
        TriangleMesh m = cube(0, 10);
        m.append(cube(3, 7, true));
        const MeshCheck c = checkMesh(m);
        CHECK(c.shells == 2);
        CHECK(c.invertedShells == 0);
        CHECK(c.watertight());
        CHECK(c.signedVolume == Approx(1000.0 - 64.0));
    }
    SECTION("cavity facing outwards is inverted")
    {
        TriangleMesh m = cube(0, 10);
        m.append(cube(3, 7));
        const MeshCheck c = checkMesh(m);
        CHECK(c.invertedShells == 1);
        CHECK_FALSE(c.watertight());
    }
    SECTION("solid inside a cavity is an outer shell again")
    {
        TriangleMesh m = cube(0, 10);
        m.append(cube(2, 8, true));
        m.append(cube(4, 6));
        const MeshCheck c = checkMesh(m);
        CHECK(c.shells == 3);
        CHECK(c.watertight());
        CHECK(c.signedVolume == Approx(1000.0 - 216.0 + 8.0));
    }
}

TEST_CASE("Volume of a tiny body far from the origin keeps its sign", "[core][check]")
{
    const TriangleMesh far = translated(
        TriangleMesh({{0, 0, 0}, {0.1, 0, 0}, {0, 0.1, 0}, {0, 0, 0.1}}, {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}),
        {100000, 100000, 100000});
    const MeshCheck c = checkMesh(far);
    CHECK(c.watertight());
    CHECK(c.signedVolume == Approx(0.001 / 6.0).epsilon(1e-6));
}

TEST_CASE("Triangles collapsed to an edge are ignored by the topology", "[core][check]")
{
    // A tessellation artefact: a triangle with two corners at the same position on an
    // existing edge. It must not turn that edge into a non-manifold one.
    TriangleMesh m = tetrahedron();
    const auto dup = m.addVertex({0, 0, 0}); // same position as vertex 0
    m.addTriangle(0, dup, 1);
    const MeshCheck c = checkMesh(m);
    CHECK(c.degenerateTriangles == 1);
    CHECK(c.nonManifoldEdges == 0);
    CHECK(c.closed());
    CHECK(c.oriented());
    CHECK(c.shells == 1);
    CHECK_FALSE(c.watertight()); // still reported as degenerate

    const TriangleMesh welded = weldVertices(m);
    CHECK(welded.triangleCount() == 4);
    CHECK(checkMesh(welded).watertight());
}
