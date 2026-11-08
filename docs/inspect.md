# Inspect tools

Analysis tools modelled on Autodesk Fusion's **Inspect** menu: measure, clearance
(interference), section analysis, and colour maps for deviation (scan vs part) and wall
thickness. All of them are commands (`inspect.*`), so the UI, scripts and an AI agent use the
same code. They read the document and set view state; they never change the model and are not
recorded in the undo history.

Units are millimetres and degrees, model space is Z-up.

## Commands

| Command | What it does |
|---|---|
| `inspect.measure {a, b?}` | One selection: its properties and the wall thickness under it. Two: minimum distance, closest points, X/Y/Z delta, angle. |
| `inspect.clearance {a, b}` | Minimum clearance between two parts; interference and overlap volume for two B-rep solids. |
| `inspect.section {...}` | Section view: cuts the parts with a plane, caps the cut and reports section curves and areas per part. |
| `inspect.deviation {measured, reference, ...}` | Colours `measured` (e.g. a scan) by its signed distance to `reference` (e.g. the CAD body). |
| `inspect.thickness {id, ...}` | Colours a part by wall thickness. |
| `inspect.add_dimension {a, b?, show?}` | Keeps a measurement in the view as a dimension (saved with the project). |
| `inspect.dimensions` | The kept dimensions, each measured now. |
| `inspect.remove_dimension {id \| all}` | Removes one or all kept dimensions. |
| `inspect.state` | The active section and heat map. |
| `inspect.clear {what?}` | Turns off the section and/or the heat map. |

### Selections (`inspect.measure`)

- `{"point": [x, y, z]}`: a free point.
- `{"part": id, "point": [x, y, z]}`: what is under a surface point (a click). On B-rep parts
  `pick: "auto"` (default) takes the vertex or edge within `tolerance` (default 1 % of the part
  size) and otherwise the face; `pick` can force `point`, `vertex`, `edge`, `face` or `body`.
- `{"part": id, "face": i}`, `{"part": id, "edge": i}`, `{"part": id, "vertex": i}`: by index
  (indices as in `feature.faces` / `feature.edges`).
- `{"part": id}` or `pick: "body"`: the whole part.

Distances on B-rep selections are exact (Open CASCADE `BRepExtrema_DistShapeShape`). Mesh
parts (STL, OBJ, 3MF, scans) have no faces or edges: picks on them are points or the whole body,
and distances are to the triangle surface. Mesh-to-mesh distances project every vertex of each
mesh onto the other surface, so they are exact for vertex-to-face contacts and an upper bound
for edge-to-edge contacts. The angle is reported for two planar faces (0-180, between the
normals), two straight edges (0-90) or an edge and a planar face (0-90). The axis of a
cylindrical or conical face and of a circle counts as a straight edge, so a hole can be checked
against a face (90 = square to it).

Sketches are measured on their curves: lines, arcs and circles are exact edges on the sketch
plane (a circle reports its radius), so `pick: "auto"` takes a vertex or a curve near the point.
A sketch laid onto a surface is measured on its drawn lines (one edge per smooth stretch).

