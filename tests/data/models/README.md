# Test model set

The same models in every import format AeroCAD reads from meshes and CAD files (CAD-22).
`tests/e2e/test_workflow.cpp` runs the whole workflow on each file: import, region
selection, base surface, modelling, export, save, reopen and export again (CAD-23).

| Model | Files | What it is |
| --- | --- | --- |
| `dome` | `dome.step`, `dome.stl`, `dome.3mf`, `dome.obj` | A revolved profile, 240 mm across and 45.7 mm tall: a smooth, doubly curved top (like a body panel or a mirror housing), a 15 mm vertical rim and a flat bottom. About 2000 triangles in the mesh formats. |

All files are in millimetres. STEP, STL and 3MF come from AeroCAD's exporters; the OBJ is
written from the STL triangles by the generator, so the OBJ reader is tested on a file
that AeroCAD's own code did not write.

Regenerate after changing a model (the files are committed, CI does not run this):

```sh
cmake --build build/dev --target aerocad_cmd
tools/make_test_models.py --cmd build/dev/bin/aerocad-cmd
```
