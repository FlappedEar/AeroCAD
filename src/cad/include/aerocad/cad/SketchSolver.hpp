// SPDX-License-Identifier: GPL-3.0-or-later
// Geometric constraint solver for sketches (Fusion: Sketch > Constraints and Sketch
// Dimension). Curves are moved as little as possible so that all constraints hold.
//
// Model: every line end, arc end, arc centre, circle centre and spline point is a 2D
// point variable; circles also have a radius variable. Curve ends that touch when the
// solve starts are merged into one point, so connected outlines (rectangles, polygons,
// chained lines) stay connected. The system is solved with Levenberg-Marquardt starting
// from the drawn geometry, which keeps the solution close to what the user drew.
#pragma once

#include "aerocad/geometry/Sketch.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aerocad::cad {

/// A point of a sketch curve, or the sketch origin.
struct SketchPointRef {
    enum class Kind { Start, End, Center, Origin, Fit, Axis };
    Kind kind = Kind::Start;
    std::size_t curve = 0; ///< index into the sketch's curve list (unused for Origin)
    std::size_t index = 0; ///< Fit: which fit point of a spline (0 = first)
    // Axis: the end of an ellipse's or elliptical arc's major axis.
};

struct SketchConstraint {
    enum class Type {
        // Geometric constraints
        Coincident,    ///< points[0] == points[1]
        PointOnCurve,  ///< points[0] lies on curves[0] (line, arc, circle)
        Midpoint,      ///< points[0] is the midpoint of line curves[0]
        Horizontal,    ///< line curves[0] (or an ellipse's major axis), or points[0] and points[1] share y
        Vertical,      ///< line curves[0] (or an ellipse's major axis), or points[0] and points[1] share x
        Parallel,      ///< lines curves[0], curves[1]
        Perpendicular, ///< lines curves[0], curves[1]
        Collinear,     ///< lines curves[0], curves[1]
        Tangent,       ///< line/arc/circle curves[0] and arc/circle curves[1] (or reversed)
        Equal,         ///< two lines (length) or two arcs/circles (radius)
        Concentric,    ///< arcs/circles curves[0], curves[1]
        Symmetric,     ///< points[0], points[1] mirrored about line curves[0]
        Fix,           ///< points (or all points of curves) keep their position
        // Dimensions (value in mm or degrees)
        Distance,           ///< points[0]-points[1], or points[0] to line curves[0]
        HorizontalDistance, ///< |x1 - x0| of points[0], points[1]
        VerticalDistance,   ///< |y1 - y0| of points[0], points[1]
        Length,             ///< line curves[0]
        Radius,             ///< arc/circle curves[0]
        Diameter,           ///< arc/circle curves[0]
        Angle,              ///< between lines curves[0] and curves[1], degrees
        MajorRadius,        ///< ellipse curves[0]: centre to the end of its major axis
        MinorRadius,        ///< ellipse curves[0]
    };
    Type type = Type::Coincident;
    std::vector<std::size_t> curves;
    std::vector<SketchPointRef> points;
    double value = 0.0;
    /// Fix: positions to hold `points` at (same order). Empty = where they were drawn.
    std::vector<geometry::Vec2> at = {};
    /// Tangent: which branch, 0 = as drawn. Line and arc/circle: +1 = centre left of the line's
    /// start->end, -1 = right. Two arcs/circles: +1 = touching outside, -1 = inside with curves[0]
    /// the larger, -2 = inside with curves[1] the larger.
    int side = 0;
};

struct SketchSolveResult {
    bool solved = true;
    /// Remaining degrees of freedom (0 = fully constrained, Fusion draws such sketches black).
    int degreesOfFreedom = 0;
    /// Indices of constraints that are not satisfied when the solve fails.
    std::vector<std::size_t> unsatisfied;
    int iterations = 0;
};

/// Parses a constraint type name ("horizontal", "distance", ...). Throws
/// geometry::ModelingError for unknown names.
[[nodiscard]] SketchConstraint::Type constraintType(std::string_view name);
[[nodiscard]] std::string_view toString(SketchConstraint::Type type);
[[nodiscard]] bool isDimension(SketchConstraint::Type type);

/// Moves the curves so that all constraints hold. Throws geometry::ModelingError for
/// malformed constraints (bad curve index, wrong curve kind). When the constraints
/// cannot be satisfied, `solved` is false and the curves are left unchanged.
SketchSolveResult solveSketch(std::vector<geometry::SketchCurve>& curves,
                              const std::vector<SketchConstraint>& constraints);

/// Degrees of freedom of the curves under the constraints, without moving anything.
[[nodiscard]] int degreesOfFreedom(const std::vector<geometry::SketchCurve>& curves,
                                   const std::vector<SketchConstraint>& constraints);

} // namespace aerocad::cad
