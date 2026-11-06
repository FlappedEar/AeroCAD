// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace aerocad::core {

/// Indexed triangle mesh. Units are millimetres.
///
/// Vertices are shared between triangles (importers weld identical positions), so the
/// mesh can later be used for topology queries such as region selection.
class TriangleMesh {
public:
    using Triangle = std::array<std::uint32_t, 3>;

    TriangleMesh() = default;
    TriangleMesh(std::vector<Vec3> vertices, std::vector<Triangle> triangles);

    [[nodiscard]] const std::vector<Vec3>& vertices() const { return m_vertices; }
    [[nodiscard]] const std::vector<Triangle>& triangles() const { return m_triangles; }
    [[nodiscard]] std::size_t vertexCount() const { return m_vertices.size(); }
    [[nodiscard]] std::size_t triangleCount() const { return m_triangles.size(); }
    [[nodiscard]] bool isEmpty() const { return m_triangles.empty(); }

    /// Appends a vertex and returns its index.
    std::uint32_t addVertex(const Vec3& v);
    void addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c);

    /// Appends another mesh (indices are offset accordingly).
    void append(const TriangleMesh& other);

    void reserve(std::size_t vertices, std::size_t triangles);

    [[nodiscard]] BoundingBox bounds() const;

    /// Unit normal of a triangle (zero vector for degenerate triangles).
    [[nodiscard]] Vec3 faceNormal(std::size_t triangle) const;

    /// Total surface area in mm^2.
    [[nodiscard]] double surfaceArea() const;

    /// Uniform scale about a point.
    void scale(double factor, const Vec3& origin);

    /// True when all triangle indices refer to existing vertices.
    [[nodiscard]] bool isValid() const;

private:
    std::vector<Vec3> m_vertices;
    std::vector<Triangle> m_triangles;
};

/// Builds an indexed mesh from triangle soup, welding vertices with identical coordinates.
/// Degenerate triangles (two or more identical corners) are dropped.
class MeshBuilder {
public:
    void addTriangle(const Vec3& a, const Vec3& b, const Vec3& c);
    [[nodiscard]] std::size_t triangleCount() const { return m_mesh.triangleCount(); }
    [[nodiscard]] TriangleMesh take();

private:
    struct VecHash {
        std::size_t operator()(const Vec3& v) const noexcept;
    };

    std::uint32_t vertexIndex(const Vec3& v);

    TriangleMesh m_mesh;
    std::unordered_map<Vec3, std::uint32_t, VecHash> m_index;
};

} // namespace aerocad::core
