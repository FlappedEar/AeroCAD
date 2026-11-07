# Working with scans

3D scans arrive as big triangle meshes (often millions of triangles) in an unknown unit
and an arbitrary orientation, with holes and flipped or duplicated faces. These commands
make them usable as a reference for modelling. All of them are undoable and, like every
command, available to scripts and to an AI agent over MCP (`commands.list`).

## Units and scale

| Command | What it does |
| --- | --- |
| `document.import {"path", "units"}` | `units`: `um`, `mm` (default), `cm`, `m`, `in`, `ft`. STL, OBJ and PLY carry no unit; the file is scaled to millimetres on import. |
| `part.convert_units {"id", "from", "to"}` | Fix a part imported in the wrong unit: `from: "m"` scales by 1000 (about the world origin). `to` defaults to `mm`. |
| `part.calibrate {"id", "a", "b", "distance", "units"}` | Scale so that the distance between two points (picked on the model) equals a known dimension. Point `a` stays fixed. Returns the applied `factor`. |

## Orientation

| Command | What it does |
| --- | --- |
| `part.align {"id", "points" \| "triangles" \| "normal"+"origin", "target", "origin", "xDirection", "flip", "keepPosition"}` | Fit a plane to 3+ picked points or to a triangle region (e.g. a selection), turn it onto a world plane (`xy` default: normal to +Z; `xz`: +Y; `yz`: +X) and move its origin to the world origin. `xDirection` fixes the in-plane rotation. Returns the 4x4 `transform` and the fit `fitRms`. |
| `part.transform {"id", "axis", "angle", "center", "translate"}` | Rigid move/rotation (degrees, right-hand rule; rotation first). |
| `part.place {"id", "onFloor"}` | Centre the bounding box on the origin in X/Y and rest it on Z = 0 (or centre it in Z). |

To put a picked face of a scan on the floor, align with that face's triangles and
`flip: true`: its outward normal then points down and the object stands on the XY plane.

## Repair

| Command | What it does |
| --- | --- |
| `mesh.stats {"id"}` | Triangles, holes (boundary loops), boundary / non-manifold / inconsistent (flipped) edges, duplicate and degenerate triangles, unused vertices, shells, area, bounds. |
| `mesh.repair {"id", "mergeDistance", "removeDegenerate", "removeDuplicates", "fixOrientation", "maxHoleEdges"}` | Merge (close) vertices, drop degenerate and duplicate triangles, make the winding consistent (closed shells face outwards), fill holes up to `maxHoleEdges` edges, drop unused vertices. Returns the counts and the stats afterwards. |
| `mesh.flip_normals {"id"}` | Reverse every triangle. |
| `mesh.decimate {"id", "targetTriangles"}` | Permanently reduce the mesh (vertex clustering). Usually not needed, see below. |

Repair, flip and decimate refuse parts with exact CAD geometry (STEP/IGES/BREP) and
timeline bodies; they are meant for meshes.

Holes are filled by ear clipping in the hole's best-fit plane (no new vertices), which suits
the small, roughly planar holes typical of scans; a hole that does not project to a simple
polygon is left open and reported as `skippedHoles`. Large holes (scanner shadows) are better closed by
modelling over them.

## Performance (5.1 M triangles, Release build, one core)

| Operation | Time |
| --- | --- |
| `decimateToTarget` to 1 M triangles | 0.7 s |
| `meshStats` | 1.4 s |
| `repairMesh` (all steps) | 3.0 s |

## In the app

- **Import** (toolbar, File > Import, drag & drop) parses files on a worker thread with a
  progress bar and a Cancel button; the part is then added through `document.import`.
  The unit for new imports is set in Scan Tools ("New imports in").
- **Scan Tools** (toolbar INSERT > Scan, Modify > Scan Tools; no shortcut, Fusion has none)
  acts on the selected imported part: mesh statistics, Repair / Flip Normals, unit
  conversion, Calibrate (Pick 2 Points, enter the true distance), Orient (Pick Points on a
  flat area, choose XY / XZ / YZ, Align; Place on Floor, Center). Picked points snap to the
  nearest vertex of the full-resolution mesh. Esc closes the panel.

## Display level of detail

Meshes above the "Max. triangles drawn" budget (Scan Tools > Display, default 1 M,
remembered between sessions) are drawn simplified by vertex clustering, computed in the
background. The document keeps the full mesh, so picking snaps, selection, fitting and
modelling always use all the scan data. A 5 M-triangle PLY imports and renders in about
5 s in the cloud CI container with software OpenGL.
