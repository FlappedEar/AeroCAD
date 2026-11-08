# CAD-95 recovered-output diagnosis

WIP diagnosis by Codex, 8 November 2026. No production geometry fix or acceptance claim.
Base: `52233eae20b33bc1e34a2b1ac8586484c8425bcf`. Python OCP 7.8.1.1, NumPy,
80-digit Decimal certificates. No application rebuild was needed for this diagnosis.

## Preserved inputs

The original synthetic CAD-37 journey is available in
[Jira attachment 10031](https://kozucharkadiusz.atlassian.net/secure/attachment/10031/CAD-37-synthetic-journey-evidence.zip).
Its original application candidate was `d855fde37895939e6aa9c1196ae45141eb3a8d0c`.
The recovered project, four STL files, four STEP files, input scan and GUI replay were
not changed. The four STL SHA-256 values match the original manifest.

The project contains saved BREP bodies `results/2-0.brep` through `results/2-3.brep`.
The probes read these original bodies directly. Parameters remain two stacked fins,
spacing -55 mm, mirror enabled, final thickness 4 mm, root fillet 2 mm, and the original
upstream outline extension. This is analysis of recovered outputs, not a fresh GUI replay.

## Observations (high confidence)

1. All four original STL files have positive-length non-coplanar crossing witnesses.
   `stl-witnesses.json` contains the original coordinates, zero-based triangle indices,
   file hashes, and Decimal intersection segments. This narrow probe is not an exhaustive
   validator; a missing witness would not mean a valid mesh.
2. Re-tessellating the saved BREP with the production export parameters
   (relative linear deflection 0.0002; angular deflection 0.15) reproduces every STL
   vertex coordinate exactly after float32 conversion, including triangle order.
3. Three selected witnesses already cross in the OCCT double-precision triangulation.
   One selected witness appears only after float32 STL conversion. The latter does not
   establish that the rest of that body is valid.

| Original STL | Witness triangles | STL crossing length, mm | Same source-double pair, mm |
| --- | --- | ---: | ---: |
| Sketch1 fin 1 | 143 / 3758 | 0.0027703814 | 0.0026980310 |
| Sketch1 fin 2 | 103 / 8182 | 0.0007826649 | 0.0018122557 |
| Sketch1 fin 1 (mirror) | 3891 / 7591 | 0.0093151055 | 0.0095201650 |
| Sketch1 fin 2 (mirror) | 3339 / 3652 | 0.0057258160 | No crossing for this pair |

4. All four saved BREP bodies pass `BRepCheck_Analyzer`, but
   `BOPAlgo_ArgumentAnalyzer` with `SelfInterMode` reports face-face self-interference:
   respectively 2, 4, 2 and 2 records. The checker reports no execution errors.
   Face pairs are recorded in `brep-diagnosis.json`; pairs 3/11 and 7/11 recur.
5. A separate section of faces 7/11 of mirrored fin 1 produces a 0.5959617590 mm
   curve in addition to curves close to their shared edge. A sampled point is
   0.0016310030 mm from the shared edge, whose tolerance is 0.0000505810 mm.
   That point projects inside both trimmed faces (classification tolerance 1e-7),
   with surface projection residuals about 1.84e-7 mm. See `face-section.json`.
   This is source BREP interference evidence independent of STL triangle conversion.
6. Other mapped mesh witnesses project outside one source face's trim and inside the
   adjacent face. Their shared-edge tolerances are recorded. These observations do not
   identify which construction operation first introduced the problem.

## Consequence and smallest next experiment

A tessellation-only or STL-precision-only repair is insufficient for this recovered
workflow: source BREP interference is independently present. The exact construction
stage remains unproven. The initial CAD-37 topology/volume readers did not establish
absence of intersections; their earlier success must not be treated as CAD-95 acceptance.

The smallest next production-fix experiment is to instrument **one unchanged mirrored
fin** through `filletedFin` in `src/geometry/src/Offset.cpp`: capture the pad/blank fuse,
fillet result, and pad-cut result; run the same self-interference/face-section diagnosis
at each stage, and use boolean history to track the first faulty face pair. Repair that
first construction/trim step while retaining the requested 2 mm root fillet and 4 mm
thickness, then re-run the original four-fin export and triangle checks. A fail-closed
postcondition can prevent acceptance of a bad result, but is not the geometry repair.
Do not loosen intersection predicates, raise tolerances, discard crossing triangles,
change the fixture, or claim CAD-95 fixed from this checkpoint.

## Reproduction

Requires NumPy and the `cadquery-ocp` Python package. From the repository root:

```sh
CAD95_RECOVERED=/path/to/extracted/original/evidence
python3 tools/diagnose_fin_mesh.py "$CAD95_RECOVERED/candidate/export" --output /tmp/cad95-stl.json
python3 tools/diagnose_fin_brep.py "$CAD95_RECOVERED/candidate/canards.aerocad" "$CAD95_RECOVERED/candidate/export" /tmp/cad95-stl.json --output /tmp/cad95-brep.json
python3 tools/diagnose_fin_brep.py "$CAD95_RECOVERED/candidate/canards.aerocad" "$CAD95_RECOVERED/candidate/export" /tmp/cad95-stl.json --body 'Sketch1 fin 1 (mirror)' --section-faces 7 11 --output /tmp/cad95-section.json
```

The probes write only the requested output files. No real-vehicle acceptance, physical
printing result, new macOS GUI run, or complete CAD-32 validator is claimed here.
