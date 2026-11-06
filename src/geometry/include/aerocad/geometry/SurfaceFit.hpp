// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace aerocad::geometry {

struct SurfaceFitOptions {
    /// Approximation tolerance in mm passed to the B-spline fit. The fit is accepted
    /// (`withinTolerance`) when both deviation checks stay below it.
    double tolerance = 0.25;
    /// Resampling grid nodes along the longer side; 0 picks one from the point count.
    int gridSize = 0;
};

/// Fit quality, both ways: mesh to surface catches a surface that misses the mesh,
/// surface to mesh catches wiggles and folds between mesh points.
struct SurfaceFitReport {
    double meshToSurfaceMax = 0.0; ///< mm, selected mesh vertices that project inside the trimmed face.
    double meshToSurfaceRms = 0.0; ///< mm.
    double surfaceToMeshMax = 0.0; ///< mm, face triangulation nodes to the selected mesh.
    /// mm, how far selected vertices that project outside the trimmed face are from its
    /// boundary: the outline fidelity (staircase of mesh edges, rounded corners). Not part
    /// of `withinTolerance`; a warning is added when it exceeds the tolerance.
    double outsideTrimMax = 0.0;
    bool withinTolerance = false;
    int uPoles = 0;
    int vPoles = 0;
    bool trimmed = false; ///< Face trimmed to the selection outline (else the fit rectangle).
    double milliseconds = 0.0;
    std::vector<std::string> warnings;
};

struct SurfaceFitResult {
    Shape face; ///< One B-spline face, null on failure.
    SurfaceFitReport report;
};

class SurfaceFitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Fits one smooth B-spline surface to the selected triangles of a mesh (approach
/// "B smoothing" of the CAD-66 spike): best-fit plane by PCA, height grid resampled from
/// the selected vertices, GeomAPI_PointsToBSplineSurface in smoothing mode, trimmed to
/// the outer outline of the selection. The region must be a height field over its
/// best-fit plane (no folds, less than about 180 degrees of wrap).
/// Throws SurfaceFitError when no surface can be built.
[[nodiscard]] SurfaceFitResult fitSurface(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                                          const SurfaceFitOptions& options = {});

} // namespace aerocad::geometry
