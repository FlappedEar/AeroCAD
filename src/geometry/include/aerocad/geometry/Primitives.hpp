// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/geometry/Shape.hpp"

#include <filesystem>

namespace aerocad::geometry {

/// Axis-aligned box with one corner at the origin (sizes in mm).
[[nodiscard]] Shape makeBox(double dx, double dy, double dz);

/// Cylinder along +Z with its base centred at the origin.
[[nodiscard]] Shape makeCylinder(double radius, double height);

} // namespace aerocad::geometry
