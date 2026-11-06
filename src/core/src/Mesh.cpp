// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Mesh.hpp"

#include <functional>
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
    BoundingBox box;
    for (const auto& v : m_vertices)
        box.extend(v);
    return box;
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

std::size_t MeshBuilder::VecHash::operator()(const Vec3& v) const noexcept
{
    const std::hash<double> h;
    std::size_t seed = h(v.x);
    seed ^= h(v.y) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    seed ^= h(v.z) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint32_t MeshBuilder::vertexIndex(const Vec3& v)
{
    // Normalise -0.0 to 0.0 so both hash and compare equal.
    const Vec3 key{v.x + 0.0, v.y + 0.0, v.z + 0.0};
    auto [it, inserted] = m_index.try_emplace(key, 0U);
    if (inserted)
        it->second = m_mesh.addVertex(key);
    return it->second;
}

void MeshBuilder::addTriangle(const Vec3& a, const Vec3& b, const Vec3& c)
{
    const auto ia = vertexIndex(a);
    const auto ib = vertexIndex(b);
    const auto ic = vertexIndex(c);
    if (ia == ib || ib == ic || ia == ic)
        return;
    m_mesh.addTriangle(ia, ib, ic);
}

TriangleMesh MeshBuilder::take()
{
    m_index.clear();
    return std::exchange(m_mesh, TriangleMesh{});
}

} // namespace aerocad::core
