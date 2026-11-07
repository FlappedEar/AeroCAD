// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSimplify.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aerocad::core {

namespace {

struct TriHash {
    std::size_t operator()(const std::array<std::uint32_t, 3>& k) const noexcept
    {
        std::uint64_t h = k[0];
        h = h * 0x9E3779B97F4A7C15ULL ^ k[1];
        h = h * 0x9E3779B97F4A7C15ULL ^ k[2];
        return static_cast<std::size_t>(h ^ (h >> 29U));
    }
};

} // namespace

TriangleMesh clusterVertices(const TriangleMesh& mesh, double cellSize)
{
    if (mesh.isEmpty() || !(cellSize > 0.0))
        return mesh;
    const BoundingBox box = mesh.bounds();
    const Vec3 size = box.size();
    const double inv = 1.0 / cellSize;
    // 21 bits per axis; clamp the grid so huge ratios do not overflow the key.
    const auto dim = [&](double extent) {
        return static_cast<std::uint64_t>(std::min(2097151.0, std::floor(extent * inv) + 1.0));
    };
    const std::uint64_t nx = dim(size.x);
    const std::uint64_t ny = dim(size.y);
    const std::uint64_t nz = dim(size.z);
    auto cell = [&](const Vec3& p) {
        const auto ix = std::min<std::uint64_t>(nx - 1, static_cast<std::uint64_t>((p.x - box.min.x) * inv));
        const auto iy = std::min<std::uint64_t>(ny - 1, static_cast<std::uint64_t>((p.y - box.min.y) * inv));
        const auto iz = std::min<std::uint64_t>(nz - 1, static_cast<std::uint64_t>((p.z - box.min.z) * inv));
        return (ix << 42U) | (iy << 21U) | iz;
    };

    const auto& vertices = mesh.vertices();
    std::unordered_map<std::uint64_t, std::uint32_t> cellIndex;
    cellIndex.reserve(vertices.size() / 4 + 16);
    std::vector<std::uint32_t> remap(vertices.size());
    std::vector<Vec3> sums;
    std::vector<std::uint32_t> counts;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        auto [it, inserted] = cellIndex.try_emplace(cell(vertices[i]), static_cast<std::uint32_t>(sums.size()));
        if (inserted) {
            sums.push_back(vertices[i]);
            counts.push_back(1);
        } else {
            sums[it->second] = sums[it->second] + vertices[i];
            ++counts[it->second];
        }
        remap[i] = it->second;
    }

    std::vector<TriangleMesh::Triangle> triangles;
    triangles.reserve(mesh.triangleCount() / 2);
    std::unordered_set<std::array<std::uint32_t, 3>, TriHash> seen;
    seen.reserve(mesh.triangleCount() / 2);
    std::vector<char> used(sums.size(), 0);
    for (const auto& t : mesh.triangles()) {
        const TriangleMesh::Triangle r{remap[t[0]], remap[t[1]], remap[t[2]]};
        if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2])
            continue;
        std::array<std::uint32_t, 3> key = r;
        std::sort(key.begin(), key.end());
        if (!seen.insert(key).second)
            continue;
        triangles.push_back(r);
        used[r[0]] = used[r[1]] = used[r[2]] = 1;
    }

    // Compact: only clusters used by a surviving triangle.
    std::vector<std::uint32_t> compact(sums.size(), 0);
    std::vector<Vec3> out;
    out.reserve(sums.size());
    for (std::size_t c = 0; c < sums.size(); ++c) {
        if (used[c] != 0) {
            compact[c] = static_cast<std::uint32_t>(out.size());
            out.push_back(sums[c] * (1.0 / static_cast<double>(counts[c])));
        }
    }
    for (auto& t : triangles)
        for (auto& i : t)
            i = compact[i];
    return TriangleMesh(std::move(out), std::move(triangles));
}

TriangleMesh decimateToTarget(const TriangleMesh& mesh, std::size_t targetTriangles)
{
    if (mesh.triangleCount() <= targetTriangles || targetTriangles == 0)
        return mesh;
    // A surface of area A sampled on a grid of cell h has about A/h^2 vertices and
    // twice as many triangles.
    const double area = mesh.surfaceArea();
    if (!(area > 0.0))
        return mesh;
    double cell = std::sqrt(2.0 * area / static_cast<double>(targetTriangles));
    TriangleMesh result = clusterVertices(mesh, cell);
    for (int iteration = 0; iteration < 6 && result.triangleCount() > targetTriangles; ++iteration) {
        const double ratio = static_cast<double>(result.triangleCount()) / static_cast<double>(targetTriangles);
        cell *= std::sqrt(ratio) * 1.05;
        result = clusterVertices(mesh, cell);
    }
    return result;
}

} // namespace aerocad::core
