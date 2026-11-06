// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshCheck.hpp"

#include <cstdint>
#include <functional>
#include <numeric>
#include <unordered_map>

namespace aerocad::core {

namespace {

struct EdgeUse {
    std::uint32_t count = 0;
    /// Uses in the direction low index -> high index.
    std::uint32_t forward = 0;
    /// First triangle using the edge (for connectivity).
    std::uint32_t triangle = 0;
};

class DisjointSets {
public:
    explicit DisjointSets(std::size_t n) : m_parent(n) { std::iota(m_parent.begin(), m_parent.end(), 0u); }

    std::uint32_t find(std::uint32_t i)
    {
        while (m_parent[i] != i) {
            m_parent[i] = m_parent[m_parent[i]];
            i = m_parent[i];
        }
        return i;
    }

    void unite(std::uint32_t a, std::uint32_t b) { m_parent[find(a)] = find(b); }

private:
    std::vector<std::uint32_t> m_parent;
};

struct PositionHash {
    std::size_t operator()(const Vec3& v) const noexcept
    {
        // +0.0 so that -0.0 and 0.0 (equal under ==) hash identically.
        const std::hash<double> h;
        std::size_t seed = h(v.x + 0.0);
        seed ^= h(v.y + 0.0) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        seed ^= h(v.z + 0.0) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }
};

/// Maps every vertex to the index of the first vertex with the same position.
/// Returns the number of distinct positions.
std::size_t canonicalVertices(const TriangleMesh& mesh, std::vector<std::uint32_t>& remap)
{
    const auto& verts = mesh.vertices();
    remap.resize(verts.size());
    std::unordered_map<Vec3, std::uint32_t, PositionHash> first;
    first.reserve(verts.size());
    for (std::size_t i = 0; i < verts.size(); ++i)
        remap[i] = first.try_emplace(verts[i], static_cast<std::uint32_t>(i)).first->second;
    return first.size();
}

std::string plural(std::size_t n, const char* singular, const char* pluralForm)
{
    return std::to_string(n) + " " + (n == 1 ? singular : pluralForm);
}

} // namespace

MeshCheck checkMesh(const TriangleMesh& mesh)
{
    MeshCheck check;
    check.vertices = mesh.vertexCount();
    check.triangles = mesh.triangleCount();
    if (mesh.isEmpty() || !mesh.isValid())
        return check;

    const auto& verts = mesh.vertices();
    const auto& tris = mesh.triangles();
    std::vector<std::uint32_t> remap;
    check.vertices = canonicalVertices(mesh, remap);

    // Relative area threshold for degenerate triangles: scale-independent.
    const double diag = mesh.bounds().diagonal();
    const double minDoubleArea = diag * diag * 1e-14;

    std::unordered_map<std::uint64_t, EdgeUse> edges;
    edges.reserve(tris.size() * 2);
    DisjointSets components(tris.size());
    double volume6 = 0.0;

    for (std::size_t t = 0; t < tris.size(); ++t) {
        const TriangleMesh::Triangle tri{remap[tris[t][0]], remap[tris[t][1]], remap[tris[t][2]]};
        const Vec3& a = verts[tri[0]];
        const Vec3& b = verts[tri[1]];
        const Vec3& c = verts[tri[2]];
        volume6 += a.dot(b.cross(c));

        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2] ||
            (b - a).cross(c - a).length() <= minDoubleArea) {
            ++check.degenerateTriangles;
        }

        for (int k = 0; k < 3; ++k) {
            const std::uint32_t from = tri[static_cast<std::size_t>(k)];
            const std::uint32_t to = tri[static_cast<std::size_t>((k + 1) % 3)];
            if (from == to)
                continue;
            const std::uint32_t lo = from < to ? from : to;
            const std::uint32_t hi = from < to ? to : from;
            const std::uint64_t key = (static_cast<std::uint64_t>(lo) << 32) | hi;
            auto [it, inserted] = edges.try_emplace(key);
            EdgeUse& use = it->second;
            if (inserted)
                use.triangle = static_cast<std::uint32_t>(t);
            else
                components.unite(static_cast<std::uint32_t>(t), use.triangle);
            ++use.count;
            if (from < to)
                ++use.forward;
        }
    }

    for (const auto& [key, use] : edges) {
        (void)key;
        if (use.count == 1)
            ++check.boundaryEdges;
        else if (use.count > 2)
            ++check.nonManifoldEdges;
        else if (use.forward != 1)
            ++check.inconsistentEdges;
    }

    for (std::size_t t = 0; t < tris.size(); ++t) {
        if (components.find(static_cast<std::uint32_t>(t)) == t)
            ++check.shells;
    }
    check.signedVolume = volume6 / 6.0;
    return check;
}

TriangleMesh weldVertices(const TriangleMesh& mesh)
{
    TriangleMesh out;
    if (!mesh.isValid())
        return out;
    std::vector<std::uint32_t> remap;
    const std::size_t distinct = canonicalVertices(mesh, remap);
    out.reserve(distinct, mesh.triangleCount());

    constexpr auto kNone = static_cast<std::uint32_t>(-1);
    std::vector<std::uint32_t> newIndex(mesh.vertexCount(), kNone);
    const auto index = [&](std::uint32_t original) {
        const std::uint32_t canonical = remap[original];
        if (newIndex[canonical] == kNone)
            newIndex[canonical] = out.addVertex(mesh.vertices()[canonical]);
        return newIndex[canonical];
    };
    for (const auto& t : mesh.triangles()) {
        const std::uint32_t a = index(t[0]);
        const std::uint32_t b = index(t[1]);
        const std::uint32_t c = index(t[2]);
        if (a != b && b != c && a != c)
            out.addTriangle(a, b, c);
    }
    return out;
}

std::vector<std::string> MeshCheck::problems() const
{
    std::vector<std::string> out;
    if (triangles == 0) {
        out.emplace_back("mesh is empty");
        return out;
    }
    if (boundaryEdges > 0)
        out.push_back("not closed: " + plural(boundaryEdges, "open edge", "open edges"));
    if (nonManifoldEdges > 0)
        out.push_back("not manifold: " + plural(nonManifoldEdges, "edge shared by more than two triangles",
                                                "edges shared by more than two triangles"));
    if (inconsistentEdges > 0)
        out.push_back("inconsistent orientation: " +
                      plural(inconsistentEdges, "edge between flipped triangles", "edges between flipped triangles"));
    else if (closed() && signedVolume <= 0.0)
        out.emplace_back("normals point inwards (negative volume)");
    if (degenerateTriangles > 0)
        out.push_back(plural(degenerateTriangles, "degenerate triangle", "degenerate triangles"));
    return out;
}

} // namespace aerocad::core
