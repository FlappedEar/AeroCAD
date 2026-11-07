# Architecture

```
app (Qt Quick, QML)          UI: viewport, panels, menus. Calls commands only.
  |
commands                      Named commands with JSON Schema parameters and JSON
  |                           results, undo/redo. Shared by the UI and (later) the MCP
  |                           server, so an AI agent can do everything the user can.
project                       .aerocad files: versioned manifest, lossless meshes,
  |                           B-rep, atomic save (docs/project-format.md).
cad                           Parametric timeline, parameters, frames, variants.
  |
model                         Document (parts), undo stack, change notifications.
  |
io        geometry            io: import (STL, OBJ, 3MF, STEP, IGES, BREP), export (STL, 3MF, STEP).
  |         |                 geometry: Open CASCADE wrapper (B-rep shapes, tessellation,
core -------+                 transforms, primitives).
                              core: math, triangle meshes, mesh repair / decimation /
                              alignment (docs/scans.md). No dependencies.
```

## Key decisions

- **One language, C++20.** Open CASCADE is the B-rep kernel; Qt 6 with Qt Quick 3D is the UI.
- **Command layer first.** Every user-visible operation is a command such as
  `document.import {"path": ...}` or `part.scale {"id": 1, "factor": 2}`. Commands
  validate their parameters against a JSON Schema, run, and return a JSON object or an
  error. `commands.list` returns all commands with their schemas, which maps 1:1 to MCP
  tool definitions. The UI uses `AppController::execute()`; nothing in QML changes the
  document directly.
- **Undo** is recorded by commands as part-state snapshots (cheap: meshes and shapes are
  shared, immutable objects).
- **No OCCT in public headers** outside `geometry`/`io`. `geometry::Shape` is a pimpl
  around `TopoDS_Shape`; modules that need OCCT link `AeroCAD::geometry_occt`.
- **Units and axes:** millimetres, Z up (CAD convention). The viewport rotates the model
  node to Qt Quick 3D's Y-up.
- **Rendering:** each part is a `QQuick3DGeometry` (`MeshGeometry`). Plain meshes are
  flat shaded so facets stay visible; tessellated B-rep parts are smooth shaded.

- **Region selection** is a sorted list of triangle indices stored in the part
  (`Part::selection`), so it is undone and redone like any other part change. The
  algorithms (smooth region, brush, lasso with a software depth buffer, grow / shrink)
  live in `core/MeshSelection` and are exposed as `selection.*` commands. Triangle
  adjacency is matched by vertex position, so tessellated B-rep faces are connected
  across their seams; it is computed once per mesh and cached. The brush preview while
  dragging is kept in `AppController`; releasing the button runs one `selection.brush`.

- **Base surface** (`geometry/SurfaceFit`, command `surface.fit_selection`), two methods:
  - *unfold* (default): the selected patch is welded, checked to be a simple sheet (Euler
    characteristic against its border loops) and flattened by a least squares conformal
    map (free border, two pinned vertices, conjugate gradients with an SSOR
    preconditioner; patches above 12 000 vertices are coarsened by vertex clustering
    first). A bicubic B-spline with the unfolded (u, v) in mm as its parameters is fit to
    the vertices by penalized least squares (thin plate penalty on the control net,
    banded Cholesky), so any amount of wrap works (mirror housings, long curved strips)
    and outline and holes are trimmed at their known (u, v), without projection.
  - *plane*: the "B smoothing" approach of the CAD-66 spike. PCA plane of the selected
    vertices, height grid by moving least squares (quadratic inside, plane at the border,
    thin-plate fill outside the data), `GeomAPI_PointsToBSplineSurface` in smoothing
    mode, trimmed to the outline projected to the surface. `auto` uses it when the patch
    cannot be unfolded (handles, non-manifold edges).
  Holes of the selection (inner border loops) are cut out of the face. Deviation is
  checked both ways; mesh to surface uses a local projection seeded from surface samples.
  Selected vertices that project outside the trimmed face are reported separately
  (`outsideTrimMax`, outline fidelity: mesh staircase, rounded corners) with a warning
  above the tolerance. Not yet: analytic primitives (plane, cylinder) before free-form
  fitting.
