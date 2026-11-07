// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "aerocad/core/Mesh.hpp"
#include "aerocad/core/MeshSelection.hpp"
#include "aerocad/geometry/Shape.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace aerocad::geometry {

/// How the selected region is parameterized before the B-spline fit.
enum class SurfaceFitMethod {
    /// Unfold, or Plane when the region cannot be unfolded (not a simple sheet).
    Auto,
    /// Height field over the best-fit plane, moving least squares. Only for regions that
    /// do not fold over their mean plane.
    Plane,
    /// Region flattened by a least squares conformal map; the surface parameters are the
    /// unfolded coordinates. Any amount of wrap, such as mirror housings or long curved
    /// strips. The region must be a simple sheet (a disc, holes allowed).
    Unfold,
};

struct SurfaceFitOptions {
    /// Deviation tolerance in mm. The fit is accepted (`withinTolerance`) when both
    /// deviation checks stay below it; the plane method also passes it to the fit.
    double tolerance = 0.25;
    /// Resampling grid nodes (plane) or control point spans (unfold) along the longer
    /// side; 0 picks one from the point count and mesh resolution.
    int gridSize = 0;
    SurfaceFitMethod method = SurfaceFitMethod::Auto;
    /// Taubin smoothing steps for the outline (the staircase of mesh border edges). 0
    /// keeps the exact mesh border, so neighbouring patches can be sewn.
    int outlineSmoothing = 6;
    /// Mesh vertex positions on the outline where the boundary is split into separate
    /// edges (where neighbouring patches meet). Used with outlineSmoothing 0.
    std::vector<core::Vec3> outlineCorners;
    /// Sorted subset of the selection the face is trimmed to (empty: all of it). The
    /// rest only adds data, so the surface runs on smoothly past the trim, for joining
    /// neighbouring patches with matching tangents.
    core::TriangleSet trimTo;
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
    bool trimmed = false; ///< Face bounded by the selection outline (else the fit rectangle).
    int holes = 0;        ///< Holes of the selection cut out of the face.
    std::string method;   ///< "plane" or "unfold".
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

/// Fits one smooth bicubic B-spline surface to the selected triangles of a mesh, trimmed
/// to the outer outline of the selection, with its holes cut out. Unfold (default): the
/// region is flattened by a least squares conformal map and the surface is fit to the
/// vertices by penalized least squares (thin plate penalty). Plane (approach "B
/// smoothing" of the CAD-66 spike): height grid over the PCA plane, then
/// GeomAPI_PointsToBSplineSurface in smoothing mode. See SurfaceFitMethod.
/// Throws SurfaceFitError when no surface can be built.
[[nodiscard]] SurfaceFitResult fitSurface(const core::TriangleMesh& mesh, const core::TriangleSet& selection,
                                          const SurfaceFitOptions& options = {});

} // namespace aerocad::geometry
