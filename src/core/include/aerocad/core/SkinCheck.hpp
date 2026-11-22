// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Cancel.hpp"
#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/MeshSelection.hpp"

#include <cstddef>
#include <memory>
#include <utility>

namespace aerocad::core {

/// How far a part reaches behind the surface of a scan (CAD-304).
struct BehindSkin {
    /// Vertices of the part that were judged: those with a closest scan point that is not on
    /// the scan's border (on the border, which side the vertex is on cannot be told).
    std::size_t examined = 0;
    /// Of those, vertices more than `tolerance` behind the scan's surface.
    std::size_t behind = 0;
    /// The deepest of them, mm, and where.
    double deepest = 0.0;
    Vec3 at{};
    [[nodiscard]] double share() const
    {
        return examined == 0 ? 0.0 : static_cast<double>(behind) / static_cast<double>(examined);
    }
};

/// A scan prepared for being measured against: its search tree and neighbours. Build it once per
/// scan and use it for every part made on that scan.
struct SkinIndex {
    explicit SkinIndex(std::shared_ptr<const TriangleMesh> mesh)
        : bvh(mesh), adjacency(adjacencyFor(mesh)), skin(std::move(mesh))
    {
    }
    MeshBvh bvh;
    std::shared_ptr<const MeshAdjacency> adjacency;
    std::shared_ptr<const TriangleMesh> skin;
};

/// Measures how far the vertices of `part` lie behind the surface of `skin`, "behind" being
/// the side the skin's triangles face away from (the scan's outward side, by its winding). Each
/// vertex is compared with the closest point of the skin and that triangle's plane. A scan is
/// an open surface with no inside, so this only says where a part is on the wrong side of the
/// skin it was drawn on; it cannot say it collides with anything. Only the vertices of `part` are
/// judged, not the inside of its triangles. A part of more than 200,000 vertices is sampled
/// (one random vertex of each block of the stride). Vertices next to triangles of the skin whose
/// winding is inconsistent, or whose closest point is on a border of the skin, are not judged.
/// `cancelled` is polled; core::Cancelled is thrown when it returns true.
[[nodiscard]] BehindSkin measureBehindSkin(const TriangleMesh& part, const SkinIndex& skin, double tolerance,
                                           const CancelCheck& cancelled = {});

/// The same, building the index for this one call.
[[nodiscard]] BehindSkin measureBehindSkin(const TriangleMesh& part, const std::shared_ptr<const TriangleMesh>& skin,
                                           double tolerance, const CancelCheck& cancelled = {});

} // namespace aerocad::core
