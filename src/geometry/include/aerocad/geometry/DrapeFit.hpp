// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Math.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Offset.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <vector>

namespace aerocad::geometry {

/// The rectangle of a sketch plane to cover; x, y, normal are orthonormal and right-handed
/// (x cross y = normal). The normal points out of the scan.
struct DrapeArea {
    core::Vec3 origin;
    core::Vec3 x;
    core::Vec3 y;
    core::Vec3 normal;
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = 0.0;
    double v1 = 0.0;
    /// Inside this part of the rectangle (the outline) the deviation is measured and the
    /// sheet to follow is chosen.
    double insideU0 = 0.0;
    double insideV0 = 0.0;
    double insideU1 = 0.0;
    double insideV1 = 0.0;
};

struct DrapeFitOptions {
    /// Deviation aimed for, mm. The surface is refined until it stays within this, or the
    /// control points reach minSpacing.
    double tolerance = 0.25;
    /// Smallest control point spacing, mm: details finer than this are smoothed. Also
    /// never finer than 1/64 of the longer side of the area, to keep the surfaces light.
    double minSpacing = 2.5;
    /// Steepest step between neighbouring samples that still counts as the same skin
    /// (rise over run); steeper walls (a hole in a grille, a wheel arch) are bridged.
    double maxSlope = 3.0;
};

struct DrapeFitReport {
    double maxDeviation = 0.0;    ///< mm, on the followed skin inside the outline.
    double rmsDeviation = 0.0;    ///< mm.
    double p99Deviation = 0.0;    ///< mm.
    double bridgedFraction = 0.0; ///< Share of the outline's rectangle with no skin to follow (holes, off the scan).
    int uPoles = 0;
    int vPoles = 0;
    double milliseconds = 0.0;
};

struct DrapedPart {
    Shape solid;       ///< One solid per outline.
    double skinArea = 0.0; ///< mm2, the side lying on the scan.
    DrapeFitReport report;
};

/// A solid that follows the skin of a scan under sketch outlines, as seen along the sketch
/// normal: the first surface met from outside, so layers behind it (the inside of a bumper, a
/// wheel arch liner) never count. Holes and steep pits are bridged smoothly.
///
/// Two surfaces are fitted over the sketch plane, the skin and its offset by `thickness`
/// (a rolling ball, so sharp details cannot make it fold). Both are height fields with the
/// sketch coordinates as parameters, so the outlines are exactly the trim curves; the walls
/// run along the sketch normal. The normal of `area` is the outward side (ThickenSide).
/// Takes well under a second for the fit, on a scan of millions of triangles.
/// Throws SurfaceFitError (SurfaceFit.hpp) when the scan is not under the area.
[[nodiscard]] DrapedPart partOnScan(const core::MeshProjector& scan, const DrapeArea& area,
                                    const std::vector<Shape>& outlines, double thickness, ThickenSide side,
                                    const DrapeFitOptions& options = {});

/// Just the skin of the scan over the area's rectangle, as one B-spline face (a height field
/// over the area, the first surface met from outside, holes bridged), e.g. to cut the root of
/// a fin standing on the scan. Throws SurfaceFitError when the scan is not under the area.
[[nodiscard]] Shape skinOnScan(const core::MeshProjector& scan, const DrapeArea& area,
                               DrapeFitReport* report = nullptr, const DrapeFitOptions& options = {});

} // namespace aerocad::geometry
