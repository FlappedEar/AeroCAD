// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshRepair.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aerocad::core {

namespace {

using Triangle = TriangleMesh::Triangle;

/// One directed triangle edge, sorted by its undirected key so that all uses of an
/// edge are adjacent.
struct EdgeUse {
    std::uint64_t key; ///< min << 32 | max
    std::uint32_t triangle;
    std::uint32_t from; ///< Directed: from -> to as traversed by the triangle.
    std::uint32_t to;
};

std::vector<EdgeUse> edgeUses(const std::vector<Triangle>& triangles)
{
    std::vector<EdgeUse> uses;
    uses.reserve(triangles.size() * 3);
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        const auto& tri = triangles[t];
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t a = tri[static_cast<std::size_t>(k)];
            const std::uint32_t b = tri[static_cast<std::size_t>((k + 1) % 3)];
            if (a == b)
                continue;
            const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32U) | std::max(a, b);
            uses.push_back({key, static_cast<std::uint32_t>(t), a, b});
        }
    }
    std::sort(uses.begin(), uses.end(), [](const EdgeUse& x, const EdgeUse& y) {
        return x.key != y.key ? x.key < y.key : x.triangle < y.triangle;
    });
    return uses;
}

/// Calls fn(begin, end) for every run of uses of the same undirected edge.
template <typename Fn> void forEachEdge(const std::vector<EdgeUse>& uses, Fn&& fn)
{
    std::size_t i = 0;
    while (i < uses.size()) {
        std::size_t j = i + 1;
        while (j < uses.size() && uses[j].key == uses[i].key)
            ++j;
        fn(i, j);
        i = j;
    }
}

bool isDegenerate(const std::vector<Vec3>& v, const Triangle& t, double areaEpsilon)
{
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2])
        return true;
    const Vec3 n = (v[t[1]] - v[t[0]]).cross(v[t[2]] - v[t[0]]);
    return n.length() <= areaEpsilon;
}

/// Relative area threshold: twice the area below which a triangle counts as degenerate.
double areaEpsilon(const std::vector<Vec3>& vertices)
{
    BoundingBox box;
    for (const auto& p : vertices)
        box.extend(p);
    const double d = box.diagonal();
    return d * d * 1e-14;
}

std::array<std::uint32_t, 3> sortedKey(const Triangle& t)
{
    std::array<std::uint32_t, 3> k = t;
    std::sort(k.begin(), k.end());
    return k;
}

/// Merges vertices within `distance` (grid hashing). Returns the remap table.
std::vector<std::uint32_t> mergeClose(const std::vector<Vec3>& vertices, double distance, std::size_t& merged)
{
    std::vector<std::uint32_t> remap(vertices.size());
    merged = 0;
    if (distance <= 0.0) {
        // Sort indices by position; equal runs collapse onto their first (lowest) index.
        std::vector<std::uint32_t> order(vertices.size());
        std::iota(order.begin(), order.end(), 0U);
        auto key = [&](std::uint32_t i) {
            const Vec3& p = vertices[i];
            return std::array<double, 3>{p.x + 0.0, p.y + 0.0, p.z + 0.0};
        };
        std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) {
            const auto kx = key(x);
            const auto ky = key(y);
            return kx != ky ? kx < ky : x < y;
        });
        std::size_t i = 0;
        while (i < order.size()) {
            std::size_t j = i + 1;
            while (j < order.size() && key(order[j]) == key(order[i]))
                ++j;
            for (std::size_t k = i; k < j; ++k)
                remap[order[k]] = order[i];
            merged += j - i - 1;
            i = j;
        }
        return remap;
    }

    auto cellOf = [distance](double c) { return static_cast<std::int64_t>(std::floor(c / distance)); };
    auto cellKey = [](std::int64_t x, std::int64_t y, std::int64_t z) {
        const auto ux = static_cast<std::uint64_t>(x) & 0x1FFFFFU;
        const auto uy = static_cast<std::uint64_t>(y) & 0x1FFFFFU;
        const auto uz = static_cast<std::uint64_t>(z) & 0x1FFFFFU;
        return (ux << 42U) | (uy << 21U) | uz;
    };
    std::unordered_multimap<std::uint64_t, std::uint32_t> grid; // representatives only
    grid.reserve(vertices.size());
    const double d2 = distance * distance;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& p = vertices[i];
        const std::int64_t cx = cellOf(p.x);
        const std::int64_t cy = cellOf(p.y);
        const std::int64_t cz = cellOf(p.z);
        std::uint32_t found = static_cast<std::uint32_t>(i);
        double best = d2;
        for (std::int64_t dx = -1; dx <= 1; ++dx) {
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    auto [b, e] = grid.equal_range(cellKey(cx + dx, cy + dy, cz + dz));
                    for (auto it = b; it != e; ++it) {
                        const Vec3 diff = vertices[it->second] - p;
                        const double dd = diff.dot(diff);
                        if (dd <= best) {
                            best = dd;
                            found = it->second;
                        }
                    }
                }
            }
        }
        remap[i] = found;
        if (found == i)
            grid.emplace(cellKey(cx, cy, cz), found);
        else
            ++merged;
    }
    return remap;
}

