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

/// Loft through planar profile faces (Fusion: Loft), in order. `solid` closes the ends;
/// `ruled` uses straight segments between sections. Profiles with one hole each (rings) give
/// a hollow loft; other hole layouts throw ModelingError.
[[nodiscard]] Shape loft(const std::vector<Shape>& sections, bool solid = true, bool ruled = false);

/// Sweeps profile faces along a path of connected edges (Fusion: Sweep).
[[nodiscard]] Shape sweep(const Shape& profileFaces, const Shape& pathEdges);

/// Sweep options (Fusion: Sweep > Twist Angle, Taper, Orientation). With any of them the profile is
/// placed at sections along the path and lofted: it spins by `twistDegrees` about the path over its
/// whole length, scales to `endScale` times its size, and with `parallel` keeps its own orientation
/// (it is only moved along the path) instead of turning with it. The path must be smooth, open and
/// not closed; throws ModelingError otherwise.
struct SweepOptions {
    double twistDegrees = 0.0;
    double endScale = 1.0;
    bool parallel = false;
};
[[nodiscard]] Shape sweep(const Shape& profileFaces, const Shape& pathEdges, const SweepOptions& options);

/// A point of a path and the unit direction of travel there.
struct PathFrame {
    core::Vec3 point;
    core::Vec3 tangent;
};

/// Rib helpers (Fusion: Create > Rib). thickenCurves() turns flat connected curves into the flat
/// faces of their outline `thickness` wide (in the plane with that normal) (open curves are rounded off at the ends);
/// curveWall() sweeps the curves `distance` along `direction` into a wall (faces, no thickness).
/// Extrude either with extrude() to get the rib solid.
[[nodiscard]] Shape thickenCurves(const Shape& pathEdges, double thickness, const core::Vec3& planeNormal);
[[nodiscard]] Shape curveWall(const Shape& pathEdges, const core::Vec3& direction, double distance);

/// The connected pieces of a set of curve edges, each as its own shape (curves that share an end
/// point belong together; crossing curves stay apart). One piece for a normal path.
[[nodiscard]] std::vector<Shape> splitPaths(const Shape& pathEdges);

/// Length of a path of connected edges (mm); throws ModelingError when they do not connect.
[[nodiscard]] double pathLength(const Shape& pathEdges);

/// The points and tangents at the given distances (mm along the path, from its start; values
/// are clamped to [0, length]). Used by Pattern on Path.
[[nodiscard]] std::vector<PathFrame> pathFrames(const Shape& pathEdges, const std::vector<double>& distances);

/// Combines shapes into one compound (null shapes are skipped).
[[nodiscard]] Shape makeCompound(const std::vector<Shape>& shapes);

/// Extrudes planar faces along `direction` (normalized internally) by `distance`. When
/// `backDistance` > 0 the faces are first moved back by it, so the solid spans
/// [-backDistance, distance] (symmetric extent: backDistance == distance). Negative
/// `distance` extrudes in the opposite direction.
/// `taperDegrees` (Fusion: Taper Angle) drafts the side faces: positive narrows the profile
/// away from the sketch plane, negative widens it; holes change the other way. With a taper
/// the two sides are separate pieces, each drafted away from the sketch plane: one from the
/// plane to `distance` and, when `backDistance` != 0, one from the plane to -backDistance.
/// Throws ModelingError when |taper| >= 89 degrees or the profile closes up before the end.
[[nodiscard]] Shape extrude(const Shape& faces, const core::Vec3& direction, double distance, double backDistance = 0.0,
                            double taperDegrees = 0.0);

/// Extrudes planar faces along `direction` until they reach `target` (Fusion: Extrude,
/// Extent "To Object"): a solid body (up to its first face reached), a surface, or the
/// faces `targetFaces` of `target` when given. Works for curved targets: each profile stops
/// on the target's shape. `reach` bounds the search (0: from the bounds of both).
/// Throws ModelingError when the extrusion does not reach the target everywhere.
[[nodiscard]] Shape extrudeToObject(const Shape& faces, const core::Vec3& direction, const Shape& target,
                                    const std::vector<std::size_t>& targetFaces = {}, double reach = 0.0);

/// A flat square face of side 2 * `halfSize` centred on `origin`, facing `normal`: a plane as a
/// tool for operations that take a shape (Extrude To Object, Replace Face).
[[nodiscard]] Shape planeFace(const core::Vec3& origin, const core::Vec3& normal, double halfSize);

/// Fusion: Modify > Replace Face. Each flat face of the solid `shape` (indices as in faces())
/// moves onto `target` (or its faces `targetFaces`): where the tool lies out of the face the body
/// grows to it, where it lies inside the body is cut back to it (a tilted plane can do both). The
/// tool must cover the whole face along its normal; the side faces end at the tool as straight
/// extensions along that normal. Throws ModelingError for curved faces or a tool that is not reached.
[[nodiscard]] Shape replaceFaces(const Shape& shape, const std::vector<std::size_t>& faceIndices, const Shape& target,
                                 const std::vector<std::size_t>& targetFaces = {});

