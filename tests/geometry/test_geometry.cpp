// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/geometry/Primitives.hpp"
#include "aerocad/geometry/Tessellation.hpp"
#include "aerocad/geometry/Transform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numbers>

using namespace aerocad;
using Catch::Approx;

TEST_CASE("Null shape", "[geometry]")
{
    geometry::Shape s;
    CHECK(s.isNull());
    CHECK(s.bounds().isEmpty());
    CHECK(s.faceCount() == 0);
    CHECK(geometry::tessellate(s).isEmpty());
}

TEST_CASE("Box primitive", "[geometry]")
{
    const auto box = geometry::makeBox(10, 20, 30);
    REQUIRE_FALSE(box.isNull());
    CHECK(box.faceCount() == 6);
    CHECK(box.solidCount() == 1);
    CHECK(box.volume() == Approx(6000.0));
    const auto b = box.bounds();
    CHECK(b.size().x == Approx(10.0).margin(1e-3));
    CHECK(b.size().z == Approx(30.0).margin(1e-3));
}

TEST_CASE("Tessellated box is closed and outward facing", "[geometry]")
{
    const auto mesh = geometry::tessellate(geometry::makeBox(10, 10, 10));
    REQUIRE(mesh.triangleCount() == 12);
    CHECK(mesh.surfaceArea() == Approx(600.0));

    // Outward normals: the normal of each face points away from the box centre.
    const core::Vec3 centre{5, 5, 5};
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        const auto& t = mesh.triangles()[i];
        const core::Vec3 faceCentre = (mesh.vertices()[t[0]] + mesh.vertices()[t[1]] + mesh.vertices()[t[2]]) * (1.0 / 3.0);
        CHECK(mesh.faceNormal(i).dot(faceCentre - centre) > 0.0);
    }
}

TEST_CASE("Tessellated cylinder approximates the exact area", "[geometry]")
{
    const double r = 5.0, h = 10.0;
    const auto mesh = geometry::tessellate(geometry::makeCylinder(r, h));
    const double exact = 2.0 * std::numbers::pi * r * h + 2.0 * std::numbers::pi * r * r;
    CHECK(mesh.surfaceArea() == Approx(exact).epsilon(0.01));
}

TEST_CASE("Scale and translate shapes", "[geometry]")
{
    const auto box = geometry::makeBox(10, 10, 10);
    const auto big = geometry::scaled(box, 2.0, {0, 0, 0});
    CHECK(big.volume() == Approx(8000.0));
    const auto moved = geometry::translated(box, {5, 0, 0});
    CHECK(moved.bounds().min.x == Approx(5.0).margin(1e-3));
    CHECK(box.bounds().min.x == Approx(0.0).margin(1e-3)); // original unchanged
}
