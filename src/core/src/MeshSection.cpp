// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSection.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace aerocad::core {

namespace {

struct PairHash {
    std::size_t operator()(const std::pair<std::uint32_t, std::uint32_t>& p) const noexcept
    {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(p.first) << 32) | p.second);
    }
};

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

bool lexLess(const Vec3& a, const Vec3& b)
{
    if (a.x != b.x)
        return a.x < b.x;
    if (a.y != b.y)
        return a.y < b.y;
    return a.z < b.z;
}

/// Point where the edge (a, b) crosses the plane. Computed from the lexicographically
/// smaller end so that the same geometric edge always gives bit-identical points, even
/// when two faces store their own copies of the vertices (tessellated B-rep).
Vec3 crossing(const Vec3& a, double da, const Vec3& b, double db)
{
    if (lexLess(b, a)) {
        const double t = db / (db - da);
        return b + (a - b) * t;
    }
    const double t = da / (da - db);
    return a + (b - a) * t;
}

double signedDistance(const CutPlane& plane, const Vec3& n, const Vec3& p)
{
    return (p - plane.origin).dot(n);
}

} // namespace

double Polyline::length() const
{
    double len = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i)
        len += (points[i] - points[i - 1]).length();
    if (closed && points.size() > 2)
        len += (points.front() - points.back()).length();
    return len;
}

TriangleMesh clipMesh(const TriangleMesh& mesh, const CutPlane& plane)
{
    const Vec3 n = plane.normal.normalized();
    const auto& verts = mesh.vertices();
    std::vector<double> dist(verts.size());
    for (std::size_t i = 0; i < verts.size(); ++i)
        dist[i] = signedDistance(plane, n, verts[i]);

    TriangleMesh out;
    // Original vertex index -> output index (only for kept vertices).
    std::vector<std::uint32_t> remap(verts.size(), UINT32_MAX);
    std::unordered_map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t, PairHash> cuts;

    auto keep = [&](std::uint32_t v) {
        if (remap[v] == UINT32_MAX)
            remap[v] = out.addVertex(verts[v]);
        return remap[v];
    };
    // Output vertex where edge (a below/on, b above) meets the plane.
    auto cut = [&](std::uint32_t a, std::uint32_t b) {
        if (dist[a] == 0.0)
            return keep(a);
        const auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
        const auto it = cuts.find(key);
        if (it != cuts.end())
            return it->second;
        const std::uint32_t idx = out.addVertex(crossing(verts[a], dist[a], verts[b], dist[b]));
        cuts.emplace(key, idx);
        return idx;
    };
    auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (a != b && b != c && a != c)
            out.addTriangle(a, b, c);
    };

    for (const auto& t : mesh.triangles()) {
        const std::array<bool, 3> above{dist[t[0]] > 0.0, dist[t[1]] > 0.0, dist[t[2]] > 0.0};
        const int count = int(above[0]) + int(above[1]) + int(above[2]);
        if (count == 3)
            continue;
        if (count == 0) {
            emit(keep(t[0]), keep(t[1]), keep(t[2]));
            continue;
        }
        // Rotate so that the corners keep their winding order.
        if (count == 1) {
            int c = above[0] ? 0 : above[1] ? 1 : 2; // the removed corner
            const std::uint32_t vc = t[static_cast<std::size_t>(c)];
            const std::uint32_t va = t[static_cast<std::size_t>((c + 1) % 3)];
            const std::uint32_t vb = t[static_cast<std::size_t>((c + 2) % 3)];
            // Kept quad: va, vb, cut(vb, vc), cut(va, vc).
            const std::uint32_t a = keep(va);
            const std::uint32_t b = keep(vb);
            const std::uint32_t pb = cut(vb, vc);
            const std::uint32_t pa = cut(va, vc);
            emit(a, b, pb);
            emit(a, pb, pa);
        } else {
            int k = !above[0] ? 0 : !above[1] ? 1 : 2; // the kept corner
            const std::uint32_t vk = t[static_cast<std::size_t>(k)];
            const std::uint32_t v1 = t[static_cast<std::size_t>((k + 1) % 3)];
            const std::uint32_t v2 = t[static_cast<std::size_t>((k + 2) % 3)];
            emit(keep(vk), cut(vk, v1), cut(vk, v2));
        }
    }
    return out;
}