/// Makes winding consistent per component; flips closed components with negative
/// volume and open ones where most triangles had to be flipped.
std::size_t orient(const std::vector<Vec3>& vertices, std::vector<Triangle>& triangles)
{
    const std::vector<EdgeUse> uses = edgeUses(triangles);
    // Adjacency over manifold edges (at most three per triangle): neighbour triangle and
    // whether both traverse the edge in the same direction (then one must be flipped).
    constexpr std::uint32_t kNone = UINT32_MAX;
    struct Links {
        std::array<std::uint32_t, 3> other{kNone, kNone, kNone};
        std::array<bool, 3> sameDirection{false, false, false};
        int count = 0;
    };
    std::vector<Links> links(triangles.size());
    std::vector<char> hasBoundary(triangles.size(), 0);
    auto link = [&](std::uint32_t t, std::uint32_t other, bool same) {
        Links& l = links[t];
        if (l.count < 3) {
            l.other[static_cast<std::size_t>(l.count)] = other;
            l.sameDirection[static_cast<std::size_t>(l.count)] = same;
            ++l.count;
        }
    };
    forEachEdge(uses, [&](std::size_t b, std::size_t e) {
        if (e - b == 2) {
            const EdgeUse& u = uses[b];
            const EdgeUse& w = uses[b + 1];
            const bool same = u.from == w.from;
            link(u.triangle, w.triangle, same);
            link(w.triangle, u.triangle, same);
        } else {
            for (std::size_t i = b; i < e; ++i)
                hasBoundary[uses[i].triangle] = 1;
        }
    });

    std::vector<int> state(triangles.size(), -1); // -1 unvisited, 0 keep, 1 flip
    std::size_t flipped = 0;
    std::vector<std::uint32_t> component;
    std::deque<std::uint32_t> queue;
    for (std::size_t seed = 0; seed < triangles.size(); ++seed) {
        if (state[seed] != -1)
            continue;
        component.clear();
        state[seed] = 0;
        queue.push_back(static_cast<std::uint32_t>(seed));
        bool open = false;
        while (!queue.empty()) {
            const std::uint32_t t = queue.front();
            queue.pop_front();
            component.push_back(t);
            open = open || hasBoundary[t] != 0;
            const Links& l = links[t];
            for (int k = 0; k < l.count; ++k) {
                const std::uint32_t o = l.other[static_cast<std::size_t>(k)];
                if (state[o] != -1)
                    continue;
                state[o] = l.sameDirection[static_cast<std::size_t>(k)] ? 1 - state[t] : state[t];
                queue.push_back(o);
            }
        }
        std::size_t flips = 0;
        for (const auto t : component)
            flips += static_cast<std::size_t>(state[t]);
        bool invert = false;
        if (!open) {
            double volume = 0.0;
            for (const auto t : component) {
                Triangle tri = triangles[t];
                if (state[t] == 1)
                    std::swap(tri[1], tri[2]);
                volume += vertices[tri[0]].dot(vertices[tri[1]].cross(vertices[tri[2]]));
            }
            invert = volume < 0.0;
        } else {
            invert = flips * 2 > component.size();
        }
        for (const auto t : component) {
            const bool flip = (state[t] == 1) != invert;
            if (flip) {
                std::swap(triangles[t][1], triangles[t][2]);
                ++flipped;
            }
        }
    }
    return flipped;
}

