// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Mesh.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace aerocad::core {

TriangleMesh::TriangleMesh(std::vector<Vec3> vertices, std::vector<Triangle> triangles)
    : m_vertices(std::move(vertices)), m_triangles(std::move(triangles))
{
    if (!isValid())
        throw std::invalid_argument("TriangleMesh: triangle index out of range");
}

std::uint32_t TriangleMesh::addVertex(const Vec3& v)
{
    m_vertices.push_back(v);
    m_bounds.reset();
    return static_cast<std::uint32_t>(m_vertices.size() - 1);
}

void TriangleMesh::addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    const auto n = m_vertices.size();
    if (a >= n || b >= n || c >= n)
        throw std::out_of_range("TriangleMesh::addTriangle: index out of range");
    m_triangles.push_back({a, b, c});
}

void TriangleMesh::append(const TriangleMesh& other)
{
    const auto offset = static_cast<std::uint32_t>(m_vertices.size());
    m_vertices.insert(m_vertices.end(), other.m_vertices.begin(), other.m_vertices.end());
    m_bounds.reset();
    m_triangles.reserve(m_triangles.size() + other.m_triangles.size());
    for (const auto& t : other.m_triangles)
        m_triangles.push_back({t[0] + offset, t[1] + offset, t[2] + offset});
}

void TriangleMesh::reserve(std::size_t vertices, std::size_t triangles)
{
    m_vertices.reserve(vertices);
    m_triangles.reserve(triangles);
}

BoundingBox TriangleMesh::bounds() const
{
    return m_bounds.get([this] {
        BoundingBox box;
        for (const auto& v : m_vertices)
            box.extend(v);
        return box;
    });
}

Vec3 TriangleMesh::faceNormal(std::size_t triangle) const
{
    const auto& t = m_triangles.at(triangle);
    const Vec3& a = m_vertices[t[0]];
    const Vec3& b = m_vertices[t[1]];
    const Vec3& c = m_vertices[t[2]];
    return (b - a).cross(c - a).normalized();
}

double TriangleMesh::surfaceArea() const
{
    double area = 0.0;
    for (const auto& t : m_triangles) {
        const Vec3& a = m_vertices[t[0]];
        const Vec3& b = m_vertices[t[1]];
        const Vec3& c = m_vertices[t[2]];
        area += 0.5 * (b - a).cross(c - a).length();
    }
    return area;
}

void TriangleMesh::scale(double factor, const Vec3& origin)
{
    for (auto& v : m_vertices)
        v = origin + (v - origin) * factor;
    m_bounds.reset();
}

bool TriangleMesh::isValid() const
{
    const auto n = m_vertices.size();
    for (const auto& t : m_triangles) {
        if (t[0] >= n || t[1] >= n || t[2] >= n)
            return false;
    }
    return true;
}

namespace {

std::uint64_t doubleBits(double d)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(d));
    std::memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/// 64-bit hash of the coordinate bit patterns. Coordinates converted from float (STL)
/// have many zero low mantissa bits, so every step folds high bits down before multiplying.
std::uint64_t hashVec(const Vec3& v)
{
    std::uint64_t h = doubleBits(v.x) * 0x9E3779B97F4A7C15ULL;
    h = ((h ^ (h >> 32U)) + doubleBits(v.y)) * 0xC2B2AE3D27D4EB4FULL;
    h = ((h ^ (h >> 29U)) + doubleBits(v.z)) * 0x165667B19E3779F9ULL;
    return h ^ (h >> 32U);
}

/// Smallest power of two table size keeping the load factor at or below 1/2.
std::size_t tableSizeFor(std::size_t entries)
{
    std::size_t size = 16;
    while (size / 2 < entries)
        size *= 2;
    return size;
}

void prefetch(const void* address)
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address);
#else
    static_cast<void>(address);
#endif
}

} // namespace

