# The golden journey

The one path through AeroCAD that must always work, because it is what the product is for:

1. Import a scan (a bumper or a whole car: STL, OBJ, PLY, 3MF, STEP).
2. Draw the outline of a part on the scan (Sketch on the scan, rectangle or any outline, Finish Sketch).
3. Part on surface: the part is created, visible and selected; it stands out of the scan by default.
4. Change the part (Edit step: thickness, direction, Shape the top: ridge height, fade-out).
5. Design on the part: sketch on its curved face (Sketch on a face, click the part), extrude the outline, round its edges.
6. Give it its aero shape: a ridge or lip with a fade-out, not only a constant thickness.
7. Wind tunnel: set up a run for the part (with the scan as context if wanted).
8. Export check, then export for printing (STL and STEP per solid).
9. Save and reopen: the same part comes back.

`tests/gui/golden_journey.ui` drives this with the user's own tools (real mouse and key events for
the outline, Create, Edit step, Sketch on a face, Export for printing) and commands for the checks
in between. Run it with ctest (`ctest -L golden`, under Xvfb on Linux) or by hand:

```sh
scripts/golden-journey.sh [build-dir] [scan.stl]     # default: build/release, tests/data/models/dome_scan.stl
```

Each step prints `GJ <n> <name>: PASS`, `FAIL <why>` or `GAP <issue>`. FAIL fails the test.
GAP is a step the product cannot do yet; the issue says which work closes it. When that work
merges, change the step in the script from a gap to a hard check (all steps are hard checks now).

No step is a gap now; a step the product cannot do again prints GAP and the issue to close.

Known defect the script works around (drop the workaround when fixed): finishing a sketch drawn on
the part offers Part on surface again (CAD-296).

Run it on a real bumper scan before a release: `python3 -I <gen_bumper.py> bumper.stl 2.5` makes a
295k-triangle one (see the project files), then `scripts/golden-journey.sh build/release bumper.stl`.
The usability gate CAD-37 (a person does the whole journey unaided in 15 minutes) is measured by
hand against this list; the baseline on 2026-11-14 was about 9 user actions from import to the part.