- **Scan to solid** (`geometry/RegionFit`, command `surface.fit_regions`): the mesh (or its
  selection) is split into smooth patches at creases (`core::segmentSmoothRegions`, small
  patches join a neighbour), each patch is fit as above with its exact mesh border (no
  outline smoothing) split into edges where the neighbour changes, and the faces are sewn
  (`BRepBuilderAPI_Sewing`); a closed shell becomes a solid. Patches that miss the
  tolerance or cannot be unfolded (tubes) are cut in two across the direction their
  normals turn most and fit again (up to 3 times). Patches across a crease meet with C0
  continuity. Where one smooth region was split, each patch is fit to its triangles plus
  `blendRings` (default 6) rings of its neighbours in the same region and trimmed back
  to its own triangles (`SurfaceFitOptions::trimTo`): both surfaces fit the same data
  across the seam, so their tangents match closely (near G1, not exact). The largest
  normal angle along those seams is reported (`seamAngleMaxDeg`; a 90-sided can drops
  from 0.87 to 0.11 degrees). Exact G1 (constrained fitting) is not done.
- **Offset / thicken** (`geometry/Offset`, commands `surface.offset`, `surface.thicken`):
  `BRepOffset_MakeSimpleOffset`, which offsets each face on its own surface (exact for
  the single-face base surfaces); with the solid flag it adds side walls and caps. The
  solid is reversed when it comes out inside out. Offsets larger than the concave radius
  of curvature fail with an error.
- **Designing on a curved surface** (an aileron or flap on a scanned wing): `sketch.create`
  with `surface: {part}` puts the sketch on the area-weighted best-fit plane
  (`core::fitPlaneToTriangles`) of the part's selection, of the area around the clicked
  point (a scan) or of the whole fitted surface, normal the way the surface faces, X
  along its long side, lifted to its highest point. The plane is only a drawing base:
  `surface.trim_by_sketch` (`geometry::trimSurface`) sweeps the closed profiles both
  ways along the sketch normal into prisms and keeps the common part (or the cut) of
  the surface, so the result lies exactly on the original curved surface. A single-face
  surface is trimmed on its whole underlying B-spline (`extend`), so the edges are the
  outline's and not the ragged border of the brushed region it was fitted to.
  `surface.part_from_sketch` does it in one step from the sketch (it remembers its part
  in `onSurface`). On a scan, or a surface fitted to one (`model::fittedScanId`), it uses
  `geometry::partOnScan`: heights over the sketch plane sampled at the first hit from
  outside (`core::MeshProjector`), so layers behind the skin never count; the largest
  sheet under the outline is kept and holes and steep pits are bridged; two B-spline
  height fields are fitted by tensor-product least squares, the skin and its rolling-ball
  offset (which cannot fold in sharp details); the sketch coordinates are their
  parameters, so the outline is laid on them exactly, without booleans, and the walls run
  along the sketch normal. The fit takes milliseconds on a 3M-triangle scan. Other
  surfaces are trimmed and thickened. Such sketches are shown lying on the scan:
  `Design::syncDocument` splits the curves and the profile fill finely and moves them onto
  it along the sketch normal (`core::projectOntoMesh`); fill over holes or walls is left
  out, lines run straight across them. Clicks and previews while sketching use a cached
  `core::MeshProjector` (`AppController::pickPartSurface`, `dropOntoPart`): triangles binned
  in a grid across the sketch normal; about a millisecond per pointer move at 3M triangles. `across: true`
  sketches stand on the surface instead (X along its normal, through the clicked point;
  turned by `angle` about the normal), for fins: `sketch.profiles` reports where the
  plane cuts the surface (`surfaceLine`, `core::sliceMesh`), which the sketch overlay
  draws on top. `surface.fin_from_sketch` (`geometry::finOnSurface`) extrudes the
  outline half the thickness each way, splits it with the surface (`BRepAlgoAPI_Splitter`) and keeps the side with more of
  the fin, so the root lies exactly on the surface. On a scan the surface under the root
  is `geometry::skinOnScan`: the same first-skin-from-outside height-field fit as
  `partOnScan` (`core::MeshProjector` along the surface normal), so an inner layer,
  holes and flipped triangles do not pull the root into the scan.

## Not yet decided / next steps

- Mesh library: **CGAL** (decided after the CAD-66 spike, see Confluence "CAD-66 Spike:
  Mesh region to B-rep surface"). The recommended pipeline needs CGAL Shape Detection
  (analytic primitive fit), and CGAL also covers surface parameterization, mesh repair and
  hole filling. CGAL's GPL packages are compatible with GPL-3.0-or-later. It will be added
  with the first feature that uses it (region selection / base surface, CAD-67).
- Large-mesh performance and picking limits (CAD-11).
- Background jobs (imports currently run on the UI thread).
