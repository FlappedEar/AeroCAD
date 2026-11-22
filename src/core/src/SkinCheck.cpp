// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/SkinCheck.hpp"

#include "aerocad/core/MeshQuery.hpp"
#include "aerocad/core/MeshSelection.hpp"

#include <algorithm>
#include <cmath>

namespace aerocad::core {

namespace {

/// Distance from `p` to the segment a-b.
double segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
{
    const Vec3 ab = b - a;
    const double len2 = ab.dot(ab);
    const double t = len2 > 0.0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return (p - (a + ab * t)).length();
}

Vec3 unitNormal(const TriangleMesh& mesh, std::uint32_t t)
{
    const auto& tri = mesh.triangles()[t];
    const auto& v = mesh.vertices();
    const Vec3 n = (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]);
    const double len = n.length();
    return len == 0.0 ? Vec3{} : n * (1.0 / len);
}

/// The direction that tells outside from behind at the closest point `c` of triangle `t`. Inside a
/// triangle it is the triangle's normal. At an edge or a corner, where the closest point of a point
/// next to a crease (a part standing in a concave corner of the scan) has two sides, the normals of
/// the triangles that meet there are added (angle weighted at a corner), so the sign is right on
/// both sides of the crease.
Vec3 sideNormal(const TriangleMesh& mesh, const MeshAdjacency& adjacency, std::uint32_t t, const Vec3& c, double eps)
{
    const auto& tri = mesh.triangles()[t];
    const auto& v = mesh.vertices();
    Vec3 sum = unitNormal(mesh, t);
    // Corner: walk the fan of triangles round it.
    for (std::size_t k = 0; k < 3; ++k) {
        if ((c - v[tri[k]]).length() > eps)
            continue;
        const std::uint32_t corner = tri[k];
        Vec3 total{};
        std::uint32_t cur = t;
        for (int guard = 0; guard < 4096; ++guard) {
            const auto& ct = mesh.triangles()[cur];
            std::size_t at = 0;
            while (at < 3 && ct[at] != corner)
                ++at;
            if (at == 3)
                break;
            const Vec3 e1 = v[ct[(at + 1) % 3]] - v[corner];
            const Vec3 e2 = v[ct[(at + 2) % 3]] - v[corner];
            const double l1 = e1.length(), l2 = e2.length();
            if (l1 > 0 && l2 > 0)
                total = total + unitNormal(mesh, cur) * std::acos(std::clamp(e1.dot(e2) / (l1 * l2), -1.0, 1.0));
            // Across the edge (corner, next) to the following triangle round the corner.
            const std::uint32_t next = adjacency.neighbours(cur)[at];
            if (next == MeshAdjacency::none || next == t)
                break;
            cur = next;
        }
        return total.dot(total) > 0.0 ? total : sum;
    }
    // Edge: add the neighbour's normal.
    for (std::size_t k = 0; k < 3; ++k) {
        const Vec3& a = v[tri[k]];
        const Vec3& b = v[tri[(k + 1) % 3]];
        if (segmentDistance(c, a, b) > eps)
            continue;
        const std::uint32_t other = adjacency.neighbours(t)[k];
        if (other != MeshAdjacency::none)
            sum = sum + unitNormal(mesh, other);
        break;
    }
    return sum;
}

/// True when the winding of triangle `t` disagrees with a neighbour's, or a neighbour's with its own
/// neighbours (flipped triangles of a scan). The side the normal points to is not trustworthy there.
bool windingUncertain(const MeshAdjacency& adjacency, std::uint32_t t)
{
    for (int slot = 0; slot < 3; ++slot) {
        if (adjacency.windingFlipped(t, slot))
            return true;
        const std::uint32_t other = adjacency.neighbours(t)[static_cast<std::size_t>(slot)];
        if (other == MeshAdjacency::none)
            continue;
        for (int k = 0; k < 3; ++k)
            if (adjacency.windingFlipped(other, k))
                return true;
    }
    return false;
}

} // namespace

BehindSkin measureBehindSkin(const TriangleMesh& part, const std::shared_ptr<const TriangleMesh>& skin,
                             double tolerance, const CancelCheck& cancelled)
{
    BehindSkin out;
    if (!skin || skin->isEmpty() || part.isEmpty())
        return out;
    const MeshBvh bvh(skin);
    const auto adjacency = adjacencyFor(skin);
    const double reach = part.bounds().diagonal() + tolerance;
    const auto& vertices = part.vertices();
    const auto& skinVertices = skin->vertices();
    const auto& skinTriangles = skin->triangles();
    constexpr std::size_t kMaxJudged = 200000;
    const std::size_t stride = std::max<std::size_t>(1, vertices.size() / kMaxJudged);
    // A point this close to a border edge of the scan is "on the border".
    const double borderEps = std::max(1e-6, part.bounds().diagonal() * 1e-7);

    for (std::size_t i = 0; i < vertices.size(); i += stride) {
        if ((i / stride & 1023) == 0)
            throwIfCancelled(cancelled);
        const Vec3& v = vertices[i];
        const auto closest = bvh.closestPoint(v, reach);
        if (!closest)
            continue;
        const auto t = static_cast<std::uint32_t>(closest->triangle);
        const auto& tri = skinTriangles[t];
        bool onBorder = false;
        for (int slot = 0; slot < 3 && !onBorder; ++slot)
            if (adjacency->neighbours(t)[static_cast<std::size_t>(slot)] == MeshAdjacency::none)
                onBorder = segmentDistance(closest->point, skinVertices[tri[static_cast<std::size_t>(slot)]],
                                           skinVertices[tri[static_cast<std::size_t>((slot + 1) % 3)]]) < borderEps;
        if (onBorder || windingUncertain(*adjacency, t))
            continue;
        const Vec3 n = sideNormal(*skin, *adjacency, t, closest->point, borderEps * 10.0);
        if (n.dot(n) == 0.0)
            continue;
        ++out.examined;
        const double behind = -(v - closest->point).dot(n * (1.0 / n.length()));
        if (behind > tolerance) {
            ++out.behind;
            if (behind > out.deepest) {
                out.deepest = behind;
                out.at = v;
            }
        }
    }
    return out;
}

} // namespace aerocad::core
