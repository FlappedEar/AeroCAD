// SPDX-License-Identifier: GPL-3.0-or-later
#include "aerocad/cfd/Wheels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <unordered_map>

namespace aerocad::cfd {

using core::TriangleMesh;
using core::Vec3;

nlohmann::json Wheel::toJson() const
{
    return {{"centre", {centre.x, centre.y, centre.z}}, {"radius", radius}, {"yMin", yMin},
            {"yMax", yMax},           {"axle", axle}};
}

Wheel Wheel::fromJson(const nlohmann::json& j)
{
    Wheel w;
    const auto& c = j.at("centre");
    w.centre = {c.at(0).get<double>(), c.at(1).get<double>(), c.at(2).get<double>()};
    w.radius = j.at("radius").get<double>();
    w.yMin = j.at("yMin").get<double>();
    w.yMax = j.at("yMax").get<double>();
    w.axle = j.value("axle", 0);
    return w;
}

namespace {

struct Facet {
    Vec3 c; ///< Centroid.
    Vec3 n; ///< Unit normal.
};

/// Contact patches: centroids of downward-facing triangles in the lowest band, grouped by
/// touching cells of an XY grid.
std::vector<std::vector<Vec3>> contactPatches(const std::vector<Facet>& facets, double zMin, double band,
                                              double cell)
{
    std::map<std::pair<long, long>, std::vector<Vec3>> grid;
    for (const Facet& f : facets)
        if (f.c.z < zMin + band && f.n.z < -0.5)
            grid[{std::lround(f.c.x / cell), std::lround(f.c.y / cell)}].push_back(f.c);
    std::vector<std::vector<Vec3>> patches;
    std::map<std::pair<long, long>, bool> seen;
    for (const auto& [key, points] : grid) {
        if (seen[key])
            continue;
        std::vector<Vec3> patch;
        std::vector<std::pair<long, long>> stack{key};
        seen[key] = true;
        while (!stack.empty()) {
            const auto k = stack.back();
            stack.pop_back();
            const auto& pts = grid.at(k);
            patch.insert(patch.end(), pts.begin(), pts.end());
            for (long dx = -1; dx <= 1; ++dx)
                for (long dy = -1; dy <= 1; ++dy) {
                    const std::pair<long, long> nk{k.first + dx, k.second + dy};
                    if (grid.count(nk) && !seen[nk]) {
                        seen[nk] = true;
                        stack.push_back(nk);
                    }
                }
        }
        patches.push_back(std::move(patch));
    }
    return patches;
}

} // namespace

std::vector<Wheel> detectWheels(const TriangleMesh& mesh, double length)
{
    std::vector<Wheel> wheels;
    if (mesh.isEmpty() || !(length > 0.0))
        return wheels;
    const auto& v = mesh.vertices();
    std::vector<Facet> facets;
    facets.reserve(mesh.triangleCount());
    for (std::size_t i = 0; i < mesh.triangleCount(); ++i) {
        const auto& t = mesh.triangles()[i];
        facets.push_back({(v[t[0]] + v[t[1]] + v[t[2]]) * (1.0 / 3.0), mesh.faceNormal(i)});
    }
    const double zMin = mesh.bounds().min.z;
    const double tol = 0.002 * length; // How far tread points may lie off the circle.

    for (const auto& patch : contactPatches(facets, zMin, 0.003 * length, 0.02 * length)) {
        if (patch.size() < 3)
            continue;
        double x0 = patch[0].x, x1 = x0, y0 = patch[0].y, y1 = y0;
        for (const Vec3& p : patch) {
            x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
            y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
        }
        const double xc = 0.5 * (x0 + x1);
        // Surface facing sideways in X/Z (tread, not the tyre's flanks) over the patch's width.
        std::vector<std::array<double, 2>> pts;
        for (const Facet& f : facets)
            if (f.c.y >= y0 && f.c.y <= y1 && std::abs(f.c.x - xc) < 0.3 * length && f.c.z < zMin + 0.6 * length &&
                std::abs(f.n.y) < 0.3)
                pts.push_back({f.c.x - xc, f.c.z - zMin});
        if (pts.size() < 20)
            continue;
        // The radius that puts the most of them on a circle resting on the bottom.
        double bestR = 0.0;
        std::size_t bestCount = 0;
        for (double r = 0.01 * length; r <= 0.3 * length; r += 0.001 * length) {
            std::size_t count = 0;
            for (const auto& p : pts)
                if (std::abs(std::hypot(p[0], p[1] - r) - r) < tol)
                    ++count;
            if (count > bestCount)
                bestCount = count, bestR = r;
        }
        if (bestCount < 20 || bestCount < pts.size() / 8)
            continue;
        // A wheel: the points on that circle go most of the way round it.
        std::array<bool, 36> sector{};
        for (const auto& p : pts)
            if (std::abs(std::hypot(p[0], p[1] - bestR) - bestR) < tol) {
                const double a = std::atan2(p[1] - bestR, p[0]) + std::numbers::pi;
                sector[std::min<std::size_t>(35, static_cast<std::size_t>(a / (2 * std::numbers::pi) * 36))] = true;
            }
        if (std::count(sector.begin(), sector.end(), true) < 24)
            continue;
        // Tyre width: from the contact patch outwards along Y, as long as slices of the
        // surface on that circle still go most of the way round it (tread, not a nearby
        // body panel that happens to touch the circle); a gap of two slices is a groove.
        Wheel w;
        w.radius = bestR;
        w.centre = {xc, 0.0, zMin + bestR};
        const double slice = 0.002 * length;
        const double reach = y0 - bestR;
        const auto slices = static_cast<std::size_t>((y1 - y0 + 2 * bestR) / slice) + 1;
        std::vector<std::array<bool, 36>> slicesRound(slices, std::array<bool, 36>{});
        for (const Facet& f : facets) {
            if (f.c.y < reach || f.c.y > y1 + bestR || std::abs(f.n.y) >= 0.3)
                continue;
            const double dx = f.c.x - xc, dz = f.c.z - w.centre.z;
            if (std::abs(std::hypot(dx, dz) - bestR) >= tol)
                continue;
            const double a = std::atan2(dz, dx) + std::numbers::pi;
            const auto k = std::min(slices - 1, static_cast<std::size_t>((f.c.y - reach) / slice));
            slicesRound[k][std::min<std::size_t>(35, static_cast<std::size_t>(a / (2 * std::numbers::pi) * 36))] = true;
        }
        auto round = [&](std::size_t k) { return std::count(slicesRound[k].begin(), slicesRound[k].end(), true) >= 18; };
        const auto first = static_cast<std::size_t>((y0 - reach) / slice);
        const auto last = std::min(slices - 1, static_cast<std::size_t>((y1 - reach) / slice));
        std::size_t lo = first, hi = last;
        for (std::size_t k = first, gap = 0; k-- > 0;) {
            if (round(k))
                lo = k, gap = 0;
            else if (++gap > 2)
                break;
        }
        for (std::size_t k = last + 1, gap = 0; k < slices; ++k) {
            if (round(k))
                hi = k, gap = 0;
            else if (++gap > 2)
                break;
        }
        w.yMin = std::min(y0, reach + static_cast<double>(lo) * slice);
        w.yMax = std::max(y1, reach + static_cast<double>(hi + 1) * slice);
        w.centre.y = 0.5 * (w.yMin + w.yMax);
        // A tyre whose tread grooves split the contact patch: one wheel.
        const auto same = std::find_if(wheels.begin(), wheels.end(), [&](const Wheel& o) {
            return std::abs(o.centre.x - xc) < 0.5 * bestR && o.yMin < w.yMax && w.yMin < o.yMax;
        });
        if (same != wheels.end()) {
            same->yMin = std::min(same->yMin, w.yMin);
            same->yMax = std::max(same->yMax, w.yMax);
            same->centre.y = 0.5 * (same->yMin + same->yMax);
            continue;
        }
        wheels.push_back(w);
    }
    // Axles: wheels on one line along Y (same X and centre height), from the front.
    std::sort(wheels.begin(), wheels.end(), [](const Wheel& a, const Wheel& b) { return a.centre.x < b.centre.x; });
    int axle = -1;
    for (std::size_t i = 0; i < wheels.size(); ++i) {
        const Wheel& prev = wheels[i == 0 ? 0 : i - 1];
        if (i == 0 || std::abs(wheels[i].centre.x - prev.centre.x) > 0.1 * wheels[i].radius ||
            std::abs(wheels[i].centre.z - prev.centre.z) > 0.1 * wheels[i].radius)
            ++axle;
        wheels[i].axle = axle;
    }
    std::stable_sort(wheels.begin(), wheels.end(), [](const Wheel& a, const Wheel& b) {
        return a.axle != b.axle ? a.axle < b.axle : a.centre.y < b.centre.y;
    });
    // One axis per axle: the mean of its wheels.
    for (int a = 0; a <= axle; ++a) {
        double x = 0, z = 0;
        int n = 0;
        for (const Wheel& w : wheels)
            if (w.axle == a)
                x += w.centre.x, z += w.centre.z, ++n;
        for (Wheel& w : wheels)
            if (w.axle == a)
                w.centre.x = x / n, w.centre.z = z / n;
    }
    return wheels;
}

bool inWheel(const Wheel& w, const Vec3& p)
{
    const double margin = 0.05 * w.radius;
    return p.y >= w.yMin - margin && p.y <= w.yMax + margin &&
           std::hypot(p.x - w.centre.x, p.z - w.centre.z) <= 1.03 * w.radius;
}

WheelSplit splitWheels(const TriangleMesh& mesh, const std::vector<Wheel>& wheels)
{
    int axles = 0;
    for (const Wheel& w : wheels)
        axles = std::max(axles, w.axle + 1);
    WheelSplit out;
    out.axles.resize(static_cast<std::size_t>(axles));
    std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> maps(out.axles.size() + 1);
    const auto& v = mesh.vertices();
    for (const auto& t : mesh.triangles()) {
        const Vec3 c = (v[t[0]] + v[t[1]] + v[t[2]]) * (1.0 / 3.0);
        std::size_t target = 0; // 0 = body, a + 1 = axle a.
        for (const Wheel& w : wheels)
            if (inWheel(w, c)) {
                target = static_cast<std::size_t>(w.axle) + 1;
                break;
            }
        TriangleMesh& m = target == 0 ? out.body : out.axles[target - 1];
        auto& map = maps[target];
        std::array<std::uint32_t, 3> idx{};
        for (int k = 0; k < 3; ++k) {
            const auto [it, added] = map.try_emplace(t[k], 0);
            if (added)
                it->second = m.addVertex(v[t[k]]);
            idx[k] = it->second;
        }
        m.addTriangle(idx[0], idx[1], idx[2]);
    }
    return out;
}

} // namespace aerocad::cfd
