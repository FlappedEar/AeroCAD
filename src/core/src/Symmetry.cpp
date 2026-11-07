// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/core/Symmetry.hpp"

#include "aerocad/core/Align.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace aerocad::core {

namespace {

/// Points binned in cubic cells, for nearest neighbours within about one cell.
class PointGrid {
public:
    PointGrid(const std::vector<Vec3>& points, double cell) : m_points(points), m_cell(cell)
    {
        for (std::size_t i = 0; i < points.size(); ++i)
            m_cells[key(cellOf(points[i].x), cellOf(points[i].y), cellOf(points[i].z))].push_back(
                static_cast<std::uint32_t>(i));
    }

    /// Squared distance to the nearest point in the 27 cells around `p` (infinity if none).
    [[nodiscard]] double nearest(const Vec3& p, Vec3& found) const
    {
        double best = std::numeric_limits<double>::infinity();
        const std::int64_t cx = cellOf(p.x), cy = cellOf(p.y), cz = cellOf(p.z);
        for (std::int64_t dx = -1; dx <= 1; ++dx)
            for (std::int64_t dy = -1; dy <= 1; ++dy)
                for (std::int64_t dz = -1; dz <= 1; ++dz) {
                    const auto it = m_cells.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == m_cells.end())
                        continue;
                    for (const auto i : it->second) {
                        const Vec3 d = m_points[i] - p;
                        const double d2 = d.dot(d);
                        if (d2 < best) {
                            best = d2;
                            found = m_points[i];
                        }
                    }
                }
        return best;
    }

private:
    [[nodiscard]] std::int64_t cellOf(double v) const { return static_cast<std::int64_t>(std::floor(v / m_cell)); }
    [[nodiscard]] static std::uint64_t key(std::int64_t x, std::int64_t y, std::int64_t z)
    {
        const auto u = [](std::int64_t v) { return static_cast<std::uint64_t>(v + (1 << 20)) & 0x1fffff; };
        return (u(x) << 42) | (u(y) << 21) | u(z);
    }

    const std::vector<Vec3>& m_points;
    double m_cell;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_cells;
};

} // namespace

Vec3 mirrorPoint(const Vec3& p, const Vec3& planePoint, const Vec3& planeNormal)
{
    return p - planeNormal * (2.0 * (p - planePoint).dot(planeNormal));
}

Vec3 mirrorDirection(const Vec3& v, const Vec3& planeNormal)
{
    return v - planeNormal * (2.0 * v.dot(planeNormal));
}

