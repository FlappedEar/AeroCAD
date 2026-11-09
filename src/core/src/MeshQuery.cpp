// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshQuery.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace aerocad::core {

namespace {

constexpr std::uint32_t kLeafSize = 4;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Closest point on triangle (a, b, c) to p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
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
    if (denom == 0.0)
        return a; // degenerate triangle
    const double v = vb / denom;
    const double w = vc / denom;
    return a + ab * v + ac * w;
}

double boxDistanceSquared(const BoundingBox& box, const Vec3& p)
{
    auto axis = [](double v, double lo, double hi) {
        if (v < lo)
            return lo - v;
        if (v > hi)
            return v - hi;
        return 0.0;
    };
    const double dx = axis(p.x, box.min.x, box.max.x);
    const double dy = axis(p.y, box.min.y, box.max.y);
    const double dz = axis(p.z, box.min.z, box.max.z);
    return dx * dx + dy * dy + dz * dz;
}

/// Slab test; returns the entry distance or +inf when the ray misses within maxT.
double rayBox(const BoundingBox& box, const Vec3& o, const Vec3& invDir, double maxT)
{
    double t0 = 0.0;
    double t1 = maxT;
    const std::array<double, 3> origin{o.x, o.y, o.z};
    const std::array<double, 3> inv{invDir.x, invDir.y, invDir.z};
    const std::array<double, 3> lo{box.min.x, box.min.y, box.min.z};
    const std::array<double, 3> hi{box.max.x, box.max.y, box.max.z};
    for (int i = 0; i < 3; ++i) {
        double ta = (lo[i] - origin[i]) * inv[i];
        double tb = (hi[i] - origin[i]) * inv[i];
        if (std::isnan(ta) || std::isnan(tb)) {
            // Ray parallel to the slab and exactly on its boundary (0 * inf).
            if (origin[i] < lo[i] || origin[i] > hi[i])
                return std::numeric_limits<double>::infinity();
            continue;
        }
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return std::numeric_limits<double>::infinity();
    }
    return t0;
}

/// Möller-Trumbore, both sides. Returns t or a negative value on a miss.
double rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 e1 = b - a;
    const Vec3 e2 = c - a;
    const Vec3 pv = d.cross(e2);
    const double det = e1.dot(pv);
    const double scale = e1.length() * e2.length();
    if (std::abs(det) <= 1e-14 * scale)
        return -1.0;
    const double inv = 1.0 / det;
    const Vec3 tv = o - a;
    const double u = tv.dot(pv) * inv;
    if (u < 0.0 || u > 1.0)
        return -1.0;
    const Vec3 qv = tv.cross(e1);
    const double v = d.dot(qv) * inv;
    if (v < 0.0 || u + v > 1.0)
        return -1.0;
    return e2.dot(qv) * inv;
}

/// Runs fn(i) for i in [0, n) on a few threads (deterministic results: each index is
/// written by exactly one thread).
template <typename Fn>
void parallelFor(std::size_t n, Fn&& fn)
{
    const std::size_t hw = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t threads = std::min<std::size_t>(hw, std::max<std::size_t>(1, n / 4096));
    if (threads <= 1) {
        for (std::size_t i = 0; i < n; ++i)
            fn(i);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads);
    const std::size_t chunk = (n + threads - 1) / threads;
    for (std::size_t t = 0; t < threads; ++t) {
        const std::size_t begin = t * chunk;
        const std::size_t end = std::min(n, begin + chunk);
        if (begin >= end)
            break;
        pool.emplace_back([begin, end, &fn] {
            for (std::size_t i = begin; i < end; ++i)
                fn(i);
        });
    }
    for (auto& th : pool)
        th.join();
}

} // namespace

MeshBvh::MeshBvh(std::shared_ptr<const TriangleMesh> mesh) : m_mesh(std::move(mesh))
{
    if (!m_mesh)
        m_mesh = std::make_shared<const TriangleMesh>();
    const auto& tris = m_mesh->triangles();
    const auto& verts = m_mesh->vertices();
    if (tris.empty())
        return;
    std::vector<Vec3> centroids(tris.size());
    m_order.resize(tris.size());
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const auto& t = tris[i];
        centroids[i] = (verts[t[0]] + verts[t[1]] + verts[t[2]]) * (1.0 / 3.0);
        m_order[i] = static_cast<std::uint32_t>(i);
    }
    m_nodes.reserve(2 * tris.size() / kLeafSize + 1);
    build(0, static_cast<std::uint32_t>(tris.size()), centroids);
}

