// SPDX-License-Identifier: GPL-3.0-or-later
// Spatial queries on triangle meshes: closest point and ray casting through a bounding
// volume hierarchy. Used by the Inspect tools (measure, deviation and thickness maps).
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSelection.hpp"

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
    /// With `includeOrigin`, a hit at t = 0 counts too (a ray starting on the surface, such
    /// as a pick from a section plane lying on a face).
    [[nodiscard]] std::optional<MeshRayHit>
    raycast(const Vec3& origin, const Vec3& direction, double maxT = std::numeric_limits<double>::infinity(),
            bool includeOrigin = false) const;

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

/// Ray picking for the viewport on meshes of any size: a MeshBvh per mesh, built on a
/// background thread the first time a mesh is picked (a few hundred ms for millions of
/// triangles), so the pointer never waits for it. Until it is ready a pick casts the ray
/// through every triangle, as before. Not thread-safe: use it from one thread (the UI).
class MeshPickCache {
public:
    MeshPickCache() = default;
    MeshPickCache(const MeshPickCache&) = delete;
    MeshPickCache& operator=(const MeshPickCache&) = delete;

    /// Meshes up to this many triangles get their hierarchy at once, on the calling thread.
    static constexpr std::size_t kSyncTriangles = 50000;

    /// Nearest hit of the ray origin + t * direction (t >= 0), both triangle sides, as
    /// core::raycast: `distance` is in units of |direction|.
    [[nodiscard]] std::optional<RayHit> raycast(const std::shared_ptr<const TriangleMesh>& mesh, const Vec3& origin,
                                                const Vec3& direction);

    /// The hierarchy for `mesh` if built (starting its build if needed), else nullptr.
    [[nodiscard]] std::shared_ptr<const MeshBvh> bvh(const std::shared_ptr<const TriangleMesh>& mesh);
    /// Starts building the hierarchy for `mesh` in the background (no-op if known).
    void prepare(const std::shared_ptr<const TriangleMesh>& mesh);
    /// Forgets the meshes not in `alive` (removed or replaced parts), so they are freed.
    void retain(const std::vector<const TriangleMesh*>& alive);
    /// Blocks until every started build is finished (tests).
    void waitForBuilds();
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    struct Build;
    struct Entry {
        const TriangleMesh* key = nullptr;
        std::shared_ptr<Build> build;
    };
    Entry& entryFor(const std::shared_ptr<const TriangleMesh>& mesh);
    std::vector<Entry> m_entries;
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
