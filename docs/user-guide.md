# User guide

The task-oriented user guide (import a scan, select a region, fit a surface, design a part
on a curved surface, CAD tools, export, projects and variants) is on Confluence:
[AeroCAD User Guide](https://kozucharkadiusz.atlassian.net/wiki/spaces/AE/pages/50429953/AeroCAD+User+Guide).

The screenshots it uses are in [images/guide](images/guide) and [images/ui](images/ui).

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
