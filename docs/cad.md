# CAD modelling (timeline, sketches, features)

AeroCAD models like Autodesk Fusion: a **parametric timeline** of features. Each feature
stores its parameters as JSON; regeneration replays the timeline on Open CASCADE B-rep and
produces **bodies** and **sketches**, which appear in the document as parts
(`document.parts` shows `kind` = `body` / `sketch` / `imported` and the owning `featureId`).

Everything is a command (see `commands.list`), so the UI, scripts and an AI agent (MCP)
use the same operations, and every change is one undo step.

## Concepts

- **Feature id**: returned when a feature is created. A body is identified by the id of the
  feature that created it (or by its name, e.g. `"Body1"`).
- **Parameters**: `parameter.set {"name": "w", "expression": "40 mm"}`. Any number in a
  feature can be an expression: `"w / 2 + 3"`. Units: mm (default), cm, m, in, deg.
  Functions: sqrt, sin, cos, tan (degrees), abs, min, max; constant pi.
- **Operation** (extrude, revolve, box, cylinder): `new_body` (default), `join`, `cut`,
  `intersect`. join/cut/intersect act on `bodies`, or on every body the result touches.
- **Edges and faces** are referenced by index (`body.edges`, `body.faces`) or, more robustly,
  by a point: `{"point": [x, y, z]}`. Fillet, chamfer, shell, press pull, delete face and draft store point references
  with matching hints: the point relative to the body's bounding box (`relative`), the edge
  `direction` or face `normal`, and the `kind` (line, circle, plane, ...). Regeneration
  picks the edge/face that best matches them, so a picked corner edge follows the body
  when a dimension makes it grow. Full persistent naming is not implemented yet.