std::vector<Polyline> sectionCurves(const TriangleMesh& mesh, const CutPlane& plane)
{
    const Vec3 n = plane.normal.normalized();
    const auto& verts = mesh.vertices();
    std::vector<double> dist(verts.size());
    for (std::size_t i = 0; i < verts.size(); ++i)
        dist[i] = signedDistance(plane, n, verts[i]);

    // Graph of section points (welded by exact position) and segments between them.
    std::vector<Vec3> nodes;
    std::vector<std::vector<std::uint32_t>> adjacency;
    std::unordered_map<Vec3, std::uint32_t, VecHash> nodeIndex;
    auto node = [&](const Vec3& p) {
        const auto [it, inserted] = nodeIndex.emplace(p, static_cast<std::uint32_t>(nodes.size()));
        if (inserted) {
            nodes.push_back(p);
            adjacency.emplace_back();
        }
        return it->second;
    };
    auto point = [&](std::uint32_t a, std::uint32_t b) {
        // a not above, b above.
        if (dist[a] == 0.0)
            return verts[a];
        return crossing(verts[a], dist[a], verts[b], dist[b]);
    };

    for (const auto& t : mesh.triangles()) {
        std::array<std::uint32_t, 2> ends{};
        int found = 0;
        for (int e = 0; e < 3 && found < 2; ++e) {
            const std::uint32_t a = t[static_cast<std::size_t>(e)];
            const std::uint32_t b = t[static_cast<std::size_t>((e + 1) % 3)];
            const bool aboveA = dist[a] > 0.0;
            const bool aboveB = dist[b] > 0.0;
            if (aboveA == aboveB)
                continue;
            ends[static_cast<std::size_t>(found++)] = aboveA ? node(point(b, a)) : node(point(a, b));
        }
        if (found == 2 && ends[0] != ends[1]) {
            auto& adjA = adjacency[ends[0]];
            auto& adjB = adjacency[ends[1]];
            // Skip duplicate segments (e.g. an edge lying in the plane seen from both faces).
            bool duplicate = false;
            for (const auto x : adjA)
                duplicate = duplicate || x == ends[1];
            if (!duplicate) {
                adjA.push_back(ends[1]);
                adjB.push_back(ends[0]);
            }
        }
    }

    // Walk the graph: open chains from their ends first, then the remaining cycles.
    std::vector<std::vector<bool>> used(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i)
        used[i].assign(adjacency[i].size(), false);
    auto markUsed = [&](std::uint32_t from, std::uint32_t to) {
        for (std::size_t k = 0; k < adjacency[from].size(); ++k) {
            if (adjacency[from][k] == to && !used[from][k]) {
                used[from][k] = true;
                return;
            }
        }
    };
    auto nextUnused = [&](std::uint32_t at) -> std::int64_t {
        for (std::size_t k = 0; k < adjacency[at].size(); ++k)
            if (!used[at][k])
                return adjacency[at][k];
        return -1;
    };

    std::vector<Polyline> curves;
    auto walk = [&](std::uint32_t start) {
        Polyline line;
        line.points.push_back(nodes[start]);
        std::uint32_t at = start;
        for (;;) {
            const std::int64_t next = nextUnused(at);
            if (next < 0)
                break;
            const auto nx = static_cast<std::uint32_t>(next);
            markUsed(at, nx);
            markUsed(nx, at);
            at = nx;
            if (at == start) {
                line.closed = true;
                break;
            }
            line.points.push_back(nodes[at]);
        }
        if (line.points.size() >= 2)
            curves.push_back(std::move(line));
    };

    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (adjacency[i].size() % 2 == 1)
            while (nextUnused(i) >= 0)
                walk(i);
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        while (nextUnused(i) >= 0)
            walk(i);
    return curves;
}

