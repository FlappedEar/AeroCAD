// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace aerocad::core {

/// Triangle set on one mesh, stored as sorted, unique triangle indices.
using TriangleSet = std::vector<std::uint32_t>;

/// Edge neighbours of every triangle. Vertices are matched by position, not by index, so
/// meshes whose faces do not share vertices (tessellated B-rep, unwelded files) are still
/// connected across their seams.
class MeshAdjacency {
public:
    static constexpr std::uint32_t none = 0xFFFFFFFFu;

    explicit MeshAdjacency(const TriangleMesh& mesh);

    /// Neighbours across edges (v0,v1), (v1,v2), (v2,v0); `none` for border edges.
    /// Non-manifold edges keep one neighbour.
    [[nodiscard]] const std::array<std::uint32_t, 3>& neighbours(std::uint32_t triangle) const
    {
        return m_neighbours[triangle];
    }
    /// True when the neighbour across edge `slot` runs along that edge in the same
    /// direction as `triangle`, i.e. the two are wound inconsistently (one is flipped).
    [[nodiscard]] bool windingFlipped(std::uint32_t triangle, int slot) const
    {
        return (m_flipped[triangle] >> slot & 1u) != 0;
    }
    [[nodiscard]] std::size_t triangleCount() const { return m_neighbours.size(); }

private:
    std::vector<std::array<std::uint32_t, 3>> m_neighbours;
    std::vector<std::uint8_t> m_flipped; ///< Bit per edge slot, see windingFlipped().
};

/// Adjacency of `mesh`, computed once and cached for the few most recently used meshes.
/// Meshes are immutable once shared, so the cache is keyed by the mesh object.
/// Not thread-safe (UI thread only, like the document).
[[nodiscard]] std::shared_ptr<const MeshAdjacency> adjacencyFor(const std::shared_ptr<const TriangleMesh>& mesh);

/// Index of the triangle closest to `point`, or std::nullopt for an empty mesh.
/// Brute force; fine for picking (one query per click or brush dab).
[[nodiscard]] std::optional<std::uint32_t> nearestTriangle(const TriangleMesh& mesh, const Vec3& point);

struct RayHit {
    std::uint32_t triangle = 0;
    double distance = 0.0; ///< Along the ray, in units of |direction|.
    Vec3 point;
};

/// Nearest intersection of the ray origin + t * direction (t >= 0) with the mesh, both
/// triangle sides. Brute force (Moller-Trumbore); fine for one query per pointer event.
[[nodiscard]] std::optional<RayHit> raycast(const TriangleMesh& mesh, const Vec3& origin, const Vec3& direction);

/// For a ray that misses the mesh (a click into a hole of a scan, such as a grille
/// opening, or just past its edge): the mesh vertex seen closest to the ray from `origin`,
/// within `maxAngleDeg` of it, as a hit on a triangle using that vertex. `distance` is along
/// the ray, in units of |direction|. Empty when no vertex in front of `origin` is that close.
[[nodiscard]] std::optional<RayHit> nearestToRay(const TriangleMesh& mesh, const Vec3& origin, const Vec3& direction,
                                                 double maxAngleDeg);

/// A mesh seen from outside along one direction: triangles binned in a grid across it, for
/// fast queries on scans of millions of triangles (build: under a second; queries:
/// microseconds). Keeps a reference to `mesh`, which must outlive it.
class MeshProjector {
public:
    /// `outward` points from the surface towards the viewer of `drop`.
    MeshProjector(const TriangleMesh& mesh, const Vec3& outward);

    [[nodiscard]] const TriangleMesh& mesh() const { return *m_mesh; }
    [[nodiscard]] const Vec3& outward() const { return m_d; }

    /// The first point of the mesh met coming from outside, along the line through `p`
    /// parallel to `outward`: the visible skin, never a layer behind it. Empty when the
    /// line misses the mesh (a hole, or past its edge).
    [[nodiscard]] std::optional<Vec3> drop(const Vec3& p) const;

    /// Same as core::raycast: nearest hit of the ray, both triangle sides.
    [[nodiscard]] std::optional<RayHit> raycast(const Vec3& origin, const Vec3& direction) const;

private:
    [[nodiscard]] int cellU(double u) const;
    [[nodiscard]] int cellV(double v) const;

    const TriangleMesh* m_mesh;
    Vec3 m_d;
    Vec3 m_e1;
    Vec3 m_e2;
    double m_u0 = 0.0;
    double m_v0 = 0.0;
    double m_su = 1.0;
    double m_sv = 1.0;
    int m_nu = 1;
    int m_nv = 1;
    std::vector<std::uint32_t> m_start;     ///< Per cell, into m_triangles; one extra at the end.
    std::vector<std::uint32_t> m_triangles;
};

