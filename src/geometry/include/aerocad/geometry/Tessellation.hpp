// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <vector>

namespace aerocad::geometry {

struct TessellationParams {
    /// Linear deflection relative to the bounding box diagonal (e.g. 0.001 = 0.1 %).
    double relativeLinearDeflection = 0.001;
    /// Angular deflection in radians.
    double angularDeflection = 0.5;
};

/// Triangulates all faces of the shape. Face orientation is respected, so triangle
/// winding is counter-clockwise when seen from outside a solid.
[[nodiscard]] core::TriangleMesh tessellate(const Shape& shape, const TessellationParams& params = {});

/// Approximates all edges of the shape by line segments, returned as pairs of points
/// (start, end, start, end, ...). Used to draw sketch curves and body edges.
[[nodiscard]] std::vector<core::Vec3> edgeSegments(const Shape& shape, const TessellationParams& params = {});

} // namespace aerocad::geometry
