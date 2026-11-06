// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cad/SketchSolver.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace aerocad;
using namespace aerocad::cad;
using geometry::SketchCurve;
using geometry::Vec2;
using Catch::Approx;
using T = SketchConstraint::Type;
using PK = SketchPointRef::Kind;

namespace {

SketchCurve line(Vec2 a, Vec2 b)
{
    return {SketchCurve::Kind::Line, {a, b}};
}

std::vector<SketchCurve> rectangle(Vec2 a, Vec2 b)
{
    return {line(a, {b.x, a.y}), line({b.x, a.y}, b), line(b, {a.x, b.y}), line({a.x, b.y}, a)};
}

/// Horizontal/vertical sides plus the origin at the first corner: what the UI adds for a rectangle.
std::vector<SketchConstraint> rectangleConstraints()
{
    return {{T::Horizontal, {0}, {}},
            {T::Vertical, {1}, {}},
            {T::Horizontal, {2}, {}},
            {T::Vertical, {3}, {}},
            {T::Coincident, {}, {{PK::Start, 0}, {PK::Origin, 0}}}};
}

double length(const SketchCurve& c)
{
    return std::hypot(c.points[1].x - c.points[0].x, c.points[1].y - c.points[0].y);
}

} // namespace

TEST_CASE("Solver: dimensions drive a rectangle and keep it connected", "[cad][solver]")
{
    auto curves = rectangle({1, 1}, {41, 21});
    auto cs = rectangleConstraints();
    CHECK(degreesOfFreedom(curves, cs) == 2); // width and height left
    cs.push_back({T::Length, {0}, {}, 60});
    cs.push_back({T::Length, {1}, {}, 30});
    const auto r = solveSketch(curves, cs);
    REQUIRE(r.solved);
    CHECK(r.degreesOfFreedom == 0);
    CHECK(curves[0].points[0].x == Approx(0).margin(1e-6));
    CHECK(curves[0].points[0].y == Approx(0).margin(1e-6));
    CHECK(length(curves[0]) == Approx(60));
    CHECK(length(curves[1]) == Approx(30));
    CHECK(length(curves[2]) == Approx(60));
    // Corners stay shared.
    for (std::size_t i = 0; i < 4; ++i) {
        const Vec2 end = curves[i].points[1];
        const Vec2 next = curves[(i + 1) % 4].points[0];
        CHECK(std::hypot(end.x - next.x, end.y - next.y) < 1e-6);
    }
}

TEST_CASE("Solver: conflicting constraints are reported and leave curves unchanged", "[cad][solver]")
{
    auto curves = rectangle({0, 0}, {10, 10});
    auto cs = rectangleConstraints();
    cs.push_back({T::Length, {0}, {}, 20});
    cs.push_back({T::Length, {2}, {}, 30}); // opposite sides of a rectangle cannot differ
    const auto before = curves;
    const auto r = solveSketch(curves, cs);
    CHECK_FALSE(r.solved);
    CHECK_FALSE(r.unsatisfied.empty());
    CHECK(curves[0].points[1].x == before[0].points[1].x);
}

TEST_CASE("Solver: circle radius, concentric, tangent and distance", "[cad][solver]")
{
    SketchCurve c1{SketchCurve::Kind::Circle, {{0, 0}}};
    c1.radius = 5;
    SketchCurve c2{SketchCurve::Kind::Circle, {{3, 1}}};
    c2.radius = 2;
    SketchCurve c3{SketchCurve::Kind::Circle, {{20, 0}}};
    c3.radius = 4;
    std::vector<SketchCurve> curves{c1, c2, c3, line({-10, -8}, {10, -8})};
    std::vector<SketchConstraint> cs{
        {T::Diameter, {0}, {}, 20},
        {T::Concentric, {0, 1}, {}},
        {T::Radius, {1}, {}, 3},
        {T::Tangent, {0, 2}, {}},
        {T::Tangent, {3, 0}, {}},
        {T::Horizontal, {3}, {}},
    };
    const auto r = solveSketch(curves, cs);
    REQUIRE(r.solved);
    CHECK(curves[0].radius == Approx(10));
    CHECK(curves[1].radius == Approx(3));
    CHECK(curves[1].points[0].x == Approx(curves[0].points[0].x).margin(1e-6));
    const double d = std::hypot(curves[2].points[0].x - curves[0].points[0].x,
                                curves[2].points[0].y - curves[0].points[0].y);
    CHECK(d == Approx(curves[0].radius + curves[2].radius));
    CHECK(std::abs(curves[3].points[0].y - curves[0].points[0].y) == Approx(10));
}

TEST_CASE("Solver: parallel, perpendicular, angle, equal", "[cad][solver]")
{
    std::vector<SketchCurve> curves{line({0, 0}, {10, 1}), line({0, 5}, {9, 7}), line({20, 0}, {22, 9}),
                                    line({30, 0}, {38, 3})};
    std::vector<SketchConstraint> cs{
        {T::Horizontal, {0}, {}},          {T::Parallel, {0, 1}, {}}, {T::Perpendicular, {0, 2}, {}},
        {T::Angle, {0, 3}, {}, 30},        {T::Equal, {0, 3}, {}},
    };
    const auto r = solveSketch(curves, cs);
    REQUIRE(r.solved);
    auto dir = [&](std::size_t i) {
        return Vec2{curves[i].points[1].x - curves[i].points[0].x, curves[i].points[1].y - curves[i].points[0].y};
    };
    CHECK(dir(0).y == Approx(0).margin(1e-6));
    CHECK(dir(1).y == Approx(0).margin(1e-6));
    CHECK(dir(2).x == Approx(0).margin(1e-6));
    const double angle = std::atan2(dir(3).y, dir(3).x) * 180 / 3.14159265358979;
    CHECK(angle == Approx(30).margin(1e-5));
    CHECK(length(curves[3]) == Approx(length(curves[0])));
}

