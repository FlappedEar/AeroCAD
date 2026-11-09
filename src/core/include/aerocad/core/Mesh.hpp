// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
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

    /// Axis-aligned bounds; computed on the first call after a change and then kept (the
    /// UI asks for the bounds of multi-million triangle scans many times per edit).
    /// Safe to call from several threads at once.
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
    /// Lazily computed bounds. Copies and assignments start empty, so a mesh assigned new
    /// geometry never keeps the old box.
    class BoundsCache {
    public:
        // noexcept, so containers of meshes move them instead of copying. Assigning to a
        // mesh that other threads read at the same time is a race anyway, hence no lock.
        BoundsCache() noexcept = default;
        BoundsCache(const BoundsCache&) noexcept {}
        BoundsCache& operator=(const BoundsCache&) noexcept
        {
            m_box.reset();
            return *this;
        }
        void reset()
        {
            const std::lock_guard lock(m_mutex);
            m_box.reset();
        }
        template <typename Compute>
        BoundingBox get(Compute&& compute) const
        {
            const std::lock_guard lock(m_mutex);
            if (!m_box)
                m_box = compute();
            return *m_box;
        }

    private:
        mutable std::mutex m_mutex;
        mutable std::optional<BoundingBox> m_box;
    };

    std::vector<Vec3> m_vertices;
    std::vector<Triangle> m_triangles;
    BoundsCache m_bounds;
};

/// Builds an indexed mesh from triangle soup, welding vertices with identical coordinates.
/// Degenerate triangles (two or more identical corners) are dropped.
///
/// Welding uses exact coordinate equality (-0.0 equals 0.0) and a flat open-addressing
/// hash table, so multi-million triangle scans are welded quickly.
class MeshBuilder {
public:
    /// Optional capacity hint (e.g. the facet count of a binary STL). Avoids rehashing
    /// and reallocation while building; never required for correctness.
    void reserve(std::size_t triangles);

    void addTriangle(const Vec3& a, const Vec3& b, const Vec3& c);
    /// Same as calling addTriangle() for each consecutive corner triple of `corners`
    /// (3 * count entries), but faster for large inputs.
    void addTriangles(const Vec3* corners, std::size_t count);
    [[nodiscard]] std::size_t triangleCount() const { return m_mesh.triangleCount(); }
    [[nodiscard]] TriangleMesh take();

private:
    struct Slot {
        std::uint32_t index = kEmpty; ///< vertex index, kEmpty for a free slot
        std::uint32_t tag = 0;        ///< low hash bits, filters most key comparisons
    };
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFU;

    std::uint32_t vertexIndex(const Vec3& key, std::uint64_t hash);
    void rehash(std::size_t capacity);

    TriangleMesh m_mesh;
    std::vector<Slot> m_slots; ///< size is zero or a power of two
    unsigned m_shift = 64;     ///< 64 - log2(m_slots.size())
    std::size_t m_used = 0;
};

} // namespace aerocad::core
