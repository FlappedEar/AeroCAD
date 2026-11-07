# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare synthetic CFD outputs and a real-GUI script; does not run an OpenFOAM solver.

python tests/gui/cfd_quality_fixture.py --cmd build/release/bin/aerocad-cmd --output /tmp/cad82
AEROCAD_CFD_DIR=/tmp/cad82/runs xvfb-run build/release/bin/AeroCAD \
    tests/data/box_200x80x60.stl --ui-script /tmp/cad82/quality.ui
"""

import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    runs = output / "runs"
    runs.mkdir(parents=True, exist_ok=True)
    model = Path(__file__).resolve().parents[1] / "data" / "box_200x80x60.stl"
    commands = [{"name": "document.import", "params": {"path": str(model)}}]
    for label in ("Missing convergence evidence fixture", "Complete convergence evidence fixture"):
        commands.append({"name": "simulation.run", "params": {"id": 1, "prepareOnly": True, "label": label}})
    env = dict(os.environ, AEROCAD_CFD_DIR=str(runs))
    result = subprocess.run([str(args.cmd.resolve()), "-"], input=json.dumps(commands), text=True,
                            capture_output=True, check=True, env=env)
    (output / "prepare.jsonl").write_text(result.stdout)
    responses = [json.loads(line) for line in result.stdout.splitlines()]
    if len(responses) != len(commands) or any(not row["ok"] for row in responses):
        raise RuntimeError("Could not prepare fixture cases: " + result.stdout)
    ids = [row["result"]["id"] for row in responses[1:]]
    for index, run_id in enumerate(ids):
        case = runs / run_id
        (case / "log.checkMesh").write_text("Mesh stats\n    cells: 1000\nMesh OK.\nEnd\n")
        coefficients = case / "postProcessing/forceCoeffs/0/coefficient.dat"
        coefficients.parent.mkdir(parents=True, exist_ok=True)
        coefficients.write_text("# Time Cd Cl Cs\n" + "".join(f"{i} 0.8 0.1 0.01\n" for i in range(1, 101)))
        if index:
            (case / "log.simpleFoam").write_text(
                "Time = 100\ntime step continuity errors : sum local = 1e-5, global = 1e-7\n"
                "SIMPLE solution converged in 100 iterations\nEnd\n")
            residuals = case / "postProcessing/solverInfo/0/solverInfo.dat"
            residuals.parent.mkdir(parents=True, exist_ok=True)
            residuals.write_text("# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n"
                                 "100 1e-6 1e-6 1e-6 1e-5 1e-6 1e-6\n")
        # Deliberately mark both fixtures finished: status alone must not imply quality OK.
        (case / "aerocad.stage").write_text("done\n")
        (case / "aerocad.status").write_text("finished\n")
        (case / "aerocad.engine").write_text("v2506\n")

    lines = ["# Synthetic result presentation only; no solver was run.", "ui.wait(800)",
             'ui.clickText("SIMULATE")', "ui.wait(250)", 'ui.clickText("Wind Tunnel")', "ui.wait(700)"]
    for run_id, expected in zip(ids, ("unknown", "ok")):
        lines += ["simulationPanel.refreshRuns()", f"simulationPanel.selectRun({json.dumps(run_id)})",
                  'ui.clickText("Results")', "ui.wait(500)",
                  '(() => { const q = simulationPanel.results.quality; '
                  f'ui.log("fixture-only {expected}: " + JSON.stringify(q)); '
                  f'if (q.status !== "{expected}") ui.fail("expected {expected}, got " + q.status); '
                  'if (q.status === "unknown" && !q.warnings.length) ui.fail("missing explanation"); })()',
                  f"ui.shot({json.dumps(str(output / f'quality-{expected}.png'))})"]
    (output / "quality.ui").write_text("\n".join(lines) + "\n")
    print(f"Fixtures ready; set AEROCAD_CFD_DIR={runs} and run AeroCAD --ui-script {output / 'quality.ui'}")


if __name__ == "__main__":
    main()
