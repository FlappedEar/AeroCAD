// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aerocad::core {

/// Two triangles of a mesh that cross each other.
struct MeshCrossing {
    /// Triangle indices in the welded mesh (see findSelfIntersections()).
    std::uint32_t a = 0, b = 0;
    /// Ends of the segment where the two triangles cut through each other. Equal for
    /// coplanar overlaps, which have an area and no segment.
    Vec3 from{}, to{};
    bool coplanar = false;
    [[nodiscard]] double length() const;
};

struct SelfIntersections {
    /// Number of crossing triangle pairs (not counting pairs that only share an edge or a
    /// vertex, or only touch).
    std::size_t pairs = 0;
    /// Length of the longest crossing segment, mm.
    double longest = 0.0;
    /// The first few pairs found, for the message and for tests.
    std::vector<MeshCrossing> examples;
    [[nodiscard]] bool any() const { return pairs > 0; }
};

/// Finds the triangles of a mesh that pass through other triangles of the same mesh.
///
/// Vertices at identical positions are merged first (B-rep tessellations keep one vertex
/// per face), and the triangle indices of the result refer to that welded mesh. Triangles
/// that share an edge or a corner are only reported when they fold back onto each other or
/// when the free edge of one pierces the other. Pairs that merely touch are not reported;
/// a crossing needs a segment longer than `minLength` mm (0: any positive length).
/// The arithmetic is long double with no tolerance on the signs, so a crossing of a few
/// microns on a part of metres is found as well as a large one.
[[nodiscard]] SelfIntersections findSelfIntersections(const TriangleMesh& mesh, double minLength = 0.0,
                                                      std::size_t maxExamples = 16);

struct IntersectionRepair {
    /// The mesh with coinciding vertices merged and the crossings that could be lifted
    /// removed.
    TriangleMesh mesh;
    /// What still crosses after the repair (empty on success).
    SelfIntersections remaining;
    /// Vertices that were moved, and the largest distance one moved from where it started.
    std::size_t moved = 0;
    double largestMove = 0.0;
};

/// Removes crossings that come from a mesh being a chord approximation of its surfaces: two
/// faces that meet at a shallow angle (a fillet running out tangent onto a surface) or along
/// an edge whose tolerance is looser than the mesh deflection cut through each other by a
/// few microns. Each crossing is lifted by moving the end of the cutting edge that is
/// closest to the plane of the triangle it cuts to the other side of that plane, by a margin
/// that survives rounding to float. No vertex moves further than `maxMove` mm from where it
/// started (use the deflection the mesh was made with); crossings that would need more are
/// left and reported in `remaining`. `roundToFloat` first rounds every vertex to float
/// precision, as STL stores it, so the result is the same mesh as the file's.
[[nodiscard]] IntersectionRepair untangleSelfIntersections(const TriangleMesh& mesh, double maxMove,
                                                           bool roundToFloat = true);

} // namespace aerocad::core
