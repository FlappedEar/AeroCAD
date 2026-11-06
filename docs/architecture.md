# Architecture

```
app (Qt Quick, QML)          UI: viewport, panels, menus. Calls commands only.
  |
commands                      Named commands with JSON Schema parameters and JSON
  |                           results, undo/redo. Shared by the UI and (later) the MCP
  |                           server, so an AI agent can do everything the user can.
model                         Document (parts), undo stack, change notifications.
  |
io        geometry            io: import (STL, OBJ, 3MF, STEP, IGES, BREP), export (STL, 3MF, STEP).
  |         |                 geometry: Open CASCADE wrapper (B-rep shapes, tessellation,
core -------+                 transforms, primitives).
                              core: math, triangle meshes. No dependencies.
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

- **Base surface** (`geometry/SurfaceFit`, command `surface.fit_selection`): the "B
  smoothing" approach of the CAD-66 spike. PCA plane of the selected vertices, height grid
  by moving least squares (quadratic inside, plane at the border, thin-plate fill outside
  the data), `GeomAPI_PointsToBSplineSurface` in smoothing mode, trimmed to the outer
  selection outline (projected to the surface's parameter space). Deviation is checked
  both ways; mesh to surface uses a local projection seeded from surface samples. Selected
  vertices that project outside the trimmed face are reported separately (`outsideTrimMax`,
  outline fidelity: mesh staircase, rounded corners) with a warning above the tolerance.
  Not yet: holes in the selection, regions that fold over their mean plane, analytic
  primitives (plane, cylinder) before free-form fitting.
- **Offset / thicken** (`geometry/Offset`, commands `surface.offset`, `surface.thicken`):
  `BRepOffset_MakeSimpleOffset`, which offsets each face on its own surface (exact for
  the single-face base surfaces); with the solid flag it adds side walls and caps. The
  solid is reversed when it comes out inside out. Offsets larger than the concave radius
  of curvature fail with an error.

## Not yet decided / next steps

- Mesh library: **CGAL** (decided after the CAD-66 spike, see Confluence "CAD-66 Spike:
  Mesh region to B-rep surface"). The recommended pipeline needs CGAL Shape Detection
  (analytic primitive fit), and CGAL also covers surface parameterization, mesh repair and
  hole filling. CGAL's GPL packages are compatible with GPL-3.0-or-later. It will be added
  with the first feature that uses it (region selection / base surface, CAD-67).
- Large-mesh performance and picking limits (CAD-11).
- Background jobs (imports currently run on the UI thread).
