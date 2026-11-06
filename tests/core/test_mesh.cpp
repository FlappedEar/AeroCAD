// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/Path.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

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

TEST_CASE("UTF-8 path round trip", "[core]")
{
    const std::string name = "mod\xC5\x82o\xC5\xBC.stl"; // "modłoż.stl"
    CHECK(pathToUtf8(pathFromUtf8(name)) == name);
}
