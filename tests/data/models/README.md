# Test model set

The same models in every import format AeroCAD reads from meshes and CAD files (CAD-22).
`tests/e2e/test_workflow.cpp` runs the whole workflow on each `dome` file, and the scan part of it on `dome_scan`: import, region
selection, base surface, modelling, export, save, reopen and export again (CAD-23).

| Model | Files | What it is |
| --- | --- | --- |
| `dome` | `dome.step`, `dome.stl`, `dome.3mf`, `dome.obj` | A revolved profile, 240 mm across and 45.7 mm tall: a smooth, doubly curved top (like a body panel or a mirror housing), a 15 mm vertical rim and a flat bottom. About 2000 triangles in the mesh formats. |
| `dome_scan` | `dome_scan.stl` | The top of the same dome as a scanner sees it: open (no rim or bottom), a 3 mm grid with a ragged outline, 0.05 mm noise and a 16 mm hole. About 9000 triangles. |

All files are in millimetres. STEP, STL and 3MF come from AeroCAD's exporters; the OBJ and
`dome_scan.stl` are written by the generator itself (the OBJ from the STL triangles), so
the readers are also tested on files that AeroCAD's own code did not write.

Regenerate after changing a model (the files are committed, CI does not run this):

```sh
cmake --build build/dev --target aerocad_cmd
tools/make_test_models.py --cmd build/dev/bin/aerocad-cmd
```
