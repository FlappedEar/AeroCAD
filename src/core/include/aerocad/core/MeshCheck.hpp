// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aerocad::core {

/// Topology report of a triangle mesh, used before export (3D printing, CFD meshing).
///
/// Vertices with identical coordinates are treated as one (B-rep tessellations keep
/// separate vertices per face), so the report describes the shape, not the index layout.
struct MeshCheck {
    /// Distinct vertex positions.
    std::size_t vertices = 0;
    std::size_t triangles = 0;
    /// Edges used by exactly one triangle (holes, open borders).
    std::size_t boundaryEdges = 0;
    /// Edges shared by more than two triangles.
    std::size_t nonManifoldEdges = 0;
    /// Edges shared by two triangles that traverse it in the same direction,
    /// i.e. neighbouring triangles with opposite winding.
    std::size_t inconsistentEdges = 0;
    /// Triangles with (near) zero area, or with repeated vertex indices.
    std::size_t degenerateTriangles = 0;
    /// Edge-connected components.
    std::size_t shells = 0;
    /// Signed enclosed volume in mm^3 (divergence theorem). Positive when a closed,
    /// consistently oriented mesh has outward-facing normals. Meaningless if not closed.
    double signedVolume = 0.0;

    /// Every edge is shared by exactly two triangles.
    [[nodiscard]] bool closed() const { return boundaryEdges == 0 && nonManifoldEdges == 0 && triangles > 0; }
    [[nodiscard]] bool manifold() const { return nonManifoldEdges == 0; }
    /// Neighbouring triangles agree on winding, and for closed meshes normals point outwards.
    [[nodiscard]] bool oriented() const { return inconsistentEdges == 0 && (!closed() || signedVolume > 0.0); }
    /// Closed, manifold, consistently and outward oriented, without degenerate triangles:
    /// a watertight solid suitable for slicing or volume meshing.
    [[nodiscard]] bool watertight() const { return closed() && oriented() && degenerateTriangles == 0; }

    /// Human-readable list of problems (empty when watertight()).
    [[nodiscard]] std::vector<std::string> problems() const;
};

/// Copy of the mesh with vertices at identical positions merged and triangles that
/// collapse to an edge or point dropped. Used before exporting formats that share vertices.
[[nodiscard]] TriangleMesh weldVertices(const TriangleMesh& mesh);

/// Analyses mesh topology. Linear in the number of triangles.
[[nodiscard]] MeshCheck checkMesh(const TriangleMesh& mesh);

} // namespace aerocad::core
