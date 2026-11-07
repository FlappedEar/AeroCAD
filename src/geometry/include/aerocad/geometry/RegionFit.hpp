// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Shape.hpp"
#include "aerocad/geometry/SurfaceFit.hpp"

#include <string>
#include <vector>

namespace aerocad::geometry {

struct RegionFitOptions {
    /// Creases sharper than this (angle between neighbouring triangle normals, degrees)
    /// split the mesh into separate patches.
    double creaseAngleDeg = 30.0;
    /// Patches smaller than this fraction of the total area join a neighbour.
    double minRegionFraction = 0.005;
    /// Options for each patch fit (method, tolerance).
    SurfaceFitOptions fit;
    /// Largest gap closed when sewing the patches (mm); 0 picks one from the mesh
    /// resolution and the fit tolerance.
    double sewTolerance = 0.0;
};

/// One smooth patch of the region and its fit.
struct RegionPatch {
    core::TriangleSet triangles;
    SurfaceFitReport report; ///< Valid when `error` is empty.
    std::string error;       ///< Why the patch could not be fit.
};

struct RegionFitResult {
    /// Sewn shell of the patch faces, a solid when it closes up; null when nothing fit.
    Shape shape;
    std::vector<RegionPatch> patches;
    int faces = 0;
    int freeEdges = 0;   ///< Edges left unsewn (open borders and gaps).
    bool solid = false;  ///< The shell closed and became a solid.
    double volume = 0.0; ///< mm^3, for a solid.
    double sewTolerance = 0.0;
    double milliseconds = 0.0;
    std::vector<std::string> warnings;
};

/// Splits the selection (the whole mesh when empty) into smooth patches at creases, fits a
/// B-spline face to each (fitSurface) and sews the faces into one shell, or a solid when
/// the shell closes (a scanned part to a solid body). Patches that cannot be fit are
/// reported and left out. Throws SurfaceFitError when no patch fits.
[[nodiscard]] RegionFitResult fitRegions(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                                         const RegionFitOptions& options = {});

} // namespace aerocad::geometry
