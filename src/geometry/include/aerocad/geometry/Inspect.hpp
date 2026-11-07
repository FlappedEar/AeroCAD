// SPDX-License-Identifier: GPL-3.0-or-later
// Exact measurements on B-rep shapes (Inspect > Measure, Interference) and planar region
// triangulation for section caps.
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSection.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::geometry {

/// Part of a shape picked for a measurement. Indices are as in faces() / edges() /
/// vertices().
struct SubShape {
    enum class Kind { Whole, Face, Edge, Vertex };
    Kind kind = Kind::Whole;
    std::size_t index = 0;
};

[[nodiscard]] const char* subShapeKindName(SubShape::Kind kind);

struct VertexInfo {
    std::size_t index = 0;
    core::Vec3 point{};
};

/// Vertices of a shape in a stable order (like faces() and edges()).
[[nodiscard]] std::vector<VertexInfo> vertices(const Shape& shape);

/// Sub-shape under a picked surface point: the vertex or edge when the point is within
/// `tolerance` (mm) of one (vertices first), otherwise the face. std::nullopt for null
/// shapes.
[[nodiscard]] std::optional<SubShape> pickSubShape(const Shape& shape, const core::Vec3& point, double tolerance);

/// Exact minimum distance between two sub-shapes.
struct ShapeDistance {
    double distance = 0.0;
    core::Vec3 pointA{};
    core::Vec3 pointB{};
};

/// Throws ModelingError when the computation fails or an index is out of range.
[[nodiscard]] ShapeDistance distance(const Shape& a, const SubShape& subA, const Shape& b, const SubShape& subB);

/// Minimum distance from a point to a sub-shape.
[[nodiscard]] ShapeDistance distance(const core::Vec3& point, const Shape& b, const SubShape& subB);

/// Direction that defines an angle for a sub-shape: plane normal of a planar face, line
/// direction of a straight edge, axis of a cylindrical or conical face or of a circular
/// edge. std::nullopt otherwise.
struct Direction {
    core::Vec3 vector{};
    bool isPlaneNormal = false; ///< false: line direction (or axis).
    bool isAxis = false;        ///< Axis of a round face or circle.
};
[[nodiscard]] std::optional<Direction> subShapeDirection(const Shape& shape, const SubShape& sub);

/// Properties of one sub-shape for the Measure panel.
struct SubShapeProperties {
    std::string type;       ///< plane, cylinder, ... / line, circle, ... / vertex / solid
    double area = 0.0;      ///< Faces and whole shapes (mm^2).
    double length = 0.0;    ///< Edges (mm).
    double volume = 0.0;    ///< Whole shapes (mm^3).
    double radius = 0.0;    ///< Circles, cylinders, spheres, cones (at the reference), 0 otherwise.
    core::Vec3 point{};     ///< Vertex position, face/edge centre, shape centre of mass.
};
[[nodiscard]] SubShapeProperties subShapeProperties(const Shape& shape, const SubShape& sub);

/// Wall thickness of a solid at a point on one of its faces: the distance from the point
/// (projected onto the face) straight inwards, against the outward face normal, to the
/// first other wall. pointA is on the face, pointB on the opposite wall. std::nullopt for
/// shapes without a solid, or when the line leaves the solid without hitting a wall.
[[nodiscard]] std::optional<ShapeDistance> wallThickness(const Shape& shape, const SubShape& face,
                                                         const core::Vec3& point);

/// Triangulation of a face (or of the whole shape); empty for edges and vertices.
[[nodiscard]] core::TriangleMesh subShapeMesh(const Shape& shape, const SubShape& sub);

/// Points on a sub-shape for approximate distances to meshes: the vertex, points along an
/// edge at most `maxStep` apart, or the triangulation nodes and boundary points of a face.
[[nodiscard]] std::vector<core::Vec3> subShapePoints(const Shape& shape, const SubShape& sub, double maxStep);

/// Edges through drawn line segments (pairs of points, as Part::edgeSegments): segments
/// that join end to start are chained, and each chain is split at corners sharper than
/// 30 degrees, so a draped sketch line or arc becomes one edge. Returns a compound.
/// Throws ModelingError when there is no segment.
[[nodiscard]] Shape polylineEdges(const std::vector<core::Vec3>& segments);

/// Volume shared by two solids (0 when they do not overlap), mm^3.
[[nodiscard]] double interferenceVolume(const Shape& a, const Shape& b);

/// Triangulates the region bounded by closed planar loops (even-odd rule: nested loops are
/// holes). Triangles face along `plane.normal`. Loops that cannot be turned into faces are
/// skipped; returns an empty mesh when nothing could be built.
[[nodiscard]] core::TriangleMesh triangulateRegion(const std::vector<core::Polyline>& loops,
                                                   const core::CutPlane& plane);

/// Replaces the larger of the mesh-based distances `values` (one per point, NaN = none)
/// by the exact distance to `shape`, keeping each value's sign: those above `threshold`,
/// largest first, at most `maxCount`. A triangulation of a sewn shape can be off the
/// exact surface near its edges by up to the sewing tolerance. Returns how many were
/// replaced.
std::size_t refineShapeDistances(const Shape& shape, const std::vector<core::Vec3>& points, std::vector<double>& values,
                                 double threshold, std::size_t maxCount);

} // namespace aerocad::geometry
