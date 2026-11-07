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
    /// Where a smooth region had to be split into several patches, each patch is fit to
    /// its triangles plus this many rings of its neighbours in the same region and then
    /// trimmed back to its own triangles: neighbouring surfaces fit the same data across
    /// the seam, so their tangents match (near G1). 0 fits each patch on its own (C0).
    int blendRings = 6;
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
    /// Seams between patches of the same smooth region (splits, not creases), and the
    /// largest angle between the two faces' normals measured along them (degrees).
    int smoothSeams = 0;
    double seamAngleMaxDeg = 0.0;
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
