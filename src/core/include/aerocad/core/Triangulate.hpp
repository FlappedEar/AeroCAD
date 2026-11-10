// SPDX-License-Identifier: GPL-3.0-or-later
// Triangulation of planar polygons with holes (ear clipping with hole bridging).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace aerocad::core {

using Point2 = std::array<double, 2>;

/// Triangulates a polygon with holes. `rings[0]` is the outline, the others are holes; each
/// ring lists its points once (the closing point is not repeated) in either orientation.
/// Returns triangle corner indices into the points of all rings concatenated in order
/// (outline first), three per triangle, counter-clockwise (x right, y up). Empty when no
/// triangulation was found. A port of the earcut algorithm: needs no kernel and takes
/// milliseconds for thousands of points, which the CAD kernel's mesher does not.
[[nodiscard]] std::vector<std::uint32_t> triangulatePolygon(const std::vector<std::vector<Point2>>& rings);

/// Douglas-Peucker simplification of a closed ring: no point of the original ring is farther
/// than `tolerance` from the result. At least 3 points are kept for a ring of 3 or more.
[[nodiscard]] std::vector<Point2> simplifyRing(const std::vector<Point2>& ring, double tolerance);

} // namespace aerocad::core
