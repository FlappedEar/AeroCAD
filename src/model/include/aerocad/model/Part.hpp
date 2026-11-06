// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace aerocad::model {

using PartId = std::uint64_t;

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
    bool visible = true;
    /// Incremented on every change, so views can cheaply detect stale caches.
    std::uint64_t revision = 0;
};

} // namespace aerocad::model
