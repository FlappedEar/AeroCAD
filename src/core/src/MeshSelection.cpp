// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <unordered_map>

namespace aerocad::core {

namespace {

struct PositionHash {
    std::size_t operator()(const Vec3& v) const noexcept
    {
        const std::hash<double> h;
        std::size_t seed = h(v.x);
        seed ^= h(v.y) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        seed ^= h(v.z) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }
};

Vec3 centroid(const TriangleMesh& mesh, std::size_t t)
{
    const auto& tri = mesh.triangles()[t];
    const auto& v = mesh.vertices();
    return (v[tri[0]] + v[tri[1]] + v[tri[2]]) * (1.0 / 3.0);
}

double cosDeg(double deg)
{
    return std::cos(std::clamp(deg, 0.0, 180.0) * std::numbers::pi / 180.0);
}

std::vector<char> toMask(std::size_t count, const TriangleSet& selection)
{
    std::vector<char> mask(count, 0);
    for (const auto t : selection) {
        if (t < count)
            mask[t] = 1;
    }
    return mask;
}

TriangleSet fromMask(const std::vector<char>& mask)
{
    TriangleSet out;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] != 0)
            out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

bool insidePolygon(double x, double y, const std::vector<std::array<double, 2>>& poly)
{
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a[1] > y) != (b[1] > y)) {
            const double xCross = a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
            if (x < xCross)
                inside = !inside;
        }
    }
    return inside;
}

} // namespace

MeshAdjacency::MeshAdjacency(const TriangleMesh& mesh)
{
    const auto& vertices = mesh.vertices();
    const auto& triangles = mesh.triangles();

    // Canonical vertex id per position.
    std::vector<std::uint32_t> canonical(vertices.size());
    {
        std::unordered_map<Vec3, std::uint32_t, PositionHash> ids;
        ids.reserve(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
            canonical[i] = ids.try_emplace(vertices[i], static_cast<std::uint32_t>(ids.size())).first->second;
    }

    m_neighbours.assign(triangles.size(), {none, none, none});
    m_flipped.assign(triangles.size(), 0);
    // Edge (canonical a < b) -> first triangle * 3 + slot that has it, times 2, plus 1
    // when that triangle runs along the edge from b to a.
    std::unordered_map<std::uint64_t, std::uint64_t> open;
    open.reserve(triangles.size() * 2);
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        for (int slot = 0; slot < 3; ++slot) {
            std::uint32_t a = canonical[triangles[t][slot]];
            std::uint32_t b = canonical[triangles[t][(slot + 1) % 3]];
            if (a == b)
                continue;
            const std::uint64_t reversed = a > b ? 1 : 0;
            if (a > b)
                std::swap(a, b);
            const std::uint64_t key = (static_cast<std::uint64_t>(a) << 32) | b;
            const std::uint64_t here = (t * 3 + static_cast<std::uint64_t>(slot)) * 2 + reversed;
            auto [it, inserted] = open.try_emplace(key, here);
            if (inserted)
                continue;
            const std::uint64_t other = it->second;
            const auto otherTri = static_cast<std::uint32_t>(other / 6);
            const auto otherSlot = static_cast<int>(other / 2 % 3);
            if (m_neighbours[otherTri][static_cast<std::size_t>(otherSlot)] == none && otherTri != t) {
                m_neighbours[otherTri][static_cast<std::size_t>(otherSlot)] = static_cast<std::uint32_t>(t);
                m_neighbours[t][static_cast<std::size_t>(slot)] = otherTri;
                // Consistent winding traverses a shared edge in opposite directions.
                if ((other & 1u) == reversed) {
                    m_flipped[otherTri] = static_cast<std::uint8_t>(m_flipped[otherTri] | 1u << otherSlot);
                    m_flipped[t] = static_cast<std::uint8_t>(m_flipped[t] | 1u << slot);
                }
            }
        }
    }
}

std::shared_ptr<const MeshAdjacency> adjacencyFor(const std::shared_ptr<const TriangleMesh>& mesh)
{
    struct Entry {
        std::weak_ptr<const TriangleMesh> mesh;
        std::shared_ptr<const MeshAdjacency> adjacency;
    };
    static std::vector<Entry> cache; // most recently used first
    if (!mesh)
        return {};
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (it->mesh.lock() == mesh) {
            Entry hit = *it;
            cache.erase(it);
            cache.insert(cache.begin(), hit);
            return hit.adjacency;
        }
    }
    auto adjacency = std::make_shared<const MeshAdjacency>(*mesh);
    cache.insert(cache.begin(), Entry{mesh, adjacency});
    constexpr std::size_t maxEntries = 4;
    if (cache.size() > maxEntries)
        cache.resize(maxEntries);
    return adjacency;
}