/// Revolves faces about an axis by `angleDegrees` (360 for a full revolution).
[[nodiscard]] Shape revolve(const Shape& faces, const core::Vec3& axisOrigin, const core::Vec3& axisDirection,
                            double angleDegrees);

enum class BooleanOp { Union, Cut, Intersect };

/// Boolean operation `a op b`. The result is cleaned (coplanar faces merged). May be an
/// empty (null) shape, e.g. when a cut removes everything.
[[nodiscard]] Shape boolean(const Shape& a, const Shape& b, BooleanOp op);

/// Rounds edges (indices as in edges()) with a constant radius. Edges that continue an
/// edge tangentially (a tangent chain, as Fusion's "Tangent chain") are rounded with it.
/// When the radius does not fit, the error names the largest radius that does (when one
/// is found), e.g. "Fillet: a 5 mm radius does not fit these edges; the largest that
/// works is about 2.4 mm".
/// With `endRadius` > 0 the radius runs linearly from `radius` at the start of each edge (as
/// edges() gives it) to `endRadius` at its end (Fusion: variable radius fillet); the hint then
/// scales both.
[[nodiscard]] Shape fillet(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double radius,
                           double endRadius = 0.0);

/// Chamfer types, as in Fusion's Chamfer dialog.
enum class ChamferType {
    EqualDistance, ///< `distance` on both faces (45 degrees on a right-angled edge).
    TwoDistances,  ///< `distance` on the first face, `distance2` on the other.
    DistanceAngle, ///< `distance` on the first face and the bevel at `angle` to it.
};

struct ChamferOptions {
    ChamferType type = ChamferType::EqualDistance;
    double distance = 1.0;  ///< mm.
    double distance2 = 1.0; ///< mm, TwoDistances only.
    double angle = 45.0;    ///< Degrees, DistanceAngle only (0 < angle < 90).
    /// Swaps which face of each edge is the first one (TwoDistances, DistanceAngle).
    bool flip = false;
};

/// Bevels edges (indices as in edges()); tangent chains are bevelled with them. When the
/// distance does not fit, the error names the largest distance that does.
[[nodiscard]] Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices,
                            const ChamferOptions& options);

/// Equal distance chamfer.
[[nodiscard]] Shape chamfer(const Shape& shape, const std::vector<std::size_t>& edgeIndices, double distance);

/// Hollows a solid: removes the given faces and keeps walls of `thickness` (inwards).
[[nodiscard]] Shape shell(const Shape& shape, const std::vector<std::size_t>& faceIndices, double thickness);
/// Press Pull (Fusion, Q): moves each face `distance` along its outward normal, adding
/// material (positive) or removing it (negative). Flat faces are pulled straight; curved
/// faces are offset (Fusion's Offset Face). Throws ModelingError for a fillet face or a push
/// deeper than the face's curvature.
[[nodiscard]] Shape pressPull(const Shape& shape, const std::vector<std::size_t>& faceIndices, double distance);

/// Removes faces of a solid and heals the gap (Fusion: Modify > Delete on faces): the
/// neighbouring faces grow to close it, so a hole, a fillet or a boss disappears. Throws
/// ModelingError when the gap cannot be closed (e.g. a face whose neighbours never meet).
[[nodiscard]] Shape deleteFaces(const Shape& shape, const std::vector<std::size_t>& faceIndices);

/// A ball (Fusion: Create > Sphere) centred at `center`.
[[nodiscard]] Shape sphere(const core::Vec3& center, double radius);

/// A ring (Fusion: Create > Torus) around the +Z axis through `center`, in the XY plane:
/// `ringRadius` from the axis to the middle of the tube, `tubeRadius` of the tube itself.
/// The tube must be thinner than the ring (tubeRadius < ringRadius).
[[nodiscard]] Shape torus(const core::Vec3& center, double ringRadius, double tubeRadius);

/// A helical coil, a spring (Fusion: Create > Coil): a circular or square section of `sectionSize`
/// (diameter or side) following a helix of radius `helixRadius` about the axis through `origin`,
/// `revolutions` turns rising `pitch` per turn from the origin. The section lies in a plane through the
/// axis. Throws ModelingError when the turns would overlap (pitch <= section size).
[[nodiscard]] Shape coil(const core::Vec3& origin, const core::Vec3& axis, double helixRadius, double pitch,
                         double revolutions, double sectionSize, bool square = false, bool leftHanded = false);

