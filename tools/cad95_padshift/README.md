# CAD-95 shifted-cutter experiment

Diagnostic only; this is not an accepted geometry fix. The captured mirrored-fin
fillet and pad are in Jira CAD-95 attachment 10047 (`CAD-95-stage-diagnosis.zip`).
After extracting that ZIP, run:

```
python tools/cad95_padshift/probe.py /path/to/extracted-evidence /path/to/output
```

The script needs NumPy and cadquery-ocp. It cuts the captured fillet with its pad
translated +/-0.001 mm in X, checks the largest resulting solid with Open CASCADE's
self-interference analyzer, then triangulates it at the production deflection and
uses an independent high-precision crossing witness. The two mesh-*.json files are
the original crossing certificates. Absence of a witness is not a mesh-validity
certificate; these probes do not check coplanar intersections.

Both largest solids had zero analyzer reports but retained certified mesh crossings:
0.0059203 mm for +0.001 and 0.0152217 mm for -0.001. The +0.001 cut generated
an additional 3.54 mm³ solid. The displacement also changes root contact. These
observations reject the experiment and show that the BREP checker alone cannot
certify mesh safety.