std::optional<RayHit> raycast(const TriangleMesh& mesh, const Vec3& origin, const Vec3& direction)
{
    std::optional<RayHit> best;
    const auto& v = mesh.vertices();
    const double eps = 1e-12;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        const Vec3& a = v[tri[0]];
        const Vec3 e1 = v[tri[1]] - a;
        const Vec3 e2 = v[tri[2]] - a;
        const Vec3 pvec = direction.cross(e2);
        const double det = e1.dot(pvec);
        if (std::abs(det) < eps)
            continue;
        const double inv = 1.0 / det;
        const Vec3 tvec = origin - a;
        const double u = tvec.dot(pvec) * inv;
        if (u < 0.0 || u > 1.0)
            continue;
        const Vec3 qvec = tvec.cross(e1);
        const double w = direction.dot(qvec) * inv;
        if (w < 0.0 || u + w > 1.0)
            continue;
        const double dist = e2.dot(qvec) * inv;
        if (dist < 0.0 || (best && dist >= best->distance))
            continue;
        best = RayHit{static_cast<std::uint32_t>(t), dist, origin + direction * dist};
    }
    return best;
}

std::vector<Vec3> sliceMesh(const TriangleMesh& mesh, const Vec3& origin, const Vec3& normal, double radius)
{
    std::vector<Vec3> out;
    const double len = normal.length();
    if (!(len > 0.0))
        return out;
    const Vec3 n = normal * (1.0 / len);
    const auto& v = mesh.vertices();
    for (const auto& tri : mesh.triangles()) {
        const Vec3* p[3] = {&v[tri[0]], &v[tri[1]], &v[tri[2]]};
        double d[3];
        int above = 0;
        for (int k = 0; k < 3; ++k) {
            d[k] = (*p[k] - origin).dot(n);
            above += d[k] > 0.0 ? 1 : 0;
        }
        if (above == 0 || above == 3)
            continue;
        if ((*p[0] - origin).length() > radius && (*p[1] - origin).length() > radius &&
            (*p[2] - origin).length() > radius)
            continue;
        // The two edges whose ends lie on opposite sides.
        int found = 0;
        for (int k = 0; k < 3 && found < 2; ++k) {
            const int j = (k + 1) % 3;
            if ((d[k] > 0.0) != (d[j] > 0.0)) {
                const double t = d[k] / (d[k] - d[j]);
                out.push_back(*p[k] + (*p[j] - *p[k]) * t);
                ++found;
            }
        }
        if (found == 1)
            out.pop_back();
    }
    return out;
}

