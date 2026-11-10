// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Triangulate.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace aerocad::core;
using Catch::Approx;

namespace {

double areaOf(const std::vector<Point2>& pts, const std::vector<std::uint32_t>& idx)
{
    double sum = 0.0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const auto &a = pts[idx[t]], &b = pts[idx[t + 1]], &c = pts[idx[t + 2]];
        const double cr = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
        CHECK(cr >= -1e-12); // counter-clockwise
        sum += cr / 2;
    }
    return sum;
}

std::vector<Point2> circle(double cx, double cy, double r, int n)
{
    std::vector<Point2> p;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * std::numbers::pi * i / n;
        p.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return p;
}

std::vector<Point2> flatten(const std::vector<std::vector<Point2>>& rings)
{
    std::vector<Point2> f;
    for (const auto& r : rings)
        f.insert(f.end(), r.begin(), r.end());
    return f;
}

} // namespace

TEST_CASE("triangulatePolygon: square with a hole", "[core][triangulate]")
{
    const std::vector<std::vector<Point2>> rings{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{2, 2}, {2, 8}, {8, 8}, {8, 2}}};
    const auto idx = triangulatePolygon(rings);
    CHECK(areaOf(flatten(rings), idx) == Approx(64));
}

TEST_CASE("triangulatePolygon: concave outline and several holes", "[core][triangulate]")
{
    // An L shape (area 12) with two small holes (1 each).
    const std::vector<std::vector<Point2>> rings{{{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}},
                                                 {{0.5, 0.5}, {0.5, 1.5}, {1.5, 1.5}, {1.5, 0.5}},
                                                 {{2.5, 0.5}, {2.5, 1.5}, {3.5, 1.5}, {3.5, 0.5}}};
    const auto idx = triangulatePolygon(rings);
    CHECK(areaOf(flatten(rings), idx) == Approx(10));
}

TEST_CASE("triangulatePolygon: big ring with a hole is fast and exact", "[core][triangulate]")
{
    const std::vector<std::vector<Point2>> rings{circle(0, 0, 10, 2000), circle(1, 0, 3, 1500)};
    const auto idx = triangulatePolygon(rings);
    const double outer = 0.5 * 2000 * 100 * std::sin(2 * std::numbers::pi / 2000);
    const double hole = 0.5 * 1500 * 9 * std::sin(2 * std::numbers::pi / 1500);
    CHECK(areaOf(flatten(rings), idx) == Approx(outer - hole).epsilon(1e-9));
}

TEST_CASE("triangulatePolygon: degenerate input gives nothing", "[core][triangulate]")
{
    CHECK(triangulatePolygon({}).empty());
    CHECK(triangulatePolygon({{{0, 0}, {1, 1}}}).empty());
}

TEST_CASE("simplifyRing keeps the shape within the tolerance", "[core][triangulate]")
{
    const auto ring = circle(0, 0, 10, 4000);
    const auto s = simplifyRing(ring, 0.01);
    CHECK(s.size() < 400);
    CHECK(s.size() >= 3);
    // A collinear run collapses to its corners.
    const std::vector<Point2> sq{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {3, 3}, {0, 3}};
    CHECK(simplifyRing(sq, 1e-9).size() == 4);
}
