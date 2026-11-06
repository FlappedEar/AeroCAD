# Spike CAD-66: mesh region → B-rep surface

Throwaway prototype for Jira CAD-66. It answers one question: how do we turn a
user-selected region of an imperfect mesh into a B-rep surface in Open CASCADE
that F360-style tools (sketch, extrude, thicken, boolean, fillet) can build on and
that exports to STEP.

This is not application code. CAD-67 will implement the chosen approach properly.

## Build and run

Requires Open CASCADE (tested with 7.6.3 from Ubuntu packages) and CMake 3.20+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && ctest --output-on-failure     # synthetic case, ~45 s
./mesh_to_brep part.stl cx cy cz r        # your own STL, selection = sphere at (cx,cy,cz), radius r
```

The program writes `spike_part.step` and `spike_part.stl` to the working directory
and exits with 0 only if the whole CAD chain produced a valid solid.

## What it does

Input: a synthetic freeform surface, 120 × 120 mm, 180k triangles, Gaussian noise
σ = 0.1 mm on every coordinate. The selection is a sphere of radius 30 mm
(17,428 points, 34,271 triangles), standing in for the UI brush.

| | Approach | OCCT API |
|---|---|---|
| A | Faceted B-rep: one planar face per triangle, sewn | `BRepBuilderAPI_MakePolygon`, `BRepBuilderAPI_Sewing` |
| B | NURBS fit: PCA plane, resampled height grid, B-spline approximation | `GeomAPI_PointsToBSplineSurface` (tolerance mode and smoothing mode) |
| C | Plate surface with point constraints, converted to B-spline | `GeomPlate_BuildPlateSurface`, `GeomPlate_MakeApprox` |

The best B surface then goes through: trim to selection (`BRepAlgoAPI_Common` with a
cylinder) → thicken 2 mm (`BRepOffsetAPI_MakeThickSolid`) → rectangle sketch
extruded "up to surface" (`BRepPrimAPI_MakePrism` + `BRepAlgoAPI_Splitter`) → fuse →
fillet 1.5 mm → STEP and STL export.

Each fitted surface is checked both ways: scan → surface (projection) and
surface → scan (surface samples to nearest scan point). The second check catches
wiggles and folds that the first one misses.

## Results (OCCT 7.6.3, Linux x86-64, one run)

| Approach | Time | Size | Scan→surface max / RMS | Surface→scan max | vs. noise-free truth |
|---|---|---|---|---|---|
| A faceted | sew 11.2 s, one boolean split 2.7 s | 34,271 faces | exact (copies noise) | – | – |
| B tolerance 25², tol 0.5 | 63 ms | 18×7 poles | 0.57 / 0.13 mm | 1.57 mm | 1.75 mm |
| B tolerance 30², tol 0.25 | 276 ms | 32×9 poles | 0.47 / 0.11 mm | **17.5 mm** | **18.1 mm** |
| B tolerance 40², tol 0.1 | 1.9 s | 42×39 poles | 0.66 / 0.11 mm | 1.35 mm | 1.55 mm |
| **B smoothing 30², tol 0.25** | **15 ms** | 51×9 poles | 0.48 / 0.11 mm | **0.43 mm** | **0.12 mm** |
| B smoothing 40², tol 0.1 | 26 ms | 99×9 poles | 0.43 / 0.11 mm | 0.44 mm | 0.12 mm |
| C GeomPlate, 406 points | 40 ms | 15×15 poles | 1.03 / 0.21 mm | 0.78 mm | 0.81 mm |

CAD chain on "B smoothing 30²": trim 357 ms, thicken 38 ms, extrude-to-surface
split 143 ms, fuse 1.7 s, fillet 26 ms, export 212 ms. Every intermediate shape
passed `BRepCheck_Analyzer`. Final part: 14 faces, 8,823 mm³.

## Findings

1. **Faceted B-rep is not a modelling base.** It is valid B-rep, but one region
   gives tens of thousands of faces, sewing alone takes seconds, and there is no
   smooth geometry for offset or fillet.
2. **The smoothing (variational) mode of `GeomAPI_PointsToBSplineSurface` is the
   best of what OCCT ships.** It is fast and lands within about the noise level of the
   true surface.
3. **Tolerance mode is unstable on noisy, resampled data.** One setting (30², 0.25 mm)
   produced an 18 mm excursion while scan→surface still looked fine. Any fit must be
   validated both ways before the user gets it.
4. **GeomPlate works but is less accurate here** (untuned). It suits hole filling and
   patches bounded by curves better than free fitting.
5. **Gotchas met:** `MakeThickSolidBySimple` can return an inside-out solid (negative
   volume, fixed by `Reverse()`); booleans against the fitted face are the slowest step
   (`SetRunParallel` and a small fuzzy value help).

## Not covered

- Real scans with holes, outliers, or uneven density.
- Regions that are not a height field over one plane (for example a cylinder wrapped
  more than about 180°). Those need splitting into patches, or a mesh
  parameterization plus a fitter that takes (u, v) per point.
  `GeomAPI_PointsToBSplineSurface` only takes a regular grid.
- Detecting analytic primitives (plane, cylinder, sphere, cone, torus) before
  free-form fitting.
- A trim curve fitted to the real selection border. Here a cylinder does the trimming.