MeshProjector::MeshProjector(const TriangleMesh& mesh, const Vec3& outward) : m_mesh(&mesh)
{
    const double len = outward.length();
    m_d = len > 0.0 ? outward * (1.0 / len) : Vec3{0, 0, 1};
    const Vec3 helper = std::abs(m_d.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    m_e1 = m_d.cross(helper).normalized();
    m_e2 = m_d.cross(m_e1);

    const auto& v = mesh.vertices();
    double u1 = -std::numeric_limits<double>::max(), v1 = u1;
    m_u0 = std::numeric_limits<double>::max();
    m_v0 = m_u0;
    std::vector<float> us(v.size()), vs(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double a = v[i].dot(m_e1), b = v[i].dot(m_e2);
        us[i] = static_cast<float>(a);
        vs[i] = static_cast<float>(b);
        m_u0 = std::min(m_u0, a);
        u1 = std::max(u1, a);
        m_v0 = std::min(m_v0, b);
        v1 = std::max(v1, b);
    }
    if (v.empty()) {
        m_start.assign(2, 0);
        return;
    }
    const double pad = 1e-6 + 1e-3 * std::max(u1 - m_u0, v1 - m_v0);
    m_u0 -= pad;
    m_v0 -= pad;
    u1 += pad;
    v1 += pad;
    // About two triangles per cell on a scan seen from the front.
    const double wide = std::max(u1 - m_u0, 1e-9), high = std::max(v1 - m_v0, 1e-9);
    const double cells = std::clamp(static_cast<double>(mesh.triangleCount()) / 2.0, 1.0, 4.0e6);
    const double side = std::sqrt(wide * high / cells);
    m_nu = std::clamp(static_cast<int>(wide / side), 1, 4096);
    m_nv = std::clamp(static_cast<int>(high / side), 1, 4096);
    m_su = wide / m_nu;
    m_sv = high / m_nv;

    // Two passes: count the entries of each cell, then fill them.
    m_start.assign(static_cast<std::size_t>(m_nu) * m_nv + 1, 0);
    auto forCells = [&](const TriangleMesh::Triangle& t, auto&& visit) {
        const float a0 = std::min({us[t[0]], us[t[1]], us[t[2]]}), a1 = std::max({us[t[0]], us[t[1]], us[t[2]]});
        const float b0 = std::min({vs[t[0]], vs[t[1]], vs[t[2]]}), b1 = std::max({vs[t[0]], vs[t[1]], vs[t[2]]});
        const int i0 = cellU(a0), i1 = cellU(a1), j0 = cellV(b0), j1 = cellV(b1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                visit(static_cast<std::size_t>(j) * m_nu + i);
    };
    for (const auto& t : mesh.triangles())
        forCells(t, [&](std::size_t c) { ++m_start[c + 1]; });
    for (std::size_t c = 1; c < m_start.size(); ++c)
        m_start[c] += m_start[c - 1];
    m_triangles.resize(m_start.back());
    std::vector<std::uint32_t> fill(m_start.begin(), m_start.end() - 1);
    const auto& tris = mesh.triangles();
    for (std::uint32_t t = 0; t < tris.size(); ++t)
        forCells(tris[t], [&](std::size_t c) { m_triangles[fill[c]++] = t; });
}

int MeshProjector::cellU(double u) const
{
    return std::clamp(static_cast<int>((u - m_u0) / m_su), 0, m_nu - 1);
}

int MeshProjector::cellV(double v) const
{
    return std::clamp(static_cast<int>((v - m_v0) / m_sv), 0, m_nv - 1);
}

namespace {

/// Moller-Trumbore on the whole line; signed distance along `d`.
std::optional<double> lineHit(const TriangleMesh& mesh, std::uint32_t t, const Vec3& origin, const Vec3& d)
{
    const auto& v = mesh.vertices();
    const auto& tri = mesh.triangles()[t];
    const Vec3& a = v[tri[0]];
    const Vec3 e1 = v[tri[1]] - a;
    const Vec3 e2 = v[tri[2]] - a;
    const Vec3 pvec = d.cross(e2);
    const double det = e1.dot(pvec);
    if (std::abs(det) < 1e-12)
        return std::nullopt;
    const double inv = 1.0 / det;
    const Vec3 tvec = origin - a;
    const double u = tvec.dot(pvec) * inv;
    if (u < 0.0 || u > 1.0)
        return std::nullopt;
    const Vec3 qvec = tvec.cross(e1);
    const double w = d.dot(qvec) * inv;
    if (w < 0.0 || u + w > 1.0)
        return std::nullopt;
    return e2.dot(qvec) * inv;
}

} // namespace

std::optional<Vec3> MeshProjector::drop(const Vec3& p) const
{
    const double a = p.dot(m_e1), b = p.dot(m_e2);
    if (a < m_u0 || b < m_v0 || a > m_u0 + m_su * m_nu || b > m_v0 + m_sv * m_nv)
        return std::nullopt;
    const std::size_t c = static_cast<std::size_t>(cellV(b)) * m_nu + cellU(a);
    // The farthest crossing along the outward direction is the first one from outside.
    std::optional<double> best;
    for (std::uint32_t k = m_start[c]; k < m_start[c + 1]; ++k) {
        const auto h = lineHit(*m_mesh, m_triangles[k], p, m_d);
        if (h && (!best || *h > *best))
            best = h;
    }
    if (!best)
        return std::nullopt;
    return p + m_d * *best;
}

std::optional<RayHit> MeshProjector::raycast(const Vec3& origin, const Vec3& direction) const
{
    std::optional<RayHit> best;
    auto visit = [&](int i, int j) {
        const std::size_t c = static_cast<std::size_t>(j) * m_nu + i;
        for (std::uint32_t k = m_start[c]; k < m_start[c + 1]; ++k) {
            const auto h = lineHit(*m_mesh, m_triangles[k], origin, direction);
            if (h && *h >= 0.0 && (!best || *h < best->distance))
                best = RayHit{m_triangles[k], *h, origin + direction * *h};
        }
    };
    // Walk the cells under the ray's shadow on the grid, in order along the ray.
    const double ou = origin.dot(m_e1), ov = origin.dot(m_e2);
    const double du = direction.dot(m_e1), dv = direction.dot(m_e2);
    const double u1 = m_u0 + m_su * m_nu, v1 = m_v0 + m_sv * m_nv;
    double t0 = 0.0, t1 = std::numeric_limits<double>::max();
    auto clip = [&](double o, double d, double lo, double hi) {
        if (std::abs(d) < 1e-14)
            return o >= lo && o <= hi;
        double a = (lo - o) / d, b = (hi - o) / d;
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        return t0 <= t1;
    };
    if (!clip(ou, du, m_u0, u1) || !clip(ov, dv, m_v0, v1))
        return best;
    int i = cellU(ou + du * t0), j = cellV(ov + dv * t0);
    const int stepI = du > 0.0 ? 1 : -1, stepJ = dv > 0.0 ? 1 : -1;
    const double dtI = std::abs(du) < 1e-14 ? std::numeric_limits<double>::max() : m_su / std::abs(du);
    const double dtJ = std::abs(dv) < 1e-14 ? std::numeric_limits<double>::max() : m_sv / std::abs(dv);
    double nextI = std::abs(du) < 1e-14 ? std::numeric_limits<double>::max()
                                        : (m_u0 + (i + (du > 0.0 ? 1 : 0)) * m_su - ou) / du;
    double nextJ = std::abs(dv) < 1e-14 ? std::numeric_limits<double>::max()
                                        : (m_v0 + (j + (dv > 0.0 ? 1 : 0)) * m_sv - ov) / dv;
    while (i >= 0 && j >= 0 && i < m_nu && j < m_nv) {
        visit(i, j);
        const double exit = std::min(nextI, nextJ);
        // Later cells are farther along the ray than a hit inside the ones walked so far.
        if ((best && best->distance <= exit) || exit > t1)
            break;
        if (nextI < nextJ) {
            nextI += dtI;
            i += stepI;
        } else {
            nextJ += dtJ;
            j += stepJ;
        }
    }
    return best;
}

std::vector<Vec3> projectOntoMesh(const TriangleMesh& mesh, const std::vector<Vec3>& points, const Vec3& direction,
                                  std::size_t* hits, std::vector<char>* missed)
{
    std::vector<Vec3> out = points;
    if (hits != nullptr)
        *hits = 0;
    if (missed != nullptr)
        missed->assign(points.size(), 1);
    const double len = direction.length();
    if (points.empty() || mesh.triangleCount() == 0 || !(len > 0.0))
        return out;
    const Vec3 d = direction * (1.0 / len);
    // Frame across the direction; the grid lives in it, over the points' footprint only.
    const Vec3 helper = std::abs(d.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 e1 = d.cross(helper).normalized();
    const Vec3 e2 = d.cross(e1);
    double u0 = std::numeric_limits<double>::max(), v0 = u0, u1 = -u0, v1 = -u0;
    for (const auto& p : points) {
        u0 = std::min(u0, p.dot(e1));
        u1 = std::max(u1, p.dot(e1));
        v0 = std::min(v0, p.dot(e2));
        v1 = std::max(v1, p.dot(e2));
    }
    const double pad = 1e-6 + 0.01 * std::max(u1 - u0, v1 - v0);
    u0 -= pad;
    v0 -= pad;
    u1 += pad;
    v1 += pad;
    const int n = 64;
    const double su = (u1 - u0) / n;
    const double sv = (v1 - v0) / n;
    auto cell = [&](double x, double lo, double step) {
        return std::clamp(static_cast<int>((x - lo) / step), 0, n - 1);
    };
    std::vector<std::vector<std::uint32_t>> grid(static_cast<std::size_t>(n * n));
    const auto& v = mesh.vertices();
    for (std::uint32_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        double a0 = std::numeric_limits<double>::max(), b0 = a0, a1 = -a0, b1 = -a0;
        for (const auto i : tri) {
            a0 = std::min(a0, v[i].dot(e1));
            a1 = std::max(a1, v[i].dot(e1));
            b0 = std::min(b0, v[i].dot(e2));
            b1 = std::max(b1, v[i].dot(e2));
        }
        if (a1 < u0 || a0 > u1 || b1 < v0 || b0 > v1)
            continue;
        for (int j = cell(b0, v0, sv); j <= cell(b1, v0, sv); ++j)
            for (int i = cell(a0, u0, su); i <= cell(a1, u0, su); ++i)
                grid[static_cast<std::size_t>(j * n + i)].push_back(t);
    }
    for (std::size_t k = 0; k < out.size(); ++k) {
        Vec3& p = out[k];
        const auto& candidates = grid[static_cast<std::size_t>(cell(p.dot(e2), v0, sv) * n + cell(p.dot(e1), u0, su))];
        std::optional<double> best;
        for (const auto t : candidates) {
            const auto h = lineHit(mesh, t, p, d);
            if (h && (!best || *h > *best))
                best = h;
        }
        if (best) {
            p = p + d * *best;
            if (hits != nullptr)
                ++*hits;
            if (missed != nullptr)
                (*missed)[k] = 0;
        }
    }
    return out;
}

double pointTriangleDistance(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
    // Closest point on triangle (Ericson, Real-Time Collision Detection, 5.1.5).
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const double d1 = ab.dot(ap);
    const double d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0)
        return ap.length();

    const Vec3 bp = p - b;
    const double d3 = ab.dot(bp);
    const double d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3)
        return bp.length();

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        return (p - (a + ab * v)).length();
    }

    const Vec3 cp = p - c;
    const double d5 = ab.dot(cp);
    const double d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6)
        return cp.length();

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        return (p - (a + ac * w)).length();
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return (p - (b + (c - b) * w)).length();
    }

    const double denom = va + vb + vc;
    if (denom == 0.0) // degenerate triangle
        return std::min({ap.length(), bp.length(), cp.length()});
    const double v = vb / denom;
    const double w = vc / denom;
    return (p - (a + ab * v + ac * w)).length();
}

