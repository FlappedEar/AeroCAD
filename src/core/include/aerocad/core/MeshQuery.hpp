// SPDX-License-Identifier: GPL-3.0-or-later
// Spatial queries on triangle meshes: closest point and ray casting through a bounding
// volume hierarchy. Used by the Inspect tools (measure, deviation and thickness maps).
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace aerocad::core {

/// Closest point on a mesh surface.
struct SurfacePoint {
    Vec3 point;            ///< Point on the surface (mm).
    double distance = 0.0; ///< Distance from the query point (mm).
    std::size_t triangle = 0;
};

struct MeshRayHit {
    double t = 0.0; ///< Distance along the (normalized) ray direction (mm).
    Vec3 point;
    std::size_t triangle = 0;
};

/// Bounding volume hierarchy over the triangles of a mesh. Immutable after construction
/// and safe to query from several threads at once. Keeps a shared reference to the mesh.
class MeshBvh {
public:
    explicit MeshBvh(std::shared_ptr<const TriangleMesh> mesh);

    [[nodiscard]] const TriangleMesh& mesh() const { return *m_mesh; }

    /// Closest point on the surface to `p`, searching only within `maxDistance`.
    /// std::nullopt for an empty mesh or when nothing is that close.
    [[nodiscard]] std::optional<SurfacePoint>
    closestPoint(const Vec3& p, double maxDistance = std::numeric_limits<double>::infinity()) const;

    /// First intersection of the ray origin + t * direction, 0 < t <= maxT, with the
    /// surface (both triangle sides). `direction` need not be normalized; t is in mm.
    [[nodiscard]] std::optional<MeshRayHit>
    raycast(const Vec3& origin, const Vec3& direction, double maxT = std::numeric_limits<double>::infinity()) const;

private:
    struct Node {
        BoundingBox box;
        std::uint32_t first = 0; ///< Leaf: first index into m_order; inner: left child.
        std::uint32_t count = 0; ///< Leaf: triangle count; 0 for inner nodes.
        std::uint32_t right = 0; ///< Inner: right child.
    };

    std::uint32_t build(std::uint32_t first, std::uint32_t count, std::vector<Vec3>& centroids);

    std::shared_ptr<const TriangleMesh> m_mesh;
    std::vector<Node> m_nodes;
    std::vector<std::uint32_t> m_order; ///< Triangle indices, grouped by leaf.
};

/// Minimum distance between two surfaces and the closest points on each.
struct SurfaceDistance {
    double distance = 0.0;
    Vec3 pointA; ///< On mesh A.
    Vec3 pointB; ///< On mesh B.
};

/// Approximate minimum distance between two meshes: every vertex of each mesh is
/// projected onto the other surface (exact for vertex-face contacts, an upper bound for
/// edge-edge contacts). std::nullopt when a mesh is empty.
[[nodiscard]] std::optional<SurfaceDistance> minimumDistance(const MeshBvh& a, const MeshBvh& b);

/// Signed distance of every vertex of `measured` to the surface of `reference`: positive
/// on the side the reference triangle normals point to (outside a closed solid), negative
/// inside. Vertices farther than `maxDistance` get NaN.
[[nodiscard]] std::vector<double> signedVertexDistances(const TriangleMesh& measured, const MeshBvh& reference,
                                                        double maxDistance = std::numeric_limits<double>::infinity());

/// Wall thickness at every vertex: for each triangle the distance from its centre along
/// the inward normal to the opposite side of the same mesh; a vertex gets the smallest
/// value of its triangles. NaN where the rays leave the mesh (open surfaces) or exceed
/// `maxThickness`.
[[nodiscard]] std::vector<double> vertexThickness(const MeshBvh& mesh,
                                                  double maxThickness = std::numeric_limits<double>::infinity());

/// Wall thickness per triangle: the distance from its centre along the inward normal to
/// the opposite side of the same mesh. NaN where the ray leaves the mesh or exceeds
/// `maxThickness`. vertexThickness() takes the smallest value around each vertex.
[[nodiscard]] std::vector<double> triangleThickness(const MeshBvh& mesh,
                                                    double maxThickness = std::numeric_limits<double>::infinity());

/// Area-weighted vertex normals (unit length; zero for unused vertices). Vertices with
/// identical coordinates are not merged.
[[nodiscard]] std::vector<Vec3> vertexNormals(const TriangleMesh& mesh);

/// Summary statistics of a value set; NaN values are skipped.
struct ValueStats {
    std::size_t count = 0; ///< Finite values.
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double rms = 0.0;
    double stddev = 0.0;
    std::size_t minIndex = 0;
    std::size_t maxIndex = 0;
};

[[nodiscard]] ValueStats valueStats(const std::vector<double>& values);

} // namespace aerocad::core