void MeshBuilder::reserve(std::size_t triangles)
{
    // Closed scan meshes have about half as many vertices as triangles.
    const std::size_t vertices = triangles / 2 + 16;
    m_mesh.reserve(vertices, triangles);
    if (tableSizeFor(vertices) > m_slots.size())
        rehash(tableSizeFor(vertices));
}

void MeshBuilder::rehash(std::size_t capacity)
{
    m_slots.assign(capacity, Slot{});
    m_shift = 64U - static_cast<unsigned>(std::countr_zero(capacity));
    const std::size_t mask = capacity - 1;
    const auto& vertices = m_mesh.vertices();
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const std::uint64_t h = hashVec(vertices[i]);
        std::size_t pos = static_cast<std::size_t>(h >> m_shift);
        while (m_slots[pos].index != kEmpty)
            pos = (pos + 1) & mask;
        m_slots[pos] = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(h)};
    }
}

std::uint32_t MeshBuilder::vertexIndex(const Vec3& key, std::uint64_t hash)
{
    const auto tag = static_cast<std::uint32_t>(hash);
    const std::size_t mask = m_slots.size() - 1;
    const auto& vertices = m_mesh.vertices();
    std::size_t pos = static_cast<std::size_t>(hash >> m_shift);
    while (true) {
        Slot& slot = m_slots[pos];
        if (slot.index == kEmpty) {
            slot = {m_mesh.addVertex(key), tag};
            ++m_used;
            return slot.index;
        }
        if (slot.tag == tag && vertices[slot.index] == key)
            return slot.index;
        pos = (pos + 1) & mask;
    }
}

void MeshBuilder::addTriangle(const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 corners[3] = {a, b, c};
    addTriangles(corners, 1);
}

void MeshBuilder::addTriangles(const Vec3* corners, std::size_t count)
{
    // Big meshes make the table and the vertex array much larger than the CPU caches, so
    // a naive lookup waits for two cache misses per corner. Batches are processed in three
    // passes: hash and prefetch the home slots, prefetch the candidate vertices, then do
    // the real (sequential, exact) lookups. The first two passes are hints only.
    constexpr std::size_t kBatch = 32; // triangles
    std::uint64_t hashes[kBatch * 3];
    Vec3 keys[kBatch * 3];
    for (std::size_t first = 0; first < count; first += kBatch) {
        const std::size_t n = std::min(kBatch, count - first) * 3;
        const Vec3* in = corners + first * 3;

        // Grow first so that slot positions computed below stay valid for the whole batch.
        if ((m_used + n) * 2 > m_slots.size())
            rehash(std::max(tableSizeFor(m_used + n), m_slots.size() * 2));

        for (std::size_t i = 0; i < n; ++i) {
            // Normalise -0.0 to 0.0 so both hash and compare equal.
            keys[i] = {in[i].x + 0.0, in[i].y + 0.0, in[i].z + 0.0};
            hashes[i] = hashVec(keys[i]);
            prefetch(&m_slots[static_cast<std::size_t>(hashes[i] >> m_shift)]);
        }
        const auto& vertices = m_mesh.vertices();
        for (std::size_t i = 0; i < n; ++i) {
            const Slot& slot = m_slots[static_cast<std::size_t>(hashes[i] >> m_shift)];
            if (slot.index != kEmpty && slot.tag == static_cast<std::uint32_t>(hashes[i]))
                prefetch(&vertices[slot.index]);
        }
        for (std::size_t i = 0; i < n; i += 3) {
            const auto ia = vertexIndex(keys[i], hashes[i]);
            const auto ib = vertexIndex(keys[i + 1], hashes[i + 1]);
            const auto ic = vertexIndex(keys[i + 2], hashes[i + 2]);
            if (ia == ib || ib == ic || ia == ic)
                continue;
            m_mesh.addTriangle(ia, ib, ic);
        }
    }
}

TriangleMesh MeshBuilder::take()
{
    m_slots = {};
    m_shift = 64;
    m_used = 0;
    return std::exchange(m_mesh, TriangleMesh{});
}

} // namespace aerocad::core