std::optional<std::uint32_t> nearestTriangle(const TriangleMesh& mesh, const Vec3& point)
{
    const auto& v = mesh.vertices();
    std::optional<std::uint32_t> best;
    double bestDistance = std::numeric_limits<double>::max();
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const auto& tri = mesh.triangles()[t];
        const double d = pointTriangleDistance(point, v[tri[0]], v[tri[1]], v[tri[2]]);
        if (d < bestDistance) {
            bestDistance = d;
            best = static_cast<std::uint32_t>(t);
        }
    }
    return best;
}

TriangleSet selectSmoothRegion(const TriangleMesh& mesh, const MeshAdjacency& adjacency, std::uint32_t seed,
                               const SmoothRegionOptions& options)
{
    if (seed >= mesh.triangleCount() || adjacency.triangleCount() != mesh.triangleCount())
        return {};

    const double neighbourCos = cosDeg(options.maxNeighbourAngleDeg);
    // 180 degrees means no limit (cos(pi) would still reject rounding noise at exactly 180).
    const double seedCos = options.maxSeedAngleDeg >= 180.0 ? -2.0 : cosDeg(options.maxSeedAngleDeg);
    const Vec3 seedNormal = mesh.faceNormal(seed);

    // Normals are oriented consistently with the seed while flooding: a neighbour that runs
    // along the shared edge in the same direction is wound the other way and its normal is
    // turned around. So inconsistent winding in STL files does not stop the region, sharp
    // folds still count as bends, and angles to the seed above 90 degrees mean something.
    std::vector<char> visited(mesh.triangleCount(), 0);
    std::vector<Vec3> oriented(mesh.triangleCount());
    std::vector<char> flipped(mesh.triangleCount(), 0); // relative to the seed's winding
    std::vector<std::uint32_t> stack{seed};
    visited[seed] = 1;
    oriented[seed] = seedNormal;
    TriangleSet out;
    while (!stack.empty()) {
        const std::uint32_t t = stack.back();
        stack.pop_back();
        out.push_back(t);
        const Vec3 n = oriented[t];
        const bool tFlipped = flipped[t] != 0;
        for (int slot = 0; slot < 3; ++slot) {
            const std::uint32_t nb = adjacency.neighbours(t)[static_cast<std::size_t>(slot)];
            if (nb == MeshAdjacency::none || visited[nb] != 0)
                continue;
            const bool nbFlipped = tFlipped != adjacency.windingFlipped(t, slot);
            Vec3 nn = mesh.faceNormal(nb);
            if (nbFlipped)
                nn = nn * -1.0;
            if (n.dot(nn) < neighbourCos || seedNormal.dot(nn) < seedCos)
                continue;
            visited[nb] = 1;
            oriented[nb] = nn;
            flipped[nb] = nbFlipped ? 1 : 0;
            stack.push_back(nb);
        }
    }
    normalizeSelection(out);
    return out;
}