/// Boundary loops in hole-filling order: each loop runs opposite to the triangle edge
/// that borders the hole at its first edge. Loops are traced over undirected boundary
/// edges, so flipped faces along a rim do not break a hole into pieces.
std::vector<std::vector<std::uint32_t>> boundaryLoops(const std::vector<EdgeUse>& uses)
{
    struct BoundaryEdge {
        std::uint32_t from;
        std::uint32_t to;
        bool used = false;
    };
    std::vector<BoundaryEdge> edges;
    forEachEdge(uses, [&](std::size_t b, std::size_t e) {
        if (e - b == 1)
            edges.push_back({uses[b].from, uses[b].to});
    });
    std::unordered_multimap<std::uint32_t, std::uint32_t> incident; // vertex -> edge index
    incident.reserve(edges.size() * 2);
    for (std::uint32_t i = 0; i < edges.size(); ++i) {
        incident.emplace(edges[i].from, i);
        incident.emplace(edges[i].to, i);
    }
    auto takeEdgeAt = [&](std::uint32_t vertex) -> std::optional<std::uint32_t> {
        auto [first, last] = incident.equal_range(vertex);
        for (auto it = first; it != last; ++it) {
            if (!edges[it->second].used)
                return it->second;
        }
        return std::nullopt;
    };

    std::vector<std::vector<std::uint32_t>> loops;
    for (std::uint32_t start = 0; start < edges.size(); ++start) {
        if (edges[start].used)
            continue;
        edges[start].used = true;
        // Walk from `to` around the hole back to `from`; the loop then closes with the
        // triangle's own edge from -> to, so it is reversed below to run against it.
        const std::uint32_t first = edges[start].to;
        const std::uint32_t goal = edges[start].from;
        std::vector<std::uint32_t> loop{first};
        std::uint32_t current = first;
        bool closed = false;
        while (true) {
            // Prefer closing the loop as soon as the start edge's other end is reached.
            if (current == goal) {
                closed = true;
                break;
            }
            const auto e = takeEdgeAt(current);
            if (!e)
                break;
            edges[*e].used = true;
            current = edges[*e].from == current ? edges[*e].to : edges[*e].from;
            if (current != goal)
                loop.push_back(current);
        }
        if (closed) {
            loop.push_back(goal);
            std::reverse(loop.begin(), loop.end());
            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
    }
    return loops;
}

/// Triangulates a hole loop by ear clipping in the loop's best-fit plane. Triangles
/// follow the loop order (so they continue the surrounding winding). Returns false when
/// the loop does not project to a simple polygon (no valid ear found).
bool triangulateLoop(const std::vector<Vec3>& vertices, const std::vector<std::uint32_t>& loop,
                     std::vector<Triangle>& out)
{
    const std::size_t n = loop.size();
    if (n == 3) {
        out.push_back({loop[0], loop[1], loop[2]});
        return true;
    }
    // Newell normal: orientation of the loop as traversed.
    Vec3 normal;
    Vec3 centroid;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3& p = vertices[loop[i]];
        const Vec3& q = vertices[loop[(i + 1) % n]];
        normal = normal + Vec3{(p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)};
        centroid = centroid + p;
    }
    normal = normal.normalized();
    if (normal == Vec3{})
        return false;
    centroid = centroid * (1.0 / static_cast<double>(n));
    const Vec3 u = (std::abs(normal.x) < 0.9 ? normal.cross(Vec3{1, 0, 0}) : normal.cross(Vec3{0, 1, 0})).normalized();
    const Vec3 v = normal.cross(u);
    struct P2 {
        double x;
        double y;
    };
    std::vector<P2> pts(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 d = vertices[loop[i]] - centroid;
        pts[i] = {d.dot(u), d.dot(v)};
    }
    // With u, v, normal right-handed the loop is counter-clockwise in (u, v).
    auto cross = [](const P2& a, const P2& b, const P2& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    std::vector<std::size_t> ring(n);
    std::iota(ring.begin(), ring.end(), std::size_t{0});
    std::vector<Triangle> result;
    result.reserve(n - 2);
    while (ring.size() > 3) {
        bool clipped = false;
        const std::size_t m = ring.size();
        for (std::size_t k = 0; k < m; ++k) {
            const std::size_t ia = ring[(k + m - 1) % m];
            const std::size_t ib = ring[k];
            const std::size_t ic = ring[(k + 1) % m];
            const P2& a = pts[ia];
            const P2& b = pts[ib];
            const P2& c = pts[ic];
            if (cross(a, b, c) <= 0.0)
                continue; // reflex or degenerate corner
            bool inside = false;
            for (const std::size_t j : ring) {
                if (j == ia || j == ib || j == ic)
                    continue;
                const P2& p = pts[j];
                if (cross(a, b, p) >= 0.0 && cross(b, c, p) >= 0.0 && cross(c, a, p) >= 0.0) {
                    inside = true;
                    break;
                }
            }
            if (inside)
                continue;
            result.push_back({loop[ia], loop[ib], loop[ic]});
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
            break;
        }
        if (!clipped)
            return false;
    }
    result.push_back({loop[ring[0]], loop[ring[1]], loop[ring[2]]});
    out.insert(out.end(), result.begin(), result.end());
    return true;
}

} // namespace

