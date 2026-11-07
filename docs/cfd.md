# Wind tunnel (CFD)

AeroCAD simulates the flow of air around a part with [OpenFOAM](https://www.openfoam.com)
in the background and shows how the part behaves: drag, lift and side force in newtons,
Cd/Cl over the iterations, surface pressure, velocity sections and streamlines. Two runs
can be compared (A/B).

Everything goes through the `simulation.*` commands, so the UI, scripts and an AI agent
(MCP tools `simulation_run`, `simulation_status`, `simulation_compare`, ...) use the same
code (module `src/cfd`, commands in `src/commands/src/SimulationCommands.cpp`).

## Engine: OpenFOAM v2506 in Docker

One OpenFOAM version is supported: **OpenFOAM v2506** (openfoam.com / OpenCFD). Cases
are generated for, and tested with, this release only.

The default engine is the official image `opencfd/openfoam-default:2506` from Docker
Hub. It is published for `linux/amd64` and `linux/arm64`, so it runs natively on Apple
Silicon (no emulation). Download size is about 380 MB (arm64) / 470 MB (amd64), 2.2 GB
unpacked; the first run pulls it automatically.

### macOS

1. Install [Docker Desktop for Mac](https://docs.docker.com/desktop/setup/install/mac-install/)
   (Apple silicon build) and start it once.
2. Optional, to avoid waiting on the first run: `docker pull opencfd/openfoam-default:2506`.
3. In AeroCAD run `simulation.environment` (or open the CFD panel): it should report
   Docker running.

Runs are stored in `~/.aerocad/cfd/<run id>/` (override with `AEROCAD_CFD_DIR`), which
Docker Desktop can mount because it shares `/Users` by default. A runs directory
outside the shared folders will not work.

Other Docker engines with the same `docker` CLI (colima, OrbStack) should work as long
as the runs directory is mounted into their VM; this has not been tested.

The app finds `docker` in `PATH` and in the usual install locations
(`/usr/local/bin`, `/opt/homebrew/bin`, `~/.docker/bin`, `~/.orbstack/bin`, Docker.app),
because apps started from the Finder do not get the shell's `PATH`. `AEROCAD_DOCKER`
overrides the path.

### Linux

Docker Engine works the same way (the container runs as your user id). Alternatively a
local OpenFOAM v2506 installation is used when no Docker is running: the app sources
`$WM_PROJECT_DIR/etc/bashrc`, `/usr/lib/openfoam/openfoam2506/etc/bashrc` (the
openfoam.com packages) or the file named by `AEROCAD_OPENFOAM_BASHRC`.

Windows is not supported yet.

## Preset `external-flow-v1`

Steady, incompressible external flow around the selected part in a virtual wind
tunnel:

| Item | Choice |
|---|---|
| Solver | `simpleFoam` (SIMPLE, consistent), initialised with `potentialFoam` |
| Turbulence | RANS k-omega SST, wall functions |
| Mesh | `blockMesh` background (cubic cells) + `snappyHexMesh` on the part |
| Domain | 3 L upstream, 8 L downstream, 2.5 L to the sides, above and below (L = largest part dimension) |
| Boundaries | fixed velocity inlet, fixed pressure outlet, slip far field; optional moving floor |
| Fluid | air: nu = 1.5e-5 m^2/s, rho = 1.205 kg/m^3 |
| Inflow turbulence | intensity 1 %, nu_t/nu = 10: k = 1.5 (U I)^2, omega = k / (nu * 10) |
| Stop | residuals p < 1e-4, U, k, omega < 1e-5, or the iteration limit |

Mesh levels (cells per L in the background mesh / surface refinement levels /
wake box level / prism layers): coarse 2 / 3-4 / 2 / 0, medium 3 / 4-5 / 3 / 2,
fine 4 / 5-6 / 4 / 3. Default iteration limits 300 / 500 / 800. Coarse is meant for
quick A/B checks; it is not validated against experiments yet (CAD-52, Ahmed body).

**Units and axes.** The model is in millimetres, the case in metres (converted when the
case is written). The flow direction is one of `+X`, `-X`, `+Y`, `-Y` in model axes
(default `+Y`: the air hits the side you see in the Front view); Z is up. The case is
rotated so the air flows along its +X axis. Results are reported as **drag** (along the
flow), **lift** (+Z) and **side** force, and field data is converted back to model
coordinates.

**Reference values.** Aref defaults to the frontal area of the part (projection on the
plane normal to the flow, rasterised), lRef to its length along the flow. Both can be
set. Forces in N do not depend on them.

**Other parts in the flow** (`context`): e.g. the body section a part is mounted on.
They are walls in the simulation but not part of the reported forces.

## Running

```json
{"name": "simulation.propose_run", "params": {"id": 1, "speed": 30}}
{"name": "simulation.run", "params": {"id": 1, "speed": 30, "label": "A"}}
{"name": "simulation.status", "params": {"run": "20261106-140102-rear-wing"}}
{"name": "simulation.results", "params": {"run": "20261106-140102-rear-wing"}}
```

`simulation.run` returns at once with a run id; the run continues in the background,
also when the app is closed (it is picked up again on the next start). Progress is
estimated from the current step (`aerocad.stage`) and the solver iteration. A run
records everything needed to repeat it: `case.json` (settings, frame, reference values,
geometry hash), `run.json` (source part and revision, engine command) and the complete
OpenFOAM case, which can be opened in ParaView through `case.foam`.

`simulation.run` with `prepareOnly: true` only writes the case; run `./Allrun` in an
OpenFOAM v2506 environment elsewhere and register the result with `simulation.import`.

## Results and checks

`simulation.results` returns the summary (mean over the last 20 % of the iterations),
histories (Cd, Cl, Cs, forces), residuals and a quality report:

- volume mesh: `checkMesh` verdict and cell count,
- mass balance: last time-step continuity error (warning above 1e-3),
- residuals: initial residuals of the last iteration (warning when p > 1e-2),
- force history: band of Cd over the last 20 % of the iterations (warning above 2 %).

Status `ok` / `warning` / `unknown`. These checks flag results to read with care; they
are not a validation of the physics.

`simulation.field` summarises the field outputs: `surface` (pressure on the part as
p, Cp and Pa), `sectionHorizontal` / `sectionVertical` (planes through the part centre,
speed in m/s and Cp) and `streamlines` (35 lines seeded on a grid upstream).

## A/B comparison

`simulation.compare {a, b}` returns the force differences in N and %, the coefficients
of B rescaled to A's reference area, the list of differing conditions (speed, direction,
mesh, preset, fluid, ground, reference values), the quality of both runs and shared
colour scale ranges (Cp on the surface, speed in the sections). Verdict:
`comparable`, `check quality` or `not comparable`. Variants of very different size get
different domain and mesh sizes, which is reported as a note. Mesh-refinement
sensitivity of the difference (CAD-54) is not assessed yet.

## Tests

`ctest -L openfoam` runs a box through the whole pipeline in OpenFOAM (about a minute
with Docker; skipped when no engine is found). It is not part of the CI gate. The unit
tests (`test_cfd`, `test_commands`) cover case generation, parsing, quality checks, the
run manager and the commands without an engine.