TriangleSet selectInSphere(const TriangleMesh& mesh, const Vec3& center, double radius, const MeshAdjacency* adjacency,
                           std::uint32_t seed)
{
    TriangleSet out;
    const double r2 = radius * radius;
    auto inside = [&](std::size_t t) {
        const Vec3 d = centroid(mesh, t) - center;
        return d.dot(d) <= r2;
    };

    if (adjacency == nullptr || seed >= mesh.triangleCount()) {
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            if (inside(t))
                out.push_back(static_cast<std::uint32_t>(t));
        }
        return out;
    }

    // The seed is always taken, even when the brush is smaller than the triangle.
    std::vector<char> visited(mesh.triangleCount(), 0);
    std::vector<std::uint32_t> stack{seed};
    visited[seed] = 1;
    while (!stack.empty()) {
        const std::uint32_t t = stack.back();
        stack.pop_back();
        out.push_back(t);
        for (const std::uint32_t nb : adjacency->neighbours(t)) {
            if (nb == MeshAdjacency::none || visited[nb] != 0)
                continue;
            visited[nb] = 1;
            if (inside(nb))
                stack.push_back(nb);
        }
    }
    normalizeSelection(out);
    return out;
}

TriangleSet selectBrushDab(const TriangleMesh& mesh, const MeshAdjacency* adjacency, const Vec3& point, double radius)
{
    if (adjacency == nullptr)
        return selectInSphere(mesh, point, radius);
    const auto seed = nearestTriangle(mesh, point);
    if (!seed)
        return {};
    return selectInSphere(mesh, point, radius, adjacency, *seed);
}

