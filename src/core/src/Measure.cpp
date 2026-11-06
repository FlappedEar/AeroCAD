// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Measure.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aerocad::core {

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const double d1 = ab.dot(ap);
    const double d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return a;

    const Vec3 bp = p - b;
    const double d3 = ab.dot(bp);
    const double d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3)
        return b;

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
        return a + ab * (d1 / (d1 - d3));

    const Vec3 cp = p - c;
    const double d5 = ab.dot(cp);
    const double d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6)
        return c;

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
        return a + ac * (d2 / (d2 - d6));

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

    const double denom = va + vb + vc;
    if (denom == 0.0) // degenerate triangle
        return a;
    const double v = vb / denom;
    const double w = vc / denom;
    return a + ab * v + ac * w;
}

std::optional<MeshPoint> closestPointOnMesh(const TriangleMesh& mesh, const Vec3& p)
{
    if (mesh.isEmpty())
        return std::nullopt;
    const auto& v = mesh.vertices();
    MeshPoint best;
    double bestSq = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        const auto& t = mesh.triangles()[i];
        const Vec3 q = closestPointOnTriangle(p, v[t[0]], v[t[1]], v[t[2]]);
        const Vec3 d = q - p;
        const double sq = d.dot(d);
        if (sq < bestSq) {
            bestSq = sq;
            best.point = q;
            best.triangle = i;
        }
    }
    best.distance = std::sqrt(bestSq);
    return best;
}

double enclosedVolume(const TriangleMesh& mesh)
{
    const auto& v = mesh.vertices();
    double sum = 0.0;
    for (const auto& t : mesh.triangles())
        sum += v[t[0]].dot(v[t[1]].cross(v[t[2]]));
    return sum / 6.0;
}

std::size_t openEdgeCount(const TriangleMesh& mesh)
{
    // Weld vertices with identical coordinates first: tessellated B-rep shapes have
    // separate vertices per face, which would otherwise make every face boundary "open".
    struct VecHash {
        std::size_t operator()(const Vec3& v) const noexcept
        {
            const std::hash<double> h;
            std::size_t seed = h(v.x);
            seed ^= h(v.y) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            seed ^= h(v.z) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
    std::unordered_map<Vec3, std::uint32_t, VecHash> canonical;
    std::vector<std::uint32_t> weld(mesh.vertexCount());
    canonical.reserve(mesh.vertexCount());
    for (std::size_t i = 0; i < mesh.vertexCount(); ++i)
        weld[i] = canonical.try_emplace(mesh.vertices()[i], static_cast<std::uint32_t>(canonical.size())).first->second;

    std::unordered_map<std::uint64_t, int> edges;
    edges.reserve(mesh.triangleCount() * 3);
    for (const auto& t : mesh.triangles()) {
        for (std::size_t i = 0; i < 3; ++i) {
            std::uint32_t a = weld[t[i]];
            std::uint32_t b = weld[t[(i + 1) % 3]];
            if (a == b)
                continue; // degenerate after welding
            if (a > b)
                std::swap(a, b);
            ++edges[(static_cast<std::uint64_t>(a) << 32) | b];
        }
    }
    return static_cast<std::size_t>(
        std::count_if(edges.begin(), edges.end(), [](const auto& e) { return e.second != 2; }));
}

} // namespace aerocad::core
