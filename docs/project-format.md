# Projects, autosave and variants

## The `.aerocad` file

A project is one file: a zip container (deflate) with

| Entry | Content |
|---|---|
| `manifest.json` | Versioned JSON manifest, always the first entry. |
| `meshes/<part id>.amesh` | Imported meshes, lossless: magic `AEROMESH`, uint32 version 1, uint64 vertex count, uint64 triangle count, vertices as 3 × float64, triangles as 3 × uint32, little-endian. |
| `selections/<part id>.u32`, `selections/timeline-<part id>.u32` | Region selection of an imported or a timeline part (CAD-13): triangle indices, little-endian uint32. Selections on bodies are restored only if they still fit the regenerated mesh. |
| `shapes/<part id>.brep` | Exact geometry of imported parts (STEP, IGES, BREP) in the Open CASCADE BREP text format. |
| `results/<feature id>-<k>.brep` | Bodies of timeline steps built on a scan (Part on surface, Fin on surface), see `timelineResults`. |

Bodies and sketches are **not** stored: they are regenerated from the timeline when the
project is opened, so a reopened project stays fully parametric (edit a parameter or a
feature, export again).

Manifest (version 1):

```json
{
  "format": "aerocad.project",
  "version": 1,
  "generator": "AeroCAD 2026.11.07-0747 (1a2b3c4)",
  "savedAt": "2026-11-06T14:00:00Z",
  "units": "mm",
  "upAxis": "Z",
  "parts": [{"id": 1, "kind": "imported", "name": "scan", "source": "/path/scan.stl",
             "visible": true, "mesh": "meshes/1.amesh", "brep": "shapes/1.brep",
             "selection": "selections/1.u32"}],
  "timelineParts": [{"feature": 2, "kind": "body", "id": 3, "name": "Wing", "visible": true}],
  "design": {"features": [...], "parameters": [...], "frames": [...], "nextId": 4, "marker": 3},
  "variants": {"active": 1, "previous": 2,
               "items": [{"id": 1, "name": "A"}, {"id": 2, "name": "B", "design": {...}}]},
  "view": {"pivot": [0, 6, 0], "pitch": -30, "yaw": 45, "distance": 140, "orthographic": false},
  "originalPath": "/path/wing.aerocad"
}
```

- `design` is the state of the active variant: features (id, type, name, params,
  suppressed), user parameters (name, expression, comment), reference frames, the next
  feature id and the timeline marker. Inactive variants carry their own `design`.
- `timelineResults` (optional) stores the bodies of timeline steps built on a scan, with the
  fingerprint (`key`) of their inputs: params, user parameters, sketch geometry and the scan's
  content. On open a step whose key still matches uses them instead of fitting the scan again;
  anything else (no entry, another key, unreadable data) is rebuilt. Only the active variant's
  results are stored. Each body has `name`, `brep` and, for a step with several bodies,
  `slot`: its stable place (a fin's place in the stack, mirrored fins from 1000), which gives
  the body's id.
- An imported part may carry `color` (Appearance, `#rrggbb`) and `importUnits`: the unit a
  unitless file (STL, OBJ, PLY, BREP) was read in when it was not mm, so the size check can
  say "imported as metres, saved in millimetres" (CAD-239).
- `timelineParts` keeps the part ids, names and visibility of bodies and sketches, so ids
  that scripts or an agent hold stay valid after reopening.
- `view` is the camera, opaque to the format (written by the UI). While a part is isolated
  the UI also stores `view.isolation` (`id`, `name`, `hidden`: the ids Isolate hid), so after
  reopening Unisolate still brings those parts back (CAD-224).
- `originalPath` is set only on copies (autosave): the project the copy belongs to.

**Versioning.** Readers accept every version up to their own and refuse newer files with
"saved by a newer AeroCAD ... Please update". Every change of the manifest that an older
reader would misread bumps `version`; additive optional fields do not.

**Atomic save.** The file is written to a temporary file in the same directory, flushed
to disk (`fsync` / `FlushFileBuffers`), then renamed over the target (`rename` /
`MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`). An interrupted save leaves the previous
file untouched; a failed save removes the temporary file.

## Commands

| Command | Notes |
|---|---|
| `project.info` | Path (empty = untitled), name, `dirty`, counts, active variant. |
| `project.new` | Empty untitled project; clears undo. Does not ask: check `dirty` first. |
| `project.save {path?, copy?, view?}` | Without `path`: the current file. `.aerocad` is appended when there is no extension. `copy: true` writes a copy (autosave) and keeps path and unsaved state. |
| `project.open {path, recover?}` | Replaces the project (a failing open changes nothing). Returns `view` and `featureErrors`. `recover: true` opens an autosave copy as unsaved with its original path. |
| `variant.list` | Variants with their parameter expressions. |
| `variant.create {name?, from?, activate?}` | Duplicates the active (or `from`) design as a new variant (B, C, ...), activated by default. |
| `variant.activate {variant}` / `variant.toggle` | Switch variants (id or name); toggle goes back to the previous one. |
| `variant.compare {a?, b?}` | Parameter and feature differences (JSON patch per feature). |
| `variant.rename`, `variant.delete` | Delete works on inactive variants. |
| `frame.set {name, origin?, normal?, xDir?}`, `frame.list`, `frame.remove` | Reference frames (work planes); `sketch.create {"plane": "<frame name>"}` sketches on one. |
| `part.duplicate {id, name?}` | Copies a part; a timeline body becomes a fixed solid (kind imported). Not for sketches or planes. |

All modifying commands are undoable, including variant switches (undo walks back through
switches and the edits made in each variant).

In the desktop app, replacing the project or changing the active variant closes feature
and sketch editors and clears picks and tool panels tied to the previous context. This
also applies to commands from MCP, command scripts and Undo/Redo. A failed open keeps the
current editing context. Opening a project restores its saved camera; variant switches
keep the current camera.

## Variants (A/B)

A variant is an alternative version of the design timeline (features, parameters,
frames). One is active and receives all edits. Imported parts are reference data shared
by all variants. Parts of features that exist in both variants keep their ids, so the
viewport updates them in place and the camera does not move when switching.

UI: toolbar group VARIANT (variant list, **A/B**, **New**) and Modify > Duplicate as New
Variant / Toggle A/B. The window title shows the active variant when there are several.

## Autosave and recovery

While there are unsaved changes, the app writes a copy every 120 s (QSettings
`project/autosaveSeconds`, 0 = off) to `<app data>/autosave/<name>-<hash>.aerocad`
(Linux `~/.local/share/FlappedEar/AeroCAD/autosave`, macOS
`~/Library/Application Support/FlappedEar/AeroCAD/autosave`). Ctrl+Shift+S (Fusion's
Recovery Save) writes it immediately. A lock file marks copies owned by a running
instance. Saving, opening, New Design and a normal exit remove the copy; after a crash the
next start offers to recover it, naming the design and the time of the copy ("Recover the
unsaved changes to “wing”? Recovery copy from 9 Nov 2026 12:00."). Copies store the current
view (camera, isolation), so a recovered project opens as it was on screen (CAD-226,
[question](images/audit/cad-226-recovery-question.png),
[recovered view](images/audit/cad-226-recovery-restored.png)).

Recent projects: File > Open Recent (last 10, QSettings `project/recent`).

## Not yet

- Calibration data (scan scale / alignment) has no place in the manifest yet; reference
  frames cover alignment for now.
- The original imported file is not embedded (only its path in `source`); the mesh is
  stored at full resolution as imported (and after `part.scale`).
- No zip64: projects must stay below 4 GB.