TriangleSet selectInLasso(const TriangleMesh& mesh, const ViewParams& view,
                          const std::vector<std::array<double, 2>>& polygon, const LassoOptions& options)
{
    if (polygon.size() < 3 || mesh.isEmpty())
        return {};

    const Vec3 f = view.forward.normalized();
    const Vec3 r = f.cross(view.up).normalized();
    const Vec3 u = r.cross(f);
    if (f.length() == 0.0 || r.length() == 0.0)
        return {};
    const double tanHalf = std::tan(std::clamp(view.fovYDeg, 1.0, 179.0) * std::numbers::pi / 360.0);
    const double aspect = view.aspect > 0.0 ? view.aspect : 1.0;
    const bool ortho = view.orthographic && view.height > 0.0;
    // An orthographic camera also sees what is behind its position.
    const double nearZ = ortho ? -std::numeric_limits<double>::max() : 1e-9;

    struct Projected {
        double x; // normalized viewport, 0..1 left to right
        double y; // normalized viewport, 0..1 top to bottom
        double z; // depth along the view direction
    };
    auto project = [&](const Vec3& p) {
        const Vec3 d = p - view.eye;
        const double z = d.dot(f);
        if (z <= nearZ)
            return Projected{0.0, 0.0, z};
        const double halfHeight = ortho ? view.height * 0.5 : z * tanHalf;
        const double ndcX = d.dot(r) / (halfHeight * aspect);
        const double ndcY = d.dot(u) / halfHeight;
        return Projected{(ndcX + 1.0) * 0.5, (1.0 - ndcY) * 0.5, z};
    };

    // Bounding box of the lasso, to skip most triangles quickly.
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    for (const auto& p : polygon) {
        minX = std::min(minX, p[0]);
        maxX = std::max(maxX, p[0]);
        minY = std::min(minY, p[1]);
        maxY = std::max(maxY, p[1]);
    }

    const int longSide = 1024;
    const int width = aspect >= 1.0 ? longSide : std::max(1, static_cast<int>(longSide * aspect));
    const int height = aspect >= 1.0 ? std::max(1, static_cast<int>(longSide / aspect)) : longSide;
    // Software depth buffer with the nearest triangle per pixel (like a GPU id buffer):
    // a triangle is visible when it is the nearest one at one or more pixels. Comparing
    // only depths would let a surface seen at a grazing angle hide itself.
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    constexpr std::uint32_t noTriangle = 0xFFFFFFFFu;
    std::vector<float> depth;
    std::vector<std::uint32_t> owner;
    std::vector<char> covered; // triangle covers at least one pixel centre
    const auto& vertices = mesh.vertices();
    if (options.visibleOnly) {
        std::vector<Projected> pv(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
            pv[i] = project(vertices[i]);
        depth.assign(pixelCount, std::numeric_limits<float>::max());
        owner.assign(pixelCount, noTriangle);
        covered.assign(mesh.triangleCount(), 0);
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const auto& tri = mesh.triangles()[t];
            const Projected& a = pv[tri[0]];
            const Projected& b = pv[tri[1]];
            const Projected& c = pv[tri[2]];
            if (a.z <= nearZ || b.z <= nearZ || c.z <= nearZ)
                continue; // crosses the camera plane; ignored as an occluder
            const double ax = a.x * width, ay = a.y * height;
            const double bx = b.x * width, by = b.y * height;
            const double cx = c.x * width, cy = c.y * height;
            const double area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (area == 0.0)
                continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
            const int x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
            const int y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
            for (int y = y0; y <= y1; ++y) {
                const double py = y + 0.5;
                for (int x = x0; x <= x1; ++x) {
                    const double px = x + 0.5;
                    const double w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const double w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const double w2 = 1.0 - w0 - w1;
                    // Small negative slack closes cracks between neighbouring triangles,
                    // which would otherwise let hidden surfaces show through.
                    constexpr double slack = -1e-7;
                    if (w0 < slack || w1 < slack || w2 < slack)
                        continue;
                    // Depth is linear in screen space for a parallel projection, 1/z for a
                    // perspective one.
                    const double z =
                        ortho ? w0 * a.z + w1 * b.z + w2 * c.z : 1.0 / (w0 / a.z + w1 / b.z + w2 / c.z);
                    covered[t] = 1;
                    const std::size_t i =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                    if (static_cast<float>(z) < depth[i]) {
                        depth[i] = static_cast<float>(z);
                        owner[i] = static_cast<std::uint32_t>(t);
                    }
                }
            }
        }
    }
    std::vector<char> ownsPixel;
    if (options.visibleOnly) {
        ownsPixel.assign(mesh.triangleCount(), 0);
        for (const auto o : owner) {
            if (o != noTriangle)
                ownsPixel[o] = 1;
        }
    }

    const double sizeTolerance = mesh.bounds().diagonal() * 1e-3;
    const double pixelAngle = 2.0 * tanHalf / height;
    TriangleSet out;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const Projected p = project(centroid(mesh, t));
        if (p.z <= nearZ || p.x < minX || p.x > maxX || p.y < minY || p.y > maxY)
            continue;
        if (!insidePolygon(p.x, p.y, polygon))
            continue;
        if (options.visibleOnly && ownsPixel[t] == 0) {
            if (covered[t] != 0)
                continue; // covers pixels, but something nearer is in front at all of them
            // Smaller than a pixel: compare its depth with the pixel under its centre.
            const int x = static_cast<int>(p.x * width);
            const int y = static_cast<int>(p.y * height);
            if (x >= 0 && x < width && y >= 0 && y < height) {
                const float nearest =
                    depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
                const double tolerance = sizeTolerance + 4.0 * (ortho ? view.height / height : p.z * pixelAngle);
                if (nearest < std::numeric_limits<float>::max() && p.z > nearest + tolerance)
                    continue;
            }
        }
        out.push_back(static_cast<std::uint32_t>(t));
    }
    return out;
}