MeshStats meshStats(const TriangleMesh& mesh)
{
    MeshStats s;
    s.vertices = mesh.vertexCount();
    s.triangles = mesh.triangleCount();
    const auto& tris = mesh.triangles();
    const auto& verts = mesh.vertices();
    const double eps = areaEpsilon(verts);

    std::vector<char> used(verts.size(), 0);
    std::vector<std::array<std::uint32_t, 3>> keys;
    keys.reserve(tris.size());
    for (const auto& t : tris) {
        for (const auto i : t)
            used[i] = 1;
        if (isDegenerate(verts, t, eps))
            ++s.degenerateTriangles;
        keys.push_back(sortedKey(t));
    }
    std::sort(keys.begin(), keys.end());
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i] == keys[i - 1])
            ++s.duplicateTriangles;
    }
    s.unusedVertices = static_cast<std::size_t>(std::count(used.begin(), used.end(), 0));

    const std::vector<EdgeUse> uses = edgeUses(tris);
    // Union-find over triangles for components.
    std::vector<std::uint32_t> parent(tris.size());
    std::iota(parent.begin(), parent.end(), 0U);
    auto find = [&](std::uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    forEachEdge(uses, [&](std::size_t b, std::size_t e) {
        const std::size_t n = e - b;
        if (n == 1)
            ++s.boundaryEdges;
        else if (n == 2 && uses[b].from == uses[b + 1].from)
            ++s.inconsistentEdges;
        else if (n > 2)
            ++s.nonManifoldEdges;
        for (std::size_t i = b + 1; i < e; ++i) {
            const auto ra = find(uses[b].triangle);
            const auto rb = find(uses[i].triangle);
            if (ra != rb)
                parent[ra] = rb;
        }
    });
    for (std::uint32_t t = 0; t < parent.size(); ++t) {
        if (find(t) == t)
            ++s.components;
    }
    const auto loops = boundaryLoops(uses);
    s.holes = loops.size();
    for (const auto& loop : loops)
        s.largestBoundaryEdges = std::max(s.largestBoundaryEdges, loop.size());
    return s;
}

