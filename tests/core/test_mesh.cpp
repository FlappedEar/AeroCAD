// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/Path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace aerocad::core;
using Catch::Approx;

TEST_CASE("BoundingBox starts empty and grows", "[core]")
{
    BoundingBox box;
    CHECK(box.isEmpty());
    CHECK(box.diagonal() == 0.0);
    box.extend({1, 2, 3});
    CHECK_FALSE(box.isEmpty());
    box.extend({-1, 0, 5});
    CHECK(box.min == Vec3{-1, 0, 3});
    CHECK(box.max == Vec3{1, 2, 5});
    CHECK(box.center() == Vec3{0, 1, 4});
    CHECK(box.size() == Vec3{2, 2, 2});
}

TEST_CASE("Vec3 arithmetic", "[core]")
{
    const Vec3 x{1, 0, 0};
    const Vec3 y{0, 1, 0};
    CHECK(x.cross(y) == Vec3{0, 0, 1});
    CHECK(x.dot(y) == 0.0);
    CHECK(Vec3{3, 4, 0}.length() == Approx(5.0));
    CHECK(Vec3{}.normalized() == Vec3{});
}

TEST_CASE("TriangleMesh validates indices", "[core]")
{
    TriangleMesh mesh;
    mesh.addVertex({0, 0, 0});
    mesh.addVertex({1, 0, 0});
    mesh.addVertex({0, 1, 0});
    mesh.addTriangle(0, 1, 2);
    CHECK(mesh.isValid());
    CHECK_THROWS_AS(mesh.addTriangle(0, 1, 3), std::out_of_range);
    CHECK_THROWS_AS(TriangleMesh({{0, 0, 0}}, {{0, 0, 1}}), std::invalid_argument);
}

TEST_CASE("TriangleMesh geometry queries", "[core]")
{
    TriangleMesh mesh;
    mesh.addVertex({0, 0, 0});
    mesh.addVertex({2, 0, 0});
    mesh.addVertex({0, 2, 0});
    mesh.addTriangle(0, 1, 2);
    CHECK(mesh.surfaceArea() == Approx(2.0));
    CHECK(mesh.faceNormal(0) == Vec3{0, 0, 1});

    mesh.scale(2.0, {0, 0, 0});
    CHECK(mesh.surfaceArea() == Approx(8.0));
    CHECK(mesh.bounds().max == Vec3{4, 4, 0});

    TriangleMesh other = mesh;
    mesh.append(other);
    CHECK(mesh.vertexCount() == 6);
    CHECK(mesh.triangleCount() == 2);
    CHECK(mesh.triangles()[1] == TriangleMesh::Triangle{3, 4, 5});
}

TEST_CASE("MeshBuilder welds identical vertices and drops degenerate triangles", "[core]")
{
    MeshBuilder b;
    const Vec3 p0{0, 0, 0}, p1{1, 0, 0}, p2{0, 1, 0}, p3{1, 1, 0};
    b.addTriangle(p0, p1, p2);
    b.addTriangle(p1, p3, p2);
    b.addTriangle(p0, p0, p1); // degenerate
    b.addTriangle({-0.0, 0, 0}, p1, p3); // -0.0 welds with 0.0
    const TriangleMesh mesh = b.take();
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.triangleCount() == 3);
    CHECK(mesh.isValid());
    CHECK(b.triangleCount() == 0);
}

TEST_CASE("MeshBuilder welds large grids across table growth and batches", "[core]")
{
    // An n x n vertex grid, two triangles per cell, added once triangle by triangle and
    // once in batches (with and without a reserve hint): all must give the same mesh.
    constexpr int n = 300;
    std::vector<Vec3> corners;
    for (int i = 0; i + 1 < n; ++i) {
        for (int j = 0; j + 1 < n; ++j) {
            const double x = i * 0.1;
            const double y = j == 0 ? -0.0 : j * 0.1; // -0.0 must weld with 0.0 from other cells
            const double x1 = (i + 1) * 0.1;
            const double y1 = (j + 1) * 0.1;
            corners.insert(corners.end(), {{x, y, 0}, {x1, y, 0}, {x1, y1, 0}, {x, y, 0}, {x1, y1, 0}, {x, y1, 0}});
        }
    }
    corners.insert(corners.end(), {{0, 0, 0}, {0, 0, 0}, {1, 1, 1}}); // degenerate
    const std::size_t triangles = corners.size() / 3;

    MeshBuilder single;
    for (std::size_t t = 0; t < triangles; ++t)
        single.addTriangle(corners[3 * t], corners[3 * t + 1], corners[3 * t + 2]);
    const TriangleMesh a = single.take();

    MeshBuilder batched;
    batched.addTriangles(corners.data(), triangles);
    const TriangleMesh b = batched.take();

    MeshBuilder reserved;
    reserved.reserve(triangles);
    reserved.addTriangles(corners.data(), 7); // uneven batch boundaries
    reserved.addTriangles(corners.data() + 21, triangles - 7);
    const TriangleMesh c = reserved.take();

    const std::size_t expectedVertices = static_cast<std::size_t>(n) * n + 1; // + the (1,1,1) corner
    for (const TriangleMesh* mesh : {&a, &b, &c}) {
        CHECK(mesh->vertexCount() == expectedVertices);
        CHECK(mesh->triangleCount() == triangles - 1);
        CHECK(mesh->isValid());
        CHECK(mesh->vertices() == a.vertices());
        CHECK(mesh->triangles() == a.triangles());
    }
    for (const Vec3& v : a.vertices())
        CHECK_FALSE(std::signbit(v.y));
}

TEST_CASE("UTF-8 path round trip", "[core]")
{
    const std::string name = "mod\xC5\x82o\xC5\xBC.stl"; // "modłoż.stl"
    CHECK(pathToUtf8(pathFromUtf8(name)) == name);
}

// The bounds cache must not stop containers from moving meshes.
static_assert(std::is_nothrow_move_constructible_v<aerocad::core::TriangleMesh>);

TEST_CASE("TriangleMesh bounds follow every change", "[core][mesh]")
{
    aerocad::core::TriangleMesh m;
    m.addVertex({0, 0, 0});
    m.addVertex({1, 0, 0});
    m.addVertex({0, 2, 0});
    m.addTriangle(0, 1, 2);
    CHECK(m.bounds().max.y == 2.0);
    CHECK(m.bounds().max.y == 2.0); // cached
    m.addVertex({0, 0, 5});
    CHECK(m.bounds().max.z == 5.0);
    m.scale(2.0, {0, 0, 0});
    CHECK(m.bounds().max.z == 10.0);
    aerocad::core::TriangleMesh other;
    other.addVertex({-3, 0, 0});
    other.addVertex({-2, 0, 0});
    other.addVertex({-3, 1, 0});
    other.addTriangle(0, 1, 2);
    m.append(other);
    CHECK(m.bounds().min.x == -3.0);
    const aerocad::core::TriangleMesh copy = m;
    CHECK(copy.bounds().min.x == -3.0);
    m = other; // assignment replaces the geometry and the box
    CHECK(m.bounds().max.z == 0.0);
    CHECK(m.bounds().max.x == -2.0);
}
