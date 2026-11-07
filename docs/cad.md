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
  by a point: `{"point": [x, y, z]}`. Fillet, chamfer and shell store point references
  with matching hints: the point relative to the body's bounding box (`relative`), the edge
  `direction` or face `normal`, and the `kind` (line, circle, plane, ...). Regeneration
  picks the edge/face that best matches them, so a picked corner edge follows the body
  when a dimension makes it grow. Full persistent naming is not implemented yet.
- **Fillet and chamfer** follow tangent chains (Fusion's "Tangent chain"): one picked edge
  rounds every edge that continues it smoothly. When the radius or distance does not fit,
  the error names the largest one that does ("a 30 mm radius does not fit these edges; the
  largest that works is about 9.5 mm"); the dialog shows it while you type.
- **Exact solids outside the timeline** (fins, fitted solids, STEP imports) can be filleted,
  chamfered, shelled and drilled directly in the UI: the dialog makes them editable
  (`feature.base_body`) on the way.
- **Marker**: `timeline.set_marker` rolls the model back; new features are inserted at it.
- Failing features: a new or edited feature that fails is rejected (nothing changes).
  Features that break because of an earlier change stay in the timeline with
  `status: "error"` and a message, like Fusion's red timeline icons.

## Commands

| Command | Fusion | Notes |
|---|---|---|
| `sketch.create` | Create Sketch | `plane` XY / XZ / YZ / {plane: construction plane id} / {origin, normal, xDir}, or `face` {body, index or point}, or `surface` {part, point / useSelection} (best-fit plane over a curved surface or scan); `offset`; `curves` |
| `sketch.add_curves` | (draw in sketch) | appends curves |
| `sketch.profiles` | | closed regions with area and centroid |
| `sketch.add_constraints` | Sketch > Constraints, Sketch Dimension (D) | see Constraints below |
| `sketch.edit_constraint` | double-click a dimension / delete a constraint | `index`, `value` or `remove` |
| `sketch.curves` | | solved curves, constraints, degrees of freedom |
| `feature.extrude` | Extrude (E) | `distance`, `extent` one_side / symmetric / two_sides, `flip`, `profiles` or `points` |
| `feature.revolve` | Revolve | `axis` X / Y / Z, {line: sketch curve index} or {origin, direction}; `angle` |
| `feature.sweep` | Sweep | profiles of `sketch` moved along `path` {sketch, curves} (default: all curves of the path sketch) |
| `feature.loft` | Loft | `sections`: sketch ids or {sketch, profile}, in order; `solid`, `ruled` |
| `feature.mirror` | Mirror | `body`, `plane` XY / XZ / YZ / {origin, normal} / planar face, `offset`; join or new_body |
| `feature.rectangular_pattern` | Rectangular Pattern | `direction`, `count`, `spacing`, optional `direction2` / `count2` / `spacing2` |
| `feature.circular_pattern` | Circular Pattern | `axis` X / Y / Z or {origin, direction}, `count`, `angle` (360 = full turn) |
| `feature.plane` | Construct > Offset Plane, Plane at Angle, Midplane, Plane Through Three Points | `base` (XY / XZ / YZ, {plane: id}, planar face {body, point}), `offset`, `angle` + `axis` x / y, `base2` (midplane) or `points` (3 points) |
| `feature.fillet` | Fillet (F) | `edges`, `radius`; tangent chains are rounded with the picked edge |
| `feature.chamfer` | Chamfer | `edges`, `type` equal_distance (default) / two_distances (`distance`, `distance2`) / distance_angle (`distance`, `angle`), `flip` swaps the first face |
| `feature.shell` | Shell | `faces` to remove, `thickness` |
| `feature.hole` | Hole (H) | `point` on a planar face, `diameter`, `depth` or through all |
| `feature.combine` | Combine | `target`, `tools`, join / cut / intersect, `keepTools` |
| `feature.box`, `feature.cylinder` | Box, Cylinder | primitives with an operation |
| `feature.base_body` | Mesh to BRep / base feature | imported part as a body: B-rep as is, meshes as faceted B-rep (coplanar triangles merged, max 50 000 triangles) |
| `feature.edit` / `rename` / `suppress` / `delete` | timeline context menu | edit = JSON merge patch of params |
| `timeline.list`, `timeline.set_marker` | timeline | |
| `parameter.set` / `remove` | Change Parameters | |
| `body.faces`, `body.edges` | | for picking by index or point |

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
- Extrude "to object" / up to a surface, loft, sweep, thicken, offset surface (CAD-69..71).
- Dense scans as bodies: `feature.base_body` refuses meshes above 50 000 triangles; those need
  region selection and surface fitting (CAD-67).
