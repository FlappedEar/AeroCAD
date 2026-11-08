# Wind tunnel validation: Ahmed body

The `external-flow-v1` preset (see [cfd.md](cfd.md)) is checked against a case with
published experimental data before it is used on parts (CAD-52). The acceptance
criteria below were written down before the first run; the results section records what
came out, whether it passes or not.

## Case

25° Ahmed body, the standard bluff-body test case for car aerodynamics.

| Item | Value | Source |
|---|---|---|
| Length, width, height | 1044 × 389 × 288 mm | [ERCOFTAC AC1-05](https://www.kbwiki.ercoftac.org/w/index.php/Description_AC1-05) |
| Front | edges rounded, centre of curvature 100 mm from the front and the upper, lower and side faces (radius 100 mm) | ERCOFTAC AC1-05 |
| Slant | 25°, slant face length 222 mm, sharp edges | ERCOFTAC AC1-05 |
| Ground clearance | 50 mm above a fixed floor | ERCOFTAC AC1-05 |
| Speed | 40 m/s, Re = 2.78 million on the body length | ERCOFTAC AC1-05 (Erlangen LSTM experiment) |
| Reference area | 0.112 m² (width × height) | [SimFlow validation](https://help.sim-flow.com/validation/ahmed-body) |

The model is generated in `tests/cfd/test_cfd_validation.cpp` (Open CASCADE box, front
edges filleted, slant cut). The four support stilts (30 mm diameter) are left out:
their positions are not given in the sources above, and the experiments report the drag
of the body. The floor is fixed (`groundMoving: false`) at 50 mm below the body
(`groundHeight`).

Experimental drag coefficient, 25° slant, Aref 0.112 m²:

| Cd | Source |
|---|---|
| 0.289 | Ahmed data, as given in the [University of Iowa CFD lab manual](https://user.engineering.uiowa.edu/~fluids/Posting/Home/CFD/Intermediate-2013/Lab%204-Ahmed/Intermediate%20Lab%204%20Manual%20V2.pdf) |
| 0.298 | Strachan, Knowles and Lawson (2007), as given in [arXiv:2001.11074](https://arxiv.org/html/2001.11074), Table 2 (moving ground, Re 1.7 million) |
| 0.299 | Meile et al. (2011), as given in [SimFlow validation](https://help.sim-flow.com/validation/ahmed-body) |

Reference value for the criteria: **Cd = 0.295** (mean of the three, spread 0.289 to
0.299).

## Acceptance criteria (set before the first run)

On the **medium** mesh:

1. Cd within ±15 % of 0.295, i.e. **0.251 ≤ Cd ≤ 0.339**.
2. The run's quality status is not `warning` (mass balance, residuals, Cd band over the
   last 20 % of the iterations below 2 %; see [cfd.md](cfd.md#results-and-checks)).
3. Lift has the measured sign for this body: Cl > 0 (the 25° slant gives positive lift
   in the experiments quoted above: Cl 0.280 in arXiv:2001.11074, Table 2).

The **coarse** mesh is reported without a pass criterion: it is meant for quick A/B
checks, not absolute values.

Why ±15 %: steady RANS (k-omega SST) with wall functions on a snappyHexMesh mesh without
resolved boundary layers is not expected to match a separated rear flow to a few
percent; the purpose is to show that drag is of the right size and that the preset has
no gross error (units, reference area, forces, boundary conditions). This is not a
statement about accuracy on other shapes.

## Running

```
ctest --test-dir build/dev -L validation
```

It needs Docker (or a local OpenFOAM v2506) and takes much longer than the unit tests,
so it is not part of CI. The test prints Cd, Cl, cell count and the quality status per
mesh level and fails when a criterion is not met.

## Results

Run on 2026-11-08 after CAD-153 (the far wake one level coarser than a near box around
the part, the fine surface one level finer), OpenFOAM v2506 in Docker
(`opencfd/openfoam-default:2506`, linux/amd64), 2 processes on a 4-core cloud machine;
40 m/s, flow +X, fixed floor 50 mm below the body, Aref 0.112 m².

| Mesh | Cells | Iterations | Time | Cd | vs 0.295 | Cl | Drag | Quality |
|---|---|---|---|---|---|---|---|---|
| coarse | 10,297 | 211 (converged) | 10 s | 0.462 | +57 % | 0.294 | 49.90 N | ok |
| medium | 65,861 | 277 (converged) | 50 s | 0.321 | +8.9 % | 0.262 | 34.69 N | ok |
| fine | 1,143,982 | 554 (converged) | 37 min | 0.264 | −10.6 % | 0.303 | 28.47 N | ok |

Before CAD-153 (2026-11-07) the same levels had 12,285 / 146,368 / 1,995,746 cells and
gave Cd 0.461 / 0.315 / 0.268 and Cl 0.294 / 0.256 / 0.303 (48 min on the fine mesh):
the coarser far wake saves 55 % of the medium cells and 43 % of the fine cells for a
change in Cd of 2 % or less.

With the forces-settled stop (CAD-155) the same coarse and medium runs end after 120
and 179 iterations instead of 211 and 277, at Cd 0.462 and 0.321 (Cl 0.294 and 0.261),
quality `ok`.

Medium mesh against the criteria:

1. Cd 0.321 is within 0.251 to 0.339: **pass**.
2. Quality status `ok` (continuity error 5.4e-7, Cd band 0.016 % over the last 20 % of
   the iterations, all final residuals below 1e-5): **pass**.
3. Cl 0.262 > 0: **pass**.

The fine mesh is also within the band (Cd 0.264) and its lift is closer to the measured
0.280 than the medium one's. Cd does not settle with refinement: coarse to medium −30 %,
medium to fine −18 %, and the measured value lies between medium and fine. Absolute
drag from this preset should therefore be read as about ±10 % here, and the coarse mesh
only as a relative tool. Comparing variants on two mesh levels (CAD-54) shows whether a
difference between them holds.

A layer setting that extrudes over sharper edges (`featureAngle 120`, relaxed quality
for the last iterations) covered 94 % instead of 77 % of the medium body with layers, but
the run then no longer converged and Cl fell to 0.185, so the preset keeps
`featureAngle 60`.

The case directories are not kept by default; set `AEROCAD_KEEP_VALIDATION=1` to keep
them, and `AEROCAD_VALIDATION_MESH=coarse,medium,fine` to choose the levels (default
`coarse,medium`).
