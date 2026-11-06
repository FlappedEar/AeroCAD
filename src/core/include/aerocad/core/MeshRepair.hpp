// SPDX-License-Identifier: GPL-3.0-or-later
// Basic repair and statistics for scanned meshes: welding, duplicate and degenerate
// faces, consistent normals, small hole filling.
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>

namespace aerocad::core {

/// Topology statistics of a mesh as stored (vertex indices are taken as they are).
struct MeshStats {
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    /// Vertices referenced by no triangle.
    std::size_t unusedVertices = 0;
    /// Triangles with repeated indices or (near) zero area.
    std::size_t degenerateTriangles = 0;
    /// Triangles that use the same three vertices as an earlier triangle (any winding).
    std::size_t duplicateTriangles = 0;
    /// Edges used by exactly one triangle.
    std::size_t boundaryEdges = 0;
    /// Closed loops of boundary edges (holes and open borders).
    std::size_t holes = 0;
    /// Edges shared by more than two triangles.
    std::size_t nonManifoldEdges = 0;
    /// Manifold edges whose two triangles disagree on winding (flipped normals).
    std::size_t inconsistentEdges = 0;
    /// Edge-connected components (shells).
    std::size_t components = 0;
};

[[nodiscard]] MeshStats meshStats(const TriangleMesh& mesh);

struct RepairOptions {
    /// Merge vertices closer than this distance (mm). 0 merges only identical positions.
    double mergeDistance = 0.0;
    bool removeDegenerate = true;
    bool removeDuplicates = true;
    bool removeUnusedVertices = true;
    /// Make the winding consistent across each shell; closed shells end up facing outwards.
    bool fixOrientation = true;
    /// Fill holes bounded by at most this many edges. 0 disables hole filling.
    std::size_t maxHoleEdges = 0;
};

struct RepairReport {
    std::size_t mergedVertices = 0;
    std::size_t removedDegenerate = 0;
    std::size_t removedDuplicates = 0;
    std::size_t removedUnusedVertices = 0;
    std::size_t flippedTriangles = 0;
    std::size_t filledHoles = 0;
    std::size_t addedTriangles = 0;
};

/// Returns a repaired copy of the mesh. Steps run in this order: merge, degenerate,
/// duplicates, orientation, hole filling, unused vertices.
[[nodiscard]] TriangleMesh repairMesh(const TriangleMesh& mesh, const RepairOptions& options,
                                      RepairReport* report = nullptr);

/// Reverses the winding (and so the normals) of all triangles.
void flipNormals(TriangleMesh& mesh);

} // namespace aerocad::core
