// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aerocad::model {

using PartId = std::uint64_t;

/// What a part represents in the document.
enum class PartKind {
    Imported, ///< Reference model loaded from a file (mesh and/or B-rep).
    Body,     ///< Solid produced by the design timeline (CAD features).
    Sketch,   ///< Sketch of the design timeline (curves and profiles).
    Plane,    ///< Construction plane of the design timeline (drawn as a square).
};

/// A body in the project: an imported reference model or (later) a designed part.
struct Part {
    PartId id = 0;
    std::string name;
    /// Where the part came from (file path), empty for parts created in the app.
    std::string source;
    /// Display / mesh representation in millimetres.
    std::shared_ptr<const core::TriangleMesh> mesh;
    /// Exact geometry when available (STEP import, CAD operations).
    std::optional<geometry::Shape> shape;
    /// Selected triangles of `mesh` (region selection for surface fitting); null = none.
    /// Must be cleared when an operation changes the mesh topology.
    std::shared_ptr<const core::TriangleSet> selection;
    PartKind kind = PartKind::Imported;
    /// For bodies and sketches: id of the timeline feature that owns this part.
    std::uint64_t featureId = 0;
    /// Lines drawn in the viewport (sketch curves, body edges) as pairs of points
    /// (segment start, end), mm.
    std::vector<core::Vec3> edgeSegments;
    bool visible = true;
    /// Incremented on every change, so views can cheaply detect stale caches.
    std::uint64_t revision = 0;
};

/// The part a derived part was made from, read from its `source` ("surface.fit_selection
/// of part 3" -> 3): e.g. the scan a surface was fitted to. 0 when there is none.
[[nodiscard]] inline PartId sourcePartId(const Part& part)
{
    static const std::string marker = " of part ";
    const auto at = part.source.rfind(marker);
    if (at == std::string::npos)
        return 0;
    PartId id = 0;
    for (std::size_t i = at + marker.size(); i < part.source.size(); ++i) {
        const char c = part.source[i];
        if (c < '0' || c > '9')
            return 0;
        id = id * 10 + static_cast<PartId>(c - '0');
    }
    return id;
}

/// The scan a reference surface was fitted to (surface.fit_selection), 0 for anything else:
/// a part made from a scan, for one, is a shape of its own and does not stand in for the scan.
[[nodiscard]] inline PartId fittedScanId(const Part& part)
{
    return part.source.rfind("surface.fit_selection of part ", 0) == 0 ? sourcePartId(part) : 0;
}

} // namespace aerocad::model