/// Moves each point along `direction` onto the first skin of the mesh seen from outside
/// (from the far side of `direction`). Points whose line misses the mesh stay where they
/// are; `missed` (optional, same size as `points`) marks them, and `hits` (optional) counts
/// the others. Used to drape a sketch drawn on a plane over a curved surface.
[[nodiscard]] std::vector<Vec3> projectOntoMesh(const TriangleMesh& mesh, const std::vector<Vec3>& points,
                                                const Vec3& direction, std::size_t* hits = nullptr,
                                                std::vector<char>* missed = nullptr);

/// Where the plane through `origin` with `normal` cuts the mesh, as line segments (pairs
/// of points), from triangles within `radius` of `origin`. Shows the surface's line in a
/// sketch drawn across it (a fin's root).
[[nodiscard]] std::vector<Vec3> sliceMesh(const TriangleMesh& mesh, const Vec3& origin, const Vec3& normal,
                                          double radius);

/// Distance from a point to a triangle.
[[nodiscard]] double pointTriangleDistance(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

struct SmoothRegionOptions {
    /// Largest angle between the normals of two neighbouring triangles that still joins
    /// them (degrees). Stops at creases and sharp edges.
    double maxNeighbourAngleDeg = 20.0;
    /// Largest angle between a triangle normal and the seed normal (degrees). 180 = off.
    /// Limits how far the region bends, e.g. to take one side of a rounded box.
    double maxSeedAngleDeg = 180.0;
};

/// Flood fill from `seed` across edges while the surface stays smooth.
[[nodiscard]] TriangleSet selectSmoothRegion(const TriangleMesh& mesh, const MeshAdjacency& adjacency,
                                             std::uint32_t seed, const SmoothRegionOptions& options);

/// Splits `selection` (all triangles when empty) into smooth regions: edge-connected
/// patches without a crease sharper than `creaseAngleDeg` between neighbours. Regions
/// smaller than `minArea` (mm^2) are merged into the neighbour they share most edges with.
/// Largest region first; each region sorted.
[[nodiscard]] std::vector<TriangleSet> segmentSmoothRegions(const TriangleMesh& mesh, const MeshAdjacency& adjacency,
                                                            const TriangleSet& selection, double creaseAngleDeg,
                                                            double minArea);

/// Triangles with their centroid within `radius` of `center`. With `adjacency`, only the
/// ones connected to `seed` inside the sphere (so a brush does not go through thin walls).
[[nodiscard]] TriangleSet selectInSphere(const TriangleMesh& mesh, const Vec3& center, double radius,
                                         const MeshAdjacency* adjacency = nullptr,
                                         std::uint32_t seed = MeshAdjacency::none);

/// One brush dab at `point` (on or near the surface): the triangles within `radius`.
/// With `adjacency`, only those connected to the triangle under `point`.
[[nodiscard]] TriangleSet selectBrushDab(const TriangleMesh& mesh, const MeshAdjacency* adjacency, const Vec3& point,
                                         double radius);

/// Camera (pinhole or orthographic) in model coordinates (mm, Z up), as seen in the viewport.
struct ViewParams {
    Vec3 eye;
    Vec3 forward;          ///< Viewing direction.
    Vec3 up;               ///< Approximate up direction of the screen.
    double fovYDeg = 40.0; ///< Vertical field of view.
    double aspect = 1.0;   ///< Viewport width / height.
    /// Parallel projection; `height` is then the viewport height in mm and fovYDeg is unused.
    bool orthographic = false;
    double height = 0.0;
};

struct LassoOptions {
    /// Only triangles visible from the camera (not hidden behind other triangles of the
    /// same mesh). False selects through the model.
    bool visibleOnly = true;
};

/// Triangles whose centroid projects inside `polygon`. Polygon points are normalized
/// viewport coordinates: x from 0 (left) to 1 (right), y from 0 (top) to 1 (bottom).
[[nodiscard]] TriangleSet selectInLasso(const TriangleMesh& mesh, const ViewParams& view,
                                        const std::vector<std::array<double, 2>>& polygon,
                                        const LassoOptions& options = {});

/// Adds (steps > 0) or removes (steps < 0) rings of edge-neighbour triangles.
[[nodiscard]] TriangleSet growSelection(const MeshAdjacency& adjacency, const TriangleSet& selection, int steps);

/// All triangles not in `selection`.
[[nodiscard]] TriangleSet invertSelection(std::size_t triangleCount, const TriangleSet& selection);

enum class SelectionMode { Replace, Add, Subtract };

/// Combines an existing selection with a new one.
[[nodiscard]] TriangleSet combineSelection(const TriangleSet& current, const TriangleSet& change, SelectionMode mode);

/// Sorts and removes duplicates.
void normalizeSelection(TriangleSet& selection);

/// Total area of the selected triangles (mm^2).
[[nodiscard]] double selectionArea(const TriangleMesh& mesh, const TriangleSet& selection);

/// Bounding box of the selected triangles (empty when nothing valid is selected).
[[nodiscard]] BoundingBox selectionBounds(const TriangleMesh& mesh, const TriangleSet& selection);

} // namespace aerocad::core
