// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <vector>

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

/// Unit normals to draw a simplified scan smooth: three per triangle, in triangle order
/// (corner k of triangle t at 3t + k). Clustered vertices are averages over grid cells, so
/// flat normals show bands where the surface crosses a grid plane. Each corner gets the
/// area-weighted average of the faces around its vertex, every face turned to the side most
/// of them face (the largest face's side when they cancel), and then turned to the side of
/// its own triangle: flipped scan triangles are lit like the others, from either side.
/// Never a zero normal for a triangle with an area.
[[nodiscard]] std::vector<Vec3> smoothCornerNormals(const TriangleMesh& mesh);

} // namespace aerocad::core
