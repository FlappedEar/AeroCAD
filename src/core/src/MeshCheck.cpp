// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshCheck.hpp"

#include <cmath>
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

/// Number of crossings of the ray origin + s * dir (s > 0) with the given triangles.
std::size_t rayCrossings(const Vec3& origin, const Vec3& dir, const std::vector<Vec3>& verts,
                         const std::vector<TriangleMesh::Triangle>& tris, const std::vector<std::uint32_t>& remap,
                         const std::vector<std::uint32_t>& triangles)
{
    std::size_t crossings = 0;
    for (const std::uint32_t t : triangles) {
        // Moller-Trumbore, coordinates relative to the ray origin.
        const Vec3 a = verts[remap[tris[t][0]]] - origin;
        const Vec3 e1 = verts[remap[tris[t][1]]] - origin - a;
        const Vec3 e2 = verts[remap[tris[t][2]]] - origin - a;
        const Vec3 p = dir.cross(e2);
        const double det = e1.dot(p);
        if (std::abs(det) < 1e-300)
            continue;
        const double inv = 1.0 / det;
        const Vec3 s = a * -1.0;
        const double u = s.dot(p) * inv;
        if (u < 0.0 || u > 1.0)
            continue;
        const Vec3 q = s.cross(e1);
        const double v = dir.dot(q) * inv;
        if (v < 0.0 || u + v > 1.0)
            continue;
        if (e2.dot(q) * inv > 0.0)
            ++crossings;
    }
    return crossings;
}

bool boxContains(const BoundingBox& outer, const BoundingBox& inner)
{
    return outer.min.x <= inner.min.x && outer.min.y <= inner.min.y && outer.min.z <= inner.min.z &&
           outer.max.x >= inner.max.x && outer.max.y >= inner.max.y && outer.max.z >= inner.max.z;
}

/// Splits the mesh into edge-connected shells, sums their signed volumes and, for closed
/// meshes, checks each shell's orientation against its nesting depth: outer shells must
/// enclose positive volume, cavities (inside an odd number of other shells) negative.
void analyseShells(MeshCheck& check, const std::vector<Vec3>& verts, const std::vector<TriangleMesh::Triangle>& tris,
                   const std::vector<std::uint32_t>& remap, DisjointSets& components)
{
    struct Shell {
        std::vector<std::uint32_t> triangles;
        double volume6 = 0.0;
        BoundingBox box;
    };
    std::vector<Shell> shells;
    std::unordered_map<std::uint32_t, std::size_t> shellOfRoot;
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const std::uint32_t root = components.find(static_cast<std::uint32_t>(t));
        const auto [it, inserted] = shellOfRoot.try_emplace(root, shells.size());
        if (inserted)
            shells.emplace_back();
        shells[it->second].triangles.push_back(static_cast<std::uint32_t>(t));
    }

    for (Shell& shell : shells) {
        // Volume relative to a point on the shell: translation does not cost precision.
        const Vec3 ref = verts[remap[tris[shell.triangles.front()][0]]];
        for (const std::uint32_t t : shell.triangles) {
            const Vec3 a = verts[remap[tris[t][0]]] - ref;
            const Vec3 b = verts[remap[tris[t][1]]] - ref;
            const Vec3 c = verts[remap[tris[t][2]]] - ref;
            shell.volume6 += a.dot(b.cross(c));
            for (const auto idx : tris[t])
                shell.box.extend(verts[remap[idx]]);
        }
        check.signedVolume += shell.volume6 / 6.0;
    }
    check.shells = shells.size();

    if (!check.closed())
        return;
    // Fixed, non axis-aligned direction so rays rarely hit edges or vertices exactly.
    const Vec3 dir = Vec3{0.5772156649, 0.6180339887, 0.5319577884}.normalized();
    for (std::size_t i = 0; i < shells.size(); ++i) {
        std::size_t depth = 0;
        if (shells.size() > 1) {
            // Centroid of one triangle: a point on this shell, inside or outside the others.
            const auto& t = tris[shells[i].triangles.front()];
            const Vec3 probe = (verts[remap[t[0]]] + verts[remap[t[1]]] + verts[remap[t[2]]]) * (1.0 / 3.0);
            for (std::size_t j = 0; j < shells.size(); ++j) {
                if (j != i && boxContains(shells[j].box, shells[i].box) &&
                    rayCrossings(probe, dir, verts, tris, remap, shells[j].triangles) % 2 == 1)
                    ++depth;
            }
        }
        const bool cavity = depth % 2 == 1;
        if (cavity ? shells[i].volume6 >= 0.0 : shells[i].volume6 <= 0.0)
            ++check.invertedShells;
    }
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

    for (std::size_t t = 0; t < tris.size(); ++t) {
        const TriangleMesh::Triangle tri{remap[tris[t][0]], remap[tris[t][1]], remap[tris[t][2]]};
        const Vec3& a = verts[tri[0]];
        const Vec3& b = verts[tri[1]];
        const Vec3& c = verts[tri[2]];

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

    analyseShells(check, verts, tris, remap, components);
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
    else if (invertedShells > 0)
        out.push_back("normals point inwards: " + plural(invertedShells, "inside-out shell", "inside-out shells"));
    if (degenerateTriangles > 0)
        out.push_back(plural(degenerateTriangles, "degenerate triangle", "degenerate triangles"));
    return out;
}

} // namespace aerocad::core