With one selection the result also has `thickness` (`value`, `pointA` on the surface, `pointB`
on the opposite wall) when the selection is a face of a solid (exact: from the point projected
onto the face, straight in against its normal, to the first other wall) or a point on a closed
mesh (an STL or 3MF part; along the triangle's inward normal). Open meshes such as scans have
no thickness.

### Kept dimensions

`inspect.add_dimension` stores the selections (as for `inspect.measure`) and what to show:
`distance` or `angle` for two selections, `thickness`, `radius` or `length` for one (default
distance with two, thickness with one). It fails when the selections do not give that value.
An `auto` pick is stored as the kind it resolved to (vertex, edge or face), so the dimension
stays on that kind of element. Dimensions are measured again whenever they are listed: after an
edit each selection is the element of that kind nearest its stored point, so a dimension
follows a changed size; one whose part is gone reports `error` instead of a value. They are
saved in the project file (in the view object, `view.dimensions`) and cleared by `project.new`.

### Section analysis

`{"plane": "xy" | "xz" | "yz", "offset": mm}` cuts at that coordinate (`xy` at z = offset, ...;
default the model centre); `{"origin", "normal", "offset"}` cuts with any plane. The normal
points to the half that is removed (defaults: XY removes the top, XZ the front, YZ the right
half, so the section faces the matching standard view); `flip` reverses it. `parts` limits the
cut to some parts, `cap: false` leaves the cut open, `includeCurves` returns the section
polylines (subsampled to `maxPoints`).

Each part reports its section curves, how many are closed, their total length and the enclosed
area (nested loops count as holes). Comparing the scan's and the part's section area and length
at the same plane is a quick check of how well they match. Closed loops are capped; open
surfaces (typical for scans) give open curves and no cap.

### Heat maps

`inspect.deviation` computes, for every vertex of the measured part, the signed distance to the
reference surface: positive outside the reference (along its normals), negative inside. The
colour scale runs from blue (-range) through green (0) to red (+range); `range` defaults to the
largest deviation. `tolerance` adds the share of points within +/- tolerance; `maxDistance`
ignores points farther away. The result includes min / max / mean / RMS / standard deviation,
where the extremes are, and a 10-bin histogram over the colour range.

When the reference is an exact part (a fitted surface, an imported solid, a body), distances
are measured against a dense triangulation of it (deflection 1/20 of the tolerance or 0.02 % of
its size), and every value above 2 % of the tolerance is then measured on the exact faces
(projection onto each face's surface, inside its trim). The display mesh alone can be off by
a tenth of a millimetre, more near sewn edges, which is more than a typical fit tolerance.

Fitted surfaces and solids (made by Fit smooth surface or Whole scan to solid) have the action
**Show deviation from the scan**: it colours the scan they were fitted to (range +/- twice the
tolerance, points farther than 8 tolerances left grey) and hides the fit so the colours show.

`inspect.thickness` casts a ray from the centre of every triangle straight inwards and takes the
distance to the opposite wall; a vertex shows the thinnest value of its triangles. Red is thin,
blue thick; `min` / `max` set the scale. Open meshes give no value where the ray leaves the part.

Only one heat map is active at a time. It applies to the meshes it was computed for
(`HeatMap::mesh`, `HeatMap::referenceMesh`); when one of them changes (an edit, undo), the app
runs the analysis again with the same parameters, or clears it when a part is gone.

## Check before export

Every export (File > Export Selected / All Visible / For Printing) first runs
`document.export_check`. When everything passes, the file is written straight away. Otherwise a
dialog lists each part with what is wrong, in plain words:

| Check | For Printing | STL / 3MF | STEP |
|---|---|---|---|
| Closed solid (no open edges, every edge shared by two faces) | error | warning | warning |
| Consistent, outward normals; no degenerate triangles | error | warning | warning |
| Exact geometry (B-rep) present | warning (STL only) | — | error |
| B-rep passes the validity check | warning | warning | warning |
| Walls thinner than the limit (thinnest wall, where, share of the surface) | warning | warning | warning |

An open mesh, such as a scan, can still be exported as STL or 3MF (e.g. to use it in another
program) after **Export Anyway**; only For Printing refuses it. The file dialog suggests the
part's name (or the project's name for all visible parts) as the file name.

Errors leave only **Cancel**; warnings allow **Export Anyway**. **Show Thin Walls** colours the
part with the thinnest wall by thickness: red is thinner than the limit, blue twice the limit
or more. The limit is set in Preferences > Export > Thinnest wall (default 0.8 mm, two
perimeters of a 0.4 mm nozzle; "off" skips the check). The wall is measured from each
triangle of the export mesh straight inwards to the opposite side, so sharp trailing edges
show up as a small share of thin surface.

`document.export_check {format?, ids?, minWall?, print?}` writes nothing and returns
`passed`, `canExport`, `summary` and per part `status` (ok / warning / error), `errors`,
`warnings`, `watertight`, `manifold`, `validBrep`, `volume` and `minWall {thinnest, at,
thinAreaPercent, limit}`.

### Writing a print-file set

**File > Export for Printing** (`document.export_for_print`) stages every STL/STEP
before replacing existing files. Destinations that are directories, symbolic links
(including dangling links) or special files are rejected. Existing outputs are kept
until all writers finish; a publication failure restores them and removes newly
published files. Sanitized names and ASCII case collisions get distinct filenames.
If the filesystem also aliases distinct Unicode names, publication is rejected and
the previous set is restored rather than silently combining two outputs.

If the filesystem prevents restoration, **Export failed** names the affected paths
and the hidden `.aerocad-export-*` recovery directory. Keep that directory: it contains
the surviving original files. A successful response contains only the final paths;
failure to remove temporary backups is returned as a cleanup warning.

This protects against errors reported during an isolated export command. It is not
a crash-safe directory transaction; a process/power failure or another program
modifying the same paths during publication can require recovery from the retained
backup directory.

## User interface

- **Measure** (Inspect menu, toolbar, key **I**): a panel like Fusion's. Click a first and a
  second selection; a third click starts over. One face shows its wall thickness under the click,
  drawn across the wall. Sketch lines and circles can be clicked too (within the pick radius).
  **Keep in view** keeps the result as a blue dimension that stays in the view after the panel
  closes (distance for two selections; thickness, radius or length for one; **Keep angle** for
  the angle) and updates when the model changes. The panel lists the kept dimensions under
  *In the view*, with a button to remove each one or all. *Selection filter*: Face / Edge / Vertex (picks
  within ~8 px of a vertex or edge, otherwise the face), Body (clearance between parts, with
  interference for B-rep solids) or Point. The closest points are joined by a dashed line with
  the distance. *Precision* sets the decimals.
- **Interference / Clearance**: Measure with the Body filter.
- **Section Analysis**: plane (XY / XZ / YZ), offset (slider, value, or drag the blue arrow in
  the view), Flip, Cap. Caps are red for bodies and blue for imported meshes / scans; the inside
  of cut meshes is drawn in a darker cap colour so open scans still read as cut. The panel lists
  the section area and curve length of every part. Closing it turns the section off.
- **Deviation Map** / **Thickness Map**: choose the parts, tolerance and range, then Compute.
  A legend with the colour scale appears in the bottom-left corner. Clear removes the map.
- **Clear Analyses** turns off the section and the heat map (also set by an agent through
  `inspect.*`).

## Implementation

- `core/MeshQuery`: bounding volume hierarchy over triangles (closest point, ray cast),
  mesh-to-mesh distance, per-vertex deviation and thickness, statistics.
- `core/MeshSection`: plane clipping, section curves (chained through shared edges and welded
  by position, so tessellated B-rep faces join up), nesting and area.
- `geometry/Inspect`: exact B-rep picking and distances, angles, properties, interference
  volume, and section caps (planar faces with holes, triangulated by Open CASCADE).
- `commands/InspectCommands`: the commands and `InspectState` (the active analyses).
- `app/InspectGeometry`: GPU geometry for the clipped part, the cap, the section curves and the
  heat map colours; `MeasurePanel`, `SectionPanel`, `HeatMapPanel`, `HeatMapLegend` (QML).
