// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/SkinCheck.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

using namespace aerocad::core;
using Catch::Approx;

namespace {

enum class Flip { None, All, OneCell };

/// A flat 100 x 100 mm sheet at z = 0 facing +z, in 10 x 10 cells.
std::shared_ptr<const TriangleMesh> sheet(Flip flip = Flip::None)
{
    std::vector<Vec3> v;
    std::vector<TriangleMesh::Triangle> t;
    const int n = 10;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            v.push_back({10.0 * i, 10.0 * j, 0.0});
    auto idx = [&](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const bool flipped = flip == Flip::All || (flip == Flip::OneCell && i == 4 && j == 4);
            TriangleMesh::Triangle a{idx(i, j), idx(i + 1, j), idx(i + 1, j + 1)};
            TriangleMesh::Triangle b{idx(i, j), idx(i + 1, j + 1), idx(i, j + 1)};
            if (flipped) {
                std::swap(a[1], a[2]);
                std::swap(b[1], b[2]);
            }
            t.push_back(a);
            t.push_back(b);
        }
    return std::make_shared<const TriangleMesh>(std::move(v), std::move(t));
}

/// A closed box [x0,x1] x [y0,y1] x [z0,z1].
TriangleMesh box(double x0, double x1, double y0, double y1, double z0, double z1)
{
    std::vector<Vec3> v;
    for (int i = 0; i < 8; ++i)
        v.push_back({i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0});
    std::vector<TriangleMesh::Triangle> t{{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6}, {0, 1, 5}, {0, 5, 4},
                                          {2, 6, 7}, {2, 7, 3}, {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
    return TriangleMesh(std::move(v), std::move(t));
}

} // namespace

TEST_CASE("A part standing on the skin is not behind it (CAD-304)", "[core][skin]")
{
    const BehindSkin on = measureBehindSkin(box(40, 60, 40, 60, 0, 8), sheet(), 0.5);
    CHECK(on.examined == 8);
    CHECK(on.behind == 0);
    const BehindSkin above = measureBehindSkin(box(40, 60, 40, 60, 3, 8), sheet(), 0.5);
    CHECK(above.behind == 0);
}

TEST_CASE("A part reaching through the skin is behind it by its depth", "[core][skin]")
{
    const BehindSkin through = measureBehindSkin(box(40, 60, 40, 60, -2, 8), sheet(), 0.5);
    CHECK(through.examined == 8);
    CHECK(through.behind == 4);
    CHECK(through.deepest == Approx(2.0));
    CHECK(through.at.z == Approx(-2.0));
    CHECK(through.share() == Approx(0.5));
    // Within the tolerance it is the skin's own roughness.
    CHECK(measureBehindSkin(box(40, 60, 40, 60, -0.3, 8), sheet(), 0.5).behind == 0);
}

TEST_CASE("Points whose nearest skin point is on the scan's border are not judged", "[core][skin]")
{
    // Beyond the sheet's edge the side cannot be told.
    const BehindSkin beyond = measureBehindSkin(box(110, 120, 40, 60, -2, 8), sheet(), 0.5);
    CHECK(beyond.examined == 0);
    CHECK(beyond.behind == 0);
    CHECK(measureBehindSkin(box(40, 60, 40, 60, -2, 8), nullptr, 0.5).examined == 0);
}

TEST_CASE("Points over triangles of inconsistent winding are not judged", "[core][skin]")
{
    // A consistently flipped sheet is still consistent: "behind" is simply the other side.
    CHECK(measureBehindSkin(box(40, 60, 40, 60, -2, 8), sheet(Flip::All), 0.5).behind == 4);
    // One flipped cell makes the triangles around it uncertain.
    const BehindSkin part = measureBehindSkin(box(40, 60, 40, 60, -2, 8), sheet(Flip::OneCell), 0.5);
    CHECK(part.examined < 8);
    CHECK(part.examined > 0);
}

TEST_CASE("The skin check stops when cancelled", "[core][skin]")
{
    CHECK_THROWS_AS(measureBehindSkin(box(40, 60, 40, 60, -2, 8), sheet(), 0.5, [] { return true; }), Cancelled);
}
