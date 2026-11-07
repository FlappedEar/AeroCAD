// SPDX-License-Identifier: GPL-3.0-or-later
// A scan-like front bumper for canard tests: symmetric about x = 0, front facing -Y, z up,
// with noise, holes, a layer behind the skin and some flipped triangles.
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/Mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace aerocad::testing {

/// Plan view: a front face at y = 0 for |x| <= halfWidth - radius, rounded corners of
/// `radius` and sides at |x| = halfWidth running back to y = radius + sideLength. The skin
/// leans in towards the top and bottom (by bulge(z) along its normal), z = 0..height.
struct SyntheticBumper {
    double halfWidth = 600.0;
    double radius = 200.0;
    double sideLength = 250.0;
    double height = 300.0;
    double step = 2.5;   ///< Triangle size, mm.
    double noise = 0.15; ///< Standard deviation of the noise along the normal, mm.

    [[nodiscard]] double bulge(double z) const
    {
        const double t = (z - 0.5 * height) / (0.5 * height);
        return 25.0 * t * t + 8.0 * t * t * t; // leans in more towards the top
    }

    /// Point and outward plan normal at arc length s along the plan curve (from the left
    /// side's back end to the right side's back end).
    void plan(double s, double& x, double& y, double& nx, double& ny) const
    {
        const double front = 2.0 * (halfWidth - radius);
        const double arc = 0.5 * 3.14159265358979 * radius;
        const double c = halfWidth - radius;
        auto side = [&](double t, double sign) { // t from the corner end backwards
            x = sign * halfWidth;
            y = radius + t;
            nx = sign;
            ny = 0.0;
        };
        auto corner = [&](double t, double sign) { // t from the front end
            const double a = t / radius;            // 0 at the front, pi/2 at the side
            x = sign * (c + radius * std::sin(a));
            y = radius - radius * std::cos(a);
            nx = sign * std::sin(a);
            ny = -std::cos(a);
        };
        if (s < sideLength)
            return side(sideLength - s, -1.0);
        s -= sideLength;
        if (s < arc)
            return corner(arc - s, -1.0);
        s -= arc;
        if (s < front) {
            x = -c + s;
            y = 0.0;
            nx = 0.0;
            ny = -1.0;
            return;
        }
        s -= front;
        if (s < arc)
            return corner(s, 1.0);
        s -= arc;
        side(std::min(s, sideLength), 1.0);
    }

    [[nodiscard]] double planLength() const
    {
        return 2.0 * sideLength + 3.14159265358979 * radius + 2.0 * (halfWidth - radius);
    }

    /// Signed distance of p outside the clean (noise-free) outer skin, mm (in plan, at p.z).
    [[nodiscard]] double outside(const core::Vec3& p) const
    {
        const double c = halfWidth - radius;
        const double b = bulge(p.z);
        const double ax = std::abs(p.x);
        if (ax <= c)
            return b - p.y;
        if (p.y >= radius)
            return ax - (halfWidth - b);
        return std::hypot(ax - c, p.y - radius) - (radius - b);
    }

    /// A point on the clean skin: on the right corner `degrees` from the front (0) towards
    /// the side (90), at height z.
    [[nodiscard]] core::Vec3 onCorner(double degrees, double z, bool left = false) const
    {
        const double a = degrees * 3.14159265358979 / 180.0;
        const double r = radius - bulge(z);
        const double c = halfWidth - radius;
        const double x = c + r * std::sin(a);
        return {left ? -x : x, radius - r * std::cos(a), z};
    }

    [[nodiscard]] core::TriangleMesh mesh(std::uint32_t seed = 7) const
    {
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, noise);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        const int nu = static_cast<int>(std::ceil(planLength() / step));
        const int nz = static_cast<int>(std::ceil(height / step));
        std::vector<core::Vec3> v;
        std::vector<core::TriangleMesh::Triangle> t;
        struct Hole {
            double s, z, rs, rz;
        };
        std::vector<Hole> holes;
        for (int i = 0; i < 12; ++i)
            holes.push_back({uniform(rng) * planLength(), uniform(rng) * height, 5.0 + 15.0 * uniform(rng),
                             5.0 + 10.0 * uniform(rng)});
        auto layer = [&](double behind, bool outer, double fraction) {
            const auto base = static_cast<std::uint32_t>(v.size());
            for (int j = 0; j <= nz; ++j)
                for (int i = 0; i <= nu; ++i) {
                    const double s = planLength() * i / nu, z = height * j / nz;
                    double x, y, nx, ny;
                    plan(s, x, y, nx, ny);
                    const double in = bulge(z) + behind - (outer ? gauss(rng) : 0.0);
                    v.push_back({x - nx * in, y - ny * in, z});
                }
            auto idx = [&](int i, int j) { return base + static_cast<std::uint32_t>(j * (nu + 1) + i); };
            for (int j = 0; j < nz; ++j)
                for (int i = 0; i < static_cast<int>(nu * fraction); ++i) {
                    const double s = planLength() * (i + 0.5) / nu, z = height * (j + 0.5) / nz;
                    if (outer && std::any_of(holes.begin(), holes.end(), [&](const Hole& h) {
                            const double ds = (s - h.s) / h.rs, dz = (z - h.z) / h.rz;
                            return ds * ds + dz * dz < 1.0;
                        }))
                        continue;
                    // Outer skin faces out (-Y at the front); the layer behind faces in.
                    std::array<std::uint32_t, 3> a{idx(i, j), idx(i + 1, j + 1), idx(i + 1, j)};
                    std::array<std::uint32_t, 3> b{idx(i, j), idx(i, j + 1), idx(i + 1, j + 1)};
                    if (outer) {
                        std::swap(a[1], a[2]);
                        std::swap(b[1], b[2]);
                    }
                    if (outer && uniform(rng) < 0.02)
                        std::swap(a[1], a[2]); // inconsistent orientation, as in real scans
                    t.push_back(a);
                    t.push_back(b);
                }
        };
        layer(0.0, true, 1.0);
        layer(3.0, false, 0.6); // the inside of the bumper, seen through the holes
        return core::TriangleMesh(std::move(v), std::move(t));
    }
};

} // namespace aerocad::testing