std::optional<SymmetryPlane> findSymmetryPlane(const TriangleMesh& mesh, double minMatch)
{
    const auto& all = mesh.vertices();
    if (all.size() < 100)
        return std::nullopt;
    // About 8000 points spread over the mesh are plenty for the plane.
    const std::size_t stride = std::max<std::size_t>(1, all.size() / 8000);
    std::vector<Vec3> points;
    for (std::size_t i = 0; i < all.size(); i += stride)
        points.push_back(all[i]);
    const auto spread = fitPlane(points);
    if (!spread)
        return std::nullopt;
    const Vec3 centre = spread->origin;
    const Vec3 widest = spread->major;
    const Vec3 middle = spread->normal.cross(widest).normalized();
    const BoundingBox box = mesh.bounds();
    const double diagonal = (box.max - box.min).length();
    if (!(diagonal > 0.0))
        return std::nullopt;
    // Mirrored samples are matched against the mesh's own vertices (up to about a million),
    // whose spacing sets the scale of "lands on the mesh".
    std::vector<Vec3> dense;
    const std::size_t denseStride = std::max<std::size_t>(1, all.size() / 1000000);
    for (std::size_t i = 0; i < all.size(); i += denseStride)
        dense.push_back(all[i]);
    const double spacing = std::sqrt(std::max(mesh.surfaceArea(), 1e-9) / static_cast<double>(dense.size()));
    const double accept = std::max(3.0 * spacing, 1e-3 * diagonal);
    const PointGrid grid(dense, accept);
    const PointGrid coarse(points, 0.05 * diagonal);

    std::optional<SymmetryPlane> best;
    double bestWidth = 0.0;
    const Vec3 starts[] = {widest, middle, spread->normal, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<SymmetryPlane> found;
    for (const Vec3& start : starts) {
        Vec3 n = start.normalized();
        // The plane's offset first, by a scan across the middle of the extent: a scan with
        // more of one side than the other (an inner layer seen through one grille) has its
        // centroid well off the centre plane, too far for the refinement below to reach.
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (const Vec3& p : points) {
            lo = std::min(lo, p.dot(n));
            hi = std::max(hi, p.dot(n));
        }
        double d = n.dot(centre);
        {
            const double reach = 0.02 * diagonal;
            std::size_t bestCount = 0;
            for (double t = lo + 0.2 * (hi - lo); t <= hi - 0.2 * (hi - lo); t += 0.5 * reach) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < points.size(); i += 4) {
                    const Vec3& p = points[i];
                    Vec3 q;
                    if (coarse.nearest(p - n * (2.0 * (n.dot(p) - t)), q) <= reach * reach)
                        ++count;
                }
                if (count > bestCount) {
                    bestCount = count;
                    d = t;
                }
            }
        }
        double radius = 0.02 * diagonal;
        for (int iteration = 0; iteration < 30; ++iteration) {
            const PointGrid& g = radius > accept ? coarse : grid;
            Vec3 sum{0, 0, 0};
            double offset = 0.0;
            std::size_t pairs = 0;
            for (const Vec3& p : points) {
                const Vec3 m = p - n * (2.0 * (n.dot(p) - d));
                Vec3 q;
                if (g.nearest(m, q) > radius * radius)
                    continue;
                Vec3 pq = p - q;
                if (pq.dot(n) < 0.0)
                    pq = pq * -1.0;
                sum = sum + pq;
                offset += n.dot((p + q) * 0.5);
                ++pairs;
            }
            if (pairs < 10 || sum.length() < 1e-12)
                break;
            const Vec3 next = sum.normalized();
            d = offset / static_cast<double>(pairs);
            const double turn = std::acos(std::clamp(next.dot(n), -1.0, 1.0));
            n = next;
            radius = std::max(accept, radius * 0.6);
            if (radius <= accept && turn < 1e-5)
                break;
        }
        // The offset to a fraction of the vertex spacing: nearest-vertex pairs settle a
        // millimetre or two off, as a mirrored point slides to a vertex beside its match.
        {
            auto cost = [&](double t) {
                double sum = 0.0;
                for (const Vec3& p : points) {
                    Vec3 q;
                    sum += std::min(grid.nearest(p - n * (2.0 * (n.dot(p) - t)), q), accept * accept);
                }
                return sum;
            };
            double a = d - 2.0 * accept, b = d + 2.0 * accept;
            const double golden = 0.5 * (std::sqrt(5.0) - 1.0);
            double x1 = b - golden * (b - a), x2 = a + golden * (b - a);
            double f1 = cost(x1), f2 = cost(x2);
            for (int i = 0; i < 25; ++i) {
                if (f1 < f2) {
                    b = x2;
                    x2 = x1;
                    f2 = f1;
                    x1 = b - golden * (b - a);
                    f1 = cost(x1);
                } else {
                    a = x1;
                    x1 = x2;
                    f1 = f2;
                    x2 = a + golden * (b - a);
                    f2 = cost(x2);
                }
            }
            d = 0.5 * (a + b);
        }
        // How well this plane maps the mesh onto itself, counting only points it moves far
        // (a tenth of the extent across it): a thin sheet maps onto itself in its own
        // tangent plane, which is no centre plane.
        lo = std::numeric_limits<double>::max();
        hi = -lo;
        for (const Vec3& p : points) {
            lo = std::min(lo, p.dot(n));
            hi = std::max(hi, p.dot(n));
        }
        const double far = 0.1 * (hi - lo);
        std::vector<double> distances;
        std::size_t matched = 0, moved = 0;
        for (const Vec3& p : points) {
            if (2.0 * std::abs(n.dot(p) - d) < far)
                continue;
            ++moved;
            const Vec3 m = p - n * (2.0 * (n.dot(p) - d));
            Vec3 q;
            const double d2 = grid.nearest(m, q);
            if (d2 <= accept * accept) {
                ++matched;
                distances.push_back(std::sqrt(d2));
            }
        }
        if (distances.empty() || moved < points.size() / 3)
            continue;
        std::nth_element(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(distances.size() / 2),
                         distances.end());
        SymmetryPlane plane{n * d, n, static_cast<double>(matched) / static_cast<double>(moved),
                            distances[distances.size() / 2]};
        found.push_back(plane);
    }
    double bestMatch = 0.0;
    for (const auto& f : found)
        bestMatch = std::max(bestMatch, f.match);
    if (bestMatch < minMatch)
        return std::nullopt;
    for (const auto& f : found) {
        if (f.match < bestMatch - 0.2 || f.match < minMatch)
            continue;
        // Extent of the mesh across this plane.
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (const Vec3& p : points) {
            lo = std::min(lo, p.dot(f.normal));
            hi = std::max(hi, p.dot(f.normal));
        }
        if (!best || hi - lo > bestWidth + 1e-6 * diagonal) {
            best = f;
            bestWidth = hi - lo;
        }
    }
    // Several starts can end up across the same direction, some stuck a little off: the one
    // that matches best.
    if (best) {
        const Vec3 direction = best->normal;
        for (const auto& f : found) {
            if (std::abs(f.normal.dot(direction)) < 0.99 || f.match < minMatch)
                continue;
            if (f.match > best->match + 1e-3 || (f.match > best->match - 1e-3 && f.distance < best->distance))
                best = f;
        }
    }
    return best;
}

} // namespace aerocad::core
