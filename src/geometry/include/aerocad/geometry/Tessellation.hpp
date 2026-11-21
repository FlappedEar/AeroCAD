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
    /// Smallest element the mesher may make, mm (0: the mesher's own choice). Without a
    /// floor, a face whose boundary is a finely wiggling edge (a fitted scan surface) gets
    /// that edge discretised at a few microns and the faces around it are meshed into
    /// slivers that cut through each other; tessellate(shape, {.., .., size}) with a floor
    /// near the linear deflection keeps them away (CAD-95).
    double minElementSize = 0.0;
};

/// Triangulates all faces of the shape. Face orientation is respected, so triangle
/// winding is counter-clockwise when seen from outside a solid.
[[nodiscard]] core::TriangleMesh tessellate(const Shape& shape, const TessellationParams& params = {});

/// The mesh settings for files and checks of an exact body: linear deflection 0.02 % of its
/// size, 8.6 degrees, and a smallest element of 2.5 times the deflection (see
/// TessellationParams::minElementSize).
[[nodiscard]] TessellationParams printTessellation(const Shape& shape);

/// The linear deflection printTessellation() works to, mm: how far a facet may lie from its
/// surface.
[[nodiscard]] double printDeflection(const Shape& shape);

/// Approximates all edges of the shape by line segments, returned as pairs of points
/// (start, end, start, end, ...). Used to draw sketch curves and body edges.
[[nodiscard]] std::vector<core::Vec3> edgeSegments(const Shape& shape, const TessellationParams& params = {});

} // namespace aerocad::geometry
