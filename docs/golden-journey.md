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

| Step | State | Issue |
|---|---|---|
| 7 aero shape (ridge, lip, fade-out) | PASS | CAD-24, CAD-25 (Edit step > Shape the top: ridge and fade-out, step 4b in the script) |

Run it on a real bumper scan before a release: `python3 -I <gen_bumper.py> bumper.stl 2.5` makes a
295k-triangle one (see the project files), then `scripts/golden-journey.sh build/release bumper.stl`.

## Timed unaided run (CAD-37)

The gate: a person who has not used AeroCAD does the whole journey above, unaided, in 15 minutes.
The script proves the path works; only a person can show it is findable. A tester needs a build
(`git pull`, build, or the app bundle), a scan (the bumper above or their own) and a timer.

Protocol:

1. Give the tester only the goal: "Make an aero part on this bumper scan, check it in the wind tunnel, export it for printing, then save the project, reopen it and show me the part is still there." No demo, no hints. Start the timer when the scan is opened.
2. The observer says nothing, and notes every point where the tester stops, asks, opens a wrong menu or undoes. Count user actions (clicks, key presses, drags) from import to each step.
3. Stop the timer when the saved project has been reopened and the same part is back, or at 15 minutes. Record the result below, with the build date and git hash from the About box.
4. Every stop of more than 30 seconds becomes a Jira issue (parent CAD-65, label ux), and is fixed before the next run.

| Date | Tester | Scan | Time to reopen | Actions to the part | Stops (what and where) | Build |
|---|---|---|---|---|---|---|
| 2026-11-14 (script, not a person) | none | generated bumper | not timed | about 9 | none (script) | main at CAD-294 |

No unaided run by a person is recorded yet, so CAD-37 stays open.
