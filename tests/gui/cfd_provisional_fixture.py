# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare synthetic result/cancellation GUI checks; no OpenFOAM calculation is performed.

python tests/gui/cfd_provisional_fixture.py --cmd build/release/bin/aerocad-cmd --output /tmp/cad83
AEROCAD_CFD_DIR=/tmp/cad83/runs xvfb-run build/release/bin/AeroCAD \
    tests/data/box_200x80x60.stl --ui-script /tmp/cad83/provisional.ui

Optional Linux/macOS process-cancellation check (starts a sleeping stand-in, not a solver):
AEROCAD_CFD_DIR=/tmp/cad83/runs AEROCAD_OPENFOAM_BASHRC=/tmp/cad83/process-fixture.bashrc \
    xvfb-run build/release/bin/AeroCAD tests/data/box_200x80x60.stl --ui-script /tmp/cad83/cancel.ui
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess


def write(case, name, text):
    path = case / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def fixture(case, complete=False, pressure=20):
    write(case, "log.checkMesh", "Mesh stats\n    cells: 1000\nMesh OK.\nEnd\n")
    write(case, "postProcessing/forceCoeffs/0/coefficient.dat",
          "# Time Cd Cl Cs\n" + "".join(f"{i} 0.8 0.1 0.01\n" for i in range(1, 21)))
    if not complete:
        return
    write(case, "postProcessing/forces/0/force.dat", "# Time total_x total_y total_z pressure_x viscous_x\n"
          + "".join(f"{i} 1.0 0.01 0.2 0.9 0.1\n" for i in range(1, 21)))
    write(case, "postProcessing/solverInfo/0/solverInfo.dat",
          "# Time Ux_initial Uy_initial Uz_initial p_initial k_initial omega_initial\n"
          + "".join(f"{i} 1e-6 1e-6 1e-6 1e-6 1e-6 1e-6\n" for i in range(1, 21)))
    write(case, "log.simpleFoam", "Time = 20\ntime step continuity errors : sum local = 1e-7, global = 1e-8\n"
          "SIMPLE solution converged in 20 iterations\nEnd\n")
    write(case, "postProcessing/surfaces/20/part.vtk",
          "# vtk DataFile Version 2.0\nSynthetic GUI fixture\nASCII\nDATASET POLYDATA\n"
          "POINTS 3 float\n0 0 0 0.1 0 0 0 0.1 0\nPOLYGONS 1 4\n3 0 1 2\n"
          f"POINT_DATA 3\nFIELD FieldData 1\np 1 3 float\n{pressure} {-pressure} 0\n")
    write(case, "aerocad.stage", "done\n")
    write(case, "aerocad.status", "finished\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    runs = output / "runs"
    runs.mkdir(parents=True, exist_ok=True)
    model = Path(__file__).resolve().parents[1] / "data/box_200x80x60.stl"
    commands = [{"name": "document.import", "params": {"path": str(model)}}]
    labels = ["Prepared A (synthetic)", "Prepared B (synthetic)",
              "Finished, incomplete output (synthetic)", "Finished, complete output (synthetic)"]
    commands += [{"name": "simulation.run", "params": {"id": 1, "prepareOnly": True, "label": label}}
                 for label in labels]
    result = subprocess.run([str(args.cmd.resolve()), "-"], input=json.dumps(commands), text=True,
                            capture_output=True, check=True, env=dict(os.environ, AEROCAD_CFD_DIR=str(runs)))
    (output / "prepare.jsonl").write_text(result.stdout)
    responses = [json.loads(line) for line in result.stdout.splitlines()]
    if len(responses) != len(commands) or any(not row["ok"] for row in responses):
        raise RuntimeError("Could not prepare cases: " + result.stdout)
    ids = [row["result"]["id"] for row in responses[1:]]
    for index, run in enumerate(ids):
        fixture(runs / run, complete=index >= 2, pressure=100 if index == 3 else 20)
    with (runs / ids[2] / "postProcessing/forces/0/force.dat").open("a") as force_file:
        force_file.write("21 1.0\n")

    lines = ["# Synthetic output presentation; no solver was run.", "ui.wait(500)",
             'ui.clickText("SIMULATE")', "ui.wait(200)", 'ui.clickText("Wind Tunnel")', "ui.wait(400)",
             "simulationPanel.refreshRuns()", f"simulationPanel.selectRun({json.dumps(ids[0])})",
             f"simulationPanel.compareRun = {json.dumps(ids[1])}; simulationPanel.loadComparison()",
             'ui.clickText("Results")', "ui.wait(300)",
             '(() => { const r = simulationPanel.results; '
             'if (r.finalResults.available || r.summary.cd !== null || !r.provisional) ui.fail("partial summary is final"); '
             'if (r.history.cd.length !== 20) ui.fail("lost provisional history"); })()',
             f"ui.shot({json.dumps(str(output / 'provisional-history.png'))})",
             'ui.wheel(window.width - 100, window.height - 150, 0, -550)', "ui.wait(300)",
             '(() => { const find = item => item.title === "Coefficients (provisional)" ? item '
             ': Array.from(item.children).map(find).find(x => x); const chart = find(simulationPanel); '
             'if (!chart || chart.series[0].y.length !== 20) ui.fail("missing provisional coefficient chart"); })()',
             f"ui.shot({json.dumps(str(output / 'provisional-charts.png'))})",
             'ui.wheel(window.width - 100, window.height - 150, 0, 1000)', "ui.wait(300)",
             'ui.clickText("A/B")', "ui.wait(300)",
             '(() => { const c = simulationPanel.comparison; ui.log(JSON.stringify(c)); '
             'if (c.verdict !== "not comparable" || !c.reasons.length || Object.keys(c.forces).length '
             '|| Object.keys(c.coefficientsOnReferenceAreaOfA).length) ui.fail("partial A/B verdict"); })()',
             f"ui.shot({json.dumps(str(output / 'provisional-results.png'))})",
             'Preferences.theme = "dark"', "ui.wait(300)",
             f"ui.shot({json.dumps(str(output / 'provisional-results-dark.png'))})",
             'Preferences.theme = "light"',
             f"simulationPanel.selectRun({json.dumps(ids[2])}); simulationPanel.compareRun = {json.dumps(ids[3])}",
             "simulationPanel.showB = true; simulationPanel.loadResults(); simulationPanel.loadComparison()",
             "ui.wait(300)",
             '(() => { const s = simulationPanel.scaleFor("surface", "Cp"); '
             'ui.log("blocked pair scale=" + JSON.stringify(s)); '
             'if (simulationPanel.finalComparisonAvailable || simulationPanel.showB '
             '|| simulationPanel.displayedRun !== simulationPanel.activeRun '
             '|| Math.abs(s[0] + 0.1) > 1e-6 || Math.abs(s[1] - 0.1) > 1e-6) '
             'ui.fail("blocked comparison still pools colour scales or displays B"); })()']
    (output / "provisional.ui").write_text("\n".join(lines) + "\n")

    # An actual child process with synthetic partial files exercises RunManager.cancel
    # and the GUI Cancel button without claiming OpenFOAM physics/engine validation.
    complete = runs / ids[3]
    bashrc = "# Synthetic process fixture; this is not an OpenFOAM installation.\n"
    bashrc += f"cp -R {shlex.quote(str(complete / 'postProcessing'))} .\n"
    bashrc += f"cp {shlex.quote(str(complete / 'log.checkMesh'))} {shlex.quote(str(complete / 'log.simpleFoam'))} .\n"
    bashrc += "printf 'solve\\n' > aerocad.stage\nexec /bin/sleep 60\n"
    (output / "process-fixture.bashrc").write_text(bashrc)
    cancel = ["# Real process cancellation with synthetic output; not an OpenFOAM calculation.",
              'ui.clickText("SIMULATE")', "ui.wait(200)", 'ui.clickText("Wind Tunnel")',
              '(() => { const r = controller.execute("simulation.run", '
              '{id: 1, backend: "local", label: "Sleeping process fixture"}); '
              'if (!r.ok) { ui.fail(r.error); return }; simulationPanel.refreshRuns(); '
              'simulationPanel.selectRun(r.result.id); })()', "ui.wait(700)",
              "simulationPanel.refreshRuns(); simulationPanel.loadResults()", 'ui.clickText("Results")',
              "ui.wait(300)",
              '(() => { const r = simulationPanel.results; if (r.run.state !== "running" '
              '|| r.finalResults.available || r.history.cd.length !== 20) ui.fail("running output is final or missing"); })()',
              'ui.clickText("Cancel")', "ui.wait(700)",
              "simulationPanel.refreshRuns(); simulationPanel.loadResults()",
              '(() => { const r = simulationPanel.results; ui.log("after Cancel: " + JSON.stringify(r.run)); '
              'if (r.run.state !== "cancelled" || r.finalResults.available || r.summary.cd !== null '
              '|| r.history.cd.length !== 20) ui.fail("cancelled output is final or history lost"); })()',
              f"ui.shot({json.dumps(str(output / 'cancelled-results.png'))})"]
    (output / "cancel.ui").write_text("\n".join(cancel) + "\n")
    print(f"Set AEROCAD_CFD_DIR={runs}; UI script: {output / 'provisional.ui'}")


if __name__ == "__main__":
    main()
