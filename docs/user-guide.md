# User guide

The step-by-step guide is the **[tutorial](manual/golden-path.md)**: load a scan, draw an outline,
create the part, sketch on its curved face, shape it, run the wind tunnel and export. The
[window and navigation](manual/interface.md), the [shortcuts](manual/shortcuts.md) and
[troubleshooting](manual/troubleshooting.md) are next to it.

The older, longer guide on Confluence
([AeroCAD User Guide](https://kozucharkadiusz.atlassian.net/wiki/spaces/AE/pages/50429953/AeroCAD+User+Guide))
is no longer kept up to date; this site is.

For AI agent access, see [AI agent access (MCP)](mcp.md): explicitly grant access to the
project shown in the panel. New, Open, recovery and application exit end that grant.

## Import units

In **MESH > Scan Tools**, **Unitless files in** selects the source unit for STL, OBJ,
PLY and BREP files. Choose it before importing; the setting is remembered.
STEP, IGES and 3MF use their own file units automatically. Their dimensions are
converted to millimetres once, regardless of the last unitless-file setting, and the
status bar reports **file units → mm**. For an incorrectly declared source file,
use **Convert**, **Calibrate**, or **Scale** explicitly after import.
See [Working with scans](scans.md#units-and-scale) for the command/API contract.

## Editing a fitted surface workflow

Fit Surface, Fit Patches, Cut Out by Sketch, Offset Surface and Thicken each add a
separate timeline step. Double-click a step to edit its parameters. For example, a
2 mm offset defines the clearance and a following 3 mm thicken defines the wall;
changing either value preserves the other. Later steps rebuild from the updated
surface. Editing the trimming sketch also rebuilds the trimmed surface and its
descendants.

A fit remembers the selected triangles when it is created. Clearing or changing
the current selection does not change that fit. Moving or scaling the source
rebuilds its dependent surfaces. If the source mesh topology changes, a fit of a
saved selection reports an error instead of silently fitting different triangles;
select the intended region and create a replacement fit. Whole-mesh Fit Patches
does not depend on triangle indices.

These steps support undo/redo, suppression, timeline rollback, project save/open
and independent variants. The source scan stays an imported reference. Older
projects containing baked surface results show **Frozen surface** or **Frozen
solid** in the browser: their original fit, clearance and thickness parameters
were not stored and cannot be recovered. New timeline operations can use such a
frozen result as a reference, but its missing history is not recreated.
