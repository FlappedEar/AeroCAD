// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>

namespace aerocad::core {

/// Simplifies a mesh by vertex clustering on a uniform grid: all vertices in one cell
/// collapse to their average, triangles that collapse are dropped. Fast (linear) and
/// robust on scans, so it is used for display levels of detail; fitting and modelling
/// keep the full-resolution mesh.
///
/// The grid size is chosen so the result has at most about `targetTriangles` triangles.
/// Meshes already at or below the target are returned unchanged, and so are meshes that
/// clustering would collapse completely (the result is never empty for a non-empty input).
[[nodiscard]] TriangleMesh decimateToTarget(const TriangleMesh& mesh, std::size_t targetTriangles);

/// One clustering pass with a given cell size (mm).
[[nodiscard]] TriangleMesh clusterVertices(const TriangleMesh& mesh, double cellSize);

} // namespace aerocad::core