std::uint32_t MeshBvh::build(std::uint32_t first, std::uint32_t count, std::vector<Vec3>& centroids)
{
    const auto& tris = m_mesh->triangles();
    const auto& verts = m_mesh->vertices();
    const auto index = static_cast<std::uint32_t>(m_nodes.size());
    m_nodes.emplace_back();

    BoundingBox box;
    BoundingBox centroidBox;
    for (std::uint32_t i = first; i < first + count; ++i) {
        for (const std::uint32_t v : tris[m_order[i]])
            box.extend(verts[v]);
        centroidBox.extend(centroids[m_order[i]]);
    }
    m_nodes[index].box = box;

    const Vec3 extent = centroidBox.size();
    if (count <= kLeafSize || extent.length() == 0.0) {
        m_nodes[index].first = first;
        m_nodes[index].count = count;
        return index;
    }

    // Median split along the longest axis of the centroids.
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    auto key = [&](std::uint32_t t) {
        const Vec3& c = centroids[t];
        return axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    };
    const std::uint32_t half = count / 2;
    std::nth_element(m_order.begin() + first, m_order.begin() + first + half, m_order.begin() + first + count,
                     [&](std::uint32_t l, std::uint32_t r) { return key(l) < key(r); });

    const std::uint32_t left = build(first, half, centroids);
    const std::uint32_t right = build(first + half, count - half, centroids);
    m_nodes[index].first = left;
    m_nodes[index].right = right;
    m_nodes[index].count = 0;
    return index;
}

std::optional<SurfacePoint> MeshBvh::closestPoint(const Vec3& p, double maxDistance) const
{
    if (m_nodes.empty())
        return std::nullopt;
    const auto& tris = m_mesh->triangles();
    const auto& verts = m_mesh->vertices();

    double best = maxDistance * maxDistance;
    std::optional<SurfacePoint> result;
    std::vector<std::uint32_t> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const Node& node = m_nodes[stack.back()];
        stack.pop_back();
        if (boxDistanceSquared(node.box, p) > best)
            continue;
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const auto& t = tris[m_order[i]];
                const Vec3 q = closestOnTriangle(p, verts[t[0]], verts[t[1]], verts[t[2]]);
                const Vec3 d = q - p;
                const double d2 = d.dot(d);
                if (d2 <= best) {
                    best = d2;
                    result = SurfacePoint{q, 0.0, m_order[i]};
                }
            }
            continue;
        }
        // Visit the nearer child first (pushed last).
        const double dl = boxDistanceSquared(m_nodes[node.first].box, p);
        const double dr = boxDistanceSquared(m_nodes[node.right].box, p);
        if (dl < dr) {
            stack.push_back(node.right);
            stack.push_back(node.first);
        } else {
            stack.push_back(node.first);
            stack.push_back(node.right);
        }
    }
    if (result)
        result->distance = std::sqrt(best);
    return result;
}

std::optional<MeshRayHit> MeshBvh::raycast(const Vec3& origin, const Vec3& direction, double maxT,
                                           bool includeOrigin) const
{
    const double len = direction.length();
    if (m_nodes.empty() || len == 0.0)
        return std::nullopt;
    const Vec3 d = direction * (1.0 / len);
    const Vec3 inv{1.0 / d.x, 1.0 / d.y, 1.0 / d.z};
    const auto& tris = m_mesh->triangles();
    const auto& verts = m_mesh->vertices();

    double best = maxT;
    std::optional<MeshRayHit> result;
    std::vector<std::uint32_t> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const Node& node = m_nodes[stack.back()];
        stack.pop_back();
        // rayBox returns +inf on a miss, which `> best` would not reject when best is +inf.
        const double entry = rayBox(node.box, origin, inv, best);
        if (!std::isfinite(entry) || entry > best)
            continue;
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const auto& t = tris[m_order[i]];
                const double hit = rayTriangle(origin, d, verts[t[0]], verts[t[1]], verts[t[2]]);
                if ((hit > 0.0 || (includeOrigin && hit == 0.0)) && hit <= best) {
                    best = hit;
                    result = MeshRayHit{hit, origin + d * hit, m_order[i]};
                }
            }
            continue;
        }
        stack.push_back(node.first);
        stack.push_back(node.right);
    }
    return result;
}