- **Fillet and chamfer** follow tangent chains (Fusion's "Tangent chain"): one picked edge
  rounds every edge that continues it smoothly. When the radius or distance does not fit,
  the error names the largest one that does ("a 30 mm radius does not fit these edges; the
  largest that works is about 9.5 mm"); the dialog shows it while you type.
- **Shell** with no face picked makes a closed hollow body (Fusion's closed shell): the
  outer walls stay and a cavity of the given wall thickness is cut inside. Bodies with
  fillets shell too, also when the removed face is tangent to a fillet (OCCT's own shell
  cannot open such a face; AeroCAD then cuts the inner offset of the body and opens each
  removed flat face through the wall). When the walls cannot be made on a shape, the error
  says so and suggests a thinner wall; the body is never replaced by an empty or broken
  result.
- **Exact solids outside the timeline** (fins, fitted solids, STEP imports) can be filleted,
  chamfered, shelled and drilled directly in the UI: the dialog makes them editable
  (`feature.base_body`) on the way.
- **Combine with parts outside the timeline**: a tool can be any imported part. An exact
  solid (a fin, a STEP part) is joined, cut or intersected like a body. A surface or a scan
  cuts the target on one side, like Fusion's Split Body keeping one piece: Cut removes what
  is behind the surface (inside the scan, against its triangles' normals), so a part sits
  flush on the car; Intersect keeps only that; `flip` swaps the sides. For a scan, a smooth
  B-spline surface is fitted to the scan around the target (its box plus a margin, at most
  200 000 triangles) and cached, so the cut face is clean, not faceted.
  In the UI: Combine, click the target, then click the tools (bodies, solids, a surface or
  the scan); with a scan or surface selected, "Cut a part to this shape" presets it as the tool.
- **Marker**: `timeline.set_marker` rolls the model back; new features are inserted at it.
- **Edit sketch**: `sketch.begin_edit` (`id`) rolls the timeline back to just after the sketch
  (Fusion: Edit Sketch); `sketch.end_edit` rolls forward again: to the end if the marker was
  there, else to just after the last feature that was active (deleted ones are skipped,
  rolled-back ones stay rolled back). Moving the marker meanwhile (`timeline.set_marker`) is
  the user's choice and ends the edit without rolling forward; deleting the edited sketch
  rolls forward. Saves and variants always store the rolled-forward marker. `timeline.list`
  reports `sketchEdit` ({sketch, restoreTo}) while one is open.
- **Reorder**: `timeline.move_feature` (`id`, `position`) moves a feature in the timeline and
  regenerates; it is refused, with nothing changed, when a feature that worked would fail in
  the new order. A feature moved across the marker becomes rolled back or active.
- **Steps built on a scan** (Part on surface, Fin on surface) are timeline features too: they
  read the scan or fitted surface from the document, so editing their thickness, or the sketch
  before them, rebuilds the part on the same scan; delete, suppress and roll back work as for
  any feature, and they are saved in the project. Fitting the skin takes seconds, so each
  result is cached by its inputs (params, user parameters, the sketch geometry, the content of
  the scan) and an unrelated change, undo or redo does not refit. One feature can make several
  bodies (a stacked or mirrored set of fins): the first has the feature's id, the others
  `id | k << 40`; any of them stands for the feature in `feature.*` commands. On opening a
  project their stored results are used while the scan, sketch and params are unchanged.
  A fit that misses its tolerance is kept and marked as an approximation (CAD-85), except
  when what is left is the scan's own noise (CAD-141): the noise is estimated from the scan
  under the outline (`fit.scanNoise`, one standard deviation, mm), and when 99% of the scan
  is within 3 of it, the rms not above 1.2 of it and the deviations of nearby samples do not
  correlate (no missed ripple or ridge; `fit.withinScanNoise`), the surface runs
  through the middle of the noise. Finer control points would only make it follow the noise,
  so the fit stops there, and it is not called an approximation.
- Failing features: a new or edited feature that fails is rejected (nothing changes).
  Features that break because of an earlier change stay in the timeline with
  `status: "error"` and a message, like Fusion's red timeline icons.

Scaling an imported source or replacing its geometry rebuilds the dependent timeline
bodies before Inspect, saving or export uses them. Undo and Redo restore the source and
its dependent bodies together, in one step. A rejected source edit leaves the model and
history unchanged. If an accepted source change prevents dependent features from
rebuilding, AeroCAD reports the problem; Undo restores the previous source.

## Commands

| Command | Fusion | Notes |
|---|---|---|
| `sketch.create` | Create Sketch | `plane` XY / XZ / YZ / {plane: construction plane id} / {origin, normal, xDir}, or `face` {body, index or point}, or `surface` {part, point / useSelection} (best-fit plane over a curved surface or scan); `offset`; `curves` |
| `sketch.add_curves` | (draw in sketch) | appends curves |
| `sketch.offset` | select a chain, Offset, type distance | `curves` (one connected chain), `distance` (> 0 outwards for a closed chain, to the left of an open one): offset curves with rounded corners added at the end of `sketch.curves` |
| `sketch.project` | in a sketch, Project (P), click a body edge | `sketch`, `body`, `edges` (`[{point}]` or edge references): the edges projected onto the sketch plane as fixed curves (lines, circles/arcs parallel to the plane, otherwise splines), circles and arcs with a radius dimension |
| `sketch.fillet` | Fillet, click a corner | `curves` [a, b] (two lines meeting at a corner), `radius`: both lines are shortened and joined by an arc held tangent with a radius dimension; the new lines and arc go at the end of `sketch.curves` |
| `sketch.mirror` | select curves, Mirror, click a line | `curves`, `line`: copies mirrored about the line, added at the end of `sketch.curves`, with symmetry constraints on their points (and equal radii) |
| `sketch.trim` | Trim (T) | `curve`, `at` [u, v]: the piece of the curve under `at` between its nearest crossings goes (whole curve when it crosses nothing; a circle becomes an arc); the pieces left are added at the end of `sketch.curves`; constraints on the curve are removed |
| `sketch.set_construction` | select sketch curves, X | `curves` (indices from `sketch.curves`), `construction` (default true): construction curves are dashed and bound no profile; part of a rectangle / polygon / slot becomes lines and arcs that keep its constraints; indices do not change |
| `sketch.move_point` | drag a sketch point | `point` {curve, point: start / end / center}, or a spline fit point {curve, point: fit, index}, `to` [u, v]: the point goes there and the solver moves the free degrees of freedom; item parameters are rewritten where the curves are now; refused (no change) when the constraints hold the point |
| `sketch.delete_curves` | select sketch curves, Delete | `curves`: indices from `sketch.curves`; a rectangle, polygon or slot that loses some curves becomes lines and arcs that keep its constraints; constraints on removed curves go, the others (and a revolve axis line or sweep path curves) are re-indexed; a revolve or sweep whose axis or path curve was removed reports an error until it is picked again |
| `sketch.profiles` | | closed regions with area and centroid |
| `sketch.add_constraints` | Sketch > Constraints, Sketch Dimension (D) | see Constraints below |
| `sketch.edit_constraint` | double-click a dimension / delete a constraint | `index`, `value` or `remove` |
| `sketch.curves` | | solved curves, constraints, degrees of freedom |
| `feature.extrude` | Extrude (E) | `distance`, `extent` one_side / symmetric / two_sides / to_object, `to` (to_object: `{body, faces?}` or `{part}`; stops on the target's shape, curved too; an error when it is not reached), `flip`, `taper` (degrees, Fusion Taper Angle: positive narrows away from the sketch plane, holes grow; each side of symmetric / two_sides tapers away from the plane; not with to_object; a taper that closes the profile is an error), `profiles` or `points` |
| `feature.revolve` | Revolve | `axis` X / Y / Z, {line: sketch curve index} or {origin, direction} (the UI sets these from a clicked sketch line or straight body edge); `angle` |
| `feature.sweep` | Sweep | profiles of `sketch` moved along `path` {sketch, curves} (default: all curves of the path sketch) |
| `feature.loft` | Loft | `sections`: sketch ids or {sketch, profile}, in order; `solid`, `ruled` |
| `feature.mirror` | Mirror | `body`, `plane` XY / XZ / YZ / {origin, normal} / planar face, `offset`; join or new_body |
| `feature.move` | Move/Rotate/Scale | `body`, `scale` (uniform factor, default 1), `axis` X / Y / Z or [x, y, z], `angle` (deg); scale and rotation about `center` (default the body's box centre), then `translate` [dx, dy, dz] (Fusion: Move/Copy, M) |
| `feature.rectangular_pattern` | Rectangular Pattern | `direction`, `count`, `spacing`, optional `direction2` / `count2` / `spacing2` |
| `feature.circular_pattern` | Circular Pattern | `axis` X / Y / Z or {origin, direction}, `count`, `angle` (360 = full turn) |
| `feature.plane` | Construct > Offset Plane, Plane at Angle, Midplane, Plane Through Three Points | `base` (XY / XZ / YZ, {plane: id}, planar face {body, point}), `offset`, `angle` + `axis` x / y, `base2` (midplane) or `points` (3 points) |
| `feature.fillet` | Fillet (F) | `edges`, `radius`; tangent chains are rounded with the picked edge |
| `feature.chamfer` | Chamfer | `edges`, `type` equal_distance (default) / two_distances (`distance`, `distance2`) / distance_angle (`distance`, `angle`), `flip` swaps the first face |
| `feature.shell` | Shell | `faces` to remove, `thickness` |
| `feature.press_pull` | Press Pull (Q) | `faces` (flat or curved: a curved face is offset, like Fusion's Offset Face; a fillet face is refused, change its radius instead), `distance` along the outward normal (negative pushes in) |
| `feature.draft` | Draft | `body`, `faces`, `plane` (fixed plane: XY default / XZ / YZ / construction plane / {origin, normal} / planar face; its normal is the pull direction), `angle` in degrees (positive leans the faces in along the pull direction, negative out) |
| `feature.delete_faces` | Delete Face | `body`, `faces`: removes them and heals the gap from the neighbouring faces (a hole, fillet or boss disappears); an error when the neighbours cannot close it |
| `feature.split` | Split Body | `body`, `plane` XY / XZ / YZ / {plane: construction plane} / {origin, normal} / planar face, `offset`; the body keeps what is behind the plane, a new body (the feature's id) gets what is in front |
| `feature.hole` | Hole (H) | `point` on a planar face, `diameter`, `depth` or through all |
| `feature.combine` | Combine, Split Body | `target`, `tools` (bodies, or imported parts `{part: id}`), join / cut / intersect, `keepTools`, `flip` |
| `feature.box`, `feature.cylinder` | Box, Cylinder | primitives with an operation |
| `feature.sphere`, `feature.torus` | Sphere, Torus | `center` (default origin), `diameter`; torus: ring `diameter` (centre line, around +Z) and `tube_diameter` (smaller than `diameter`); with an operation |
| `feature.base_body` | Mesh to BRep / base feature | imported part as a body: B-rep as is, meshes as faceted B-rep (coplanar triangles merged, max 50 000 triangles) |
| `surface.part_from_sketch` → `part_on_surface` | (AeroCAD) Part on surface | timeline step built on a scan or fitted surface: `sketch`, `thickness`, `side`, `profiles`, `tolerance`, `id` (surface part, default: the one the sketch was drawn on) |
| `surface.fin_from_sketch` → `fin_on_surface` | (AeroCAD) Fin on surface | timeline step: `sketch` (drawn across the surface), `thickness`, `rootFillet`, `count`, `spacing`, `mirror`, `mirrorPlane`; one body per fin |
| `feature.edit` / `rename` / `suppress` / `delete` | timeline context menu | edit = JSON merge patch of params |
| `timeline.list`, `timeline.set_marker`, `timeline.move_feature` | timeline | |
| `parameter.set` / `remove` | Change Parameters | |
| `body.faces`, `body.edges` | | for picking by index or point; `point` adds the nearest edge / face (and the face normal there), `before` lists the body as it is before a feature (rolled back, as when editing it) |

Sketch curves: `line {start, end}`, `rectangle {corner, width, height | opposite}`,
`center_rectangle {center, width, height}`, `circle {center, radius | diameter}`,
`arc {start, through, end}`, `center_arc {center, radius, startAngle, endAngle}`,
`polygon {center, radius, sides, angle?}`, `slot {start, end, width}`,
`spline {points, closed?}`; any curve can be `construction: true`. Curves are split at
intersections, so a circle inside a rectangle gives two profiles (ring and disk).

### Constraints and dimensions

A sketch can carry `constraints` (in `sketch.create` or via `sketch.add_constraints`). They
refer to curves by index in `sketch.curves` (a rectangle is four lines in order bottom,
right, top, left; a slot is line, arc, line, arc) and to points as
`{"curve": i, "point": "start" | "end" | "center"}` or `"origin"`.

- Geometric: `coincident`, `point_on_curve`, `midpoint`, `horizontal`, `vertical`,
  `parallel`, `perpendicular`, `collinear`, `tangent`, `equal`, `concentric`, `symmetric`, `fix`.
- Dimensions (`value` is a number or an expression): `distance` (two points, or a point and a
  line), `horizontal_distance`, `vertical_distance`, `length`, `radius`, `diameter`, `angle`.

The solver (Levenberg-Marquardt, `src/cad/src/SketchSolver.cpp`) starts from the drawn
curves and moves them as little as possible. Curve ends that touch are merged, so outlines
stay closed. Shapes keep their implicit constraints: rectangle sides stay horizontal and
vertical, polygon sides equal, slot ends tangent. Conflicting constraints are rejected with
the index of the failing constraint. `degreesOfFreedom` (0 = fully constrained) is returned
by `sketch.create`, `sketch.add_constraints` and `sketch.curves`.
`fix` holds points where they are when the constraint is added (`sketch.add_constraints`
stores the solved positions in `at`; a fixed circle also keeps its radius). Lines that must
turn far (horizontal on a vertical line, perpendicular on parallel lines) are turned in the
start guess, so they rotate instead of collapsing.

```json
{"name": "sketch.add_constraints", "params": {"sketch": 1, "constraints": [
  {"type": "coincident", "points": [{"curve": 0, "point": "start"}, "origin"]},
  {"type": "length", "curve": 0, "value": "w"},
  {"type": "length", "curve": 1, "value": 20}]}}
```

Sketch planes (Z up): XY is the ground plane (normal +Z), XZ the front plane (normal -Y,
sketch v = world Z), YZ the right plane (normal +X, sketch v = world Z). A reference frame
created with `frame.set` can be used by name. Saving, variants and frames: see
[project-format.md](project-format.md).

## Example (what an agent would send)

```json
{"name": "parameter.set", "params": {"name": "w", "expression": "40"}}
{"name": "sketch.create", "params": {"plane": "XY", "curves": [
  {"type": "center_rectangle", "center": [0, 0], "width": "w", "height": "w / 2"}]}}
{"name": "feature.extrude", "params": {"sketch": 1, "distance": 10}}
{"name": "sketch.create", "params": {"face": {"body": 2, "point": [0, 0, 10]}, "curves": [
  {"type": "circle", "center": [0, 0], "diameter": 8}]}}
{"name": "feature.extrude", "params": {"sketch": 3, "distance": 10, "flip": true, "operation": "cut"}}
{"name": "feature.fillet", "params": {"body": 2, "edges": [{"point": [20, 10, 5]}], "radius": 2}}
```

## Not yet

- Constraint inference while drawing (Fusion adds coincident/horizontal/... automatically);
  today the UI adds them explicitly.
- Full persistent topological naming (references use index, or a point with matching hints).
- Extrude "to object" / up to a surface.
- Dense scans as bodies: `feature.base_body` refuses meshes above 50 000 triangles; those need
  region selection and surface fitting (CAD-67).
