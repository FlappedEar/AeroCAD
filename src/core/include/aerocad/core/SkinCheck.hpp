// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Cancel.hpp"
#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <memory>

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
    [[nodiscard]] double share() const { return examined == 0 ? 0.0 : static_cast<double>(behind) / static_cast<double>(examined); }
};

/// Measures how far the vertices of `part` lie behind the surface of `skin`, "behind" being
/// the side the skin's triangles face away from (the scan's outward side, by its winding). Each
/// vertex is compared with the closest point of the skin and that triangle's plane. A scan is
/// an open surface with no inside, so this only says where a part is on the wrong side of the
/// skin it was drawn on; it cannot say it collides with anything. Parts of up to a few million
/// vertices are sampled (about 200,000 vertices judged). `cancelled` is polled;
/// core::Cancelled is thrown when it returns true.
[[nodiscard]] BehindSkin measureBehindSkin(const TriangleMesh& part, const std::shared_ptr<const TriangleMesh>& skin,
                                           double tolerance, const CancelCheck& cancelled = {});

} // namespace aerocad::core
