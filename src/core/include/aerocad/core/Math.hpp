// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace aerocad::core {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr bool operator==(const Vec3&) const = default;

    [[nodiscard]] constexpr double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const
    {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] double length() const { return std::sqrt(dot(*this)); }
    [[nodiscard]] Vec3 normalized() const
    {
        const double len = length();
        return len > 0.0 ? *this * (1.0 / len) : Vec3{};
    }
};

/// Axis-aligned bounding box. A default-constructed box is empty.
struct BoundingBox {
    Vec3 min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max()};
    Vec3 max{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::lowest()};

    [[nodiscard]] bool isEmpty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }

    void extend(const Vec3& p)
    {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }

    void merge(const BoundingBox& other)
    {
        if (!other.isEmpty()) {
            extend(other.min);
            extend(other.max);
        }
    }

    [[nodiscard]] Vec3 center() const { return isEmpty() ? Vec3{} : (min + max) * 0.5; }
    [[nodiscard]] Vec3 size() const { return isEmpty() ? Vec3{} : max - min; }
    [[nodiscard]] double diagonal() const { return size().length(); }
};

} // namespace aerocad::core