struct MeshPickCache::Build {
    std::atomic<bool> ready{false};
    std::mutex mutex;
    std::condition_variable finished;
    std::shared_ptr<const MeshBvh> bvh; ///< Set once, before `ready`.
};

MeshPickCache::Entry& MeshPickCache::entryFor(const std::shared_ptr<const TriangleMesh>& mesh)
{
    for (auto& e : m_entries)
        if (e.key == mesh.get())
            return e;
    auto build = std::make_shared<Build>();
    if (mesh->triangleCount() <= kSyncTriangles) {
        build->bvh = std::make_shared<const MeshBvh>(mesh);
        build->ready = true;
    } else {
        // The thread owns what it uses (the build state and the mesh), so dropping the
        // entry while it runs is safe and never waits for it.
        std::thread([build, mesh] {
            auto bvh = std::make_shared<const MeshBvh>(mesh);
            {
                const std::lock_guard lock(build->mutex);
                build->bvh = std::move(bvh);
                build->ready = true;
            }
            build->finished.notify_all();
        }).detach();
    }
    m_entries.push_back({mesh.get(), std::move(build)});
    return m_entries.back();
}

void MeshPickCache::prepare(const std::shared_ptr<const TriangleMesh>& mesh)
{
    if (mesh && !mesh->isEmpty())
        (void)entryFor(mesh);
}

std::shared_ptr<const MeshBvh> MeshPickCache::bvh(const std::shared_ptr<const TriangleMesh>& mesh)
{
    if (!mesh || mesh->isEmpty())
        return nullptr;
    Build& b = *entryFor(mesh).build;
    if (!b.ready.load(std::memory_order_acquire))
        return nullptr;
    const std::lock_guard lock(b.mutex);
    return b.bvh;
}

std::optional<RayHit> MeshPickCache::raycast(const std::shared_ptr<const TriangleMesh>& mesh, const Vec3& origin,
                                             const Vec3& direction)
{
    if (!mesh || mesh->isEmpty())
        return std::nullopt;
    const std::shared_ptr<const MeshBvh> index = bvh(mesh);
    if (!index)
        return core::raycast(*mesh, origin, direction);
    const double len = direction.length();
    if (!(len > 0.0))
        return std::nullopt;
    // t >= 0 as core::raycast: a section pick starts on the section plane, which can lie on
    // a face of the mesh.
    const auto hit = index->raycast(origin, direction, std::numeric_limits<double>::infinity(), true);
    if (!hit)
        return std::nullopt;
    return RayHit{static_cast<std::uint32_t>(hit->triangle), hit->t / len, hit->point};
}

void MeshPickCache::retain(const std::vector<const TriangleMesh*>& alive)
{
    std::erase_if(m_entries, [&alive](const Entry& e) {
        return std::find(alive.begin(), alive.end(), e.key) == alive.end();
    });
}

void MeshPickCache::waitForBuilds()
{
    for (const auto& e : m_entries) {
        std::unique_lock lock(e.build->mutex);
        e.build->finished.wait(lock, [&e] { return e.build->ready.load(); });
    }
}

std::optional<SurfaceDistance> minimumDistance(const MeshBvh& a, const MeshBvh& b)
{
    if (a.mesh().isEmpty() || b.mesh().isEmpty())
        return std::nullopt;
    std::optional<SurfaceDistance> best;
    auto scan = [&best](const MeshBvh& from, const MeshBvh& onto, bool swapped) {
        for (const Vec3& v : from.mesh().vertices()) {
            const double limit = best ? best->distance : std::numeric_limits<double>::infinity();
            const auto hit = onto.closestPoint(v, limit);
            if (hit && (!best || hit->distance < best->distance)) {
                best = swapped ? SurfaceDistance{hit->distance, hit->point, v}
                               : SurfaceDistance{hit->distance, v, hit->point};
            }
        }
    };
    scan(a, b, false);
    scan(b, a, true);
    return best;
}

