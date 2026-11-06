// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/geometry/Shape.hpp"

#include <stdexcept>

namespace aerocad::geometry {

class OffsetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Which way a surface is thickened, relative to its normal.
enum class ThickenSide {
    Outside, ///< From the surface along its normal.
    Inside,  ///< From the surface against its normal.
    Both,    ///< Half the thickness each way.
};

/// Offsets every face of a surface (face or shell) by `distance` mm along its normal
/// (negative: against it). Used for clearances, e.g. a part that keeps 2 mm off a body.
/// Throws OffsetError when the offset is not possible, for example a distance larger
/// than the radius of curvature on the concave side.
[[nodiscard]] Shape offsetSurface(const Shape& surface, double distance);

/// Turns a surface (face or shell) into a closed solid of the given thickness (mm, > 0),
/// with side walls along the boundary. Throws OffsetError on failure.
[[nodiscard]] Shape thickenSurface(const Shape& surface, double thickness, ThickenSide side = ThickenSide::Outside);

/// Area of all faces in mm^2.
[[nodiscard]] double surfaceArea(const Shape& shape);

} // namespace aerocad::geometry
