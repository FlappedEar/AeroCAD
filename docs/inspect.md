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
| `inspect.measure {a, b?}` | One selection: its properties. Two: minimum distance, closest points, X/Y/Z delta, angle. |
| `inspect.clearance {a, b}` | Minimum clearance between two parts; interference and overlap volume for two B-rep solids. |
| `inspect.section {...}` | Section view: cuts the parts with a plane, caps the cut and reports section curves and areas per part. |
| `inspect.deviation {measured, reference, ...}` | Colours `measured` (e.g. a scan) by its signed distance to `reference` (e.g. the CAD body). |
| `inspect.thickness {id, ...}` | Colours a part by wall thickness. |
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
normals), two straight edges (0-90) or an edge and a planar face (0-90).

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

`inspect.thickness` casts a ray from the centre of every triangle straight inwards and takes the
distance to the opposite wall; a vertex shows the thinnest value of its triangles. Red is thin,
blue thick; `min` / `max` set the scale. Open meshes give no value where the ray leaves the part.

Only one heat map is active at a time. It applies to the mesh it was computed for
(`HeatMap::mesh`); once the part's mesh changes, it no longer applies until the analysis runs
again.

## Implementation

- `core/MeshQuery`: bounding volume hierarchy over triangles (closest point, ray cast),
  mesh-to-mesh distance, per-vertex deviation and thickness, statistics.
- `core/MeshSection`: plane clipping, section curves (chained through shared edges and welded
  by position, so tessellated B-rep faces join up), nesting and area.
- `geometry/Inspect`: exact B-rep picking and distances, angles, properties, interference
  volume, and section caps (planar faces with holes, triangulated by Open CASCADE).
- `commands/InspectCommands`: the commands and `InspectState` (the active analyses).
