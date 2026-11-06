// SPDX-License-Identifier: GPL-3.0-or-later
// Planar sections of triangle meshes (Inspect > Section Analysis).
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <vector>

namespace aerocad::core {

/// Cutting plane through `origin`. `normal` points to the half-space that is cut away.
struct CutPlane {
    Vec3 origin;
    Vec3 normal{0, 0, 1};

    /// Signed distance of p from the plane (positive on the removed side).
    [[nodiscard]] double distance(const Vec3& p) const { return (p - origin).dot(normal.normalized()); }
};

/// A section curve: points in order; `closed` when the last point connects to the first
/// (the first point is not repeated).
struct Polyline {
    std::vector<Vec3> points;
    bool closed = false;

    [[nodiscard]] double length() const;
};

/// Keeps the part of the mesh on the side opposite to the plane normal; triangles crossing
/// the plane are split exactly at the plane.
[[nodiscard]] TriangleMesh clipMesh(const TriangleMesh& mesh, const CutPlane& plane);

/// Intersection curves of the mesh with the plane, chained through shared mesh edges.
/// Closed loops come from watertight regions; open meshes (typical for scans) can give
/// open polylines. Very short segments are kept, duplicate points removed.
[[nodiscard]] std::vector<Polyline> sectionCurves(const TriangleMesh& mesh, const CutPlane& plane);

/// Orthonormal in-plane axes (u, v) with u x v = normal.
void planeAxes(const Vec3& normal, Vec3& u, Vec3& v);

/// Area enclosed by closed loops lying in a plane, counting nested loops as holes
/// (even-odd rule). Open polylines are ignored. mm^2.
[[nodiscard]] double sectionArea(const std::vector<Polyline>& curves, const CutPlane& plane);

/// For each curve: index of the innermost closed loop that contains it, or -1. Open
/// curves get -1.
[[nodiscard]] std::vector<std::ptrdiff_t> nestingParents(const std::vector<Polyline>& curves, const CutPlane& plane);

/// For each curve: how many other closed loops contain it (0 = outer boundary, 1 = hole,
/// 2 = island in a hole, ...). Open curves get 0.
[[nodiscard]] std::vector<std::size_t> nestingDepths(const std::vector<Polyline>& curves, const CutPlane& plane);

} // namespace aerocad::core
