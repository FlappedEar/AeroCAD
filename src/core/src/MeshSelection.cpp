// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/MeshSelection.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
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
    // Edge (canonical a < b) -> first triangle * 3 + slot that has it.
    std::unordered_map<std::uint64_t, std::uint64_t> open;
    open.reserve(triangles.size() * 2);
    for (std::size_t t = 0; t < triangles.size(); ++t) {
        for (int slot = 0; slot < 3; ++slot) {
            std::uint32_t a = canonical[triangles[t][slot]];
            std::uint32_t b = canonical[triangles[t][(slot + 1) % 3]];
            if (a == b)
                continue;
            if (a > b)
                std::swap(a, b);
            const std::uint64_t key = (static_cast<std::uint64_t>(a) << 32) | b;
            const std::uint64_t here = t * 3 + static_cast<std::uint64_t>(slot);
            auto [it, inserted] = open.try_emplace(key, here);
            if (inserted)
                continue;
            const std::uint64_t other = it->second;
            const auto otherTri = static_cast<std::uint32_t>(other / 3);
            const auto otherSlot = static_cast<std::size_t>(other % 3);
            if (m_neighbours[otherTri][otherSlot] == none && otherTri != t) {
                m_neighbours[otherTri][otherSlot] = static_cast<std::uint32_t>(t);
                m_neighbours[t][static_cast<std::size_t>(slot)] = otherTri;
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
    const double seedCos = cosDeg(options.maxSeedAngleDeg);
    const Vec3 seedNormal = mesh.faceNormal(seed);

    std::vector<char> visited(mesh.triangleCount(), 0);
    std::vector<std::uint32_t> stack{seed};
    visited[seed] = 1;
    TriangleSet out;
    while (!stack.empty()) {
        const std::uint32_t t = stack.back();
        stack.pop_back();
        out.push_back(t);
        const Vec3 n = mesh.faceNormal(t);
        for (const std::uint32_t nb : adjacency.neighbours(t)) {
            if (nb == MeshAdjacency::none || visited[nb] != 0)
                continue;
            const Vec3 nn = mesh.faceNormal(nb);
            // Accepts flipped neighbours too (inconsistent winding in many STL files).
            const double c = std::abs(n.dot(nn));
            if (c < neighbourCos || std::abs(seedNormal.dot(nn)) < seedCos)
                continue;
            visited[nb] = 1;
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
                if (nb == MeshAdjacency::none)
                    continue;
                if (grow)
                    next[nb] = 1;
                else if (mask[nb] == 0)
                    next[t] = 0; // on the selection border
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
