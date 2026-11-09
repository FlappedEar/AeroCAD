// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace aerocad::geometry {

/// Error raised by modelling operations (invalid input, failed OCCT algorithm).
class ModelingError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(double s) const { return {x * s, y * s}; }
    constexpr bool operator==(const Vec2&) const = default;
};

/// A sketch plane: origin plus an orthonormal frame. Sketch coordinates (u, v) map to
/// origin + u * xDir + v * yDir(). The normal is the extrude direction.
struct Plane {
    core::Vec3 origin{};
    core::Vec3 normal{0, 0, 1};
    core::Vec3 xDir{1, 0, 0};

    [[nodiscard]] core::Vec3 yDir() const { return normal.cross(xDir).normalized(); }
    [[nodiscard]] core::Vec3 toWorld(const Vec2& p) const { return origin + xDir * p.x + yDir() * p.y; }
    [[nodiscard]] Vec2 toLocal(const core::Vec3& p) const
    {
        const core::Vec3 d = p - origin;
        return {d.dot(xDir), d.dot(yDir())};
    }

    /// Returns a valid orthonormal plane (normal normalized, xDir projected into the plane;
    /// a default xDir is picked when the given one is parallel to the normal).
    /// Throws ModelingError for a zero normal.
    [[nodiscard]] Plane normalized() const;

    [[nodiscard]] static Plane xy() { return {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}}; }
    [[nodiscard]] static Plane xz() { return {{0, 0, 0}, {0, -1, 0}, {1, 0, 0}}; }
    [[nodiscard]] static Plane yz() { return {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}; }
};

/// One sketch curve in plane coordinates.
struct SketchCurve {
    enum class Kind {
        Line,   ///< points = {start, end}
        Arc,    ///< points = {start, through, end} (three-point arc)
        Circle, ///< points = {center}, radius
        Spline, ///< points = interpolated fit points (>= 2), closed flag
        Point,  ///< points = {position}; a sketch point (Fusion: Point): no edge, no profile
    };
    Kind kind = Kind::Line;
    std::vector<Vec2> points;
    double radius = 0.0;
    bool closed = false;
    /// Construction geometry is shown but never forms profiles.
    bool construction = false;
    /// False: the solver does not join this curve's start to an earlier curve end that only happens
    /// to be at the same place (an explicit coincident constraint joins it). Later curves still join it.
    bool joinStart = true;
    /// The same for the end of a line or arc.
    bool joinEnd = true;
};

/// Result of evaluating a sketch.
struct SketchGeometry {
    /// All curves as 3D edges (compound), including construction curves.
    Shape curves;
    /// Closed regions bounded by the (non-construction) curves, as planar faces. Regions
    /// are minimal: a circle inside a rectangle gives a ring and a disk. Ordered by
    /// centroid (x, then y in plane coordinates).
    std::vector<Shape> profiles;
};

/// Builds the curves and closed profiles of a sketch. Curves are split at their mutual
/// intersections; open chains are allowed (they just do not bound a profile).
/// Throws ModelingError for degenerate curves.
[[nodiscard]] SketchGeometry buildSketch(const Plane& plane, const std::vector<SketchCurve>& curves);

/// The given curves as 3D edges (a compound), unsplit; used as sweep paths.
[[nodiscard]] Shape curveEdges(const Plane& plane, const std::vector<SketchCurve>& curves);

/// Offsets a connected chain of sketch curves (Fusion: Sketch Offset) by `distance` in
/// plane coordinates: positive outwards for a closed chain (to the left of an open one),
/// negative the other way. Corners are rounded with arcs. The result is lines, arcs,
/// circles and splines (sampled) in plane coordinates. Throws ModelingError when the
/// curves are not one chain or the offset collapses.
[[nodiscard]] std::vector<SketchCurve> offsetCurves(const std::vector<SketchCurve>& curves, double distance);

/// Edges `edgeIndices` of `shape` (indices as Modeling's edges()) projected onto `plane`
/// as sketch curves in plane coordinates (Fusion: Sketch > Project): lines stay lines,
/// circles and arcs parallel to the plane stay circles and arcs, anything else becomes a
/// spline through sampled points. Edges seen end-on (a point) are left out.
[[nodiscard]] std::vector<SketchCurve> projectEdges(const Shape& shape, const std::vector<std::size_t>& edgeIndices,
                                                    const Plane& plane);

/// Points along the interpolating spline through the fit points of `spline` (a Spline
/// curve; the same curve the sketch builds), in plane coordinates: `perSpan` points
/// per span between fit points, the first and last fit points included (a closed
/// spline ends where it starts). Throws ModelingError when interpolation fails.
[[nodiscard]] std::vector<Vec2> sampleSpline(const SketchCurve& spline, int perSpan = 16);

/// Area of all faces of a shape in mm^2.
[[nodiscard]] double area(const Shape& shape);

/// Area centroid of all faces of a shape.
[[nodiscard]] core::Vec3 centroid(const Shape& shape);

/// True when the point (in the face's plane, within tolerance) lies inside the face.
[[nodiscard]] bool faceContains(const Shape& face, const core::Vec3& point);

} // namespace aerocad::geometry