TriangleMesh repairMesh(const TriangleMesh& mesh, const RepairOptions& options, RepairReport* reportOut)
{
    RepairReport report;
    std::vector<Vec3> vertices = mesh.vertices();
    std::vector<Triangle> triangles = mesh.triangles();

    // 1. Merge coincident / close vertices.
    {
        std::size_t merged = 0;
        const std::vector<std::uint32_t> remap = mergeClose(vertices, options.mergeDistance, merged);
        report.mergedVertices = merged;
        if (merged > 0) {
            for (auto& t : triangles)
                for (auto& i : t)
                    i = remap[i];
        }
    }

    // 2. Degenerate and 3. duplicate triangles (first occurrence is kept).
    {
        const double eps = areaEpsilon(vertices);
        std::vector<char> drop(triangles.size(), 0);
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            const auto& t = triangles[i];
            // Triangles with repeated indices are never valid and always dropped.
            const bool collapsed = t[0] == t[1] || t[1] == t[2] || t[0] == t[2];
            if (collapsed || (options.removeDegenerate && isDegenerate(vertices, t, eps))) {
                drop[i] = 1;
                ++report.removedDegenerate;
            }
        }
        if (options.removeDuplicates) {
            std::vector<std::pair<std::array<std::uint32_t, 3>, std::uint32_t>> keys;
            keys.reserve(triangles.size());
            for (std::size_t i = 0; i < triangles.size(); ++i) {
                if (drop[i] == 0)
                    keys.emplace_back(sortedKey(triangles[i]), static_cast<std::uint32_t>(i));
            }
            std::sort(keys.begin(), keys.end());
            for (std::size_t i = 1; i < keys.size(); ++i) {
                if (keys[i].first == keys[i - 1].first) {
                    drop[keys[i].second] = 1;
                    ++report.removedDuplicates;
                }
            }
        }
        std::size_t out = 0;
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            if (drop[i] == 0)
                triangles[out++] = triangles[i];
        }
        triangles.resize(out);
    }

    // 4. Orientation.
    if (options.fixOrientation)
        report.flippedTriangles = orient(vertices, triangles);

    // 5. Hole filling: ear clipping in the plane of each hole. Holes that do not project
    // to a simple polygon are left open (counted in skippedHoles).
    if (options.maxHoleEdges >= 3) {
        for (const auto& loop : boundaryLoops(edgeUses(triangles))) {
            if (loop.size() > options.maxHoleEdges) {
                ++report.largeBoundaries;
                continue;
            }
            const std::size_t before = triangles.size();
            if (triangulateLoop(vertices, loop, triangles)) {
                ++report.filledHoles;
                report.addedTriangles += triangles.size() - before;
            } else {
                ++report.skippedHoles;
            }
        }
        // A shell closed by filling must face outwards as well.
        if (options.fixOrientation && report.filledHoles > 0)
            report.flippedTriangles += orient(vertices, triangles);
    }

    // 6. Unused vertices (always compacted after merging so the index layout is clean).
    if (options.removeUnusedVertices) {
        std::vector<std::uint32_t> remap(vertices.size(), UINT32_MAX);
        std::vector<Vec3> compact;
        compact.reserve(vertices.size());
        for (auto& t : triangles) {
            for (auto& i : t) {
                if (remap[i] == UINT32_MAX) {
                    remap[i] = static_cast<std::uint32_t>(compact.size());
                    compact.push_back(vertices[i]);
                }
                i = remap[i];
            }
        }
        report.removedUnusedVertices = vertices.size() - compact.size() - report.mergedVertices;
        vertices = std::move(compact);
    }

    if (reportOut != nullptr)
        *reportOut = report;
    return TriangleMesh(std::move(vertices), std::move(triangles));
}

void flipNormals(TriangleMesh& mesh)
{
    std::vector<Triangle> triangles = mesh.triangles();
    for (auto& t : triangles)
        std::swap(t[1], t[2]);
    mesh = TriangleMesh(mesh.vertices(), std::move(triangles));
}

} // namespace aerocad::core