TEST_CASE("Solver: arc keeps its direction and stays on its circle", "[cad][solver]")
{
    // Half circle above the x axis, closed by a line: a slot end.
    std::vector<SketchCurve> curves{line({-10, 0}, {10, 0}), {SketchCurve::Kind::Arc, {{10, 0}, {0, 10}, {-10, 0}}}};
    std::vector<SketchConstraint> cs{{T::Radius, {1}, {}, 15}, {T::Horizontal, {0}, {}},
                                     {T::Midpoint, {}, {{PK::Origin, 0}}, 0}};
    cs[2].curves = {0};
    const auto r = solveSketch(curves, cs);
    REQUIRE(r.solved);
    const auto& arc = curves[1];
    CHECK(arc.points[0].x == Approx(15).margin(1e-5));
    CHECK(arc.points[2].x == Approx(-15).margin(1e-5));
    CHECK(arc.points[1].y == Approx(15).margin(1e-5)); // still bulging upwards
    CHECK(curves[0].points[0].x == Approx(-15).margin(1e-5));
}

TEST_CASE("Solver: symmetric points, fix, horizontal distance and point on curve", "[cad][solver]")
{
    std::vector<SketchCurve> curves{line({0, -10}, {0, 10}), line({-3, 2}, {5, 4}), line({50, 50}, {60, 50})};
    std::vector<SketchConstraint> cs{
        {T::Fix, {0}, {}},
        {T::Symmetric, {0}, {{PK::Start, 1}, {PK::End, 1}}},
        {T::HorizontalDistance, {}, {{PK::Start, 1}, {PK::End, 1}}, 12},
        {T::PointOnCurve, {0}, {{PK::Start, 2}}},
        {T::Distance, {}, {{PK::Start, 2}, {PK::Origin, 0}}, 5},
    };
    const auto r = solveSketch(curves, cs);
    REQUIRE(r.solved);
    CHECK(curves[0].points[0].y == Approx(-10)); // fixed
    CHECK(curves[1].points[0].x == Approx(-6).margin(1e-6));
    CHECK(curves[1].points[1].x == Approx(6).margin(1e-6));
    CHECK(curves[1].points[0].y == Approx(curves[1].points[1].y).margin(1e-6));
    CHECK(curves[2].points[0].x == Approx(0).margin(1e-6));
    CHECK(std::abs(curves[2].points[0].y) == Approx(5));
}

TEST_CASE("Solver: malformed constraints throw", "[cad][solver]")
{
    auto curves = rectangle({0, 0}, {10, 10});
    std::vector<SketchConstraint> bad{{T::Radius, {0}, {}, 5}};
    CHECK_THROWS_AS(solveSketch(curves, bad), geometry::ModelingError);
    std::vector<SketchConstraint> out{{T::Horizontal, {9}, {}}};
    CHECK_THROWS_AS(solveSketch(curves, out), geometry::ModelingError);
    CHECK_THROWS_AS(constraintType("glue"), geometry::ModelingError);
    CHECK(constraintType("horizontal_distance") == T::HorizontalDistance);
}

TEST_CASE("Solver: turning a line by 90 degrees does not collapse it", "[cad][solver]")
{
    std::vector<SketchCurve> curves{line({0, 0}, {10, 0})};
    auto r = solveSketch(curves, {{T::Vertical, {0}, {}}});
    REQUIRE(r.solved);
    CHECK(length(curves[0]) == Approx(10).epsilon(1e-6));
    CHECK(curves[0].points[0].x == Approx(curves[0].points[1].x).margin(1e-6));

    curves = {line({0, 0}, {0, 10})};
    r = solveSketch(curves, {{T::Horizontal, {0}, {}}, {T::Length, {0}, {}, 10}});
    REQUIRE(r.solved);
    CHECK(curves[0].points[0].y == Approx(curves[0].points[1].y).margin(1e-6));

    // Perpendicular on parallel lines, and parallel on perpendicular lines.
    curves = {line({0, 0}, {10, 0}), line({0, 5}, {10, 5})};
    r = solveSketch(curves, {{T::Perpendicular, {0, 1}, {}}});
    REQUIRE(r.solved);
    CHECK(length(curves[1]) == Approx(10).epsilon(1e-6));
    curves = {line({0, 0}, {10, 0}), line({20, 0}, {20, 10})};
    r = solveSketch(curves, {{T::Parallel, {0, 1}, {}}});
    REQUIRE(r.solved);
    CHECK(length(curves[1]) == Approx(10).epsilon(1e-6));
}

TEST_CASE("Solver: fix holds explicit positions", "[cad][solver]")
{
    std::vector<SketchCurve> curves{line({0, 0}, {10, 0})};
    SketchConstraint fix{T::Fix, {}, {{PK::Start, 0}, {PK::End, 0}}};
    fix.at = {{0, 0}, {20, 0}};
    const auto r = solveSketch(curves, {{T::Length, {0}, {}, 20}, fix});
    REQUIRE(r.solved);
    CHECK(curves[0].points[1].x == Approx(20));
}