std::vector<TriangleSet> segmentSmoothRegions(const TriangleMesh& mesh, const MeshAdjacency& adjacency,
                                              const TriangleSet& selection, double creaseAngleDeg, double minArea)
{
    const std::size_t count = mesh.triangleCount();
    if (adjacency.triangleCount() != count)
        return {};
    constexpr std::int64_t outside = -1, unlabelled = -2;
    std::vector<std::int64_t> label(count, selection.empty() ? unlabelled : outside);
    for (const auto t : selection)
        if (t < count)
            label[t] = unlabelled;

    // Flood fill per region, orienting normals by edge traversal as selectSmoothRegion does.
    const double creaseCos = cosDeg(creaseAngleDeg);
    std::vector<Vec3> oriented(count);
    std::vector<char> flipped(count, 0);
    std::vector<double> regionArea;
    std::vector<std::uint32_t> stack;
    for (std::uint32_t seed = 0; seed < count; ++seed) {
        if (label[seed] != unlabelled)
            continue;
        const auto id = static_cast<std::int64_t>(regionArea.size());
        regionArea.push_back(0.0);
        label[seed] = id;
        oriented[seed] = mesh.faceNormal(seed);
        stack.push_back(seed);
        while (!stack.empty()) {
            const std::uint32_t t = stack.back();
            stack.pop_back();
            const auto& tri = mesh.triangles()[t];
            const auto& v = mesh.vertices();
            regionArea.back() += 0.5 * (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]).length();
            for (int slot = 0; slot < 3; ++slot) {
                const std::uint32_t nb = adjacency.neighbours(t)[static_cast<std::size_t>(slot)];
                if (nb == MeshAdjacency::none || label[nb] != unlabelled)
                    continue;
                const bool nbFlipped = (flipped[t] != 0) != adjacency.windingFlipped(t, slot);
                Vec3 nn = mesh.faceNormal(nb);
                if (nbFlipped)
                    nn = nn * -1.0;
                if (oriented[t].dot(nn) < creaseCos)
                    continue;
                label[nb] = id;
                oriented[nb] = nn;
                flipped[nb] = nbFlipped ? 1 : 0;
                stack.push_back(nb);
            }
        }
    }

    // Merge small regions (noise, thin chamfers) into the neighbour sharing most edges,
    // smallest first; repeated while merges grow regions past the size limit of others.
    std::vector<std::int64_t> parent(regionArea.size());
    for (std::size_t r = 0; r < parent.size(); ++r)
        parent[r] = static_cast<std::int64_t>(r);
    auto find = [&](std::int64_t r) {
        while (parent[static_cast<std::size_t>(r)] != r)
            r = parent[static_cast<std::size_t>(r)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(r)])];
        return r;
    };
    for (bool merged = true; merged;) {
        merged = false;
        // Shared edge counts between current regions.
        std::map<std::pair<std::int64_t, std::int64_t>, int> shared;
        for (std::uint32_t t = 0; t < count; ++t) {
            if (label[t] < 0)
                continue;
            const auto a = find(label[t]);
            for (const auto nb : adjacency.neighbours(t)) {
                if (nb == MeshAdjacency::none || label[nb] < 0)
                    continue;
                const auto b = find(label[nb]);
                if (a != b)
                    ++shared[{a, b}];
            }
        }
        std::vector<std::int64_t> order;
        for (std::size_t r = 0; r < parent.size(); ++r)
            if (find(static_cast<std::int64_t>(r)) == static_cast<std::int64_t>(r) && regionArea[r] < minArea)
                order.push_back(static_cast<std::int64_t>(r));
        std::sort(order.begin(), order.end(), [&](auto x, auto y) {
            return regionArea[static_cast<std::size_t>(x)] < regionArea[static_cast<std::size_t>(y)];
        });
        for (const auto r : order) {
            if (find(r) != r)
                continue;
            std::int64_t best = -1;
            int bestCount = 0;
            for (auto it = shared.lower_bound({r, std::numeric_limits<std::int64_t>::min()});
                 it != shared.end() && it->first.first == r; ++it) {
                const auto b = find(it->first.second);
                if (b != r && it->second > bestCount) {
                    bestCount = it->second;
                    best = b;
                }
            }
            if (best < 0)
                continue;
            parent[static_cast<std::size_t>(r)] = best;
            regionArea[static_cast<std::size_t>(best)] += regionArea[static_cast<std::size_t>(r)];
            merged = true; // counts are per original region pair; find() maps them
        }
    }

    std::map<std::int64_t, TriangleSet> regions;
    for (std::uint32_t t = 0; t < count; ++t)
        if (label[t] >= 0)
            regions[find(label[t])].push_back(t);
    std::vector<TriangleSet> out;
    for (auto& [r, tris] : regions)
        out.push_back(std::move(tris));
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.size() > y.size(); });
    return out;
}

