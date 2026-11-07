// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <stdexcept>
#include <vector>

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

/// Cuts a surface (face or shell, for example a surface fitted to a scan) with closed
/// outlines projected along `direction` (a sketch's profiles along its normal; both ways,
/// so the sketch may lie above, below or through the surface). Keeps the part of the
/// surface inside the outlines (or outside with keepInside = false), still exactly on the
/// original curved surface, e.g. the skin of an aileron drawn over a scanned wing.
/// With `extend` (default) a single-face surface is first taken on its whole underlying
/// B-spline, past its own trim outline and holes, so the edges of the result are the
/// projected outline and not the (ragged) border of the region it was fitted to.
/// Throws OffsetError when nothing is left (the outline misses the surface).
[[nodiscard]] Shape trimSurface(const Shape& surface, const std::vector<Shape>& outlines, const core::Vec3& direction,
                                bool keepInside = true, bool extend = true);

/// A fin standing on a surface, e.g. a canard on a scanned bumper: the closed outlines
/// (drawn in a plane across the surface, its root reaching into the surface) are extruded
/// `thickness` mm symmetrically along `normal` (the outline plane's normal), and the
/// root is cut off where the solid passes through the surface: of the two sides (told
/// apart along `outward`, roughly the surface normal), the one with more of the fin is
/// kept, so it does not matter which way the outline was drawn. The root face lies
/// exactly on the curved surface. A single-face surface is taken on its whole B-spline
/// (see trimSurface).
/// With `rootFillet` > 0 the fin flares into the surface with a concave fillet of that
/// radius (mm) all round its root, tangent to the surface: a stronger joint and a bigger
/// face to glue a printed fin on; the root face still lies exactly on the surface.
/// Throws OffsetError when the outline does not reach through the surface, or the fillet
/// does not fit (a radius too big for the fin or the surface around it).
[[nodiscard]] Shape finOnSurface(const Shape& surface, const std::vector<Shape>& outlines, const core::Vec3& normal,
                                 double thickness, const core::Vec3& outward, double rootFillet = 0.0);

/// Cuts a solid with a surface (face or shell, e.g. a surface fitted to a scan) and keeps
/// what lies on one side of it: `keepFront` keeps the side the surface normal points to
/// (outside the scan for fitted surfaces), otherwise the side behind it. A single face is
/// taken on its whole underlying B-spline, past its trim, so it reaches across the solid.
/// Returns a null shape when nothing of the solid lies on that side; throws OffsetError
/// when the surface does not cut the solid at all ("does not reach").
[[nodiscard]] Shape splitBySurface(const Shape& solid, const Shape& surface, bool keepFront);

/// Area of all faces in mm^2.
[[nodiscard]] double surfaceArea(const Shape& shape);

} // namespace aerocad::geometry