void planeAxes(const Vec3& normal, Vec3& u, Vec3& v)
{
    const Vec3 n = normal.normalized();
    const Vec3 ref = std::abs(n.z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    u = ref.cross(n).normalized();
    v = n.cross(u);
}

namespace {

struct P2 {
    double x;
    double y;
};

std::vector<P2> project(const Polyline& line, const CutPlane& plane, const Vec3& u, const Vec3& v)
{
    std::vector<P2> out;
    out.reserve(line.points.size());
    for (const Vec3& p : line.points) {
        const Vec3 d = p - plane.origin;
        out.push_back({d.dot(u), d.dot(v)});
    }
    return out;
}

double polygonArea(const std::vector<P2>& poly)
{
    double a = 0.0;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        a += poly[j].x * poly[i].y - poly[i].x * poly[j].y;
    return a * 0.5;
}

bool inside(const std::vector<P2>& poly, const P2& p)
{
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if ((poly[i].y > p.y) != (poly[j].y > p.y)) {
            const double x = poly[j].x + (p.y - poly[j].y) * (poly[i].x - poly[j].x) / (poly[i].y - poly[j].y);
            if (p.x < x)
                in = !in;
        }
    }
    return in;
}

} // namespace

std::vector<std::ptrdiff_t> nestingParents(const std::vector<Polyline>& curves, const CutPlane& plane)
{
    Vec3 u;
    Vec3 v;
    planeAxes(plane.normal, u, v);
    std::vector<std::vector<P2>> polys(curves.size());
    std::vector<double> areas(curves.size(), 0.0);
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (curves[i].closed && curves[i].points.size() >= 3) {
            polys[i] = project(curves[i], plane, u, v);
            areas[i] = std::abs(polygonArea(polys[i]));
        }
    }

    std::vector<std::ptrdiff_t> parent(curves.size(), -1);
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (polys[i].empty())
            continue;
        // Test a point between the first two vertices: vertices may be shared by touching loops.
        const P2 probe{(polys[i][0].x + polys[i][1].x) / 2, (polys[i][0].y + polys[i][1].y) / 2};
        for (std::size_t j = 0; j < curves.size(); ++j) {
            if (j == i || polys[j].empty() || !inside(polys[j], probe))
                continue;
            // The innermost container is the smallest one.
            if (parent[i] < 0 || areas[j] < areas[static_cast<std::size_t>(parent[i])])
                parent[i] = static_cast<std::ptrdiff_t>(j);
        }
    }
    return parent;
}

std::vector<std::size_t> nestingDepths(const std::vector<Polyline>& curves, const CutPlane& plane)
{
    const auto parent = nestingParents(curves, plane);
    std::vector<std::size_t> depth(curves.size(), 0);
    for (std::size_t i = 0; i < curves.size(); ++i) {
        std::size_t d = 0;
        // Bounded walk: containment is acyclic for proper loops, but guard against bad input.
        for (std::ptrdiff_t p = parent[i]; p >= 0 && d <= curves.size(); p = parent[static_cast<std::size_t>(p)])
            ++d;
        depth[i] = d;
    }
    return depth;
}

double sectionArea(const std::vector<Polyline>& curves, const CutPlane& plane)
{
    Vec3 u;
    Vec3 v;
    planeAxes(plane.normal, u, v);
    const auto depth = nestingDepths(curves, plane);
    double area = 0.0;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        if (!curves[i].closed || curves[i].points.size() < 3)
            continue;
        const double a = std::abs(polygonArea(project(curves[i], plane, u, v)));
        area += depth[i] % 2 == 0 ? a : -a;
    }
    return area;
}

} // namespace aerocad::core