/// A modeled thread on cylindrical faces (Fusion: Create > Thread): a helical V groove of the given
/// `pitch` and `depth` (default 0.6134 * pitch, the ISO external thread depth) cut into each face along
/// its axis, over `length` from its lower end (<= 0: the whole face; `flip`: from the other end). An
/// outer cylinder keeps its diameter as the major diameter and is cut inwards; a hole keeps its diameter
/// as the minor diameter and is cut outwards. Throws ModelingError for a face that is not a cylinder,
/// a face too short for the pitch, or a depth that does not fit.
[[nodiscard]] Shape thread(const Shape& shape, const std::vector<std::size_t>& faceIndices, double pitch, double depth = 0.0,
                           double length = 0.0, bool leftHanded = false, bool flip = false);

/// Draft (Fusion: Modify > Draft): tilts each face by `angleDegrees` about the line where it
/// meets the fixed (neutral) plane through `planeOrigin` with `pullDirection` as its normal.
/// A positive angle tilts the faces inwards along the pull direction (the part narrows away
/// from the plane, like a mould wall); negative tilts them outwards. Throws ModelingError for
/// a face that cannot be drafted (e.g. one parallel to the plane).
[[nodiscard]] Shape draft(const Shape& shape, const std::vector<std::size_t>& faceIndices,
                          const core::Vec3& planeOrigin, const core::Vec3& pullDirection, double angleDegrees);

/// The two sides of a body cut by a plane (Fusion: Split Body). Each side is a solid, or a
/// compound when that side has several separate pieces.
struct SplitResult {
    Shape back;  ///< behind the plane (against its normal)
    Shape front; ///< in front of the plane (along its normal)
};
/// Cuts `shape` with the infinite plane through `origin` with `normal`. Throws
/// ModelingError when the plane does not cut through the body.
[[nodiscard]] SplitResult splitByPlane(const Shape& shape, const core::Vec3& origin, const core::Vec3& normal);

/// Splits the picked faces along the plane through `origin` with `normal` (Fusion: Modify >
/// Split Face). The body stays one solid; each cut face becomes two or more faces that can be
/// pushed, drafted or deleted separately. Throws ModelingError when the plane misses the faces.
[[nodiscard]] Shape splitFaces(const Shape& shape, const std::vector<std::size_t>& faceIndices, const core::Vec3& origin,
                               const core::Vec3& normal);

/// Closed spaces enclosed by the tools (Fusion: Create > Boundary Fill): solids, shells or
/// faces cut each other and every bounded cell between them becomes a solid. Throws
/// ModelingError when nothing is enclosed.
[[nodiscard]] std::vector<Shape> boundaryFill(const std::vector<Shape>& tools);
/// True when `point` is strictly inside the solid.
[[nodiscard]] bool containsPoint(const Shape& solid, const core::Vec3& point);

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
    /// Axis of a cylinder, cone or torus face (a point on it and its direction).
    bool hasAxis = false;
    core::Vec3 axisOrigin{};
    core::Vec3 axisDirection{};
};

struct EdgeInfo {
    std::size_t index = 0;
    std::string type; ///< line, circle, ellipse, bspline, other
    double length = 0.0;
    core::Vec3 start{};
    core::Vec3 end{};
    core::Vec3 middle{};
    /// Axis of a line (through it) or circle (through its centre, along its normal) edge.
    bool seam = false; ///< the seam of a closed face (a cylinder's), not a real edge of the body
    bool hasAxis = false;
    core::Vec3 axisOrigin{};
    core::Vec3 axisDirection{};
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
/// Outward normal of face `faceIndex` at the surface point nearest to `point` (curved faces
/// too), or std::nullopt for invalid indices and undefined normals.
[[nodiscard]] std::optional<core::Vec3> faceNormalNear(const Shape& shape, std::size_t faceIndex,
                                                       const core::Vec3& point);
/// The surface point of face `faceIndex` nearest to `point` and the outward normal there (curved
/// faces too), or std::nullopt for invalid indices and undefined normals.
struct SurfacePoint {
    core::Vec3 point{};
    core::Vec3 normal{};
};
[[nodiscard]] std::optional<SurfacePoint> faceSurfaceNear(const Shape& shape, std::size_t faceIndex,
                                                          const core::Vec3& point);
/// Distance from a point to every edge / face (same order as edges() / faces();
/// infinity for degenerate edges).
[[nodiscard]] std::vector<double> edgeDistances(const Shape& shape, const core::Vec3& point);
[[nodiscard]] std::vector<double> faceDistances(const Shape& shape, const core::Vec3& point);

/// The plane of a planar face (normal pointing out of the solid), or std::nullopt for
/// non-planar faces and invalid indices.
[[nodiscard]] std::optional<Plane> facePlane(const Shape& shape, std::size_t faceIndex);

} // namespace aerocad::geometry
