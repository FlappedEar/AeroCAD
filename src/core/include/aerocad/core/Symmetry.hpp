// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"

#include <optional>

namespace aerocad::core {

/// A mirror plane of a mesh, e.g. the centre plane of a car in a scan of its front bumper.
struct SymmetryPlane {
    Vec3 point;  ///< A point on the plane.
    Vec3 normal; ///< Unit normal.
    /// Share of the sampled points (away from the plane) whose mirror image lands on the mesh.
    double match = 0.0;
    /// Median distance from a mirrored point to the mesh, mm (scan noise plus asymmetry).
    double distance = 0.0;
};

/// Finds the plane the mesh is most nearly mirror symmetric about. Starts from planes
/// through the centroid across each principal axis and each world axis and refines each by
/// matching mirrored points to their nearest neighbours (like ICP for a reflection).
/// Among planes that match nearly as well (within 0.2; a bumper can also be nearly
/// symmetric top to bottom, and a scan's holes are never symmetric), the one across the
/// widest extent wins: a car's centre plane is across its width. Returns nothing for a
/// mesh with fewer than 100 vertices, or when no candidate matches at least `minMatch` of
/// the points. A part of a scan can be nearly symmetric in some other plane (a corner
/// about its diagonal), so the caller shows which plane it used.
[[nodiscard]] std::optional<SymmetryPlane> findSymmetryPlane(const TriangleMesh& mesh, double minMatch = 0.7);

/// `p` mirrored in the plane.
[[nodiscard]] Vec3 mirrorPoint(const Vec3& p, const Vec3& planePoint, const Vec3& planeNormal);
/// Direction `v` mirrored in a plane with the given normal.
[[nodiscard]] Vec3 mirrorDirection(const Vec3& v, const Vec3& planeNormal);

} // namespace aerocad::core