std::vector<Vec3> vertexNormals(const TriangleMesh& mesh)
{
    const auto& verts = mesh.vertices();
    std::vector<Vec3> normals(verts.size());
    for (const auto& t : mesh.triangles()) {
        const Vec3 n = (verts[t[1]] - verts[t[0]]).cross(verts[t[2]] - verts[t[0]]);
        for (const std::uint32_t v : t)
            normals[v] = normals[v] + n;
    }
    for (auto& n : normals)
        n = n.normalized();
    return normals;
}

std::vector<double> signedVertexDistances(const TriangleMesh& measured, const MeshBvh& reference, double maxDistance)
{
    const auto& verts = measured.vertices();
    std::vector<double> out(verts.size(), kNaN);
    if (reference.mesh().isEmpty())
        return out;
    parallelFor(verts.size(), [&](std::size_t i) {
        const auto hit = reference.closestPoint(verts[i], maxDistance);
        if (!hit)
            return;
        double sign = 1.0;
        if (hit->distance > 0.0) {
            const Vec3 n = reference.mesh().faceNormal(hit->triangle);
            sign = (verts[i] - hit->point).dot(n) < 0.0 ? -1.0 : 1.0;
        }
        out[i] = sign * hit->distance;
    });
    return out;
}

std::vector<double> triangleThickness(const MeshBvh& bvh, double maxThickness)
{
    // Rays start at triangle centres along the inward face normal (vertex normals are
    // skewed at sharp edges).
    const TriangleMesh& mesh = bvh.mesh();
    const auto& verts = mesh.vertices();
    const auto& tris = mesh.triangles();
    const double eps = std::max(mesh.bounds().diagonal() * 1e-7, 1e-9);
    std::vector<double> perTriangle(tris.size(), kNaN);
    parallelFor(tris.size(), [&](std::size_t i) {
        const Vec3 inward = mesh.faceNormal(i) * -1.0;
        if (inward.length() == 0.0)
            return;
        const auto& t = tris[i];
        const Vec3 centre = (verts[t[0]] + verts[t[1]] + verts[t[2]]) * (1.0 / 3.0);
        // Start slightly inside so the ray does not hit its own triangle.
        const auto hit = bvh.raycast(centre + inward * eps, inward, maxThickness);
        if (hit)
            perTriangle[i] = hit->t + eps;
    });
    return perTriangle;
}

std::vector<double> vertexThickness(const MeshBvh& bvh, double maxThickness)
{
    // A vertex takes the thinnest value of its triangles, which is what a thickness check
    // looks for.
    const TriangleMesh& mesh = bvh.mesh();
    const auto& tris = mesh.triangles();
    const std::vector<double> perTriangle = triangleThickness(bvh, maxThickness);
    std::vector<double> out(mesh.vertices().size(), kNaN);
    for (std::size_t i = 0; i < tris.size(); ++i) {
        if (!std::isfinite(perTriangle[i]))
            continue;
        for (const std::uint32_t v : tris[i])
            if (!(out[v] <= perTriangle[i]))
                out[v] = perTriangle[i];
    }
    return out;
}

ValueStats valueStats(const std::vector<double>& values)
{
    ValueStats s;
    double sum = 0.0;
    double sumSq = 0.0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double v = values[i];
        if (!std::isfinite(v))
            continue;
        if (s.count == 0 || v < s.min) {
            s.min = v;
            s.minIndex = i;
        }
        if (s.count == 0 || v > s.max) {
            s.max = v;
            s.maxIndex = i;
        }
        sum += v;
        sumSq += v * v;
        ++s.count;
    }
    if (s.count > 0) {
        const auto n = static_cast<double>(s.count);
        s.mean = sum / n;
        s.rms = std::sqrt(sumSq / n);
        s.stddev = std::sqrt(std::max(0.0, sumSq / n - s.mean * s.mean));
    }
    return s;
}

} // namespace aerocad::core
