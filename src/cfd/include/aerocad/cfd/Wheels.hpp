// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace aerocad::cfd {

/// A wheel found on a part in the case frame (m; flow +X, Z up, axles along Y) (CAD-154).
struct Wheel {
    core::Vec3 centre;   ///< On the axle, at the middle of the tyre's width.
    double radius = 0.0; ///< Tyre radius: the centre is this high above the part's bottom.
    double yMin = 0.0;   ///< Width of the tyre along the axle.
    double yMax = 0.0;
    int axle = 0;        ///< Wheels on one axle line share it (0 = frontmost).

    [[nodiscard]] nlohmann::json toJson() const;
    static Wheel fromJson(const nlohmann::json& json);
};

/// Wheels standing on the bottom of a part: every place where the part rests on its
/// lowest Z (the tyre's contact patch) and whose surface around it, in the same band along
/// Y, lies on a circle through that place and centred above it (the tread). `length` is
/// the part's characteristic length (sets the tolerances). Sorted by axle, then Y.
[[nodiscard]] std::vector<Wheel> detectWheels(const core::TriangleMesh& caseMesh, double length);

/// Whether a triangle with this centroid belongs to a wheel: within the tyre's radius
/// (plus 3 %) of the axle and within its width (plus 5 % of the radius).
[[nodiscard]] bool inWheel(const Wheel& wheel, const core::Vec3& point);

/// The part split into the surface that stays still and one surface per axle (index =
/// Wheel::axle) whose triangles turn with the wheels.
struct WheelSplit {
    core::TriangleMesh body;
    std::vector<core::TriangleMesh> axles;
};
[[nodiscard]] WheelSplit splitWheels(const core::TriangleMesh& caseMesh, const std::vector<Wheel>& wheels);

} // namespace aerocad::cfd