TriangleSet growSelection(const MeshAdjacency& adjacency, const TriangleSet& selection, int steps)
{
    std::vector<char> mask = toMask(adjacency.triangleCount(), selection);
    const bool grow = steps > 0;
    for (int s = 0; s < std::abs(steps); ++s) {
        std::vector<char> next = mask;
        for (std::size_t t = 0; t < mask.size(); ++t) {
            if (mask[t] == 0)
                continue;
            for (const std::uint32_t nb : adjacency.neighbours(static_cast<std::uint32_t>(t))) {
                if (grow) {
                    if (nb != MeshAdjacency::none)
                        next[nb] = 1;
                } else if (nb == MeshAdjacency::none || mask[nb] == 0) {
                    next[t] = 0; // on the selection border or an open mesh edge
                }
            }
        }
        mask = std::move(next);
    }
    return fromMask(mask);
}

TriangleSet invertSelection(std::size_t triangleCount, const TriangleSet& selection)
{
    std::vector<char> mask = toMask(triangleCount, selection);
    for (auto& m : mask)
        m = m != 0 ? 0 : 1;
    return fromMask(mask);
}

TriangleSet combineSelection(const TriangleSet& current, const TriangleSet& change, SelectionMode mode)
{
    TriangleSet out;
    switch (mode) {
    case SelectionMode::Replace:
        return change;
    case SelectionMode::Add:
        std::set_union(current.begin(), current.end(), change.begin(), change.end(), std::back_inserter(out));
        return out;
    case SelectionMode::Subtract:
        std::set_difference(current.begin(), current.end(), change.begin(), change.end(), std::back_inserter(out));
        return out;
    }
    return out;
}

void normalizeSelection(TriangleSet& selection)
{
    std::sort(selection.begin(), selection.end());
    selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
}

double selectionArea(const TriangleMesh& mesh, const TriangleSet& selection)
{
    const auto& v = mesh.vertices();
    double area = 0.0;
    for (const auto t : selection) {
        if (t >= mesh.triangleCount())
            continue;
        const auto& tri = mesh.triangles()[t];
        area += 0.5 * (v[tri[1]] - v[tri[0]]).cross(v[tri[2]] - v[tri[0]]).length();
    }
    return area;
}

} // namespace aerocad::core
