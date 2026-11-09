// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <optional>

namespace aerocad::core {

/// Closest point on triangle (a, b, c) to p (Ericson, Real-Time Collision Detection 5.1.5).
[[nodiscard]] Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

struct MeshPoint {
    Vec3 point;            ///< Closest point on the surface (mm).
    double distance = 0.0; ///< Distance from the query point (mm).
    std::size_t triangle = 0;
};

/// Closest point on the mesh surface to p (exact, brute force over all triangles).
/// std::nullopt for an empty mesh.
[[nodiscard]] std::optional<MeshPoint> closestPointOnMesh(const TriangleMesh& mesh, const Vec3& p);

/// A neighbourhood for a picked normal (patchNormal, Align.hpp): twice the longest edge of `triangle`, and at least
/// 0.2 % of the mesh's bounding-box diagonal.
[[nodiscard]] double defaultNormalRadius(const TriangleMesh& mesh, std::size_t triangle);

/// Signed volume enclosed by the mesh (mm^3), positive for outward-facing triangles.
/// Only meaningful for closed meshes, see openEdgeCount().
[[nodiscard]] double enclosedVolume(const TriangleMesh& mesh);

/// Centre of mass of the solid a closed triangle mesh encloses (uniform density), from
/// the signed tetrahedra to the origin. nullopt when the enclosed volume is (near) zero.
[[nodiscard]] std::optional<Vec3> enclosedCentroid(const TriangleMesh& mesh);

/// Number of edges not shared by exactly two triangles (boundary or non-manifold edges).
/// 0 means the mesh is closed (watertight). Vertices with identical coordinates count as
/// one, so meshes with per-face vertices (tessellated B-rep) are handled.
[[nodiscard]] std::size_t openEdgeCount(const TriangleMesh& mesh);

} // namespace aerocad::core
