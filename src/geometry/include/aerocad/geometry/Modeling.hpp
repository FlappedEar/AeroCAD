// SPDX-License-Identifier: GPL-3.0-or-later
// Solid modelling operations on B-rep shapes (Open CASCADE behind the Shape pimpl).
// All functions return new shapes and throw ModelingError on failure.
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"
#include "aerocad/geometry/Sketch.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::geometry {

/// Converts a triangle mesh into a B-rep: one planar face per triangle, sewn together and
/// made a solid when the mesh is closed; coplanar neighbours are merged into larger
/// planar faces. Meant for low-poly models (printed parts, CAD exports), not for dense
/// scans: throws ModelingError above `maxTriangles`.
[[nodiscard]] Shape meshToBrep(const core::TriangleMesh& mesh, std::size_t maxTriangles = 50000);

/// Copies of a shape mirrored about a plane, or turned about an axis (degrees). Moving
/// is translated() in Transform.hpp.
[[nodiscard]] Shape mirrored(const Shape& shape, const core::Vec3& planeOrigin, const core::Vec3& planeNormal);
[[nodiscard]] Shape rotated(const Shape& shape, const core::Vec3& axisOrigin, const core::Vec3& axisDirection,
                            double angleDegrees);

/// Loft through the outer boundaries of planar profile faces (Fusion: Loft), in order.
/// `solid` closes the ends; `ruled` uses straight segments between sections.
[[nodiscard]] Shape loft(const std::vector<Shape>& sections, bool solid = true, bool ruled = false);

/// Sweeps profile faces along a path of connected edges (Fusion: Sweep).
[[nodiscard]] Shape sweep(const Shape& profileFaces, const Shape& pathEdges);

/// Combines shapes into one compound (null shapes are skipped).
[[nodiscard]] Shape makeCompound(const std::vector<Shape>& shapes);

/// Extrudes planar faces along `direction` (normalized internally) by `distance`. When
/// `backDistance` > 0 the faces are first moved back by it, so the solid spans
/// [-backDistance, distance] (symmetric extent: backDistance == distance). Negative
/// `distance` extrudes in the opposite direction.
[[nodiscard]] Shape extrude(const Shape& faces, const core::Vec3& direction, double distance, double backDistance = 0.0);

/// Revolves faces about an axis by `angleDegrees` (360 for a full revolution).
[[nodiscard]] Shape revolve(const Shape& faces, const core::Vec3& axisOrigin, const core::Vec3& axisDirection,
                            double angleDegrees);

enum class BooleanOp { Union, Cut, Intersect };

/// Boolean operation `a op b`. The result is cleaned (coplanar faces merged). May be an
/// empty (null) shape, e.g. when a cut removes everything.
[[nodiscard]] Shape boolean(const Shape& a, const Shape& b, BooleanOp op);

/// Rounds edges (indices as in edges()) with a constant radius.
[[nodiscard]] Shape fillet(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double radius);

/// Bevels edges (indices as in edges()) with a constant distance.
[[nodiscard]] Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double distance);

/// Hollows a solid: removes the given faces and keeps walls of `thickness` (inwards).
[[nodiscard]] Shape shell(const Shape& shape, const std::vector<std::size_t>& faceIndices, double thickness);

/// True when the shape passes the B-rep validity check.
[[nodiscard]] bool isValid(const Shape& shape);

/// True when both shapes' bounding boxes overlap (with a small tolerance).
[[nodiscard]] bool boundsOverlap(const Shape& a, const Shape& b);

/// True when the shapes touch or overlap: their minimum distance is within `tolerance`,
/// or one solid lies inside the other. Bounding boxes are checked first.
[[nodiscard]] bool touches(const Shape& a, const Shape& b, double tolerance = 1e-6);

struct FaceInfo {
    std::size_t index = 0;
    std::string type; ///< plane, cylinder, cone, sphere, torus, bspline, other
    double area = 0.0;
    core::Vec3 center{};
    /// Outward normal at the centre (for planar faces: the plane normal).
    core::Vec3 normal{};
};

struct EdgeInfo {
    std::size_t index = 0;
    std::string type; ///< line, circle, ellipse, bspline, other
    double length = 0.0;
    core::Vec3 start{};
    core::Vec3 end{};
    core::Vec3 middle{};
};

/// Faces / edges of a shape, indexed in a stable order for a given shape. Indices are
/// what fillet(), chamfer(), shell() and facePlane() take. They change when the
/// topology changes (no persistent naming yet).
[[nodiscard]] std::vector<FaceInfo> faces(const Shape& shape);
[[nodiscard]] std::vector<EdgeInfo> edges(const Shape& shape);

/// Index of the edge / face closest to a point (std::nullopt for shapes without any).
/// Used to keep references stable when earlier features change the topology.
[[nodiscard]] std::optional<std::size_t> nearestEdge(const Shape& shape, const core::Vec3& point);
[[nodiscard]] std::optional<std::size_t> nearestFace(const Shape& shape, const core::Vec3& point);
/// Distance from a point to every edge / face (same order as edges() / faces();
/// infinity for degenerate edges).
[[nodiscard]] std::vector<double> edgeDistances(const Shape& shape, const core::Vec3& point);
[[nodiscard]] std::vector<double> faceDistances(const Shape& shape, const core::Vec3& point);

/// The plane of a planar face (normal pointing out of the solid), or std::nullopt for
/// non-planar faces and invalid indices.
[[nodiscard]] std::optional<Plane> facePlane(const Shape& shape, std::size_t faceIndex);

} // namespace aerocad::geometry
